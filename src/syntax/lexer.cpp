#include "syntax/lexer.h"

#include "substrate/char_utils.h"

namespace lesh::syntax {

namespace {

constexpr bool is_blank(char c) noexcept { return c == ' ' || c == '\t'; }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Bytes whose presence in a word means expansion or quote removal has work to do.
// Their absence is what flag_literal records.
constexpr bool needs_expansion(char c) noexcept {
	switch (c) {
		case '$': case '`': case '\\': case '\'': case '"':
		case '~': case '*': case '?': case '[':
			return true;
		default:
			return false;
	}
}

} // namespace

bool lexer::skip_blanks_and_comments() noexcept {
	const uint32_t start = _position;
	for (;;) {
		while (!at_end() && is_blank(peek()))
			++_position;
		// A line continuation between tokens is nothing at all, so it is skipped
		// here with the blanks. Left in place it began a WORD - `\<newline>{` lexed
		// as one word rather than as the reserved `{` - which is eleven of
		// quote-p.tst's cases (#42).
		if (const uint32_t after = past_continuations(_position); after != _position) {
			_position = after;
			continue;
		}
		// POSIX: '#' opens a comment only where a word could begin. Inside a word
		// it is an ordinary character, which is why this runs before lex_word and
		// never inside it.
		if (!at_end() && peek() == '#') {
			while (!at_end() && peek() != '\n')
				++_position;
			continue;  // a comment may be followed by more blanks
		}
		break;
	}
	return _position != start;
}

uint32_t lexer::skip_quoted_or_expansion(uint32_t at,
                                         bool inside_double_quotes) const noexcept {
	char expect[kMaxScanNesting];
	// Whether a single quote is an ordinary byte at each level. `${...}` INHERITS
	// the context it was opened in, while `$(...)` and a backquote start the shell
	// language over and so reset it - which is why this travels beside the closer
	// rather than being read off the bytes.
	bool ordinary_single[kMaxScanNesting];
	int depth = 0;
	uint32_t p = at;

	bool too_deep = false;
	auto open = [&](char closer, uint32_t width, bool single_is_ordinary) {
		if (depth < kMaxScanNesting) {
			expect[depth] = closer;
			ordinary_single[depth] = single_is_ordinary;
			++depth;
		} else {
			too_deep = true;
		}
		p += width;
	};

	const char first = char_at(p);
	if (first == '\'' && !inside_double_quotes) {
		// Nothing inside single quotes is ever special, so there is no stack to
		// keep: the run ends at the next quote or at the end of the input.
		++p;
		while (p < _source.size() && _source[p] != '\'')
			++p;
		return p < _source.size() ? p + 1 : p;
	}
	if (first == '"')
		open('"', 1, /*single_is_ordinary=*/true);
	else if (first == '`')
		open('`', 1, false);
	else if (first == '$' && char_at(p + 1) == '{')
		open('}', 2, inside_double_quotes);
	else if (first == '$' && char_at(p + 1) == '(')
		open(')', 2, false);
	else
		return at;

	while (depth > 0 && p < _source.size()) {
		const char c = _source[p];
		// A backslash consumes the next byte for DELIMITING purposes wherever it
		// appears, which is what makes `\`` not close a backquote and `\"` not close
		// a quoted string. What it MEANS is the expander's business.
		if (c == '\\' && p + 1 < _source.size()) {
			p += 2;
			continue;
		}
		if (c == expect[depth - 1]) {
			--depth;
			++p;
			continue;
		}
		if (c == '\'') {
			// A single quote is a quote everywhere EXCEPT where double quotes are in
			// force - the distinction that made `echo "it's"` print `it` when it was
			// missed (#33), and that keeps `"${x-'}"` from swallowing the rest of the
			// input.
			if (ordinary_single[depth - 1]) {
				++p;
				continue;
			}
			p = skip_quoted_or_expansion(p);
			continue;
		}
		if (c == '"') {
			open('"', 1, true);
			continue;
		}
		if (c == '`') {
			open('`', 1, false);
			continue;
		}
		if (c == '$' && char_at(p + 1) == '{') {
			open('}', 2, ordinary_single[depth - 1]);
			continue;
		}
		if (c == '$' && char_at(p + 1) == '(') {
			open(')', 2, false);
			continue;
		}
		// A bare paren or brace nests the construct it belongs to, which is how
		// `$(a $(b) c)` and `${x-${y}}` were counted before there was a stack.
		if (c == '(' && expect[depth - 1] == ')') {
			open(')', 1, ordinary_single[depth - 1]);
			continue;
		}
		if (c == '{' && expect[depth - 1] == '}') {
			open('}', 1, ordinary_single[depth - 1]);
			continue;
		}
		++p;
	}
	// Nested deeper than the scan will follow: report it as running to the end of
	// the input, which the callers already treat as unterminated. Continuing with a
	// stack that lost its closers would put `p` somewhere arbitrary and hand the
	// expander a word nobody wrote.
	return too_deep ? static_cast<uint32_t>(_source.size()) : p;
}

token lexer::lex_operator() noexcept {
	const uint32_t start = _position;
	// The characters of an operator may be separated by line continuations, which
	// POSIX removes before the input is tokenised: `>\<newline>>` is `>>` and
	// `<\<newline><\<newline>-` is `<<-`. So each character is looked up past
	// them and `emit` takes the END position rather than a length - the token still
	// SPANS the continuation bytes, because nothing reads an operator's text.
	const uint32_t at1 = past_continuations(start + 1);
	const uint32_t at2 = past_continuations(at1 + 1);
	const uint32_t at3 = past_continuations(at2 + 1);
	const char c = char_at(start);
	const char c1 = char_at(at1);
	const char c2 = char_at(at2);

	auto emit = [&](token_kind kind, uint32_t end) {
		_position = end;
		token t;
		t.kind = kind;
		t.offset = start;
		t.length = end - start;
		return t;
	};

	switch (c) {
		case '|': return c1 == '|' ? emit(token_kind::or_if, at2) : emit(token_kind::pipe, at1);
		case '&': return c1 == '&' ? emit(token_kind::and_if, at2) : emit(token_kind::amp, at1);
		case ';': return c1 == ';' ? emit(token_kind::dsemi, at2) : emit(token_kind::semi, at1);
		case '(': return emit(token_kind::lparen, at1);
		case ')': return emit(token_kind::rparen, at1);
		case '<':
			if (c1 == '<' && c2 == '-') return emit(token_kind::dless_dash, at3);
			if (c1 == '<') return emit(token_kind::dless, at2);
			if (c1 == '&') return emit(token_kind::less_and, at2);
			if (c1 == '>') return emit(token_kind::less_great, at2);
			return emit(token_kind::less, at1);
		case '>':
			if (c1 == '>') return emit(token_kind::dgreat, at2);
			if (c1 == '&') return emit(token_kind::great_and, at2);
			if (c1 == '|') return emit(token_kind::clobber, at2);
			return emit(token_kind::great, at1);
		default: {
			// Unreachable for callers that check is_word_terminator first, but a
			// lexer that never fails cannot have an unreachable path that traps.
			token t = emit(token_kind::word, at1);
			t.error = token_error::unexpected_byte;
			t.error_offset = start;
			return t;
		}
	}
}

token lexer::lex_word(lex_mode mode) noexcept {
	const uint32_t start = _position;
	bool literal = true;

	// In a here-document delimiter, quoting still delimits (it decides whether the
	// body is expanded) but nothing else is special.
	const bool operators_terminate = (mode != lex_mode::here_doc_delimiter);

	token t;
	t.kind = token_kind::word;
	t.offset = start;

	auto finish = [&](token_error error = token_error::none, uint32_t error_at = 0) {
		t.length = _position - start;
		t.error = error;
		t.error_offset = (error != token_error::none) ? error_at : 0;
		if (literal)
			t.flags |= flag_literal;
		return t;
	};

	while (!at_end()) {
		const char c = peek();

		if (c == '\'') {
			literal = false;
			const uint32_t quote_at = _position;
			++_position;
			while (!at_end() && peek() != '\'')
				++_position;
			if (at_end()) {
				_incomplete = true;
				return finish(token_error::unterminated_single_quote, quote_at);
			}
			++_position;  // closing quote
			continue;
		}

		if (c == '"') {
			literal = false;
			const uint32_t quote_at = _position;
			++_position;
			while (!at_end() && peek() != '"') {
				// Inside double quotes a backslash escapes only a few bytes, but for
				// delimiting purposes it always consumes the next one.
				if (peek() == '\\' && _position + 1 < _source.size()) {
					_position += 2;
					continue;
				}
				// A substitution inside the quotes may contain quotes of its own, at
				// any depth: `"$(echo "x")"`, `` "`echo "x"`" `` and `"${e=a"b"c}"` are
				// each ONE quoted string, and scanning to the next `"` split all three.
				if (peek() == '`' ||
				    (peek() == '$' && (peek(1) == '(' || peek(1) == '{'))) {
					const uint32_t after =
						skip_quoted_or_expansion(_position, /*inside_double_quotes=*/true);
					if (after > _position) {
						_position = after;
						continue;
					}
				}
				++_position;
			}
			if (at_end()) {
				_incomplete = true;
				return finish(token_error::unterminated_double_quote, quote_at);
			}
			++_position;  // closing quote
			continue;
		}

		if (c == '\\' && peek(1) == '\n') {
			// A line continuation, not an escape: both characters are removed
			// entirely, so `echo one\<newline>two` prints `onetwo`.
			literal = false;
			_position += 2;
			continue;
		}

		if (c == '\\') {
			literal = false;
			if (_position + 1 >= _source.size()) {
				// A trailing backslash is a line continuation waiting for more input,
				// not a malformed token.
				++_position;
				_incomplete = true;
				return finish();
			}
			_position += 2;
			continue;
		}

		// `$(...)` is part of the word, not an operator followed by one. Without
		// this the '(' terminates the word and `echo $(x)` lexes as `$`, `(`, `x`,
		// `)` - which is how the expander first came to receive a bare `$`.
		// Parens are counted so nesting works: $(a $(b) c).
		// `${...}` is part of the word even when it contains blanks, which
		// `${x:?some message}` and `${x:-a default}` both do. Without this the word
		// split at the space and the closing brace leaked into the next word.
		// Past line continuations, for the same reason the segment scan is: `(` is a
		// word TERMINATOR, so `echo $\<newline>(\<newline>(1+2))` ended the word at
		// the paren and parsed as a subshell.
		if (c == '$' && char_at(past_continuations(_position + 1)) == '{') {
			literal = false;
			const uint32_t opened_at = _position;
			_position = past_continuations(_position + 1) + 1;
			int depth = 1;
			while (!at_end() && depth > 0) {
				// A quote inside the braces is a quote, so a `}` inside it does NOT
				// close the expansion: `${e=a"b"c}` ends at the last brace and
				// `${a+\}}` at the second. Counted braces alone stopped at the first.
				if (peek() == '\\' && _position + 1 < _source.size()) {
					_position += 2;
					continue;
				}
				if (peek() == '\'' || peek() == '"' || peek() == '`' ||
				    (peek() == '$' && (peek(1) == '(' || peek(1) == '{'))) {
					// Not inside double quotes: the `"` handler above owns those, so a
					// `${` reaching here is at word level.
					const uint32_t after = skip_quoted_or_expansion(_position);
					if (after > _position) {
						_position = after;
						continue;
					}
				}
				if (peek() == '{') ++depth;
				else if (peek() == '}') --depth;
				++_position;
			}
			if (depth > 0) {
				// Incomplete AND defective: `echo ${x` is a word the shell must refuse,
				// not a word it runs without its expansion. Reported as incomplete only,
				// it reached the executor and printed nothing at status zero (#47).
				_incomplete = true;
				return finish(token_error::unterminated_parameter_expansion, opened_at);
			}
			continue;
		}

		if (c == '$' && char_at(past_continuations(_position + 1)) == '(') {
			literal = false;
			const uint32_t opened_at = _position;
			const uint32_t at_paren = past_continuations(_position + 1);
			// `$((` is arithmetic, and counting parens closes it correctly either way -
			// but the two are worth telling apart in a diagnostic, which is the only
			// reason this is looked at here rather than by counting alone.
			const bool arithmetic = char_at(past_continuations(at_paren + 1)) == '(';
			_position = at_paren + 1;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '(') ++depth;
				else if (peek() == ')') --depth;
				++_position;
			}
			if (depth > 0) {
				_incomplete = true;
				return finish(arithmetic ? token_error::unterminated_arithmetic
				                         : token_error::unterminated_command_sub,
				              opened_at);
			}
			continue;
		}

		if (c == '`') {
			literal = false;
			const uint32_t tick_at = _position;
			++_position;
			while (!at_end() && peek() != '`') {
				if (peek() == '\\' && _position + 1 < _source.size())
					++_position;
				++_position;
			}
			if (at_end()) {
				_incomplete = true;
				return finish(token_error::unterminated_backquote, tick_at);
			}
			++_position;
			continue;
		}

		if (operators_terminate && is_word_terminator(c))
			break;

		if (needs_expansion(c))
			literal = false;
		++_position;
	}

	return finish();
}

// Lexes one segment of a word's interior. The caller has already established
// where the word starts and ends; this decomposes it.
//
// Segments are delimited, not interpreted: seg_parameter spans `${x:-y}` without
// deciding what `:-` means, and seg_command_sub spans `$(...)` without parsing
// its contents. Interpretation belongs to the expander, and the contents of a
// command substitution belong to a fresh parse. Keeping the split here means the
// lexer never needs to know what an expansion *does*.
token lexer::lex_word_segment(lex_mode mode) noexcept {
	const uint32_t start = _position;

	token t;
	t.offset = start;

	auto finish = [&](token_kind kind, token_error error = token_error::none,
	                  uint32_t error_at = 0) {
		t.kind = kind;
		t.length = _position - start;
		t.error = error;
		t.error_offset = (error != token_error::none) ? error_at : 0;
		return t;
	};

	const char c = peek();
	// Which quote characters are quotes here. Inside double quotes a single quote
	// is just a byte; in a here-document body BOTH are, because POSIX 2.7.4 gives
	// the body double-quote semantics minus the `"`. Everything else - $, `, \ -
	// is special in all three.
	const bool quotes_are_bytes = mode == lex_mode::double_quote_interior ||
	                              mode == lex_mode::here_doc_body;
	const bool double_quotes_are_bytes = mode == lex_mode::here_doc_body;

	if (c == '\'' && !quotes_are_bytes) {
		++_position;
		while (!at_end() && peek() != '\'')
			++_position;
		if (at_end()) {
			_incomplete = true;
			return finish(token_kind::seg_single_quoted, token_error::unterminated_single_quote, start);
		}
		++_position;
		return finish(token_kind::seg_single_quoted);
	}

	if (c == '"' && !double_quotes_are_bytes) {
		++_position;
		while (!at_end() && peek() != '"') {
			if (peek() == '\\' && _position + 1 < _source.size()) {
				_position += 2;
				continue;
			}
			// A substitution inside double quotes may itself contain quotes:
			// `"outer $(echo "inner") end"` is ONE quoted string. Scanning to the
			// next `"` split it at the inner quote and left a stray `)`.
			if (peek() == '`' ||
			    (peek() == '$' && (peek(1) == '(' || peek(1) == '{'))) {
				const uint32_t after =
					skip_quoted_or_expansion(_position, /*inside_double_quotes=*/true);
				if (after > _position) {
					_position = after;
					continue;
				}
			}
			++_position;
		}
		if (at_end()) {
			_incomplete = true;
			return finish(token_kind::seg_double_quoted, token_error::unterminated_double_quote, start);
		}
		++_position;
		return finish(token_kind::seg_double_quoted);
	}

	// Where a tilde is eligible: the start of a word, and - in an assignment's
	// value only - after an unquoted colon, so `PATH=~/bin:~/sbin` expands both.
	const bool in_assignment = mode == lex_mode::assignment_interior;
	if (c == '~' && (mode == lex_mode::word_interior || in_assignment) &&
	    (start == 0 || (in_assignment && start > 0 && _source[start - 1] == ':'))) {
		// POSIX 2.6.1: the tilde-prefix runs to the first unquoted `/` - or, in an
		// assignment, `:` - or to the end of the word, and if ANY character in it is
		// quoted then NONE of them is a login name. So the prefix is inspected
		// before the segment is claimed, and one that holds quoting is left to the
		// literal scan, which removes those quotes: dash prints `~root` for
		// `~"root"` and for `~roo\t`, quotes gone and tilde intact. `$` and a
		// backquote disqualify it too - `echo ~$USER` prints `~dimitarnanov`.
		uint32_t look = _position + 1;
		bool quoted = false;
		while (look < _source.size()) {
			const char b = _source[look];
			if (b == '/' || is_blank(b) || (in_assignment && b == ':'))
				break;
			if (b == '\'' || b == '"' || b == '\\' || b == '$' || b == '`') {
				quoted = true;
				break;
			}
			++look;
		}
		if (!quoted) {
			_position = look;
			return finish(token_kind::seg_tilde);
		}
		// Fall through: the `~` is ordinary text and the quotes still come off.
	}

	if (c == '`') {
		++_position;
		while (!at_end() && peek() != '`') {
			if (peek() == '\\' && _position + 1 < _source.size())
				++_position;
			++_position;
		}
		if (at_end()) {
			_incomplete = true;
			return finish(token_kind::seg_command_sub, token_error::unterminated_backquote, start);
		}
		++_position;
		return finish(token_kind::seg_command_sub);
	}

	if (c == '$') {
		// Past the line continuations POSIX removes before tokenising, in BOTH
		// lookaheads: `$\<newline>{f}` is a parameter expansion and
		// `$\<newline>(\<newline>(1+2))` an arithmetic one. Read literally, the `$`
		// was "a lone dollar" and the `(` went on to terminate the word, which is
		// three of quote-p.tst's cases.
		const uint32_t at_next = past_continuations(_position + 1);
		const uint32_t at_next2 = past_continuations(at_next + 1);
		const char next = char_at(at_next);
		// The three expansions below report the same defect the command-mode scan
		// reports, on the same construct. Saying it in only one of the two scans is
		// what let `echo $(` through: the word carried no error, so the tree the
		// executor refused to run was not the tree it was given (#47).
		if (next == '(' && char_at(at_next2) == '(') {
			_position = at_next2 + 1;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '(') ++depth;
				else if (peek() == ')') --depth;
				++_position;
			}
			// The second `)` of `))` may be separated from the first by a line
			// continuation, so it is looked for past them: `$((1)\<newline>)` closes.
			// Without this the paren count ended at the first `)` and the second was
			// left in the word as literal text, which printed a stray `)`.
			const uint32_t at_close = past_continuations(_position);
			if (char_at(at_close) == ')')
				_position = at_close + 1;
			else if (at_end()) {
				_incomplete = true;
				return finish(token_kind::seg_arithmetic,
				              token_error::unterminated_arithmetic, start);
			}
			return finish(token_kind::seg_arithmetic);
		}
		if (next == '(') {
			_position = at_next + 1;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '(') ++depth;
				else if (peek() == ')') --depth;
				++_position;
			}
			if (depth > 0) {
				_incomplete = true;
				return finish(token_kind::seg_command_sub,
				              token_error::unterminated_command_sub, start);
			}
			return finish(token_kind::seg_command_sub);
		}
		if (next == '{') {
			// Braces are COUNTED: `${x:-${y:-z}}` nests, and stopping at the first
			// `}` left the outer brace as literal text. Quoted runs are skipped
			// whole, because a `}` inside quotes closes nothing.
			_position = at_next + 1;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '\\' && _position + 1 < _source.size()) {
					_position += 2;
					continue;
				}
				if (peek() == '\'' || peek() == '"' || peek() == '`' ||
				    (peek() == '$' && (peek(1) == '(' || peek(1) == '{'))) {
					const uint32_t after =
						skip_quoted_or_expansion(_position, quotes_are_bytes);
					if (after > _position) {
						_position = after;
						continue;
					}
				}
				if (peek() == '{') ++depth;
				else if (peek() == '}') --depth;
				++_position;
			}
			if (depth > 0) {
				_incomplete = true;
				return finish(token_kind::seg_parameter,
				              token_error::unterminated_parameter_expansion, start);
			}
			return finish(token_kind::seg_parameter);
		}
		if (lesh::string_utils::is_valid_var_name_first_char(static_cast<unsigned char>(next))) {
			_position = at_next + 1;
			for (;;) {
				const uint32_t at = past_continuations(_position);
				if (at >= _source.size() ||
				    !lesh::string_utils::is_valid_var_name_non_first_char(
				        static_cast<unsigned char>(_source[at])))
					break;
				_position = at + 1;
			}
			return finish(token_kind::seg_parameter);
		}
		// The special parameters. Each is exactly one character and none is a valid
		// variable name, which is why they need their own case rather than a
		// widened name predicate - `$?x` is `$?` followed by a literal `x`.
		if (next == '?' || next == '#' || next == '$' || next == '!' ||
		    next == '@' || next == '*' || next == '-') {
			_position = at_next + 1;
			return finish(token_kind::seg_parameter);
		}
		// A positional parameter: $0 through $9. Multi-digit needs braces
		// (`${10}`), which POSIX requires and which the ${...} path already
		// handles.
		if (next >= '0' && next <= '9') {
			_position = at_next + 1;
			return finish(token_kind::seg_parameter);
		}
		// A lone '$' is an ordinary character.
		++_position;
		return finish(token_kind::seg_literal);
	}

	// A literal run: everything up to the next byte that starts a segment.
	while (!at_end()) {
		const char ch = peek();
		// A tilde after an unquoted colon starts a new segment in an assignment,
		// so the run has to stop before it: `x=a:~` expands the tilde. Guarded on
		// having consumed something, or a `~` the tilde branch above declined would
		// end a zero-length run forever.
		if (in_assignment && ch == '~' && _position > start && _source[_position - 1] == ':')
			break;
		if (ch == '\\' && peek(1) == '\n') {
			_position += 2;  // line continuation
			continue;
		}
		if (ch == '\\' && _position + 1 < _source.size()) {
			_position += 2;
			continue;
		}
		if ((ch == '\'' && !quotes_are_bytes) || (ch == '"' && !double_quotes_are_bytes) ||
		    ch == '$' || ch == '`')
			break;
		++_position;
	}
	return finish(token_kind::seg_literal);
}

token lexer::next(lex_mode mode) noexcept {
	_incomplete = false;

	if (mode == lex_mode::word_interior || mode == lex_mode::assignment_interior ||
	    mode == lex_mode::double_quote_interior || mode == lex_mode::here_doc_body) {
		if (at_end()) {
			token t;
			t.kind = token_kind::end;
			t.offset = _position;
			return t;
		}
		return lex_word_segment(mode);
	}

	const bool skipped = skip_blanks_and_comments();

	if (at_end()) {
		token t;
		t.kind = token_kind::end;
		t.offset = _position;
		return t;
	}

	const char c = peek();

	if (c == '\n') {
		token t;
		t.kind = token_kind::newline;
		t.offset = _position;
		t.length = 1;
		++_position;
		if (skipped)
			t.flags |= flag_preceded_by_blank;
		return t;
	}

	// IO_NUMBER: a digit run is only a file descriptor when a redirection operator
	// follows immediately, with no blank between. `2>file` redirects; `2 >file`
	// passes 2 as an argument. The lexer can see this without the parser's help,
	// which is why it is one of the few things it decides alone.
	if (mode == lex_mode::command && is_digit(c)) {
		// Across line continuations, both between the digits and before the
		// operator: `3\<newline>>\<newline>>redir` is `3>>redir`, and read as a word
		// the `3` became an ARGUMENT and the redirection landed on stdout - which is
		// how quote-p.tst's operator case came to report `3: not open for output`.
		uint32_t ahead = past_continuations(_position);
		while (ahead < _source.size() && is_digit(_source[ahead]))
			ahead = past_continuations(ahead + 1);
		if (ahead < _source.size() && (_source[ahead] == '<' || _source[ahead] == '>')) {
			token t;
			t.kind = token_kind::io_number;
			t.offset = _position;
			t.length = ahead - _position;
			t.flags |= flag_literal;
			if (skipped)
				t.flags |= flag_preceded_by_blank;
			_position = ahead;
			return t;
		}
	}

	token t = (mode == lex_mode::command && is_word_terminator(c))
	          ? lex_operator()
	          : lex_word(mode);
	if (skipped)
		t.flags |= flag_preceded_by_blank;
	return t;
}

} // namespace lesh::syntax
