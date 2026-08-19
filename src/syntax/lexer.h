#pragma once

#include "syntax/token.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lesh::syntax {

// The lexer's context, supplied by the caller on every call.
//
// Shell lexing is not context-free and nobody has made it so: the same bytes
// tokenise differently in command position, inside a word, in a here-document
// delimiter, or in an arithmetic expression. Every shell surveyed for issue #14
// solves this the same way - dash's `checkkwd`, mksh's mode flags, zsh's mode
// globals, Oils' 24 lex modes - by letting the parser feed context back to the
// lexer. The difference here is that the channel is a parameter rather than
// mutable state, so the lexer stays a pure function of (source, position, mode).
enum class lex_mode : uint8_t {
	command,             // operators are operators, words are words
	word_interior,       // inside a word: quoting and expansion boundaries
	here_doc_delimiter,  // the word after << : quoting matters, expansion does not
};

// A cursor over bytes it does not own and never modifies.
//
// Owning nothing is what lets the same lexer serve the parser and a highlighter
// that runs on every keystroke over a buffer the editor owns. Never modifying is
// what the legacy parser could not offer: it null-terminated the input in place
// as it consumed it, which is why highlighting and completion were impossible.
//
// Restartable by construction: build one at any offset and lex from there.
class lexer {
public:
	constexpr lexer(std::string_view source, uint32_t position = 0) noexcept
		: _source(source), _position(position) {}

	// Produces the next token. Never fails: a malformed construct yields a token
	// that says so and carries where.
	[[nodiscard]] token next(lex_mode mode = lex_mode::command) noexcept;

	// True when the last token ended because the input ran out mid-construct, as
	// opposed to being malformed. Deliberately separate from token::error: an
	// interactive shell answers "incomplete" with a continuation prompt and
	// "malformed" with a diagnostic, and it cannot tell them apart if they share
	// one channel. yash demonstrates the cost of conflating them - because its
	// parser pulls input and blocks, it can never report "need more".
	[[nodiscard]] constexpr bool incomplete() const noexcept { return _incomplete; }

	[[nodiscard]] constexpr uint32_t position() const noexcept { return _position; }
	[[nodiscard]] constexpr std::string_view source() const noexcept { return _source; }

	// The bytes a token spans. Borrowed from the source, never copied.
	[[nodiscard]] constexpr std::string_view text(const token& t) const noexcept {
		return _source.substr(t.offset, t.length);
	}

private:
	std::string_view _source;
	uint32_t _position = 0;
	bool _incomplete = false;

	[[nodiscard]] constexpr bool at_end() const noexcept { return _position >= _source.size(); }
	[[nodiscard]] constexpr char peek(size_t ahead = 0) const noexcept {
		const size_t at = _position + ahead;
		return at < _source.size() ? _source[at] : '\0';
	}

	bool skip_blanks_and_comments() noexcept;  // returns whether anything was skipped
	token lex_operator() noexcept;
	token lex_word(lex_mode mode) noexcept;
	token lex_word_segment() noexcept;
};

// True for bytes that end an unquoted word in command position.
[[nodiscard]] constexpr bool is_word_terminator(char c) noexcept {
	switch (c) {
		case '\0': case ' ': case '\t': case '\n':
		case '|': case '&': case ';': case '(': case ')': case '<': case '>':
			return true;
		default:
			return false;
	}
}

} // namespace lesh::syntax
