#include "runtime/invocation.h"

#include <gtest/gtest.h>

using namespace lesh::runtime;

namespace {

// parse_invocation reads a real argv, so the tests build one. The array outlives
// the returned invocation, which points into it - same lifetime as main's argv.
invocation parse(std::initializer_list<const char*> words) {
	static std::vector<const char*> storage;
	storage.assign(words.begin(), words.end());
	return parse_invocation(static_cast<int>(storage.size()),
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
