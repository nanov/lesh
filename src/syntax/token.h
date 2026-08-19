#pragma once

#include <cstdint>

namespace lesh::syntax {

// What the lexer emits. See issue #9.
//
// POSIX distinguishes tokens that are operators from tokens that are words.
// Reserved words (`if`, `while`, ...) are deliberately absent: POSIX recognises
// them by grammatical position, not lexically, so `if` is a keyword in command
// position and an ordinary word as an argument. That decision belongs to the
// parser, which is why the lexer takes a mode rather than deciding for itself.
enum class token_kind : uint16_t {
	end,          // end of input
	newline,      // significant in the shell grammar, unlike other blanks
	word,         // anything that is not an operator
	io_number,    // a digit run immediately before < or >
	// control operators
	pipe,         // |
	and_if,       // &&
	or_if,        // ||
	semi,         // ;
	dsemi,        // ;;
	amp,          // &
	lparen,       // (
	rparen,       // )
	// redirection operators
	less,         // <
	great,        // >
	dless,        // <<
	dgreat,       // >>
	dless_dash,   // <<-
	less_and,     // <&
	great_and,    // >&
	less_great,   // <>
	clobber,      // >|
};

// Errors are data, not failures. Lexing always produces a token; a malformed one
// says so and carries where. This is what lets the same lexer serve a parser that
// must not throw and a highlighter that runs on every keystroke over half-typed
// input.
enum class token_error : uint8_t {
	none,
	unterminated_single_quote,
	unterminated_double_quote,
	unterminated_backquote,
	unexpected_byte,
};

// Hints the lexer can supply for free, because it already looked at every byte.
enum token_flags : uint8_t {
	flag_none = 0,
	// The word contains none of $ ` ~ \ ' " * ? [ - so expansion and quote removal
	// are provably no-ops and the expander can hand the bytes through untouched.
	// Recording it here costs nothing and saves the expander a second scan of
	// every literal word, which is most of them.
	flag_literal = 1 << 0,
	// A blank separated this token from the previous one. The parser needs this to
	// distinguish `a b` from `ab` after the lexer has already split them.
	flag_preceded_by_blank = 1 << 1,
};

// 16 bytes, trivially copyable, owns nothing. Offsets rather than pointers so a
// token stays valid across buffer growth and can be serialised or compared.
struct token {
	uint32_t offset = 0;        // start, as a byte offset into the source
	uint32_t length = 0;        // extent in bytes
	uint32_t error_offset = 0;  // where the problem is, when error != none
	token_kind kind = token_kind::end;
	token_error error = token_error::none;
	uint8_t flags = flag_none;

	[[nodiscard]] constexpr bool is_error() const { return error != token_error::none; }
	[[nodiscard]] constexpr bool is_operator() const { return kind >= token_kind::pipe; }
	[[nodiscard]] constexpr uint32_t end_offset() const { return offset + length; }
};

static_assert(sizeof(token) == 16, "token must stay small: one is produced per input byte run");
static_assert(alignof(token) == 4);

} // namespace lesh::syntax
