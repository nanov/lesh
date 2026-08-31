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
	// It is asked of a SEGMENT, so it stays generous about `[`: a bracket
	// expression can open in one segment and close in another. is_pattern below is
	// the strict one, and the two differ on exactly this input.
	EXPECT_TRUE(has_pattern_characters("[a")) << "a segment that may yet open one";
}

// --- the bracket rule at the glob gate (#204) --------------------------------
//
// POSIX 2.13.1: a '[' with no matching ']' later in the same word is an ordinary
// character. The MATCHER has always known that - `pattern_match("[abc", "[abc")`
// above is the same rule - but the gate that decides whether to walk the
// filesystem at all did not, so `[ $i -lt 1 ]` opened the current directory twice
// per loop iteration to match two words that could only ever match themselves.

TEST(Pattern, AnUnterminatedBracketIsNotAPattern) {
	EXPECT_FALSE(is_pattern("[")) << "nothing to close it";
	EXPECT_FALSE(is_pattern("[abc"));
	EXPECT_FALSE(is_pattern("a[b"));
	// A ']' first is a MEMBER rather than the terminator, so `[]` opens a bracket
	// expression whose first element is ']' and then runs out of word.
	EXPECT_FALSE(is_pattern("[]"));
	EXPECT_FALSE(is_pattern("[!"));
	EXPECT_FALSE(is_pattern("[a\\]")) << "an escaped ']' does not close it either";
}

TEST(Pattern, ARightBracketAloneIsNeverAPattern) {
	EXPECT_FALSE(is_pattern("]"));
	EXPECT_FALSE(is_pattern("a]b"));
	EXPECT_FALSE(is_pattern("]["));
}

TEST(Pattern, AWellFormedBracketIsStillAPattern) {
	EXPECT_TRUE(is_pattern("[a]"));
	EXPECT_TRUE(is_pattern("[!a]"));
	EXPECT_TRUE(is_pattern("[^a]"));
	EXPECT_TRUE(is_pattern("x[abc]z"));
	EXPECT_TRUE(is_pattern("[a-z]"));
	EXPECT_TRUE(is_pattern("[]a]")) << "a ']' first is a member, and the second closes";
	EXPECT_TRUE(is_pattern("[a\\]]")) << "the escaped ']' is a member, the third closes";
	EXPECT_TRUE(is_pattern("[[:alpha:]]"));
	// The matcher re-reads from the byte after a '[' that opened nothing, so the
	// first '[' here is literal and the SECOND opens a character class. Testing
	// only the first '[' would call this word literal and stop globbing it.
	EXPECT_TRUE(is_pattern("[[:alpha:]"));
	EXPECT_TRUE(is_pattern("[[:]")) << "`[` then the bracket expression `[:]`";
}

TEST(Pattern, TheOtherMetacharactersAreUnaffectedByTheBracketRule) {
	EXPECT_TRUE(is_pattern("*"));
	EXPECT_TRUE(is_pattern("?"));
	EXPECT_TRUE(is_pattern("[abc*")) << "the '[' is literal but the '*' is not";
	EXPECT_FALSE(is_pattern("plain"));
	EXPECT_FALSE(is_pattern(""));
}

TEST(Pattern, QuotingIsUnchangedByTheBracketRule) {
	// `\[` was already literal and stays so; the rule adds nothing here. Quoting
	// reaches the matcher as backslashes, which is the form the expander emits.
	EXPECT_FALSE(is_pattern("\\["));
	EXPECT_FALSE(is_pattern("\\[a\\]"));
	EXPECT_FALSE(is_pattern("\\*"));
	// ...and an escaped '[' does not open the expression a later ']' would close.
	EXPECT_FALSE(is_pattern("\\[a]"));
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

// --- the bracket rule at the walk (#204) -------------------------------------

TEST(Pattern, AnUnterminatedBracketDoesNotOpenADirectory) {
	// The observable is expand_pathnames' RETURN VALUE. `false` is decided by
	// is_pattern before the walk starts - it is the first statement in the
	// function - so it is precisely the assertion that no opendir happened.
	// Timing would prove nothing against a warm page cache, and counting through
	// a fake filesystem would be asserting something about the fake.
	lesh::testing::temp_path present;
	// These files EXIST, which is the harder half: a word that names a real file
	// must still not be walked to find it. The walk would produce the same
	// pathname; what it would cost is the point.
	for (const char* name : {"[", "]", "[abc", "a[b", "[]"}) {
		std::ofstream touch(present.file(name));
		ASSERT_TRUE(touch.good()) << name;
	}
	lesh::testing::temp_path absent;  // ...and here none of them do.

	lesh::buffer_pool pool{1024 * 32};
	for (const lesh::testing::temp_path* scratch : {&present, &absent}) {
		for (const char* name : {"[", "]", "[abc", "a[b", "[]", "\\["}) {
			lesh::arena_array<std::string_view> out{pool, 4};
			const std::string word = scratch->file(name);
			EXPECT_FALSE(expand_pathnames(pool, word, out)) << word;
			EXPECT_EQ(out.size(), 0u) << word;
		}
	}
}

TEST(Pattern, AWellFormedBracketStillWalksTheDirectory) {
	// The control for the test above: the same directory, reached the same way, is
	// scanned when the bracket expression actually closes - so `false` there is the
	// rule at work and not a scratch directory nothing could ever match in.
	lesh::testing::temp_path scratch;
	for (const char* name : {"a", "b"}) {
		std::ofstream touch(scratch.file(name));
		ASSERT_TRUE(touch.good()) << name;
	}

	lesh::buffer_pool pool{1024 * 32};
	{
		lesh::arena_array<std::string_view> out{pool, 4};
		EXPECT_TRUE(expand_pathnames(pool, scratch.file("[a]"), out));
		ASSERT_EQ(out.size(), 1u);
		EXPECT_EQ(out[0], scratch.file("a"));
	}
	{
		lesh::arena_array<std::string_view> out{pool, 4};
		EXPECT_TRUE(expand_pathnames(pool, scratch.file("[!a]"), out));
		ASSERT_EQ(out.size(), 1u);
		EXPECT_EQ(out[0], scratch.file("b"));
	}
	{
		lesh::arena_array<std::string_view> out{pool, 4};
		EXPECT_TRUE(expand_pathnames(pool, scratch.file("[ab]"), out));
		ASSERT_EQ(out.size(), 2u);
		EXPECT_EQ(out[0], scratch.file("a"));
		EXPECT_EQ(out[1], scratch.file("b"));
	}
}

TEST(Pattern, ABracketExpressionCannotSpanAPathComponent) {
	// `[a/b]` holds a '[' and a ']', but POSIX gives '/' to the path separator
	// alone: neither component is a pattern, so neither directory is opened and
	// the word names itself.
	lesh::testing::temp_path scratch;
	ASSERT_EQ(::mkdir(scratch.file("[a").c_str(), 0755), 0);
	{
		std::ofstream touch(scratch.file("[a/b]"));
		ASSERT_TRUE(touch.good());
	}

	lesh::buffer_pool pool{1024 * 32};
	lesh::arena_array<std::string_view> out{pool, 4};
	const std::string word = scratch.file("[a/b]");
	// True, because the word-level test cannot see that the '/' separates them -
	// but each COMPONENT is taken literally, so the walk extends the path by name
	// and confirms it with lstat instead of scanning either directory.
	EXPECT_TRUE(expand_pathnames(pool, word, out));
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0], word);
}

// --- quote removal, and the two forms of one field (#210) ---------------------

TEST(Pattern, RemovingEscapesKeepsTheEscapedByteAndDropsTheBackslash) {
	// The inverse of the escaping the expander does on the way in. Sized for the
	// worst case, which is the input length: unescaping only ever shortens.
	const auto unescaped = [](std::string_view pattern) {
		std::string out(pattern.size(), '\0');
		out.resize(remove_pattern_escapes(pattern, out.data()));
		return out;
	};

	EXPECT_EQ(unescaped("a\\*b"), "a*b");
	EXPECT_EQ(unescaped("plain"), "plain");
	EXPECT_EQ(unescaped("\\\\"), "\\") << "an escaped backslash is one backslash";
	EXPECT_EQ(unescaped("a\\"), "a\\")
		<< "a trailing lone backslash escapes nothing, so it stands for itself - "
		   "which is exactly what match_here does with it";
	EXPECT_EQ(unescaped(""), "");
	// The whole escaped alphabet, since is_pattern_syntax is wider than the three
	// metacharacters.
	EXPECT_EQ(unescaped("\\*\\?\\[\\]\\-\\!\\^"), "*?[]-!^");
}

TEST(Pattern, AnEscapedMetacharacterIsNotAPatternAndOpensNoDirectory) {
	// `a\*b` names one file. Nothing in it is live, so it is not a pattern at all
	// and the walk is refused before it starts - the same observable #204 uses.
	// The file EXISTS here, which is the harder half: the word must not be walked
	// even though a scan would produce the very same pathname.
	lesh::testing::temp_path scratch;
	{
		std::ofstream touch(scratch.file("a*b"));
		ASSERT_TRUE(touch.good());
	}

	lesh::buffer_pool pool{1024 * 32};
	for (const char* name : {"a\\*b", "a\\?b", "\\[a\\]", "a\\*b\\*"}) {
		lesh::arena_array<std::string_view> out{pool, 4};
		EXPECT_FALSE(expand_pathnames(pool, scratch.file(name), out)) << name;
		EXPECT_EQ(out.size(), 0u) << name;
	}
}

TEST(Pattern, TheWalkReadsThePatternAndFallsBackToTheQuoteRemovedForm) {
	// The two forms POSIX 2.6 hands to pathname expansion and to quote removal.
	// A live `*` beside an escaped one makes the word a pattern; only the live one
	// wildcards, so `a\*b*` selects the file literally called `a*b` and not `aXb`.
	lesh::testing::temp_path scratch;
	for (const char* name : {"a*b", "aXb"}) {
		std::ofstream touch(scratch.file(name));
		ASSERT_TRUE(touch.good()) << name;
	}

	lesh::buffer_pool pool{1024 * 32};
	{
		lesh::arena_array<std::string_view> out{pool, 4};
		EXPECT_TRUE(expand_pathnames(pool, scratch.file("a\\*b*"),
		                             scratch.file("a*b*"), out));
		ASSERT_EQ(out.size(), 1u);
		EXPECT_EQ(out[0], scratch.file("a*b"));
	}
	{
		// Nothing matched, so the word expands to itself - and then quote removal
		// runs on it, which is the second form and not the first. Passing the
		// pattern here instead would print the backslash the user typed.
		lesh::arena_array<std::string_view> out{pool, 4};
		EXPECT_TRUE(expand_pathnames(pool, scratch.file("a\\*q*"),
		                             scratch.file("a*q*"), out));
		ASSERT_EQ(out.size(), 1u);
		EXPECT_EQ(out[0], scratch.file("a*q*"));
	}
	{
		// A LITERAL component of a globbed word is extended by name, and the name is
		// the unescaped one: the directory really is called `a*b`.
		lesh::testing::temp_path tree;
		ASSERT_EQ(::mkdir(tree.file("a*b").c_str(), 0755), 0);
		std::ofstream touch(tree.file("a*b/f"));
		ASSERT_TRUE(touch.good());

		lesh::arena_array<std::string_view> out{pool, 4};
		EXPECT_TRUE(expand_pathnames(pool, tree.file("a\\*b/*"),
		                             tree.file("a*b/*"), out));
		ASSERT_EQ(out.size(), 1u);
		EXPECT_EQ(out[0], tree.file("a*b/f"));
	}
}
