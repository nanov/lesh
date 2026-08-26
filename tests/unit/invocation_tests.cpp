#include "runtime/invocation.h"

#include <gtest/gtest.h>

using namespace lesh::runtime;

namespace {

// parse_invocation reads a real argv, so the tests build one. The array outlives
// the returned invocation, which points into it - same lifetime as main's argv.
invocation parse(std::initializer_list<const char*> words) {
	static std::vector<const char*> storage;
	storage.assign(words.begin(), words.end());
	// argv[argc] IS A NULL POINTER in a real main - ISO C 5.1.2.2.1 and POSIX XBD 8
	// both require it - and the parser reads the array through that terminator, the
	// way every other option scan in the tree does. `argc` stays the word count.
	storage.push_back(nullptr);
	return parse_invocation(static_cast<int>(words.size()),
	                        const_cast<char**>(storage.data()));
}

} // namespace

// The bug that gated 3,600 conformance assertions: the yash signal suite invokes
// the shell as `sh +i +m`, and lesh took `+i` for a script pathname.
TEST(Invocation, PlusPolarityOptionsAreAccepted) {
	const invocation inv = parse({"sh", "+i", "+m"});
	EXPECT_EQ(inv.error, nullptr);
	EXPECT_EQ(inv.script_path, nullptr);
	EXPECT_EQ(inv.command_string, nullptr);
	ASSERT_TRUE(inv.interactive.has_value());
	EXPECT_FALSE(*inv.interactive);
	EXPECT_FALSE(inv.options.monitor);
}

TEST(Invocation, MinusPolarityOptionsAreAccepted) {
	const invocation inv = parse({"sh", "-i", "-m"});
	EXPECT_EQ(inv.error, nullptr);
	ASSERT_TRUE(inv.interactive.has_value());
	EXPECT_TRUE(*inv.interactive);
	EXPECT_TRUE(inv.options.monitor);
}

TEST(Invocation, NeitherPolarityLeavesInteractiveUndecided) {
	// Absent -i and +i, interactive is decided by isatty, not by this parser.
	const invocation inv = parse({"sh", "-e"});
	EXPECT_FALSE(inv.interactive.has_value());
	EXPECT_TRUE(inv.options.exit_on_error);
}

TEST(Invocation, OptionsGroupInOneWord) {
	const invocation inv = parse({"sh", "-eux"});
	EXPECT_EQ(inv.error, nullptr);
	EXPECT_TRUE(inv.options.exit_on_error);
	EXPECT_TRUE(inv.options.error_on_unset);
	EXPECT_TRUE(inv.options.trace);
}

TEST(Invocation, PlusPolarityDisablesWithinAGroup) {
	const invocation inv = parse({"sh", "-eu", "+e"});
	EXPECT_FALSE(inv.options.exit_on_error);
	EXPECT_TRUE(inv.options.error_on_unset);
}

TEST(Invocation, LongOptionNamesViaDashO) {
	const invocation a = parse({"sh", "-o", "errexit"});
	EXPECT_EQ(a.error, nullptr);
	EXPECT_TRUE(a.options.exit_on_error);

	const invocation b = parse({"sh", "-o", "noglob", "+o", "noglob"});
	EXPECT_EQ(b.error, nullptr);
	EXPECT_FALSE(b.options.no_glob);
}

TEST(Invocation, UnknownOptionIsAnErrorRatherThanAScriptName) {
	const invocation inv = parse({"sh", "-Z"});
	EXPECT_NE(inv.error, nullptr);
}

TEST(Invocation, DashCTakesTheCommandString) {
	const invocation inv = parse({"sh", "-c", "echo hi"});
	ASSERT_NE(inv.command_string, nullptr);
	EXPECT_STREQ(inv.command_string, "echo hi");
	EXPECT_EQ(inv.script_path, nullptr);
}

TEST(Invocation, DashCLastInAGroupStillTakesItsArgument) {
	const invocation inv = parse({"sh", "-ec", "echo hi"});
	EXPECT_TRUE(inv.options.exit_on_error);
	ASSERT_NE(inv.command_string, nullptr);
	EXPECT_STREQ(inv.command_string, "echo hi");
}

// The command string is the first OPERAND, so any letter after `c` in the same
// word is still an option. lesh ended the group at `c` and dropped them:
// `sh -cn CMD` executed CMD, which is option-p.tst's 'noexec on: for command is
// not executed'. dash reads `-cn` the same way.
TEST(Invocation, LettersAfterDashCAreStillOptions) {
	const invocation inv = parse({"sh", "-cn", "echo hi"});
	ASSERT_NE(inv.command_string, nullptr);
	EXPECT_STREQ(inv.command_string, "echo hi");
	EXPECT_TRUE(inv.options.no_exec);
}

// POSIX: `sh -c command_string [command_name [argument...]]`. The operand after
// the command string is $0, NOT $1 - which lesh had wrong.
TEST(Invocation, OperandAfterCommandStringIsDollarZero) {
	const invocation inv = parse({"sh", "-c", "echo hi", "myname", "a", "b"});
	ASSERT_NE(inv.command_name, nullptr);
	EXPECT_STREQ(inv.command_name, "myname");
	EXPECT_EQ(inv.first_argument, 4);
}

TEST(Invocation, ScriptOperandIsDollarZeroAndTheRestArePositional) {
	const invocation inv = parse({"sh", "script.sh", "a", "b"});
	ASSERT_NE(inv.script_path, nullptr);
	EXPECT_STREQ(inv.script_path, "script.sh");
	EXPECT_EQ(inv.first_argument, 2);
}

TEST(Invocation, DoubleDashEndsOptions) {
	const invocation inv = parse({"sh", "--", "-i"});
	ASSERT_NE(inv.script_path, nullptr);
	EXPECT_STREQ(inv.script_path, "-i");
	EXPECT_FALSE(inv.interactive.has_value());
}

TEST(Invocation, DashSReadsStandardInputEvenWithOperands) {
	// `sh -s a b` sets the positional parameters and still reads stdin.
	const invocation inv = parse({"sh", "-s", "a", "b"});
	EXPECT_TRUE(inv.read_stdin);
	EXPECT_EQ(inv.script_path, nullptr);
	EXPECT_EQ(inv.first_argument, 2);
}

// POSIX: `sh [options] -c command_string [command_name [argument...]]`. Once the
// command string has been consumed the option list is OVER, so a later word that
// looks like an option is an operand. lesh kept walking argv, read `--` as the
// option terminator and then took `-x` for $0 - losing a positional parameter.
// dash and bash both report $0 as `--` and $1 as `-x` (issue #44).
TEST(Invocation, TheOptionListEndsAtTheCommandString) {
	const invocation inv = parse({"sh", "-c", "echo hi", "--", "-x"});
	EXPECT_EQ(inv.error, nullptr);
	ASSERT_NE(inv.command_name, nullptr);
	EXPECT_STREQ(inv.command_name, "--");
	EXPECT_EQ(inv.first_argument, 4);
	EXPECT_FALSE(inv.interactive.has_value());
}

// Even a word that IS a valid option letter is an operand there: `-c code -s foo`
// gives $0 = `-s` and $1 = `foo`, and does NOT read standard input.
TEST(Invocation, AnOptionLetterAfterTheCommandStringIsAnOperand) {
	const invocation inv = parse({"sh", "-c", "echo hi", "-s", "foo"});
	EXPECT_EQ(inv.error, nullptr);
	EXPECT_FALSE(inv.read_stdin);
	ASSERT_NE(inv.command_name, nullptr);
	EXPECT_STREQ(inv.command_name, "-s");
	EXPECT_EQ(inv.first_argument, 4);
}

// The two halves of the rule in one case, because they pull against each other:
// the rest of the `-cn` WORD is still option letters, and no later word is.
TEST(Invocation, TheRestOfTheDashCWordIsOptionsButNoLaterWordIs) {
	const invocation inv = parse({"sh", "-cn", "echo hi", "-x", "a"});
	EXPECT_EQ(inv.error, nullptr);
	EXPECT_TRUE(inv.options.no_exec);
	EXPECT_FALSE(inv.options.trace);
	ASSERT_NE(inv.command_name, nullptr);
	EXPECT_STREQ(inv.command_name, "-x");
	EXPECT_EQ(inv.first_argument, 4);
}

// The other face of the same bug. The option list CONTINUES after -c, because
// POSIX's form is `sh -c [options] command_string ...` and the command string is
// an operand. Taking the next word instead ran `-e` as the command.
TEST(Invocation, AnOptionMaySitBetweenDashCAndTheCommandString) {
	const invocation inv = parse({"sh", "-c", "-e", "echo hi"});
	EXPECT_EQ(inv.error, nullptr);
	EXPECT_TRUE(inv.options.exit_on_error);
	ASSERT_NE(inv.command_string, nullptr);
	EXPECT_STREQ(inv.command_string, "echo hi");
}

// POSIX: "A single hyphen shall be treated as the first operand and then
// ignored." So it is not the command string - `sh -c - code` and `sh -c -- code`
// both run `code`, which is startup-p.tst:158 and :169. lesh ran the hyphen.
TEST(Invocation, AHyphenBeforeTheCommandStringIsIgnored) {
	const invocation hyphen = parse({"sh", "-c", "-", "echo hi"});
	EXPECT_EQ(hyphen.error, nullptr);
	ASSERT_NE(hyphen.command_string, nullptr);
	EXPECT_STREQ(hyphen.command_string, "echo hi");

	const invocation double_hyphen = parse({"sh", "-c", "--", "echo hi"});
	EXPECT_EQ(double_hyphen.error, nullptr);
	ASSERT_NE(double_hyphen.command_string, nullptr);
	EXPECT_STREQ(double_hyphen.command_string, "echo hi");
}

// -c with options but no operand has no command string, and a shell that cannot
// parse its own command line must not guess.
TEST(Invocation, DashCWithNoOperandIsRefused) {
	const invocation inv = parse({"sh", "-c", "-e"});
	EXPECT_NE(inv.error, nullptr);
	EXPECT_EQ(inv.command_string, nullptr);
}

// $0 follows argv[0] exactly, symlink included, whenever no operand names it.
// It came from a literal `"lesh"` default in shell_state, so every invocation
// without a command_file operand reported that string - including `sh -s`, which
// startup-p.tst:79 asserts. The fallback lives in parse_invocation rather than in
// main() so the rule is assertable at all (issue #43).
TEST(Invocation, DollarZeroFallsBackToArgvZero) {
	const invocation absolute = parse({"/tmp/d/sh", "-c", "echo hi"});
	ASSERT_NE(absolute.command_name, nullptr);
	EXPECT_STREQ(absolute.command_name, "/tmp/d/sh");

	// startup-p.tst's '$0 with -s': the operand is $1, so argv[0] is still $0.
	const invocation with_s = parse({"./sh", "-s", "X"});
	ASSERT_NE(with_s.command_name, nullptr);
	EXPECT_STREQ(with_s.command_name, "./sh");
	EXPECT_EQ(with_s.first_argument, 2);

	// No operands at all: the script arrives on standard input.
	const invocation bare = parse({"lesh"});
	ASSERT_NE(bare.command_name, nullptr);
	EXPECT_STREQ(bare.command_name, "lesh");
}

TEST(Invocation, AnOperandNamingDollarZeroBeatsArgvZero) {
	const invocation named = parse({"/bin/sh", "-c", "echo hi", "myname"});
	ASSERT_NE(named.command_name, nullptr);
	EXPECT_STREQ(named.command_name, "myname");

	const invocation script = parse({"/bin/sh", "script.sh"});
	ASSERT_NE(script.command_name, nullptr);
	EXPECT_STREQ(script.command_name, "script.sh");
}
