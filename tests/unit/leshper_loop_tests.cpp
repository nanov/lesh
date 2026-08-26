#include "leshper/loop.h"
#include "leshper/shell_actor.h"
#include "leshper/tty.h"
#include "leshper/workers.h"
#include "substrate/fork_guard.h"
#include "substrate/log.h"

#include "interactive_signal_guard.h"
#include "temp_path.h"

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
namespace log = lesh::log;

// THE EVENT LOOP (#129): poll(2), five topics, quiesce.
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

// A pipe standing in for a terminal. Non-blocking on the read end, because the
// loop polls before reading and a test that got the poll wrong should fail
// rather than hang the suite.
class fake_tty {
public:
	fake_tty() {
		[&] { ASSERT_EQ(::pipe(_in), 0); }();
		[&] { ASSERT_EQ(::pipe(_out), 0); }();
		::fcntl(_in[0], F_SETFL, O_NONBLOCK);
		::fcntl(_out[0], F_SETFL, O_NONBLOCK);
	}
	~fake_tty() {
		for (int fd : {_in[0], _in[1], _out[0], _out[1]})
			if (fd >= 0)
				::close(fd);
	}

	fake_tty(const fake_tty&) = delete;
	fake_tty& operator=(const fake_tty&) = delete;

	[[nodiscard]] loop_fds fds() const { return loop_fds{_in[0], _out[1]}; }

	void type(std::string_view bytes) const {
		ASSERT_EQ(::write(_in[1], bytes.data(), bytes.size()),
		          static_cast<ssize_t>(bytes.size()));
	}

	void close_input() {
		if (_in[1] >= 0) {
			::close(_in[1]);
			_in[1] = -1;
		}
	}

	// Everything the loop has written since the last call.
	[[nodiscard]] std::string painted() const {
		std::string all;
		char chunk[4096];
		for (;;) {
			const ssize_t n = ::read(_out[0], chunk, sizeof(chunk));
			if (n <= 0)
				break;
			all.append(chunk, static_cast<std::size_t>(n));
		}
		return all;
	}

private:
	int _in[2]{-1, -1};
	int _out[2]{-1, -1};
};

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

// --- An action, for the timer topic ----------------------------------------

std::atomic<int> g_action_runs{0};

int32_t counting_action(lesh_editor*, const lesh_invocation*, void*) {
	g_action_runs.fetch_add(1, std::memory_order_relaxed);
	return LESH_OK;
}

int32_t exiting_action(lesh_editor* editor, const lesh_invocation*, void*) {
	return lesh_exit(editor, 7);
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

TEST(LeshperLoopTty, BytesBecomeEditsInOneTurn) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};
	loop.enter_read();

	tty.type("hi");
	const turn_result result = loop.turn(50);

	EXPECT_EQ(buffer_of(loop), "hi");
	EXPECT_EQ(result.events, 2u);
	EXPECT_TRUE(result.rendered);
}

TEST(LeshperLoopTty, ReadsAreBatchedWhileTheFdIsStillReadable) {
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

TEST(LeshperLoopTty, ABracketedPasteIsOneMutationAndOneGeneration) {
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

TEST(LeshperLoopTty, EndOfFileIsAHangupAndNotAKey) {
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

TEST(LeshperLoopTty, AnEscapeResolvesOnItsOwnDeadlineAndNotBefore) {
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

TEST(LeshperLoopSignals, ResizesAreCountedRatherThanQueued) {
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

TEST(LeshperLoopSignals, DrainConsumesTheByteAndThePendingSet) {
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

TEST(LeshperLoopSignals, ASignalBecomesAnEventOnTheOrdinaryPath) {
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

TEST(LeshperLoopSignals, AResizeIsDeliveredAsAnEventWithTheQueriedSize) {
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

TEST(LeshperLoopSignals, ASignalDoesNotTearAMultibyteSequence) {
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

TEST(LeshperLoopSignals, InstallPutsBackExactlyWhatItReplaced) {
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

TEST(LeshperLoopSignals, ReassertOverAForeignHandlerRetargetsTheChain) {
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

TEST(LeshperLoopSignals, ReassertOverAForeignWinchHandlerRetargetsTheChainToo) {
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

TEST(LeshperLoopSignals, ReassertLeavesAnIgnoreStanding) {
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

TEST(LeshperLoopSignals, ReassertLeavesAForeignHandlerOnTheIgnoredSetStanding) {
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

TEST(LeshperLoopSignals, UninstallRestoresTheEntryDispositionAndNotTheNewest) {
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

TEST(LeshperLoopSignals, TheLoopNeverWritesADisposition) {
	// #142's second amendment, as an assertion rather than a convention. Taking
	// the dispositions back used to happen on the LOOP thread, in `enter_read`
	// and on the unpark - two threads writing one piece of process-wide state,
	// the other being the shell thread's `trap` builtin. The re-assert moved to
	// the shell side of the wiring site (`read.cpp`), and what is left here must
	// call no `sigaction` at all: a foreign handler installed after `install()`
	// survives a read entry and a turn untouched.
	const every_hub_disposition guards;
	guards.child().default_action();

	fake_tty tty;
	// THE HUB IS DECLARED BEFORE THE LOOP, for the reason `session` states about
	// its actor: `~event_loop` calls `request_stop`, which pokes the attached
	// hub's pipe. A hub declared after the loop dies first and that poke is a use
	// after scope - ASan says so immediately, which is how this line got written.
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

TEST(LeshperLoopSignals, SighupIsNeverTakenAtAll) {
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
// The worker topic
// ===========================================================================

TEST(LeshperLoopWorkers, AReadableFdIsAnsweredWithDrain) {
	// #126's rule, written in its own header: answer the readable fd with
	// `drain()`, never by reading it. Reading it would leave the queue armed and
	// the next wakeup would be lost forever.
	fake_tty tty;
	registry reg;
	worker_pool helpers{1};
	std::size_t seen = 0;
	ASSERT_EQ(lesh_reactor_register(&reg, "counter", LESH_EVENT_BUFFER_CHANGED,
	                                &counting_reactor, &seen),
	          LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.attach_helpers(helpers);
	loop.enter_read();

	tty.type("ab");
	loop.turn(50);

	ASSERT_TRUE(turn_until(loop, [&] { return loop.applied_batches() > 0; }));
	EXPECT_TRUE(helpers.completions().empty());
	EXPECT_FALSE(helpers.completions().armed()) << "drain disarms; a read would not have";
	// #141: a taken batch lands in the editor's own decorations, namespaced by
	// the reactor that emitted it. There is no loop-side store any more.
	ASSERT_EQ(loop.editor().marks.layers().size(), 1u);
	EXPECT_EQ(loop.editor().marks.layers().front().reactor, "counter");
}

TEST(LeshperLoopWorkers, ABatchComputedAgainstAnOlderGenerationIsDropped) {
	// N-4, and the loop is the only applier, so this is the only place the rule
	// is decided.
	fake_tty tty;
	registry reg;
	worker_pool helpers{1};
	ASSERT_EQ(lesh_reactor_register(&reg, "counter", LESH_EVENT_BUFFER_CHANGED,
	                                &counting_reactor, nullptr),
	          LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.attach_helpers(helpers);
	loop.enter_read();

	// Submit against the generation the editor is at, then move the editor on
	// before the answer is drained.
	helpers.submit("counter", snapshot_of(loop.editor(), LESH_EVENT_BUFFER_CHANGED),
	               &counting_reactor, nullptr);
	loop.editor().gen.bump();

	ASSERT_TRUE(turn_until(loop, [&] { return loop.dropped_batches() > 0; }));
	EXPECT_EQ(loop.applied_batches(), 0u);
	EXPECT_TRUE(loop.editor().marks.layers().empty());
}

// ===========================================================================
// The shell topic (ADR-0009)
// ===========================================================================

TEST(LeshperLoopShell, TheHighlighterRunsOnTheShellThreadAndComesBackOverTheTopic) {
	fake_tty tty;
	registry reg;
	fake_shell shell;
	shell_actor actor{shell, nullptr};
	ASSERT_EQ(lesh_reactor_register(&reg, "highlighter", LESH_EVENT_BUFFER_CHANGED,
	                                &counting_reactor, nullptr),
	          LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.attach_shell(actor);
	loop.enter_read();

	tty.type("x");
	loop.turn(50);
	EXPECT_FALSE(actor.idle()) << "the highlight went to the shell thread's slot";

	// The shell thread, run by hand: `serve_one` is what `run()` is written in
	// terms of, so driving it here exercises the same path.
	ASSERT_TRUE(actor.serve_one());
	EXPECT_TRUE(actor.replies().armed());

	ASSERT_TRUE(turn_until(loop, [&] { return loop.applied_batches() > 0; }));
	ASSERT_EQ(loop.editor().marks.layers().size(), 1u);
	EXPECT_EQ(loop.editor().marks.layers().front().reactor, "highlighter");
	EXPECT_FALSE(actor.replies().armed()) << "drain disarms the shell topic too";
}

TEST(LeshperLoopShell, ANewerHighlightOverwritesAPendingOne) {
	// ADR-0009: "a newer highlight overwrites a pending one, which is the
	// cancellation." There is no cancel call in the seam, and that is the point.
	fake_shell shell;
	shell_actor actor{shell, nullptr};

	state target;
	actor.post_highlight("highlighter", &counting_reactor, nullptr,
	                     snapshot_of(target, LESH_EVENT_BUFFER_CHANGED));
	target.gen.bump();
	actor.post_highlight("highlighter", &counting_reactor, nullptr,
	                     snapshot_of(target, LESH_EVENT_BUFFER_CHANGED));

	EXPECT_EQ(actor.dropped(), 1u);
	ASSERT_TRUE(actor.serve_one());
	EXPECT_FALSE(actor.serve_one()) << "one slot, depth one, latest wins";

	std::vector<shell_message> inbox;
	ASSERT_EQ(actor.replies().drain(inbox), 1u);
	EXPECT_EQ(inbox.front().computed_against, target.gen);
	actor.replies().recycle(inbox);
}

TEST(LeshperLoopShell, ExecuteOutranksAPendingHighlight) {
	fake_shell shell;
	shell_actor actor{shell, nullptr};

	state target;
	actor.post_highlight("highlighter", &counting_reactor, nullptr,
	                     snapshot_of(target, LESH_EVENT_BUFFER_CHANGED));
	actor.post_execute("echo hi", target.gen);

	ASSERT_TRUE(actor.serve_one());
	EXPECT_EQ(shell.executed, "echo hi") << "priority order: execute, port_call, highlight";
}

TEST(LeshperLoopShell, MessagesAreRecycledRatherThanReallocated) {
	fake_shell shell;
	shell_actor actor{shell, nullptr};
	state target;

	std::vector<shell_message> inbox;
	for (int round = 0; round < 5; ++round) {
		actor.post_highlight("highlighter", &counting_reactor, nullptr,
		                     snapshot_of(target, LESH_EVENT_BUFFER_CHANGED));
		ASSERT_TRUE(actor.serve_one());
		ASSERT_EQ(actor.replies().drain(inbox), 1u);
		actor.replies().recycle(inbox);
		EXPECT_TRUE(inbox.empty());
	}
	EXPECT_EQ(actor.served(), 5u);
}

TEST(LeshperLoopShell, APortCallIsSynchronousFromTheActionsPointOfView) {
	// #92's contract, unchanged by the thread split: the action blocks, the loop
	// waits on the `shell` and `signal` topics, and the terminal keeps the
	// EDITOR's modes throughout (fish #7770).
	fake_tty tty;
	fake_shell shell;
	shell.port_status = 3;
	shell_actor actor{shell, nullptr};

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_shell(actor);
	loop.enter_read();

	std::thread shell_thread{[&] { actor.run(); }};
	const port_result answered = loop.call_port("echo from an action");
	actor.stop();
	shell_thread.join();

	EXPECT_TRUE(answered.answered);
	EXPECT_EQ(answered.status, 3);
	EXPECT_EQ(shell.called, "echo from an action");
}

// ===========================================================================
// Accept and quiesce
// ===========================================================================

TEST(LeshperLoopQuiesce, AcceptParksTheHelpersBeforeTheShellRuns) {
	// The whole of quiesce, asserted from inside the execution: by the time
	// `execute` runs on the shell thread, the helpers are parked and the loop is
	// blocked in its poll. That is the moment a fork is legal.
	fake_tty tty;
	fake_shell shell;
	worker_pool helpers{2};
	shell_actor actor{shell, nullptr};

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_helpers(helpers);
	loop.attach_shell(actor);
	loop.enter_read();

	bool parked_during_execute = false;
	shell.on_execute = [&] { parked_during_execute = helpers.is_quiesced(); };
	shell.execute_status = 42;

	tty.type("echo hi");
	loop.turn(50);
	ASSERT_EQ(buffer_of(loop), "echo hi");

	std::thread shell_thread{[&] { actor.run(); }};
	const std::optional<std::int32_t> status = loop.accept_current_line();
	actor.stop();
	shell_thread.join();

	EXPECT_TRUE(parked_during_execute) << "quiesce is the helpers parked plus the terminal";
	ASSERT_TRUE(status.has_value());
	EXPECT_EQ(*status, 42);
	EXPECT_EQ(shell.executed, "echo hi");
	// The line is finished and the editor is fresh, in one edit so undo does not
	// walk back into a command that has already run.
	EXPECT_EQ(buffer_of(loop), "");
	// Parking NESTS and resume released it: the pool is live again.
	EXPECT_FALSE(helpers.is_quiesced());
	EXPECT_FALSE(loop.quiesced());
}

TEST(LeshperLoopQuiesce, QuiesceNestsAndAssertsBothHalves) {
	fake_tty tty;
	worker_pool helpers{1};
	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_helpers(helpers);
	loop.enter_read();

	loop.quiesce();
	EXPECT_TRUE(loop.quiesced());
	loop.assert_quiesced();
	loop.quiesce();
	loop.assert_quiesced();

	loop.resume_after_execution();
	EXPECT_TRUE(loop.quiesced()) << "two parks need two resumes";
	loop.resume_after_execution();
	EXPECT_FALSE(loop.quiesced());
	EXPECT_FALSE(helpers.is_quiesced());
}

TEST(LeshperLoopQuiesce, ASignalArrivingDuringExecutionIsDeferredNotLost) {
	fake_tty tty;
	fake_shell shell;
	shell_actor actor{shell, nullptr};

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_shell(actor);
	loop.enter_read();

	// Delivered from inside `execute`, which is exactly the window where the
	// loop is blocked on the `shell` and `signal` topics only.
	shell.on_execute = [&] { loop.signals().deliver(SIGINT); };

	std::thread shell_thread{[&] { actor.run(); }};
	loop.accept_current_line();
	actor.stop();
	shell_thread.join();

	// The next ordinary turn delivers it: nothing is dropped because the editor
	// was not there to receive it.
	const turn_result result = loop.turn(0);
	EXPECT_EQ(result.events, 1u);
}

// ===========================================================================
// The timer topic
// ===========================================================================

TEST(LeshperLoopTimers, AnArmedTimerDispatchesItsAction) {
	fake_tty tty;
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "tick", &counting_action, nullptr), LESH_OK);

	std::uint64_t id = 0;
	ASSERT_EQ(lesh_timer_start(&reg, 5, "tick", &id), LESH_OK);
	EXPECT_NE(id, 0u);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.enter_read();

	const int before = g_action_runs.load();
	ASSERT_TRUE(turn_until(loop, [&] { return g_action_runs.load() > before; }));
	EXPECT_GE(loop.timer_dispatches(), 1u);

	EXPECT_EQ(lesh_timer_stop(&reg, id), LESH_OK);
	EXPECT_EQ(lesh_timer_stop(&reg, id), LESH_ERR_NOTFOUND) << "an id is stopped once";
}

TEST(LeshperLoopTimers, TheTimeoutIsTheMinimumDeadlineAndMinusOneWhenNothingWaits) {
	fake_tty tty;
	registry reg;
	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.enter_read();

	// Unarmed, the mechanism costs nothing: the loop blocks.
	EXPECT_EQ(loop.poll_timeout_ms(), -1);

	std::uint64_t id = 0;
	ASSERT_EQ(lesh_timer_start(&reg, 1000, "tick", &id), LESH_OK);
	loop.turn(0);  // the arm happens on the turn that first sees the table
	const int timeout = loop.poll_timeout_ms();
	EXPECT_GT(timeout, 0);
	EXPECT_LE(timeout, 1001);
}

TEST(LeshperLoopTimers, ZeroIntervalAndABadNameAreRefused) {
	registry reg;
	std::uint64_t id = 0;
	EXPECT_EQ(lesh_timer_start(&reg, 0, "tick", &id), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_timer_start(&reg, 5, "NotSnakeCase", &id), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_timer_start(nullptr, 5, "tick", &id), LESH_ERR_INVAL);
	EXPECT_TRUE(reg.timers.empty());
}

TEST(LeshperLoopTimers, AnActionsExitOutcomeEndsTheLoop) {
	fake_tty tty;
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "quit", &exiting_action, nullptr), LESH_OK);
	std::uint64_t id = 0;
	ASSERT_EQ(lesh_timer_start(&reg, 1, "quit", &id), LESH_OK);

	event_loop loop{tty.fds(), pipe_options()};
	loop.attach_registry(reg);
	loop.enter_read();

	ASSERT_TRUE(turn_until(loop, [&] { return loop.exiting(); }));
	EXPECT_EQ(loop.exit_status(), 7);
}

// ===========================================================================
// Rendering
// ===========================================================================

TEST(LeshperLoopRender, TheLoopKeepsThePreviousSurfaceAndDiffsAgainstIt) {
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

TEST(LeshperLoopRender, AResizeForcesAFullRepaint) {
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

// ===========================================================================
// The thread (#134's two calls)
// ===========================================================================

TEST(LeshperLoopThread, StopWakesALoopBlockedInPoll) {
	fake_tty tty;
	event_loop loop{tty.fds(), pipe_options()};

	loop.start();
	EXPECT_TRUE(loop.running());
	// Blocked in `poll` with nothing to say: `stop` rings the signal topic's own
	// pipe, which is the wakeup that always exists.
	std::this_thread::sleep_for(std::chrono::milliseconds{10});
	loop.stop();
	EXPECT_FALSE(loop.running());
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

TEST(LeshperLoopTerminal, AZeroWinsizeFallsBackToEightyByTwentyFour) {
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

TEST(LeshperLoopTerminal, RawModeIsEnteredAndRestoredWithoutEchoInBetween) {
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

TEST(LeshperLoopTerminal, EnteringRawKeepsWhateverAChildLeftBehind) {
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

TEST(LeshperLoopTerminal, BracketedPasteIsTurnedOnWithRawAndOffWithCooked) {
	pty_pair pty;
	ASSERT_TRUE(pty.ok());

	terminal tty{pty.slave()};
	ASSERT_TRUE(tty.enter_raw());
	EXPECT_NE(pty.drain().find(kBracketedPasteOn), std::string::npos);

	ASSERT_TRUE(tty.leave_raw());
	EXPECT_NE(pty.drain().find(kBracketedPasteOff), std::string::npos);
}

TEST(LeshperLoopTerminal, SetForegroundPgrpOnSomethingThatIsNotATtyAnswersNoTty) {
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

TEST(LeshperLoopTerminal, TheExitRestoreDoesNothingWhenTheTerminalIsNotOurs) {
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

TEST(LeshperLoopForkGuard, TheGuardCountsAllocationsBetweenForkAndExec) {
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

TEST(LeshperLoopForkGuard, LogSafeFormatsWithoutAllocatingOrCallingVsnprintf) {
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

TEST(LeshperLoopReplay, ARecordedByteLogReproducesTheSameStateAtEveryReadBoundary) {
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

TEST(LeshperLoopReplay, TheEventCategoryRecordsWhatTheLoopDrained) {
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

TEST(LeshperLoopTerminal, FatalRestoreHandlersAreInstalledOnlyOverTheDefault) {
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
