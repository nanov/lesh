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

#include <sys/wait.h>
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

	// AND IT REALLY WAITS (#208). A double that answered without reaping would
	// leave the executor reading an uninitialised status and the shell full of
	// zombies, so what is counted is the CALL and what is done is exactly what the
	// no-op does. The flags are recorded per call, because "WUNTRACED exactly
	// where the file has it today" is the half of this seam a regression would
	// silently break: without it a Ctrl-Z'd foreground command never returns.
	pid_t wait_child(pid_t pid, int flags, int* status) noexcept override {
		++waits;
		flags_seen.push_back(flags);
		return ::waitpid(pid, status, flags);
	}

	// AND THIS ONE DOES NOTHING BUT COUNT, which is exactly what the no-op does
	// (#209): the verb transfers no bytes, reports no error and answers nothing,
	// so a double that records the descriptor and returns IS the production no-op
	// - and the counts below are therefore statements about the runtime's call
	// sites rather than about a stub.
	void await_readable(int fd) noexcept override {
		++awaits;
		fds_awaited.push_back(fd);
	}

	std::size_t calls = 0;
	std::size_t waits = 0;
	std::vector<int> flags_seen;
	std::size_t awaits = 0;
	std::vector<int> fds_awaited;
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

	// The other counter, over the same door. Returns the flags of every wait the
	// snippet made, in order, so a test can assert both how many and which.
	std::vector<int> waits(std::string_view src) {
		host.waits = 0;
		host.flags_seen.clear();
		state.set_cooperation(host);
		(void)run(src);
		return host.flags_seen;
	}

	// The third counter (#209): every descriptor the snippet said it was about to
	// block on, in order. Runs the snippet with `text` on fd 0, because `read` is
	// the only caller and it has no input without one.
	std::vector<int> awaits(std::string_view src, std::string_view text) {
		int fds[2] = {-1, -1};
		[&] { ASSERT_EQ(::pipe(fds), 0); }();
		[&] {
			ASSERT_EQ(::write(fds[1], text.data(), text.size()),
			          static_cast<ssize_t>(text.size()));
		}();
		::close(fds[1]);
		const int saved = ::dup(STDIN_FILENO);
		::dup2(fds[0], STDIN_FILENO);
		::close(fds[0]);

		host.awaits = 0;
		host.fds_awaited.clear();
		state.set_cooperation(host);
		(void)run(src);

		::dup2(saved, STDIN_FILENO);
		::close(saved);
		return host.fds_awaited;
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
// THE FOREGROUND WAIT (#208, phase 2a of #145)
// ---------------------------------------------------------------------------
//
// Same discipline as the boundary above: what is asserted is WHERE the runtime
// asks and WITH WHICH FLAGS, because those are the contract a host builds a
// park on. The counts are derived from the file - one `reap` per foreground
// child, one per `wait` operand - rather than recorded from a run.
//
// `/bin/echo` AND `/bin/cat` rather than the builtins of the same name, because
// a builtin does not fork and therefore never reaches a wait at all. That is
// itself the first assertion.

// A builtin runs in this process. No fork, no child, no wait - which is what
// makes "one wait per foreground EXTERNAL" the sentence it is.
TEST_F(CooperationTest, ABuiltinNeverReachesTheWait) {
	EXPECT_TRUE(waits(":\n:\n:\n").empty());
}

// THE ONE WAIT CTRL-Z CAN REACH, and the only reason `WUNTRACED` is in this file
// at all (#161): #159 handed the terminal to this child and reset its SIGTSTP to
// the default, so a plain wait would never return for a stopped command.
TEST_F(CooperationTest, AForegroundExternalIsOneWaitWithWUNTRACED) {
	EXPECT_EQ(waits("/bin/echo a >/dev/null\n"), (std::vector<int>{WUNTRACED}));
}

// Every stage of a foreground pipeline is a foreground child, so every stage is
// a `WUNTRACED` wait - which is what lets Ctrl-Z stop `sleep 30 | cat` rather
// than only its head.
TEST_F(CooperationTest, EveryPipelineStageIsAWaitWithWUNTRACED) {
	EXPECT_EQ(waits("/bin/echo a | /bin/cat >/dev/null\n"),
	          (std::vector<int>{WUNTRACED, WUNTRACED}));
}

// A `( )` the shell is waiting on is a foreground job and reaps like one (#158
// decision 5). ONE wait here and not two: the command inside the subshell is
// waited for by the SUBSHELL, whose `enter_subshell` put the no-op back, so its
// wait never reaches this host.
TEST_F(CooperationTest, AForegroundSubshellIsOneWaitWithWUNTRACED) {
	EXPECT_EQ(waits("( /bin/echo a >/dev/null )\n"), (std::vector<int>{WUNTRACED}));
}

// NO `WUNTRACED` HERE, and #161 gives the reason: a command substitution never
// receives the terminal (#158 decision 3 forbids it by name), so Ctrl-Z cannot
// reach this child and a stop has no status the enclosing expansion could be
// finished with.
TEST_F(CooperationTest, ACommandSubstitutionWaitsWithoutWUNTRACED) {
	EXPECT_EQ(waits("x=$(/bin/echo hi)\n"), (std::vector<int>{0}));
}

// ONE PER OPERAND, and no `WUNTRACED` in either form of `wait`: XCU `wait` waits
// for TERMINATION, and a stopped background job has not terminated (#161).
TEST_F(CooperationTest, OneWaitPerWaitOperandAndNeverWUNTRACED) {
	// Two `&` children - which are not waited for at the `&` - and then one
	// `wait` per known child in the no-operand form.
	EXPECT_EQ(waits("/bin/echo a >/dev/null &\n/bin/echo b >/dev/null &\nwait\n"),
	          (std::vector<int>{0, 0}));
}

// The seam is never null and the default is `::waitpid`, so a shell that
// installed no host still runs externals correctly. Asserted through the STATUS,
// because a `reap` that answered without reaping would leave `$?` reading an
// uninitialised `wait_status`.
TEST_F(CooperationTest, TheNoOpWaitIsWaitpidAndTheStatusIsRight) {
	EXPECT_EQ(&state.cooperation(), &noop_cooperation());
	EXPECT_EQ(run("/bin/sh -c 'exit 3'\n"), 3);
	EXPECT_EQ(run("/bin/sh -c 'exit 0'\n"), 0);
}

// ---------------------------------------------------------------------------
// THE INPUT WAIT (#209, phase 2b of #145)
// ---------------------------------------------------------------------------
//
// Same discipline again: WHERE the runtime says it and HOW OFTEN, derived from
// the file rather than recorded from a run. `read_byte` is the one call site,
// and it says the verb before EACH of its one-byte reads - which is what makes
// the counts below arithmetic on the input rather than a number to be updated.

// A shell with no `read` in it never reaches the verb. The first assertion,
// because "one await per byte `read` consumes" is only a sentence if there is
// something it does not apply to.
TEST_F(CooperationTest, NothingButReadReachesTheInputWait) {
	EXPECT_TRUE(awaits(":\n:\n:\n", "").empty());
	EXPECT_TRUE(awaits("/bin/echo a >/dev/null\n", "").empty());
}

// ONE AWAIT PER BYTE, AND ALWAYS FD 0. `read x` on `ab\n` consumes three bytes -
// 'a', 'b' and the delimiter - so three awaits; the delimiter ends the line, so
// there is no fourth. The descriptor is `STDIN_FILENO` at every one of them,
// which is the half a host builds a poll set on.
TEST_F(CooperationTest, OneAwaitPerByteReadAndAlwaysOnFdZero) {
	EXPECT_EQ(awaits("read x\n", "ab\n"), (std::vector<int>{0, 0, 0}));
	EXPECT_EQ(awaits("read x\n", "\n"), (std::vector<int>{0}));
}

// EOF IS AN AWAIT TOO, and the one that would be tempting to skip: the read that
// returns 0 is still a read that could have blocked, so the host has to be told
// before it. `read x` on `a` with no newline consumes 'a' and then meets end of
// input - two awaits, and a status of 1.
TEST_F(CooperationTest, TheReadThatFindsEndOfInputWasAwaitedLikeAnyOther) {
	EXPECT_EQ(awaits("read x\n", "a"), (std::vector<int>{0, 0}));
	EXPECT_EQ(awaits("read x\n", ""), (std::vector<int>{0}));
}

// A LOOP IS BYTES ALL THE WAY DOWN, which is the property the whole verb rests
// on: `while read line` hands the host the thread once per byte, forever, and
// not once per line. Two lines of two characters plus their delimiters is six
// reads, then the seventh finds end of input.
TEST_F(CooperationTest, EachByteOfEachIterationOfAWhileReadIsAnAwait) {
	EXPECT_EQ(awaits("while read l; do :; done\n", "ab\ncd\n").size(), 7u);
}

// The seam moves NOTHING, and this is the assertion that says so: the same
// snippet through a host that counts and through the no-op leaves the same
// variables with the same values and the same status. `read` is a builtin with
// twelve field-splitting cases behind it, and a verb that transfers no bytes
// cannot have touched one of them.
TEST_F(CooperationTest, TheAwaitChangesNothingAboutWhatReadReads) {
	const auto value_of = [this](std::string_view name) {
		std::string_view text;
		return state.lookup(name, text) ? std::string{text} : std::string{"<unset>"};
	};

	(void)awaits("read a b\n", "one two three\n");
	EXPECT_EQ(value_of("a"), "one");
	EXPECT_EQ(value_of("b"), "two three");

	state.set_cooperation(noop_cooperation());
	host.awaits = 0;
	int fds[2] = {-1, -1};
	ASSERT_EQ(::pipe(fds), 0);
	const std::string_view text = "one two three\n";
	ASSERT_EQ(::write(fds[1], text.data(), text.size()),
	          static_cast<ssize_t>(text.size()));
	::close(fds[1]);
	const int saved = ::dup(STDIN_FILENO);
	::dup2(fds[0], STDIN_FILENO);
	::close(fds[0]);
	(void)run("read c d\n");
	::dup2(saved, STDIN_FILENO);
	::close(saved);

	EXPECT_EQ(value_of("c"), "one");
	EXPECT_EQ(value_of("d"), "two three");
	EXPECT_EQ(host.awaits, 0u) << "the no-op was installed and something still called out";
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

	// See `counting_cooperation`: the double really reaps, so what it proves is
	// about the seam and not about a stub.
	pid_t wait_child(pid_t pid, int flags, int* status) noexcept override {
		return ::waitpid(pid, status, flags);
	}

	// Nothing, for the reason the no-op says nothing: this test is about which
	// PROCESS reaches the seam, and no snippet in it reads.
	void await_readable(int) noexcept override {}

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
