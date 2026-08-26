#include "leshper/complete.h"

#include "substrate/assert.h"
#include "syntax/lexer.h"
#include "syntax/token.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// LOOK AT THE INCLUDES, the way builtin_reactors.cpp asks you to.
//
// `syntax/` and `substrate/` and the POSIX headers the directory walk needs -
// and NO `runtime/` header. The expander lives in `lesh_runtime`, which
// `lesh_leshper` does not link (CMakeLists, spec 4.4), so an include that
// reached for it would not link. That is what makes "$VAR in a completion prefix
// is never expanded" (spec 6.9, #11) a property of the build graph rather than
// of anyone's care - and `LeshperCompleteIncludeDiscipline` in the unit tests
// reads this file and says so out loud, so the rule is also visible to someone
// who is only reading the tests.

namespace lesh::leshper {
namespace {

using syntax::lexer;
using syntax::token;
using syntax::token_kind;

// --- Quoting ---------------------------------------------------------------

// The bytes the shell does not read back literally.
//
// `~` and `#` are conditional and handled by the caller: a tilde is special only
// at the start of a word and a `#` only where a word begins, so escaping them
// mid-name would insert a backslash the shell would then keep.
constexpr std::string_view kAlwaysSpecial = " \t\n\v\f\r\"'\\$`&|;<>()*?[]{}!";

[[nodiscard]] bool is_name_byte(char c) noexcept {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
	       || c == '_';
}

// `text` from `at` on. A named helper only so the expression above reads as one
// thing rather than as arithmetic on two offsets.
[[nodiscard]] std::string_view query_after(std::string_view text, std::size_t at) noexcept {
	return at >= text.size() ? std::string_view{} : text.substr(at);
}

[[nodiscard]] bool starts_a_command(token_kind kind) noexcept {
	switch (kind) {
		case token_kind::newline:
		case token_kind::pipe:
		case token_kind::and_if:
		case token_kind::or_if:
		case token_kind::semi:
		case token_kind::dsemi:
		case token_kind::semi_and:
		case token_kind::amp:
		case token_kind::lparen:
		case token_kind::rparen:
			return true;
		default:
			return false;
	}
}

[[nodiscard]] bool is_redirection(token_kind kind) noexcept {
	return kind >= token_kind::less && kind <= token_kind::clobber;
}

// `NAME=` at the head of a word, POSIX 2.10.2's assignment rule.
//
// The one thing standing between `FOO=bar gr<Tab>` and completing `gr` as an
// argument: an assignment does not end command position, it precedes it.
[[nodiscard]] bool is_assignment(std::string_view text) noexcept {
	if (text.empty() || (text[0] >= '0' && text[0] <= '9'))
		return false;
	for (std::size_t at = 0; at < text.size(); ++at) {
		if (text[at] == '=')
			return at != 0;
		if (!is_name_byte(text[at]))
			return false;
	}
	return false;
}

// --- The directory walk ----------------------------------------------------

class posix_reader final : public directory_reader {
public:
	bool read(std::string_view path, std::vector<entry>& into) const override {
		// `opendir` needs a NUL-terminated name and the caller has a view, so one
		// copy happens here. At Tab frequency, against a readdir.
		std::string where{path.empty() ? std::string_view{"."} : path};
		DIR* dir = ::opendir(where.c_str());
		if (dir == nullptr)
			return false;
		if (where.back() != '/')
			where.push_back('/');
		const std::size_t prefix = where.size();
		for (;;) {
			errno = 0;
			const dirent* found = ::readdir(dir);
			if (found == nullptr)
				break;
			const std::string_view name{found->d_name};
			// `.` and `..` are never candidates: nobody Tabs for them, and a
			// completion that offered `..` on an empty prefix would bury the
			// entries someone was looking for.
			if (name == "." || name == "..")
				continue;
			entry one;
			one.name.assign(name);
			where.resize(prefix);
			where.append(name);
			classify(where, *found, one);
			into.push_back(std::move(one));
		}
		::closedir(dir);
		return true;
	}

private:
	// `d_type` WHERE IT IS TRUSTWORTHY, `stat` where it is not. Some filesystems
	// answer DT_UNKNOWN for everything, and a symlink has to be followed to know
	// whether Tab should append `/` - so a link is always stat'ed, which is what
	// `ls -F` does to decide between `@` and `/`.
	static void classify(const std::string& full, const dirent& found, entry& one) {
#ifdef DT_UNKNOWN
		one.symlink = found.d_type == DT_LNK;
		if (found.d_type == DT_DIR) {
			one.directory = true;
			return;
		}
		if (found.d_type != DT_UNKNOWN && found.d_type != DT_LNK && found.d_type != DT_REG)
			return;
#else
		(void)found;
#endif
		struct ::stat info {};
		if (::stat(full.c_str(), &info) != 0)
			return;
		one.directory = S_ISDIR(info.st_mode);
		if (!one.directory && S_ISREG(info.st_mode))
			one.executable = ::access(full.c_str(), X_OK) == 0;
	}
};

// --- Candidate assembly ----------------------------------------------------

// QUOTED AT THE POINT OF CREATION, which is the one place that sees both the
// real name and the fact that it is about to become buffer bytes. `text` is
// therefore what the pager shows AND what it inserts, which is pager.h's own
// one-spelling rule honoured rather than worked around.
void add_candidate(std::string_view name, pager_kind kind, std::vector<candidate>& into) {
	candidate one;
	one.kind = kind;
	if (needs_quoting(name))
		quote_into(name, one.text);
	else
		one.text.assign(name);
	into.push_back(std::move(one));
}

// Sorted by text and de-duplicated by text.
//
// BY TEXT AND NOT BY THE WHOLE CANDIDATE: `cd` is a builtin and may also be a
// file in `$PATH`, and the two rows insert identical bytes. Which KIND survives
// is the first in sort order, which is stable only because the kind is part of
// the comparison's tie-break - so the answer does not depend on the order the
// sources were asked in.
void tidy(std::vector<candidate>& candidates) {
	std::sort(candidates.begin(), candidates.end(),
	          [](const candidate& a, const candidate& b) {
		          if (a.text != b.text)
			          return a.text < b.text;
		          return static_cast<std::uint32_t>(a.kind) < static_cast<std::uint32_t>(b.kind);
	          });
	candidates.erase(std::unique(candidates.begin(), candidates.end(),
	                             [](const candidate& a, const candidate& b) {
		                             return a.text == b.text;
	                             }),
	                 candidates.end());
}

// `~` and `~/...`, EXPANDED FOR THE LISTING ONLY (spec 6.9).
//
// `$HOME` FROM THE ENVIRONMENT, and it is not the door `$VAR` was refused. The
// difference is what is being asked: expanding `$VAR` means running the
// expander, which is arbitrary shell evaluation on the keystroke path and the
// thing #11 made structurally impossible here; reading `$HOME` is one lookup
// that every shell does for `~` at parse time. The cost of using the process
// environment rather than the shell's variable is that `HOME=/tmp` typed on the
// line and not yet run is not honoured - which is the same answer the completer
// gives for every other unexecuted assignment on the line, and is the honest one
// for a completer that does not expand.
//
// `~user` IS NOT EXPANDED in v1: it needs `getpwnam`, which is a database lookup
// with its own failure modes, and no candidate is offered rather than a wrong
// directory being walked.
[[nodiscard]] std::string expand_tilde_for_listing(std::string_view directory) {
	if (directory.empty() || directory.front() != '~')
		return std::string{directory};
	if (directory.size() > 1 && directory[1] != '/')
		return std::string{};  // `~user/` - unsupported, and no directory to walk
	const char* home = std::getenv("HOME");
	if (home == nullptr || home[0] == '\0')
		return std::string{};
	std::string where{home};
	if (!where.empty() && where.back() == '/')
		where.pop_back();
	where.append(directory.substr(1));
	return where;
}

} // namespace

// ---------------------------------------------------------------------------
// Quoting
// ---------------------------------------------------------------------------

bool needs_quoting(std::string_view text) noexcept {
	if (text.empty())
		return false;
	if (text.front() == '~' || text.front() == '#')
		return true;
	return text.find_first_of(kAlwaysSpecial) != std::string_view::npos;
}

void quote_into(std::string_view text, std::string& into) {
	for (std::size_t at = 0; at < text.size(); ++at) {
		const char c = text[at];
		const bool special = kAlwaysSpecial.find(c) != std::string_view::npos
		                     || (at == 0 && (c == '~' || c == '#'));
		if (special)
			into.push_back('\\');
		into.push_back(c);
	}
}

void unquote_into(std::string_view text, std::string& into) {
	// WHAT THE USER TYPED, READ BACK AS A NAME. Only what a half-typed word can
	// contain: backslash escapes and the two quote forms, taken as delimiters. No
	// expansion of any kind - see the file header. An UNTERMINATED quote runs to
	// the end, because that is what a word being typed looks like.
	//
	// A backslash inside double quotes escapes anything here, where POSIX 2.2.3
	// makes it special only before `$`, `` ` ``, `"`, `\\` and a newline. The
	// difference shows only on `"a\b"`, where the shell keeps the backslash and
	// this drops it - a candidate then fails to match a file literally named
	// `a\b`, which is a missing completion and never a wrong insertion.
	enum class inside : std::uint8_t { plain, single_quoted, double_quoted };
	inside where = inside::plain;
	for (std::size_t at = 0; at < text.size(); ++at) {
		const char c = text[at];
		if (where == inside::single_quoted) {
			if (c == '\'')
				where = inside::plain;
			else
				into.push_back(c);
			continue;
		}
		if (c == '\\' && at + 1 < text.size()) {
			into.push_back(text[++at]);
			continue;
		}
		if (where == inside::double_quoted) {
			if (c == '"')
				where = inside::plain;
			else
				into.push_back(c);
			continue;
		}
		if (c == '\'') {
			where = inside::single_quoted;
			continue;
		}
		if (c == '"') {
			where = inside::double_quoted;
			continue;
		}
		into.push_back(c);
	}
}

// ---------------------------------------------------------------------------
// C-6: the token under the cursor
// ---------------------------------------------------------------------------

token_under_cursor classify_token(std::string_view buffer, std::size_t cursor) {
	token_under_cursor found;
	if (cursor > buffer.size())
		cursor = buffer.size();
	found.from = cursor;

	// The lexical stand-in for `word_role::command_name`. See the declaration:
	// this is a lex, so command position is "the last significant token opened a
	// command", with assignments and redirection targets stepped over.
	bool at_command_start = true;
	bool expecting_redirect_target = false;
	bool covering_is_word = false;
	token target{};
	bool target_at_command_start = true;

	lexer scan{buffer};
	for (;;) {
		const token one = scan.next();
		if (one.kind == token_kind::end)
			break;
		if (one.offset > cursor)
			break;

		// A word (or a comment) that ENDS at the cursor is still what is being
		// typed. An OPERATOR that ends at the cursor is not: `echo |<Tab>` is a
		// fresh command position, not a completion of `|`.
		const bool inside = cursor >= one.offset && cursor < one.end_offset();
		const bool ends_here = one.end_offset() == cursor;
		const bool is_word = one.kind == token_kind::word;
		if (inside || (ends_here && (is_word || one.kind == token_kind::comment))) {
			target = one;
			target_at_command_start = at_command_start && !expecting_redirect_target;
			covering_is_word = is_word;
			break;
		}

		if (starts_a_command(one.kind)) {
			at_command_start = true;
			expecting_redirect_target = false;
		} else if (is_redirection(one.kind) || one.kind == token_kind::io_number) {
			expecting_redirect_target = is_redirection(one.kind);
		} else if (is_word) {
			if (expecting_redirect_target)
				expecting_redirect_target = false;
			else if (!(at_command_start && is_assignment(buffer.substr(one.offset, one.length))))
				at_command_start = false;
		}

		if (one.end_offset() >= cursor)
			break;
	}

	// Nothing covers the cursor: a fresh word is beginning here. That is a
	// command position after `|`, `;` or nothing at all, and an argument
	// otherwise - `ls <Tab>` completes paths, `<Tab>` on an empty line completes
	// commands.
	if (target.kind == token_kind::end) {
		found.source = (at_command_start && !expecting_redirect_target)
			? completion_source::command
			: completion_source::path;
		return found;
	}
	if (!covering_is_word)
		return found;  // inside a comment or an operator: nothing to complete

	const std::string_view typed = buffer.substr(target.offset, cursor - target.offset);

	// `$name` and `${name`, and ONLY those two: the tail after the last `$` has
	// to be a name for this to be a variable completion, which rules out `$(`,
	// `${#`, `$?` and everything else that is not a plain parameter.
	if (const std::size_t dollar = typed.rfind('$'); dollar != std::string_view::npos) {
		std::size_t name_at = dollar + 1;
		if (name_at < typed.size() && typed[name_at] == '{')
			++name_at;
		const std::string_view name = typed.substr(name_at);
		if (std::all_of(name.begin(), name.end(), is_name_byte)) {
			found.source = completion_source::variable;
			found.from = target.offset + name_at;
			return found;
		}
		// A `$` that is not a plain parameter. NO CANDIDATES, and that is the
		// no-expander rule showing: completing a path whose prefix contains an
		// expansion would mean knowing what it expands to.
		return found;
	}

	// THE VALUE OF AN ASSIGNMENT IS A PATH, not a command name. `FOO=ba<Tab>` is
	// completing what `FOO` will be set to, and POSIX 2.6.1 even makes a tilde
	// after the `=` eligible for expansion - so the component starts after the
	// `=` and the rest of this function treats it as any other path.
	std::size_t component = target.offset;
	if (target_at_command_start && is_assignment(typed)) {
		component = target.offset + typed.find('=') + 1;
		target_at_command_start = false;
	}
	const std::string_view value = query_after(typed, component - target.offset);

	// POSIX 2.9.1.1: a command name containing a slash is a pathname and is
	// looked up in no table. So is a word that is not in command position.
	const std::size_t slash = value.rfind('/');
	if (target_at_command_start && slash == std::string_view::npos) {
		found.source = completion_source::command;
		found.from = component;
		return found;
	}

	found.source = completion_source::path;
	if (slash == std::string_view::npos) {
		found.from = component;
	} else {
		found.directory = value.substr(0, slash + 1);
		found.from = component + slash + 1;
	}
	return found;
}

// ---------------------------------------------------------------------------
// The reader
// ---------------------------------------------------------------------------

const directory_reader& posix_directory_reader() noexcept {
	static const posix_reader one;
	return one;
}

// ---------------------------------------------------------------------------
// The completer
// ---------------------------------------------------------------------------

void shell_completer::complete(const completion_query& query, completion_result& into) const {
	into.clear();
	std::size_t cursor = query.cursor;
	if (cursor > query.buffer.size())
		cursor = query.buffer.size();

	const token_under_cursor found = classify_token(query.buffer, cursor);
	into.replace_from = found.from;
	into.replace_to = cursor;
	if (found.source == completion_source::none)
		return;

	// What the user typed, read back as a name. The candidates are compared
	// against THIS and the insertion is quoted back from it, which is how
	// `my\ fi<Tab>` finds `my file.txt`.
	std::string typed;
	unquote_into(query.buffer.substr(found.from, cursor - found.from), typed);

	// The per-Tab scratch. A member would make `complete` non-const and shared
	// across a nested read; a local is one allocation per Tab, which is what an
	// edit-scale event is allowed (the keystroke pins in allocation_tests.cpp are
	// about idle turns and self-insert, not this).
	std::vector<std::string> names;

	switch (found.source) {
		case completion_source::command:
			gather_commands(typed, into.candidates, names);
			break;
		case completion_source::variable:
			// A variable is a WORD: no `ls -F` marker exists for one, and what
			// follows `$HOME` on the line is another word.
			gather_names(name_domain::variable, pager_kind::word, typed, into.candidates,
			             names);
			break;
		case completion_source::path:
			gather_paths(found.directory, typed, into.candidates);
			break;
		case completion_source::none:
			break;
	}

	tidy(into.candidates);
}

void shell_completer::gather_names(name_domain which, pager_kind kind,
                                   std::string_view typed, std::vector<candidate>& out,
                                   std::vector<std::string>& scratch) const {
	if (_names == nullptr)
		return;
	scratch.clear();
	if (!_names->names(which, scratch))
		return;
	for (const std::string& name : scratch) {
		if (name.size() >= typed.size() && std::string_view{name}.substr(0, typed.size()) == typed)
			add_candidate(name, kind, out);
	}
}

void shell_completer::gather_commands(std::string_view typed, std::vector<candidate>& out,
                                      std::vector<std::string>& scratch) const {
	// A BUILTIN, A FUNCTION AND AN ALIAS ARE ALL `word`, and that is #138's
	// vocabulary rather than an omission: `ls -F` has a marker for a file and
	// none for a shell's own tables, and #137 fixed the marker as the only v1
	// description. F-21 paints those three distinctly on the LINE, which is the
	// highlighter's job over text the user has committed to - a candidate list is
	// not that. Adding three markers would be adding a description vocabulary the
	// pager does not have.
	gather_names(name_domain::builtin, pager_kind::word, typed, out, scratch);
	gather_names(name_domain::function, pager_kind::word, typed, out, scratch);
	gather_names(name_domain::alias, pager_kind::word, typed, out, scratch);

	// THE `$PATH` WALK, HERE AND NOT ON THE SHELL THREAD. See shell_knowledge.h:
	// the shell hands over the split value and this side does the readdir, which
	// is the same readdir path completion is already doing and the same place a
	// memo would go.
	//
	// MEMOIZED PER TAB AND NOT ACROSS TABS: the directories are visited once per
	// call, and a `$PATH` element that appears twice is walked once. A cache that
	// survived between Tabs is spec 6.9's recorded v2 ("per-thread cached copies
	// of the command list - it changes rarely"), and it is deliberately not built
	// here, because a stale one answers a question about a shell that no longer
	// exists.
	if (_names == nullptr)
		return;
	scratch.clear();
	if (!_names->names(name_domain::path_directory, scratch))
		return;
	std::vector<directory_reader::entry> entries;
	std::vector<std::string> walked;
	for (const std::string& where : scratch) {
		if (std::find(walked.begin(), walked.end(), where) != walked.end())
			continue;
		walked.push_back(where);
		entries.clear();
		if (!_directories->read(where, entries))
			continue;
		for (const directory_reader::entry& one : entries) {
			if (one.directory || !one.executable)
				continue;
			if (one.name.size() < typed.size()
			    || std::string_view{one.name}.substr(0, typed.size()) != typed)
				continue;
			add_candidate(one.name, pager_kind::executable, out);
		}
	}
}

void shell_completer::gather_paths(std::string_view directory, std::string_view typed,
                                   std::vector<candidate>& out) const {
	std::string unquoted_directory;
	unquote_into(directory, unquoted_directory);
	const std::string where = expand_tilde_for_listing(unquoted_directory);
	if (where.empty() && !unquoted_directory.empty())
		return;  // `~user/`, or a `$HOME` that is not set: nothing to walk

	std::vector<directory_reader::entry> entries;
	if (!_directories->read(where, entries))
		return;
	// A DOTFILE IS OFFERED ONLY WHEN ASKED FOR, which is every shell's rule and
	// the reason `ls <Tab>` in a home directory is usable at all.
	const bool wants_hidden = !typed.empty() && typed.front() == '.';
	for (const directory_reader::entry& one : entries) {
		if (!wants_hidden && !one.name.empty() && one.name.front() == '.')
			continue;
		if (one.name.size() < typed.size()
		    || std::string_view{one.name}.substr(0, typed.size()) != typed)
			continue;
		// `word` and not `plain` for an ordinary file: a completed argument is
		// followed by another word, so it closes with a space (6.9).
		const pager_kind kind = one.directory ? pager_kind::directory
			: one.symlink                     ? pager_kind::symlink
			: one.executable                  ? pager_kind::executable
			                                  : pager_kind::word;
		add_candidate(one.name, kind, out);
	}
}

} // namespace lesh::leshper
