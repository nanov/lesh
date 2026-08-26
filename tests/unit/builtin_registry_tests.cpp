#include "runtime/builtins.h"

#include "runtime/executor.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// The builtin registry, and the agreement between classification and
// implementation. See issue #35.
//
// A builtin could be CLASSIFIED without being IMPLEMENTED, and the result silently
// succeeded: `test` and `readonly` were in shell_state.cpp's name lists with no
// handler in builtins.cpp, so the command search stopped at "this is a builtin" and
// never reached PATH, try_run_builtin returned false, and run_simple_command
// discarded it. `test 1 = 2; echo $?` printed 0.
//
// builtins.cpp now static_asserts the handler table against the registry, so that
// exact drift is a compile error. These tests are the other half: they assert that
// what the registry CLAIMS is true of the running shell - a static_assert can only
// compare two tables, not check that the executor really implements the four names
// the registry says it does.
class BuiltinRegistryTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	// Every snippet run here, kept alive: a tree's spans are views into its source,
	// and shell state holds the tree a function body is a node in (#106). Declared
	// between `pool` and `state` so the three die in the order that points at.
	std::deque<std::string> sources;
	shell_state state;
	lesh::testing::temp_path scratch;

	int run(std::string_view src) {
		const std::string& source = sources.emplace_back(src);
		tree_walking_executor ex{pool, state};
		return ex.run(state.retain_tree(parse(pool, source)));
	}

	std::string capture(std::string_view src) {
		const std::string path = scratch.file("registry_capture.txt");
		std::remove(path.c_str());
		std::string wrapped{"{ "};
		wrapped.append(src);
		wrapped += "; } > ";
		wrapped += path;
		(void)run(wrapped);
		std::ifstream in{path};
		std::ostringstream text;
		text << in.rdbuf();
		std::remove(path.c_str());
		return text.str();
	}
};

} // namespace

TEST_F(BuiltinRegistryTest, EveryClassifiedNameIsImplementedSomewhere) {
	// The runtime mirror of the static_assert. It exists because the two checks
	// fail differently: the assert fires at build time for a name added to one
	// table only, and this fires for a registry row whose `home` is a lie.
	for (const auto& descriptor : kBuiltinRegistry) {
		ASSERT_NE(classify_builtin(descriptor.name), builtin_kind::none)
			<< descriptor.name << " is in the registry but classify_builtin says it "
			<< "is not a builtin, so the command search would look for it on PATH";

		// Asked rather than called: running every builtin to see whether it exists
		// would block in `read` and move this process in `cd`.
		EXPECT_EQ(builtin_has_handler(descriptor.name),
		          descriptor.home == builtin_home::table)
			<< descriptor.name << ": the registry says its implementation lives in "
			<< (descriptor.home == builtin_home::table ? "builtins.cpp" : "the executor")
			<< " and the dispatch disagrees. A classified name that nothing implements "
			<< "silently succeeds - that is #35.";
	}
}

TEST_F(BuiltinRegistryTest, ExecutorOwnedBuiltinsAreActuallyImplementedThere) {
	// Five names are marked builtin_home::executor because they need the front end,
	// the process, the function table, or the record of background jobs. Nothing but
	// behaviour can show that the executor really has them, so each is exercised: a
	// silent success would be indistinguishable from a working builtin without an
	// assertion on the EFFECT.
	EXPECT_EQ(capture("command -v :"), ":\n") << "command must reach the executor";
	EXPECT_EQ(run("eval 'exit 3'"), 3) << "eval must re-enter the front end";
	// Non-zero rather than a number: what `.` reports for a file it cannot read is
	// its own business, and dash says 2 where lesh says 127. What the registry
	// claims is only that something implements it, and a silent 0 would be the
	// failure this test exists for.
	EXPECT_NE(run(". /nonexistent/lesh/script 2>/dev/null"), 0);
	EXPECT_EQ(capture("( exec echo through-exec )"), "through-exec\n")
		<< "exec must become the command";
	EXPECT_EQ(run("wait 999999"), 127)
		<< "POSIX gives 127 for a pid that is not a child of this shell";
}

TEST_F(BuiltinRegistryTest, TestAndBracketAreTheSameImplementation) {
	// `[` was in NEITHER table, so it forked /bin/[ and answered correctly while
	// `test` - classified with no handler - reported 0 for everything. That is
	// exactly how the bug stayed invisible to anyone writing brackets.
	EXPECT_EQ(classify_builtin("test"), builtin_kind::regular);
	EXPECT_EQ(classify_builtin("["), builtin_kind::regular);
	EXPECT_EQ(run("test 1 = 2"), 1);
	EXPECT_EQ(run("[ 1 = 2 ]"), 1);
	EXPECT_EQ(run("test 1 = 1"), 0);
	EXPECT_EQ(run("[ 1 = 1 ]"), 0);
	// The one difference between the two spellings.
	EXPECT_EQ(run("[ 1 = 1 2>/dev/null"), 2) << "`[` requires its closing bracket";
}

TEST_F(BuiltinRegistryTest, SpecialBuiltinsAreExactlyThePosixList) {
	// POSIX XCU 2.14's list is closed, and membership decides whether a failure
	// exits a non-interactive shell and whether a prefix assignment persists.
	// Spelled out here so a name cannot drift between kinds unnoticed.
	for (const auto& name : {"break", ":", "continue", ".", "eval", "exec", "exit",
	                         "export", "readonly", "return", "set", "shift", "times",
	                         "trap", "unset"})
		EXPECT_EQ(classify_builtin(name), builtin_kind::special) << name;
	// `bind` and `prompt` are lesh's own rather than POSIX's, and REGULAR for the
	// reason the closed list above gives: a prompt that would not parse must not
	// end an rc file, and 2.14's fatality belongs to the fifteen names only.
	for (const auto& name : {"cd", "echo", "false", "pwd", "true", "test", "[",
	                         "alias", "unalias", "read", "command", "kill",
	                         "getopts", "wait", "bind", "prompt"})
		EXPECT_EQ(classify_builtin(name), builtin_kind::regular) << name;
	EXPECT_EQ(classify_builtin("grep"), builtin_kind::none);
	EXPECT_EQ(classify_builtin(""), builtin_kind::none);
}

// --- which of a special builtin's failures is fatal (issue #66) ---------------
//
// POSIX XCU 2.8.1's table lists the error classes that end a non-interactive
// shell when they happen in a SPECIAL builtin, and the list is closed: a shell
// language syntax error, an expansion error, a redirection error, a variable
// assignment error, and a UTILITY SYNTAX ERROR - which that row itself
// parenthesises as "option or operand error", meaning the command line was not
// the shape the utility accepts. A command line that WAS the right shape, and
// whose operation then failed, appears on none of those rows: it reports a status
// and the shell carries on.
//
// try_run_builtin had the two collapsed - every non-zero status from a special
// builtin became an exit - so `trap "" "" || echo reached` killed a script at a
// line whose author had written down that the failure was expected. dash, bash,
// zsh and yash all keep the shell alive there, and it was trap-p.tst's last
// failure at 41/42.
//
// The cases below come in pairs deliberately. Narrowing a rule that #34 built,
// and that holds error-p.tst at 220/220, is only safe if the rows it must NOT
// touch are asserted beside the row it must.

TEST_F(BuiltinRegistryTest, AnInvalidSignalNameIsReportedRatherThanFatal) {
	// WHICH SIGNAL NAMES EXIST IS A PROPERTY OF THE PLATFORM - SIGURG, SIGINFO and
	// SIGPWR are not all present everywhere - so a shell that made an unknown one
	// fatal would have made 2.8.1's fatality rule itself platform-dependent, and a
	// script would die on a host that merely lacks a signal.
	EXPECT_EQ(capture("trap '' '' 2>/dev/null; echo reached"), "reached\n");
	EXPECT_EQ(capture("trap '' NOSUCHSIG 2>/dev/null; echo reached"), "reached\n");
	EXPECT_EQ(capture("trap - NOSUCHSIG 2>/dev/null; echo reached"), "reached\n");
	EXPECT_EQ(capture("trap 'echo x' NOSUCHSIG 2>/dev/null; echo reached"),
	          "reached\n");
	// A NUMBER outside the range is the same error and takes the same answer: the
	// operand is a well-formed condition that this platform does not have.
	EXPECT_EQ(capture("trap '' 9999 2>/dev/null; echo reached"), "reached\n");
	EXPECT_EQ(capture("trap - 99999999999999999999 2>/dev/null; echo reached"),
	          "reached\n");
	// The listing form reaches the same lookup and must answer the same way, or the
	// distinction would depend on which spelling of `trap` asked.
	EXPECT_EQ(capture("trap -p NOSUCHSIG 2>/dev/null; echo reached"), "reached\n");
	// 1, which is what all four reference shells report - not 2. This is not a
	// usage error being reported quietly; it is not a usage error.
	EXPECT_EQ(run("trap '' NOSUCHSIG 2>/dev/null"), 1);
}

TEST_F(BuiltinRegistryTest, TrapRefusesAnOptionItDoesNotHave) {
	// The option loop used to BREAK on anything unrecognised, so `-Z` became the
	// ACTION STRING and `trap -Z x INT` went on to read `x` as a condition -
	// reporting `x: bad signal` at 1, a diagnostic about the wrong operand. dash
	// and yash both report a usage error (#73).
	//
	// 2 AND FATAL, unlike the bad-condition case above: POSIX XCU 2.8.1 makes an
	// unknown option a UTILITY SYNTAX ERROR, and `trap` is a special builtin. The
	// two rows of that table sit side by side here on purpose - a condition this
	// platform lacks is not a malformed command line, and an option that does not
	// exist is.
	EXPECT_EQ(run("trap -Z x INT 2>/dev/null"), 2);
	EXPECT_EQ(capture("trap -Z x INT 2>/dev/null; echo not reached"), "");
	// That a diagnostic is actually WRITTEN is asserted in the differential corpus,
	// where the case leaves stderr unredirected and dash is the oracle for it.
}

TEST_F(BuiltinRegistryTest, TrapStillTakesABareHyphenAndTheSeparator) {
	// The two things the check above must not swallow. A bare `-` is the RESET
	// ACTION - every `trap - SIG` in both test suites depends on it - and after
	// `--` an operand may start with as many hyphens as it likes.
	EXPECT_EQ(capture("trap 'echo x' USR1; trap - USR1; echo reached"), "reached\n");
	EXPECT_EQ(capture("trap -- '- trapped' USR1; echo reached"), "reached\n");
	// `-p` is lesh's own, and stays. dash has no `-p` and reports `Illegal option`
	// for it, so this is asserted here rather than in the differential corpus.
	EXPECT_EQ(capture("trap -p INT >/dev/null; echo reached"), "reached\n");
}

TEST_F(BuiltinRegistryTest, ABadConditionDoesNotStopTrapReadingTheRest) {
	// The loop reports and continues, so a condition after the bad one is still
	// set. SIGURG because its default action is to discard, which keeps a case that
	// reaches the old path from killing this binary instead of failing.
	EXPECT_EQ(capture("trap 'echo hit' NOSUCHSIG URG 2>/dev/null; kill -s URG $$"),
	          "hit\n");
}

TEST_F(BuiltinRegistryTest, CommandDemotesTheFatalityAsWellAsTheAssignment) {
	// POSIX XCU `command`: when the name is a special builtin, "the special
	// properties in XCU 2.14 shall not occur" - and 2.14 names exactly two, the
	// abort and the persisting assignment. lesh demoted the assignment and the
	// REDIRECTION failure and left the status half special, so `command set -Z`
	// still ended the shell where dash and bash both report and carry on.
	EXPECT_EQ(capture("command trap '' NOSUCHSIG 2>/dev/null; echo reached"),
	          "reached\n");
	EXPECT_EQ(capture("command set -Z 2>/dev/null; echo reached"), "reached\n");
	EXPECT_EQ(capture("command shift abc 2>/dev/null; echo reached"), "reached\n");
	// A fresh NAME per line: the fixture's shell_state outlives a single capture(),
	// so reusing one would make the SECOND line's `readonly ro=1` the failure being
	// observed rather than the line under test.
	EXPECT_EQ(capture("readonly ro=1; command readonly ro=2 2>/dev/null; echo reached"),
	          "reached\n");
	EXPECT_EQ(capture("readonly ru=1; command unset ru 2>/dev/null; echo reached"),
	          "reached\n");
}

TEST_F(BuiltinRegistryTest, TheErrorClassesPosixMakesFatalStillAre) {
	// One row of 2.8.1's table per line, each reached through a different special
	// builtin. Every one of these agreed with dash before #66 and has to still.
	EXPECT_EQ(capture("set -Z 2>/dev/null; echo notreached"), "");
	EXPECT_EQ(capture("shift abc 2>/dev/null; echo notreached"), "");
	EXPECT_EQ(capture("shift 5 2>/dev/null; echo notreached"), "");
	EXPECT_EQ(capture("export 1bad=x 2>/dev/null; echo notreached"), "");
	EXPECT_EQ(capture("readonly 1bad=x 2>/dev/null; echo notreached"), "");
	// A fresh NAME on each of these two: the fixture's shell_state outlives a single
	// capture(), so a reused one would make the second line's `readonly` the
	// failure being observed rather than the `export` or `unset` under test.
	EXPECT_EQ(capture("readonly re=1; export re=2 2>/dev/null; echo notreached"), "");
	EXPECT_EQ(capture("readonly rv=1; unset rv 2>/dev/null; echo notreached"), "");
	EXPECT_EQ(capture("eval 'if' 2>/dev/null; echo notreached"), "");
	EXPECT_EQ(capture(". ./_lesh_no_such_file_ 2>/dev/null; echo notreached"), "");
	// The redirection row, on the very builtin this ticket softened elsewhere. The
	// `2>/dev/null` comes FIRST because redirections are applied left to right and
	// the failing one would otherwise never let it take effect.
	EXPECT_EQ(capture("trap '' INT 2>/dev/null <./_lesh_no_such_file_; echo notreached"),
	          "");
	EXPECT_EQ(capture(": 2>/dev/null <./_lesh_no_such_file_; echo notreached"), "");
	// And a REGULAR builtin's failure is still not fatal in any of those rows.
	EXPECT_EQ(capture("kill -l 2>/dev/null <./_lesh_no_such_file_; echo reached"),
	          "reached\n");
	EXPECT_EQ(capture("kill -s NOSUCH $$ 2>/dev/null; echo reached"), "reached\n");
}

TEST_F(BuiltinRegistryTest, ExecutorBuiltinsWorkInsideAPipelineStage) {
	// A pipeline stage builds its own argv and called try_run_builtin directly,
	// which has no entry for the executor's four - and the false return was
	// discarded there too, so `echo hi | eval cat` printed nothing and reported
	// success. The same defect shape as the unimplemented `test`, one function
	// away.
	EXPECT_EQ(capture("echo hi | eval cat"), "hi\n");
	EXPECT_EQ(run("true | eval 'exit 3'"), 3);
}

// `command` (#31). Every form below either had no implementation at all or was
// taken for a command NAME: `command -V echo` reported `-V: No such file or
// directory`, which is a NON-ZERO status, and that is the only reason
// command-p.tst's 'describing non-existent command (-V)' was among its 14 passes.
TEST_F(BuiltinRegistryTest, CommandDescribesEveryCategoryOfName) {
	// A reserved word is recognised by POSITION in the parser, so the runtime has
	// to ask syntax::is_reserved_word rather than keep a list of its own.
	EXPECT_EQ(capture("command -v if"), "if\n");
	EXPECT_EQ(capture("command -v '!'"), "!\n");
	EXPECT_EQ(capture("command -V while"), "while is a shell keyword\n");
	EXPECT_EQ(capture("command -V :"), ": is a special shell builtin\n");
	EXPECT_EQ(capture("command -V read"), "read is a shell builtin\n");
	EXPECT_EQ(capture("f() { :; }; command -v f"), "f\n");
	EXPECT_EQ(capture("f() { :; }; command -V f"), "f is a shell function\n");
	// POSIX writes an alias as a command line that RE-CREATES it, keyword and all -
	// `abc='echo ABC'` without it would assign a variable on re-input.
	EXPECT_EQ(capture("alias abc='echo ABC'; command -v abc"),
	          "alias abc='echo ABC'\n");
	EXPECT_EQ(capture("alias abc=xyz; command -V abc"), "abc is an alias for xyz\n");
}

TEST_F(BuiltinRegistryTest, CommandDescribesAUtilityByPathnameAndTheRestByName) {
	// The builtin_report split: a REGULAR built-in utility is written as the
	// pathname the search finds and a must-be-built-in one as its own name.
	EXPECT_EQ(capture("command -v echo"), "/bin/echo\n");
	// The -V line repeats the pathname because command-p.tst greps the -V output for
	// the whole of the -v output.
	EXPECT_EQ(capture("command -V echo"), "echo is a shell builtin (/bin/echo)\n");
	EXPECT_EQ(capture("command -v cat"), "/bin/cat\n");
	EXPECT_EQ(capture("command -V cat"), "cat is /bin/cat\n");
	// `bind` and `prompt` are the must-be-built-in end of that split, and the
	// clearest case of it: there is no `/usr/bin/prompt` for the search to find,
	// and a prompt configured in a subprocess would die with the subprocess.
	// Nothing but the registry row decides this, so a row that drifted to
	// `pathname` would show up as a pathname search that finds nothing.
	EXPECT_EQ(capture("command -v bind"), "bind\n");
	EXPECT_EQ(capture("command -v prompt"), "prompt\n");
	EXPECT_EQ(capture("command -V prompt"), "prompt is a shell builtin\n");
}

TEST_F(BuiltinRegistryTest, CommandReportsANameItCannotFindAsNotFound) {
	// 127 rather than 1: the question was what would run, and nothing would.
	EXPECT_EQ(run("PATH= command -v _no_such_command_"), 127);
	EXPECT_EQ(capture("PATH= command -v _no_such_command_"), "")
		<< "the -v form must be silent - command-p.tst requires stdout AND stderr "
		   "empty for a name that does not exist";
	EXPECT_EQ(capture("PATH= command -V _no_such_command_"),
	          "_no_such_command_: not found\n");
	EXPECT_EQ(run("PATH= command -V _no_such_command_"), 127);
}

TEST_F(BuiltinRegistryTest, CommandStandardPathFindsAUtilityWithoutPath) {
	// -p searches the path the system guarantees finds the standard utilities, which
	// is the whole point of the option. It was taken for a command name: `command -p
	// ls` reported `-p: No such file or directory`.
	EXPECT_EQ(capture("PATH= command -p echo through-p"), "through-p\n");
	EXPECT_EQ(run("PATH= command -pv cat >/dev/null"), 0);
	EXPECT_EQ(run("PATH= command -p cat </dev/null"), 0);
}

TEST_F(BuiltinRegistryTest, CommandOptionsAndTheirAbsence) {
	// -V wins over -v whatever order they arrive in, which is what dash answers for
	// both spellings. "Last one wins" would disagree with it on the first.
	EXPECT_EQ(capture("command -v -V :"), ": is a special shell builtin\n");
	EXPECT_EQ(capture("command -V -v :"), ": is a special shell builtin\n");
	// `--` ends the options, so what follows is a command name and not an option.
	EXPECT_EQ(run("command -- -v 2>/dev/null"), 127);
	// Nothing to run: a command that does nothing and succeeds.
	EXPECT_EQ(run("command"), 0);
	EXPECT_EQ(run("command --"), 0);
	EXPECT_EQ(run("command -p"), 0);
	EXPECT_EQ(run("command -z cat 2>/dev/null"), 2) << "an option command has not";
}

TEST_F(BuiltinRegistryTest, AFunctionNamedCommandShadowsTheBuiltin) {
	// POSIX's search order puts functions ahead of REGULAR builtins, and `command`
	// is regular. Stripping the prefix regardless ran the operand instead of the
	// function - builtins-p.tst's 'function overrides non-special command command'.
	EXPECT_EQ(capture("command() { echo FUNCTION; }; command XXX"), "FUNCTION\n");
	// Only the first prefix: after one has been stripped, function lookup is
	// bypassed for everything after it, `command` included.
	EXPECT_EQ(capture("command() { echo F; }; command command echo hi"), "F\n");
}

TEST_F(BuiltinRegistryTest, CommandRunsAndBypassesFunctionsInAPipelineStage) {
	// A stage stripped no `command` prefix at all, so argv[0] stayed `command`, the
	// registry said that was a builtin, and the handler table had no entry for it:
	// the stage ran NOTHING and reported success.
	EXPECT_EQ(capture("echo hi | command cat"), "hi\n");
	EXPECT_EQ(capture("cat() { echo FUNCTION; }; echo hi | command cat"), "hi\n");
	EXPECT_EQ(capture("echo hi | command -p cat"), "hi\n");
}

// --- what `kill` validates (issue #45) ---------------------------------------
//
// The same failure this file exists for, one builtin further in: a form the shell
// did not handle reported SUCCESS. `kill -s TERM` signalled nothing and answered
// 0, and every unrecognised PID operand went through `atoi` - which answers 0 for
// `notanumber`, for `--`, for `%1` and for `0x10`. kill(0, sig) signals the whole
// PROCESS GROUP, so those were not diagnostics being missed but the wrong syscall
// being made.
//
// SIGURG throughout, and deliberately: it is discarded by default, so a case that
// still reached the old code path would leave this binary alive to report the
// failure. With TERM the pre-fix behaviour kills the test runner instead of
// failing a test, which is the same reason the corpus cases for the dangerous
// spelling live in a shell of their own.

TEST_F(BuiltinRegistryTest, KillWithNoPidOperandIsAUsageError) {
	// 2, which is what dash answers for every one of these and what this shell
	// already answers for `break` and `continue`.
	EXPECT_EQ(run("kill -s URG 2>/dev/null"), 2);
	EXPECT_EQ(run("kill 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -URG 2>/dev/null"), 2);
	// `--` ends the options, so this names no pid either. It used to reach
	// atoi("--") - and signal the group.
	EXPECT_EQ(run("kill -s URG -- 2>/dev/null"), 2);
}

TEST_F(BuiltinRegistryTest, KillWithNoSignalAfterDashSSaysSo) {
	// The old reading fell through to the `-NAME` form and reported `s: bad
	// signal`, a diagnostic about a letter the user did not type.
	EXPECT_EQ(run("kill -s 2>/dev/null"), 2);
}

TEST_F(BuiltinRegistryTest, KillRefusesAnOperandThatIsNotAProcessId) {
	EXPECT_EQ(run("kill -s URG notanumber 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -s URG 12abc 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -s URG '' 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -s URG 0x10 2>/dev/null"), 2);
	// A job specification is not a pid and job control is out of scope (ADR-0001),
	// so it is refused rather than quietly meaning "this process group".
	EXPECT_EQ(run("kill -s URG %1 2>/dev/null"), 2);
	// A bare negative operand is an OPTION as far as the scan is concerned, and an
	// unknown one. POSIX writes a process group as `kill -s URG -- -$pgid`, which
	// is the form kill4-p.tst uses; dash refuses the bare spelling the same way.
	EXPECT_EQ(run("kill -s URG -1 2>/dev/null"), 2);
}

TEST_F(BuiltinRegistryTest, KillRefusesASignalItDoesNotKnow) {
	EXPECT_EQ(run("kill -s NOSUCH $$ 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -x 1 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -n 9 $$ 2>/dev/null"), 2);
}

TEST_F(BuiltinRegistryTest, KillStillAcceptsEveryFormPOSIXDefines) {
	// The null signal asks the kernel nothing and reports whether it could have,
	// which makes it the one signal safe to send this process from its own tests.
	EXPECT_EQ(run("kill -s 0 $$"), 0);
	EXPECT_EQ(run("kill -0 $$"), 0);
	EXPECT_EQ(run("kill -s 0 -- $$"), 0);
	EXPECT_EQ(run("kill -s0 $$"), 0);
	// dash's tolerances, copied rather than tightened: surrounding blanks, a
	// leading `+`, and leading zeros are all a pid.
	EXPECT_EQ(run("kill -s 0 \" $$ \""), 0);
	EXPECT_EQ(run("kill -s 0 \"+$$\""), 0);
	// Two operands, both signalled, and the status is still zero.
	EXPECT_EQ(run("kill -s 0 $$ $$"), 0);
	// -l is unaffected: it takes one operand, reads a 128+n exit status back, and
	// refuses what names no signal - with the usage status, as dash does.
	EXPECT_EQ(capture("kill -l 9"), "KILL\n");
	EXPECT_EQ(capture("kill -l 137"), "KILL\n");
	EXPECT_EQ(capture("kill -l -- 1"), "HUP\n");
	EXPECT_EQ(run("kill -l 0 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -l 128 2>/dev/null"), 2);
}

TEST_F(BuiltinRegistryTest, AFailedKillIsOneAndNotAUsageError) {
	// The two statuses mean different things: 2 for a line the shell refused to
	// run, 1 for one the SYSTEM refused. Conflating them would have made the fix
	// above unfalsifiable.
	EXPECT_EQ(run("kill -s 0 99999999 2>/dev/null"), 1);
}

TEST_F(BuiltinRegistryTest, KillRefusesEXITBecauseItIsATrapConditionAndNotASignal) {
	// The name resolves to 0 and the NUMBER 0 is the null signal, so this is not
	// the same operand twice: `kill -s 0 $$` must work and `kill -s EXIT $$` must
	// not. It reported success having sent nothing - the same category error
	// `kill -l` refuses at the other end, and dash refuses this one too.
	EXPECT_EQ(run("kill -s EXIT $$ 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -EXIT $$ 2>/dev/null"), 2);
	EXPECT_EQ(run("kill -s 0 $$"), 0);
}

// ---------------------------------------------------------------------------
// THE NO-BYPASS GUARD for the one numeric-operand parser (issue #63).
//
// substrate/numeric.h has TWO guarantees and they need two different mechanisms.
//
// The first is compile-time and lives in the header: `numeric_site` and
// kNumericPolicies are static_asserted against each other, so a site cannot be
// added to the enum without a row, and a row cannot drift out of order into
// another site's range. That is #35's registry shape exactly, and #35's registry
// is the precedent that actually prevented recurrence.
//
// The second is what a static_assert cannot express, and it is the one that
// matters here: a NEW site could simply not use the mechanism. Fifteen sites and
// six idioms is what this ticket found, and every one of the six was written out
// by hand at its own call site - so the check has to be over the SOURCE, and it is
// the same grep that took the inventory:
//
//     grep -rnE "atoi|\* *10 *\+" src/runtime src/syntax
//
// Kept as a test rather than a lint script because a lint script is a thing
// someone has to remember to run, and #35's lesson is that the guard which works
// is the one in the build. src/legacy/ used to be exempt from it; #28 deleted the
// directory, so the guard now covers every line of src/ with no carve-out.

namespace {

// The repository root, from this file's own compile-time path. There is no
// CMake-supplied define for it and adding one is a change to a file this ticket
// does not own, so __FILE__ is the honest route - CMake passes source paths
// absolutely, and the test fails loudly below if that ever stops being true.
std::filesystem::path repository_root() {
	std::filesystem::path here{__FILE__};
	return here.parent_path().parent_path().parent_path();   // tests/unit/<this>
}

// The idioms a numeric operand must no longer be read with. The atoi family
// CANNOT REPORT FAILURE AT ALL, which is the whole defect; the accumulation
// shapes are the hand-written loops that overflowed.
constexpr std::string_view kBannedIdioms[] = {
	"atoi", "atol", "atoll",
	"strtol", "strtoul", "strtoll", "strtoull", "strtoimax", "strtoumax",
	"std::sto",
	"* 10 +", "*10+", "* 8 +", "* 16 +", "* base +", "*base+",
};

// Everything outside a comment. The banned idioms are DISCUSSED at length in the
// comments of the very files this scans - each one explains what it replaced -
// so a guard that read prose would fire on the explanation of itself.
std::string code_only(const std::string& line, bool& in_block) {
	std::string out;
	for (size_t i = 0; i < line.size(); ++i) {
		if (in_block) {
			if (line.compare(i, 2, "*/") == 0) {
				in_block = false;
				++i;
			}
			continue;
		}
		if (line.compare(i, 2, "//") == 0)
			break;
		if (line.compare(i, 2, "/*") == 0) {
			in_block = true;
			++i;
			continue;
		}
		out.push_back(line[i]);
	}
	return out;
}

} // namespace

TEST(NumericParserGuard, TheSourceTreeItScansIsActuallyThere) {
	// The guard below can only fail if it is looking at something. An absolute
	// __FILE__ is what makes that true, and a relative one would turn the whole
	// check into a silent pass - the exact failure mode #35 was about.
	const std::filesystem::path root = repository_root();
	ASSERT_TRUE(std::filesystem::exists(root / "src" / "runtime" / "builtins.cpp"))
		<< "the guard could not find the source tree from __FILE__ (" << __FILE__ << ")";
	ASSERT_TRUE(std::filesystem::exists(root / "src" / "substrate" / "numeric.h"));
}

TEST(NumericParserGuard, NoSiteReadsANumberWithoutTheOneParser) {
	const std::filesystem::path root = repository_root();
	const std::filesystem::path mechanism = root / "src" / "substrate" / "numeric.h";

	std::vector<std::string> offences;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "src")) {
		if (!entry.is_regular_file())
			continue;
		const std::filesystem::path& path = entry.path();
		if (path.extension() != ".cpp" && path.extension() != ".h")
			continue;
		// The mechanism itself is the one place these idioms are allowed to live.
		if (path == mechanism)
			continue;

		std::ifstream in{path};
		std::string line;
		bool in_block = false;
		for (int number = 1; std::getline(in, line); ++number) {
			const std::string code = code_only(line, in_block);
			for (const std::string_view idiom : kBannedIdioms) {
				if (code.find(idiom) == std::string::npos)
					continue;
				offences.emplace_back(
					std::filesystem::relative(path, root).string() + ":" +
					std::to_string(number) + ": " + std::string(idiom) + " -- " + code);
			}
		}
	}

	EXPECT_TRUE(offences.empty())
		<< "a numeric operand is being read outside substrate/numeric.h.\n"
		<< "Every site goes through parse_integer or scan_digits, and a new one adds\n"
		<< "a numeric_site enumerator and a policy row rather than its own digits.\n"
		<< [&offences] {
			std::string joined;
			for (const auto& o : offences)
				joined += "  " + o + "\n";
			return joined;
		}();
}
