#pragma once

// THE v1 COMPLETER (#139, #137, spec §6.9): three sources, synchronously, on
// the loop thread.
//
// WHAT TAB DOES, in one paragraph. The token under the cursor is found with
// C-6's LEXER - a lex, never a parse: the parser would want a whole grammatical
// line and the line under a cursor is half typed by definition. The token is
// then classified into one of three sources: a word in command position
// completes to command names, a token whose tail is `$name` completes to
// variable names, and everything else completes to paths. The two name lists
// come from `shell_knowledge::enumerate`, called RIGHT HERE on the loop thread -
// one copy per Tab per domain. The directory walk runs here too.
//
// READING THE SHELL'S TABLES FROM AN ACTION IS ADR-0009's OWN RULE (#151), not
// an exception to it: the loop reads shell state while nothing executes, and
// nothing can execute while the loop is inside an action, because `execute` and
// `port_call` are the only writers and the loop is what calls each of them (and
// blocked for the whole of each while they were messages, before #201). #139
// shipped this as a round trip through a fourth slot on `shell_actor` and a
// `name_source` interface in front of it; #151 deleted both, because the copy is
// the same copy either way and the protocol was the part that could be got
// wrong.
//
// SYNCHRONOUS, AND THAT IS THE OWNER-APPROVED DEVIATION FROM F-31. F-31 wants
// candidates streamed from a worker so a cold directory cannot block. §6.9 defers
// that to the index stage and says so in the spec rather than quietly: "a Tab on
// a huge cold directory briefly blocks, as fish's did for years". Nothing in the
// #94 contract changes when the work moves to a helper, because #94's `Completer`
// is the shape below and not the thread it runs on.
//
// NOTHING FROM THE EXPANDER, AND A TEST IS WHAT ENFORCES IT NOW. `completion.cpp`
// includes `syntax/` and `substrate/` and no `runtime/` header. While this was
// `src/leshper/complete.cpp` the link graph said so for us - `lesh_leshper` does
// not link `lesh_runtime` (spec §4.4), so an include that reached for the
// expander would have failed to LINK rather than failing review. #168 Phase B
// moved the file into `lesh_ui`, which links both halves, and took that guarantee
// away with it. The RULE is unchanged - #11's `$VAR` in a completion prefix is
// never expanded in v1 - and `UiCompleteIncludeDiscipline` in the unit tests is
// what keeps it: it reads this file's include lines and fails on a `runtime/`
// among them. The one environment read is `$HOME` for a leading `~`, which §6.9
// asks for by name; see `expand_tilde_for_listing` in the .cpp for why that is
// not the same door.
//
// F-30 IS NOT HERE, AND THAT IS #138's DECISION HONOURED. `lesh_pager_commit`
// decides between inserting an unambiguous extension and opening the list, so
// that the completer, the history search and a plugin cannot disagree about what
// "unambiguous" means. This file finds candidates and stops; the `complete_word`
// action feeds them to the pager and commits.
//
// v2, RECORDED IN §6.9 AND DELIBERATELY ABSENT: a command index/registry done
// properly, per-thread cached command lists, per-command and option completion,
// user completers through A-13, and `$VAR`-in-prefix expansion by a safe path.

#include "ui/shell_knowledge.h"
#include "leshper/state.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::ui {

// ---------------------------------------------------------------------------
// What a candidate is (#94's `Completer` contract, spoken in the pager's words)
// ---------------------------------------------------------------------------

// ONE VOCABULARY AND NOT TWO. #94 named a candidate as (text, description,
// trailing slash-vs-space, needs-quoting) and #138's pager answers three of
// those four with the pager's `leshper::pager_kind`: the `ls -F` marker IS the
// v1 description, and the marker decides the trailer. So a candidate here is a
// `leshper::pager_candidate` and nothing more, and `lesh_pager_add` takes it apart in the two arguments it
// already has. A second candidate type beside the pager's would be two
// spellings of one thing - exactly what pager.h refuses inside itself.
//
// NEEDS-QUOTING IS THE FOURTH, and it is spent rather than carried: `text` is
// the QUOTED form, escaped where the shell would not read the name back
// literally. It has to be, because the pager's rule is that what is shown is
// what is inserted, and the completer is the only side that knows shell
// quoting. The escaping covers the REPLACED COMPONENT only - the range below -
// so the `~`, the directory prefix and the `$` are never re-quoted (spec 6.9).
using candidate = leshper::pager_candidate;

// One Tab's whole answer.
//
// NO VERDICT AND NO INSERTION TEXT. F-30 lives in `lesh_pager_commit`, editor
// side, so that the completer, the history search and a plugin all get the same
// answer to "is this unambiguous" (abi.h's pager block gives the argument). What
// this type carries is what was FOUND, and nothing about what to do with it.
struct completion_result {
	// The half-open byte range of the buffer the candidates REPLACE - what
	// `lesh_pager_open` is given.
	//
	// It is the COMPONENT under the cursor and not the whole token: for `~/Doc`
	// it starts at the `D`, so the `~` stays in the buffer exactly as 6.9
	// requires, and for `$HOM` it starts at the `H`. `replace_to` is the cursor -
	// v1 completes what is behind the cursor and leaves what is in front of it
	// alone.
	std::size_t replace_from = 0;
	std::size_t replace_to = 0;

	// Sorted by text, de-duplicated by text.
	std::vector<candidate> candidates;

	void clear() noexcept {
		replace_from = 0;
		replace_to = 0;
		candidates.clear();
	}
};

// What the completer is asked. A view over the buffer, which the caller owns.
struct completion_query {
	std::string_view buffer;
	std::size_t cursor = 0;
};

// ---------------------------------------------------------------------------
// The two doors the completer reaches through (A-5, both)
// ---------------------------------------------------------------------------

// THE FIRST DOOR IS `shell_knowledge` (shell_knowledge.h), unchanged and not
// wrapped. #139 put a second interface in front of it - `name_source`, one
// method, every implementation a cross-thread round trip - because the completer
// was on the loop and the tables were the shell thread's. #151 removed the round
// trip, and with it the only difference between the two shapes; a second
// interface whose implementation is now `return _knowledge->enumerate(...)` is
// one spelling too many for one idea. A null `shell_knowledge*` says what
// `name_source::names` returning false used to say: no shell attached.

// The filesystem, injectable - which is what makes every path rule below
// testable without a temporary directory per case.
class directory_reader {
public:
	directory_reader() = default;
	virtual ~directory_reader() = default;

	directory_reader(const directory_reader&) = delete;
	directory_reader& operator=(const directory_reader&) = delete;

	struct entry {
		std::string name;
		bool directory = false;
		bool executable = false;
		bool symlink = false;
	};

	// Appends the entries of `path` (never `.` or `..`) to `into`. False when the
	// directory cannot be read, which a completer answers with no candidates
	// rather than with a diagnostic: half a path that does not exist yet is what
	// someone typing looks like.
	virtual bool read(std::string_view path, std::vector<entry>& into) const = 0;
};

// The real one: `opendir`/`readdir`, `stat` only where `d_type` cannot answer.
[[nodiscard]] const directory_reader& posix_directory_reader() noexcept;

// ---------------------------------------------------------------------------
// #94's `Completer` override point
// ---------------------------------------------------------------------------

// DECLARED SINCE #134 in what is now `ui/session.h`, as a forward declaration
// with the note "completion and its pager are not this ticket's"; this is that
// type arriving.
// The bundle field keeps its meaning: null means the session builds the default
// one below, and a caller that supplies its own replaces the whole source trio -
// which is the override point #94 asked for, one indirect call at Tab frequency.
class completer {
public:
	completer() = default;
	virtual ~completer() = default;

	completer(const completer&) = delete;
	completer& operator=(const completer&) = delete;

	// `into` is CLEARED first; a caller may reuse one across Tabs and keep its
	// capacity.
	virtual void complete(const completion_query& query, completion_result& into) const = 0;
};

// The v1 trio.
//
// Both doors are BORROWED and must outlive it. A null `knowledge` is legal and
// means "no shell attached" - path completion still works, which is what a
// leshper embedded in something that is not this shell would want.
//
// ON THE LOOP THREAD, and the pointer is only safe there because the loop is not
// turning while the shell writes; see the file header and ADR-0009.
class shell_completer final : public completer {
public:
	explicit shell_completer(const shell_knowledge* knowledge,
	                         const directory_reader& directories
	                         = posix_directory_reader()) noexcept
		: _knowledge(knowledge), _directories(&directories) {}

	void complete(const completion_query& query, completion_result& into) const override;

private:
	// One domain, one call into the shell's tables, filtered by prefix.
	// `scratch` is the caller's, reused across the domains of one Tab.
	void gather_names(name_domain which, leshper::pager_kind kind, std::string_view typed,
	                  std::vector<candidate>& out, std::vector<std::string>& scratch) const;
	// The three tables plus the `$PATH` walk.
	void gather_commands(std::string_view typed, std::vector<candidate>& out,
	                     std::vector<std::string>& scratch) const;
	void gather_paths(std::string_view directory, std::string_view typed,
	                  std::vector<candidate>& out) const;

	const shell_knowledge* _knowledge;
	const directory_reader* _directories;
};

// ---------------------------------------------------------------------------
// The pieces, exposed so the rules can be tested one at a time
// ---------------------------------------------------------------------------

// Which of the trio a token belongs to.
enum class completion_source : std::uint8_t {
	none = 0,      // nothing here can be completed
	command = 1,   // a word in command position
	variable = 2,  // the tail after a `$` or `${`
	path = 3,      // everything else
};

// What the lexer found under the cursor.
struct token_under_cursor {
	completion_source source = completion_source::none;
	// The component being completed: [from, cursor). For a path this excludes
	// the directory prefix; for a variable it excludes the `$`.
	std::size_t from = 0;
	// The directory part of a path token, AS TYPED - `~/`, `../`, `/usr/`, or
	// empty. Empty for the other two sources.
	std::string_view directory;
};

// C-6, and nothing above it: classify the token under `cursor` (#139, §6.9).
//
// A LEX AND NOT A PARSE. Command position is decided from the token stream -
// a word is a command name when the last significant token before it opened a
// command (`|`, `&&`, `||`, `;`, `;;`, `;&`, `&`, `(`, `)`, a newline, or the
// start of input), with leading assignments and redirection targets skipped, the
// same way `word_role::command_name` falls out of the grammar. Doing it with the
// parser instead would mean asking a half-typed line for a tree, and the answer
// on `if [ -f x ]; then gre` is that there is no tree yet.
[[nodiscard]] token_under_cursor classify_token(std::string_view buffer, std::size_t cursor);

// True when `text` contains a byte the shell would not read back literally.
[[nodiscard]] bool needs_quoting(std::string_view text) noexcept;

// `text` with every such byte backslash-escaped, appended to `into`.
void quote_into(std::string_view text, std::string& into);

// The reverse, for matching what the user typed against real names: backslash
// escapes removed, quotes taken literally-as-delimiters. Appended to `into`.
void unquote_into(std::string_view text, std::string& into);

} // namespace lesh::ui
