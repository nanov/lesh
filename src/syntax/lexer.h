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
	// `terminated`, when given, reports whether the construct actually closed - the
	// caller needs it to say `unterminated command substitution` rather than
	// guessing from the returned position.
	[[nodiscard]] uint32_t skip_quoted_or_expansion(
		uint32_t at, bool inside_double_quotes = false,
		bool* terminated = nullptr) const noexcept;
	// Advances past a `$'...'` beginning at `at`, returning the position just after
	// the closing quote - or the end of the input when it never closes, which
	// `terminated` reports.
	//
	// `at` must be the `$`. The caller has already established that a `'` follows
	// it and that a single quote is a QUOTE in this context; inside double quotes
	// and in a here-document body it is an ordinary byte, so there is no construct
	// there to step over at all.
	//
	// Its own helper because THREE scans need the same extent and none of them can
	// borrow the single-quote one: in `'...'` a backslash is an ordinary byte, so a
	// scan for the next `'` stops at the one in `\'`. The word scan would then end
	// `$'a\'b c'` at `b` and split the word; the brace counter would let a `}`
	// inside escape; the segment scan would emit a short token. One escape rule in
	// one place is what keeps those three agreeing.
	[[nodiscard]] uint32_t skip_dollar_single_quote(uint32_t at,
	                                                bool* terminated = nullptr) const noexcept;
	// True where a word could begin, which is where a `#` opens a comment.
	[[nodiscard]] bool starts_a_word(uint32_t p, uint32_t begin) const noexcept;
	// True where a COMMAND could begin, which is where `case` and `esac` are
	// reserved words rather than ordinary ones. Narrower than starts_a_word: a
	// BLANK separates two words and does not end a command, so `echo case` is an
	// argument spelled `case` and not a clause the scan has to follow (#68).
	[[nodiscard]] bool starts_a_command(uint32_t p, uint32_t begin) const noexcept;
	// How deep the scan will follow nesting. 256 matches the expander's own
	// kMaxExpansionDepth, which is the layer that refuses well-formed input nested
	// deeper than that anyway. PAST it the construct is reported as UNTERMINATED
	// rather than mis-scanned: an unmatched closer would otherwise leave the scan
	// somewhere arbitrary and produce a silently wrong word, and a diagnostic at
	// status 2 is the answer this project gives to input it will not follow.
	static constexpr int kMaxScanNesting = 256;

	// Blanks and line continuations only. Comments are TOKENS now (#103), so
	// they are lexed, not skipped.
	bool skip_blanks() noexcept;  // returns whether anything was skipped
	token lex_operator() noexcept;
	token lex_word(lex_mode mode) noexcept;
	token lex_word_segment(lex_mode mode) noexcept;
};

// Does `line` equal the here-document delimiter spelled `raw`, once quote removal
// is applied to `raw`?
//
// POSIX applies quote removal to the delimiter word, so `<<\END`, `<<'END'`,
// `<<"END"` and `<<E'ND'` all end the body at a line reading `END`. Only the fully
// quoted forms were handled before, which meant `<<\END` - the spelling the yash
// conformance suite uses in every one of its ~1,700 here-documents - never matched
// its terminator, so the rest of the file silently became the body. Twenty signal
// files scored zero for that reason alone.
//
// This compares rather than unquoting into a buffer, because neither caller has
// any business allocating for a comparison it makes once per line.
//
// It lives HERE, beside the lexer, because TWO layers ask it and a second copy
// would drift. The parser asks to collect a body it will execute (#21); the
// SCAN asks to step over one while it looks for a command substitution's closing
// paren (#68). The two must agree on where a body ends or the parser would be
// handed text the scan already ruled out.
[[nodiscard]] bool here_doc_delimiter_matches(std::string_view raw,
                                              std::string_view line) noexcept;

// ONE STEP of `$'...'` decoding: what the bytes at `at` mean, and how many of
// them the step used.
//
// Returned rather than written, because the two callers have nowhere in common to
// write TO. The expander appends into the arena buffer it is already building the
// field in - which is the answer to where the decoded bytes live, since they are
// not in the source and the lexer owns no memory (#75) - while
// here_doc_delimiter_matches compares against a line and needs no buffer at all.
// A shared decoder that allocated would force a buffer on the comparison, and two
// copies of the escape table would drift the moment one grew an escape.
//
// At most TWO bytes out, because the widest thing a step yields is an
// UNRECOGNISED escape, which keeps both of its bytes: `\q` is `\q` as in bash,
// where zsh would give `q`. That is also what an incomplete `\x` does.
struct ansi_c_step {
	uint8_t bytes[2] = {0, 0};
	uint8_t count = 0;      // how many of `bytes` are output
	uint8_t consumed = 0;   // source bytes the step used; never 0, so a loop ends
	// The decoded byte is NUL, so the STRING ENDS HERE and `count` is 0. bash
	// truncates at a NUL and zsh embeds one; lesh follows bash, because a field is
	// a view into an arena and an embedded NUL would survive inside the shell only
	// to be truncated by execve on the way to an external command - the same word
	// meaning one thing to a builtin and another to /bin/cat. Truncating at decode
	// is the honest version of a limit that exists either way.
	bool truncates = false;
};

// `body` is the text BETWEEN the quotes, `at` an index into it. Never returns a
// zero `consumed`.
[[nodiscard]] ansi_c_step decode_ansi_c_escape(std::string_view body, size_t at) noexcept;

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
