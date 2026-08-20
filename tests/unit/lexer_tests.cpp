#include "syntax/lexer.h"

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

using namespace lesh::syntax;

namespace {

struct Lexed {
	token_kind kind;
	std::string text;
};

std::vector<Lexed> lex_all(std::string_view src) {
	lexer lx{src};
	std::vector<Lexed> out;
	for (;;) {
		const token t = lx.next();
		if (t.kind == token_kind::end)
			break;
		out.push_back({t.kind, std::string(lx.text(t))});
	}
	return out;
}

std::vector<token_kind> kinds_of(std::string_view src) {
	std::vector<token_kind> out;
	for (const auto& l : lex_all(src))
		out.push_back(l.kind);
	return out;
}

} // namespace

TEST(Lexer, EmptyInputIsImmediatelyEnd) {
	lexer lx{""};
	EXPECT_EQ(lx.next().kind, token_kind::end);
	EXPECT_FALSE(lx.incomplete());
}

TEST(Lexer, SimpleCommandSplitsIntoWords) {
	const auto got = lex_all("echo hello world");
	ASSERT_EQ(got.size(), 3u);
	EXPECT_EQ(got[0].text, "echo");
	EXPECT_EQ(got[1].text, "hello");
	EXPECT_EQ(got[2].text, "world");
	for (const auto& l : got)
		EXPECT_EQ(l.kind, token_kind::word);
}

TEST(Lexer, DoesNotModifyItsSource) {
	// The whole point of contract 1: a highlighter runs this on the editor's own
	// buffer, on every keystroke.
	std::string src = "echo 'hi' | cat";
	const std::string before = src;
	lexer lx{src};
	while (lx.next().kind != token_kind::end) {}
	EXPECT_EQ(src, before);
}

TEST(Lexer, IsRestartableFromAnyOffset) {
	const std::string src = "echo hello";
	lexer from_start{src};
	std::ignore = from_start.next();  // consume "echo"
	const token a = from_start.next();

	lexer from_offset{src, 5};
	const token b = from_offset.next();

	EXPECT_EQ(a.offset, b.offset);
	EXPECT_EQ(a.length, b.length);
}

TEST(Lexer, RecognisesMultiCharacterOperatorsGreedily) {
	EXPECT_EQ(kinds_of("a && b"),
	          (std::vector{token_kind::word, token_kind::and_if, token_kind::word}));
	EXPECT_EQ(kinds_of("a || b"),
	          (std::vector{token_kind::word, token_kind::or_if, token_kind::word}));
	EXPECT_EQ(kinds_of("a | b"),
	          (std::vector{token_kind::word, token_kind::pipe, token_kind::word}));
	EXPECT_EQ(kinds_of("a >> b"),
	          (std::vector{token_kind::word, token_kind::dgreat, token_kind::word}));
	EXPECT_EQ(kinds_of("a <<- b"),
	          (std::vector{token_kind::word, token_kind::dless_dash, token_kind::word}));
	EXPECT_EQ(kinds_of("a ;; b"),
	          (std::vector{token_kind::word, token_kind::dsemi, token_kind::word}));
}

TEST(Lexer, OperatorsSplitWordsWithoutBlanks) {
	EXPECT_EQ(kinds_of("a|b"),
	          (std::vector{token_kind::word, token_kind::pipe, token_kind::word}));
}

TEST(Lexer, NewlineIsItsOwnToken) {
	EXPECT_EQ(kinds_of("a\nb"),
	          (std::vector{token_kind::word, token_kind::newline, token_kind::word}));
}

TEST(Lexer, IoNumberOnlyWhenARedirectionFollowsImmediately) {
	// `2>file` redirects fd 2; `2 >file` passes 2 as an argument.
	EXPECT_EQ(kinds_of("2>f"),
	          (std::vector{token_kind::io_number, token_kind::great, token_kind::word}));
	EXPECT_EQ(kinds_of("2 >f"),
	          (std::vector{token_kind::word, token_kind::great, token_kind::word}));
}

TEST(Lexer, QuotedBlanksDoNotSplitWords) {
	const auto got = lex_all("echo 'a b' \"c d\"");
	ASSERT_EQ(got.size(), 3u);
	EXPECT_EQ(got[1].text, "'a b'");
	EXPECT_EQ(got[2].text, "\"c d\"");
}

TEST(Lexer, QuotedOperatorsAreNotOperators) {
	const auto got = lex_all("echo '|' \"&&\"");
	ASSERT_EQ(got.size(), 3u);
	EXPECT_EQ(got[1].kind, token_kind::word);
	EXPECT_EQ(got[2].kind, token_kind::word);
}

TEST(Lexer, BackslashEscapesTheNextByte) {
	const auto got = lex_all("a\\ b");
	ASSERT_EQ(got.size(), 1u) << "an escaped blank must not split the word";
	EXPECT_EQ(got[0].text, "a\\ b");
}

TEST(Lexer, CommentRunsToEndOfLine) {
	EXPECT_EQ(kinds_of("echo # not a word\nnext"),
	          (std::vector{token_kind::word, token_kind::newline, token_kind::word}));
}

TEST(Lexer, HashInsideAWordIsLiteral) {
	const auto got = lex_all("a#b");
	ASSERT_EQ(got.size(), 1u);
	EXPECT_EQ(got[0].text, "a#b");
}

TEST(Lexer, LiteralFlagMarksWordsThatNeedNoExpansion) {
	lexer plain{"hello"};
	EXPECT_TRUE(plain.next().flags & flag_literal);

	for (std::string_view src : {"$x", "a`b`", "a*", "a?", "a[b]", "~x", "'q'", "\"q\"", "a\\b"}) {
		lexer lx{src};
		EXPECT_FALSE(lx.next().flags & flag_literal) << "source: " << src;
	}
}

TEST(Lexer, BlankSeparationIsRecorded) {
	lexer lx{"a b"};
	const token first = lx.next();
	const token second = lx.next();
	EXPECT_FALSE(first.flags & flag_preceded_by_blank);
	EXPECT_TRUE(second.flags & flag_preceded_by_blank);
}

// --- errors are data, and incomplete is not an error -------------------------

TEST(Lexer, UnterminatedSingleQuoteIsIncompleteNotMalformed) {
	lexer lx{"echo 'abc"};
	std::ignore = lx.next();
	const token t = lx.next();
	EXPECT_EQ(t.kind, token_kind::word);
	EXPECT_EQ(t.error, token_error::unterminated_single_quote);
	EXPECT_TRUE(lx.incomplete()) << "an interactive shell answers this with a continuation prompt";
	EXPECT_EQ(t.error_offset, 5u) << "points at the opening quote, not the end of input";
}

TEST(Lexer, UnterminatedDoubleQuoteIsIncompleteNotMalformed) {
	lexer lx{"echo \"abc"};
	std::ignore = lx.next();
	const token t = lx.next();
	EXPECT_EQ(t.error, token_error::unterminated_double_quote);
	EXPECT_TRUE(lx.incomplete());
}

TEST(Lexer, TrailingBackslashIsIncompleteWithoutBeingAnError) {
	lexer lx{"echo a\\"};
	std::ignore = lx.next();
	const token t = lx.next();
	EXPECT_EQ(t.error, token_error::none) << "a line continuation is not malformed";
	EXPECT_TRUE(lx.incomplete());
}

TEST(Lexer, LexingNeverFailsOnHalfTypedInput) {
	// A highlighter sees every prefix of what the user types. None may throw, hang,
	// or fail to terminate.
	const std::string full = "echo \"a $(b) 'c\" | grep -e 'x' > out 2>&1 && done";
	for (size_t n = 0; n <= full.size(); ++n) {
		lexer lx{std::string_view(full).substr(0, n)};
		int guard = 0;
		for (;;) {
			const token t = lx.next();
			if (t.kind == token_kind::end)
				break;
			ASSERT_LT(++guard, 1000) << "did not terminate on prefix of length " << n;
		}
	}
}

TEST(Lexer, TokenStaysSmall) {
	// One token per input byte run; the size is a latency property, not a detail.
	EXPECT_EQ(sizeof(token), 16u);
}

// Inside double quotes a single quote is an ordinary byte, and a leading tilde is
// not eligible for tilde expansion. Lexing the interior in word_interior mode
// instead made `echo "it's"` print `it` - the trailing `'s"` was taken for the
// start of a single-quoted segment and its quotes were removed.
TEST(LexerDoubleQuoteInterior, SingleQuoteIsOrdinary) {
	lexer lx{"it's"};
	const token t = lx.next(lex_mode::double_quote_interior);
	EXPECT_EQ(t.kind, token_kind::seg_literal);
	EXPECT_EQ(t.length, 4u) << "the whole run is literal, quotes and all";
}

TEST(LexerDoubleQuoteInterior, SingleQuoteStartsASegmentInAPlainWord) {
	// The contrast that makes the mode necessary: outside double quotes the same
	// bytes DO begin a quoted segment.
	lexer lx{"'s'"};
	const token t = lx.next(lex_mode::word_interior);
	EXPECT_EQ(t.kind, token_kind::seg_single_quoted);
}

TEST(LexerDoubleQuoteInterior, TildeIsNotSpecial) {
	lexer lx{"~/x"};
	const token t = lx.next(lex_mode::double_quote_interior);
	EXPECT_EQ(t.kind, token_kind::seg_literal);

	lexer plain{"~/x"};
	EXPECT_EQ(plain.next(lex_mode::word_interior).kind, token_kind::seg_tilde)
		<< "outside double quotes a leading tilde is still eligible";
}

TEST(LexerDoubleQuoteInterior, ExpansionsAreStillRecognised) {
	// Only quoting changes. `$`, backtick and backslash keep their meaning.
	lexer lx{"$HOME"};
	EXPECT_EQ(lx.next(lex_mode::double_quote_interior).kind, token_kind::seg_parameter);
	lexer sub{"$(echo hi)"};
	EXPECT_EQ(sub.next(lex_mode::double_quote_interior).kind, token_kind::seg_command_sub);
}
