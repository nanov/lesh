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
	// Four names are marked builtin_home::executor because they need the front end,
	// the process, or the record of background jobs. Nothing but behaviour can show
	// that the executor really has them, so each is exercised: a silent success
	// would be indistinguishable from a working builtin without an assertion on the
	// EFFECT.
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
