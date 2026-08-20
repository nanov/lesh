#include "runtime/builtins.h"

#include "runtime/executor.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// getopts. See issue #31.
//
// The builtin did not exist, and macOS ships a /usr/bin/getopts stub that runs
// `builtin getopts` in a CHILD shell and exits 0 - so every call succeeded, set
// nothing, and eleven of getopts-p.tst's assertions passed on the strength of
// `$opt` being empty. That is the "stub that succeeds" trap, arriving from the
// filesystem rather than from this repo, and it is why these tests assert the
// EFFECT on OPTIND, OPTARG and the operand variable rather than a status.
class GetoptsTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	// One state for the whole test, because getopts' state PERSISTS between calls -
	// that is the entire builtin. A test that runs more than one snippet therefore
	// says `OPTIND=1` to start a fresh parse, exactly as a script would.
	shell_state state;

	int run(std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}

	// Runs `src` with its standard output in a file and returns what it wrote. A
	// redirection rather than freopen, because the shell's own stdio is what is
	// under test and swapping it out from underneath would change the thing
	// measured.
	std::string capture(std::string_view src) {
		return capture_fd("1", src);
	}

	std::string capture_stderr(std::string_view src) {
		return capture_fd("2", src);
	}

	std::string capture_fd(std::string_view fd, std::string_view src) {
		const std::string path = ::testing::TempDir() + "lesh_getopts_capture.txt";
		std::remove(path.c_str());
		std::string wrapped{"{ "};
		wrapped.append(src);
		wrapped += "; } ";
		wrapped.append(fd);
		wrapped += "> ";
		wrapped += path;
		(void)run(wrapped);
		std::ifstream in{path};
		std::ostringstream text;
		text << in.rdbuf();
		std::remove(path.c_str());
		return text.str();
	}

	// The value of a variable as the shell holds it, and whether it is set at all.
	// `${x-unset}` in a captured script would answer the second question too, but
	// reading the state directly is what tells an empty string from an absent
	// variable without depending on the expander being right.
	[[nodiscard]] std::string value_of(std::string_view name) const {
		std::string_view text;
		return state.lookup(name, text) ? std::string{text} : std::string{"<unset>"};
	}
};

} // namespace

TEST_F(GetoptsTest, GetoptsIsARegularBuiltin) {
	// POSIX 2.14 does not list getopts among the special builtins, so a failure of
	// it must NOT exit a non-interactive shell. Registering it in the wrong table
	// would make `getopts` with too few operands - a usage error scripts hit while
	// being written - kill the script.
	EXPECT_EQ(classify_builtin("getopts"), builtin_kind::regular);
	EXPECT_EQ(capture("getopts 2>/dev/null; echo reached"), "reached\n");
}

TEST_F(GetoptsTest, UsageErrorsAreLoudRatherThanSilentSuccess) {
	// The whole point of the item: an unimplementable or malformed call reports.
	// `getopts` with one operand cannot do anything useful, and returning 0 there
	// is how eleven assertions passed against a shell that had no getopts at all.
	EXPECT_EQ(run("getopts ab 2>/dev/null"), 2);
	EXPECT_NE(capture_stderr("getopts ab"), "");
	EXPECT_EQ(run("getopts ab 1bad -a 2>/dev/null"), 2)
		<< "the operand is a variable the shell must be able to assign";
	EXPECT_NE(capture_stderr("getopts ab 1bad -a"), "");
}

TEST_F(GetoptsTest, OptindIsOneAtStartupAndIsNotExported) {
	// POSIX: OPTIND is initialised to 1 when the shell is invoked. Not exported,
	// which getopts-p.tst checks by asking a child shell for it - a child that
	// inherited a parent's half-finished index would resume a parse of arguments
	// it never saw.
	EXPECT_EQ(value_of("OPTIND"), "1");
	EXPECT_FALSE(state.is_exported("OPTIND"));
	EXPECT_EQ(value_of("OPTARG"), "<unset>");
}

TEST_F(GetoptsTest, OneOptionPerCallIncludingGroupedLetters) {
	EXPECT_EQ(capture("getopts abc o -abc; echo $o; getopts abc o -abc; echo $o; "
	                  "getopts abc o -abc; echo $o"),
	          "a\nb\nc\n");
}

TEST_F(GetoptsTest, OptindNamesTheArgumentStillBeingParsed) {
	// The divergence from dash, asserted so it cannot drift silently. POSIX defines
	// OPTIND as the index of the next argument to be PROCESSED, and a word with
	// letters left in it is still that argument - so OPTIND stays 1 through `-abc`
	// and moves to 2 only when `c` is taken. bash, ksh and zsh all report it this
	// way; dash advances on entering the word, which makes `shift $((OPTIND-1))`
	// after a mid-word `?` throw away letters nobody has looked at.
	EXPECT_EQ(capture("set -- -abc; getopts abc o; echo $OPTIND; getopts abc o; "
	                  "echo $OPTIND; getopts abc o; echo $OPTIND"),
	          "1\n1\n2\n");
}

TEST_F(GetoptsTest, TheWithinWordPositionLivesBesideOptindAndAnAssignmentClearsIt) {
	// Decision 1. OPTIND cannot say "the third letter of the second word", so the
	// offset lives in the state next to it - and because OPTIND is 1 for the whole
	// of `-abc`, a caller's `OPTIND=1` writes the value that is already there.
	// Detecting the reset by comparing OPTIND against a shadow copy would see no
	// change and resume mid-word; hooking the assignment is what makes the reset
	// POSIX documents actually work.
	(void)run("set -- -abc; getopts abc o");
	EXPECT_EQ(value_of("OPTIND"), "1");
	EXPECT_EQ(state.getopts_offset(), 2u) << "past the `-` and the `a`";
	EXPECT_EQ(capture("OPTIND=1; getopts abc o; echo $o"), "a\n");
	EXPECT_EQ(capture("OPTIND=1; set -- -abc; getopts abc o; getopts abc o; echo $o; "
	                  "OPTIND=1; getopts abc o; echo $o"),
	          "b\na\n");
}

TEST_F(GetoptsTest, UnsettingOptindAlsoRestartsTheParse) {
	// Same hook, other spelling. dash makes this FATAL - `unset OPTIND` there
	// aborts the shell with "Illegal number:" from inside its assignment hook -
	// while POSIX says nothing about the case at all.
	EXPECT_EQ(capture("set -- -abc; getopts abc o; unset OPTIND; getopts abc o; echo $o"),
	          "a\n");
}

TEST_F(GetoptsTest, AnOptionArgumentIsTakenAdjoinedOrSeparate) {
	EXPECT_EQ(capture("getopts a:b o -a'  foo' -b; echo \"[$OPTARG][$OPTIND]\""),
	          "[  foo][2]\n");
	EXPECT_EQ(capture("OPTIND=1; getopts a:b o -a '-x  foo' -b; echo \"[$OPTARG][$OPTIND]\""),
	          "[-x  foo][3]\n")
		<< "an argument is taken whole, even when it looks like an option";
	EXPECT_EQ(capture("OPTIND=1; getopts a:b o -a '' -b; echo \"[$OPTARG][$OPTIND]\""),
	          "[][3]\n");
}

TEST_F(GetoptsTest, OptargIsUnsetWhenTheOptionTakesNoArgument) {
	// POSIX requires UNSET, which is what lets `${OPTARG-unset}` tell an
	// argument-less option from one whose argument was the empty string. dash and
	// zsh leave the previous OPTARG as an empty string and both fail
	// getopts-p.tst's two assertions about it.
	(void)run("getopts a:b o -a foo -b; getopts a:b o -a foo -b");
	EXPECT_EQ(value_of("o"), "b");
	EXPECT_EQ(value_of("OPTARG"), "<unset>");
	(void)run("OPTIND=1; getopts a x -a; getopts a x -a");
	EXPECT_EQ(value_of("OPTARG"), "<unset>") << "and unset again at the end of options";
}

TEST_F(GetoptsTest, AnUnknownOptionIsQuestionMarkAndStatusZero) {
	// Status ZERO: POSIX gives >0 to the END of the options, and an unknown option
	// was still an option found. Returning 1 here would break the `while getopts`
	// idiom - the loop would stop before the script's own `?)` case could report.
	EXPECT_EQ(run("getopts '' o -a 2>/dev/null"), 0);
	EXPECT_EQ(capture("getopts '' o -a 2>/dev/null; echo \"[$o]\""), "[?]\n");
	EXPECT_NE(capture_stderr("OPTIND=1; getopts '' o -a"), "");
	EXPECT_EQ(value_of("OPTARG"), "<unset>");
}

TEST_F(GetoptsTest, ALeadingColonHandsTheDiagnosticToTheCaller) {
	// The `:` form is not cosmetic: it is how a script reports its own errors, so
	// the letter has to arrive in OPTARG and nothing may be printed.
	EXPECT_EQ(capture("getopts : o -a; echo \"[$o][$OPTARG]\""), "[?][a]\n");
	EXPECT_EQ(capture_stderr("getopts : o -a"), "");
}

TEST_F(GetoptsTest, AMissingOptionArgumentIsColonOrQuestionMark) {
	EXPECT_EQ(capture("getopts :a: v -a; echo \"[$v][$OPTARG]\""), "[:][a]\n");
	EXPECT_EQ(capture_stderr("OPTIND=1; getopts :a: v -a"), "");
	EXPECT_EQ(capture("OPTIND=1; getopts a: v -a 2>/dev/null; echo \"[$v]\""), "[?]\n");
	EXPECT_NE(capture_stderr("OPTIND=1; getopts a: v -a"), "");
	EXPECT_EQ(value_of("OPTARG"), "<unset>");
}

TEST_F(GetoptsTest, TheColonInAnOptstringIsAMarkerAndNeverAnOptionLetter) {
	// `-:` has to be an unknown option even though `:` appears in the optstring:
	// there it marks `a` as taking an argument. Searching the optstring without
	// this test would parse a colon as a valid option and set OPTARG from the next
	// word.
	EXPECT_EQ(capture("getopts a:b v -: 2>/dev/null; echo \"[$v]\""), "[?]\n");
}

TEST_F(GetoptsTest, DoubleHyphenEndsTheOptionsAndOptindPointsPastIt) {
	EXPECT_EQ(capture("getopts ab x -a -- -b; echo \"[$x]\"; "
	                  "getopts ab x -a -- -b || echo \"[$OPTIND]\""),
	          "[a]\n[3]\n");
	EXPECT_EQ(capture("getopts '' x --; echo $OPTIND"), "2\n");
}

TEST_F(GetoptsTest, ALoneHyphenIsAnOperandRatherThanAnOption) {
	// A word of one character cannot be an option, so the length is tested before
	// the leading `-` is believed. `getopts '' x -` reporting `?` with status 1 is
	// what stops `sh - <file` style operands being eaten.
	EXPECT_EQ(run("getopts '' x -"), 1);
	EXPECT_EQ(value_of("OPTIND"), "1") << "the operand is left for the caller";
}

TEST_F(GetoptsTest, TheEndOfTheOptionsIsStatusOneWithQuestionMarkAndUnsetOptarg) {
	EXPECT_EQ(run("getopts a x -a; getopts a x -a"), 1);
	EXPECT_EQ(value_of("x"), "?");
	EXPECT_EQ(value_of("OPTARG"), "<unset>");
	EXPECT_EQ(capture("OPTIND=1; getopts '' x operand; echo $OPTIND"), "1\n")
		<< "OPTIND is the first operand's index, so `shift $((OPTIND-1))` keeps it";
}

TEST_F(GetoptsTest, ThePositionalParametersAreTheDefaultArguments) {
	EXPECT_EQ(capture("set -- -a -b arg -c; getopts ab:c o; echo $o; getopts ab:c o; "
	                  "echo \"$o $OPTARG\"; getopts ab:c o; echo $o"),
	          "a\nb arg\nc\n");
}

TEST_F(GetoptsTest, OptionLettersMayBeAnyAlphanumeric) {
	// POSIX says option characters are alphanumeric, and a digit is the case an
	// optstring scan written with isalpha() would drop.
	EXPECT_EQ(capture("set -- -1 -2; getopts ab:01: o; echo \"[$o][$OPTARG]\""),
	          "[1][-2]\n");
}

TEST_F(GetoptsTest, AnOptindPastTheLastArgumentIsTheEndOfTheOptions) {
	// Decision 2, and POSIX calls it unspecified: the shells disagree, so this
	// picks the answer that cannot lose arguments. bash clamps to $#+1 and reports
	// the end; dash silently restarts the whole parse from word 1, which re-runs
	// options the script has already acted on.
	EXPECT_EQ(run("set -- -a -b; OPTIND=99; getopts ab o"), 1);
	EXPECT_EQ(value_of("o"), "?");
	EXPECT_EQ(value_of("OPTIND"), "3");
	// A non-numeric or zero OPTIND restarts instead, since there is no argument it
	// could be naming. dash aborts the shell here.
	EXPECT_EQ(capture("set -- -a -b; getopts ab o; OPTIND=junk; getopts ab o; echo $o"),
	          "a\n");
}

TEST_F(GetoptsTest, TheStateIsSharedWithAFunctionRatherThanRestartedPerCall) {
	// Decision 4. POSIX keeps the index in a shell variable, so a function sees
	// the shell's index and a function that wants its own parse must say
	// `OPTIND=1` - which is exactly what the yash suite's own testcase() does.
	// bash and ksh agree; dash and zsh reset their internal index on function
	// entry while leaving the OPTIND variable alone, so inside a dash function the
	// variable and the parse disagree about where they are.
	EXPECT_EQ(capture("f() { getopts ab o; echo \"f:$o\"; }; set -- -a -b; "
	                  "getopts ab o; echo \"main:$o\"; f -a -b; echo \"OPTIND=$OPTIND\""),
	          "main:a\nf:b\nOPTIND=3\n");
	EXPECT_EQ(capture("f() { OPTIND=1; while getopts ab o; do echo \"f:$o\"; done; }; "
	                  "set -- -a; getopts ab o; f -a -b"),
	          "f:a\nf:b\n")
		<< "the documented way for a function to parse its own arguments";
}

TEST_F(GetoptsTest, ArgumentsThatChangeUnderASavedOffsetRestartTheWord) {
	// Also unspecified by POSIX, and the one case that could read off the end of a
	// word: the offset was saved against a longer argument list than the next call
	// passes. Starting the word over is the only answer that stays in bounds.
	EXPECT_EQ(capture("getopts abc o -abc; getopts abc o -a; echo \"[$o][$OPTIND]\""),
	          "[a][2]\n");
}
