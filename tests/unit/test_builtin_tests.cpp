#include "runtime/builtins.h"

#include "runtime/executor.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// The `test` builtin, and `[`. See issue #35.
//
// `test` was CLASSIFIED as a builtin with no handler anywhere, so it silently
// returned 0: `test 1 = 2` was true, `test -s /nonexistent` was true, and every
// script that branched on `test` took the wrong branch with no diagnostic. `[` was
// in neither table and forked /bin/[, which answered correctly - so the bug was
// invisible to anyone testing with brackets.
//
// Status is 0 for true, 1 for FALSE and 2 for an ERROR, and the tests assert the
// difference everywhere: 1 and 2 are the two answers the broken version could not
// tell apart from success.
class TestBuiltinTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	shell_state state;

	int run(std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}
	// Every error case is measured with stderr redirected, so a passing suite stays
	// quiet and the DIAGNOSTIC is asserted where it matters rather than printed.
	int fails(std::string_view src) {
		std::string wrapped{src};
		wrapped += " 2>/dev/null";
		return run(wrapped);
	}
};

} // namespace

TEST_F(TestBuiltinTest, ArgumentCountRulesComeBeforeTheOperators) {
	// The part of POSIX's `test` that implementations get wrong, and the reason the
	// count rules are written out in builtins.cpp rather than pattern-matched.
	//
	// ZERO arguments is FALSE, not an error. ONE argument is a string test, whatever
	// the string looks like - which is what makes `test "$x"` behave when x happens
	// to hold `-n`, an operator name.
	EXPECT_EQ(run("test"), 1);
	EXPECT_EQ(run("[ ]"), 1);
	EXPECT_EQ(run("test ''"), 1);
	EXPECT_EQ(run("test x"), 0);
	EXPECT_EQ(run("x=-n; test \"$x\""), 0) << "one argument: a non-empty string";
	EXPECT_EQ(run("x=-z; test \"$x\""), 0);
	EXPECT_EQ(run("test !"), 0) << "`!` alone is a one-argument string test";
	EXPECT_EQ(run("test '('"), 0);
	// TWO arguments: `!` negates the one-argument rule, and a unary primary applies.
	EXPECT_EQ(run("test ! ''"), 0);
	EXPECT_EQ(run("test ! x"), 1);
	EXPECT_EQ(run("test -n -n"), 0) << "`-n` as the OPERAND of -n";
	EXPECT_EQ(run("test ! -n"), 1) << "`-n` as the operand of `!`";
	EXPECT_EQ(run("test -z ''"), 0);
	// THREE arguments: a binary primary wins over everything, then `!`, then parens.
	EXPECT_EQ(run("test = = ="), 0) << "`=` comparing two `=` strings";
	EXPECT_EQ(run("test -n = -n"), 0);
	EXPECT_EQ(run("test '(' = ')'"), 1);
	EXPECT_EQ(run("test ! ! ''"), 1) << "the two-argument rule inside the three";
	EXPECT_EQ(run("test ! ! !"), 0);
	EXPECT_EQ(run("test '(' x ')'"), 0);
	EXPECT_EQ(run("test '(' '' ')'"), 1);
	// FOUR arguments: `!` in front of the three-argument rule, or parens around two.
	EXPECT_EQ(run("test ! 1 = 1"), 1);
	EXPECT_EQ(run("test ! 1 = 2"), 0);
	EXPECT_EQ(run("test '(' ! '' ')'"), 0);
	EXPECT_EQ(run("test '(' -n x ')'"), 0);
}

TEST_F(TestBuiltinTest, StringAndIntegerComparison) {
	EXPECT_EQ(run("test abc = abc"), 0);
	EXPECT_EQ(run("test abc != abc"), 1);
	EXPECT_EQ(run("test 1 -eq 1"), 0);
	EXPECT_EQ(run("test 1 -ne 1"), 1);
	EXPECT_EQ(run("test 1 -lt 2"), 0);
	EXPECT_EQ(run("test 2 -le 2"), 0);
	EXPECT_EQ(run("test 3 -gt 2"), 0);
	EXPECT_EQ(run("test 2 -ge 3"), 1);
	EXPECT_EQ(run("test -1 -lt 0"), 0);
	EXPECT_EQ(run("test 007 -eq 7"), 0) << "leading zeros are decimal, not octal";
	EXPECT_EQ(run("test ' 1 ' -eq 1"), 0) << "dash allows blanks around the number";
	// A non-numeric operand is an ERROR, and 10 = 9 is not: an implementation that
	// compared the strings would answer 1 here and be wrong quietly.
	EXPECT_EQ(fails("test x -eq 1"), 2);
	EXPECT_EQ(fails("test 1 -eq x"), 2);
	EXPECT_EQ(fails("test '' -eq 0"), 2);
	EXPECT_EQ(fails("test 0x10 -eq 16"), 2) << "hexadecimal is not a POSIX integer";
	EXPECT_EQ(fails("test 99999999999999999999 -eq 1"), 2)
		<< "overflow must report rather than compare a truncated value";
	EXPECT_EQ(run("test 10 -gt 9"), 0) << "numeric, not lexicographic";
}

TEST_F(TestBuiltinTest, TheNegativeLimitIsAnOperandAndNotAnOverflow) {
	// ISSUE #62. `test -9223372036854775808 -eq 1` is a LEGAL comparison - dash,
	// bash and zsh all answer 1 with no diagnostic - and test_integer's range check
	// admits the magnitude 2^63 for exactly that reason. The value was then built
	// by NEGATING the signed conversion of it, and -INT64_MIN has no int64_t to be,
	// so the one operand the check exists to let through was the one that reached
	// undefined behaviour: an abort under the debug preset's UBSan, and in release
	// a wrap that made `test -9223372036854775808 -lt 0` FALSE.
	EXPECT_EQ(run("test -9223372036854775808 -eq 1"), 1);
	EXPECT_EQ(run("test -9223372036854775808 -lt 0"), 0)
		<< "the negative limit is negative";
	EXPECT_EQ(run("test -9223372036854775808 -eq -9223372036854775808"), 0);
	EXPECT_EQ(run("test -9223372036854775808 -lt -9223372036854775807"), 0)
		<< "and is the smallest operand there is";
	EXPECT_EQ(run("test 9223372036854775807 -gt 0"), 0) << "the positive limit too";

	// ONE PAST EITHER LIMIT STAYS A USAGE ERROR rather than saturating, which is
	// where this site parts company with arithmetic's over-large literal (#59).
	// `test` compares rather than computes, so an operand it cannot represent has
	// no nearest value worth standing in for it; dash and bash both report and exit
	// 2, and a clamped operand would silently compare a number nobody wrote.
	EXPECT_EQ(fails("test -9223372036854775809 -eq 1"), 2);
	EXPECT_EQ(fails("test 9223372036854775808 -gt 0"), 2);
	EXPECT_EQ(fails("test -99999999999999999999 -lt 0"), 2);
}

TEST_F(TestBuiltinTest, FileTests) {
	EXPECT_EQ(run("test -e /"), 0);
	EXPECT_EQ(run("test -d /"), 0);
	EXPECT_EQ(run("test -f /"), 1);
	EXPECT_EQ(run("test -e /nonexistent/lesh/path"), 1);
	EXPECT_EQ(run("test -s /nonexistent/lesh/path"), 1)
		<< "the case from the ticket: an unimplemented test reported 0 here";
	EXPECT_EQ(run("test -r /"), 0);
	EXPECT_EQ(run("test -x /"), 0);
	EXPECT_EQ(run("test -f /etc/hosts"), 0);
	EXPECT_EQ(run("test -s /etc/hosts"), 0);
	// -e follows a symlink and -h does not, which is why there are two stat calls
	// rather than one: /etc on macOS is a link to /private/etc.
	EXPECT_EQ(run("test -h /nonexistent/lesh/path"), 1);
	EXPECT_EQ(run("test -d /dev/null"), 1);
	EXPECT_EQ(run("test -c /dev/null"), 0);
	EXPECT_EQ(run("test -t 9"), 1) << "an fd that is not open is not a terminal";
}

TEST_F(TestBuiltinTest, NegationAndOrParentheses) {
	EXPECT_EQ(run("test 1 = 1 -a 2 = 2"), 0);
	EXPECT_EQ(run("test 1 = 1 -a 2 = 3"), 1);
	EXPECT_EQ(run("test 1 = 2 -o 2 = 2"), 0);
	EXPECT_EQ(run("test 1 = 2 -o 2 = 3"), 1);
	EXPECT_EQ(run("test '(' 1 = 1 ')'"), 0);
	EXPECT_EQ(run("test '(' 1 = 2 ')' -o '(' 3 = 3 ')'"), 0);
	EXPECT_EQ(run("test ! '(' 1 = 1 ')'"), 1);
	EXPECT_EQ(run("test '(' '(' 1 = 1 ')' ')'"), 0);
	// -a binds tighter than -o, which is what makes this true rather than false.
	EXPECT_EQ(run("test -z '' -a -n x -o -n ''"), 0);
	EXPECT_EQ(run("test -n x -a -n y -a -n z -a -n w"), 0);
	EXPECT_EQ(fails("test '(' 1 = 1"), 2) << "an unclosed paren is an error";
	EXPECT_EQ(fails("test 1 = 1 ')'"), 2);
}

TEST_F(TestBuiltinTest, ErrorsAreTwoRatherThanASilentAnswer) {
	// The distinction the whole builtin turns on: 1 is "the expression is false"
	// and 2 is "the expression is not an expression". An unimplemented `test`
	// answered 0 to both.
	EXPECT_EQ(fails("test bogusop x"), 2);
	EXPECT_EQ(fails("test x bogusop y"), 2);
	EXPECT_EQ(fails("test 1 = 1 = 1"), 2);
	EXPECT_EQ(fails("test 1 = 1 junk"), 2) << "leftover operands are not ignored";
	EXPECT_EQ(fails("test -N /"), 2) << "-N is not a primary dash has either";
	EXPECT_NE(fails("test x -eq 1"), 0);
	EXPECT_EQ(run("test 1 = 2"), 1) << "false, not an error";
}

TEST_F(TestBuiltinTest, BracketRequiresItsClosingBracket) {
	EXPECT_EQ(run("[ 1 = 1 ]"), 0);
	EXPECT_EQ(run("[ 1 = 2 ]"), 1);
	EXPECT_EQ(run("[ ! ]"), 0) << "`!` is a one-argument string test here too";
	EXPECT_EQ(fails("[ 1 = 1"), 2);
	EXPECT_EQ(fails("["), 2);
	EXPECT_EQ(fails("[ 1 = 1 ] extra"), 2) << "the bracket must be LAST";
}

TEST_F(TestBuiltinTest, FreshnessComparisonsAgainstAMissingFile) {
	// A file that does not exist has no modification time: an existing file is
	// NEWER than it, and it is OLDER than every existing file. test-p.tst asserts
	// both (`XXXXX -ot newer` true, `newer -nt XXXXX` true) and bash answers the
	// same; dash, zsh and macOS test(1) report false as soon as either stat fails,
	// which is the divergence recorded in ADR-0001.
	const lesh::testing::temp_path scratch;
	const std::string present = scratch.file("test_present");
	{
		std::ofstream out{present};
		out << "x";
	}
	const std::string missing = scratch.file("test_missing");
	EXPECT_EQ(run("test " + missing + " -ot " + present), 0) << "missing is older";
	EXPECT_EQ(run("test " + present + " -nt " + missing), 0) << "present is newer";
	EXPECT_EQ(run("test " + missing + " -nt " + present), 1);
	EXPECT_EQ(run("test " + present + " -ot " + missing), 1);
	// Two missing operands are neither newer nor older than one another, and `-ef`
	// is unmoved: a pathname that names nothing names nothing in common.
	EXPECT_EQ(run("test " + missing + " -nt " + missing), 1);
	EXPECT_EQ(run("test " + missing + " -ot " + missing), 1);
	EXPECT_EQ(run("test " + missing + " -ef " + present), 1);
	EXPECT_EQ(run("test " + present + " -ef " + present), 0);
	std::remove(present.c_str());
}

// --- the `test` operand, read through the one numeric parser (issue #63) -----

TEST_F(TestBuiltinTest, TheIntegerLimitsAreOperandsRatherThanOverflows) {
	// INT64_MIN is a LEGAL operand - dash, bash and zsh all compare it without
	// complaint - so the range deliberately admits magnitude 2^63 and the negation
	// happens in the unsigned domain, where it is defined (#62). This is the one
	// value the range check exists to let through and the one the old code
	// negated undefined.
	EXPECT_EQ(run("test -9223372036854775808 -eq 1"), 1);
	EXPECT_EQ(run("test -9223372036854775808 -lt 0"), 0);
	EXPECT_EQ(run("test 9223372036854775807 -gt 0"), 0);
	EXPECT_EQ(run("test -9223372036854775808 -eq -9223372036854775808"), 0);
}

TEST_F(TestBuiltinTest, AnOperandPastTheLimitIsAUsageErrorRatherThanASaturatedOne) {
	// `test` COMPARES rather than computes, so an operand it cannot represent has
	// no nearest answer worth giving: 2 and a diagnostic, as dash and bash both
	// answer. Saturating - which is what an arithmetic literal does (#59) - would
	// silently compare a number the script never wrote.
	EXPECT_EQ(fails("test 9223372036854775808 -gt 0"), 2);
	EXPECT_EQ(fails("test -9223372036854775809 -eq 1"), 2);
	EXPECT_EQ(fails("test 99999999999999999999 -eq 1"), 2);
	EXPECT_EQ(fails("test 3x -eq 3"), 2) << "atoi would have truncated this to 3";
}

TEST_F(TestBuiltinTest, TheOperandKeepsDashsBlankAndSignTolerances) {
	// Copied from dash's `getn` deliberately rather than tightened, and now stated
	// in the policy table where every site's tolerances are: blanks either side,
	// including a newline, and a leading `+`.
	EXPECT_EQ(run("test ' 1 ' -eq 1"), 0);
	EXPECT_EQ(run("test '+1' -eq 1"), 0);
	EXPECT_EQ(fails("test '1 2' -eq 1"), 2);
}
