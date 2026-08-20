#include "runtime/pattern.h"

#include <gtest/gtest.h>

using namespace lesh::runtime;

// One matcher, three callers (#23): globbing, `case`, and ${x#pat}. These tests
// pin the shared behaviour so all three inherit it.

TEST(Pattern, LiteralTextMatchesItself) {
	EXPECT_TRUE(pattern_match("abc", "abc"));
	EXPECT_FALSE(pattern_match("abc", "abd"));
}

TEST(Pattern, MatchIsAnchoredAtBothEnds) {
	// Shell patterns are not regular expressions: `case abc in b)` does NOT match.
	EXPECT_FALSE(pattern_match("b", "abc"));
	EXPECT_FALSE(pattern_match("ab", "abc"));
	EXPECT_FALSE(pattern_match("bc", "abc"));
}

TEST(Pattern, StarMatchesAnythingIncludingEmpty) {
	EXPECT_TRUE(pattern_match("*", ""));
	EXPECT_TRUE(pattern_match("*", "anything"));
	EXPECT_TRUE(pattern_match("a*", "a"));
	EXPECT_TRUE(pattern_match("a*c", "abc"));
	EXPECT_TRUE(pattern_match("a*c", "abbbbc"));
	EXPECT_FALSE(pattern_match("a*c", "abd"));
}

TEST(Pattern, QuestionMatchesExactlyOne) {
	EXPECT_TRUE(pattern_match("a?c", "abc"));
	EXPECT_FALSE(pattern_match("a?c", "ac"));
	EXPECT_FALSE(pattern_match("a?c", "abbc"));
	EXPECT_FALSE(pattern_match("?", ""));
}

TEST(Pattern, BracketMatchesAnyMember) {
	EXPECT_TRUE(pattern_match("[abc]", "b"));
	EXPECT_FALSE(pattern_match("[abc]", "d"));
	EXPECT_TRUE(pattern_match("x[abc]z", "xbz"));
}

TEST(Pattern, BracketRanges) {
	EXPECT_TRUE(pattern_match("[a-z]", "m"));
	EXPECT_FALSE(pattern_match("[a-z]", "M"));
	EXPECT_TRUE(pattern_match("[0-9]", "5"));
	EXPECT_TRUE(pattern_match("[a-cx-z]", "y"));
}

TEST(Pattern, NegatedBracket) {
	EXPECT_TRUE(pattern_match("[!abc]", "d"));
	EXPECT_FALSE(pattern_match("[!abc]", "a"));
	EXPECT_TRUE(pattern_match("[^abc]", "d")) << "^ is accepted as well as !";
}

TEST(Pattern, UnterminatedBracketIsALiteralBracket) {
	// POSIX: a '[' that opens nothing is an ordinary character.
	EXPECT_TRUE(pattern_match("[abc", "[abc"));
}

TEST(Pattern, BackslashEscapesAMetacharacter) {
	EXPECT_TRUE(pattern_match("a\\*b", "a*b"));
	EXPECT_FALSE(pattern_match("a\\*b", "axxb"));
}

TEST(Pattern, MultipleStarsDoNotExplode) {
	// Recursion on '*' is O(2^n) on this input and can blow the stack. The matcher
	// backtracks iteratively instead, so this must return promptly.
	EXPECT_FALSE(pattern_match("*a*a*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	EXPECT_TRUE(pattern_match("*a*a*a*b", "aaaaaaaaaaaaaaab"));
}

TEST(Pattern, LeadingPeriodMustBeMatchedExplicitly) {
	// The rule for filenames: `*` does not match `.hidden`.
	EXPECT_FALSE(pattern_match("*", ".hidden", /*period_is_special=*/true));
	EXPECT_TRUE(pattern_match(".*", ".hidden", true));
	EXPECT_TRUE(pattern_match("*", "visible", true));
	// ...and it does NOT apply to `case` or parameter trimming.
	EXPECT_TRUE(pattern_match("*", ".hidden", /*period_is_special=*/false));
}

TEST(Pattern, MetacharacterDetectionSkipsEscaped) {
	EXPECT_TRUE(has_pattern_characters("a*b"));
	EXPECT_TRUE(has_pattern_characters("a?b"));
	EXPECT_TRUE(has_pattern_characters("a[bc]d"));
	EXPECT_FALSE(has_pattern_characters("plain"));
	EXPECT_FALSE(has_pattern_characters("a\\*b")) << "an escaped star is literal";
}

// --- the ${x#pat} family -----------------------------------------------------

TEST(Pattern, PrefixMatchShortestAndLongest) {
	// ${x#*/} versus ${x##*/} on "/a/b/c"
	EXPECT_EQ(match_prefix("*/", "/a/b/c", /*longest=*/false), 1u);
	EXPECT_EQ(match_prefix("*/", "/a/b/c", /*longest=*/true), 5u);
}

TEST(Pattern, SuffixMatchShortestAndLongest) {
	// ${x%.*} versus ${x%%.*} on "a.b.c"
	EXPECT_EQ(match_suffix(".*", "a.b.c", /*longest=*/false), 2u);
	EXPECT_EQ(match_suffix(".*", "a.b.c", /*longest=*/true), 4u);
}

TEST(Pattern, NoMatchIsDistinctFromAnEmptyMatch) {
	// Zero is a legitimate result - `*` matches the empty string - so "no match"
	// needs its own value rather than being conflated with length zero.
	EXPECT_EQ(match_prefix("x", "abc", false), no_match);
	EXPECT_EQ(match_prefix("*", "abc", false), 0u);
}
