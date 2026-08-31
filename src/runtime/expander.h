#pragma once

#include "runtime/arithmetic.h"

#include "substrate/arena.h"
#include "substrate/arena_array.h"
#include "syntax/ast.h"
#include "syntax/lexer.h"

#include <string_view>

namespace lesh::runtime {

// Word expansion. See issue #11.
//
// The expander is a pure function of (word node, shell state) producing zero or
// more FIELDS. It is called by the executor PER COMMAND at execution time, not
// as a stage that runs once: POSIX requires a loop body to re-expand on every
// iteration, and `$?` changes between commands.
//
// It depends on two ports rather than on the executor or on a concrete state
// type. That keeps it testable in isolation, and it breaks a cycle every shell
// surveyed in #14 has: command substitution must run a command, and running a
// command must expand words.

// What the expander needs from shell state, as a port - so it does not depend on
// the state's representation, which #12 has not designed yet, and so tests can
// supply a fake.
class parameter_source {
public:
	virtual ~parameter_source() = default;
	// False when the parameter is unset. An unset parameter expands to nothing;
	// whether that is an error under `set -u` is the caller's policy, not ours.
	[[nodiscard]] virtual bool lookup(std::string_view name, std::string_view& value) const = 0;
	[[nodiscard]] virtual std::string_view home_directory() const = 0;
	// A NAMED user's home directory, for `~user`. False when there is no such user,
	// which POSIX leaves unspecified and dash answers by leaving the word alone -
	// `echo ~nosuchuser` prints `~nosuchuser` at status zero. On the port rather
	// than in the expander so the password database stays out of a pure function of
	// (word, shell state), and so a test can name a user that exists nowhere.
	[[nodiscard]] virtual bool home_directory_of(std::string_view user,
	                                             std::string_view& out) const = 0;
	// Field separators. POSIX defaults to space, tab, newline when IFS is unset.
	[[nodiscard]] virtual std::string_view ifs() const = 0;

	// Special and positional parameters. Separate from lookup() because they are
	// not variables: they have no names in the variable table, `shift` renumbers
	// them, and a function replaces them for the duration of a call.
	[[nodiscard]] virtual int last_status_value() const = 0;
	[[nodiscard]] virtual int process_id_value() const = 0;
	[[nodiscard]] virtual size_t positional_count() const = 0;
	// 1-based, matching $1. Returns false past the end.
	[[nodiscard]] virtual bool positional_at(size_t index, std::string_view& out) const = 0;
	[[nodiscard]] virtual std::string_view script_name_value() const = 0;
	// `$-`: the letters of the shell options currently on. A parameter, not a
	// query about options, which is why it belongs on this port rather than
	// reaching into shell state - the expander must not know what an option is.
	[[nodiscard]] virtual std::string_view option_flags() const = 0;
};

// What a command substitution DID. Three answers, not two.
//
// It was a bool, and the bool could not say the thing that mattered: a body that
// will not PARSE is a different answer from one that ran and failed, and the
// difference is exactly what #57 lost. The child refused, `_exit(2)`, and its
// status was indistinguishable from a body that ran `exit 2` - so nothing could
// act on it and `echo $(if true)` reported success.
enum class substitution_result {
	ok,
	// The substitution could not be performed at all: no pipe, or no process.
	// The shell is out of resources rather than the input being wrong.
	unavailable,
	// The body is not shell input, and has already been reported. POSIX makes a
	// syntax error fatal to a non-interactive shell wherever it appears, and this
	// is one - dash refuses `$(if true)` at 2 without running the command around
	// it.
	malformed,
};

// The port that breaks the cycle.
//
// Rather than the expander depending on the executor, the executor supplies
// this. Completion supplies NOTHING, and that is a supported mode: expanding
// without the power to execute is exactly what a line editor needs, and it is
// how Oils' CompletionWordEvaluator and fish's FAIL_ON_CMDSUBST both work.
// Running a destructive command substitution merely to offer a completion would
// be catastrophic, so the type system prevents it rather than a convention
// someone has to remember.
class command_runner {
public:
	virtual ~command_runner() = default;
	[[nodiscard]] virtual substitution_result run_and_capture(std::string_view code,
	                                                          arena_array<char>& out) = 0;
};

// Assigning to a parameter from ${x=default}. Separate from arithmetic's port
// because it writes strings rather than integers.
class parameter_assigner {
public:
	virtual ~parameter_assigner() = default;
	// False when the assignment was REFUSED - a readonly variable - having already
	// reported it. POSIX makes that a variable assignment error, which is fatal to
	// a non-interactive shell, so `readonly x; : ${x=1}; echo not reached` must
	// print nothing: the expander cannot decide that without a status back.
	[[nodiscard]] virtual bool assign_parameter(std::string_view name,
	                                            std::string_view value) = 0;
};

// What a caller wants expanded into a single VALUE rather than into a field list.
//
// All three suppress field splitting and pathname expansion, and all three differ
// in their QUOTING rules - which is exactly what one `quoted` flag could not say.
// It stood for "no field splitting" and "double-quoted backslash rules" at once,
// so a redirection operand kept the backslash in `cat <\i'n'"0"`, an assignment
// kept it in `x=\!`, and a here-document body had its quotes REMOVED because it
// was lexed as the interior of a word (#42).
enum class value_context {
	// `x=value`, and the value half of an `export`/`readonly`/prefix assignment.
	// Unquoted backslash rules, and a tilde is eligible after an unquoted colon.
	assignment,
	// The word after `<` or `>`. Unquoted backslash rules; a tilde is eligible only
	// at the start, because POSIX confines the after-colon rule to assignments.
	redirection_operand,
	// Between the delimiter lines of an unquoted here-document. POSIX 2.7.4 makes
	// it behave as if double-quoted EXCEPT that `"` is not special - so `'` and `"`
	// are ordinary bytes and a backslash escapes only `$`, `` ` ``, `\` and a
	// newline. dash prints `a\"b 'c' "d"` unchanged.
	here_document_body,
};

// POSIX 2.9.1's DECLARATION UTILITY, as a scan over one simple command's words.
//
// `export A=$a` and `readonly A=$a` are assignments wearing a command's clothes:
// POSIX expands their `NAME=value` operands the way it expands a plain `A=$a` -
// a tilde is eligible after an unquoted colon, and neither field splitting nor
// pathname expansion applies. Every such operand went through expand_word like
// an ordinary argument, so `a='x y'; export A=$a` exported `x` and handed `y` to
// export as a second operand, and `export A=~:~` exported the tildes verbatim.
// One wrong entry point, two wrong answers.
//
// It is a SCAN the caller drives rather than a word_role on the node the way a
// `case` pattern is (#54), because the parser cannot know: `v=export; $v A=~:~`
// is a declaration utility in dash, so the utility's name is only settled once
// the first word has been expanded. The executor is the one place that sees a
// command's words in order and their fields as they arrive.
class declaration_scan {
public:
	// True when `word` - the word's text AS WRITTEN - is one of the operands the
	// rule covers. The unexpanded text is what decides, and that is not an
	// approximation: declutil-p.tst asserts both halves of it, `export A=$a` not
	// splitting and `export $a` with a='A B' exporting two names. dash reads the
	// same text and answers the same way, which is why `export "A"=~` leaves the
	// tilde alone - the word does not OPEN with a name.
	[[nodiscard]] bool operand_is_assignment(std::string_view word) const noexcept;

	// Offers the first field a word expanded to, for a word that was not taken as
	// an assignment operand. Only the words BEFORE the utility's name can change
	// the answer: once a declaration utility has been named nothing takes it back,
	// so `export B A=$a` still assigns `x y` as dash does.
	void note_expanded_word(std::string_view field) noexcept;

private:
	enum class state : uint8_t {
		// Before the command name. `command` and its options may still precede it.
		searching,
		// A declaration utility has been named. Sticky.
		declaring,
		// Some other utility has been named.
		other,
	};
	state _state = state::searching;
	// A `command` has been seen, so a following `-p`/`-v`/`-V`/`--` belongs to it
	// rather than being the command name - `command -p export A=~:~` expands both
	// tildes in dash. Tracked rather than assumed of any leading `-`, because in
	// the command name's OWN position a word opening with `-` is the name.
	bool _after_command = false;
};

enum class expansion_status {
	ok,
	command_substitution_unavailable,  // no runner supplied - completion's mode
	unsupported_construct,             // a construct that is not implemented
	parameter_unset,                   // ${x?message} fired
	// An unterminated construct INSIDE an expansion, or expansion nested deeper
	// than the shell will follow. Separate from unsupported_construct because the
	// INPUT is at fault rather than the shell, and POSIX makes it fatal to a
	// non-interactive shell the way a syntax error is (#48).
	malformed_expansion,
	// POSIX 2.8.1's EXPANSION ERROR: the expansion was attempted and could not be
	// performed. Arithmetic that will not evaluate (`$((1/0))`, `$((--))`) and a
	// name that cannot be assigned (`${1=x}`) are the two.
	//
	// Its own value because unsupported_construct was standing for this AND for
	// "lesh has not built that", and a real error wearing an unimplemented
	// construct's value is precisely how `echo $((1/0))` reached the command line
	// as an empty field at status zero (#39). The two want opposite treatment:
	// this one is fatal to a non-interactive shell, and an unbuilt construct must
	// not be.
	expansion_error,
};

class expander {
public:
	// `vars` is optional and separate from `params`, because arithmetic ASSIGNS
	// (`$((i += 1))`) while parameter expansion only reads. Completion supplies
	// none, and arithmetic then evaluates against zeroes rather than mutating
	// state as a side effect of drawing a suggestion.
	expander(buffer_pool& pool, const parameter_source& params,
	         command_runner* runner = nullptr, bool glob_enabled = true,
	         arithmetic_variables* vars = nullptr,
	         parameter_assigner* assign = nullptr,
	         bool unset_is_error = false) noexcept
		: _pool(pool), _params(params), _runner(runner), _glob_enabled(glob_enabled),
		  _vars(vars), _assign(assign), _unset_is_error(unset_is_error) {}

	// True when an expansion reported an error POSIX makes FATAL to a
	// non-interactive shell: `${x?message}` fired, or `set -u` met an unset
	// parameter.
	//
	// Sticky, and separate from the returned expansion_status, because
	// expand_value() returns a VALUE and has no status to give back -
	// so a redirection target or an assignment right-hand side would otherwise
	// swallow the error the caller has to act on.
	[[nodiscard]] bool fatal_error() const noexcept { return _fatal_error; }

	// Expands one word node, appending its fields to `out`.
	//
	// Zero fields is a normal result: an unquoted unset parameter expands to
	// nothing at all, which is why `echo $unset` passes no arguments rather than
	// one empty one.
	expansion_status expand_word(const syntax::tree& t, syntax::node_index word,
	                             arena_array<std::string_view>& out) noexcept;

	// Expands raw text into a single VALUE with no field splitting and no pathname
	// expansion: `x=a b` assigns "a" and runs `b`, but `x="a b"` assigns "a b" as
	// one value, and neither is globbed. The context says which quoting rules
	// apply - see value_context, which exists because they are not all the same.
	[[nodiscard]] std::string_view expand_value(std::string_view text,
	                                            value_context context) noexcept;

private:
	// Which of POSIX's word-expansion treatments apply to a piece of text.
	//
	// One bool used to stand for all of these, and it has now gone wrong three
	// times: `echo "it's"` printed `it` until the LEXING mode became its own
	// parameter (#33), and then `cat <\i'n'"0"` kept its backslash and a
	// here-document body lost its quotes, because "no field splitting" and
	// "double-quoted backslash rules" were still one flag (#42). They are
	// independent properties of a context, not faces of one.
	struct expand_context {
		// Field splitting applies to the result of an unquoted expansion here, and
		// so does pathname expansion - POSIX applies the two to exactly the same
		// text.
		bool split = true;
		// A FIELD LIST is being produced rather than one value, so `"$@"` may yield
		// more than one field. Distinct from `split`: inside double quotes in a
		// command's argument splitting is off while `"$@"` still gives one field per
		// parameter, and in an assignment value neither holds - `x="$@"` joins.
		bool fields = true;
		// POSIX 2.2.3: inside double quotes a backslash escapes only `$`, `` ` ``,
		// `"`, `\` and a newline, and `$@` keeps one field per parameter.
		bool double_quoted = false;
		// The text IS the result of an expansion - the argument of a `${x-word}` or
		// `${x+word}` operator - so its own unquoted literal bytes are part of what
		// field splitting sees. In an ordinary word they are not: `echo a b` is two
		// words because the LEXER split them, and `echo "$x"` on `a b` is one field.
		// But `${a+ x}` really does substitute a leading blank that then separates,
		// which is why the two cannot share one flag.
		bool substituted = false;
		// The text is a PATTERN - the argument of `${x#word}` and friends. Quoting
		// inside it is TRANSLATED into backslash escapes rather than removed,
		// because a matcher's only channel for "this asterisk is data" is a
		// backslash: `${s#'*'}` trims one literal asterisk, while `${a#*1}` still
		// wildcards. Removing the quotes lost the distinction and made every quoted
		// metacharacter a metacharacter again.
		bool pattern = false;
		// The text is the interior of a `${...}`, where a `}` would END the expansion
		// - so a backslash escapes one even inside double quotes, which is the only
		// way to write a literal brace there. `"${a+a\}b}"` is `a}b` in dash and was
		// `a\}b` here, the whole of quote-p.tst's remaining three cases.
		bool in_braces = false;
		// How the text is LEXED, which is a third thing again: a single quote is an
		// ordinary byte inside double quotes and in a here-document body, and a
		// tilde is eligible only where a word begins.
		//
		// Last, so that a designated initialiser reads in the order POSIX applies
		// the rules; every construction site names its fields, because a positional
		// one silently reassigned the members the day `substituted` was added.
		syntax::lex_mode mode = syntax::lex_mode::word_interior;

		// True when a backslash before `c` escapes it. The here-document body is the
		// case that forced this out of the double_quoted flag: it has double-quote
		// rules minus `"`, so `\"` stays `\"` there and becomes `"` inside quotes.
		[[nodiscard]] constexpr bool escapes(char c) const noexcept {
			if (!double_quoted)
				return true;  // outside quotes a backslash escapes anything
			if (c == '$' || c == '`' || c == '\\' || c == '\n')
				return true;
			if (c == '}' && in_braces)
				return true;
			return c == '"' && mode != syntax::lex_mode::here_doc_body;
		}
	};

	expansion_status expand_text(std::string_view text, expand_context ctx,
	                             arena_array<std::string_view>& out) noexcept;
	// Expands `text` into one value with the quoting rules of `outer`, which is what
	// the argument of `${x=word}` and `${x?word}` needs: the value assigned or the
	// message printed is one string, but its backslashes and quotes read the same
	// way as the text around the expansion.
	[[nodiscard]] std::string_view expand_to_value(std::string_view text,
	                                               expand_context outer) noexcept;
	void append(std::string_view bytes) noexcept;
	void append_split(std::string_view bytes, arena_array<std::string_view>& out) noexcept;
	void push_byte(char c) noexcept;
	// The same context, marked as the interior of a `${...}`.
	[[nodiscard]] static expand_context brace_ctx(expand_context ctx) noexcept;
	// A segment body with its line continuations removed. See the definition.
	[[nodiscard]] std::string_view without_continuations(std::string_view body) noexcept;
	// The code inside backquotes, with the escapes POSIX 2.6.3 removes.
	[[nodiscard]] std::string_view unescape_backquotes(std::string_view code,
	                                                   bool in_double_quotes) noexcept;
	// Bytes that arrived QUOTED: escaped in a pattern context; verbatim in the
	// field otherwise, and escaped in the PATTERN FORM beside it when one is being
	// built - see _pattern.
	void append_quoted(std::string_view bytes, expand_context ctx) noexcept;
	// Marks the next byte appended to the field as DATA in the pattern form.
	void escape_in_pattern() noexcept;
	// Bytes that came out of an EXPANSION: escaped only when the expansion itself
	// was inside double quotes, because an unquoted `${a}` in a pattern is a
	// pattern - `w='ab\bc' a='*'; ${w#${a}b}` matches while `${w#"${a}b"}` does not.
	void append_value(std::string_view bytes, expand_context ctx) noexcept;
	bool finish_field(arena_array<std::string_view>& out, bool even_if_empty = false) noexcept;

	// Where a run of IFS separators stands. Held across segments because ONE
	// separator can be split between them: `$a$b` with a='1 ' and b=' 2' is two
	// fields, not three, and the two spaces belong to one separator.
	enum class split_run : uint8_t {
		none,       // in a field, or before the word's first byte
		white,      // inside a separator that has seen only IFS white space
		delimited,  // inside a separator whose one non-white-space slot is used
	};
	split_run _run = split_run::none;
	// Whether the separator in progress has already ended a field. A separator owes
	// exactly one field boundary, and which byte of it pays depends on whether
	// there was a field to close when the run began.
	bool _run_closed_a_field = false;

	buffer_pool& _pool;
	const parameter_source& _params;
	command_runner* _runner;
	// `set -f` disables pathname expansion entirely.
	bool _glob_enabled = true;
	arithmetic_variables* _vars = nullptr;
	parameter_assigner* _assign = nullptr;
	// `set -u`: expanding an unset parameter is an error rather than an empty
	// string. Held as state rather than asked of the parameter_source, because
	// POSIX makes it the CALLER's policy - completion expands the same words
	// without wanting the shell to die over an unset variable.
	bool _unset_is_error = false;
	bool _fatal_error = false;

	// Reports an unset parameter under `set -u` and records the fatal error.
	// Returns true when it fired, so a caller can skip substituting nothing.
	bool report_unset(std::string_view name) noexcept;

	// Reports an unterminated construct found inside an expansion and records the
	// fatal error. Worded exactly as the parser words it, because which layer
	// caught it is lesh's business and not the user's.
	void report_malformed(syntax::token_error error) noexcept;

	// Reports arithmetic that would not evaluate and records the fatal error.
	// `expression` is the text as the evaluator received it, which dash prints
	// too - `$((1/0))` and `$((x/0))` are the same complaint about different
	// input, and the message that names neither is the one nobody can act on.
	void report_arithmetic(const char* why, std::string_view expression) noexcept;

	// How deep expansion may nest.
	//
	// The expander is re-entrant BY DESIGN - a parameter default, an assignment
	// value and arithmetic's inner text all re-enter expand_text - so nesting in
	// the input is nesting on the C++ stack, and refusing malformed input bounds
	// the recursion only by the LENGTH of the input, which is not a bound. Measured
	// on the perfectly well-formed `${x-${x-...hi...}}`: 1500 levels expanded,
	// 2000 overflowed the stack on the debug build. dash is not a counterexample,
	// only a bigger frame budget - it prints `hi` at 16000 and takes SIGSEGV at
	// 18000, so the reference implementation has this bug too and answers it with
	// a crash.
	//
	// 256 mirrors the executor's kMaxFunctionDepth, for the same reason and with
	// the same shape of answer: a diagnostic and a non-zero status, never a silent
	// empty result. Human-written expansion nests less than ten deep.
	static constexpr int kMaxExpansionDepth = 256;
	int _depth = 0;

	bool lookup_parameter(std::string_view name, std::string_view& out) noexcept;
	std::string_view int_to_scratch(int value) noexcept;

	// Accumulates the field under construction. Completed fields are copied into
	// exact-size arena blocks, because this buffer relocates as it grows and a
	// string_view into it would dangle.
	arena_array<char>* _current = nullptr;
	bool _field_started = false;
	// Set when a glob metacharacter arrived from unquoted text. Quoted ones are
	// literal, so `echo "*.txt"` must not touch the filesystem.
	bool _field_globbable = false;

	// The PATTERN FORM of the field under construction: the same bytes as
	// _current, except that one which arrived QUOTED is preceded by a backslash.
	//
	// A SECOND form rather than escaping _current itself, because POSIX 2.6 gives
	// pathname expansion and quote removal different text and the field is what
	// quote removal produced. Escaping in place and unescaping at the end cannot
	// be right: `\Q` arriving in a VARIABLE'S VALUE is a live pattern escape that
	// quote removal must LEAVE (`x='\Q'; echo *$x` prints `*\Q`) while the `\Q` a
	// user typed must go (`echo *\Q` prints `*Q`), and one backslash cannot say
	// which it was. So the field stays exactly what it was before #210 and the
	// walk gets its own copy - non-null only in expand_word, and only where
	// pathname expansion could read it.
	//
	// Written only from the first quoted metacharacter of a field onwards: until
	// then the two forms are the same bytes, which _field_escaped records and
	// which is every field in almost every script. escape_in_pattern catches it up.
	arena_array<char>* _pattern = nullptr;
	// One pattern form per completed field, in the order finish_field emitted them,
	// so pattern_fields[k] belongs to the field at out[before_fields + k].
	arena_array<std::string_view>* _pattern_fields = nullptr;
	// Whether _pattern holds this field. False means "the field IS its pattern".
	bool _field_escaped = false;

	// Whether the text just expanded held a `"$@"` with no positional parameters,
	// and whether it held anything else. POSIX: `"$@"` with nothing to expand
	// yields ZERO fields even inside double quotes, so the quotes do not start one
	// either - `set --; bracket "$@"` prints nothing while `bracket "$null$@"`
	// prints one empty field, because the empty VARIABLE is content and the `$@` is
	// not. Two flags rather than one, because `""` must still start a field.
	bool _saw_empty_at = false;
	bool _saw_other_content = false;
};

} // namespace lesh::runtime
