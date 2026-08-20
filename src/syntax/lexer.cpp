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

token lexer::lex_operator() noexcept {
	const uint32_t start = _position;
	const char c = peek();
	const char c1 = peek(1);
	const char c2 = peek(2);

	auto emit = [&](token_kind kind, uint32_t length) {
		_position += length;
		token t;
		t.kind = kind;
		t.offset = start;
		t.length = length;
		return t;
	};

	switch (c) {
		case '|': return c1 == '|' ? emit(token_kind::or_if, 2) : emit(token_kind::pipe, 1);
		case '&': return c1 == '&' ? emit(token_kind::and_if, 2) : emit(token_kind::amp, 1);
		case ';': return c1 == ';' ? emit(token_kind::dsemi, 2) : emit(token_kind::semi, 1);
		case '(': return emit(token_kind::lparen, 1);
		case ')': return emit(token_kind::rparen, 1);
		case '<':
			if (c1 == '<' && c2 == '-') return emit(token_kind::dless_dash, 3);
			if (c1 == '<') return emit(token_kind::dless, 2);
			if (c1 == '&') return emit(token_kind::less_and, 2);
			if (c1 == '>') return emit(token_kind::less_great, 2);
			return emit(token_kind::less, 1);
		case '>':
			if (c1 == '>') return emit(token_kind::dgreat, 2);
			if (c1 == '&') return emit(token_kind::great_and, 2);
			if (c1 == '|') return emit(token_kind::clobber, 2);
			return emit(token_kind::great, 1);
		default: {
			// Unreachable for callers that check is_word_terminator first, but a
			// lexer that never fails cannot have an unreachable path that traps.
			token t = emit(token_kind::word, 1);
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
				if (peek() == '\\' && _position + 1 < _source.size())
					++_position;
				++_position;
			}
			if (at_end()) {
				_incomplete = true;
				return finish(token_error::unterminated_double_quote, quote_at);
			}
			++_position;  // closing quote
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
		if (c == '$' && peek(1) == '{') {
			literal = false;
			_position += 2;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '{') ++depth;
				else if (peek() == '}') --depth;
				++_position;
			}
			if (depth > 0) {
				_incomplete = true;
				return finish();
			}
			continue;
		}

		if (c == '$' && peek(1) == '(') {
			literal = false;
			_position += 2;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '(') ++depth;
				else if (peek() == ')') --depth;
				++_position;
			}
			if (depth > 0) {
				_incomplete = true;
				return finish();
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
token lexer::lex_word_segment() noexcept {
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

	if (c == '\'') {
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

	if (c == '"') {
		++_position;
		while (!at_end() && peek() != '"') {
			if (peek() == '\\' && _position + 1 < _source.size())
				++_position;
			++_position;
		}
		if (at_end()) {
			_incomplete = true;
			return finish(token_kind::seg_double_quoted, token_error::unterminated_double_quote, start);
		}
		++_position;
		return finish(token_kind::seg_double_quoted);
	}

	if (c == '~' && start == 0) {
		// Only a leading tilde is eligible. POSIX confines tilde expansion to the
		// start of a word (and after ':' in assignments, which is #12's problem).
		++_position;
		while (!at_end() && peek() != '/' && !is_blank(peek()))
			++_position;
		return finish(token_kind::seg_tilde);
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
		const char next = peek(1);
		if (next == '(' && peek(2) == '(') {
			_position += 3;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '(') ++depth;
				else if (peek() == ')') --depth;
				++_position;
			}
			if (!at_end() && peek() == ')')
				++_position;
			else if (at_end())
				_incomplete = true;
			return finish(token_kind::seg_arithmetic);
		}
		if (next == '(') {
			_position += 2;
			int depth = 1;
			while (!at_end() && depth > 0) {
				if (peek() == '(') ++depth;
				else if (peek() == ')') --depth;
				++_position;
			}
			if (depth > 0)
				_incomplete = true;
			return finish(token_kind::seg_command_sub);
		}
		if (next == '{') {
			_position += 2;
			while (!at_end() && peek() != '}')
				++_position;
			if (at_end())
				_incomplete = true;
			else
				++_position;
			return finish(token_kind::seg_parameter);
		}
		if (lesh::string_utils::is_valid_var_name_first_char(static_cast<unsigned char>(next))) {
			_position += 2;
			while (!at_end() &&
			       lesh::string_utils::is_valid_var_name_non_first_char(
			           static_cast<unsigned char>(peek())))
				++_position;
			return finish(token_kind::seg_parameter);
		}
		// The special parameters. Each is exactly one character and none is a valid
		// variable name, which is why they need their own case rather than a
		// widened name predicate - `$?x` is `$?` followed by a literal `x`.
		if (next == '?' || next == '#' || next == '$' || next == '!' ||
		    next == '@' || next == '*' || next == '-') {
			_position += 2;
			return finish(token_kind::seg_parameter);
		}
		// A positional parameter: $0 through $9. Multi-digit needs braces
		// (`${10}`), which POSIX requires and which the ${...} path already
		// handles.
		if (next >= '0' && next <= '9') {
			_position += 2;
			return finish(token_kind::seg_parameter);
		}
		// A lone '$' is an ordinary character.
		++_position;
		return finish(token_kind::seg_literal);
	}

	// A literal run: everything up to the next byte that starts a segment.
	while (!at_end()) {
		const char ch = peek();
		if (ch == '\\' && _position + 1 < _source.size()) {
			_position += 2;
			continue;
		}
		if (ch == '\'' || ch == '"' || ch == '$' || ch == '`')
			break;
		++_position;
	}
	return finish(token_kind::seg_literal);
}

token lexer::next(lex_mode mode) noexcept {
	_incomplete = false;

	if (mode == lex_mode::word_interior) {
		if (at_end()) {
			token t;
			t.kind = token_kind::end;
			t.offset = _position;
			return t;
		}
		return lex_word_segment();
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
		uint32_t ahead = _position;
		while (ahead < _source.size() && is_digit(_source[ahead]))
			++ahead;
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
