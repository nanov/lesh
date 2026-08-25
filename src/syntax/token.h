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
	semi_and,     // ;& - POSIX.1-2024 case fallthrough, distinct from `;` `&`
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

	// --- word interior -------------------------------------------------------
	// Emitted only in lex_mode::word_interior. A word is not one rich token (see
	// issue #9): it is a run of flat segments, which is what keeps the token small
	// and the lexer allocation-free. The expander consumes these rather than
	// re-scanning the word itself, so shell quoting rules live in exactly one
	// place. Two scanners for one grammar is how zsh ended up maintaining a second
	// parser for its line editor.
	seg_literal,        // plain bytes, possibly containing backslash escapes
	seg_single_quoted,  // '...' - no expansion inside, ever
	// $'...' - ANSI-C quoting (POSIX.1-2024). Quoted like '...', but its escapes
	// are DECODED, and the decoded bytes are not in the source text. That is the
	// whole reason it is its own kind rather than a flag on seg_single_quoted: the
	// lexer only delimits it - a backslash escapes the closing quote, which is
	// already an extent `'...'` does not have - and the expander does the decoding
	// as it writes the field, where the bytes have somewhere to live (#75).
	seg_dollar_single_quoted,
	seg_double_quoted,  // "..." - expansion inside, but no field splitting
	seg_parameter,      // $name or ${...}
	seg_command_sub,    // $(...) or `...`
	seg_arithmetic,     // $((...))
	seg_tilde,          // a leading ~ eligible for tilde expansion
};

// Errors are data, not failures. Lexing always produces a token; a malformed one
// says so and carries where. This is what lets the same lexer serve a parser that
// must not throw and a highlighter that runs on every keystroke over half-typed
// input.
//
// ORTHOGONAL to lexer::incomplete(). An error says the token is DEFECTIVE as it
// stands; incomplete says more input could still complete it. Both are true of
// `echo "x`, which an interactive shell continues and a script must reject.
// Neither is a stand-in for the other: a trailing backslash and an unterminated
// here-document are incomplete without being defective, and dash runs both.
enum class token_error : uint8_t {
	none,
	unterminated_single_quote,
	unterminated_double_quote,
	unterminated_backquote,
	// `$(`, `$((` and `${` with the input running out before the closing
	// delimiter. These were once reported as incomplete and NOT as errors, so
	// `lesh -c 'echo $('` printed nothing and reported success (#47), and
	// `lesh -c 'echo $((1'` reached an expander that strips `$((` and `))` from a
	// segment too short to have them and recursed until the stack ran out.
	unterminated_command_sub,
	unterminated_arithmetic,
	unterminated_parameter_expansion,
	unexpected_byte,
};

// What an unterminated construct is CALLED in a diagnostic, or nullptr when the
// error says nothing beyond "syntax error".
//
// Beside the enum rather than in the tree, because TWO layers name the same
// defects on the same constructs: the parser, over the tokens of a defective
// node, and the expander, over one segment of a word it is expanding. The second
// is not optional - `${x-$((1}` is well formed at the command level, because the
// word scan counts braces and the `}` closes the expansion, so the unterminated
// `$((` is only ever seen when the default is expanded (#48). Two copies of these
// phrases would drift, and the user cannot tell which layer caught it.
[[nodiscard]] constexpr const char* error_phrase(token_error error) noexcept {
	switch (error) {
		case token_error::unterminated_single_quote:
		case token_error::unterminated_double_quote:
			return "unterminated quoted string";
		case token_error::unterminated_backquote:
		case token_error::unterminated_command_sub:
			return "unterminated command substitution";
		case token_error::unterminated_arithmetic:
			return "unterminated arithmetic expansion";
		case token_error::unterminated_parameter_expansion:
			return "unterminated parameter expansion";
		case token_error::unexpected_byte:
		case token_error::none:
			break;
	}
	return nullptr;
}

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
