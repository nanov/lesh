#include "runtime/executor.h"

#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
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

// `! pipeline`. POSIX puts Bang in the pipeline production, so it binds to the
// whole pipeline and inverts only the sense of the status - one for zero, zero for
// anything else, never the value itself.
TEST_F(ExecutorTest, NegationInvertsTheSenseOfTheStatus) {
	EXPECT_EQ(run("! false"), 0);
	EXPECT_EQ(run("! true"), 1);
	// Any non-zero status becomes exactly zero, not the value negated. A subshell
	// is used to produce 7 because `exit 7` would unwind before `!` could apply.
	EXPECT_EQ(run("! (exit 7)"), 0);
}

// `set -e` must EXIT the shell. Ending the enclosing list instead left the loop
// free to iterate again, so `while true; do false; done` ran forever.
TEST_F(ExecutorTest, ErrexitExitsRatherThanEndingTheList) {
	EXPECT_EQ(run("set -e; while true; do false; done; echo unreachable"), 1);
}

// POSIX suppresses `set -e` where the status is TESTED. Each of these must reach
// its `exit 42`, which is the only way to tell "did not exit" from "exited zero".
TEST_F(ExecutorTest, ErrexitIsSuppressedWhereTheStatusIsTested) {
	const char* survives[] = {
		"set -e; if false; then :; fi; exit 42",
		"set -e; while false; do :; done; exit 42",
		"set -e; until true; do :; done; exit 42",
		"set -e; false && :; exit 42",
		"set -e; { false && :; }; exit 42",
		"set -e; while true; do false && :; break; done; exit 42",
		"set -e; ! true; exit 42",
	};
	for (const char* src : survives)
		EXPECT_EQ(run(src), 42) << src;
}

TEST_F(ExecutorTest, ErrexitStillFiresWhereTheStatusIsActedOn) {
	const char* exits[] = {
		"set -e; false; exit 42",
		"set -e; true && false; exit 42",
		"set -e; false || false; exit 42",
		"set -e; case x in x) false;; esac; exit 42",
	};
	for (const char* src : exits)
		EXPECT_EQ(run(src), 1) << src;
}

// POSIX: `wait` with no operands waits for every known child and its status is
// ZERO - not the last child's. Reporting the last one made `false & wait` fail,
// and under `set -e` that would have exited the shell.
TEST_F(ExecutorTest, WaitWithNoOperandsIsAlwaysZero) {
	EXPECT_EQ(run("false & wait"), 0);
}

TEST_F(ExecutorTest, WaitOnANonChildReportsTheStatusPosixDefines) {
	// 127, and no diagnostic: this is a specified result, not a failure.
	EXPECT_EQ(run("wait 99999"), 127);
}

// --- exec --------------------------------------------------------------------
//
// `exec` was in kSpecialBuiltins with no handler anywhere, so try_run_builtin
// found nothing and reported success: `exec echo hi; echo notreached` printed
// only `notreached`. Every case below is a behaviour that stub silently had
// wrong. The replacing cases run inside a subshell on purpose - an `exec` at the
// top level of these tests would replace the test binary itself.

TEST_F(ExecutorTest, ExecWithNoCommandKeepsTheShellRunning) {
	// Only the redirections happen, and the shell carries on. Reaching `exit 42`
	// is the proof; a shell that had been replaced could not.
	EXPECT_EQ(run("exec; exit 42"), 42);
}

TEST_F(ExecutorTest, ExecReplacesTheProcessRatherThanForking) {
	// The `exit 42` after it never runs, which is the only observable difference
	// between replacing this process and spawning a child.
	EXPECT_EQ(run("(exec /usr/bin/true; exit 42)"), 0);
	EXPECT_EQ(run("(exec /usr/bin/false; exit 42)"), 1);
}

TEST_F(ExecutorTest, ExecRedirectionOutlivesTheCommand) {
	// `exec >file` is how a script redirects itself for good, and it is the one
	// place where "apply without saving" is observable. Inside a subshell so the
	// test binary's own stdout survives.
	const std::string path = ::testing::TempDir() + "lesh_exec_redirect.txt";
	ASSERT_EQ(run("(exec >" + path + "; /bin/echo written)"), 0);
	std::ifstream written{path};
	std::string line;
	std::getline(written, line);
	EXPECT_EQ(line, "written") << "the redirection did not outlive the exec";
	std::remove(path.c_str());
}

TEST_F(ExecutorTest, ExecFailureExitsANonInteractiveShell) {
	// POSIX: 127 not found, 126 found but not executable, and a non-interactive
	// shell EXITS - so `exit 42` must be unreachable in both.
	EXPECT_EQ(run("exec /nonexistent/command/xyz; exit 42"), 127);
	EXPECT_EQ(run("exec /etc/hosts; exit 42"), 126);
}

TEST_F(ExecutorTest, ExecFailureLeavesAnInteractiveShellAlive) {
	// The other half of the same POSIX sentence, and the half dash gets wrong: it
	// exits interactively too. exec-p.tst asserts the shell survives and reports
	// 127, so lesh follows the standard rather than the reference shell.
	state.set_interactive(true);
	EXPECT_EQ(run("exec /nonexistent/command/xyz; exit 42"), 42);
}

TEST_F(ExecutorTest, ExecRedirectionFailureIsFatalToANonInteractiveShell) {
	// A redirection error on a SPECIAL builtin kills a non-interactive shell.
	// dash exits 2 here, for both a missing file and a closed fd.
	EXPECT_EQ(run("exec </nonexistent/file/xyz; exit 42"), 2);
	EXPECT_EQ(run("exec >&9; exit 42"), 2);
}

TEST_F(ExecutorTest, CommandPrefixDemotesExecSoARedirectionFailureIsSurvivable) {
	// POSIX: `command` makes a special builtin behave like a regular one, which
	// removes the "exits the shell" half. exec-p.tst's 'redirection error on exec'
	// case tests exactly this and requires a status of 1..125.
	EXPECT_EQ(run("command exec </nonexistent/file/xyz; exit 42"), 42);
}

TEST_F(ExecutorTest, ExecAssignmentPersistsWhenThereIsNoCommand) {
	// exec is a special builtin, so a prefixing assignment outlives it. dash
	// leaves it visible to the shell and absent from `env`, so it is set, not
	// exported.
	run("PERSISTED=yes exec");
	std::string_view value;
	ASSERT_TRUE(state.lookup("PERSISTED", value));
	EXPECT_EQ(value, "yes");
}

TEST_F(ExecutorTest, ExecAcceptsTheDoubleDashSeparator) {
	// POSIX XBD 12.2 guideline 10. dash does NOT accept it - `exec -- echo hi`
	// reports `exec: --: not found` - and fails both `-- separator` cases in
	// exec-p.tst. A deliberate divergence from the reference shell.
	EXPECT_EQ(run("exec --; exit 42"), 42);
	EXPECT_EQ(run("(exec -- /usr/bin/false; exit 42)"), 1);
}

TEST_F(ExecutorTest, ExecInAPipelineStageReplacesOnlyThatStage) {
	// The stage's own process is replaced and the SHELL survives, so `exit 42`
	// still runs. Without the dispatch in run_pipeline this printed nothing at
	// all: try_run_builtin has no `exec` entry and reported success.
	EXPECT_EQ(run("/usr/bin/true | exec /usr/bin/false"), 1);
	EXPECT_EQ(run("/usr/bin/true | exec /usr/bin/false; exit 42"), 42);
}
