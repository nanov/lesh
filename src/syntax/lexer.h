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
	// Inside an ASSIGNMENT's value. Quoting reads exactly as in word_interior; the
	// one difference is that a tilde after an unquoted colon is eligible for tilde
	// expansion, which POSIX 2.6.1 confines to assignments so `PATH=~/bin:~/sbin`
	// works. A separate mode rather than a flag on the expander, because it is the
	// LEXER that decides where a tilde segment begins.
	assignment_interior,
	// Inside DOUBLE quotes. A single quote is an ordinary character there, and a
	// leading tilde is not special. Lexing the interior in word_interior mode
	// instead made `echo "it's"` print `it`, because `'s"` was taken for the start
	// of a single-quoted segment and its quotes were removed.
	double_quote_interior,
	// The BODY of an unquoted here-document. POSIX 2.7.4 makes it behave as if
	// double-quoted except that `"` is not special either, so BOTH quote
	// characters are ordinary bytes. Lexed as a word interior instead, a body
	// containing `it's` came out as `its` and `a"b` as `ab` - quote removal on text
	// that never had quotes (#42).
	here_doc_body,
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

	// Resumes lexing at a different offset. The parser uses this to skip a
	// here-document body it has already collected: the lexer never reads input
	// itself, so somebody has to tell it where the body ended. That the lexer is
	// restartable at ANY offset (#9) is what makes this a one-liner rather than a
	// second input path.
	constexpr void seek(uint32_t position) noexcept { _position = position; }
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
	[[nodiscard]] constexpr char char_at(uint32_t at) const noexcept {
		return at < _source.size() ? _source[at] : '\0';
	}
	// The first position at or after `at` that does not begin a line continuation.
	//
	// POSIX 2.2.1 removes `\<newline>` BEFORE the input is tokenised, so every
	// lookahead has to look PAST one: `>\<newline>>` is the operator `>>`,
	// `c\<newline>ase` is the reserved word `case`, and `$\<newline>{f}` is a
	// parameter expansion. Treated as removal here rather than by rewriting the
	// input, because the lexer owns no memory (#9) and a rewritten buffer would
	// break every offset the line editor and `node_at` depend on. A TRAILING
	// backslash is left alone: it is incomplete input, not a continuation yet.
	[[nodiscard]] constexpr uint32_t past_continuations(uint32_t at) const noexcept {
		while (at + 1 < _source.size() && _source[at] == '\\' && _source[at + 1] == '\n')
			at += 2;
		return at;
	}

	// Advances past ONE quoted run or expansion beginning at `at`, returning the
	// position just after it - or `at` when nothing starts there.
	//
	// Four scans needed this and all four had the same defect: a loop looking for
	// one delimiter walked straight through a nested construct that could contain
	// it. `"outer $(echo "inner") end"` is ONE quoted string, `"`echo "x"`"` is one
	// too, and `${e=a"b"c}` ends at the brace AFTER the quotes rather than at
	// whichever `"` or `}` came first. Iterative, with an explicit stack of the
	// closers each open construct wants, because the nesting depth is the INPUT's
	// and recursion here would be a stack overflow waiting for a test case.
	// `inside_double_quotes` says whether a single quote at `at` is a QUOTE or an
	// ordinary byte. It is not derivable from the bytes: `"${x-'}"` prints one
	// single quote at status zero in dash, because inside double quotes the `${...}`
	// body inherits the context, while `"$(echo 'x')"` really does quote - a
	// substitution starts the shell language over.
	[[nodiscard]] uint32_t skip_quoted_or_expansion(
		uint32_t at, bool inside_double_quotes = false) const noexcept;
	// How deep the scan will follow nesting. 256 matches the expander's own
	// kMaxExpansionDepth, which is the layer that refuses well-formed input nested
	// deeper than that anyway. PAST it the construct is reported as UNTERMINATED
	// rather than mis-scanned: an unmatched closer would otherwise leave the scan
	// somewhere arbitrary and produce a silently wrong word, and a diagnostic at
	// status 2 is the answer this project gives to input it will not follow.
	static constexpr int kMaxScanNesting = 256;

	bool skip_blanks_and_comments() noexcept;  // returns whether anything was skipped
	token lex_operator() noexcept;
	token lex_word(lex_mode mode) noexcept;
	token lex_word_segment(lex_mode mode) noexcept;
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
