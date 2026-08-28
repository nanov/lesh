#include "ui/history_search.h"
#include "ui/reactors.h"
#include "leshper/keymap.h"
#include "ui/loop.h"
#include "ui/shell_side.h"
#include "ui/tty.h"
#include "ui/reactor_call.h"
#include "substrate/fork_guard.h"
#include "substrate/log.h"

#include "interactive_signal_guard.h"
#include "temp_path.h"
#include "ui_fakes.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

using namespace lesh::leshper;
using namespace lesh::ui;
using lesh::testing::fake_tty;
namespace log = lesh::log;

// THE EVENT LOOP (#129): poll(2), five topics, quiesce.
//
// ONE THREAD SINCE #201. There is no `loop.start()` and no shell thread to
// spawn beside it: `run()` is a call on the calling thread, and the loop reaches
// the shell by calling `shell_side` - so a test that used to post to a slot,
// start the actor and wait for a byte now calls the loop and asserts on what
// came back. `fake_shell` is unchanged, which is the point of the interface.
//
// EVERY TEST HERE DRIVES FDS THE TEST OWNS. Pipes for the ordinary path,
// `openpty` where real termios is the point, and never the process's own
// terminal - which is #128's test contract and also self-defence: a test that
// got raw mode wrong on the real tty would leave the developer's shell unusable
// and the failure would be invisible in the gtest output.
//
// The dispositions are the other thing not to disturb. `signal_hub::deliver()`
// is the handler's entire body, so a test delivers to a hub it owns and no
// `sigaction` is anywhere near the binary's real state; the one test that DOES
// install guards with `saved_disposition`, which the shell's own signal tests
// have used since #37.

namespace {

// A loop over a pipe, with the terminal management off - there is nothing to
// manage, and leaving it on would put `tcsetpgrp` in the path of every test.
loop_options pipe_options() {
	loop_options options;
	options.manage_terminal = false;
	options.prompt = "> ";
	return options;
}

std::string buffer_of(const event_loop& loop) {
	return std::string{loop.editor().buffer.text()};
}

// --- A reactor, registered exactly as a plugin would ------------------------

std::atomic<int> g_reactor_runs{0};

int32_t counting_reactor(lesh_request* request, void* userdata) {
	g_reactor_runs.fetch_add(1, std::memory_order_relaxed);
	std::size_t length = 0;
	lesh_request_buffer_length(request, &length);
	if (length > 0)
		lesh_emit_span(request, 0, length, LESH_STYLE_NONE + 1);
	if (userdata != nullptr)
		*static_cast<std::size_t*>(userdata) = length;
	return LESH_OK;
}

// A REACTOR THAT YIELDS ONCE MID-COMPUTE. `lesh_request_superseded` is the
// cancellation poll, and since #202 the poll IS the yield - so one call hands the
// thread back to the host in the middle of the compute, which is the only moment
// at which the editor's generation can move between the snapshot a compute was
// handed and the batch it emits. That is N-4's drop rule in its real shape.
int32_t yielding_reactor(lesh_request* request, void* userdata) {
	int32_t superseded = 0;
	(void)lesh_request_superseded(request, &superseded);
	return counting_reactor(request, userdata);
}

// --- An action, for the timer topic ----------------------------------------

std::atomic<int> g_action_runs{0};

int32_t counting_action(lesh_editor*, const lesh_invocation*, void*) {
	g_action_runs.fetch_add(1, std::memory_order_relaxed);
	return LESH_OK;
}

int32_t exiting_action(lesh_editor* editor, const lesh_invocation*, void*) {
	return lesh_exit(editor, 7);
}

int32_t accepting_action(lesh_editor* editor, const lesh_invocation*, void*) {
	return lesh_accept_line(editor);
}

// --- The shell side (A-5), faked -------------------------------------------

class fake_shell : public shell_side {
public:
	std::int32_t execute(std::string_view line) override {
		executed.assign(line);
		if (on_execute)
			on_execute();
		return execute_status;
	}

	std::int32_t port_call(std::string_view code) override {
		called.assign(code);
		return port_status;
	}

	std::string executed;
	std::string called;
	std::int32_t execute_status = 0;
	std::int32_t port_status = 0;
	std::function<void()> on_execute;
};

// Runs `predicate` turns until it holds or the budget runs out. Bounded rather
// than a sleep: the shell thread and the worker pool are real threads here, and
// a fixed sleep is either flaky or slow.
template <typename Predicate>
bool turn_until(event_loop& loop, Predicate predicate, int budget = 200) {
	for (int i = 0; i < budget; ++i) {
		if (predicate())
			return true;
		loop.turn(5);
	}
	return predicate();
}

} // namespace

// ===========================================================================
// The tty topic
// ===========================================================================

TEST(UiLoopTty, BytesBecomeEditsInOneTurn) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	tty.type("hi");
	const turn_result result = loop.turn(50);

	EXPECT_EQ(buffer_of(loop), "hi");
	EXPECT_EQ(result.events, 2u);
	EXPECT_TRUE(result.rendered);
}

TEST(UiLoopTty, ReadsAreBatchedWhileTheFdIsStillReadable) {
	// #128's trap 4, fish's `read_normal_chars`: a paste is one edit and one
	// repaint. Two writes land before the turn, and both are consumed by it -
	// which is what the zero-timeout re-poll buys.
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	tty.type("abc");
	tty.type("def");
	const turn_result result = loop.turn(50);

	EXPECT_EQ(buffer_of(loop), "abcdef");
	EXPECT_EQ(result.events, 6u);
	// ONE repaint for the lot, which is the property the batching exists for.
	EXPECT_TRUE(result.rendered);
}

TEST(UiLoopTty, ABracketedPasteIsOneMutationAndOneGeneration) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	const std::uint64_t before = loop.editor().gen.value();
	tty.type("\x1b[200~hello world\x1b[201~");
	loop.turn(50);

	EXPECT_EQ(buffer_of(loop), "hello world");
	// F-6: one buffer mutation, one undo entry, ONE generation bump.
	EXPECT_EQ(loop.editor().gen.value(), before + 1);
}

TEST(UiLoopTty, EndOfFileIsAHangupAndNotAKey) {
	// fish `input.cpp`: EOF on the tty is `reader_sighup`, not a keypress.
	// Ctrl-D at an empty prompt is a BINDING on a real U+0004 and is a different
	// thing entirely.
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	tty.close_input();
	const turn_result result = loop.turn(50);

	EXPECT_TRUE(result.exiting);
	EXPECT_TRUE(loop.exiting());
	EXPECT_EQ(buffer_of(loop), "");
}

TEST(UiLoopTty, AnEscapeResolvesOnItsOwnDeadlineAndNotBefore) {
	fake_tty tty;
	loop_options options = pipe_options();
	options.escape_timeout = std::chrono::milliseconds{15};
	event_loop loop{tty.fds(), options};
	loop.enter_read();

	tty.type("\x1b");
	const turn_result held = loop.turn(0);
	EXPECT_EQ(held.events, 0u) << "a bare ESC is ambiguous until its timeout";
	// The timer topic IS the poll timeout, and this is the deadline in it.
	EXPECT_GT(loop.poll_timeout_ms(), 0);
	EXPECT_LE(loop.poll_timeout_ms(), 16);

	std::this_thread::sleep_for(std::chrono::milliseconds{25});
	const turn_result resolved = loop.turn(0);
	EXPECT_EQ(resolved.events, 1u);
}

// ===========================================================================
// The signal topic
// ===========================================================================

TEST(UiLoopSignals, ResizesAreCountedRatherThanQueued) {
	// #128's trap 12: SIGWINCH bumps a counter and the size is read from the
	// kernel. Queueing resizes would be queueing sizes that were stale when they
	// were queued.
	signal_hub hub;
	EXPECT_EQ(hub.resize_count(), 0u);

	hub.deliver(SIGWINCH);
	hub.deliver(SIGWINCH);
	hub.deliver(SIGWINCH);
	EXPECT_EQ(hub.resize_count(), 3u);

	std::vector<int> pending;
	EXPECT_EQ(hub.drain(pending), 0u) << "SIGWINCH is the counter, never a queued number";
	EXPECT_TRUE(pending.empty());
}

TEST(UiLoopSignals, DrainConsumesTheByteAndThePendingSet) {
	signal_hub hub;
	hub.deliver(SIGINT);
	hub.deliver(SIGINT);
	hub.deliver(SIGCHLD);

	std::vector<int> pending;
	EXPECT_EQ(hub.drain(pending), 2u) << "the pending set is a set, not a count";
	EXPECT_EQ(pending.size(), 2u);

	// The wakeup fd is disarmed by the same call: a poll now finds nothing.
	struct pollfd watch{};
	watch.fd = hub.wakeup_fd();
	watch.events = POLLIN;
	EXPECT_EQ(::poll(&watch, 1, 0), 0);

	pending.clear();
	EXPECT_EQ(hub.drain(pending), 0u);
}

TEST(UiLoopSignals, ASignalBecomesAnEventOnTheOrdinaryPath) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	loop.signals().deliver(SIGINT);
	const turn_result result = loop.turn(50);

	// A-9: the entrance exists. What SIGINT is BOUND to is keymap data (#93),
	// and this ticket binds it to nothing.
	EXPECT_EQ(result.events, 1u);
	EXPECT_GE(result.topics_drained, 1u);
}

TEST(UiLoopSignals, AResizeIsDeliveredAsAnEventWithTheQueriedSize) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	// A pipe has no winsize, so the query falls back - 80x24, the answer
	// `kFallbackTerminalSize` names.
	loop.editor().columns = 0;
	loop.editor().rows = 0;
	loop.signals().deliver(SIGWINCH);
	const turn_result result = loop.turn(50);

	EXPECT_EQ(result.events, 1u);
	EXPECT_EQ(loop.editor().columns, kFallbackTerminalSize.columns);
	EXPECT_EQ(loop.editor().rows, kFallbackTerminalSize.rows);
}

TEST(UiLoopSignals, ASignalDoesNotTearAMultibyteSequence) {
	// #128's trap 2, and fish's never-fixed FIXME: "here signals may break
	// multibyte sequences." An interrupt injected an event ahead of a half-read
	// UTF-8 sequence.
	//
	// It cannot happen here, and the reason is structural rather than careful:
	// the incomplete prefix lives INSIDE the decoder (#111), so an injected
	// event is appended after whatever the bytes in hand completed - which for
	// half a codepoint is nothing at all. The codepoint arrives whole on the
	// turn its last byte does.
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	// U+00E9, LATIN SMALL LETTER E WITH ACUTE: 0xC3 0xA9.
	tty.type("\xC3");
	EXPECT_EQ(loop.turn(50).events, 0u) << "half a codepoint completes nothing";
	EXPECT_EQ(buffer_of(loop), "");

	loop.signals().deliver(SIGWINCH);
	const turn_result torn = loop.turn(50);
	EXPECT_EQ(torn.events, 1u) << "the resize, and nothing from the held byte";
	EXPECT_EQ(buffer_of(loop), "");

	tty.type("\xA9");
	EXPECT_EQ(loop.turn(50).events, 1u);
	EXPECT_EQ(buffer_of(loop), "\xC3\xA9") << "the codepoint survived the signal intact";
}

TEST(UiLoopSignals, InstallPutsBackExactlyWhatItReplaced) {
	// The one test that touches the process's real dispositions, guarded the way
	// the shell's own signal tests have been since #37.
	lesh::testing::saved_disposition interrupt{SIGINT};
	lesh::testing::saved_disposition quit{SIGQUIT};
	lesh::testing::saved_disposition child{SIGCHLD};
	lesh::testing::saved_disposition winch{SIGWINCH};
	lesh::testing::saved_disposition pipe{SIGPIPE};
	lesh::testing::saved_disposition tstp{SIGTSTP};
	lesh::testing::saved_disposition ttou{SIGTTOU};
	lesh::testing::saved_disposition ttin{SIGTTIN};
	interrupt.default_action();
	quit.default_action();

	{
		signal_hub hub;
		ASSERT_TRUE(hub.install());
		EXPECT_TRUE(hub.installed());

		struct sigaction current{};
		ASSERT_EQ(::sigaction(SIGINT, nullptr, &current), 0);
		EXPECT_NE(current.sa_handler, SIG_DFL);
		// SIGINT WITHOUT SA_RESTART: the poll must be interrupted.
		EXPECT_EQ(current.sa_flags & SA_RESTART, 0);

		ASSERT_EQ(::sigaction(SIGCHLD, nullptr, &current), 0);
		// SIGCHLD WITH it: "we want SIGCHLD to not interrupt restartable
		// syscalls" (fish `signal.cpp`).
		EXPECT_NE(current.sa_flags & SA_RESTART, 0);

		ASSERT_EQ(::sigaction(SIGQUIT, nullptr, &current), 0);
		EXPECT_EQ(current.sa_handler, SIG_IGN);
		ASSERT_EQ(::sigaction(SIGTSTP, nullptr, &current), 0);
		EXPECT_EQ(current.sa_handler, SIG_IGN);

		hub.uninstall();
	}

	struct sigaction after{};
	ASSERT_EQ(::sigaction(SIGINT, nullptr, &after), 0);
	EXPECT_EQ(after.sa_handler, SIG_DFL) << "uninstall restores what install saved";
}

// ---------------------------------------------------------------------------
// Ownership: what the hub takes, and what it does not (#142)
// ---------------------------------------------------------------------------

namespace {

// Every disposition an installed hub can touch, restored on the way out. One
// guard per signal was not enough in the shell's own signal tests (#52) and it
// is not enough here either: a hub takes eight signals at once, and a test that
// guarded three would leave the other five as this hub left them for the whole
// rest of the binary.
class every_hub_disposition {
public:
	[[nodiscard]] const lesh::testing::saved_disposition& hangup() const { return _guards[0]; }
	[[nodiscard]] const lesh::testing::saved_disposition& child() const { return _guards[3]; }
	[[nodiscard]] const lesh::testing::saved_disposition& stop() const { return _guards[6]; }

private:
	std::array<lesh::testing::saved_disposition, 9> _guards{
		lesh::testing::saved_disposition{SIGHUP},   lesh::testing::saved_disposition{SIGINT},
		lesh::testing::saved_disposition{SIGQUIT},  lesh::testing::saved_disposition{SIGCHLD},
		lesh::testing::saved_disposition{SIGWINCH}, lesh::testing::saved_disposition{SIGPIPE},
		lesh::testing::saved_disposition{SIGTSTP},  lesh::testing::saved_disposition{SIGTTOU},
		lesh::testing::saved_disposition{SIGTTIN}};
};

// What a `trap` installs, as far as the hub can tell: a real function that is
// not ours. `runtime/signals.cpp`'s `record_signal` is the only one that ever
// appears in a real lesh process, and it is this shape.
volatile sig_atomic_t g_trap_handler_ran = 0;

void trap_style_handler(int) { g_trap_handler_ran = 1; }

void install_handler(int signo, void (*fn)(int)) {
	struct sigaction sa{};
	sa.sa_handler = fn;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	ASSERT_EQ(::sigaction(signo, &sa, nullptr), 0);
}

[[nodiscard]] struct sigaction disposition_of(int signo) {
	struct sigaction current{};
	sigaction(signo, nullptr, &current);
	return current;
}

} // namespace

TEST(UiLoopSignals, ReassertOverAForeignHandlerRetargetsTheChain) {
	// Rule 4a, and the defect it fixes. `trap 'cmd' CHLD` installs
	// `record_signal`; the old save-once hub stomped it and went on chaining to
	// what was there BEFORE the trap existed, so `g_pending[CHLD]` was never set
	// and the trap body never ran - not while editing, not during a command.
	const every_hub_disposition guards;
	guards.child().default_action();

	signal_hub hub;
	ASSERT_TRUE(hub.install());

	// The trap, typed after the hub took the disposition.
	install_handler(SIGCHLD, trap_style_handler);
	EXPECT_TRUE(hub.reassert());

	// Taken back - the wakeup is genuinely ours, because a job notice has to
	// reach a loop blocked in `poll`.
	EXPECT_NE(disposition_of(SIGCHLD).sa_handler, trap_style_handler)
		<< "reassert left SIGCHLD with the trap's handler and lost the wakeup";

	g_trap_handler_ran = 0;
	hub.deliver(SIGCHLD);
	EXPECT_EQ(g_trap_handler_ran, 1) << "the chain still points at the pre-trap disposition";

	hub.uninstall();
}

TEST(UiLoopSignals, ReassertOverAForeignWinchHandlerRetargetsTheChainToo) {
	// The same rule on the other caught signal that a `trap` can reach, because
	// "INT works" was how the CHLD defect stayed invisible: INT escaped only by
	// the accident that the interactive default had installed `record_signal`
	// before the hub's first take.
	const every_hub_disposition guards;

	signal_hub hub;
	ASSERT_TRUE(hub.install());
	install_handler(SIGWINCH, trap_style_handler);
	EXPECT_TRUE(hub.reassert());

	g_trap_handler_ran = 0;
	hub.deliver(SIGWINCH);
	EXPECT_EQ(g_trap_handler_ran, 1);
	// And the hub's own work happened first: the counter, never a queue.
	EXPECT_EQ(hub.resize_count(), 1u);

	hub.uninstall();
}

TEST(UiLoopSignals, ReassertLeavesAnIgnoreStanding) {
	// Rule 3. The newest ignore stands, whoever set it - `nohup`'s before exec
	// or `trap '' CHLD` a moment ago. This is what used to be a SIGHUP-shaped
	// special case, generalized into the rule that governs every signal.
	const every_hub_disposition guards;
	guards.child().default_action();

	signal_hub hub;
	ASSERT_TRUE(hub.install());
	ASSERT_NE(disposition_of(SIGCHLD).sa_handler, SIG_IGN) << "the hub never took SIGCHLD";

	guards.child().ignore();
	EXPECT_TRUE(hub.reassert());
	EXPECT_EQ(disposition_of(SIGCHLD).sa_handler, SIG_IGN)
		<< "reassert stomped an ignore the user asked for";

	hub.uninstall();
}

TEST(UiLoopSignals, ReassertLeavesAForeignHandlerOnTheIgnoredSetStanding) {
	// Rule 4b, and the second defect. The hub never CONSUMED SIGTSTP - its
	// SIG_IGN was only "better than the default action" - so a user's
	// `trap 'cmd' TSTP` outranks it. The old hub re-ignored unconditionally and
	// the trap was not one level removed but erased.
	const every_hub_disposition guards;
	guards.stop().default_action();

	signal_hub hub;
	ASSERT_TRUE(hub.install());
	ASSERT_EQ(disposition_of(SIGTSTP).sa_handler, SIG_IGN);

	install_handler(SIGTSTP, trap_style_handler);
	EXPECT_TRUE(hub.reassert());
	EXPECT_EQ(disposition_of(SIGTSTP).sa_handler, trap_style_handler)
		<< "reassert erased a TSTP trap";

	hub.uninstall();
}

TEST(UiLoopSignals, UninstallRestoresTheEntryDispositionAndNotTheNewest) {
	// The reason `_saved` and `_chain` are two slots. `_chain` follows the newest
	// handler so the trap fires; `_saved` must not, or leaving the editor would
	// hand the process back a disposition it never started with.
	const every_hub_disposition guards;
	guards.child().default_action();

	{
		signal_hub hub;
		ASSERT_TRUE(hub.install());
		install_handler(SIGCHLD, trap_style_handler);
		EXPECT_TRUE(hub.reassert());
		hub.uninstall();
	}

	EXPECT_EQ(disposition_of(SIGCHLD).sa_handler, SIG_DFL)
		<< "uninstall put back the trap's handler rather than the entry disposition";
}

TEST(UiLoopSignals, TheLoopNeverWritesADisposition) {
	// #142's second amendment, as an assertion rather than a convention. Taking
	// the dispositions back used to happen on the LOOP thread, in `enter_read`
	// and on the unpark - two threads writing one piece of process-wide state,
	// the other being the shell thread's `trap` builtin. The re-assert moved to
	// the shell side of the ui layer (`ui/session.cpp`), and what is left here must
	// call no `sigaction` at all: a foreign handler installed after `install()`
	// survives a read entry and a turn untouched.
	const every_hub_disposition guards;
	guards.child().default_action();

	fake_tty tty;
	// THE HUB IS STILL DECLARED BEFORE THE LOOP. It had to be while `~event_loop`
	// called `request_stop` and `request_stop` poked the attached hub's pipe - a
	// hub declared after the loop died first and that poke was a use after scope,
	// which ASan said immediately and is how this line got written. #201 deleted
	// both the destructor's call and the poke; the declaration order stays,
	// because the loop borrows the hub either way and a borrowed thing outliving
	// its borrower is not a rule worth re-deriving per member.
	signal_hub hub;
	event_loop loop{tty.fds(), pipe_options()};
	ASSERT_TRUE(hub.install());
	loop.attach_signals(hub);

	install_handler(SIGCHLD, trap_style_handler);
	loop.enter_read();
	tty.type("x");
	EXPECT_EQ(loop.turn(50).events, 1u);
	loop.leave_read();

	EXPECT_EQ(disposition_of(SIGCHLD).sa_handler, trap_style_handler)
		<< "the loop thread wrote a disposition";

	hub.uninstall();
}

TEST(UiLoopSignals, SighupIsNeverTakenAtAll) {
	// #142 removed SIGHUP from the hub entirely, and this test is the guard on
	// that decision rather than on the old `nohup` conditional. The editor's
	// hangup is the tty's POLLHUP, which `drain_tty` synthesizes; a real SIGHUP
	// is the shell's own business, so `trap - HUP` is fatal as POSIX says and
	// `nohup`'s ignore is respected by construction. BOTH entry states are
	// checked, because the old conditional got the ignore right and the default
	// wrong - and the default is the one that swallowed a `kill -HUP`.
	{
		const every_hub_disposition guards;
		guards.hangup().ignore();
		signal_hub hub;
		ASSERT_TRUE(hub.install());
		EXPECT_EQ(disposition_of(SIGHUP).sa_handler, SIG_IGN) << "nohup is respected";
		EXPECT_TRUE(hub.reassert());
		EXPECT_EQ(disposition_of(SIGHUP).sa_handler, SIG_IGN);
		hub.uninstall();
	}
	{
		const every_hub_disposition guards;
		guards.hangup().default_action();
		signal_hub hub;
		ASSERT_TRUE(hub.install());
		EXPECT_EQ(disposition_of(SIGHUP).sa_handler, SIG_DFL)
			<< "the hub caught a default-fatal SIGHUP and swallowed it";
		EXPECT_TRUE(hub.reassert());
		EXPECT_EQ(disposition_of(SIGHUP).sa_handler, SIG_DFL);
		hub.uninstall();
	}
}

// ===========================================================================
// The reactors (#202: fibers, where this section used to be the `worker` topic)
// ===========================================================================

TEST(UiLoopReactors, AnEmissionLandsInTheEditorsOwnDecorations) {
	// What the `worker` topic's drain rule used to be asserted through. There is
	// no descriptor and no queue: the reactor's fiber applies its own batch inside
	// the turn, so the assertion is about where the answer LANDS, which is the
	// part that never depended on how it travelled.
	fake_tty tty;
	registry reg;
	std::size_t seen = 0;
	ASSERT_EQ(lesh_reactor_register(&reg, "counter", LESH_EVENT_BUFFER_CHANGED,
	                                &counting_reactor, &seen),
	          LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.enter_read();

	tty.type("ab");
	loop.turn(50);

	ASSERT_TRUE(turn_until(loop, [&] { return loop.applied_batches() > 0; }));
	EXPECT_EQ(seen, 2u) << "the compute saw the whole line";
	// #141: a taken batch lands in the editor's own decorations, namespaced by
	// the reactor that emitted it. There is no loop-side store any more.
	ASSERT_EQ(loop.editor().marks.layers().size(), 1u);
	EXPECT_EQ(loop.editor().marks.layers().front().reactor, "counter");
}

TEST(UiLoopReactors, ABatchComputedAgainstAnOlderGenerationIsDropped) {
	// N-4, and the loop is the only applier, so this is the only place the rule is
	// decided. The staleness is arranged MID-COMPUTE: the reactor yields at its
	// cancellation poll, the host moves the editor's generation on while the fiber
	// is suspended there, and the batch that comes back is about a buffer the
	// editor has left behind.
	//
	// THIS USED TO USE `quiesce()` TO PARK THE GROUP, and #208 took that away: the
	// tty topic is out of the poll set while `executing`, so a keystroke typed
	// after a `quiesce` is not read at all - which is the point of the exclusion,
	// the terminal being the running command's. The yield is a better arrangement
	// anyway: a fiber suspended in the middle of a walk is the production shape,
	// where a parked group with a queued wake was a stand-in for it.
	fake_tty tty;
	registry reg;
	ASSERT_EQ(lesh_reactor_register(&reg, "counter", LESH_EVENT_BUFFER_CHANGED,
	                                &yielding_reactor, nullptr),
	          LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.enter_read();

	tty.type("a");
	loop.turn(50);
	// The fiber took the notification, started computing and yielded at its poll:
	// runnable, not finished, and nothing applied yet.
	ASSERT_TRUE(loop.reactors().runnable(group_mask(fiber_group::emitters)))
		<< "the reactor should be suspended mid-compute at its cancellation poll";
	ASSERT_EQ(loop.applied_batches(), 0u);

	loop.editor().gen.bump();

	ASSERT_TRUE(turn_until(loop, [&] { return loop.dropped_batches() > 0; }));
	EXPECT_EQ(loop.applied_batches(), 0u);
	EXPECT_TRUE(loop.editor().marks.layers().empty());
}

TEST(UiLoopReactors, AcceptingAnAutosuggestionOnTheRealLoopCommitsTheLine) {
	// #154's regression anchor for F-25 on the REAL loop path - the deterministic
	// in-harness cousin of the pty accept test, with no terminal timing in it.
	// The autosuggester runs on ITS OWN FIBER since #202 (it was a helper worker,
	// and before that a pool submission); the whole point of the ticket is that its
	// proposal, not only its virtual text, reaches `state::proposals` where
	// `lesh_proposal_read` walks. Type a prefix, let the batch land, dispatch the
	// DEFAULT accept key, and the buffer must become the whole candidate with one
	// undo entry for the accept.
	//
	// The unit suite drove the accepting actions through `loop_harness::react` +
	// `apply_batch` - a fake scheduler on the test thread - so it never exercised
	// the real notify-compute-apply path end to end. This does, which is the seam
	// #154 was filed against.
	fake_tty tty;
	registry reg;
	vector_history_source history{{"echo hello"}};
	owned_autosuggester self{&history};
	ASSERT_EQ(register_autosuggester(reg, self.get()), 1u);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.enter_read();

	// Type a prefix of the remembered line and let the reactor's batch arrive.
	tty.type("ec");
	ASSERT_TRUE(turn_until(loop, [&] {
		return loop.applied_batches() > 0 && !loop.editor().proposals.empty();
	})) << "the autosuggester's proposal never reached state.proposals";

	// The proposal an accepting action would read is the WHOLE candidate.
	const proposal* offer =
		loop.editor().proposals.find(LESH_PROPOSAL_AUTOSUGGESTION, 0);
	ASSERT_NE(offer, nullptr);
	EXPECT_EQ(offer->bytes, "echo hello");

	// Ctrl-F is the emacs default `accept_suggestion_or_forward_char`, chosen
	// over `<Right>` because it is a single byte with no escape-timing to wait
	// out - the cursor is at the end and a suggestion is showing, so it accepts.
	tty.type("\x06");
	ASSERT_TRUE(turn_until(loop, [&] {
		return std::string{loop.editor().buffer.text()} == "echo hello";
	})) << "the accept never committed; buffer=" << buffer_of(loop);

	// One undo entry for the accept: the typing run is its own step and the
	// accept broke it and added a second (undo.h: every non-inserting action ends
	// the run), so undo puts back exactly what was typed.
	EXPECT_EQ(loop.editor().undo.step_count(), 2u);
	ASSERT_EQ(context_of(loop.editor()).loop().invoke(loop.editor(), "undo", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(buffer_of(loop), "ec");
}

// ===========================================================================
// The shell, called directly (ADR-0009 as amended by #201)
// ===========================================================================

TEST(UiLoopShell, TheShellStateReactorRunsInPlaceAndLandsWithinTheTurn) {
	// ADR-0009's keystone, with the thread taken out of it (#201): the reactor
	// that reads the alias, function and builtin tables runs on the thread that
	// owns them, and that is this one. What used to be post-serve-reply-drain
	// across a pipe is a call inside `notify_reactors`, so the batch is applied
	// before the turn that produced the keystroke returns - one turn, not two.
	fake_tty tty;
	registry reg;
	fake_shell shell;
	ASSERT_EQ(lesh_reactor_register(&reg, "highlighter", LESH_EVENT_BUFFER_CHANGED,
	                                &counting_reactor, nullptr),
	          LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.attach_shell(shell);
	loop.enter_read();

	tty.type("x");
	loop.turn(50);

	EXPECT_EQ(loop.applied_batches(), 1u) << "the same turn applied it";
	EXPECT_EQ(loop.dropped_batches(), 0u);
	ASSERT_EQ(loop.editor().marks.layers().size(), 1u);
	EXPECT_EQ(loop.editor().marks.layers().front().reactor, "highlighter");
}

TEST(UiLoopShell, EveryKeystrokesHighlightIsFinishedBeforeTheNextOneIsRead) {
	// WHAT REPLACED LATEST-WINS. ADR-0009 gave the `highlight` slot depth one and
	// said "a newer highlight overwrites a pending one, which is the
	// cancellation"; with the reactor run in place there is never a pending one to
	// overwrite, because the call returns before the loop can read the next key.
	// Five keystrokes are five batches applied and none dropped - and the store
	// still holds one layer, because latest-wins lives there and always did.
	//
	// THE COST OF THIS IS THE NEXT TICKET'S. A reactor that walks `$PATH` holds
	// the keystroke it was computed for until it returns; the fiber step is what
	// gives the walk a yield point back.
	fake_tty tty;
	registry reg;
	fake_shell shell;
	ASSERT_EQ(lesh_reactor_register(&reg, "highlighter", LESH_EVENT_BUFFER_CHANGED,
	                                &counting_reactor, nullptr),
	          LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.attach_shell(shell);
	loop.enter_read();

	for (const char* key : {"e", "c", "h", "o", " "}) {
		tty.type(key);
		loop.turn(50);
	}

	EXPECT_EQ(loop.applied_batches(), 5u);
	EXPECT_EQ(loop.dropped_batches(), 0u);
	EXPECT_EQ(loop.editor().marks.layers().size(), 1u);
	EXPECT_EQ(buffer_of(loop), "echo ");
}

TEST(UiLoopShell, AcceptCallsExecuteOnThisThreadBeforeTheTurnReturns) {
	// THE WHOLE OF #201 IN ONE ASSERTION. `execute` used to be a message filled in
	// on this thread and run on another, with the loop blocked in a second poll
	// until the reply came back. It is a call now: it happens inside the turn that
	// carried the accept, on the thread that made it.
	fake_tty tty;
	fake_shell shell;
	shell.execute_status = 42;

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_shell(shell);
	loop.enter_read();

	// Enter is the session's binding, not a default (F-35), so the accepting key
	// is bound here the way `ui_session_tests.cpp` binds one.
	editing_context& context = context_of(loop.editor());
	ASSERT_EQ(lesh_action_register(&context.actions(), "ask_accept", &accepting_action, nullptr),
	          LESH_OK);
	keymap* map = context.keymaps().find(keymap_registry::emacs);
	ASSERT_NE(map, nullptr);
	std::string encoded;
	ASSERT_TRUE(parse_key_notation("<C-a>", encoded));
	map->bind(encoded, "ask_accept");

	std::thread::id ran_on;
	shell.on_execute = [&] { ran_on = std::this_thread::get_id(); };

	tty.type("echo hi\x01");
	const turn_result result = loop.turn(50);

	EXPECT_EQ(shell.executed, "echo hi") << "no second turn was needed";
	EXPECT_EQ(ran_on, std::this_thread::get_id()) << "and no second thread";
	EXPECT_EQ(loop.exit_status(), 42);
	EXPECT_EQ(buffer_of(loop), "") << "the line is finished and the editor is fresh";
	EXPECT_FALSE(result.exiting);
}

TEST(UiLoopShell, APortCallIsSynchronousFromTheActionsPointOfView) {
	// #92's contract, and the implementation change #92 predicted, twice: ADR-0009
	// made it a cross-thread round trip, #201 made it a call. The action blocks
	// either way, and the terminal keeps the EDITOR's modes throughout (fish
	// #7770).
	fake_tty tty;
	fake_shell shell;
	shell.port_status = 3;

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_shell(shell);
	loop.enter_read();

	const port_result answered = loop.call_port("echo from an action");

	EXPECT_TRUE(answered.answered);
	EXPECT_EQ(answered.status, 3);
	EXPECT_EQ(shell.called, "echo from an action");
}

TEST(UiLoopShell, ASixteenKeyTurnWithAnAcceptInTheMiddleLosesNothing) {
	// WHAT #162 LEFT BEHIND, AND WHERE THE HAZARD WENT (#202). #162 was a
	// heap-use-after-free: the turn walked `_events` by reference while `handle`
	// pushed onto it, and the push reallocated the vector out from under the walk.
	// Two producers found it in turn - a shell message drained inside
	// `wait_on_shell` (deleted by #201) and the in-place shell reactor's own
	// `worker_result` (deleted here) - and the reactor's push now happens from a
	// FIBER SLICE, which `turn` runs between its two event walks rather than inside
	// one. So the hazard has no producer left; the swap stays as the rule, and this
	// is the test that says the SEQUENCE is still lossless.
	//
	// The arithmetic is still the anchor. `event_loop` reserves exactly sixteen
	// events, so sixteen bytes read in one go fill the queue to its capacity - and
	// the accept is in the MIDDLE of them, so the walk still has elements to
	// dereference after a `quiesce` has parked the emitters group underneath it.
	fake_tty tty;
	registry reg;
	fake_shell shell;
	// DECLARED BEFORE THE LOOP so it outlives it: `~event_loop` runs the emitters
	// out to their next poll, and that poll reads the reactor's userdata.
	std::size_t seen_length = 0;
	ASSERT_EQ(lesh_reactor_register(&reg, "highlighter", LESH_EVENT_BUFFER_CHANGED,
	                                &counting_reactor, &seen_length),
	          LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.attach_shell(shell);
	loop.enter_read();

	editing_context& context = context_of(loop.editor());
	ASSERT_EQ(lesh_action_register(&context.actions(), "ask_accept", &accepting_action, nullptr),
	          LESH_OK);
	keymap* map = context.keymaps().find(keymap_registry::emacs);
	ASSERT_NE(map, nullptr);
	std::string encoded;
	ASSERT_TRUE(parse_key_notation("<C-a>", encoded));
	map->bind(encoded, "ask_accept");

	tty.type("12345678\x01" "abcdefg");
	const turn_result result = loop.turn(50);

	EXPECT_EQ(shell.executed, "12345678");
	EXPECT_EQ(buffer_of(loop), "abcdefg")
		<< "the seven keys typed after the accept still reached the fresh line";
	// SIXTEEN KEYS PLUS ONE. Fifteen of the sixteen change the buffer - the accept
	// itself changes none - and all fifteen send into a capacity-one conflating
	// slot, so there is ONE compute and ONE `worker_result`, pushed by the trailing
	// slice and walked by the pass after it. It was 31 and 15 when every keystroke
	// had a worker of its own; the drop to 17 and 1 is latest-wins arriving where
	// #90 always said it should.
	EXPECT_EQ(result.events, 17u)
		<< "the event a trailing slice pushed was dropped rather than walked";
	EXPECT_EQ(loop.applied_batches(), 1u);
	// AND THE ONE COMPUTE IS FOR THE LINE THAT SURVIVED. Seven of the fifteen sends
	// landed while the group was parked for the execution; the resume replayed the
	// wake, and what the fiber then received was the newest of them.
	ASSERT_EQ(loop.editor().marks.layers().size(), 1u);
	EXPECT_EQ(loop.editor().marks.layers().front().reactor, "highlighter");
	EXPECT_EQ(seen_length, 7u) << "the batch was computed for `abcdefg`";
}

// ===========================================================================
// Accept and quiesce
// ===========================================================================

TEST(UiLoopQuiesce, AcceptParksTheEmittersBeforeTheShellRuns) {
	// The whole of quiesce, asserted from inside the execution: by the time
	// `execute` runs, the emitters group is parked and the terminal has been handed
	// back. That is the moment a fork is legal - and since #201 the fork happens
	// one stack frame below this assertion rather than on another thread, which
	// makes the ordering the call's own. #202 turned "the helper pool is parked"
	// into one scheduler bit and left the ordering alone.
	fake_tty tty;
	fake_shell shell;

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_shell(shell);
	loop.enter_read();

	bool parked_during_execute = false;
	shell.on_execute = [&] {
		parked_during_execute =
			loop.reactors().group_parked(group_index(fiber_group::emitters));
	};
	shell.execute_status = 42;

	tty.type("echo hi");
	loop.turn(50);
	ASSERT_EQ(buffer_of(loop), "echo hi");

	const std::optional<std::int32_t> status = loop.accept_current_line();

	EXPECT_TRUE(parked_during_execute) << "quiesce is the emitters parked plus the terminal";
	ASSERT_TRUE(status.has_value());
	EXPECT_EQ(*status, 42);
	EXPECT_EQ(shell.executed, "echo hi");
	// The line is finished and the editor is fresh, in one edit so undo does not
	// walk back into a command that has already run.
	EXPECT_EQ(buffer_of(loop), "");
	// And the resume released it: the group is runnable again.
	EXPECT_FALSE(loop.reactors().group_parked(group_index(fiber_group::emitters)));
	EXPECT_FALSE(loop.quiesced());
}

TEST(UiLoopQuiesce, QuiesceIsIdempotentAndOneResumeUndoesIt) {
	// #203: the depth counter is a bit. Two parks used to need two resumes, which
	// was #91's apparatus for a set of threads that could each ask for one; the
	// two callers are `accept_current_line` and `finish_cancelled_line` and
	// neither is reachable from inside the other, so what a second call means now
	// is "nothing to do".
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	loop.quiesce();
	EXPECT_TRUE(loop.quiesced());
	loop.assert_quiesced();
	loop.quiesce();
	EXPECT_TRUE(loop.quiesced()) << "a second park is a no-op, not a second park";
	loop.assert_quiesced();

	loop.resume_after_execution();
	EXPECT_FALSE(loop.quiesced()) << "one resume undoes both calls";
	EXPECT_FALSE(loop.reactors().group_parked(group_index(fiber_group::emitters)));
}

TEST(UiLoopQuiesce, ASignalArrivingDuringExecutionIsNotLost) {
	// #201 moved the mechanism and kept the fact. The loop used to be blocked in a
	// second poll over the `shell` and `signal` topics for the whole execution and
	// pushed what arrived onto `_deferred`; now nothing polls while `execute` runs,
	// and what holds the signal is the self-pipe byte the handler wrote. Either
	// way the next ordinary turn delivers it: nothing is dropped because the
	// editor was not there to receive it.
	fake_tty tty;
	fake_shell shell;

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_shell(shell);
	loop.enter_read();

	// Delivered from inside `execute`, which is the window where the editor does
	// not exist as far as the terminal is concerned.
	shell.on_execute = [&] { loop.signals().deliver(SIGINT); };

	loop.accept_current_line();

	const turn_result result = loop.turn(0);
	EXPECT_EQ(result.events, 1u);
}

// ===========================================================================
// The timer topic
// ===========================================================================

TEST(UiLoopTimers, AnArmedTimerDispatchesItsAction) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	// THE STATE'S OWN REGISTRY, which is what the session attaches and what a
	// keystroke reaches (#144). It matters here since #168: an expiry is a
	// `timer_fired` EVENT and `step` dispatches it through `context_of(state)`, so
	// a timer armed on some other registry is a timer whose action nothing can
	// find - which is the same thing a key bound to an unregistered name is.
	registry& reg = context_of(loop.editor()).actions();
	ASSERT_EQ(lesh_action_register(&reg, "tick", &counting_action, nullptr), LESH_OK);

	std::uint64_t id = 0;
	ASSERT_EQ(lesh_timer_start(&reg, 5, "tick", &id), LESH_OK);
	EXPECT_NE(id, 0u);

	loop.attach_registry(reg);
	loop.enter_read();

	const int before = g_action_runs.load();
	ASSERT_TRUE(turn_until(loop, [&] { return g_action_runs.load() > before; }));
	EXPECT_GE(loop.timer_dispatches(), 1u);

	EXPECT_EQ(lesh_timer_stop(&reg, id), LESH_OK);
	EXPECT_EQ(lesh_timer_stop(&reg, id), LESH_ERR_NOTFOUND) << "an id is stopped once";
}

TEST(UiLoopTimers, TheTimeoutIsTheMinimumDeadlineAndMinusOneWhenNothingWaits) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	registry& reg = context_of(loop.editor()).actions();
	loop.attach_registry(reg);
	loop.enter_read();

	// Unarmed, the mechanism costs nothing: the loop blocks.
	EXPECT_EQ(loop.poll_timeout_ms(), -1);

	std::uint64_t id = 0;
	ASSERT_EQ(lesh_timer_start(&reg, 1000, "tick", &id), LESH_OK);
	loop.turn(0);  // the arm is an effect, taken at the top of a turn
	const int timeout = loop.poll_timeout_ms();
	EXPECT_GT(timeout, 0);
	EXPECT_LE(timeout, 1001);
}

TEST(UiLoopTimers, ZeroIntervalAndABadNameAreRefused) {
	registry reg;
	std::uint64_t id = 0;
	EXPECT_EQ(lesh_timer_start(&reg, 0, "tick", &id), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_timer_start(&reg, 5, "NotSnakeCase", &id), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_timer_start(nullptr, 5, "tick", &id), LESH_ERR_INVAL);
	// Nothing was minted and nothing was queued: a refused arm leaves neither an
	// id behind nor an `arm_timer` for the host to act on (#168).
	EXPECT_TRUE(reg.armed_timers.empty());
	EXPECT_TRUE(reg.pending.empty());
}

TEST(UiLoopTimers, AnActionsExitOutcomeEndsTheLoop) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	registry& reg = context_of(loop.editor()).actions();
	ASSERT_EQ(lesh_action_register(&reg, "quit", &exiting_action, nullptr), LESH_OK);
	std::uint64_t id = 0;
	ASSERT_EQ(lesh_timer_start(&reg, 1, "quit", &id), LESH_OK);

	loop.attach_registry(reg);
	loop.enter_read();

	ASSERT_TRUE(turn_until(loop, [&] { return loop.exiting(); }));
	EXPECT_EQ(loop.exit_status(), 7);
}

// ===========================================================================
// Rendering
// ===========================================================================

TEST(UiLoopRender, TheLoopKeepsThePreviousSurfaceAndDiffsAgainstIt) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();
	loop.render();
	const std::string first = tty.painted();
	EXPECT_FALSE(first.empty()) << "the first render is a full paint";

	tty.type("a");
	loop.turn(50);
	const std::string second = tty.painted();
	EXPECT_FALSE(second.empty());
	// A one-character edit is a much smaller update than a repaint of the whole
	// surface - which is what keeping `previous` here buys.
	EXPECT_LT(second.size(), first.size());
}

TEST(UiLoopRender, AResizeForcesAFullRepaint) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();
	tty.type("hello");
	loop.turn(50);
	(void)tty.painted();

	// The size the pipe reports never changes, so drive the invalidation the way
	// a resize does: the previous surface is a different shape and cannot be
	// diffed against.
	loop.editor().columns = 40;
	loop.invalidate();
	loop.render();
	EXPECT_FALSE(tty.painted().empty());
}

// #185: a resize used to paint the new frame from wherever the cursor was - the
// end of the buffer - so every resize left one more copy of the prompt on
// screen. The repaint now walks up to the top of the frame the terminal is
// SHOWING, erases from there down, and paints. How far up that is is the whole
// question, and these four are its cases.
//
// THE ASSERTION IS THE PREFIX AND NOT THE WHOLE PAINT. What follows the erase is
// `paint`'s business and `LeshperBlitter` pins it byte for byte; what belongs to
// the loop is the count of rows, because only the loop knows what the terminal
// did to the old frame.

TEST(UiLoopRender, AResizeRepaintsFromTheTopOfTheReflowedFrame) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();
	// `> ` plus sixty cells is one row at eighty columns and two at forty, and
	// the cursor sits at the end of it either way - so a reflowing terminal has
	// put the frame's top one row above the cursor.
	tty.type(std::string(60, 'a'));
	loop.turn(50);
	ASSERT_EQ(buffer_of(loop).size(), 60u);
	(void)tty.painted();

	loop.editor().columns = 40;
	loop.render();
	const std::string narrower = tty.painted();
	EXPECT_TRUE(narrower.starts_with("\r\x1b[1A\x1b[J"))
		<< "expected a walk up one row and an erase, got: " << narrower.substr(0, 16);

	// And back. The same input at eighty columns is one row again, so the cursor
	// is already on the frame's top row and there is no walk to do - only the
	// erase, which is what stops the two-row frame from outliving the one-row one.
	loop.editor().columns = 80;
	loop.render();
	const std::string wider = tty.painted();
	EXPECT_TRUE(wider.starts_with("\r\x1b[J"))
		<< "expected an erase with no cursor-up, got: " << wider.substr(0, 16);
	EXPECT_FALSE(wider.starts_with("\r\x1b[1A"));
}

TEST(UiLoopRender, RowsAfterAHardNewlineDoNotMergeIntoTheOneBeforeThem) {
	// The reason the start row is a RE-LAYOUT and not arithmetic on the old row
	// count. Sixty-two cells and twelve cells are two rows at eighty columns; if
	// the reflow model simply rewrapped seventy-four cells it would make two rows
	// at forty as well, and the repaint would land one row short of the frame's
	// top. `lay_out` starts a new row at a U+000A at either width, so the answer
	// is three rows and the walk is two.
	loop_options options = pipe_options();
	options.continuation = "..";
	fake_tty tty;
	event_loop loop{tty.fds(), options};
	loop.enter_read();

	state& s = loop.editor();
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(),
	                 std::string(60, 'a') + "\n" + std::string(10, 'b'));
	s.cursor = s.buffer.end_position();
	s.gen.bump();
	loop.render();
	(void)tty.painted();

	loop.editor().columns = 40;
	loop.render();
	const std::string narrower = tty.painted();
	EXPECT_TRUE(narrower.starts_with("\r\x1b[2A\x1b[J"))
		<< "expected a walk up two rows, got: " << narrower.substr(0, 16);
}

TEST(UiLoopRender, ATerminalThatDoesNotReflowKeepsTheRowCountItAlreadyHad) {
	// xterm and Terminal.app leave the rows where they were, so the frame's top
	// is as far up as the picture we painted says - the count from BEFORE the
	// resize, which for a one-row frame is no rows at all.
	loop_options options = pipe_options();
	options.assume_reflow = false;
	fake_tty tty;
	event_loop loop{tty.fds(), options};
	loop.enter_read();
	tty.type(std::string(60, 'a'));
	loop.turn(50);
	ASSERT_EQ(buffer_of(loop).size(), 60u);
	(void)tty.painted();

	loop.editor().columns = 40;
	loop.render();
	const std::string narrower = tty.painted();
	EXPECT_TRUE(narrower.starts_with("\r\x1b[J"))
		<< "expected the unreflowed row count, got: " << narrower.substr(0, 16);
	EXPECT_FALSE(narrower.starts_with("\r\x1b[1A"));
}

TEST(UiLoopRender, AResizeRepaintsTheSoftRowsThroughTheWrapAndNeverMovesBetweenThem) {
	// #189, and the half of #185 that its fix left open. The walk back up to the
	// frame's top assumes the terminal reflowed the frame as ONE soft-wrapped
	// logical line - but the paint moved between every pair of rows with `\r`
	// and `ESC[B`, which makes each of them a hard line of its own. So the two
	// models disagreed: a shrink clipped every row separately and left fragments,
	// and a grow never rejoined them and left the old top row behind.
	//
	// `> ` plus a hundred cells is soft-wrapped at every width here - two rows at
	// eighty, three at forty - so EVERY row below the top is a continuation, and
	// any vertical move at all in the paint is a soft row painted as a hard one.
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();
	tty.type(std::string(100, 'a'));
	loop.turn(50);
	ASSERT_EQ(buffer_of(loop).size(), 100u);
	(void)tty.painted();

	loop.editor().columns = 40;
	loop.render();
	const std::string narrower = tty.painted();
	// Three rows at forty, and the cursor is on the last of them - so the frame
	// the terminal is showing starts two rows up.
	EXPECT_TRUE(narrower.starts_with("\r\x1b[2A\x1b[J"))
		<< "expected a walk up two rows and an erase, got: " << narrower.substr(0, 16);
	EXPECT_EQ(narrower.find("\x1b[1B"), std::string::npos)
		<< "a soft row was reached by moving to it: " << narrower;
	EXPECT_EQ(narrower.find("\x1b[B"), std::string::npos) << narrower;

	// AND BACK, which is the case the ticket's screenshots are of. Two rows
	// again, the second of them soft, and still not one move between them.
	loop.editor().columns = 80;
	loop.render();
	const std::string wider = tty.painted();
	EXPECT_TRUE(wider.starts_with("\r\x1b[1A\x1b[J"))
		<< "expected a walk up one row, got: " << wider.substr(0, 16);
	EXPECT_EQ(wider.find("\x1b[1B"), std::string::npos)
		<< "a soft row was reached by moving to it: " << wider;
	EXPECT_EQ(wider.find("\x1b[B"), std::string::npos) << wider;
	EXPECT_NE(wider.find(std::string(100, 'a')), std::string::npos)
		<< "the buffer must reach the wire as one unbroken run of cells";
}

TEST(UiLoopRender, ARepaintStillMovesToARowThatBeginsAfterAHardNewline) {
	// The other side of #189: a hard newline is a line of its own to the
	// terminal at every width, so the row it starts is POSITIONED to. Sixty
	// cells wrap at forty columns and the `b` line does not, which puts one of
	// each in the same paint.
	loop_options options = pipe_options();
	options.continuation = "..";
	fake_tty tty;
	event_loop loop{tty.fds(), options};
	loop.enter_read();

	state& s = loop.editor();
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(),
	                 std::string(60, 'a') + "\n" + std::string(10, 'b'));
	s.cursor = s.buffer.end_position();
	s.gen.bump();
	loop.render();
	(void)tty.painted();

	loop.editor().columns = 40;
	loop.render();
	const std::string narrower = tty.painted();
	// Exactly one vertical move: none into the soft row, one into the row the
	// newline starts.
	std::size_t moves = 0;
	for (std::size_t at = narrower.find("\x1b[1B"); at != std::string::npos;
	     at = narrower.find("\x1b[1B", at + 1))
		++moves;
	EXPECT_EQ(moves, 1u) << narrower;
}

TEST(UiLoopRender, TheFirstPaintOfAReadErasesNothing) {
	// The other half of the contract: with no previous frame the cursor really is
	// at the surface's origin and the rows below it are not ours. An ESC[J here
	// would erase whatever the last command printed.
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();
	loop.render();
	const std::string first = tty.painted();
	EXPECT_FALSE(first.empty());
	EXPECT_EQ(first.find("\x1b[J"), std::string::npos) << first;
}

// ===========================================================================
// Running (#134's sequencing; ONE call since #201)
// ===========================================================================

namespace {

std::thread::id g_action_thread{};

int32_t stopping_action(lesh_editor*, const lesh_invocation*, void* self) {
	g_action_thread = std::this_thread::get_id();
	static_cast<event_loop*>(self)->request_stop();
	return LESH_OK;
}

} // namespace

TEST(UiLoopRun, RunTurnsOnTheCallingThreadUntilRequestStop) {
	// WHAT REPLACED `start()`/`stop()`/`running()`. There is no loop thread and
	// nothing to join: `run()` turns on the caller's thread - which in a real
	// session is main - and leaves when `request_stop` has been set, which is what
	// Ctrl-D, an `exit` and a hangup all do. The action below stands in for all
	// three, and records the thread it ran on to say which one that is.
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};

	editing_context& context = context_of(loop.editor());
	ASSERT_EQ(lesh_action_register(&context.actions(), "ask_stop", &stopping_action, &loop),
	          LESH_OK);
	keymap* map = context.keymaps().find(keymap_registry::emacs);
	ASSERT_NE(map, nullptr);
	std::string encoded;
	ASSERT_TRUE(parse_key_notation("<C-a>", encoded));
	map->bind(encoded, "ask_stop");

	g_action_thread = std::thread::id{};
	// In the pipe before the first poll, so `run` reads it on its first turn
	// rather than blocking for a key that would never come.
	tty.type("\x01");
	loop.run();

	EXPECT_EQ(g_action_thread, std::this_thread::get_id());
	// `run` left the read on its way out, which is what every exit path owes the
	// terminal - and the prompt it painted before the first poll is proof it got
	// as far as a turn at all.
	// The prompt's trailing space is an ESC[K and a move rather than a byte, so
	// what is asserted is the character the prompt starts with.
	EXPECT_NE(tty.painted().find('>'), std::string::npos);
}

// ===========================================================================
// The terminal (tty.h)
// ===========================================================================

namespace {

// A real pty pair. Only the tests that are ABOUT termios open one; everything
// else runs on pipes, and nothing anywhere touches the process's own terminal.
class pty_pair {
public:
	pty_pair() {
		if (::openpty(&_master, &_slave, nullptr, nullptr, nullptr) != 0) {
			_master = -1;
			_slave = -1;
			return;
		}
		::fcntl(_master, F_SETFL, O_NONBLOCK);
	}
	~pty_pair() {
		// Drain the master before closing the slave. Closing a terminal
		// descriptor with output still queued waits for it to go out, and that
		// wait was 600 ms per test until this loop was here.
		drain();
		if (_slave >= 0)
			::close(_slave);
		if (_master >= 0)
			::close(_master);
	}

	// Everything the slave side has written since the last call.
	std::string drain() const {
		std::string all;
		char chunk[512];
		for (;;) {
			const ssize_t n = ::read(_master, chunk, sizeof(chunk));
			if (n <= 0)
				break;
			all.append(chunk, static_cast<std::size_t>(n));
		}
		return all;
	}

	pty_pair(const pty_pair&) = delete;
	pty_pair& operator=(const pty_pair&) = delete;

	[[nodiscard]] bool ok() const { return _master >= 0 && _slave >= 0; }
	[[nodiscard]] int slave() const { return _slave; }
	[[nodiscard]] int master() const { return _master; }

private:
	int _master = -1;
	int _slave = -1;
};

} // namespace

TEST(UiLoopTerminal, AZeroWinsizeFallsBackToEightyByTwentyFour) {
	// #128's trap 12: a zero in either axis is "no answer", not a zero-sized
	// screen, and a layout at zero columns is not a picture.
	pty_pair pty;
	ASSERT_TRUE(pty.ok());

	struct winsize zero{};
	ASSERT_EQ(::ioctl(pty.slave(), TIOCSWINSZ, &zero), 0);
	EXPECT_EQ(query_terminal_size(pty.slave()), kFallbackTerminalSize);

	struct winsize real{};
	real.ws_col = 120;
	real.ws_row = 40;
	ASSERT_EQ(::ioctl(pty.slave(), TIOCSWINSZ, &real), 0);
	EXPECT_EQ(query_terminal_size(pty.slave()), (terminal_size{120, 40}));

	// A pipe and a closed descriptor answer the same way rather than failing.
	EXPECT_EQ(query_terminal_size(-1), kFallbackTerminalSize);
}

TEST(UiLoopTerminal, RawModeIsEnteredAndRestoredWithoutEchoInBetween) {
	pty_pair pty;
	ASSERT_TRUE(pty.ok());

	struct termios before{};
	ASSERT_EQ(::tcgetattr(pty.slave(), &before), 0);

	{
		terminal tty{pty.slave()};
		ASSERT_TRUE(tty.is_terminal());
		ASSERT_TRUE(tty.enter_raw());
		EXPECT_TRUE(tty.raw());

		struct termios during{};
		ASSERT_EQ(::tcgetattr(pty.slave(), &during), 0);
		EXPECT_EQ(during.c_lflag & ICANON, 0u);
		EXPECT_EQ(during.c_lflag & ECHO, 0u);
		// ISIG STAYS ON: Ctrl-C must reach us as SIGINT so #98's `cancel-line`
		// runs off a signal event.
		EXPECT_NE(during.c_lflag & ISIG, 0u);
	}

	// The destructor restored: the failure this guards against is an early
	// return leaving a terminal nobody can type into.
	struct termios after{};
	ASSERT_EQ(::tcgetattr(pty.slave(), &after), 0);
	EXPECT_NE(after.c_lflag & ICANON, 0u);
	EXPECT_NE(after.c_lflag & ECHO, 0u);
	EXPECT_EQ(after.c_lflag & ICANON, before.c_lflag & ICANON);
}

TEST(UiLoopTerminal, EnteringRawKeepsWhateverAChildLeftBehind) {
	// fish's `term_copy_modes`: a `stty` change a user made in one command is
	// still there for the next one; only the bits the editor needs are forced.
	pty_pair pty;
	ASSERT_TRUE(pty.ok());

	struct termios child_left{};
	ASSERT_EQ(::tcgetattr(pty.slave(), &child_left), 0);
	child_left.c_cc[VERASE] = 0x08;  // the child chose backspace-as-BS
	ASSERT_EQ(::tcsetattr(pty.slave(), TCSANOW, &child_left), 0);

	terminal tty{pty.slave()};
	ASSERT_TRUE(tty.enter_raw());

	struct termios during{};
	ASSERT_EQ(::tcgetattr(pty.slave(), &during), 0);
	EXPECT_EQ(during.c_cc[VERASE], 0x08);
}

TEST(UiLoopTerminal, BracketedPasteIsTurnedOnWithRawAndOffWithCooked) {
	pty_pair pty;
	ASSERT_TRUE(pty.ok());

	terminal tty{pty.slave()};
	ASSERT_TRUE(tty.enter_raw());
	EXPECT_NE(pty.drain().find(kBracketedPasteOn), std::string::npos);

	ASSERT_TRUE(tty.leave_raw());
	EXPECT_NE(pty.drain().find(kBracketedPasteOff), std::string::npos);
}

TEST(UiLoopTerminal, SetForegroundPgrpOnSomethingThatIsNotATtyAnswersNoTty) {
	// fish #6573's case: job control without a controlling terminal. Not a
	// failure - the editor runs, it just has nobody to hand to.
	int fds[2] = {-1, -1};
	ASSERT_EQ(::pipe(fds), 0);
	EXPECT_EQ(set_foreground_pgrp(fds[0], ::getpgrp()), tty_transfer::no_tty);
	EXPECT_EQ(set_foreground_pgrp(-1, ::getpgrp()), tty_transfer::no_tty);
	EXPECT_FALSE(owns_terminal(fds[0]));
	::close(fds[0]);
	::close(fds[1]);
}

TEST(UiLoopTerminal, TheExitRestoreDoesNothingWhenTheTerminalIsNotOurs) {
	// fish `restore_term_mode`: restore only `if (getpgrp() == tcgetpgrp(...))`.
	// Without the check we steal the terminal from whoever has it (#7060).
	pty_pair pty;
	ASSERT_TRUE(pty.ok());

	disarm_exit_restore();
	EXPECT_FALSE(exit_restore_armed());

	{
		terminal tty{pty.slave()};
		ASSERT_TRUE(tty.enter_raw());
		EXPECT_TRUE(exit_restore_armed());

		// This pty is not our controlling terminal, so `tcgetpgrp` on it does not
		// answer our process group and the restore declines - which is precisely
		// the guard, exercised.
		struct termios during{};
		ASSERT_EQ(::tcgetattr(pty.slave(), &during), 0);
		restore_terminal_for_exit();
		struct termios after{};
		ASSERT_EQ(::tcgetattr(pty.slave(), &after), 0);
		EXPECT_EQ(during.c_lflag, after.c_lflag);
	}
	disarm_exit_restore();
	EXPECT_FALSE(exit_restore_armed());
}

// ===========================================================================
// The fork-child discipline (substrate/fork_guard.h, substrate/log.h)
// ===========================================================================

TEST(UiLoopForkGuard, TheGuardCountsAllocationsBetweenForkAndExec) {
	lesh::install_fork_child_detection();
	// The parent is not a forked child, and the flag is consulted only by debug
	// assertions - never by the signal handler, which compares getpid().
	EXPECT_FALSE(lesh::is_forked_child());

	lesh::fork_child_guard guard;
	EXPECT_EQ(guard.allocations_since_entry(), 0u);
#ifdef LESH_ENABLE_ASSERTS
	// One arena allocation is visible to the guard, which is what makes the
	// assertion in its destructor mean something rather than always passing.
	lesh::buffer_pool arena{256};
	char* scratch_bytes = nullptr;
	(void)arena.allocate(16, scratch_bytes);
	EXPECT_GT(guard.allocations_since_entry(), 0u);
#endif
	// Released before the (notional) execve, so the destructor stops checking.
	guard.exec_reached();
}

TEST(UiLoopForkGuard, LogSafeFormatsWithoutAllocatingOrCallingVsnprintf) {
	const lesh::testing::temp_path scratch;
	const std::string path = scratch.file("log");

	log::settings settings;
	settings.enabled = ~0ull >> 9;
	settings.log_path = path;
	settings.log_path_explicit = true;
	log::options with;
	with.interactive = false;
	ASSERT_TRUE(log::configure(settings, with));

	LESH_LOG_SAFE("exec failed: ", "no such file", " errno=", 2);
	LESH_LOG_SAFE("negative=", -9223372036854775807LL - 1);

	log::shutdown();

	std::ifstream file{path};
	std::stringstream all;
	all << file.rdbuf();
	const std::string text = all.str();
	EXPECT_NE(text.find("exec failed: no such file errno=2"), std::string::npos);
	EXPECT_NE(text.find("negative=-9223372036854775808"), std::string::npos);
}

// ===========================================================================
// Replay: a recorded byte log through the identical path (N-3, #128's test
// contract)
// ===========================================================================

namespace {

// A recorded session's bytes: typing, an arrow key, a multibyte character, a
// bracketed paste, and a backspace. What a byte log off a real terminal looks
// like, written down rather than captured, so the fixture cannot drift.
constexpr std::string_view kRecordedByteLog =
	"echo \xC3\xA9\x1b[D\x1b[C\x7f"
	"\x1b[200~ | wc -l\x1b[201~"
	"x";

// Feeds `bytes` through a loop in chunks of `chunk` and answers the state it
// produced. The loop is the one under test, over a pipe - the identical path a
// real terminal takes, which is the whole point of the core being a function
// over descriptors.
state replay_through_the_loop(std::string_view bytes, std::size_t chunk) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	for (std::size_t at = 0; at < bytes.size(); at += chunk) {
		tty.type(bytes.substr(at, chunk));
		loop.turn(50);
	}
	// One more turn past the escape timeout, so a trailing ambiguous ESC
	// resolves the way it would after a pause at a real keyboard.
	std::this_thread::sleep_for(default_escape_timeout + std::chrono::milliseconds{10});
	loop.turn(0);
	return loop.editor();
}

} // namespace

TEST(UiLoopReplay, ARecordedByteLogReproducesTheSameStateAtEveryReadBoundary) {
	// N-3's property, asked of the LOOP rather than of the editor: the same
	// bytes produce an EQUAL state - equal by the operator `state` carries over
	// every field, which exists for exactly this - no matter how the kernel
	// happened to split them across reads.
	//
	// The read boundary is the variable because it is the one the loop
	// introduces. #111's decoder holds partial prefixes and #129's turn feeds
	// them; a bug in either shows up as a codepoint split across two reads
	// becoming two characters, or a paste marker split across two reads becoming
	// literal text.
	const state whole = replay_through_the_loop(kRecordedByteLog, kRecordedByteLog.size());
	// Left, Right, then DEL: the backspace lands on the multibyte cluster and
	// takes it whole - F-3's invariant, visible here as one deletion rather than
	// a stranded continuation byte.
	EXPECT_EQ(std::string{whole.buffer.text()}, "echo  | wc -lx");

	for (std::size_t chunk : {1u, 2u, 3u, 5u, 8u, 13u}) {
		const state again = replay_through_the_loop(kRecordedByteLog, chunk);
		EXPECT_TRUE(again == whole) << "read boundary every " << chunk << " bytes";
	}
}

TEST(UiLoopReplay, TheEventCategoryRecordsWhatTheLoopDrained) {
	// #120's structured sink is the replay file, and #129 owes it every input
	// the loop drained. The writer is `log_event` at the entrance of `step`, so
	// what this asserts is that the loop's topics all deliver THROUGH that
	// entrance rather than around it.
	const lesh::testing::temp_path scratch;
	const std::string replay = scratch.file("replay.jsonl");

	log::settings settings;
	settings.enabled = ~0ull >> 9;
	settings.log_path = scratch.file("log");
	settings.log_path_explicit = true;
	settings.replay_path = replay;
	log::options with;
	with.interactive = false;
	ASSERT_TRUE(log::configure(settings, with));

	{
		fake_tty tty;
		event_loop loop{tty.fds(), pipe_options()};
		loop.enter_read();
		tty.type("ab");
		loop.turn(50);
		loop.signals().deliver(SIGWINCH);
		loop.turn(50);
		loop.signals().deliver(SIGINT);
		loop.turn(50);
	}
	log::shutdown();

	std::ifstream file{replay};
	std::stringstream all;
	all << file.rdbuf();
	const std::string text = all.str();

	EXPECT_NE(text.find("\"kind\":\"key\""), std::string::npos);
	EXPECT_NE(text.find("\"kind\":\"resize\""), std::string::npos);
	EXPECT_NE(text.find("\"kind\":\"signal\""), std::string::npos);
}

TEST(UiLoopTerminal, FatalRestoreHandlersAreInstalledOnlyOverTheDefault) {
	// #98 decision 5's last exit path. The rule that matters is the one about
	// NOT installing: ASan owns SIGSEGV and SIGBUS under the sanitized gate, and
	// its report is worth more than a restored terminal after a segfault.
	lesh::testing::saved_disposition segv{SIGSEGV};
	lesh::testing::saved_disposition bus{SIGBUS};
	lesh::testing::saved_disposition ill{SIGILL};
	lesh::testing::saved_disposition fpe{SIGFPE};
	lesh::testing::saved_disposition abrt{SIGABRT};

	ill.default_action();
	fpe.default_action();
	abrt.default_action();
	// Somebody else's handler, which must survive untouched.
	segv.ignore();

	const int installed = install_fatal_restore_handlers();
	EXPECT_GE(installed, 3) << "SIGILL, SIGFPE and SIGABRT were at the default";

	struct sigaction current{};
	ASSERT_EQ(::sigaction(SIGSEGV, nullptr, &current), 0);
	EXPECT_EQ(current.sa_handler, SIG_IGN) << "a handler somebody else owns is left alone";

	// LESH_ASSERT dies through abort(), which is why SIGABRT is on the list at
	// all: it IS #98's "assert-and-die path".
	ASSERT_EQ(::sigaction(SIGABRT, nullptr, &current), 0);
	EXPECT_NE(current.sa_handler, SIG_DFL);
	EXPECT_NE(current.sa_handler, SIG_IGN);

	// Installed twice, nothing is at SIG_DFL any more, so nothing is taken.
	EXPECT_EQ(install_fatal_restore_handlers(), 0);
}

// ===========================================================================
// The watch topic (#195) - §8's fd-readable hook, and the loop's sixth topic
// ===========================================================================
//
// THE LOOP KNOWS NOTHING ABOUT WHAT IS ON THE OTHER END, which is why these
// tests use a plain pipe. The one shipped user is the history's directory watch
// (inotify on Linux, a kqueue on macOS) and its own tests are in
// `ui_history_stale_tests.cpp`; what is being tested HERE is the contract the
// loop holds up: poll the fd, run the hook on the loop thread when it is
// readable, run it never otherwise, and count the turn as a drained topic.

namespace {

struct watch_probe {
	int fd = -1;
	std::size_t runs = 0;

	// Consumes what made the fd readable, which is the topic's rule: a hook that
	// leaves its descriptor readable turns the poll into a spin.
	static void on_readable(void* userdata) {
		auto* self = static_cast<watch_probe*>(userdata);
		++self->runs;
		char drained[64];
		while (::read(self->fd, drained, sizeof(drained)) > 0) {
		}
	}
};

} // namespace

TEST(UiLoopWatch, AReadableWatchFdRunsTheHookOnTheLoopThread) {
	fake_tty tty;
	int pipe_fds[2] = {-1, -1};
	ASSERT_EQ(::pipe(pipe_fds), 0);
	ASSERT_EQ(::fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK), 0);

	watch_probe probe;
	probe.fd = pipe_fds[0];

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_watch(pipe_fds[0], &watch_probe::on_readable, &probe);
	EXPECT_EQ(loop.watch_fd(), pipe_fds[0]);
	loop.enter_read();

	// Nothing on the pipe: the topic exists and stays quiet.
	const turn_result quiet = loop.turn(0);
	EXPECT_EQ(probe.runs, 0u);
	EXPECT_EQ(loop.watch_drains(), 0u);
	EXPECT_EQ(quiet.topics_drained, 0u);

	ASSERT_EQ(::write(pipe_fds[1], "x", 1), 1);
	ASSERT_TRUE(turn_until(loop, [&] { return probe.runs > 0; }));
	EXPECT_EQ(loop.watch_drains(), 1u);

	// AND THE HOOK CONSUMED IT, so the next turn is quiet again. Without that
	// the loop would spin at 100% on a level-triggered descriptor - the failure
	// mode this topic's contract is entirely about.
	const std::size_t after = probe.runs;
	loop.turn(0);
	EXPECT_EQ(probe.runs, after);

	loop.detach_watch();
	EXPECT_EQ(loop.watch_fd(), -1);
	::close(pipe_fds[0]);
	::close(pipe_fds[1]);
}

TEST(UiLoopWatch, ADetachedWatchIsNeverPolled) {
	// The loop must not hold on to a descriptor it was told to forget: a history
	// destroyed before the session would otherwise leave the loop polling a
	// closed fd, which `poll` reports as POLLNVAL forever.
	fake_tty tty;
	int pipe_fds[2] = {-1, -1};
	ASSERT_EQ(::pipe(pipe_fds), 0);

	watch_probe probe;
	probe.fd = pipe_fds[0];

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_watch(pipe_fds[0], &watch_probe::on_readable, &probe);
	loop.detach_watch();
	loop.enter_read();

	ASSERT_EQ(::write(pipe_fds[1], "x", 1), 1);
	for (int i = 0; i < 5; ++i)
		loop.turn(0);
	EXPECT_EQ(probe.runs, 0u);
	EXPECT_EQ(loop.watch_drains(), 0u);

	::close(pipe_fds[0]);
	::close(pipe_fds[1]);
}

TEST(UiLoopWatch, AnFdWithNoHookIsNotATopic) {
	// Both or neither (`attach_watch`): a descriptor with nothing to consume it
	// is the spin above with no way out, so the attachment refuses rather than
	// half-arming.
	fake_tty tty;
	int pipe_fds[2] = {-1, -1};
	ASSERT_EQ(::pipe(pipe_fds), 0);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_watch(pipe_fds[0], nullptr, nullptr);
	EXPECT_EQ(loop.watch_fd(), -1);

	loop.attach_watch(-1, &watch_probe::on_readable, nullptr);
	EXPECT_EQ(loop.watch_fd(), -1);

	::close(pipe_fds[0]);
	::close(pipe_fds[1]);
}

// ===========================================================================
// EXECUTION: ON A FIBER, OR ON THE HOST'S OWN STACK (#208)
// ===========================================================================
//
// BOTH MODES ARE FIRST-CLASS AND BOTH ARE TESTED, which is the owner's
// requirement on this ticket rather than a nicety. Every case below runs twice,
// once per `execution_mode`, and the ones that differ say what differs and why.
//
// THE SHELL IN THESE TESTS REALLY FORKS AND REALLY WAITS, through
// `event_loop::await_child` - which is the same function
// `cooperation::wait_child` reaches one indirect call away. A `fake_shell` that
// returns without waiting cannot exercise any of this: the park only happens at a
// wait, and the loop only stays alive because something parked.
//
// AND THE FORK IS ON THE FIBER STACK in `on_a_fiber` mode, under ASan, which is
// the first-contact risk #202 flagged. That it is a plain test here rather than a
// separate experiment is the finding.

namespace {

loop_options pipe_options_with(execution_mode how) {
	loop_options options = pipe_options();
	options.execution = how;
	return options;
}

const char* name_of(execution_mode how) {
	return how == execution_mode::on_a_fiber ? "on_a_fiber" : "inline_";
}

// A shell that forks a child, awaits it through the host's verb, and reports what
// it saw while it was in there.
class awaiting_shell : public shell_side {
public:
	event_loop* loop = nullptr;
	// How long the child lives. Long enough that the host is certain to reach its
	// poll before the SIGCHLD arrives, short enough not to make the suite slow.
	unsigned child_ms = 40;
	// A second child that is NEVER awaited, forked first and exiting at once - the
	// background job whose zombie must survive this command.
	bool fork_an_unawaited_child = false;
	// Bytes typed into the test's own tty from inside the command, which is the
	// only way a single-threaded test can type "while a command runs".
	std::string type_during;
	fake_tty* tty = nullptr;

	pid_t awaited = -1;
	pid_t unawaited = -1;
	pid_t reaped = -1;
	int wait_status = 0;
	std::size_t executes = 0;
	phase phase_inside = phase::editing;
	bool on_a_fiber_inside = false;
	std::size_t watch_drains_before = 0;

	std::int32_t execute(std::string_view) override {
		++executes;
		phase_inside = loop->session_phase();
		on_a_fiber_inside = loop->reactors().current() != nullptr;
		watch_drains_before = loop->watch_drains();
		if (tty != nullptr && !type_during.empty())
			tty->type(type_during);
		if (fork_an_unawaited_child) {
			unawaited = ::fork();
			if (unawaited == 0)
				::_exit(4);
			// Given a moment to become a zombie, so that a sweep reaping `-1`
			// would certainly have taken it.
			::usleep(5 * 1000);
		}
		awaited = ::fork();
		if (awaited == 0) {
			::usleep(child_ms * 1000);
			::_exit(9);
		}
		reaped = loop->await_child(awaited, 0, &wait_status);
		return WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
	}

	std::int32_t port_call(std::string_view) override { return 0; }
};

} // namespace

TEST(UiLoopExecution, BothModesRunTheLineWaitForTheChildAndReportItsStatus) {
	for (const execution_mode how : {execution_mode::on_a_fiber, execution_mode::inline_}) {
		const every_hub_disposition guards;
		guards.child().default_action();

		fake_tty tty;
		signal_hub hub;
		awaiting_shell shell;
		event_loop loop{tty.fds(), pipe_options_with(how)};
		ASSERT_TRUE(hub.install());
		loop.attach_signals(hub);
		loop.attach_shell(shell);
		shell.loop = &loop;
		loop.enter_read();

		const std::optional<std::int32_t> status = loop.accept_current_line();

		ASSERT_TRUE(status.has_value()) << name_of(how);
		EXPECT_EQ(*status, 9) << name_of(how) << ": the child's exit status";
		EXPECT_EQ(shell.executes, 1u) << name_of(how);
		EXPECT_EQ(shell.reaped, shell.awaited)
			<< name_of(how) << ": the wait answered with the pid it was given";
		EXPECT_TRUE(WIFEXITED(shell.wait_status)) << name_of(how);
		EXPECT_EQ(WEXITSTATUS(shell.wait_status), 9) << name_of(how);
		// PHASE IS STILL WRITTEN AT THE TWO HOST PLACES, and a command sees
		// `executing` from inside itself whichever stack it is on.
		EXPECT_EQ(shell.phase_inside, phase::executing) << name_of(how);
		EXPECT_EQ(loop.session_phase(), phase::editing)
			<< name_of(how) << ": and `editing` again on the way out";
		// AND THE WAITER TABLE IS EMPTY AGAIN. An entry left behind would be a
		// pointer into a frame that has returned.
		EXPECT_EQ(loop.awaited_children(), 0u) << name_of(how);

		loop.leave_read();
		hub.uninstall();
	}
}

TEST(UiLoopExecution, TheModeDecidesWhetherThereIsAFiberAtAll) {
	for (const execution_mode how : {execution_mode::on_a_fiber, execution_mode::inline_}) {
		const every_hub_disposition guards;
		guards.child().default_action();

		fake_tty tty;
		signal_hub hub;
		awaiting_shell shell;
		event_loop loop{tty.fds(), pipe_options_with(how)};
		ASSERT_TRUE(hub.install());
		loop.attach_signals(hub);
		loop.attach_shell(shell);
		shell.loop = &loop;
		loop.enter_read();

		// NOTHING IS SPAWNED UNTIL A LINE IS ACCEPTED, in either mode: a loop that
		// only ever edits reserves no execution stack.
		EXPECT_FALSE(loop.has_execution_fiber()) << name_of(how);

		(void)loop.accept_current_line();

		if (how == execution_mode::on_a_fiber) {
			EXPECT_TRUE(loop.has_execution_fiber());
			EXPECT_TRUE(shell.on_a_fiber_inside)
				<< "`execute` ran on a fiber, so `current()` is not null inside it";
			// TWO SLICES AT LEAST FOR ONE LINE, and that is the park made visible:
			// one slice runs down to the wait, another resumes it after the wake.
			EXPECT_GE(loop.execution_slices(), 2u)
				<< "one slice would mean the wait never parked";
		} else {
			EXPECT_FALSE(loop.has_execution_fiber());
			EXPECT_EQ(loop.execution_slices(), 0u);
			EXPECT_FALSE(shell.on_a_fiber_inside)
				<< "`current()` must be null throughout the inline path";
		}

		// A SECOND LINE REUSES THE FIBER. It is spawned once and parks on its
		// inbox between commands; a second one would be a second 8 MB reserve and
		// a second thing to drain at shutdown.
		const std::size_t fibers = loop.reactors().fiber_count();
		(void)loop.accept_current_line();
		EXPECT_EQ(loop.reactors().fiber_count(), fibers) << name_of(how);
		EXPECT_EQ(shell.executes, 2u) << name_of(how);

		loop.leave_read();
		hub.uninstall();
	}
}

TEST(UiLoopExecution, OnAFiberTheLoopIsALIVEWhileACommandRunsAndInlineItIsNot) {
	// THE WHOLE POINT OF THE TICKET, and the one observable that separates the two
	// modes at a glance: the `watch` topic. The child makes the watched descriptor
	// readable and then takes its time dying, so a loop that is turning during the
	// command drains it DURING the command and a loop that is blocked in a
	// `waitpid` drains it only afterwards.
	for (const execution_mode how : {execution_mode::on_a_fiber, execution_mode::inline_}) {
		const every_hub_disposition guards;
		guards.child().default_action();

		int watch_fds[2] = {-1, -1};
		ASSERT_EQ(::pipe(watch_fds), 0);
		ASSERT_EQ(::fcntl(watch_fds[0], F_SETFL, O_NONBLOCK), 0);
		watch_probe probe;
		probe.fd = watch_fds[0];

		fake_tty tty;
		signal_hub hub;
		awaiting_shell shell;
		event_loop loop{tty.fds(), pipe_options_with(how)};
		ASSERT_TRUE(hub.install());
		loop.attach_signals(hub);
		loop.attach_shell(shell);
		loop.attach_watch(watch_fds[0], &watch_probe::on_readable, &probe);
		shell.loop = &loop;
		// The write happens in the child, AFTER the fork, so the byte is there
		// while the parent is inside its wait.
		const int notify_fd = watch_fds[1];
		shell.child_ms = 60;
		shell.loop = &loop;
		loop.enter_read();

		// One byte written by the test just before the accept is enough: what is
		// under test is WHEN the loop notices it, not who wrote it.
		const char one = 'x';
		ASSERT_EQ(::write(notify_fd, &one, 1), 1);

		(void)loop.accept_current_line();

		if (how == execution_mode::on_a_fiber) {
			EXPECT_GE(probe.runs, 1u)
				<< "the loop polled the watch topic while the command was running";
			EXPECT_GE(loop.watch_drains(), 1u);
		} else {
			EXPECT_EQ(probe.runs, 0u)
				<< "the inline path blocks in `waitpid`: nothing polls anything";
			EXPECT_EQ(loop.watch_drains(), 0u);
			// AND IT IS NOT LOST - the first ordinary turn afterwards drains it,
			// which is exactly what happened before this ticket existed.
			(void)loop.turn(0);
			EXPECT_GE(probe.runs, 1u);
		}

		loop.detach_watch();
		loop.leave_read();
		hub.uninstall();
		::close(watch_fds[0]);
		::close(watch_fds[1]);
	}
}

TEST(UiLoopExecution, TheTtyTopicIsOutOfTheTurnWhileACommandRuns) {
	// Point 4: the terminal is the child's. A loop that read it would earn a
	// SIGTTIN in a real session, and the bytes it took would be the command's
	// input rather than the editor's. So they are LEFT IN THE DESCRIPTOR, and the
	// first turn after the command picks them up - which is the same thing that
	// happened when nothing polled at all.
	for (const execution_mode how : {execution_mode::on_a_fiber, execution_mode::inline_}) {
		const every_hub_disposition guards;
		guards.child().default_action();

		fake_tty tty;
		signal_hub hub;
		awaiting_shell shell;
		event_loop loop{tty.fds(), pipe_options_with(how)};
		ASSERT_TRUE(hub.install());
		loop.attach_signals(hub);
		loop.attach_shell(shell);
		shell.loop = &loop;
		shell.tty = &tty;
		shell.type_during = "ab";
		loop.enter_read();

		(void)loop.accept_current_line();

		EXPECT_EQ(buffer_of(loop), "")
			<< name_of(how) << ": nothing typed during the command reached the editor";
		const turn_result after = loop.turn(0);
		// TWO KEYSTROKES AND THE CHILD'S OWN SIGCHLD, which is three events and
		// not two: the SIGCHLD that ended the wait reaches the editor on this turn
		// exactly as it did before this ticket, when the byte simply stayed in the
		// self-pipe for the length of the command.
		EXPECT_GE(after.events, 2u)
			<< name_of(how) << ": and both bytes were still in the descriptor";
		EXPECT_EQ(buffer_of(loop), "ab") << name_of(how);

		loop.leave_read();
		hub.uninstall();
	}
}

TEST(UiLoopExecution, TheTimerTopicIsOutOfTheTurnWhileACommandRuns) {
	// The other half of point 4, and the reason is not symmetry: a timer expiring
	// during a command would dispatch an ACTION into an editor that is not on
	// screen and whose terminal belongs to the child.
	for (const execution_mode how : {execution_mode::on_a_fiber, execution_mode::inline_}) {
		const every_hub_disposition guards;
		guards.child().default_action();

		fake_tty tty;
		signal_hub hub;
		awaiting_shell shell;
		event_loop loop{tty.fds(), pipe_options_with(how)};
		ASSERT_TRUE(hub.install());
		loop.attach_signals(hub);
		loop.attach_shell(shell);
		shell.loop = &loop;
		shell.child_ms = 60;

		registry& reg = context_of(loop.editor()).actions();
		loop.attach_registry(reg);
		g_action_runs.store(0, std::memory_order_relaxed);
		ASSERT_EQ(lesh_action_register(&reg, "tick", &counting_action, nullptr), LESH_OK);
		std::uint64_t id = 0;
		ASSERT_EQ(lesh_timer_start(&reg, 1, "tick", &id), LESH_OK);
		loop.enter_read();

		(void)loop.accept_current_line();

		EXPECT_EQ(loop.timer_dispatches(), 0u)
			<< name_of(how) << ": a 1 ms timer must not fire inside a 60 ms command";
		// ARMED THROUGHOUT, not disarmed: the very next turn fires it.
		ASSERT_TRUE(turn_until(loop, [&] { return loop.timer_dispatches() > 0; }));

		loop.leave_read();
		hub.uninstall();
	}
}

TEST(UiLoopExecution, ASignalDeliveredDuringACommandIsReplayedWhenTheEditorIsBack) {
	// #201's fact, kept through a change of mechanism for the second time. The
	// byte used to sit in the self-pipe for the whole command; now the command's
	// own turns consume it - it is what ends the foreground wait - so the NUMBER
	// is held and the first ordinary turn makes the same event of it.
	//
	// WHAT MUST NOT HAPPEN is the event being dispatched during the command: a
	// SIGINT turned into `cancel_line` there would call `execute` from inside
	// `execute`. `executes == 1` is that assertion.
	for (const execution_mode how : {execution_mode::on_a_fiber, execution_mode::inline_}) {
		const every_hub_disposition guards;
		guards.child().default_action();

		fake_tty tty;
		signal_hub hub;
		awaiting_shell shell;
		event_loop loop{tty.fds(), pipe_options_with(how)};
		ASSERT_TRUE(hub.install());
		loop.attach_signals(hub);
		loop.attach_shell(shell);
		shell.loop = &loop;
		loop.enter_read();

		// Delivered to the hub the way the handler delivers it, with no signal
		// raised at the process: the child's own SIGCHLD is the other byte in
		// there, and both have to come out of one drain.
		hub.deliver(SIGINT);

		(void)loop.accept_current_line();
		EXPECT_EQ(shell.executes, 1u)
			<< name_of(how) << ": the deferred SIGINT must not re-enter `execute`";

		const turn_result after = loop.turn(0);
		EXPECT_GE(after.events, 1u)
			<< name_of(how) << ": the signal reaches the editor once it exists again";

		loop.leave_read();
		hub.uninstall();
	}
}

TEST(UiLoopExecution, OnlyAwaitedPidsAreEverReaped) {
	// Point 5, and it is what keeps this ticket free of job-control consequences: a
	// sweep that reaped `-1` would take the `&` child the script has not waited
	// for yet, and `wait` would then answer 127 for a job it started. So the
	// unawaited child is STILL A ZOMBIE when the command is over, and this test is
	// the one that can prove it - it reaps it itself.
	for (const execution_mode how : {execution_mode::on_a_fiber, execution_mode::inline_}) {
		const every_hub_disposition guards;
		guards.child().default_action();

		fake_tty tty;
		signal_hub hub;
		awaiting_shell shell;
		event_loop loop{tty.fds(), pipe_options_with(how)};
		ASSERT_TRUE(hub.install());
		loop.attach_signals(hub);
		loop.attach_shell(shell);
		shell.loop = &loop;
		shell.fork_an_unawaited_child = true;
		shell.child_ms = 60;
		loop.enter_read();

		(void)loop.accept_current_line();

		ASSERT_GT(shell.unawaited, 0);
		int leftover = 0;
		EXPECT_EQ(::waitpid(shell.unawaited, &leftover, WNOHANG), shell.unawaited)
			<< name_of(how) << ": the loop reaped a pid nobody had awaited";
		EXPECT_TRUE(WIFEXITED(leftover));
		EXPECT_EQ(WEXITSTATUS(leftover), 4);

		loop.leave_read();
		hub.uninstall();
	}
}

TEST(UiLoopExecution, WithNoSIGCHLDToWakeItTheWaitIsTakenInline) {
	// The park is paid for by exactly one wake, and that wake is the hub's
	// self-pipe byte. `reassert`'s rule 3 leaves an inherited SIG_IGN and a
	// `trap '' CHLD` alone - both legitimate, both meaning nothing will ever ring
	// the pipe again - so `await_child` asks the kernel first and blocks on its own
	// if the answer is no. Without this the shell hangs on `trap '' CHLD; sleep 1`.
	const every_hub_disposition guards;
	guards.child().ignore();   // what a parent that ignored SIGCHLD hands us

	fake_tty tty;
	signal_hub hub;
	awaiting_shell shell;
	event_loop loop{tty.fds(), pipe_options_with(execution_mode::on_a_fiber)};
	ASSERT_TRUE(hub.install());
	// Rule 3: the newest ignore stands, so the hub did NOT take SIGCHLD.
	ASSERT_FALSE(hub.catches(SIGCHLD));
	loop.attach_signals(hub);
	loop.attach_shell(shell);
	shell.loop = &loop;
	loop.enter_read();

	const std::optional<std::int32_t> status = loop.accept_current_line();

	ASSERT_TRUE(status.has_value());
	EXPECT_TRUE(loop.has_execution_fiber()) << "still on the fiber - only the WAIT differs";
	// ONE SLICE, because the wait never parked: the fiber ran the whole command in
	// the slice it was given.
	EXPECT_EQ(loop.execution_slices(), 1u);
	EXPECT_EQ(loop.awaited_children(), 0u);

	loop.leave_read();
	hub.uninstall();
}

TEST(UiLoopExecution, AndTheHubKnowsWhetherItHoldsASignal) {
	// `catches` is asked of the KERNEL, which is the only side that knows: a
	// `trap` inside the command that is running now has already replaced the
	// disposition and the reassert on the way out has not happened yet.
	const every_hub_disposition guards;
	guards.child().default_action();

	signal_hub hub;
	EXPECT_FALSE(hub.catches(SIGCHLD)) << "nothing installed yet";
	ASSERT_TRUE(hub.install());
	EXPECT_TRUE(hub.catches(SIGCHLD));
	EXPECT_TRUE(hub.catches(SIGINT));
	// The IGNORED set is never the hub's to hold: its SIG_IGN was only "better
	// than the default".
	EXPECT_FALSE(hub.catches(SIGTSTP));
	// And a `trap` typed a moment ago takes it away, which is the case this
	// function exists for.
	install_handler(SIGCHLD, trap_style_handler);
	EXPECT_FALSE(hub.catches(SIGCHLD));

	hub.uninstall();
}

TEST(UiLoopExecution, ACancelledLineTakesTheSameDoorAsAnAcceptedOne) {
	// `finish_cancelled_line` delivers an EMPTY line through the same `execute`,
	// on the same path, because an INT trap body is arbitrary shell code and may
	// fork - so it must fork from wherever an ordinary command forks.
	for (const execution_mode how : {execution_mode::on_a_fiber, execution_mode::inline_}) {
		const every_hub_disposition guards;
		guards.child().default_action();

		fake_tty tty;
		signal_hub hub;
		awaiting_shell shell;
		event_loop loop{tty.fds(), pipe_options_with(how)};
		ASSERT_TRUE(hub.install());
		loop.attach_signals(hub);
		loop.attach_shell(shell);
		shell.loop = &loop;
		loop.enter_read();

		loop.finish_cancelled_line();

		EXPECT_EQ(shell.executes, 1u) << name_of(how);
		EXPECT_EQ(shell.phase_inside, phase::executing) << name_of(how);
		EXPECT_EQ(loop.exit_status(), 9) << name_of(how) << ": the status is kept";
		EXPECT_EQ(loop.session_phase(), phase::editing) << name_of(how);
		EXPECT_EQ(loop.awaited_children(), 0u) << name_of(how);
		if (how == execution_mode::on_a_fiber)
			EXPECT_GE(loop.execution_slices(), 2u);

		loop.leave_read();
		hub.uninstall();
	}
}

TEST(UiLoopExecution, APortCallNeverParksBecauseThereIsNoFiberUnderIt) {
	// `port_call` stays a direct call (point 3): the execution fiber is parked in
	// `recv` whenever an action runs, so an action's shell code runs on the HOST's
	// stack and every wait under it is a blocking `::waitpid` - `current() == null`
	// throughout, with no branch anywhere saying so.
	const every_hub_disposition guards;
	guards.child().default_action();

	fake_tty tty;
	signal_hub hub;
	awaiting_shell shell;
	event_loop loop{tty.fds(), pipe_options_with(execution_mode::on_a_fiber)};
	ASSERT_TRUE(hub.install());
	loop.attach_signals(hub);
	loop.attach_shell(shell);
	shell.loop = &loop;
	loop.enter_read();

	// One accepted line first, so the fiber exists and is parked on its inbox.
	(void)loop.accept_current_line();
	ASSERT_TRUE(loop.has_execution_fiber());
	const std::size_t slices = loop.execution_slices();

	// A wait taken from the host, through the same verb the runtime reaches.
	const pid_t child = ::fork();
	if (child == 0)
		::_exit(5);
	int status = 0;
	EXPECT_EQ(loop.await_child(child, 0, &status), child);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 5);
	EXPECT_EQ(loop.execution_slices(), slices)
		<< "the execution fiber was not resumed for a wait that was not its own";
	EXPECT_EQ(loop.awaited_children(), 0u);

	loop.leave_read();
	hub.uninstall();
}
