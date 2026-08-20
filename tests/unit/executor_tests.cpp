#include "runtime/executor.h"

#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <string>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

class ExecutorTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	shell_state state;

	int run(std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}
};

} // namespace

TEST_F(ExecutorTest, EmptyInputKeepsTheLastStatus) {
	state.set_last_status(7);
	EXPECT_EQ(run(""), 7);
}

TEST_F(ExecutorTest, SuccessfulCommandReportsZero) {
	EXPECT_EQ(run("/usr/bin/true"), 0);
}

TEST_F(ExecutorTest, FailingCommandReportsItsStatus) {
	EXPECT_EQ(run("/usr/bin/false"), 1);
}

TEST_F(ExecutorTest, CommandNotFoundIsOneTwentySeven) {
	EXPECT_EQ(run("/nonexistent/command/xyz"), 127);
}

TEST_F(ExecutorTest, FoundButNotExecutableIsOneTwentySix) {
	EXPECT_EQ(run("/etc/hosts"), 126);
}

TEST_F(ExecutorTest, AndOrShortCircuitsOnFailure) {
	// `false && x` must not run x.
	EXPECT_EQ(run("/usr/bin/false && /nonexistent/xyz"), 1)
		<< "status should be false's, not 127, proving the right side never ran";
}

TEST_F(ExecutorTest, AndOrRunsTheRightSideOnSuccess) {
	EXPECT_EQ(run("/usr/bin/true && /usr/bin/false"), 1);
}

TEST_F(ExecutorTest, OrShortCircuitsOnSuccess) {
	EXPECT_EQ(run("/usr/bin/true || /nonexistent/xyz"), 0);
}

TEST_F(ExecutorTest, OrRunsTheRightSideOnFailure) {
	EXPECT_EQ(run("/usr/bin/false || /usr/bin/true"), 0);
}

TEST_F(ExecutorTest, StatusIsRecordedForTheNextCommand) {
	run("/usr/bin/false");
	EXPECT_EQ(state.last_status(), 1);
}

TEST_F(ExecutorTest, SyntaxErrorReportsTwo) {
	EXPECT_EQ(run(";;"), 2) << "POSIX: a syntax error exits non-zero";
}

// --- shell state -------------------------------------------------------------

TEST(ShellState, InheritsTheProcessEnvironment) {
	shell_state state;
	std::string_view value;
	EXPECT_TRUE(state.lookup("PATH", value)) << "PATH should come from environ";
	EXPECT_FALSE(value.empty());
}

TEST(ShellState, SetAndLookupRoundTrip) {
	shell_state state;
	state.set("MY_VAR", "my value");
	std::string_view value;
	ASSERT_TRUE(state.lookup("MY_VAR", value));
	EXPECT_EQ(value, "my value");
}

TEST(ShellState, OwnsItsValuesRatherThanBorrowingThem) {
	// ADR-0007: every allocation has an owner. Legacy got this wrong - it
	// strdup()s alias text, points ASTs into it, and can never free it. Here the
	// value survives its source going away, because it was copied in.
	shell_state state;
	{
		const std::string temporary = "value that goes away";
		state.set("OWNED", temporary);
	}
	std::string_view value;
	ASSERT_TRUE(state.lookup("OWNED", value));
	EXPECT_EQ(value, "value that goes away");
}

TEST(ShellState, UnsetRemoves) {
	shell_state state;
	state.set("GONE", "here");
	state.unset("GONE");
	std::string_view value;
	EXPECT_FALSE(state.lookup("GONE", value));
}

TEST(ShellState, OnlyExportedVariablesReachTheEnvironmentBlock) {
	shell_state state;
	state.set("LOCAL_ONLY", "no");
	state.set_exported("EXPORTED", "yes");

	bool saw_exported = false, saw_local = false;
	for (char** e = state.environment_block(); *e != nullptr; ++e) {
		const std::string_view entry{*e};
		if (entry == "EXPORTED=yes") saw_exported = true;
		if (entry.starts_with("LOCAL_ONLY=")) saw_local = true;
	}
	EXPECT_TRUE(saw_exported);
	EXPECT_FALSE(saw_local) << "an unexported variable must not reach a child";
}

TEST(ShellState, IfsDefaultsToSpaceTabNewlineWhenUnset) {
	shell_state state;
	state.unset("IFS");
	EXPECT_EQ(state.ifs(), " \t\n");
}

TEST(ShellState, IfsSetToEmptyMeansNoSplitting) {
	// Distinct from unset. POSIX: IFS="" disables field splitting entirely, which
	// is why ifs() returns the value rather than falling back on emptiness.
	shell_state state;
	state.set("IFS", "");
	EXPECT_EQ(state.ifs(), "");
}

TEST(ShellState, SpecialBuiltinsAreDistinguishedFromRegularOnes) {
	// Not cosmetic: a failure in a special builtin exits a non-interactive shell,
	// and assignments preceding one persist afterwards.
	EXPECT_EQ(classify_builtin("export"), builtin_kind::special);
	EXPECT_EQ(classify_builtin("eval"), builtin_kind::special);
	EXPECT_EQ(classify_builtin(":"), builtin_kind::special);
	EXPECT_EQ(classify_builtin("cd"), builtin_kind::regular);
	EXPECT_EQ(classify_builtin("echo"), builtin_kind::regular);
	EXPECT_EQ(classify_builtin("ls"), builtin_kind::none);
}

// POSIX 2.9.1: a command with no command name completes with the exit status of
// the LAST command substitution it performed, and zero only when it performed
// none. `x=$(exit 3); echo $?` prints 3 - it does not print 0.
TEST_F(ExecutorTest, AssignmentOnlyCommandTakesTheLastSubstitutionStatus) {
	EXPECT_EQ(run("x=$(exit 3)"), 3);
	EXPECT_EQ(run("x=$(exit 3) y=$(exit 4)"), 4);
	EXPECT_EQ(run("x=$(true)"), 0);
}

TEST_F(ExecutorTest, AssignmentWithNoSubstitutionIsAlwaysZero) {
	state.set_last_status(9);
	EXPECT_EQ(run("x=1"), 0) << "a plain assignment must not inherit the old status";
}
