// The runtime's cooperation seam (#199, step 1a of #145).
//
// What is worth testing about an interface with one empty implementation is not
// the implementation - it is WHERE and HOW OFTEN the runtime calls it, because
// that is the contract the one-thread ticket will build a scheduler on. So every
// test here counts calls against a shape of shell code, and the counts are
// derived in the comments rather than recorded from a run: a number nobody can
// re-derive would be updated when it changed instead of investigated.

#include "runtime/cooperation.h"

#include "runtime/executor.h"
#include "runtime/shell_state.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

using namespace lesh::runtime;

namespace {

// The whole test double: count the boundaries, do nothing at them. It must not
// touch the shell, and that is the point of the seam - a host that needed shell
// state at a boundary would be a different design.
class counting_cooperation final : public cooperation {
public:
	void on_command_boundary() noexcept override { ++calls; }

	std::size_t calls = 0;
};

class CooperationTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	// See ExecutorTest for why the sources outlive the runs and why the three
	// members are declared in this order.
	std::deque<std::string> sources;
	shell_state state;
	counting_cooperation host;

	// THROUGH `run_input`, not `run`: the boundary lives in the command loop that
	// both the script path and the interactive path reach, and `run_input` is the
	// door both of them use. `echo_when_verbose` is false so nothing is printed.
	int run(std::string_view src) {
		const std::string& source = sources.emplace_back(src);
		tree_walking_executor ex{pool, state};
		return ex.run_input(source, false);
	}

	std::size_t boundaries(std::string_view src) {
		host.calls = 0;
		state.set_cooperation(host);
		(void)run(src);
		return host.calls;
	}
};

// The default, and the one thing every non-interactive shell relies on: there is
// something there. A shell nobody wired cooperates with the static no-op, so no
// site in the runtime has a null to check.
TEST_F(CooperationTest, DefaultIsTheNoOp) {
	const shell_state fresh;
	EXPECT_EQ(&fresh.cooperation(), &noop_cooperation());
}

// `lesh -c` installs nothing, and neither does running a script. Asserted over a
// snippet that does the things a shell does - assigns, branches, loops, defines
// and calls a function - because "nothing installed anything" is only interesting
// if the shell actually ran.
TEST_F(CooperationTest, RunningACommandInstallsNothing) {
	EXPECT_EQ(&state.cooperation(), &noop_cooperation());
	(void)run("f() { i=0; while test $i -lt 3; do i=$((i+1)); done; }\n"
	          "f\n"
	          "if test $i -eq 3; then :; fi\n");
	EXPECT_EQ(&state.cooperation(), &noop_cooperation());
}

// Taking it away again is `set_cooperation(noop_cooperation())`, which is the
// only spelling there is: the setter takes a reference, so installing null is
// not expressible.
TEST_F(CooperationTest, TheNoOpCanBeInstalledBack) {
	state.set_cooperation(host);
	EXPECT_EQ(&state.cooperation(), &host);
	state.set_cooperation(noop_cooperation());
	EXPECT_EQ(&state.cooperation(), &noop_cooperation());
	EXPECT_EQ(host.calls, 0u);
}

// N simple commands, N boundaries. One per command and not one per input.
TEST_F(CooperationTest, OneCallPerSimpleCommand) {
	EXPECT_EQ(boundaries(":\n"), 1u);
	EXPECT_EQ(boundaries(":\n:\n"), 2u);
	EXPECT_EQ(boundaries(":\n:\n:\n:\n:\n"), 5u);
}

// A `;` list is the same three commands with a different separator, so it is the
// same three boundaries. `&&` is NOT: its right side does not run, and a command
// that did not run has no boundary.
TEST_F(CooperationTest, OneCallPerCommandOfAList) {
	EXPECT_EQ(boundaries(":; :; :"), 3u);
	EXPECT_EQ(boundaries("true && false"), 1u);
	EXPECT_EQ(boundaries("false || true"), 1u);
}

// A LOOP BODY IS COMMANDS, which is the property the whole seam rests on: the
// host gets a slice inside a long-running loop and not only when the loop is
// over. Derived rather than recorded:
//
//   `i=0`                          1
//   the `while` compound itself    1
//   `test $i -lt n`                n + 1  (the failing evaluation counts)
//   `i=$((i+1))`                   n
//
// so 2 + 2n + 1. The second assertion is the one that matters - two more
// boundaries per iteration, forever - because a design that reported the loop
// once would satisfy the first and be useless.
TEST_F(CooperationTest, EachLoopIterationIsABoundary) {
	EXPECT_EQ(boundaries("i=0; while test $i -lt 5; do i=$((i+1)); done"), 3u + 2 * 5);
	EXPECT_EQ(boundaries("i=0; while test $i -lt 20; do i=$((i+1)); done"), 3u + 2 * 20);
}

// A `for` loop's body, on the same terms, without a condition command to count:
// one for the compound, one per body command per iteration.
TEST_F(CooperationTest, EachForIterationIsABoundary) {
	EXPECT_EQ(boundaries("for x in a b c; do :; done"), 1u + 3);
	EXPECT_EQ(boundaries("for x in a b c; do :; :; done"), 1u + 6);
}

// The command loop is ONE function reached by every body in the language (#77),
// so these follow from the loop cases rather than adding a mechanism - which is
// exactly why they are worth asserting: a second call site added later would
// double-count here first.
TEST_F(CooperationTest, CompoundBodiesCountTheirOwnCommands) {
	// The group, then the two commands inside it.
	EXPECT_EQ(boundaries("{ :; :; }"), 1u + 2);
	// The `if`, its condition, and the one command in the taken branch.
	EXPECT_EQ(boundaries("if true; then :; fi"), 1u + 1 + 1);
	// The function definition is a command; the call is a command; the body's two
	// commands are commands.
	EXPECT_EQ(boundaries("f() { :; :; }\nf\n"), 1u + 1 + 2);
}

// Ctrl-C at the prompt is the second call site, and it is a boundary with no
// command in it: nothing ran, the shell is about to read again. Asserted with
// `$?` because the call is LAST in that function for exactly this reason - the
// host must not be told about the boundary while the status is still being fixed
// up (#98 decision 3 sets 130 twice, around the traps).
TEST_F(CooperationTest, ACancelledPromptIsABoundary) {
	state.set_cooperation(host);
	tree_walking_executor ex{pool, state};
	ex.interrupt_at_prompt();
	EXPECT_EQ(host.calls, 1u);
	EXPECT_EQ(state.last_status(), 130);
}

// An `eval` and a `.` read commands through the same loop, so what they run is
// boundaries too - a host must not be starved for the length of an `eval`.
TEST_F(CooperationTest, EvalRunsBoundariesOfItsOwn) {
	EXPECT_EQ(boundaries("eval ':; :'"), 1u + 2);
}

// ---------------------------------------------------------------------------
// A FORKED CHILD COOPERATES WITH NOBODY (#202, answering #199's open question)
// ---------------------------------------------------------------------------

// The direct statement of the rule: `enter_subshell` puts the pointer back,
// beside the `_tty_fd` clear it already did for the same class of reason.
//
// EVERY ROLE, which is the part worth pinning: the fd has an exception for a
// foreground `( )`, because such a subshell genuinely manages the terminal for
// the jobs it runs. There is no matching exception here - nothing a child can do
// makes its parent's host the right thing to talk to - so a test that only
// checked the default role would pass while a `( nvim )` called into a scheduler
// that is not its own.
TEST_F(CooperationTest, EnterSubshellPutsTheNoOpBack) {
	for (const subshell_role role : {subshell_role::detached,
	                                 subshell_role::foreground_job}) {
		shell_state child;
		child.set_cooperation(host);
		ASSERT_EQ(&child.cooperation(), &host);
		child.enter_subshell(role);
		EXPECT_EQ(&child.cooperation(), &noop_cooperation())
			<< "role " << static_cast<int>(role) << " inherited its parent's host";
	}
}

namespace {

// A cooperation that says WHICH PROCESS reached the boundary, down a pipe.
//
// COUNTING WOULD NOT DO, and that is the whole design of this test. A child's
// increments land in the child's copy-on-write page, so a parent that counted
// its own calls would read the same number whether or not the child had called
// too - the test would pass for the wrong reason and keep passing after the
// reset was deleted. One byte per boundary, 'P' from the process that installed
// the host and 'C' from anybody else, makes the child's calls visible to the
// parent because a pipe is shared where memory is not.
class reporting_cooperation final : public cooperation {
public:
	explicit reporting_cooperation(int write_fd, pid_t owner) noexcept
		: _fd(write_fd), _owner(owner) {}

	void on_command_boundary() noexcept override {
		const char who = ::getpid() == _owner ? 'P' : 'C';
		// Nothing else: a boundary handler is called from inside the executor's
		// command loop, and in a forked child the only safe things are raw libc.
		while (::write(_fd, &who, 1) < 0 && errno == EINTR) {
		}
	}

private:
	int _fd = -1;
	pid_t _owner = 0;
};

} // namespace

TEST_F(CooperationTest, ASubshellNeverReachesItsParentsHost) {
	int fds[2] = {-1, -1};
	ASSERT_EQ(::pipe(fds), 0);
	reporting_cooperation reporter{fds[1], ::getpid()};
	state.set_cooperation(reporter);

	// A DETACHED SUBSHELL AND A FOREGROUND ONE, and a command substitution, which
	// is the third fork that runs shell code without exec'ing. Each body is two
	// commands, so an inherited pointer would show up as several 'C's.
	(void)run("( :; : )\n"
	          "x=$( :; : )\n"
	          ": | ( :; : )\n");

	ASSERT_EQ(::close(fds[1]), 0);
	std::string reported;
	char chunk[256];
	for (;;) {
		const ssize_t got = ::read(fds[0], chunk, sizeof chunk);
		if (got > 0) {
			reported.append(chunk, static_cast<std::size_t>(got));
			continue;
		}
		if (got < 0 && errno == EINTR)
			continue;
		break;
	}
	ASSERT_EQ(::close(fds[0]), 0);

	EXPECT_EQ(reported.find('C'), std::string::npos)
		<< "a forked child called into its parent's host: " << reported;
	// AND THE PARENT DID REACH BOUNDARIES, so the absence above is evidence rather
	// than an empty pipe: three commands at the top level is three of them.
	EXPECT_GE(std::count(reported.begin(), reported.end(), 'P'), 3);
}

} // namespace
