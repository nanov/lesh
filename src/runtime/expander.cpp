#include "runtime/expander.h"

#include "substrate/numeric.h"

#include "runtime/arithmetic.h"
#include "runtime/glob.h"
#include "runtime/pattern.h"

#include "substrate/char_utils.h"

#include "syntax/lexer.h"

#include <cstdio>
#include <cstring>
#include <tuple>

namespace lesh::runtime {

using syntax::lex_mode;
using syntax::lexer;
using syntax::token;
using syntax::token_error;
using syntax::token_kind;

namespace {

bool is_ifs(char c, std::string_view ifs) noexcept {
	return ifs.find(c) != std::string_view::npos;
}

// "IFS white space" is the subset of IFS that is space, tab or newline. POSIX
// treats it differently from the rest of IFS at both ends of the input and around
// a non-white-space separator, which is the whole reason field splitting is a
// state machine rather than a scan for separators. `read` already had to make this
// distinction (builtins.cpp split_line); this is the same rule for expansions.
bool is_ifs_whitespace(char c, std::string_view ifs) noexcept {
	return (c == ' ' || c == '\t' || c == '\n') && is_ifs(c, ifs);
}

// True for an unterminated construct whose DELIMITERS this file strips off the
// segment before working on what is left. An unterminated one means the bytes it
// would strip are not there, and the arithmetic case then re-expands the segment
// unchanged - which is how `${x-$((1}` came to recurse until the stack ran out.
//
// The two QUOTE errors are deliberately absent. Whether a quote inside `${x-...}`
// is a quote at all depends on the double-quote context POSIX 2.6.2 gives it -
// `dash -c 'echo "${x-'"'"'}"'` prints one single quote at status zero - and
// the expander re-lexes its text without that context yet (#42). So
// refusing there would trade a crash for a diagnostic on input dash accepts.
// Every case is listed and there is no default, so a new token_error cannot be
// left out of this decision silently - the release build takes -Wswitch as an
// error.
constexpr bool is_unterminated_substitution(token_error error) noexcept {
	switch (error) {
		case token_error::unterminated_command_sub:
		case token_error::unterminated_backquote:
		case token_error::unterminated_arithmetic:
		case token_error::unterminated_parameter_expansion:
			return true;
		case token_error::unterminated_single_quote:
		case token_error::unterminated_double_quote:
		case token_error::unexpected_byte:
		case token_error::none:
			return false;
	}
	return false;
}

// True for a name the LENGTH form can be taken of: a variable name, `*`, `@`, or
// a positional number. What decides whether `${#...}` is a length at all.
//
// POSIX 2.6.2 disambiguates `${#-word}` this way and dash agrees: `#` followed by
// something that cannot be a name is the parameter `#` with an operator, not the
// length of a parameter with a peculiar name. Read as a length, `${#+y}` gave the
// length of an unset parameter called `+y` - zero - where dash prints `y`.
bool is_length_target(std::string_view name) noexcept {
	if (name.empty())
		return false;
	// A SPECIAL parameter, but only when it is the whole rest of the body. `${#?}`
	// is the length of `$?` and `${#?X}` is the count with an error-if-unset
	// message, which is the distinction param-p.tst asserts in two directions at
	// once - :236 wants the first and :268 the second.
	if (name.size() == 1) {
		switch (name[0]) {
			case '*': case '@': case '?': case '$': case '!': case '-': case '#':
				return true;
			default:
				break;
		}
	}
	bool all_digits = true;
	for (const char c : name)
		all_digits = all_digits && c >= '0' && c <= '9';
	if (all_digits)
		return true;
	if (!lesh::string_utils::is_valid_var_name_first_char(
	        static_cast<unsigned char>(name[0])))
		return false;
	for (const char c : name.substr(1)) {
		if (!lesh::string_utils::is_valid_var_name_non_first_char(
		        static_cast<unsigned char>(c)))
			return false;
	}
	return true;
}

// True for a name `${x=...}` may assign to: a variable, and nothing else. A
// positional or special parameter is not assignable, which is a diagnostic rather
// than a silent substitution.
bool is_assignable_name(std::string_view name) noexcept {
	if (name.empty() || !lesh::string_utils::is_valid_var_name_first_char(
	                        static_cast<unsigned char>(name[0])))
		return false;
	for (const char c : name.substr(1)) {
		if (!lesh::string_utils::is_valid_var_name_non_first_char(
		        static_cast<unsigned char>(c)))
			return false;
	}
	return true;
}

// Past any line continuations at `at`.
constexpr size_t past_continuations(std::string_view text, size_t at) noexcept {
	while (at + 1 < text.size() && text[at] == '\\' && text[at + 1] == '\n')
		at += 2;
	return at;
}

// The body of a parameter segment: what sits between `${` and `}`, or after `$`.
std::string_view parameter_body(std::string_view segment) noexcept {
	// Past the line continuations after the `$`, which POSIX removed before the
	// input was tokenised: `$\<newline>{f}` is `${f}`.
	const size_t at = past_continuations(segment, 1);
	if (at < segment.size() && segment[at] == '{') {
		const size_t close = segment.rfind('}');
		if (close == std::string_view::npos || close < at + 1)
			return segment.substr(at + 1);
		return segment.substr(at + 1, close - at - 1);
	}
	return segment.substr(at);
}

// How a ${...} form modifies the value. POSIX's set, with the colon variants
// distinguishing "unset" from "unset or empty" - a real difference: `${x:-d}`
// substitutes for an empty x while `${x-d}` does not.
enum class param_op {
	none,
	length,        // ${#x}
	use_default,   // ${x-d}  ${x:-d}
	assign_default,// ${x=d}  ${x:=d}
	error_if_unset,// ${x?m}  ${x:?m}
	use_alternate, // ${x+d}  ${x:+d}
	trim_prefix_short,  // ${x#pat}
	trim_prefix_long,   // ${x##pat}
	trim_suffix_short,  // ${x%pat}
	trim_suffix_long,   // ${x%%pat}
};

struct parsed_parameter {
	std::string_view name;
	param_op op = param_op::none;
	std::string_view argument;
	bool colon = false;  // the colon variants also treat empty as unset
};

parsed_parameter parse_parameter(std::string_view body) noexcept {
	parsed_parameter out;
	if (body.empty())
		return out;

	// A body that is exactly one special parameter is a NAME, not an operator.
	// `$?` is the exit status; `${x?msg}` is the error-if-unset form. Without this
	// the operator scan reads `?` as the operator and leaves an empty name, which
	// turned `echo $?` into "parameter null or not set".
	if (body.size() == 1) {
		switch (body[0]) {
			case '?': case '#': case '$': case '!': case '@': case '*': case '-':
				out.name = body;
				return out;
			default:
				break;
		}
		if (body[0] >= '0' && body[0] <= '9') {
			out.name = body;
			return out;
		}
	}

	if (body[0] == '#' && body.size() > 1 && is_length_target(body.substr(1))) {
		// ${#x} is length - but `$#` alone is the positional count, which reaches
		// here as a bare name rather than through this branch, and `${#-y}` is that
		// count with a default rather than the length of a parameter called `-y`.
		out.op = param_op::length;
		out.name = body.substr(1);
		return out;
	}

	// Scan for the operator, which starts after the name. It starts at 1 for `#`
	// because `#` is itself a parameter name and every operator character can
	// follow it: `${#-y}`, `${#+y}`, `${#=y}`. Starting at 0 read the `#` AS the
	// operator and left the name empty.
	size_t at = body[0] == '#' ? 1 : 0;
	while (at < body.size() && body[at] != ':' && body[at] != '-' && body[at] != '=' &&
	       body[at] != '?' && body[at] != '+' && body[at] != '#' && body[at] != '%')
		++at;

	out.name = body.substr(0, at);
	if (at >= body.size())
		return out;

	if (body[at] == ':' && at + 1 < body.size()) {
		out.colon = true;
		++at;
	}

	const char op = body[at];
	// Only `#` and `%` have a doubled form. Testing every operator for a repeated
	// character ate the first byte of the ARGUMENT: `${u--x}` is the default `-x`,
	// not a `--` operator, and it substituted `x` where dash gives `-x`. That was
	// three of fsplit-p.tst's cases, whose defaults all begin with a hyphen.
	const bool doubled = (op == '#' || op == '%') && at + 1 < body.size() &&
	                     body[at + 1] == op;
	switch (op) {
		case '-': out.op = param_op::use_default; break;
		case '=': out.op = param_op::assign_default; break;
		case '?': out.op = param_op::error_if_unset; break;
		case '+': out.op = param_op::use_alternate; break;
		case '#': out.op = doubled ? param_op::trim_prefix_long : param_op::trim_prefix_short; break;
		case '%': out.op = doubled ? param_op::trim_suffix_long : param_op::trim_suffix_short; break;
		default: return out;
	}
	at += doubled ? 2 : 1;
	out.argument = at <= body.size() ? body.substr(at) : std::string_view{};
	return out;
}

// The code inside `$(...)` or backticks.
std::string_view substitution_body(std::string_view segment) noexcept {
	if (segment.size() >= 2 && segment[0] == '`')
		return segment.substr(1, segment.size() >= 2 ? segment.size() - 2 : 0);
	if (segment.size() >= 3)
		return segment.substr(2, segment.size() - 3);
	return {};
}

} // namespace

void expander::append(std::string_view bytes) noexcept {
	for (const char c : bytes)
		_current->push(c);
	// Content, so any separator run is over even when `bytes` is empty: `''` after
	// a separator starts a field, which is what makes `bracket ''$b''` two fields
	// rather than one (fsplit-p.tst 'empty field removal').
	_run = split_run::none;
	_run_closed_a_field = false;
	_field_started = true;
}

// Splits on IFS as it appends. Only the RESULT of an unquoted expansion is split;
// literal text never is, which is why `a b` is one word and `$x` holding "a b" is
// two fields. Getting that distinction wrong is the classic shell bug.
//
// POSIX 2.6.5 gives a separator a SHAPE rather than a set of bytes: IFS white
// space, then at most one non-white-space IFS character, then more IFS white
// space. Two consequences the previous "drop every IFS byte" loop got wrong, for
// ten of fsplit-p.tst's twelve failures: `IFS=-; echo [$a]` on `a=1--2` has an
// EMPTY field between the two separators, and a separator that has used its
// non-white-space slot ends the run, so a second one starts a new separator.
// White space at the very start of the word is not a separator at all, which is
// why the two cases below differ.
void expander::append_split(std::string_view bytes,
                            arena_array<std::string_view>& out) noexcept {
	const std::string_view ifs = _params.ifs();
	for (const char c : bytes) {
		if (!is_ifs(c, ifs)) {
			push_byte(c);
			continue;
		}
		const bool white = is_ifs_whitespace(c, ifs);
		if (_run == split_run::none) {
			// White space closes a field that EXISTS; before the word's first byte
			// there is nothing to close, so `echo [$a]` on `a=' 1'` is one field
			// while `echo [-$a]` is two. A non-white-space IFS character always ends
			// a field, even an empty one: `IFS=-; echo [$a]` on `a=-1` yields an
			// empty field and then `1`.
			const bool closed = finish_field(out, /*even_if_empty=*/!white);
			_run = white ? split_run::white : split_run::delimited;
			_run_closed_a_field = closed;
			continue;
		}
		if (white)
			continue;  // trailing white space of the same separator
		if (_run == split_run::white && _run_closed_a_field) {
			// Fills this separator's one non-white-space slot, and the leading white
			// space has already ended the field, so nothing more is owed:
			// `IFS=' -'; a='1 -2'` is two fields, not three.
			_run = split_run::delimited;
			continue;
		}
		// Either the leading white space had no field to close - `IFS=' -'` on
		// `'  --33'` - or the separator has already used its slot, so this byte
		// begins a NEW one and the field between them is empty. Both owe one.
		std::ignore = finish_field(out, /*even_if_empty=*/true);
		_run = split_run::delimited;
		_run_closed_a_field = true;
	}
}

// Bytes a pattern matcher must read as DATA. Escaping rather than removing the
// quotes is how the quoting survives expansion: `\*` is the only channel there is
// for telling the matcher that an asterisk is an asterisk.
void expander::append_quoted(std::string_view bytes, expand_context ctx) noexcept {
	if (!ctx.pattern) {
		append(bytes);
		return;
	}
	for (const char c : bytes) {
		if (is_pattern_syntax(c))
			_current->push('\\');
		push_byte(c);
	}
	_field_started = true;
}

void expander::append_value(std::string_view bytes, expand_context ctx) noexcept {
	// Only a value that was inside double quotes is data. An unquoted one is a
	// PATTERN, which is what makes `${w#${a}b}` and `${w#"${a}b"}` differ when a
	// holds an asterisk - param-p.tst's 'parameter expansion in embedded pattern'.
	append_quoted(bytes, ctx.double_quoted ? ctx : expand_context{.pattern = false});
}

// The interior of an expansion with its LINE CONTINUATIONS removed - except inside
// SINGLE quotes, where a backslash-newline is two literal bytes and nothing else.
//
// POSIX 2.2.1 removes them before the input is tokenised, but the lexer records
// the extent a segment SPANS rather than the text it means, so the expander is
// where `${\<newline>f\<newline>}` has to become `${f}`. It cannot be done by the
// lexer without rewriting the input, and it cannot be left to the literal-segment
// handler either: by then the NAME and the OPERATOR have already been read off the
// raw bytes, and `${f\<newline>#\<newline>f}` had a name of `f\<newline>` and no
// operator at all (quote-p.tst's 'line continuation in parameter expansion').
//
// Returns the body unchanged when there is nothing to remove, which is almost
// always, and costs no allocation then.
std::string_view expander::without_continuations(std::string_view body) noexcept {
	if (body.find('\\') == std::string_view::npos)
		return body;
	char* block = nullptr;
	_pool.allocate(body.empty() ? 1 : body.size(), block, 1);
	size_t written = 0;
	bool in_single_quotes = false;
	for (size_t i = 0; i < body.size(); ++i) {
		if (body[i] == '\'')
			in_single_quotes = !in_single_quotes;
		if (!in_single_quotes && body[i] == '\\' && i + 1 < body.size() &&
		    body[i + 1] == '\n') {
			++i;
			continue;
		}
		block[written++] = body[i];
	}
	return {block, written};
}

// The code inside backquotes, with the escapes POSIX 2.6.3 removes.
//
// A backslash there retains its literal meaning EXCEPT before `$`, a backquote or
// another backslash - and, inside double quotes, before a `"`, which is only in
// the source to stop the quoted string ending. Removing them is unconditional
// rather than quote-aware, which is the standard's rule and observably so:
// `` `echoraw '\$y'` `` prints `$y`, so the `\$` inside SINGLE quotes was still
// unescaped before the body was ever parsed.
//
// Without this the body was handed to the parser exactly as written, so
// `` `echo \`echo x\`` `` was a syntax error rather than a nested substitution and
// `"`echo \"1\"`"` printed `"1"` - four of cmdsub-p.tst's cases.
std::string_view expander::unescape_backquotes(std::string_view code,
                                               bool in_double_quotes) noexcept {
	if (code.find('\\') == std::string_view::npos)
		return code;
	char* block = nullptr;
	_pool.allocate(code.empty() ? 1 : code.size(), block, 1);
	size_t written = 0;
	for (size_t i = 0; i < code.size(); ++i) {
		if (code[i] == '\\' && i + 1 < code.size()) {
			const char next = code[i + 1];
			if (next == '$' || next == '`' || next == '\\' ||
			    (in_double_quotes && next == '"')) {
				block[written++] = next;
				++i;
				continue;
			}
		}
		block[written++] = code[i];
	}
	return {block, written};
}

// The same context, marked as the interior of a `${...}`. Named rather than
// written out at each of the four argument sites, because forgetting it at one of
// them is a difference nobody would see until a `}` came out with a backslash.
expander::expand_context expander::brace_ctx(expand_context ctx) noexcept {
	ctx.in_braces = true;
	return ctx;
}

// One byte of ordinary text. Ends any separator run in progress, which is what
// makes a separator's trailing white space belong to the separator rather than
// to the field after it.
void expander::push_byte(char c) noexcept {
	_run = split_run::none;
	_run_closed_a_field = false;
	_current->push(c);
	_field_started = true;
}

// Closes the field under construction. Returns whether a field was actually
// emitted, which is what append_split needs to know whether a separator run still
// owes one.
bool expander::finish_field(arena_array<std::string_view>& out, bool even_if_empty) noexcept {
	// A separator run is over the moment a field boundary is taken, so the next
	// byte starts a fresh one. Cleared here rather than at every call site because
	// "$@" takes boundaries too.
	_run = split_run::none;
	_run_closed_a_field = false;
	if (!_field_started && !even_if_empty)
		return false;
	const size_t n = _current->size();
	char* block = nullptr;
	// Exact-size copy out of the accumulator: the accumulator relocates as it
	// grows, so a view into it would dangle the moment the next field is built.
	_pool.allocate(n == 0 ? 1 : n, block);
	if (n > 0)
		std::memcpy(block, _current->data(), n);
	out.push(std::string_view(block, n));
	_current->truncate(0);
	_field_started = false;
	return true;
}

// Resolves a parameter name, including the special ones. `$?`, `$#`, `$$` and
// `$1` are not variables and are not in the variable table.
bool expander::lookup_parameter(std::string_view name, std::string_view& out) noexcept {
	if (name.empty())
		return false;

	if (name == "?") {
		out = int_to_scratch(_params.last_status_value());
		return true;
	}
	if (name == "#") {
		out = int_to_scratch(static_cast<int>(_params.positional_count()));
		return true;
	}
	if (name == "$") {
		out = int_to_scratch(_params.process_id_value());
		return true;
	}
	if (name == "0") {
		out = _params.script_name_value();
		return true;
	}
	if (name == "-") {
		out = _params.option_flags();
		return true;
	}
	if (name == "@" || name == "*") {
		// ALWAYS set, even with no positional parameters: `${@-unset}` is empty in
		// dash rather than `unset`, and only the colon forms treat it as absent.
		// Reported unset, `${@=x}` took the assignment path and reported a bad
		// variable name where dash substitutes nothing at status zero.
		arena_array<char> joined{_pool, 32};
		const std::string_view sep = _params.ifs();
		const size_t count = _params.positional_count();
		for (size_t i = 1; i <= count; ++i) {
			std::string_view arg;
			if (!_params.positional_at(i, arg))
				continue;
			if (i > 1 && !sep.empty())
				joined.push(sep[0]);
			for (const char c : arg)
				joined.push(c);
		}
		char* block = nullptr;
		_pool.allocate(joined.size() == 0 ? 1 : joined.size(), block, 1);
		if (joined.size() > 0)
			std::memcpy(block, joined.data(), joined.size());
		out = std::string_view(block, joined.size());
		return true;
	}
	if (name[0] >= '1' && name[0] <= '9') {
		// AN INDEX PAST THE END IS AN UNSET PARAMETER, so this site CLAMPS - and the
		// clamp is not a compromise here, it is the right answer arrived at cheaply.
		// INT64_MAX is past every $# there can be, so a value too large to represent
		// substitutes nothing, which is what `${18446744073709551617}` should always
		// have done. It used to accumulate into a size_t and WRAP: 2^64 + 1 came back
		// as 1, and the expansion substituted `$1` (#63).
		//
		// A name that is not all digits is not an index at all - `1a` is a variable
		// name - so not_a_number falls through to the ordinary lookup rather than
		// being refused. dash, bash and zsh disagree with each other about where the
		// wrap happens; none of them substitutes an argument the script did not ask
		// for once the number is past their own limit.
		const numeric_result parsed =
			parse_integer(name, numeric_site::positional_parameter_index);
		if (parsed.status == numeric_parse::not_a_number)
			return _params.lookup(name, out);
		return _params.positional_at(static_cast<size_t>(parsed.value), out);
	}
	return _params.lookup(name, out);
}

// `set -u` met an unset parameter. Returns true when that is an error, so the
// caller can stop rather than substitute nothing.
//
// The message matches dash's `x: parameter not set` because dash is the reference
// for the POSIX floor, minus its line number, which lesh does not track.
bool expander::report_unset(std::string_view name) noexcept {
	if (!_unset_is_error)
		return false;
	std::fprintf(stderr, "lesh: %.*s: parameter not set\n",
	             static_cast<int>(name.size()), name.data());
	_fatal_error = true;
	return true;
}

// The word scan could not see this one, so the expander is where it is reported.
// Worded as the parser words it, from the same table, so `echo $((1` and
// `echo ${x-$((1}` - the same defect one nesting level apart - do not answer
// differently depending on which layer noticed.
void expander::report_malformed(token_error error) noexcept {
	const char* phrase = syntax::error_phrase(error);
	std::fprintf(stderr, "lesh: syntax error: %s\n",
	             phrase != nullptr ? phrase : "malformed expansion");
	_fatal_error = true;
}

// Arithmetic that would not evaluate. POSIX 2.8.1 makes it an EXPANSION ERROR,
// which is fatal to a non-interactive shell - and until this existed there was no
// message at all: `echo $((1/0))` printed an empty line and reported success.
//
// dash's shape, minus the line number lesh does not track:
// `dash: 1: arithmetic expression: division by zero: "1/0"`. The expression is
// quoted back because the evaluator's `why` alone cannot say which of several
// `$(( ))` on a line is the one that failed.
void expander::report_arithmetic(const char* why, std::string_view expression) noexcept {
	std::fprintf(stderr, "lesh: arithmetic expression: %s: \"%.*s\"\n",
	             why != nullptr ? why : "invalid expression",
	             static_cast<int>(expression.size()), expression.data());
	_fatal_error = true;
}

// Formats an integer into arena storage so the returned view outlives the call.
std::string_view expander::int_to_scratch(int value) noexcept {
	char digits[24];
	const int n = std::snprintf(digits, sizeof(digits), "%d", value);
	const size_t len = n > 0 ? static_cast<size_t>(n) : 0;
	char* block = nullptr;
	_pool.allocate(len == 0 ? 1 : len, block, 1);
	std::memcpy(block, digits, len);
	return {block, len};
}

expansion_status expander::expand_text(std::string_view text, expand_context ctx,
                                       arena_array<std::string_view>& out) noexcept {
	// BOUNDED. Every nesting level in the input is a frame on this stack: a
	// parameter default and arithmetic's inner text both come back here through
	// expand_to_value. 2000 levels of the WELL-FORMED `${x-${x-...}}`
	// exhausted the stack on the debug build, so refusing malformed input is not
	// enough on its own - see kMaxExpansionDepth (#48).
	if (_depth >= kMaxExpansionDepth) {
		std::fprintf(stderr, "lesh: expansion nested too deeply\n");
		_fatal_error = true;
		return expansion_status::malformed_expansion;
	}
	++_depth;

	expansion_status status = expansion_status::ok;
	lexer lx{text};

	for (;;) {
		const token seg = lx.next(ctx.mode);
		if (seg.kind == token_kind::end)
			break;
		// Refused HERE because nothing upstream could have refused it. `${x-$((1}`
		// is well formed at the command level - the word scan counts braces and the
		// `}` closes the expansion - so the parser sees no defect and the damage is
		// entirely inside the expansion. The lexer had recorded it on the segment
		// all along; this loop simply never asked, and the arithmetic case below
		// then stripped `$((` and `))` from a segment too short to hold them and
		// re-expanded the same bytes forever (#48).
		if (is_unterminated_substitution(seg.error)) {
			report_malformed(seg.error);
			status = expansion_status::malformed_expansion;
			break;
		}
		const std::string_view body = text.substr(seg.offset, seg.length);
		// Content unless proven otherwise, which only the empty `"$@"` below does.
		bool segment_is_content = true;

		switch (seg.kind) {
			case token_kind::seg_literal: {
				// Quoting suppresses pathname expansion: `echo "*.txt"` must print
				// *.txt, not a filename. So a field is only glob-eligible when a
				// metacharacter arrived from unquoted text.
				if (ctx.split && has_pattern_characters(body))
					_field_globbable = true;
				// Backslash removal is part of quote removal, and which bytes a
				// backslash escapes depends on the context - see
				// expand_context::escapes, where the three sets live.
				for (size_t i = 0; i < body.size(); ++i) {
					if (body[i] == '\\' && i + 1 < body.size() && body[i + 1] == '\n') {
						// A line continuation: BOTH characters vanish. Removing only
						// the backslash left the newline in the field, so
						// `echo one\<newline>two` printed two lines.
						++i;
						continue;
					}
					if (body[i] == '\\' && i + 1 < body.size()) {
						const char next = body[i + 1];
						if (ctx.escapes(next)) {
							// An escaped byte is quoted, so it never separates fields even
							// where the unescaped one would: `${a+\ x}` is one field - and
							// in a pattern it stays escaped rather than turning back into a
							// metacharacter.
							append_quoted(body.substr(i + 1, 1), ctx);
							++i;
							continue;
						}
					}
					if (ctx.pattern && ctx.double_quoted)
						append_quoted(body.substr(i, 1), ctx);
					else if (ctx.split && ctx.substituted)
						append_split(body.substr(i, 1), out);
					else
						push_byte(body[i]);
				}
			} break;

			case token_kind::seg_single_quoted: {
				// Nothing inside single quotes is ever special, and the quotes
				// themselves are removed. An empty '' still starts a field. In a
				// pattern the quoting becomes escapes rather than nothing.
				if (body.size() >= 2)
					append_quoted(body.substr(1, body.size() - 2), ctx);
				_field_started = true;
			} break;

			case token_kind::seg_double_quoted: {
				// Expansion happens inside, field splitting does not - but whether a
				// FIELD LIST is being built is the enclosing context's business, so
				// `"$@"` in a command argument still gives one field per parameter
				// while `x="$@"` joins.
				//
				// The quotes start a field, EXCEPT when all they contain is a `"$@"`
				// with no positional parameters: POSIX makes that zero fields, quotes
				// and all, so `set --; bracket "$@"` prints nothing. Forcing the field
				// here unconditionally printed one empty one (param-p.tst:560), while
				// `bracket "$null$@"` must still print it - an empty variable is
				// content and an absent `$@` is not.
				const bool outer_empty_at = _saw_empty_at;
				const bool outer_other = _saw_other_content;
				_saw_empty_at = false;
				_saw_other_content = false;
				if (body.size() >= 2) {
					// The interior is lexed as the inside of double quotes: a single
					// quote there is an ordinary byte, not the start of a quoted run.
					const expansion_status inner =
						expand_text(body.substr(1, body.size() - 2),
						            expand_context{.split = false,
						                           .fields = ctx.fields,
						                           .double_quoted = true,
						                           .pattern = ctx.pattern,
						                           .mode = lex_mode::double_quote_interior},
						            out);
					if (inner != expansion_status::ok)
						status = inner;
				}
				const bool quotes_hold_only_an_absent_at =
					_saw_empty_at && !_saw_other_content;
				_saw_empty_at = outer_empty_at;
				_saw_other_content = outer_other;
				if (!quotes_hold_only_an_absent_at)
					_field_started = true;
				else
					segment_is_content = false;
			} break;

			case token_kind::seg_parameter: {
				const std::string_view pbody =
					parameter_body(without_continuations(body));
				const parsed_parameter p = parse_parameter(pbody);

				// $@ and $* are the only expansions whose FIELD COUNT depends on
				// quoting: "$@" produces one field per positional parameter, while
				// "$*" produces exactly one, joined by the first character of IFS.
				// Getting this wrong breaks every argument-forwarding script ever
				// written, which is why it is handled before anything else.
				if (p.op == param_op::none && (p.name == "@" || p.name == "*")) {
					const size_t count = _params.positional_count();
					if (ctx.double_quoted && ctx.fields && p.name == "@" && count == 0) {
						// Nothing at all - not even the field the quotes would start.
						// The enclosing seg_double_quoted reads this.
						segment_is_content = false;
						_saw_empty_at = true;
						break;
					}
					if (ctx.double_quoted && ctx.fields && p.name == "@") {
						for (size_t i = 1; i <= count; ++i) {
							std::string_view arg;
							if (!_params.positional_at(i, arg))
								continue;
							if (i > 1)
								finish_field(out);   // one field each
							append(arg);
						}
					} else if (!ctx.split) {
						// One value: `"$*"`, and also `x=$@` and `x="$@"`, which dash
						// joins - the field list `"$@"` would make has nowhere to go, and
						// keeping only the last field is what the old flag did (`x=$@`
						// assigned `c`).
						const std::string_view sep = _params.ifs();
						const char joiner = sep.empty() ? '\0' : sep[0];
						for (size_t i = 1; i <= count; ++i) {
							std::string_view arg;
							if (!_params.positional_at(i, arg))
								continue;
							if (i > 1 && joiner != '\0')
								append(std::string_view(&joiner, 1));
							append(arg);
						}
						_field_started = true;
					} else {
						// Unquoted, both behave alike: split like any other expansion.
						for (size_t i = 1; i <= count; ++i) {
							std::string_view arg;
							if (!_params.positional_at(i, arg))
								continue;
							if (i > 1)
								finish_field(out);
							append_split(arg, out);
						}
					}
					break;
				}

				std::string_view value;
				bool found = lookup_parameter(p.name, value);

				if (p.op == param_op::length) {
					// ${#x} is the length in bytes; ${#@} and ${#*} the count.
					size_t n = 0;
					if (p.name == "@" || p.name == "*") {
						n = _params.positional_count();
					} else if (found) {
						n = value.size();
					} else if (report_unset(p.name)) {
						// `set -u` covers ${#x} as much as $x: dash exits 2 for
						// `dash -u -c 'echo "${#x}"'`, and option-p.tst's
						// 'nounset on: unset variable ${#foo}' requires it.
						status = expansion_status::parameter_unset;
						break;
					}
					char digits[24];
					const int written = std::snprintf(digits, sizeof(digits), "%zu", n);
					append(std::string_view(digits, written > 0 ? static_cast<size_t>(written) : 0));
					break;
				}

				// The colon variants treat an EMPTY value as unset. Without the
				// colon only a genuinely unset parameter qualifies - a real
				// difference that `${x:-d}` versus `${x-d}` turns on.
				const bool absent = !found || (p.colon && value.empty());

				switch (p.op) {
					// The argument of `-` and `+` is expanded IN PLACE, in the context
					// of the expansion itself, rather than expanded to a value that is
					// then appended. POSIX 2.6.2 makes the argument part of the word, so
					// its backslashes and quotes read the way the surrounding text does
					// and field splitting sees its STRUCTURE: `${a+\ x y}` is two fields
					// `[ x]` and `[y]`, because the escaped blank is not a separator and
					// the bare one is. Expanding to a value first flattened both into
					// bytes and then split on the bytes, which is quote-p.tst's eleven
					// 'backslashes/quotes in substitution of expansion' cases.
					case param_op::use_default:
						if (absent) {
							expand_context arg = ctx;
							arg.substituted = true;
							arg.in_braces = true;
							const expansion_status inner = expand_text(p.argument, arg, out);
							if (inner != expansion_status::ok)
								status = inner;
							break;
						}
						goto substitute_value;

					case param_op::assign_default:
						if (absent && !is_assignable_name(p.name)) {
							// POSIX: only a VARIABLE can be assigned to. `${1:=x}` and
							// `${*:=x}` reported nothing and quietly substituted the
							// default - a stub that succeeded, which param-p.tst:198 and
							// :202 both assert against. dash says `1: bad variable name`
							// and exits 2.
							//
							// Checked only when the assignment would actually HAPPEN,
							// which is dash's rule and not an approximation of it:
							// `set a; echo ${1=x}` substitutes `a` at status zero, and
							// `${#=y}` substitutes the count, because neither needs to
							// assign anything.
							std::fprintf(stderr, "lesh: %.*s: bad variable name\n",
							             static_cast<int>(p.name.size()), p.name.data());
							// An expansion error, not an unsupported construct. It was the
							// latter, alongside a genuinely-unbuilt construct and alongside
							// `$((1/0))`, which is how one value came to mean both "fatal" and
							// "harmless" (#39). Nothing read it here - `_fatal_error` carried
							// the decision - but the value said the wrong thing.
							status = expansion_status::expansion_error;
							_fatal_error = true;
							break;
						}
						if (absent) {
							// `=` is the one that needs a value as well as a substitution,
							// and the two are NOT the same string: quote removal happens
							// before the assignment, so the value holds a literal blank
							// where the word held `\ ` - and the substitution then splits
							// the VALUE, leading blank and all. quote-p.tst's 'quotes in
							// substitution of expansion ${a=b}' turns on exactly that:
							// `${a=\ x}` substitutes `[x]` while `${a+\ x}` gives `[ x]`.
							const std::string_view d = expand_to_value(p.argument, brace_ctx(ctx));
							// A REFUSED assignment - `readonly x; : ${x=1}` - is a variable
							// assignment error, which POSIX makes fatal to a non-interactive
							// shell. The assigner has already reported it by name; what it
							// cannot do is stop the command, so the flag is set here.
							if (_assign != nullptr && !_assign->assign_parameter(p.name, d))
								_fatal_error = true;
							if (ctx.split) append_split(d, out); else append(d);
							break;
						}
						goto substitute_value;

					case param_op::use_alternate:
						if (!absent) {
							expand_context arg = ctx;
							arg.substituted = true;
							arg.in_braces = true;
							const expansion_status inner = expand_text(p.argument, arg, out);
							if (inner != expansion_status::ok)
								status = inner;
						}
						break;

					case param_op::error_if_unset:
						if (absent) {
							// The default message is dash's, word for word: `${x?}` says
							// "parameter not set" and `${x:?}` distinguishes the null case.
							const std::string_view fallback =
								p.colon ? std::string_view{"parameter not set or null"}
								        : std::string_view{"parameter not set"};
							const std::string_view message =
								p.argument.empty() ? fallback
								                   : expand_to_value(p.argument, brace_ctx(ctx));
							std::fprintf(stderr, "lesh: %.*s: %.*s\n",
							             static_cast<int>(p.name.size()), p.name.data(),
							             static_cast<int>(message.size()), message.data());
							// Fatal to a non-interactive shell, whatever `set -u` says:
							// `${x?}` is the caller ASKING for the shell to stop. Until
							// this was recorded the message was printed and the command
							// ran anyway, so `echo "${x?}"` printed a blank line and
							// reported success.
							status = expansion_status::parameter_unset;
							_fatal_error = true;
							break;
						}
						goto substitute_value;

					case param_op::trim_prefix_short:
					case param_op::trim_prefix_long:
					case param_op::trim_suffix_short:
					case param_op::trim_suffix_long: {
						if (!found) {
							if (report_unset(p.name))
								status = expansion_status::parameter_unset;
							break;
						}
						// The pattern is expanded but NOT globbed - it is a pattern,
						// not a filename. The matcher is #23's, shared with `case`.
						//
						// `pattern` rather than `double_quoted`, which is what this used
						// to say. Both keep a backslash through expansion, but only
						// `pattern` also keeps the quoting that had no backslash:
						// `${s#'*'}` on `***` must trim ONE asterisk, and reading those
						// quotes as nothing left the matcher a bare `*` that swallowed all
						// three. Not double-quoted, because the outer quotes of
						// `"${a#*1}"` do not quote the pattern - it still wildcards.
						//
						// Lexed as a WORD interior whatever the enclosing mode, because a
						// quote inside `${...}` is a quote even where the surrounding text
						// has none: `cat <<END` holding `${foo%"oo"}` trims `oo`, while a
						// here-document body's own `"` is an ordinary byte (redir-p.tst's
						// 'parameter expansion with unquoted here-document delimiter').
						const std::string_view pat = expand_to_value(
							p.argument, expand_context{.split = false,
							                           .fields = false,
							                           .double_quoted = false,
							                           .pattern = true,
							                           .mode = lex_mode::word_interior});
						std::string_view result = value;
						const bool prefix = p.op == param_op::trim_prefix_short ||
						                    p.op == param_op::trim_prefix_long;
						const bool longest = p.op == param_op::trim_prefix_long ||
						                     p.op == param_op::trim_suffix_long;
						const size_t n = prefix ? match_prefix(pat, value, longest)
						                        : match_suffix(pat, value, longest);
						if (n != no_match && n > 0) {
							result = prefix ? value.substr(n)
							                : value.substr(0, value.size() - n);
						}
						if (ctx.split) append_split(result, out); else append_value(result, ctx);
						break;
					}

					case param_op::none:
					substitute_value:
						if (!found && report_unset(p.name)) {
							status = expansion_status::parameter_unset;
							break;
						}
						if (found) {
							if (ctx.split) {
								if (has_pattern_characters(value))
									_field_globbable = true;
								append_split(value, out);
							} else {
								append_value(value, ctx);
							}
						}
						break;

					default:
						break;
				}
			} break;

			case token_kind::seg_tilde: {
				// The prefix arrives whole and unquoted: the lexer declines to claim a
				// tilde-prefix that contains quoting, so this is `~` or `~name` and
				// nothing else. It is appended rather than split - POSIX 2.6.1 puts
				// tilde expansion before field splitting, but the RESULT is not split
				// (tilde-p.tst's 'result of tilde expansion is not subject to field
				// splitting'), and a `*` in a home directory is not a pattern either.
				if (body == "~") {
					append(_params.home_directory());
					break;
				}
				std::string_view home;
				if (_params.home_directory_of(body.substr(1), home))
					append(home);
				else
					// No such user. dash leaves the word alone at status zero rather
					// than reporting anything, and POSIX leaves it unspecified.
					append(body);
			} break;

			case token_kind::seg_command_sub: {
				if (_runner == nullptr) {
					// Completion's mode: expand everything else, refuse to execute.
					status = expansion_status::command_substitution_unavailable;
					break;
				}
				arena_array<char> captured{_pool, 256};
				// The backquoted form escapes; `$(...)` does not, which is the whole
				// reason POSIX prefers it.
				std::string_view code = substitution_body(without_continuations(body));
				if (!body.empty() && body[0] == '`')
					code = unescape_backquotes(code, ctx.double_quoted);
				switch (_runner->run_and_capture(code, captured)) {
					case substitution_result::malformed:
						// The body is not shell input. Fatal to a non-interactive shell,
						// exactly as an unterminated construct inside an expansion is (#48)
						// and as a syntax error at the top level is - the fault is in the
						// input either way, and the runner has already reported it. This is
						// the whole of #57: the answer was a bool, `false` meant only "the
						// fork failed", and the refusal had nowhere to go.
						_fatal_error = true;
						status = expansion_status::malformed_expansion;
						break;

					case substitution_result::unavailable:
						// No pipe or no process. The shell is out of resources rather than
						// the input being wrong, so this is not the input's fault - but it is
						// still not a substitution that produced nothing, which is what
						// discarding the old `false` made it look like. The runner has
						// reported the errno; a non-interactive shell stops rather than
						// running a command with a word that silently lost its middle.
						_fatal_error = true;
						status = expansion_status::expansion_error;
						break;

					case substitution_result::ok: {
						std::string_view result{captured.data(), captured.size()};
						// POSIX: trailing newlines are removed from the result.
						while (!result.empty() && result.back() == '\n')
							result.remove_suffix(1);
						if (ctx.split) {
							// The result is glob-eligible: cmdsub-p.tst's 'pathname expansion
							// on result of command substitution' requires `$(echo 'dumm*ile')`
							// to become the matching filename. Missing here while the same
							// rule was applied to a variable's value two branches up.
							if (has_pattern_characters(result))
								_field_globbable = true;
							append_split(result, out);
						} else {
							append_value(result, ctx);
						}
					} break;
				}
			} break;

			case token_kind::seg_arithmetic: {
				// The lexer spans `$((...))`; strip the delimiters and evaluate. The
				// delimiters are at fixed offsets only once the line continuations
				// between their characters are gone - `$\<newline>(\<newline>(1+2))`
				// has a five-byte opener.
				std::string_view inner = without_continuations(body);
				if (inner.size() >= 5)
					inner = inner.substr(3, inner.size() - 5);

				// The inner text is expanded first: `$((x + $y))` is legal, and the
				// evaluator sees only arithmetic. Double-quoted rules, because the
				// evaluator wants the bytes the user wrote rather than a shell-quoted
				// version of them.
				const std::string_view resolved = expand_to_value(
					inner, expand_context{.split = false,
					                      .fields = false,
					                      .double_quoted = true,
					                      .mode = lex_mode::word_interior});

				if (_vars == nullptr) {
					// No mutable state - completion's mode. Evaluating would still be
					// safe for reads, but an assignment would mutate the shell as a
					// side effect of drawing a suggestion.
					status = expansion_status::unsupported_construct;
					break;
				}
				const arithmetic_result r = evaluate(resolved, *_vars);
				if (!r.ok) {
					// A refused assignment - `readonly x; echo $((x=1))` - is a variable
					// assignment error rather than a malformed expression, and POSIX makes
					// it fatal to a non-interactive shell. shell_state has already named
					// the variable, the way dash does, so there is nothing to add here.
					if (r.assignment_refused) {
						_fatal_error = true;
						status = expansion_status::expansion_error;
						break;
					}
					// EVERY other refusal is an expansion error too, and this is the line
					// #39 named: it turned all of them into unsupported_construct, which
					// nothing treats as fatal, so `echo $((1/0))` and `echo $((--))`
					// reached the command line as an empty field at status zero. The
					// evaluator had already refused both; the report was thrown away above
					// it.
					//
					// unsupported_construct now means only what it says - a construct lesh
					// has not built - and the one such case here is the branch above, where
					// there is no mutable state to evaluate against.
					report_arithmetic(r.error, resolved);
					status = expansion_status::expansion_error;
					break;
				}
				// POSIX applies `set -u` inside arithmetic too, so `$((x))` on an
				// unset x is an error rather than zero. dash does NOT do this and
				// fails option-p.tst's 'nounset on: unset variable $((foo))';
				// divergence in lesh's favour, recorded rather than copied.
				if (!r.unset_name.empty() && report_unset(r.unset_name)) {
					status = expansion_status::parameter_unset;
					break;
				}
				char digits[24];
				const int n = std::snprintf(digits, sizeof(digits), "%lld",
				                            static_cast<long long>(r.value));
				// Split like any other expansion result. Digits look unsplittable
				// until IFS holds one: fsplit-p.tst's 'field splitting applies to
				// results of expansions' sets `IFS=' 0'` and requires `$((708))` to
				// become two fields.
				const std::string_view text{digits, n > 0 ? static_cast<size_t>(n) : 0};
				if (ctx.split) append_split(text, out); else append_value(text, ctx);
			} break;

			default:
				append(body);
				break;
		}
		if (segment_is_content)
			_saw_other_content = true;
	}
	--_depth;
	return status;
}

// POSIX 2.9.1's declaration utility. The header argues why this is a scan the
// caller drives rather than a role the parser marks.

bool declaration_scan::operand_is_assignment(std::string_view word) const noexcept {
	if (_state != state::declaring)
		return false;
	// The word AS WRITTEN, so an `=` that only appears after an expansion does not
	// count: `a='A=1'; export $a` sets A through field splitting, not through this.
	const size_t eq = word.find('=');
	return eq != std::string_view::npos && is_assignable_name(word.substr(0, eq));
}

void declaration_scan::note_expanded_word(std::string_view field) noexcept {
	// The utility is settled by the first word that names one, and a later operand
	// does not unname it: `export B A=$a` assigns `x y` in dash.
	if (_state != state::searching)
		return;
	if (field == "export" || field == "readonly") {
		_state = state::declaring;
		return;
	}
	// `command` bypasses function lookup; it does not change what the utility it
	// runs IS. declutil-p.tst requires the rule to survive TWO of them, which is
	// the assertion zsh fails and dash passes.
	if (field == "command") {
		_after_command = true;
		return;
	}
	if (_after_command && !field.empty() && field[0] == '-')
		return;  // an option of that `command`, not the utility's name
	_state = state::other;
}

// One VALUE rather than a field list, in the caller's own quoting rules.
std::string_view expander::expand_value(std::string_view text,
                                        value_context context) noexcept {
	expand_context ctx;
	ctx.split = false;   // no field splitting and no pathname expansion, in all three
	ctx.fields = false;  // and so `"$@"` joins rather than making fields nobody reads
	switch (context) {
		case value_context::assignment:
			// Unquoted backslash rules, plus the one lexical difference an
			// assignment has: a tilde after an unquoted colon is eligible, so
			// `PATH=~/bin:~/sbin` expands both.
			ctx.double_quoted = false;
			ctx.mode = lex_mode::assignment_interior;
			break;
		case value_context::redirection_operand:
			// UNQUOTED backslash rules. This is the half the old single flag got
			// wrong: `x=\!` assigned `\!` and `cat <\i'n'"0"` looked for a file
			// called `\in0`, because suppressing field splitting also switched the
			// backslash rules to the double-quoted ones (#42). No after-colon tilde:
			// POSIX confines that to assignments, and dash does not do it here.
			ctx.double_quoted = false;
			ctx.mode = lex_mode::word_interior;
			break;
		case value_context::here_document_body:
			// As if double-quoted, except that `"` is not special - so the quotes in
			// the body survive. Lexed as a word interior, they did not: `it's` in a
			// body came out as `its` and `a"b` as `ab`.
			ctx.double_quoted = true;
			ctx.mode = lex_mode::here_doc_body;
			break;
	}
	return expand_to_value(text, ctx);
}

std::string_view expander::expand_to_value(std::string_view text,
                                           expand_context ctx) noexcept {
	// A value is one string, so field splitting and `"$@"`'s field list are both
	// off however the caller's context reads.
	ctx.split = false;
	ctx.fields = false;

	// RE-ENTRANT. Arithmetic expansion calls this from inside expand_text to
	// resolve its own inner text, so the caller's accumulator must survive.
	// Nulling _current on exit unconditionally clobbered it - UBSan caught the
	// null dereference on the very first `$((1 + 2))`.
	arena_array<char>* const outer_current = _current;
	const bool outer_started = _field_started;
	const bool outer_globbable = _field_globbable;

	arena_array<char> accumulator{_pool, 64};
	arena_array<std::string_view> discard{_pool, 4};
	_current = &accumulator;
	_field_started = false;
	_field_globbable = false;
	const split_run outer_run = _run;
	const bool outer_run_closed = _run_closed_a_field;
	_run = split_run::none;
	_run_closed_a_field = false;

	std::ignore = expand_text(text, ctx, discard);

	const size_t n = accumulator.size();
	char* block = nullptr;
	_pool.allocate(n == 0 ? 1 : n, block, 1);
	if (n > 0)
		std::memcpy(block, accumulator.data(), n);

	_current = outer_current;
	_field_started = outer_started;
	_field_globbable = outer_globbable;
	_run = outer_run;
	_run_closed_a_field = outer_run_closed;
	return {block, n};
}

expansion_status expander::expand_word(const syntax::tree& t, syntax::node_index word,
                                       arena_array<std::string_view>& out) noexcept {
	const syntax::node& n = t[word];
	const std::string_view text = t.text_of(n);
	const token& tok = t.token_at(n.first_token);

	// The fast path the lexer's flag_literal exists for. A word with no quoting,
	// no expansion and no glob characters expands to itself - so the field is a
	// view into the original source and costs no allocation and no copy at all.
	// Most words in most command lines take this path.
	// The fast path flag_literal exists for: no quoting, no expansion, no glob
	// characters, so the field is a view into the source at no cost. The lexer
	// already excludes glob characters from flag_literal, so this cannot skip
	// pathname expansion.
	//
	// A case pattern may take it too: the same absence that makes the word its own
	// field makes it its own pattern, since the bytes flag_literal rules out are
	// exactly the quoting a pattern would have to escape and the metacharacters it
	// would have to keep.
	if ((tok.flags & syntax::flag_literal) != 0) {
		out.push(text);
		return expansion_status::ok;
	}

	// The role the parser recorded, because only the parser can see it: `*)` and
	// the `*` in `echo *` are the same node otherwise.
	//
	// Both `case` roles are ONE result rather than a field list. POSIX subjects
	// both to the expansions and neither to field splitting - so neither is
	// pathname-expanded either, the two applying to exactly the same text - and a
	// second field would have nowhere to go: the executor reads pattern[0] and
	// subject[0]. It follows that a word expanding to nothing is the EMPTY pattern
	// or the empty subject rather than none, where a command argument that expands
	// to nothing is no argument at all.
	//
	// Only the pattern is a PATTERN. A subject is text: `case "$x"` has its quotes
	// removed and nothing escaped, or every backslash a variable holds would start
	// escaping the byte after it.
	const syntax::word_role role = n.kind == syntax::node_kind::word
		? static_cast<syntax::word_role>(n.aux)
		: syntax::word_role::ordinary;
	const bool one_value = role != syntax::word_role::ordinary;
	const expand_context ctx = one_value
		? expand_context{.split = false, .fields = false,
		                 .pattern = role == syntax::word_role::pattern}
		: expand_context{};

	arena_array<char> accumulator{_pool, 64};
	_current = &accumulator;
	_field_started = false;
	// One word, one separator state: a run left over from the previous word would
	// swallow the first byte of this one.
	_run = split_run::none;
	_run_closed_a_field = false;

	// Pathname expansion runs AFTER field splitting, on each resulting field, and
	// only on unquoted text - which is why it happens here rather than inside
	// expand_text, where quoting context is still being tracked.
	const size_t before_fields = out.size();
	_field_globbable = false;
	const expansion_status status = expand_text(text, ctx, out);
	finish_field(out, /*even_if_empty=*/one_value);

	if (_glob_enabled && _field_globbable) {
		arena_array<std::string_view> globbed{_pool, out.size() - before_fields + 4};
		bool any = false;
		for (size_t i = before_fields; i < out.size(); ++i) {
			if (expand_pathnames(_pool, out[i], globbed))
				any = true;
			else
				globbed.push(out[i]);
		}
		if (any) {
			out.truncate(before_fields);
			for (const auto& g : globbed)
				out.push(g);
		}
	}

	_current = nullptr;
	return status;
}

} // namespace lesh::runtime
