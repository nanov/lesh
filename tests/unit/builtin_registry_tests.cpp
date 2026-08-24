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
	shell_state state;

	int run(std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}

	std::string capture(std::string_view src) {
		const std::string path = ::testing::TempDir() + "lesh_registry_capture.txt";
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
	for (const auto& name : {"cd", "echo", "false", "pwd", "true", "test", "[",
	                         "alias", "unalias", "read", "command", "kill",
	                         "getopts", "wait"})
		EXPECT_EQ(classify_builtin(name), builtin_kind::regular) << name;
	EXPECT_EQ(classify_builtin("grep"), builtin_kind::none);
	EXPECT_EQ(classify_builtin(""), builtin_kind::none);
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
