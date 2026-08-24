#include "runtime/executor.h"

#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include "interactive_signal_guard.h"

#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>

#include <unistd.h>

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

	// The standard output of `src`, taken through a FILE rather than a pipe: an
	// external command in the source writes to fd 1 directly, so intercepting
	// stdio would not see it.
	std::string capture(std::string_view src) {
		const std::string path = ::testing::TempDir() + "lesh_executor_capture.txt";
		std::remove(path.c_str());
		std::string wrapped{"{ "};
		wrapped.append(src);
		wrapped += "; } > ";
		wrapped += path;
		(void)run(wrapped);
		std::ifstream in{path};
		const std::string out{std::istreambuf_iterator<char>{in},
		                      std::istreambuf_iterator<char>{}};
		std::remove(path.c_str());
		return out;
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
	std::ignore = state.set("MY_VAR", "my value");
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
		std::ignore = state.set("OWNED", temporary);
	}
	std::string_view value;
	ASSERT_TRUE(state.lookup("OWNED", value));
	EXPECT_EQ(value, "value that goes away");
}

TEST(ShellState, AliasesAreListedSortedByName) {
	// Sorted because the map is unordered and POSIX leaves the order unspecified: a
	// stable order is what makes the listing diffable, which is what alias-p.tst's
	// `alias >save_1; unalias -a; eval alias $(cat save_1); alias >save_2; diff`
	// round trip asks of it.
	shell_state state;
	state.set_alias("c", "3");
	state.set_alias("a", "1");
	state.set_alias("b", "2");
	const auto rows = state.aliases();
	ASSERT_EQ(rows.size(), 3u);
	EXPECT_EQ(rows[0].name, "a");
	EXPECT_EQ(rows[1].name, "b");
	EXPECT_EQ(rows[2].name, "c");
	EXPECT_EQ(rows[0].value, "1");
}

TEST(ShellState, UnsetAliasSaysWhetherThereWasOne) {
	// POSIX makes `unalias nosuch` an ERROR, and the builtin cannot report one it
	// is not told about: unalias used to return 0 whatever happened.
	shell_state state;
	state.set_alias("a", "1");
	EXPECT_TRUE(state.unset_alias("a"));
	EXPECT_FALSE(state.unset_alias("a")) << "the second removal has nothing to remove";
	EXPECT_FALSE(state.unset_alias("never-defined"));
}

TEST(ShellState, ClearAliasesRemovesEveryOne) {
	// `unalias -a`, which used to be looked up as an alias NAMED `-a` - removing
	// nothing, reporting nothing, and passing its conformance case anyway because
	// the listing it was compared against printed nothing either.
	shell_state state;
	state.set_alias("a", "1");
	state.set_alias("b", "2");
	state.clear_aliases();
	EXPECT_FALSE(state.has_aliases());
	EXPECT_TRUE(state.aliases().empty());
}

TEST(ShellState, UnsetRemoves) {
	shell_state state;
	std::ignore = state.set("GONE", "here");
	std::ignore = state.unset("GONE");
	std::string_view value;
	EXPECT_FALSE(state.lookup("GONE", value));
}

TEST(ShellState, OnlyExportedVariablesReachTheEnvironmentBlock) {
	shell_state state;
	std::ignore = state.set("LOCAL_ONLY", "no");
	std::ignore = state.set_exported("EXPORTED", "yes");

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
	std::ignore = state.unset("IFS");
	EXPECT_EQ(state.ifs(), " \t\n");
}

TEST(ShellState, IfsSetToEmptyMeansNoSplitting) {
	// Distinct from unset. POSIX: IFS="" disables field splitting entirely, which
	// is why ifs() returns the value rather than falling back on emptiness.
	shell_state state;
	std::ignore = state.set("IFS", "");
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
	//
	// The guard puts the interactive SIGNAL defaults back afterwards (#52); this
	// case also exercises the pair of them, because `exec` drops those dispositions
	// before execve and has to restore them when there is nothing to become.
	const lesh::testing::interactive_disposition_guard dispositions;
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

// --- redirection file descriptors (#34) ---------------------------------------
//
// apply_redirection remembered a displaced fd with dup(2). A fd that was CLOSED
// gave it nothing to remember, so nothing was pushed onto the saved_fd array and
// restore_fds never closed it again - the descriptor a redirection opened outlived
// the construct that opened it. saved_fd::closed is the sentinel that fixes it,
// and every case below failed before it existed.

TEST_F(ExecutorTest, ARedirectionOnAGroupingClosesTheFdItOpened) {
	// The bug in one line: `3>&2` opens fd 3 for the group, the group ends, and
	// `>&3` afterwards must fail. It printed `foo` and reported success.
	EXPECT_EQ(run("{ :; } 3>&2; /bin/echo foo >&3"), 2);
}

TEST_F(ExecutorTest, ARedirectionOnALoopClosesTheFdItOpened) {
	// Loops reach the same save/restore path through the synthesised brace group,
	// so they leaked identically.
	EXPECT_EQ(run("for i in 1; do :; done 3>&2; /bin/echo x >&3"), 2);
	EXPECT_EQ(run("while false; do :; done 3>&2; /bin/echo x >&3"), 2);
}

TEST_F(ExecutorTest, ARedirectionOnAFunctionCallClosesTheFdItOpened) {
	EXPECT_EQ(run("f() { :; }; f 3>&2; /bin/echo x >&3"), 2);
}

TEST_F(ExecutorTest, ARedirectionOnABuiltinClosesTheFdItOpened) {
	EXPECT_EQ(run("cd . 3>&2; /bin/echo x >&3"), 2);
}

TEST_F(ExecutorTest, ADisplacedFdThatWasOpenIsPutBackRatherThanClosed) {
	// The other half of the sentinel: an fd that WAS open must come back, so
	// distinguishing the two cases cannot be done by always closing either.
	const std::string path = ::testing::TempDir() + "lesh_restore_fd.txt";
	ASSERT_EQ(run("(exec 3>" + path + "; { /bin/echo inner >&3; } 3>&2; "
	              "/bin/echo outer >&3)"), 0);
	std::ifstream written{path};
	std::string first;
	std::getline(written, first);
	EXPECT_EQ(first, "outer") << "fd 3 was not restored to the file exec opened";
	std::remove(path.c_str());
}

TEST_F(ExecutorTest, TheSavedCopyIsOutOfReachOfALaterRedirection) {
	// The copy is taken at fd 10 or above. dup(2) returns the LOWEST free fd,
	// which `4>&3` in the same command then overwrote - so the restore put back
	// whatever fd 4 had been aimed at instead of the original fd 3.
	const std::string path = ::testing::TempDir() + "lesh_saved_copy.txt";
	ASSERT_EQ(run("(exec 3>" + path + "; { :; } 3>&2 4>&3; "
	              "/bin/echo outer >&3)"), 0);
	std::ifstream written{path};
	std::string first;
	std::getline(written, first);
	EXPECT_EQ(first, "outer") << "the saved copy of fd 3 was clobbered by 4>&3";
	std::remove(path.c_str());
}

TEST_F(ExecutorTest, ARedirectionFailureReportsTwo) {
	// dash answers 2 in every position; lesh answered 1, which is also what a
	// command that ran and failed reports.
	EXPECT_EQ(run("/bin/echo x < /_lesh_no_such_file_"), 2);
	EXPECT_EQ(run("cd . < /_lesh_no_such_file_"), 2);
	EXPECT_EQ(run("{ :; } < /_lesh_no_such_file_"), 2);
	EXPECT_EQ(run("f() { :; }; f < /_lesh_no_such_file_"), 2);
	EXPECT_EQ(run("/bin/echo x >&9"), 2);
}

TEST_F(ExecutorTest, ARedirectionFailureOnASpecialBuiltinExitsANonInteractiveShell) {
	// POSIX 2.8.1, and dash agrees: `: </missing` exits 2 without reaching the
	// next command, while the same redirection on a regular builtin does not.
	EXPECT_EQ(run(": < /_lesh_no_such_file_; exit 42"), 2);
	EXPECT_EQ(run("eval : < /_lesh_no_such_file_; exit 42"), 2);
	EXPECT_EQ(run("cd . < /_lesh_no_such_file_; exit 42"), 42);
}

TEST_F(ExecutorTest, CommandDemotesASpecialBuiltinSoARedirectionFailureIsSurvivable) {
	EXPECT_EQ(run("command : < /_lesh_no_such_file_; exit 42"), 42);
}

TEST_F(ExecutorTest, ClosingAnFdThatWasNeverOpenIsNotAnError) {
	// `5>&-` on a closed fd 5 is a no-op, not a failure - and the sentinel must
	// not turn it into one by trying to restore something that never existed.
	EXPECT_EQ(run("/bin/echo a 5>&- 6<&-"), 0);
}

TEST_F(ExecutorTest, ClosingStdoutMakesABuiltinThatWritesFail) {
	// dash reports `echo: I/O error` and 1. lesh reported success AND printed the
	// bytes later, because a failed flush leaves them in the stdio buffer and the
	// next flush ran after restore_fds had put the shell's own fd 1 back.
	EXPECT_EQ(run("cd . >&-"), 0) << "a builtin that writes nothing cannot fail";
	EXPECT_EQ(run("echo x >&-"), 1) << "a builtin that writes must notice it could not";
	EXPECT_EQ(run("echo after-the-failure"), 0)
		<< "the shell's own fd 1 was not put back";
}

TEST_F(ExecutorTest, ACommandThatIsOnlyRedirectionsStillPerformsThem) {
	// POSIX 2.9.1: no command name does not mean no redirections. lesh dropped
	// them entirely, so `>file` never created the file and `<missing` never failed.
	const std::string path = ::testing::TempDir() + "lesh_bare_redirect.txt";
	std::remove(path.c_str());
	EXPECT_EQ(run("> " + path), 0);
	std::ifstream created{path};
	EXPECT_TRUE(created.good()) << "a bare `>file` did not create the file";
	std::remove(path.c_str());
	EXPECT_EQ(run("< /_lesh_no_such_file_"), 2);
}

TEST_F(ExecutorTest, RedirectionsWithNoCommandNameRunInASubshell) {
	// POSIX 2.9.1 performs them in a subshell environment, so an assignment made
	// while expanding the operand is not visible afterwards. dash performs them in
	// the current environment and fails redir-p.tst for it.
	run("< ${leaked=no/such/file}");
	std::string_view value;
	EXPECT_FALSE(state.lookup("leaked", value))
		<< "the operand was expanded in the shell rather than in a subshell";
}

TEST_F(ExecutorTest, AnAssignmentIsSkippedWhenItsRedirectionFails) {
	// dash leaves the variable unset after `x=1 </missing` and sets it after
	// `x=1 </dev/null`, so the redirection gates the assignment.
	run("kept=yes < /dev/null");
	run("dropped=yes < /_lesh_no_such_file_");
	std::string_view value;
	EXPECT_TRUE(state.lookup("kept", value));
	EXPECT_FALSE(state.lookup("dropped", value));
}

TEST_F(ExecutorTest, AHereDocumentGoesToTheFdItWasWrittenFor) {
	// `3<<END` feeds fd 3, not stdin. The fd was hardcoded to STDIN_FILENO, so
	// `cat 3<<END <&3` read the terminal and several here-documents on one command
	// all landed on stdin, leaving only the last one readable.
	EXPECT_EQ(run("/bin/cat 3<<END <&3 >/dev/null\nfoo\nEND\n"), 0);
	EXPECT_EQ(run("{ /bin/cat <&4; /bin/cat <&3; } <<A 3<<B 4<<C >/dev/null\n"
	              "zero\nA\nthree\nB\nfour\nC\n"), 0);
}

TEST_F(ExecutorTest, DuplicatingADescriptorChecksItsAccessMode) {
	// POSIX 2.7.5/2.7.6: `>&n` requires n open for output and `<&n` for input.
	// dup2 cannot tell, so `3</dev/null >&3` aimed a read-only fd at stdout and
	// succeeded - in lesh and in dash, which fails redir-p.tst for it.
	EXPECT_EQ(run("{ :; } 3</dev/null >&3"), 2);
	EXPECT_EQ(run("{ :; } 3>/dev/null <&3"), 2);
	EXPECT_EQ(run("{ :; } 3</dev/null <&3"), 0);
	EXPECT_EQ(run("{ :; } 3>/dev/null >&3"), 0);
}

namespace {

// The hang in issue #50, asserted without a terminal.
//
// A pipeline stage's words were expanded in the SHELL, before the stage's child
// had the pipe on fd 0, so a command substitution in them read the shell's own
// standard input. Every runner in this project passes /dev/null there - probe.py
// and spec_run.py both do, deliberately - and against /dev/null the bug is a
// wrong answer: an empty string instead of the piped data. On a TERMINAL the same
// read blocks and the shell hangs, which is how the bug was found and what it
// cost two sessions on the #17 map.
//
// The differential corpus cannot express that, so this asserts the CAUSE instead
// of the symptom: fd 0 carries bytes the stage must never see, and they must
// still be unread afterwards. An fd 0 that no stage reads is exactly the property
// that makes the terminal case terminate.
class PipelineStdinTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	shell_state state;
	int _original_stdin = -1;

	void SetUp() override { _original_stdin = ::dup(STDIN_FILENO); }

	void TearDown() override {
		if (_original_stdin >= 0) {
			::dup2(_original_stdin, STDIN_FILENO);
			::close(_original_stdin);
		}
	}

	// Puts bytes on fd 0 that no pipeline stage is allowed to read. The write end
	// is CLOSED: a stage that reads them anyway then sees EOF rather than blocking,
	// because a test that hangs when it fails is the same failure mode it is here
	// to prevent. What is left on fd 0 is the verdict.
	void feed_stdin(std::string_view text) {
		int fds[2] = {-1, -1};
		ASSERT_EQ(::pipe(fds), 0);
		ASSERT_EQ(::write(fds[1], text.data(), text.size()),
		          static_cast<ssize_t>(text.size()));
		::close(fds[1]);
		ASSERT_EQ(::dup2(fds[0], STDIN_FILENO), STDIN_FILENO);
		::close(fds[0]);
	}

	std::string unread_stdin() {
		char buffer[64] = {};
		const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
		return got > 0 ? std::string{buffer, static_cast<size_t>(got)} : std::string{};
	}

	// Runs `code` with its output in a file, because the shell writes to fd 1 and
	// the test needs to read what it wrote.
	std::string output_of(const std::string& code) {
		const std::string path = ::testing::TempDir() + "lesh_pipeline_stage_stdin.txt";
		std::remove(path.c_str());
		// The source must OUTLIVE the tree: the nodes hold views into it, not copies.
		const std::string source = "{ " + code + "; } > " + path;
		const tree t = parse(pool, source);
		tree_walking_executor ex{pool, state};
		std::ignore = ex.run(t);
		std::ifstream in{path};
		std::string out{std::istreambuf_iterator<char>{in},
		                std::istreambuf_iterator<char>{}};
		std::remove(path.c_str());
		return out;
	}
};

} // namespace

TEST_F(PipelineStdinTest, ACommandSubstitutionInAStageReadsThePipeNotTheShellsStdin) {
	feed_stdin("TERMINAL\n");
	EXPECT_EQ(output_of("echo a | echo $(cat)"), "a\n");
	EXPECT_EQ(unread_stdin(), "TERMINAL\n")
		<< "the substitution read the shell's fd 0 - on a terminal that is the hang";
}

TEST_F(PipelineStdinTest, AnExternalStagesSubstitutionReadsThePipeNotTheShellsStdin) {
	// The external path forks and execs; the builtin path stays in the stage's own
	// process. Both expanded in the parent, so both have to be asserted.
	feed_stdin("TERMINAL\n");
	EXPECT_EQ(output_of("echo a | /bin/echo $(cat)"), "a\n");
	EXPECT_EQ(unread_stdin(), "TERMINAL\n");
}

TEST_F(PipelineStdinTest, ASubstitutionInsideArithmeticInAStageReadsThePipe) {
	// `$(( $(cat) + 1 ))` reached the same expander from a different entry point,
	// and answered 1 - arithmetic on the empty string - rather than hanging only
	// once the terminal case was tried.
	feed_stdin("TERMINAL\n");
	EXPECT_EQ(output_of("echo 3 | echo $(( $(cat) + 1 ))"), "4\n");
	EXPECT_EQ(unread_stdin(), "TERMINAL\n");
}

// Assignment prefixes (#31). Four paths apply a prefix - a function call, a
// builtin in the table, one of the executor's own, and `exec` - and two of them
// did not apply it at all, while a fifth expanded it in the wrong process.
TEST_F(ExecutorTest, ACommandSubstitutionInAPrefixReachesTheCommand) {
	// The child expanded the prefix itself, with an expander built with NO command
	// runner, so a substitution in a prefix value had nothing to run it and the
	// variable was exported empty. It is the shell that read the command which owes
	// the expansion, POSIX 2.9.1.
	EXPECT_EQ(capture("x=$(echo z) /bin/sh -c 'echo [$x]'"), "[z]\n");
	EXPECT_EQ(capture("echo body | x=$(cat) /bin/sh -c 'echo [$x]'"), "[body]\n");
}

TEST_F(ExecutorTest, APrefixOnEvalAndDotIsVisibleAndPersists) {
	// try_run_executor_builtin was handed the assignments and passed them on to
	// `exec` alone, so neither `eval` nor `.` could see its own prefix.
	EXPECT_EQ(capture("x=1 eval 'echo in=[$x]'"), "in=[1]\n");
	std::string_view value;
	// Both are SPECIAL builtins, so POSIX makes the prefix persist rather than be
	// restored. dash prints 1 for `x=1 . /dev/null; echo $x`.
	EXPECT_EQ(run("kept=yes eval ':'"), 0);
	ASSERT_TRUE(state.lookup("kept", value));
	EXPECT_EQ(value, "yes");
	EXPECT_EQ(run("dotted=yes . /dev/null"), 0);
	ASSERT_TRUE(state.lookup("dotted", value));
	EXPECT_EQ(value, "yes");
}

TEST_F(ExecutorTest, CommandDemotesASpecialBuiltinSoItsPrefixIsRestored) {
	// `command` makes a special builtin behave like a regular one, and the
	// assignment follows the demotion: command-p.tst's 'assignment on special
	// built-in is temporary' is `a=a; a=b command :; echo $a` expecting `a`.
	std::ignore = state.set("a", "a");
	EXPECT_EQ(run("a=b command :"), 0);
	std::string_view value;
	ASSERT_TRUE(state.lookup("a", value));
	EXPECT_EQ(value, "a") << "the prefix outlived a builtin `command` had demoted";
	EXPECT_EQ(capture("x=1 command eval 'echo in=[$x]'"), "in=[1]\n")
		<< "demoted or not, the builtin still has to SEE its prefix";
	EXPECT_FALSE(state.lookup("x", value));
	// `exec` with no command is the same rule: it persists, unless demoted.
	EXPECT_EQ(run("gone=yes command exec"), 0);
	EXPECT_FALSE(state.lookup("gone", value));
}

TEST_F(ExecutorTest, APrefixOnARegularExecutorBuiltinIsRestored) {
	// `wait` is regular, so its prefix lasts only for the command - the same rule
	// as `cd`, reached through a different dispatch.
	EXPECT_EQ(run("temp=yes wait"), 0);
	std::string_view value;
	EXPECT_FALSE(state.lookup("temp", value));
}

// --- `--` ends the options of an operand-only special builtin -----------------
//
// POSIX XCU 1.4: a utility that takes operands and no options recognises `--` as
// a first argument to be discarded. `break`, `continue`, `.`, `eval`, `exit` and
// `return` are all of that shape and every one read argv[1] as its operand, so
// `return -- 56` returned 0 and `eval -- 'echo foo'` looked for a command named
// `--`. dash rejects all four and fails the case in each of return-p.tst,
// exit-p.tst and eval-p.tst; the divergence is deliberate.

TEST_F(ExecutorTest, TheSeparatorPrecedesTheOperandOfExitAndReturn) {
	EXPECT_EQ(run("exit -- 56"), 56);
	EXPECT_EQ(run("f() { return -- 56; }; f"), 56);
	// With nothing after it the separator leaves the DEFAULT operand, not an
	// operand of `--`: `exit --` is `exit`.
	state.set_last_status(9);
	EXPECT_EQ(run("(exit 41); exit --"), 41);
}

TEST_F(ExecutorTest, TheSeparatorPrecedesTheOperandOfEvalAndDot) {
	EXPECT_EQ(capture("eval -- 'echo foo'"), "foo\n");
	EXPECT_EQ(run("eval -- 'exit 23'"), 23);
	// `.` too: dot-p.tst's 'option-operand separator' is `. -- ./file1`. A real
	// script rather than /dev/null, so the assertion is that the file after the
	// separator was READ and not merely that nothing complained.
	const std::string script = ::testing::TempDir() + "lesh_dot_separator.sh";
	{
		std::ofstream out{script};
		out << "exit 23\n";
	}
	EXPECT_EQ(run(". -- " + script), 23);
	std::remove(script.c_str());
	// Only the FIRST `--` is discarded, so a second one is an operand - here a
	// filename that does not exist.
	EXPECT_NE(run(". -- -- 2>/dev/null"), 0);
}

// --- control flow unwinds before the operator that follows it ------------------

TEST_F(ExecutorTest, ABreakBeforeAndDoesNotRunTheRightHandSide) {
	// `break` reports 0, and 0 is exactly what `&&` continues on, so reading the
	// left operand's STATUS alone ran the echo. break-p.tst's 'breaking before &&'.
	EXPECT_EQ(capture("for i in 1; do break && echo reached1; echo reached2; done"), "");
	EXPECT_EQ(capture("for i in 1; do continue && echo reached1; echo reached2; done"),
	          "");
	EXPECT_EQ(capture("f() { return && echo reached1; echo reached2; }; f"), "");
	// The `||` forms passed already, 0 being what `||` stops on - kept so a fix
	// that swapped the two operators would be caught.
	EXPECT_EQ(capture("for i in 1; do break || echo reached1; echo reached2; done"), "");
	EXPECT_EQ(capture("f() { return || echo reached1; echo reached2; }; f"), "");
	// `exit` unwinds through the operator too, and there the status is visible.
	EXPECT_EQ(run("exit 5 && echo reached"), 5);
}

TEST_F(ExecutorTest, AnUnwindBeforeAndKeepsTheStatusTheBuiltinReported) {
	// The and-or list reports the LEFT operand, not zero from a right-hand side it
	// never ran. `break` and a bare `return` both report 0, so a non-zero `return`
	// is what distinguishes the two.
	EXPECT_EQ(run("f() { return 7 && echo reached; }; f"), 7);
	EXPECT_EQ(capture("f() { return 7 && echo reached; }; f; echo st=$?"), "st=7\n");
}

// --- `break 0` is an error, not a no-op ---------------------------------------

TEST_F(ExecutorTest, AZeroOperandToBreakIsAnError) {
	// Zero levels is what consume_loop_flow reads as "already arrived", so the
	// break vanished and the loop ran on into `echo reached`. break-p.tst's
	// 'zero operand' wants a diagnostic and a non-zero status; dash exits 2.
	EXPECT_EQ(run("for i in 1; do break 0; done 2>/dev/null"), 2);
	EXPECT_EQ(run("for i in 1; do continue 0; done 2>/dev/null"), 2);
	EXPECT_EQ(capture("for i in 1; do break 0; echo reached; done 2>/dev/null"), "");
	// A special builtin's failure exits a non-interactive shell, so nothing after
	// the loop runs either.
	EXPECT_EQ(capture("for i in 1; do break 0; done 2>/dev/null; echo after"), "");
}

TEST_F(ExecutorTest, ANonNumericOrNegativeOperandToBreakIsAnError) {
	// atoi answered 0 for `x` and -1 for `-1`, and a negative level would make
	// `--_flow_level <= 0` true at the first loop - indistinguishable from 1.
	EXPECT_EQ(run("for i in 1; do break x; done 2>/dev/null"), 2);
	EXPECT_EQ(run("for i in 1; do break -1; done 2>/dev/null"), 2);
	EXPECT_EQ(run("for i in 1; do continue 1x; done 2>/dev/null"), 2);
}

TEST_F(ExecutorTest, ALevelPastTheNestingDepthStillBreaksOutOfTheLoop) {
	// The clamp must not turn a huge operand back into the zero this refuses.
	// break-p.tst's 'breaking much more than actual nest level one'.
	EXPECT_EQ(capture("for i in 1; do break 100; echo reached; done"), "");
	EXPECT_EQ(capture("for i in 1; do break 99999999999999; echo reached; done"), "");
}

// --- nothing to run is status zero --------------------------------------------

TEST_F(ExecutorTest, EvalWithNothingToRunReportsZero) {
	// run_source started from the CALLER's `$?`, so `eval` on blank text reported
	// the status of whatever ran before it. eval-p.tst's 'evaluating null
	// operands' is `false; eval '' '' ''` expecting 0.
	EXPECT_EQ(run("(exit 1); eval '' '' ''"), 0);
	EXPECT_EQ(run("(exit 1); eval ''"), 0);
	EXPECT_EQ(run("(exit 1); eval"), 0);
	EXPECT_EQ(run("(exit 1); eval '# only a comment'"), 0);
	// A command that DID run still decides, so this is not a blanket zero.
	EXPECT_EQ(run("eval 'exit 3'"), 3);
}

TEST_F(ExecutorTest, DottingAFileWithNoCommandsReportsZero) {
	// dot-p.tst's 'empty dot script': `(exit 1); . /dev/null` expects 0.
	EXPECT_EQ(run("(exit 1); . /dev/null"), 0);
	// And `$?` inside the script is still the caller's, which is what dot-p.tst's
	// 'non-empty dot script' asserts - the initial status is the value RETURNED
	// when nothing runs, not a write to `$?`.
	const std::string script = ::testing::TempDir() + "lesh_dot_status.sh";
	{
		std::ofstream out{script};
		out << "echo $?\n";
	}
	EXPECT_EQ(capture("(exit 5); . " + script), "5\n");
	std::remove(script.c_str());
}

// --- `.` searches $PATH, and its failure is fatal (dot-p.tst) ------------------

TEST_F(ExecutorTest, DotSearchesPathForAnOperandWithNoSlash) {
	// run_file fopen()ed the operand, which searches the WORKING DIRECTORY.
	// dot-p.tst's 'dot script in $PATH' passed anyway because the case sets
	// `PATH=$PWD` and the two answers coincide there.
	const std::string dir = ::testing::TempDir() + "lesh_dot_path";
	ASSERT_EQ(run("mkdir -p " + dir), 0);
	const std::string script = dir + "/on_the_path";
	{
		std::ofstream out{script};
		out << "exit 11\n";
	}
	std::ignore = state.set("PATH", dir);
	EXPECT_EQ(run(". on_the_path"), 11);
	// A dot script need only be READABLE, never executable: sharing the command
	// search's X_OK test would break `. lib.sh` on every library ever written.
	EXPECT_EQ(run("chmod 444 " + script + "; . on_the_path"), 11);
	// And the search does NOT fall back to the working directory, which is what
	// dash does. `.` with a PATH that does not name it must not find the file.
	std::ignore = state.set("PATH", "/nonexistent");
	EXPECT_NE(run(". on_the_path 2>/dev/null"), 0);
	std::remove(script.c_str());
}

TEST_F(ExecutorTest, DotFailingToFindItsScriptExitsANonInteractiveShell) {
	// `.` is a SPECIAL builtin, so the rule #34 established for a redirection
	// failure applies: `. _no_such_file_; echo not reached` printed `not reached`.
	// dot-p.tst's 'dot script not found' cases, in $PATH and relative.
	EXPECT_EQ(capture(". _no_such_file_ 2>/dev/null; echo not reached"), "");
	EXPECT_EQ(capture(". ./_no_such_file_ 2>/dev/null; echo not reached"), "");
	EXPECT_EQ(capture(". 2>/dev/null; echo not reached"), "");
	// 2, which is what dash answers; the operand of `.` is not a command name, so
	// the 127 lesh reported was the status of a search that never happened.
	EXPECT_EQ(run(". ./_no_such_file_ 2>/dev/null"), 2);
	// A SUBSHELL exits and the shell that forked it carries on.
	EXPECT_EQ(capture("(. ./_no_such_file_ 2>/dev/null); echo reached"), "reached\n");
	// A status the SCRIPT reported is not a search failure and must not be fatal.
	const std::string script = ::testing::TempDir() + "lesh_dot_fatal.sh";
	{
		std::ofstream out{script};
		out << "(exit 3)\n";
	}
	EXPECT_EQ(capture(". " + script + "; echo reached=$?"), "reached=3\n");
	std::remove(script.c_str());
}

TEST_F(ExecutorTest, DotFailingIsSurvivableInAnInteractiveShellAndUnderCommand) {
	// An interactive shell reports and carries on - dot-p.tst asserts both halves
	// separately - and `command .` demotes the special builtin, which is the same
	// distinction the redirection-failure path makes.
	EXPECT_EQ(capture("command . ./_no_such_file_ 2>/dev/null; echo reached"),
	          "reached\n");
	const lesh::testing::interactive_disposition_guard dispositions;
	state.set_interactive(true);
	EXPECT_EQ(capture(". ./_no_such_file_ 2>/dev/null; echo reached"), "reached\n");
}

// --- a dot script is a return boundary ---------------------------------------

TEST_F(ExecutorTest, ReturnFromADotScriptStopsAtThatScript) {
	// POSIX XCU `return`: the unwind ends at the innermost function OR dot script.
	// It ended at neither here - `return` in a sourced script unwound past every
	// enclosing one, which is return-p.tst's 'returning from dot script, nested in
	// another dot script' and 'nested in function'.
	const std::string dir = ::testing::TempDir();
	const std::string inner = dir + "lesh_return_inner.sh";
	const std::string outer = dir + "lesh_return_outer.sh";
	{
		std::ofstream out{inner};
		out << "echo in inner\nreturn\necho not reached\n";
	}
	{
		std::ofstream out{outer};
		out << "echo in outer\n. " << inner << "\necho out outer\n";
	}
	EXPECT_EQ(capture(". " + outer + "; echo after"),
	          "in outer\nin inner\nout outer\nafter\n");
	EXPECT_EQ(capture("f() { echo in f; . " + inner + "; echo out f; }; f; echo after"),
	          "in f\nin inner\nout f\nafter\n");
	std::remove(inner.c_str());
	std::remove(outer.c_str());
}

TEST_F(ExecutorTest, ReturnFromADotScriptStillReportsItsOwnStatus) {
	// The boundary consumes the UNWIND and not the status: return-p.tst's
	// 'specifying exit status in returning from dot script' expects 17.
	const std::string script = ::testing::TempDir() + "lesh_return_status.sh";
	{
		std::ofstream out{script};
		out << "(exit 1)\nreturn 17\n";
	}
	EXPECT_EQ(run(". " + script), 17);
	std::remove(script.c_str());
}

TEST_F(ExecutorTest, EvalIsNotAReturnBoundary) {
	// `eval` shares run_source with `.` and must NOT stop the unwind:
	// return-p.tst's 'returning out of eval' has the function return, not the eval.
	EXPECT_EQ(capture("f() { eval return; echo not reached; }; f; echo after"),
	          "after\n");
}

// --- a return outside a function ends the input -------------------------------

TEST_F(ExecutorTest, ReturnOutsideAFunctionEndsTheInput) {
	// POSIX leaves `return` with no function and no dot script around it
	// unspecified. dash and zsh both END THE INPUT with the status it asked for -
	// `return; echo x` prints nothing in either - and lesh ran the next command,
	// so the `return` reported a status and did nothing else.
	EXPECT_EQ(capture("return; echo x"), "");
	EXPECT_EQ(capture("return 7; echo x"), "");
	EXPECT_EQ(run("return 7; echo x"), 7);
	// Through a brace group and through a loop, both of which already let the
	// unwind out - what was missing was anything to stop at the top.
	EXPECT_EQ(capture("{ return 7; echo x; }; echo y"), "");
	EXPECT_EQ(capture("while :; do return 4; done; echo x"), "");
	EXPECT_EQ(run("while :; do return 4; done; echo x"), 4);
	// `eval` is not a boundary, so a return inside one ends the input too.
	EXPECT_EQ(capture("eval return 4; echo x"), "");
}

TEST_F(ExecutorTest, ReturnInsideAFunctionStillReturnsFromTheFunction) {
	// The guard on the rule above: the boundary that already existed must still
	// win, or every `return` would end the script.
	EXPECT_EQ(capture("f() { return 5; }; f; echo after=$?"), "after=5\n");
	EXPECT_EQ(capture("f() { return; echo not reached; }; f; echo after"), "after\n");
}

// --- break and continue outside a loop ---------------------------------------

TEST_F(ExecutorTest, ABreakWithNoEnclosingLoopDoesNothing) {
	// POSIX leaves it unspecified and dash makes it a silent no-op. lesh unwound,
	// which was invisible at the top level - `break; echo x` printed x either way -
	// and visible one construct in: `{ break; echo x; }` printed NOTHING, because
	// the brace group ended for an unwind no loop was waiting behind.
	EXPECT_EQ(capture("break; echo x"), "x\n");
	EXPECT_EQ(capture("{ break; echo x; }"), "x\n");
	EXPECT_EQ(capture("continue; echo x"), "x\n");
	EXPECT_EQ(capture("{ continue; echo x; }"), "x\n");
	EXPECT_EQ(capture("if :; then break; echo x; fi"), "x\n");
	// The operand is still checked: `break 0` is an error whether or not there is
	// a loop to break, which is the rule the previous fix in this area established.
	EXPECT_EQ(run("break 0 2>/dev/null"), 2);
}

TEST_F(ExecutorTest, AFunctionCallIsABoundaryForBreakAndContinue) {
	// dash: the loops the CALLER is inside are not loops the body is inside, so a
	// break in a function called from a loop does not break that loop. lesh broke
	// it, which is zsh's dynamic answer rather than the POSIX floor's (ADR-0001).
	EXPECT_EQ(capture("f() { break; }; for i in 1 2; do f; echo in; done; echo out"),
	          "in\nin\nout\n");
	EXPECT_EQ(capture("f() { continue; }; for i in 1 2; do f; echo in; done; echo out"),
	          "in\nin\nout\n");
	// Nor does a LEVEL travel out with it: `break 3` inside one loop in the body
	// stops at that loop and leaves the caller's alone.
	EXPECT_EQ(capture("f() { for i in 1 2; do break 3; done; echo in f; }; "
	                  "for j in 1 2; do f; echo body; done; echo out"),
	          "in f\nbody\nin f\nbody\nout\n");
	// A loop INSIDE the function is a loop the body is inside, so break works there.
	EXPECT_EQ(capture("f() { for i in 1 2; do break; echo no; done; echo in f; }; f"),
	          "in f\n");
}

TEST_F(ExecutorTest, BreakStopsAtTheOutermostLoopRatherThanEscapingIt) {
	// break-p.tst's 'breaking one more than actual nest level two': the echo
	// between the two loops must not run, and the shell carries on afterwards.
	EXPECT_EQ(capture("for i in 1; do for j in a; do break 3; echo n1; done; "
	                  "echo n2; done; echo after"),
	          "after\n");
	EXPECT_EQ(capture("for i in 1; do break 2; done; echo x"), "x\n");
	EXPECT_EQ(capture("for i in 1; do break 100; echo no; done; echo x"), "x\n");
}

TEST_F(ExecutorTest, DotAndEvalAreTransparentToBreak) {
	// dash keeps the count across both: a break in a dot script breaks the
	// CALLER's loop, and so does one inside an eval (break-p.tst's 'breaking out
	// of eval'). Only a function call is a boundary.
	EXPECT_EQ(capture("for i in 1 2; do eval break; echo body; done; echo after"),
	          "after\n");
	const std::string script = ::testing::TempDir() + "lesh_break_dot.sh";
	{
		std::ofstream out{script};
		out << "echo lib\nbreak\necho not reached\n";
	}
	EXPECT_EQ(capture("for i in 1 2; do . " + script + "; echo body; done; echo after"),
	          "lib\nafter\n");
	// And with no loop anywhere it is a no-op inside the script too, so the script
	// runs to its last line.
	EXPECT_EQ(capture(". " + script + "; echo caller"), "lib\nnot reached\ncaller\n");
	std::remove(script.c_str());
}

// --- exit inside a trap action ------------------------------------------------

TEST_F(ExecutorTest, TheExitTrapRunsItsWholeBodyAfterAnExit) {
	// `_exit_requested` already stood when the trap ran, and run_source stops at
	// the first command whenever it does - so `trap 'echo A; echo B' EXIT; exit 1`
	// printed A and not B. Half a cleanup handler is worse than none: the half
	// that did not run is the half that removes the temporary files.
	// In a SUBSHELL, because `capture` redirects a brace group and the shell's own
	// EXIT trap runs after that redirection is undone - the subshell's runs inside
	// it, and exercises the same code path.
	EXPECT_EQ(capture("( trap 'echo A; echo B' EXIT; exit 1 )"), "A\nB\n");
	EXPECT_EQ(capture("( trap 'echo A; echo B' EXIT; echo body )"), "body\nA\nB\n");
}

TEST_F(ExecutorTest, AnExitInTheExitTrapReplacesTheShellsStatus) {
	// exit-p.tst:50 and :55: the trap's `exit` wins over the one that reached it.
	EXPECT_EQ(run("trap 'exit 7' EXIT; exit 1"), 7);
	EXPECT_EQ(run("trap 'exit 0' EXIT; exit 1"), 0);
	// A body that merely RAN commands does not: exit-p.tst's 'exit status with
	// EXIT trap' leaves the 1 the shell was already exiting with.
	EXPECT_EQ(run("trap '(exit 2)' EXIT; (exit 1); exit"), 1);
}

TEST_F(ExecutorTest, ExitWithNoOperandInATrapReportsTheEntryStatus) {
	// POSIX XCU `exit`: inside a trap action "the last command" is the one that ran
	// immediately BEFORE the action, so the `(exit 1)` inside the body does not
	// decide - exit-p.tst's 'default exit status with previous command in trap in
	// exiting with default' expects 2.
	EXPECT_EQ(run("trap '(exit 1); exit' EXIT; (exit 2); exit"), 2);
	EXPECT_EQ(run("trap exit EXIT; (exit 2); exit"), 2);
	EXPECT_EQ(run("trap exit EXIT; exit 1"), 1);
	// And the body's own commands still update `$?` while it runs, which is why the
	// entry status is held separately rather than read out of `$?`.
	EXPECT_EQ(capture("( trap '(exit 2); echo $?' EXIT; exit 1 )"), "2\n");
}

TEST_F(ExecutorTest, ASubshellInATrapDoesNotInheritTheEntryStatus) {
	// A subshell is a new execution environment, so `exit` inside one reports ITS
	// last command and not the command the enclosing trap interrupted. exit-p.tst's
	// 'default exit status in subshell in signal trap' expects 2, and it regressed
	// the moment the entry status was added without shell_state::enter_subshell.
	EXPECT_EQ(capture("( trap '((exit 2); exit); echo $?' EXIT; exit 1 )"), "2\n");
}

TEST_F(ExecutorTest, ReturnWithNoOperandInATrapReportsTheEntryStatus) {
	// The same rule `exit` follows inside a trap action, and return-p.tst's
	// 'default exit status in function in trap' is what asserts it: `fn` runs
	// `true` and then `return`, and the status reported is the one the trap
	// INTERRUPTED - 19 - rather than `true`'s zero.
	//
	// A divergence from dash, recorded in ADR-0001: dash applies the rule to `exit`
	// and not to `return`, bash and zsh to neither.
	EXPECT_EQ(capture("( fn() { true; return; }; trap 'fn; echo trapped $?' EXIT; "
	                  "(exit 19); exit )"),
	          "trapped 19\n");
	// An operand still wins, and outside a trap nothing changes.
	EXPECT_EQ(capture("( fn() { true; return 3; }; trap 'fn; echo trapped $?' EXIT; "
	                  "(exit 19); exit )"),
	          "trapped 3\n");
	EXPECT_EQ(capture("fn() { true; return; }; (exit 19); fn; echo plain $?"),
	          "plain 0\n");
}

// --- set -v echoes a dot script as it reads it -------------------------------

TEST_F(ExecutorTest, VerboseEchoesADotScriptAsItIsRead) {
	// POSIX `-v`: the shell writes its input to standard error as it READS it, and
	// a dot script is input. lesh echoed the `. ./script` line and then went silent
	// for the script itself (dot-p.tst's 'with verbose option'), which is the one
	// place the option had least to say and most to show.
	const std::string script = ::testing::TempDir() + "lesh_verbose_dot.sh";
	{
		std::ofstream out{script};
		out << ":\n";
	}
	EXPECT_EQ(capture("set -v; . " + script + " 2>&1"), ":\n");
	// Per COMMAND, in the script's own bytes, the way run_input echoes a script.
	{
		std::ofstream out{script};
		out << "# a comment\n:\n";
	}
	EXPECT_EQ(capture("set -v; . " + script + " 2>&1"), "# a comment\n:\n");
	// And nothing is echoed without the option. `set +v` because the fixture keeps
	// one shell state for the whole test, so the option above is still on.
	EXPECT_EQ(capture("set +v; . " + script + " 2>&1"), "");
	std::remove(script.c_str());
}

TEST_F(ExecutorTest, VerboseDoesNotEchoAnEvalOperandTwice) {
	// dash echoes neither an `eval` operand nor a trap body: both have already been
	// echoed once as part of the line that carried them, and only `.` brings in
	// text the shell has not already read.
	EXPECT_EQ(capture("set -v; eval ':' 2>&1"), "");
	EXPECT_EQ(capture("set -v; trap ':' EXIT 2>&1"), "");
}

// --- a pipeline stage is a subshell environment (issue #53) -------------------
//
// Every case that must see THE STAGE take a signal has a grandchild send it
// through $PPID. `$$` in a stage is the SHELL's pid, so `kill -s USR1 $$` from a
// stage signals the shell, which then runs the trap perfectly correctly - which is
// how a stage keeping its parent's handlers survived twenty conformance files and
// two suites.

TEST_F(ExecutorTest, APipelineStageDoesNotRunTheHandlerItInherited) {
	// The disposition is restored because `trap` installs a real handler in this
	// process, and the next shell state's constructor reads what it finds (#37).
	const lesh::testing::saved_disposition guard{SIGUSR1};
	// `done` is the point: the STAGE dies of SIGUSR1 - its default action, since a
	// subshell resets the handler - and the SHELL carries on. Asserting emptiness
	// alone would also pass if the whole pipeline had failed to run.
	EXPECT_EQ(capture("trap 'echo TRAP' USR1; "
	                  "{ /bin/sh -c 'kill -s USR1 $PPID'; echo body; } | cat; echo done"),
	          "done\n")
		<< "the stage kept the parent's handler, so it ran TRAP and carried on";
}

TEST_F(ExecutorTest, APipelineStageKeepsAnIgnore) {
	// The asymmetry POSIX asks for and the reason the reset is not a clear-all: an
	// IGNORE protects a whole subtree, a handler belongs to the shell that set it.
	const lesh::testing::saved_disposition guard{SIGUSR1};
	EXPECT_EQ(capture("trap '' USR1; "
	                  "{ /bin/sh -c 'kill -s USR1 $PPID'; echo body; } | cat"),
	          "body\n");
}

TEST_F(ExecutorTest, ATrapSetInsideAPipelineStageFiresThere) {
	const lesh::testing::saved_disposition guard{SIGUSR1};
	EXPECT_EQ(capture("{ trap 'echo INSIDE' USR1; /bin/sh -c 'kill -s USR1 $PPID'; "
	                  "echo body; } | cat"),
	          "INSIDE\nbody\n");
}

TEST_F(ExecutorTest, APipelineStageRunsItsOwnExitTrap) {
	// The same answer run_subshell gave in 7a52868, because a stage is the same
	// kind of environment. It printed only `body` before.
	EXPECT_EQ(capture("{ trap 'echo S' EXIT; echo body; } | cat"), "body\nS\n");
	// And on the way out of an `exit`, which is where a stage's cleanup matters.
	EXPECT_EQ(capture("{ trap 'echo S' EXIT; exit 3; } | cat"), "S\n");
	// The LAST stage too, which is the one that is not a writer to a pipe.
	EXPECT_EQ(capture("echo a | { trap 'echo S' EXIT; cat; }"), "a\nS\n");
}

TEST_F(ExecutorTest, APipelineStageDoesNotRunTheExitTrapItInherited) {
	// Both halves of the one reset, and they are separable: a stage that ran the
	// inherited trap would print T twice, and a shell that reset its own would
	// print it never. In a subshell because `capture` redirects a brace group and
	// the SHELL's own EXIT trap runs after that redirection is undone.
	EXPECT_EQ(capture("( trap 'echo T' EXIT; { echo body; } | cat; echo after )"),
	          "body\nafter\nT\n");
	EXPECT_EQ(capture("( trap 'echo T' EXIT; { trap 'echo S' EXIT; echo body; } | cat )"),
	          "body\nS\nT\n");
}

TEST_F(ExecutorTest, APipelineStageInheritsTheShellsSpecialParameters) {
	// The neighbouring "what does a stage inherit" question, and the answer is that
	// these two are NOT reset: POSIX gives `$$` the pid of the invoking shell, and
	// a subshell does not become a new invocation. Both shells agree, and nothing
	// in either suite covers it.
	EXPECT_EQ(capture("p=$$; { [ \"$$\" = \"$p\" ] && echo same; } | cat"), "same\n");
	EXPECT_EQ(capture("sleep 0 & b=$!; { [ \"$!\" = \"$b\" ] && echo inherited; } | cat; wait"),
	          "inherited\n");
}
