#include "runtime/pattern.h"

#include "runtime/glob.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

TEST(Pattern, CharacterClasses) {
	// POSIX's twelve, in the C locale - the only locale lesh has.
	EXPECT_TRUE(pattern_match("[[:lower:]]", "a"));
	EXPECT_FALSE(pattern_match("[[:upper:]]", "a"));
	EXPECT_TRUE(pattern_match("[[:alpha:]]", "a"));
	EXPECT_FALSE(pattern_match("[[:digit:]]", "a"));
	EXPECT_TRUE(pattern_match("[[:alnum:]]", "a"));
	EXPECT_FALSE(pattern_match("[[:punct:]]", "a"));
	EXPECT_TRUE(pattern_match("[[:punct:]]", "!"));
	EXPECT_TRUE(pattern_match("[[:graph:]]", "a"));
	EXPECT_FALSE(pattern_match("[[:graph:]]", " "));
	EXPECT_TRUE(pattern_match("[[:print:]]", " "));
	EXPECT_FALSE(pattern_match("[[:cntrl:]]", "a"));
	EXPECT_TRUE(pattern_match("[[:cntrl:]]", "\t"));
	EXPECT_TRUE(pattern_match("[[:blank:]]", "\t"));
	EXPECT_FALSE(pattern_match("[[:blank:]]", "\n"));
	EXPECT_TRUE(pattern_match("[[:space:]]", "\n"));
	EXPECT_TRUE(pattern_match("[[:xdigit:]]", "f"));
	EXPECT_FALSE(pattern_match("[[:xdigit:]]", "g"));
}

TEST(Pattern, AClassIsOneMemberOfASetLikeAnyOther) {
	EXPECT_TRUE(pattern_match("[a[:digit:]]", "a"));
	EXPECT_TRUE(pattern_match("[a[:digit:]]", "7"));
	EXPECT_FALSE(pattern_match("[a[:digit:]]", "b"));
	EXPECT_TRUE(pattern_match("[![:digit:]]", "a")) << "and negation still applies";
	EXPECT_FALSE(pattern_match("[![:digit:]]", "7"));
}

TEST(Pattern, AnUnknownClassNameMatchesNothing) {
	// POSIX leaves it undefined; dash matches nothing, including the bytes the
	// construct is spelled with.
	EXPECT_FALSE(pattern_match("[[:nosuch:]]", "a"));
	EXPECT_FALSE(pattern_match("[[:nosuch:]]", "["));
	EXPECT_FALSE(pattern_match("[[:nosuch:]]", ":"));
}

TEST(Pattern, AnUnterminatedClassLeavesTheOpeningBracketOrdinary) {
	// POSIX leaves a `[:` that never closes undefined. The answer here is the one
	// POSIX already gives a '[' that opens nothing - it is an ordinary character -
	// rather than a second rule, and it agrees with dash on the two spellings
	// anyone could write: neither the class's own bytes nor the '[' matches.
	EXPECT_FALSE(pattern_match("[[:]", ":"));
	EXPECT_FALSE(pattern_match("[[:]", "["));
	EXPECT_FALSE(pattern_match("[[:x]", ":"));
	// The `[` having opened nothing, what follows is read again from there - and
	// `[:]` is a perfectly good bracket expression holding a colon.
	EXPECT_TRUE(pattern_match("[[:]", "[:"));
}

TEST(Pattern, CollatingSymbolsAndEquivalenceClasses) {
	// In the C locale each names exactly one character, which is what makes
	// `[[.0.]-[.2.]]` a range: both endpoints are single characters written the
	// long way.
	EXPECT_TRUE(pattern_match("[[.a.]]", "a"));
	EXPECT_FALSE(pattern_match("[[.a.]]", "b"));
	EXPECT_TRUE(pattern_match("[[=a=]]", "a"));
	EXPECT_TRUE(pattern_match("[[.0.]-[.2.]]", "1"));
	EXPECT_FALSE(pattern_match("[[.0.]-[.2.]]", "3"));
	EXPECT_TRUE(pattern_match("[[.a.]-[.c.]]", "b"));
	EXPECT_FALSE(pattern_match("[[.ab.]]", "a")) << "no multi-character symbol in C";
}

TEST(Pattern, ABackslashInsideABracketEscapesTheNextCharacter) {
	// POSIX leaves this to the locale, but the SHELL has already put the backslash
	// there on purpose: quoting a metacharacter is translated into `\` on its way
	// here, so `["*"]` must be the set holding an asterisk and not the set holding
	// a backslash as well. dash agrees on every line.
	EXPECT_TRUE(pattern_match("[\\*]", "*"));
	EXPECT_FALSE(pattern_match("[\\*]", "\\"));
	EXPECT_TRUE(pattern_match("[\\]]", "]")) << "and an escaped ']' does not close it";
	EXPECT_TRUE(pattern_match("[a\\-c]", "-")) << "an escaped '-' is not a range";
	EXPECT_FALSE(pattern_match("[a\\-c]", "b"));
}

TEST(Pattern, ADashBeforeTheClosingBracketIsAnOrdinaryCharacter) {
	EXPECT_TRUE(pattern_match("[a-]", "-"));
	EXPECT_TRUE(pattern_match("[a-]", "a"));
	EXPECT_FALSE(pattern_match("[a-]", "b"));
}

TEST(Pattern, ARightBracketFirstIsAMemberRatherThanTheTerminator) {
	EXPECT_TRUE(pattern_match("[]a]", "]"));
	EXPECT_TRUE(pattern_match("[]a]", "a"));
	EXPECT_FALSE(pattern_match("[!]a]", "]"));
	EXPECT_TRUE(pattern_match("[!]a]", "b"));
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

// --- the filesystem walk (#69) -----------------------------------------------
//
// The matcher above is not what #69 was about: pattern_match is right, and the
// walk in glob.cpp that turns matches into pathnames is what asserted a file
// existed without being permitted to check. See issue #69.

TEST(Pattern, ALiteralComponentPastAnUnsearchableDirectoryIsNotConfirmed) {
	// Root bypasses directory search permission entirely, so this would pass for
	// the wrong reason - the walk would never even attempt the check this test
	// exists to exercise.
	if (::geteuid() == 0)
		GTEST_SKIP() << "root ignores search permission; the check under test never runs";

	lesh::testing::temp_path scratch;
	const std::string dir = scratch.file("no_search_dir");
	ASSERT_EQ(::mkdir(dir.c_str(), 0755), 0);
	{
		std::ofstream touch(dir + "/file");
		ASSERT_TRUE(touch.good());
	}
	// Read but not search: readdir() can still list `file`, but resolving
	// `dir/file` by name needs the `x` bit this removes.
	ASSERT_EQ(::chmod(dir.c_str(), 0644), 0);

	lesh::buffer_pool pool{1024 * 32};
	lesh::arena_array<std::string_view> out{pool, 4};
	const std::string word = scratch.dir() + "/no_search_d*r/file";

	EXPECT_TRUE(expand_pathnames(pool, word, out));
	// A literal `file` after a directory the walk cannot search is unconfirmable,
	// not confirmed: POSIX leaves the whole pattern alone rather than have lesh
	// assert a file exists that it was never permitted to look for.
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0], word);

	// Restore before temp_path's destructor tries to remove this directory's
	// contents, or the scratch tree is left behind for every run after this one.
	ASSERT_EQ(::chmod(dir.c_str(), 0755), 0);
}
