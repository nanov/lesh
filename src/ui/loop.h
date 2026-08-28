#pragma once

// The event loop: `poll(2)`, four topics, and quiesce (#129; #128's resolution;
// architecture spec §4 and §4.1; ADR-0011).
//
// THE HOST'S, AND IN `src/ui/` SINCE #168. This file drives leshper; it is not
// part of it. The editor is `step(state, event, now) -> effects` and knows no
// thread, fd, poll, timer or mailbox; everything on this side of that sentence -
// the loop, the tty, the reactor fibers, the timers, the shell handoff - is the
// host,
// which sends events in and performs the effects that come back. `lesh_ui` links
// both `lesh_leshper` and `lesh_runtime`, so the arrow that used to be a rule
// about includes is now a rule about which target a file is compiled into.
//
// ONE BACKEND, AND NO SEAM FOR A SECOND. `poll(2)`, POSIX, one file. #115
// measured wake-to-callback at 1.8-2.4 us on every backend with the run-to-run
// spread wider than the backend spread, so what was left to decide was not
// latency: libuv costs +57 KiB stripped and spawns four threadpool threads that
// survive `uv_loop_close` with no API to park them, sitting exactly on #91's
// quiesce hazard. There is no kqueue here, no epoll, and no `EVFILT_TIMER` -
// the last of those specifically, because XNU doubles its slack and #115
// measured the 2:1 timer gap that came of it.
//
// TOPICS ARE THE VOCABULARY (the owner's word). Four of them since #202 - the
// `shell` topic went with the thread it was a wakeup from (#201) and the
// `worker` topic went with the helper pool - and the fd is each one's
// implementation detail:
//
//   `tty`     bytes from the terminal, decoded by #111's `input_decoder`.
//   `signal`  a self-pipe. The handler saves errno, bumps a counter, writes one
//             byte, and does nothing else that could be unsafe.
//   `timer`   no fd at all: the poll timeout is `min(deadlines) - now` on a
//             monotonic clock, and `lesh_timer_start` is the public door.
//   `watch`   §8's `fd-readable` hook, and the last topic to arrive (#195). A
//             descriptor somebody else owns plus a callback the loop runs
//             inside the turn when it is readable. Its one user is the history's
//             directory watch (ADR-0010 §Locking and staleness), which is
//             `inotify` on Linux and a `kqueue` on macOS, and which the loop
//             deliberately knows nothing about: what it holds is an int and a
//             function pointer.
//
// THAT IS WHAT "TOPIC" BOUGHT over "a list of descriptors": `watch` arrived as
// two members and three lines in `turn`, and `shell` and `worker` both left the
// same way - nothing that reasons about the tty had to learn about any of it.
//
// A TURN: a reactor slice each -> poll -> drain the topics into events ->
// `editor.step` each -> a reactor slice each -> `lay_out` ->
// `blit.update(previous, desired)` -> write. The loop keeps the previous
// surface, because #112's blitter deliberately keeps no "actual screen" of its
// own that could drift out of step with the terminal.
//
// THE REACTOR SLICES ARE THE TICK (#202, step 1d of #145). This loop owns a
// `fiber::scheduler` and gives every registered reactor a long-lived fiber in
// the `emitters` group; the two `tick` calls above are the owner's words -
// "reactor slices before and after the UI part" - and the poll timeout is 0
// while any of them is still runnable. Cancellation is a `slot` send, which is
// what a buffer change already produces; the reactor notices at its next
// cancellation poll, which is also its yield point. See THE TICK below.
//
// THE CORE IS A FUNCTION OVER FDS PASSED IN. Nothing here opens `/dev/tty` or
// assumes `STDIN_FILENO`: the tests drive it over pipes and `openpty` pairs,
// never over the process's own terminal, and the N-3 replay file goes through
// the identical path. That is the test contract #128 wrote, and it is the
// reason the terminal rules live in tty.h where they can be exercised alone.
//
// ONE THREAD, AND SINCE #202 THE ONLY ONE. This loop RUNS ON MAIN. `run()` is
// what `ui/session.cpp` calls, and everything ADR-0009 split across two owner
// threads is now serialized by being one: the loop owns editor state and the
// terminal, the shell owns `shell_state`, and the loop reaches the shell by
// CALLING `shell_side::execute` and `shell_side::port_call` where it used to
// fill a slot and block on a wakeup pipe (#201). The highlighter and the
// autosuggester run here too - each on a fiber of its own, resumed by the two
// ticks a turn takes - so the process has no helper threads and no locks at all.
//
// THE TICK, in the owner's words: "run the autosuggest and highlight and so on;
// if no new keys arrived, tick them; if a new key arrived, cancel them and pass
// to leshper." Mapped onto the code below:
//
//   `turn` gives every runnable emitter one slice, polls (with a timeout of 0
//   if any of them is still runnable and the ordinary deadline otherwise),
//   drains the topics, dispatches what arrived - which is where a buffer change
//   SENDS the new snapshot into each reactor's slot, superseding whatever that
//   reactor had in flight - gives every runnable emitter one more slice, and
//   renders whatever has landed.
//
//   CANCELLATION IS THE BUFFER-CHANGE GENERATION BUMP and nothing else (#90's
//   rule, unchanged): a cursor move that leaves the buffer alone cancels
//   nothing, and a reactor mid-walk notices the send at its next cancellation
//   poll - which is also, since #202, the point at which it yields.
//
//   A SLICE IS BOUNDED BY THE REACTOR'S OWN POLL, not by the host. `kPollEvery`
//   in the highlighter's sweep and the per-entry poll in the autosuggester's
//   history walk are yield points now, so a long walk is spread over many turns
//   and every one of them polls the terminal first.
//
// ADR-0009 SEPARATED THEM ONLY BECAUSE WORKER THREADS READ SHELL STATE, and it
// stopped being true when #151 made the shell-thread reactor the one reader:
// with the reads serialized on the owning thread there is nothing left for a
// second thread to protect, and the condvar, the three slots, the reply pool and
// the `shell` topic were the whole cost of having one. `shell_executing_flag` stays
// as the assertion that says so - trivially true now, and still the tripwire if
// a future thread ever wants a read.
//
// QUIESCE, AND WHAT IS LEFT OF IT (#202). `quiesce()` is the emitters group
// parked plus the terminal restored and given up; the fork then happens inside
// the `execute` this loop calls, on this thread. The parking half used to be
// load-bearing for a reason that no longer exists - lesh forks to RUN SHELL
// CODE, which allocates in the child at once, and a child born beside a THREAD
// holding malloc's lock deadlocks on its first allocation - and there are no
// other threads now, so a fork here is taken from a genuinely single-threaded
// process by construction. What parking the group still buys is F-22's rule: at
// accept every emitter is superseded and its group is parked, so nothing
// computes for a line that has already run and nothing it computed is applied.
// The exec lanes take fish's async-signal-safe discipline anyway, and that half
// lives in substrate/fork_guard.h.
//
// ALLOCATION (N-2, and tests/unit/allocation_tests.cpp is the gate). Everything
// a turn touches is a member that keeps its capacity: the read buffer, the
// event vector, each reactor lane's two snapshots and its batch, the blitter's
// output string, the pollfd array. A warm turn allocates nothing - which is why
// a lane owns TWO snapshots and swaps them at `recv` rather than moving one
// through the slot: a moved-from string has given its capacity away.

#include "leshper/blit.h"
#include "leshper/decode.h"
#include "leshper/editor.h"
#include "leshper/layout.h"
#include "leshper/host.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "fiber/scheduler.h"
#include "fiber/slot.h"
#include "runtime/cooperation.h"
#include "ui/reactor_call.h"
#include "ui/shell_knowledge.h"
#include "ui/shell_side.h"
#include "ui/tty.h"
#include "substrate/grapheme.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::ui {

// ---------------------------------------------------------------------------
// The signal topic.
// ---------------------------------------------------------------------------

// Signal numbers this tracks. 32 spans every named signal on macOS and every
// non-real-time signal on Linux, which is every signal a shell's editor has a
// disposition for. It sizes an array a HANDLER writes, so it must be a
// compile-time constant and can never become a container (`runtime/signals.h`
// carries the same rule for the same reason).
inline constexpr int kMaxTrackedSignal = 32;

// THERE IS NO `block_caught_signals_on_this_thread` (#203). It blocked the
// hub's caught set on the calling thread, and delivery is pinned to main by
// having no other thread at all now: #201 put the loop on main and #202 turned
// the helper pool into fibers, which left the function with no callers. It is
// deleted rather than kept as documentation, and the rule it carried is written
// where a future thread will read it - ADR-0011's background-thread contract:
//
//   A process-directed signal is delivered to any one thread that does not
//   block it, so a thread that shares the caught set steals Ctrl-C at the
//   kernel's whim. But A SIGNAL MASK SURVIVES `execve`, so the block goes on the
//   spawned thread and never on main: main is the thread that forks and execs,
//   and a child born with SIGINT blocked ignores the `kill -INT` meant for it
//   (#142, #143). Any thread this host ever spawns therefore blocks the caught
//   set first thing in its body and never spawns children of its own.
//
// Main pays for the mask it does not take with an EINTR out of `poll`, which
// `turn` has always handled: the wakeup it acts on is the self-pipe byte, never
// the EINTR.

// The self-pipe, the pending set, and the resize counter.
//
// INSTANTIABLE, and only `install()` is process-global. A signal handler cannot
// take a context pointer, so exactly one hub at a time may own the
// dispositions - but the pipe and the counters are ordinary members, which is
// what lets a test build a hub, call `deliver()` on it (the handler's entire
// body) and drive the loop's signal topic with no `sigaction` anywhere near the
// test binary's real dispositions.
//
// THE HANDLER'S DISCIPLINE, and every clause of it is one of #128's traps:
//
//   errno is saved and restored. A handler that clobbers errno turns the
//   interrupted `read` into a lie.
//
//   `getpid() != main_pid` IS CHECKED FIRST, before anything else, and a child
//   resets the disposition to SIG_DFL and re-raises. NEVER an atfork flag for
//   this - fish's own comment: "Don't use is_forked_child: it relies on atfork
//   handlers which may have not yet run." substrate/fork_guard.h's flag exists
//   and is deliberately not consulted here.
//
//   The body is atomics and one `write`. No allocation, no lock, no formatting.
//
//   SIGWINCH BUMPS A COUNTER, it does not queue. #128's trap 12: the counter is
//   read by the loop BEFORE the `TIOCGWINSZ` ioctl, so a resize landing between
//   the two leaves the counter ahead and the next turn re-queries, where the
//   other order would swallow it.
class signal_hub {
public:
	signal_hub();
	~signal_hub();

	signal_hub(const signal_hub&) = delete;
	signal_hub& operator=(const signal_hub&) = delete;

	// The fd the loop's `signal` topic polls.
	[[nodiscard]] int wakeup_fd() const noexcept { return _read_fd; }

	// Installs the dispositions #128 fixed, saving what was there:
	//
	//   SIGINT   caught, WITHOUT SA_RESTART - so the `poll` is interrupted and
	//            Ctrl-C reaches the editor as an event within the turn.
	//   SIGCHLD  caught, WITH SA_RESTART - "we want SIGCHLD to not interrupt
	//            restartable syscalls" (fish `signal.cpp`).
	//   SIGWINCH caught, with SA_RESTART; the counter is the payload.
	//   SIGPIPE, SIGQUIT, SIGTSTP, SIGTTOU, SIGTTIN ignored. fish's words on the
	//            last three: "We are a shell, we know what is best for the user."
	//
	// EACH ONE ONLY WHERE THE KERNEL SAYS IT IS OURS TO TAKE - see `reassert`,
	// whose rules `install` is the first application of, not a special case.
	//
	// SIGHUP IS NOT IN THAT LIST AND THAT IS THE DECISION (#142). The editor's
	// hangup is the TTY's, not the signal's: `drain_tty` turns a POLLHUP on the
	// input fd into `signal_event{SIGHUP}` plus `_exiting`, which is the path
	// that actually fires when a terminal goes away. A real SIGHUP is left to
	// the shell's own disposition, so `trap 'cmd' HUP` fires at the next command
	// boundary exactly as it always did, `trap - HUP` kills the shell as POSIX
	// says, and `nohup`'s SIG_IGN is respected by construction rather than by a
	// conditional. THE RESIDUAL, written down rather than discovered later:
	// death by a SIG_DFL SIGHUP skips the termios restore, because nothing of
	// ours runs on that path. On a real hangup the terminal is already gone and
	// restoring it means nothing; if it is ever wanted for a synthetic
	// `kill -HUP`, `install_fatal_restore_handlers` (tty.h) is where it goes,
	// alongside the other default-fatal signals it already covers.
	//
	// Answers false if any `sigaction` failed. `uninstall()` puts back exactly
	// what was saved, which is what lets a test install and restore without
	// disturbing the rest of the binary (`tests/unit/interactive_signal_guard.h`
	// is the same discipline for the shell's own dispositions).
	bool install() noexcept;
	void uninstall() noexcept;
	[[nodiscard]] bool installed() const noexcept { return _installed; }

	// Takes the dispositions again - ASKING THE KERNEL, per signal, first.
	//
	// #134's resolution of the ownership question #129 returned. The shell's own
	// `trap` machinery (`runtime/signals.cpp`) installs a handler of its own the
	// moment a user types `trap ... INT` at the prompt, and that `sigaction`
	// silently replaces ours - after which Ctrl-C sets `g_pending` and rings no
	// pipe, so the editor never hears it. The wiring site therefore re-asserts at
	// every read entry and after every command, which is where a `trap` can have
	// run.
	//
	// TWO SAVED SLOTS PER SIGNAL, and separating them is what #142 fixed. The
	// old single save served two purposes at once - the thing `uninstall` puts
	// back AND the thing `deliver` chains to - and one slot cannot be both: a
	// save that is once-per-signal keeps `uninstall` honest but leaves the chain
	// pointing at a disposition from before the user's `trap` existed, and a save
	// that re-runs keeps the chain fresh but loses the entry-time original (or
	// records our own handler, an infinite chain). So:
	//
	//   `_saved`  written AT MOST ONCE per signal, read only by `uninstall`.
	//   `_chain`  retargeted at every take, read only by `deliver`.
	//
	// THE RULES, applied per signal at `install()` and at every `reassert()`
	// alike, from the kernel's own answer to `sigaction(signo, nullptr, &now)`:
	//
	//   1. It is already our handler -> held. Nothing to do, and self-chaining
	//      is excluded here rather than guarded against downstream.
	//   2. SIG_DFL -> take it (catch or ignore per the table above), with no
	//      chain: there was no handler, so there is nothing to call.
	//   3. SIG_IGN -> LEAVE IT. The newest ignore stands, whoever set it -
	//      `nohup`'s before exec, or `trap '' SIG` a moment ago. This is what
	//      used to be a SIGHUP-shaped special case, generalized into the rule.
	//      It makes `trap '' INT` mean what it means in bash: Ctrl-C inert at
	//      the prompt. That is intended.
	//   4. Any other real handler - in a lesh process that is only ever
	//      `runtime/signals.cpp`'s `record_signal`, installed by a `trap`:
	//        - caught set (INT, CHLD, WINCH): TAKE IT AND RETARGET `_chain` to
	//          it. The wakeup is genuinely ours - a job notice has to reach a
	//          loop blocked in `poll` - and the user's trap still runs because
	//          `deliver` calls through afterwards. Without the retarget the
	//          chain points at whatever was there before the trap and
	//          `g_pending` is never set, which is exactly how `trap 'cmd' CHLD`
	//          was silently dead.
	//        - ignored set (PIPE, QUIT, TSTP, TTOU, TTIN): LEAVE IT. The hub
	//          never consumed these; its SIG_IGN was only "better than the
	//          default", and a user's trap outranks "we are a shell, we know
	//          what is best".
	//
	// THE SHELL SIDE IS THE ONLY WRITER, and this call belongs to it (#142's
	// second amendment; trivially true since #201). `install`, `reassert` and
	// `uninstall` all `sigaction`, which is process-wide state, and the trap
	// builtin writes the same state - so the writer, the other writer and the
	// handler are one thread, which since #202 is the only thread there is. That
	// is why what the LOOP does with the hub is still only READING it: `drain`,
	// `resize_count`, and the byte in the pipe (`TheLoopNeverWritesADisposition`).
	//
	// The chain slots are atomic anyway (see `_chain`): a `sigprocmask` fence
	// would have been no fence at all, because it masks the calling thread and
	// says nothing about delivery elsewhere, and one lock-free pointer store
	// costs nothing and keeps the handler-versus-mainline race benign.
	bool reassert() noexcept;

	// THE HANDLER'S WHOLE BODY, async-signal-safe, exposed so a test can deliver
	// a signal to this hub without one being raised at the process.
	//
	// Public because a free-function handler must reach it, and named for what
	// it is: a signal has been delivered to this hub.
	//
	// AND THEN IT CHAINS (#134). After the self-pipe work - and only after, so a
	// previous handler that never returns cannot cost us the wakeup - the
	// handler in `_chain[signo]` is called, when one is there and it is a real
	// function rather than SIG_DFL or SIG_IGN. That is what keeps the shell's
	// `g_pending` being set while the editor owns the dispositions, so a user's
	// `trap INT` still fires (#98 decision 3, the zsh way) - during editing AND
	// during a command, when this thread is inside `run_input` and nothing of the
	// editor's is running at all.
	//
	// `_chain`, NOT `_saved` (#142). The chain target is whatever the kernel had
	// at the LAST take, so a `trap` typed five commands into the session is the
	// thing called; `_saved` is the entry-time disposition and belongs to
	// `uninstall` alone. See `reassert` for why one slot could not be both.
	//
	// THIS RUNS ON MAIN in a real session, which since #202 is the only thread
	// the process has - so there is nowhere else a process-directed signal could
	// be delivered. The loop hears about it as a byte all the same: the handler's
	// whole body is atomics and one `write`, and the byte is what turns a signal
	// into an event inside a turn.
	//
	// A previous handler installed with SA_SIGINFO is NOT chained: this entry
	// point has only a signal number, and inventing a `siginfo_t` to pass it
	// would be worse than saying so. Nothing lesh installs uses SA_SIGINFO, and
	// the sanitizers' handlers are for signals this hub never takes.
	void deliver(int signo) noexcept;

	// WHETHER THIS HUB CURRENTLY HOLDS `signo` - ASKED OF THE KERNEL, not read off
	// a member (#208). The question is "will a delivery of this signal ring my
	// pipe", and only the kernel knows: a `trap '' CHLD` inside the command that
	// is running RIGHT NOW has already replaced the disposition, and the next
	// `reassert` (which is on the way out of that command) has not happened yet.
	//
	// The one caller is `event_loop::await_child`, which parks a fiber on a
	// SIGCHLD it will never get if the answer is no - see the note there. One
	// `sigaction` query per foreground command, which is nowhere near a keystroke.
	[[nodiscard]] bool catches(int signo) const noexcept;

	// One byte, no signal. What `event_loop::stop()` rings to wake a loop that
	// is blocked in `poll` with nothing else to say.
	void poke() noexcept;

	// The SIGWINCH counter. READ THIS BEFORE THE ioctl, never after.
	[[nodiscard]] unsigned resize_count() const noexcept;

	// Consumes the wakeup byte and moves the pending signal numbers into `out`.
	// SIGWINCH is NOT among them - it is the counter, and a queue of resizes
	// would be a queue of stale sizes.
	std::size_t drain(std::vector<int>& out) noexcept;

private:
	// The body `install` and `reassert` share: ask the kernel, then catch,
	// ignore or stand aside per the rules on `reassert`.
	bool take_dispositions() noexcept;

	int _read_fd = -1;
	int _write_fd = -1;
	bool _installed = false;

	// Written by the handler, read by the loop. `volatile sig_atomic_t` is what
	// the C standard permits a handler to touch, and the array is fixed for the
	// same reason `runtime/signals.h`'s is.
	volatile sig_atomic_t _pending[kMaxTrackedSignal]{};
	volatile sig_atomic_t _resizes = 0;

	// What was installed before us the FIRST time we took this signal, put back
	// by `uninstall` and read by nothing else.
	struct sigaction _saved[kMaxTrackedSignal]{};
	bool _saved_valid[kMaxTrackedSignal]{};

	// What `deliver` chains to: the handler the kernel had at the LAST take, or
	// null for "nothing to call".
	//
	// A LOCK-FREE ATOMIC POINTER, and nothing bigger. `take_dispositions` writes
	// these and the handler reads them, so the store must be un-tearable - and it
	// cannot be fenced with `sigprocmask`, which masks the CALLING THREAD only
	// and says nothing about delivery to another. One pointer-sized relaxed store
	// against one relaxed load is the whole synchronisation: the race is benign
	// (an in-flight signal chains to the old target or the new one, both of which
	// are real handlers) and the value is never half-written.
	//
	// A `struct sigaction` will not do here for that reason; the flags it carried
	// are consumed at the STORE instead - an SA_SIGINFO handler is stored as null,
	// because this entry point has only a signal number to give it.
	std::atomic<void (*)(int)> _chain[kMaxTrackedSignal]{};
	static_assert(std::atomic<void (*)(int)>::is_always_lock_free,
	              "the chain slot is read from a signal handler and must never lock");
};

// ---------------------------------------------------------------------------
// The loop.
// ---------------------------------------------------------------------------

// The loop's inputs, by name. The fd behind one is its implementation detail,
// and `timer` has none at all.
enum class topic : std::uint8_t {
	tty,
	signal,
	timer,
	watch,
	count_,
};

[[nodiscard]] const char* name_of(topic which) noexcept;

// The descriptors the core is a function over. Passed in, never opened here.
struct loop_fds {
	int input = -1;
	int output = -1;
};

// HOW AN ACCEPTED LINE IS RUN (#208, and the owner's requirement on it).
//
// Both are first-class and both are tested. The choice is the HOST's, made once
// per `execute`, and nothing below `event_loop` can tell which was taken: the
// runtime sees `cooperation`, and `cooperation` sees `scheduler::block_or_park`,
// which is where the one branch lives.
enum class execution_mode : std::uint8_t {
	// The default. `execute` runs on the execution fiber, so the foreground wait
	// at the bottom of the interpreter can park and the loop stays alive: signals
	// are drained, the history's watch is polled, and #209's awaits have somewhere
	// to land.
	on_a_fiber,
	// `execute` on the host stack, `current() == nullptr` throughout, every wait a
	// blocking `::waitpid`. This is exactly the shell as it ran before this
	// ticket, kept because it is the honest fallback if a fiber stack ever turns
	// out to be the wrong place to fork from - and because a path that is not
	// exercised is a path that does not work.
	inline_,
};

struct loop_options {
	// The left prompt (F-40). A provider (#94) supplies it later; until then the
	// caller does, which is the same seam.
	std::string prompt = "$ ";
	std::string continuation;

	std::chrono::milliseconds escape_timeout = leshper::default_escape_timeout;
	leshper::terminal_capabilities capabilities = leshper::terminal_capabilities::floor();
	grapheme::width_policy width{};
	leshper::style prompt_pen{};
	leshper::style text_pen{};

	// The reactor that READS SHELL STATE (ADR-0009's keystone), NAMED FOR WHAT THE
	// NAME DECIDES. Every reactor runs on this thread - there is no other - so what
	// is left is the whole of what it ever decided: WHOSE HOST is stamped on the
	// token. A name rather than an ABI flag, because #93's registration tuple is
	// fixed and adding a bit to it for one built-in would be the side door the
	// whole registry design exists to prevent.
	std::string host_stamped_reactor = "highlighter";

	// WHAT THE SCHEDULER'S WATCHDOG DOES when ANY slice runs 50 ms without
	// yielding (#198) - it is the scheduler's, not a reactor's. `log` for a shell,
	// where a frozen prompt is bad and a dead prompt is worse; the fiber-facing
	// tests pass `abort_` because in a test a slice that never checks in is the
	// defect under study.
	fiber::watchdog_action watchdog = fiber::watchdog_action::log;

	// What a SIGINT at the prompt runs (#98 decision 2). A name, so Ctrl-C is
	// rebindable exactly as a key is (F-13), and dispatched by the loop rather
	// than by the keymap because Ctrl-C never arrives as a byte: ISIG stays on,
	// so the driver turns it into a signal before any decoder could see it.
	// Empty means the loop does nothing with SIGINT but note it, which is what
	// the editor-only tests want.
	std::string interrupt_action = "cancel_line";

	// Whether the loop owns the terminal's modes and foreground group. False in
	// the tests that drive it over a plain pipe, where there is nothing to own.
	bool manage_terminal = true;

	// WHAT THE TERMINAL DID TO THE FRAME ALREADY ON SCREEN WHEN IT WAS RESIZED
	// (#185, F-38). A repaint has to walk back up to that frame's top row before
	// it erases and redraws, and how far up that is depends on the terminal:
	//
	//   true  - a REFLOWING terminal rewrapped the frame's soft-wrapped rows at
	//           the new width, so the frame's top is now
	//           `lay_out(previous input, NEW width).screen.cursor().row` rows
	//           above the cursor. iTerm2, kitty, WezTerm, VTE (GNOME Terminal,
	//           Tilix), Windows Terminal and tmux's own panes all do this.
	//   false - a NON-REFLOWING terminal left the rows where they were, so the
	//           frame's top is `_previous.screen.cursor().row` rows above the
	//           cursor - the count from BEFORE the resize. xterm and macOS
	//           Terminal.app.
	//
	// Reflow is the default because it is what the terminals people use do, and
	// because #97 forbids asking the terminal anything at start-up - there is no
	// capability that answers this, so it is a setting rather than a probe. A
	// wrong answer costs one frame's worth of stale rows above the repaint, not
	// a corrupted screen: `\r` plus ESC[J still leave the terminal in a state
	// the next frame is painted correctly into.
	//
	// IT IS NOW ONLY ABOUT THE TERMINAL (#189). It used to be about us as well:
	// the blitter positioned to every row, so every row of the frame was a hard
	// line and nothing was soft for a reflowing terminal to rewrap - `true` was
	// describing a frame we had not painted, which is why a grow after a shrink
	// under-counted the rows and the walk-up stopped short. The blitter now
	// writes soft rows through the wrap, so `true` is correct BY CONSTRUCTION on
	// a terminal that reflows. The setting stays because the other kind still
	// exists: on xterm and Terminal.app a soft-wrapped row is still where it was,
	// and the old row count is still the answer.
	bool assume_reflow = true;

	// WHICH OF THE TWO WAYS AN ACCEPTED LINE IS RUN (#208). `LESH_EXECUTION=inline`
	// picks the other one at the wiring site; every `UiLoop*` and `UiPty*` case
	// that touches the accept path runs both.
	execution_mode execution = execution_mode::on_a_fiber;

	// The paste rule (#128's trap 4, fish's `read_normal_chars`): keep reading
	// while a zero-timeout poll says the fd is still readable, so a paste is one
	// edit and one repaint while a typed character paints now. The cap keeps a
	// process piping a gigabyte into the editor from being read in one turn.
	std::size_t readahead_limit = 64 * 1024;
};

// WHERE THE SESSION IS, AND WHICH FIBERS MAY RUN (#202; #145's grilling record).
//
// Written at exactly two places - `quiesce()`, which is the accept path's park,
// and `resume_after_execution()`, which is what runs when `execute` returns -
// and read by nothing that decides anything: which groups are runnable is
// derived from it rather than tracked beside it. `editing` and `executing` are
// the two states a session is ever observed in; `boundary` exists for the
// instant between them, where the history append has already happened inside
// `session::execute` and the prompt is refreshed before the editor comes back.
enum class phase : std::uint8_t {
	editing,     // the emitters are runnable and the terminal is ours
	executing,   // a command is running: the emitters group is parked
	boundary,    // `execute` has returned; the prompt is being put back
};

[[nodiscard]] const char* name_of(phase which) noexcept;

// THE SCHEDULER'S LANES, NAMED. `fiber::scheduler` is deliberately agnostic
// about what its eight group indices mean (#200); these are the host's names for
// the two the design has.
//
// `observers` HAS NO MEMBERS YET and the id is reserved on purpose: the owner's
// framing of the two reactor kinds puts history persistence and telemetry there,
// they are fed by an ordered `queue<T,N>` rather than by a conflating `slot`, and
// they are the group that stays runnable through `boundary`. Naming the id here
// is what keeps `emitters` from silently meaning "all fibers".
enum class fiber_group : std::uint8_t {
	emitters = 0,
	observers = 1,
	// THE EXECUTION LANE (#208), AND IT IS NEVER PARKED BY PHASE. Every other
	// lane's bit is derived from where the session is; this one is what the phase
	// is ABOUT, so deriving it would be circular - `executing` means "the
	// execution fiber is the thing that may run". It holds exactly one fiber, and
	// only in `execution_mode::on_a_fiber`.
	execution = 2,
};

[[nodiscard]] inline constexpr std::uint8_t group_index(fiber_group which) noexcept {
	return static_cast<std::uint8_t>(which);
}

[[nodiscard]] inline constexpr std::uint8_t group_mask(fiber_group which) noexcept {
	return fiber::group_mask_of(group_index(which));
}

// One registered reactor's fiber, its slot, and the storage its compute serves
// out of. Defined in loop.cpp - nothing outside it needs the shape, and holding
// it behind a `unique_ptr` is what makes a lane's address stable for the life of
// the loop, which the fiber's userdata and the slot's parked-receiver pointer
// both require.
struct reactor_lane;

// What one turn did. A value rather than a log line, so a test can assert on it.
struct turn_result {
	std::size_t events = 0;         // events fed to editor.step
	std::size_t topics_drained = 0; // topics that had something to say
	bool rendered = false;
	bool timed_out = false;         // poll expired with nothing ready
	bool exiting = false;           // the loop is done: EOF, an exit action, stop()
};

// The port's answer, for the action blocked on it (#92's contract: synchronous
// from the action's point of view).
struct port_result {
	std::int32_t status = LESH_OK;
	bool answered = false;
};

class event_loop {
public:
	event_loop(loop_fds fds, loop_options options = {});
	~event_loop();

	event_loop(const event_loop&) = delete;
	event_loop& operator=(const event_loop&) = delete;

	// THE EDITOR STATE, owned here. ADR-0009: the loop owns it, and nothing else
	// may touch it while the loop is running.
	[[nodiscard]] leshper::state& editor() noexcept { return _state; }
	[[nodiscard]] const leshper::state& editor() const noexcept { return _state; }

	[[nodiscard]] const loop_options& options() const noexcept { return _options; }
	[[nodiscard]] loop_options& options() noexcept { return _options; }
	[[nodiscard]] terminal& tty() noexcept { return _terminal; }

	// --- Attachment ---------------------------------------------------------
	//
	// Each is optional and each adds a topic. An editor with nothing attached is
	// still a working editor over `tty`, `signal` and `timer`, which is what the
	// smallest tests drive.

	// The `signal` topic, which always exists: the loop builds its own hub, so
	// `stop()` has a pipe to ring even in a process that installed no handler.
	// `attach_signals` repoints it at one the caller owns.
	[[nodiscard]] signal_hub& signals() noexcept { return *_signals; }

	// THERE IS NO `attach_helpers` (#202). The helper pool, its four threads, its
	// latest-wins slots, its arenas, its pooled messages and the `worker` topic
	// that carried their answers back are all gone: a reactor is a fiber in this
	// loop's own scheduler, spawned when the dispatch table is built, and there is
	// nothing left to attach. `attach_registry` below is what gives the loop
	// reactors to spawn fibers for.
	void attach_registry(leshper::registry& reg) noexcept;

	// THE SHELL, CALLED DIRECTLY (#201). What this attaches is no longer a topic:
	// `execute` and `port_call` are made from `accept_current_line`,
	// `finish_cancelled_line` and `call_port` on this thread, and the actor, the
	// slots, the reply pool and the wakeup pipe that used to carry them are gone.
	//
	// `host` is the door to what the shell KNOWS (`leshper::host`, #168 Phase B),
	// stamped on every token the shell-thread reactor is given - and it is a
	// PARAMETER rather than a second attachment for the reason #151 wrote when it
	// moved off the snapshot: a field that has to be filled in somewhere else is a
	// field that can be forgotten there, and that one was, for a whole wave. Null
	// is still legal and still means "no host attached" - every name classifies as
	// LESH_COMMAND_UNKNOWN - but it has to be WRITTEN.
	//
	// `executing` is ADR-0009's tripwire, raised around the two calls. Null means
	// the loop keeps raising its OWN, which nothing reads - the same arrangement
	// `_own_signals` makes for the hub, and what lets the raise itself be
	// unconditional (#211 §1.7).
	//
	// None of the three is owned and all must outlive the loop.
	void attach_shell(shell_side& shell, const leshper::host* host = nullptr,
	                  shell_executing_flag* executing = nullptr) noexcept;

	// What every token this loop mints for the shell-thread reactor reads through.
	// Exposed so the wiring site can assert the loop and the registry are looking
	// at one object, which is the assertion #151 left behind.
	[[nodiscard]] const leshper::host* shell_host() const noexcept { return _shell_host; }

	void attach_signals(signal_hub& hub) noexcept;

	// THE `watch` TOPIC (#195), which is §8's fd-readable hook and is one
	// attachment like the four above.
	//
	// A FUNCTION POINTER AND A `void*`, NOT A `std::function`, and it is the
	// same trade #93's reactor tuple makes: a `std::function` built from a
	// capturing lambda heap-allocates at the attachment site and hides an
	// indirect call behind a type nothing here can name. The one call site is a
	// captureless lambda plus the owner's address, which is three lines there
	// and no allocation anywhere.
	//
	// `on_readable` RUNS INSIDE THE TURN, on the one thread, and is responsible
	// for CONSUMING what made the fd readable - a hook that leaves its descriptor
	// readable turns the poll into a spin. It must not block; the history's drain is a
	// non-blocking read, a `stat` and a pointer swap.
	//
	// The descriptor is BORROWED. The loop never closes it and never reads it;
	// whoever attached it owns it and must detach before it dies.
	void attach_watch(int fd, void (*on_readable)(void* userdata),
	                  void* userdata) noexcept;
	void detach_watch() noexcept;
	[[nodiscard]] int watch_fd() const noexcept { return _watch_fd; }
	// Times the `watch` topic's hook has been run. A test waits on this.
	[[nodiscard]] std::size_t watch_drains() const noexcept { return _watch_drains; }

	// THERE IS STILL NO `attach_shell_knowledge` (#151). #135 had one: the loop
	// held the session's `shell_knowledge*` and put it on the snapshot of the
	// shell-state reactor, on no other, and the token built on the far side then
	// dropped it - a field that has to be copied across a thread is a field that
	// can be dropped there. #151 moved it onto `shell_actor`; #201 moved it here
	// WITH THE SHELL (`attach_shell`'s second parameter), which is the same rule
	// holding: it arrives with the object it describes, is stamped at the one place
	// a token is minted, and cannot be omitted without being written.
	// What a helper's snapshot carries is unchanged: null, meaning "no host
	// attached", which is the honest answer for a state-free reactor. (#168 Phase
	// B: what is stamped is a `const leshper::host*`, the one door the editor has;
	// the tables are behind it with the `$PATH` sweep.)

	// --- The read entry (#98, fish #9181) -----------------------------------

	// Everything that must happen every time the editor starts reading:
	// SIGTTOU ignored, the foreground group RECLAIMED unconditionally, raw mode
	// re-asserted, and the winsize re-queried - which is what makes a missed
	// resize during a command structurally impossible rather than handled.
	void enter_read();

	// Gives the terminal back: cooked modes, bracketed paste off, held input
	// dropped. What `quiesce` calls, and what the caller calls when the editor
	// is finished for good.
	void leave_read();

	// --- Turns ---------------------------------------------------------------

	// One poll and everything that follows from it. `timeout_ms` is the caller's
	// - a test passes 0 for "whatever is ready now" - and the no-argument form
	// computes it from the deadlines, which is what `run()` uses.
	turn_result turn(int timeout_ms);
	turn_result turn();

	// `min(deadlines) - now`, in milliseconds, or -1 when nothing is waiting on
	// time. The `timer` topic IS this number: there is no timer fd.
	[[nodiscard]] int poll_timeout_ms() const noexcept;

	// Turns until the editor exits or `request_stop()` is called. THE READ, whole:
	// `enter_read`, the first paint, turns, `leave_read`.
	//
	// ON THE CALLER'S THREAD, WHICH IS MAIN (#201). This was a thread body - the
	// loop `start()`ed itself and `stop()` joined it - and now it is what
	// `session::run` calls where those two calls used to be. `start`, `stop` and
	// `running` are gone with the `std::thread`; there is nothing to join.
	void run();

	// Asks `run` to leave after the turn it is in. Set by Ctrl-D, by an `exit`
	// the shell just ran (`session::execute`, from inside the `execute` call this
	// loop made) and by a hangup; read by `run`'s own `while` and by
	// `accept_current_line` immediately after the call returns.
	void request_stop() noexcept;

	// --- Quiesce (#91, #128, ADR-0011) --------------------------------------

	// The emitters superseded and their group parked, the terminal restored and
	// given up, the decoder's held bytes dropped. After this returns, a fork on
	// this thread is legal.
	//
	// AND IT IS ONE OF THE TWO PHASE WRITERS (#202): this is the accept path's
	// park, so it is where `editing` becomes `executing`. The supersede is F-22's
	// rule - at accept a pending highlight is abandoned - and it is what makes
	// "an emission computed for the dead line is never applied" a fact rather
	// than a hope: the flag is up, so the fiber's own poll abandons the walk, and
	// the receiver declines to apply a batch whose token was superseded.
	//
	// IDEMPOTENT, AND NOTHING NESTS (#203). This used to keep a depth counter,
	// because #91's park was a negotiation with a set of threads and two callers
	// could each need one. The two callers are `accept_current_line` and
	// `finish_cancelled_line`, neither reachable from inside the other, and the
	// nested case the counter was really held for - `vared` running the editor
	// from inside a command - is phase 2's nested await and not a second park.
	// So a second call is a no-op and one resume is owed, which `resume_after_execution`
	// asserts.
	void quiesce();

	// The other end, and the SECOND phase writer: `executing` becomes `boundary`
	// the moment `execute` has returned - the history append already happened
	// inside `session::execute` and the prompt is refreshed here - and `boundary`
	// becomes `editing` when the terminal, the modes and the size are back and the
	// emitters group is resumed. The next render is a full repaint: the screen is
	// whatever the command left, so there is no `previous` to diff against.
	void resume_after_execution();

	// The debug assertion every fork-and-continue site carries (#91), and since
	// #203 it asserts the two things that are not already a store two lines up:
	// NO EMITTER IS MID-SLICE, and the terminal is out of raw mode. The first is
	// structurally true - the host is the only resumer, and nothing a reactor can
	// reach forks - and is asserted anyway, because "structurally true" is a
	// sentence and an assertion is a test; the second is the half only the loop
	// can check. What went is the bookkeeping: asserting that `quiesce()` set the
	// bit it sets is not evidence of anything.
	void assert_quiesced() const noexcept;
	[[nodiscard]] bool quiesced() const noexcept { return _quiesced; }

	// --- Accept, and the port ------------------------------------------------

	// The whole accept path: park, restore the terminal, CALL `execute`, then
	// reclaim, re-assert, resume and redraw with a fresh line.
	//
	// Answers the status the shell reported, or nothing when no shell is
	// attached - in which case the line is still finished and cleared, which is
	// what the editor-only tests exercise.
	std::optional<std::int32_t> accept_current_line();

	// The other half of `cancel_line`: the shell sets `$?` = 130 and runs the
	// user's INT trap (#98 decision 3). Delivered as an EMPTY LINE through the
	// same `execute` an accepted line takes, on the same park-restore path,
	// because a trap body may fork. A no-op with no shell attached.
	void finish_cancelled_line();

	// #92's port, from the loop's side: call `shell_side::port_call` and answer
	// what it said. The action sees a synchronous call because it IS one; the
	// terminal keeps the EDITOR's modes throughout (fish #7770 - an action's shell
	// code never gets ECHO back).
	port_result call_port(std::string_view code);

	// --- The foreground wait (#208) -----------------------------------------

	// WHAT `cooperation::wait_child` BECOMES ON AN INTERACTIVE HOST, and the only
	// thing in this class the runtime can reach. `loop_cooperation` below is the
	// adapter; this is the implementation, and it is short because
	// `scheduler::block_or_park` owns the one decision:
	//
	//   nobody to park (inline mode, an action's `port_call`, the EXIT trap after
	//   `run()` has returned) -> a blocking `::waitpid`, exactly as before;
	//   on the execution fiber -> the waiter goes into `_awaits`, the fiber parks,
	//   and the SIGCHLD wake reaps it.
	//
	// ONLY AWAITED PIDS ARE EVER REAPED, never `-1`: a background child stays a
	// zombie until `wait` asks for it, exactly as today, so no job-control
	// semantics move in this ticket.
	pid_t await_child(pid_t pid, int flags, int* status) noexcept;

	// --- The input wait (#209) ----------------------------------------------

	// WHAT `cooperation::await_readable` BECOMES ON AN INTERACTIVE HOST, and the
	// sibling of `await_child` in every respect that matters: the same primitive
	// owns the same one decision, the entry lives on the awaiting fiber's own
	// frame, and this function is five lines because of it.
	//
	//   nobody to park -> nothing at all; the caller's `::read` blocks as today;
	//   on the execution fiber -> the fd goes into `_awaits`, it joins the poll
	//   set for exactly as long as the wait lasts, the fiber parks, and the
	//   readiness wakes it.
	//
	// AND IT IS THE TTY'S WAY BACK INTO THE POLL SET WHILE A COMMAND RUNS. #208
	// took the tty TOPIC out during `executing` - the bytes a user types belong to
	// the command, not to an editor that is not on screen - and this puts the same
	// descriptor back, as an INTEREST rather than as a topic: nothing drains it,
	// nothing decodes it, and the byte that made the poll return is still in the
	// kernel for the builtin's own `::read` to take. A key that arrives while
	// nothing awaits the tty does exactly what it did before: nothing. It waits in
	// the kernel buffer for the next `read` or for the next prompt.
	void await_readable(int fd) noexcept;

	// THERE IS NO `read_names` (#151). #139 gave the completer a round trip on
	// the actor's `enumerate` slot; the owner's reading of ADR-0009 removed the
	// need for one. The loop may read shell state directly while nothing
	// executes, and while the loop is running an action nothing CAN execute:
	// `execute` and `port_call` are the only writers and the loop is what CALLS
	// them, so an action dispatched by the loop is by construction not inside
	// either. So the completer holds a `const shell_knowledge*` and calls
	// `enumerate` on it, on this thread, and `shell_executing_flag` asserts the
	// premise on every read rather than leaving it as a paragraph. (#168 Phase B:
	// the completer is `ui::shell_completer` and the loop reaches it through
	// `ui::editor_host`, which is what `lesh_complete` raises `want_completion`
	// at. Same thread, same premise, one fewer interface.)

	// --- Rendering -----------------------------------------------------------

	// Lays out and blits. `previous` is kept here, and a full repaint is what
	// happens when the sizes disagree or the screen's contents are unknown.
	void render();
	// Forgets `previous`, so the next render repaints everything.
	void invalidate() noexcept { _have_previous = false; }

	// --- What the loop learned -----------------------------------------------

	// What each reactor last said, applied under the generation drop rule, is
	// `editor().marks` and `editor().proposals` - the painted half and the half
	// an accepting action reads - and is not accessible from anywhere else. #129
	// left a `decorations()` accessor here standing in for `state::decorations`
	// while that was still a placeholder, and said that when it gained a type the
	// application would land in `take_batch` and the accessor would go with it.
	// #141 gave it a type and #144 gave the proposals one beside it; what is left
	// here is a COUNT, which is what a test waits on.
	[[nodiscard]] std::size_t applied_batches() const noexcept { return _applied; }
	// Batches dropped because their generation had moved on (N-4, counted).
	[[nodiscard]] std::size_t dropped_batches() const noexcept { return _dropped; }
	[[nodiscard]] std::size_t timer_dispatches() const noexcept { return _timer_dispatches; }

	// --- The reactor fibers (#202) -------------------------------------------

	[[nodiscard]] phase session_phase() const noexcept { return _phase; }
	// Whether the execution fiber has been spawned. Null until the first line is
	// accepted, and null for ever in `execution_mode::inline_` - which is what a
	// test asserts to know which of the two paths it is on.
	[[nodiscard]] bool has_execution_fiber() const noexcept { return _execution != nullptr; }
	// WHETHER A COMMAND IS ON THAT FIBER RIGHT NOW, which is the one state this
	// loop may never be destroyed in: `~scheduler` unmaps a parked stack without
	// unwinding it, so an abandoned `execute` leaks the executor's frames, leaves
	// its redirection fds open, orphans its child and never unwinds
	// `shell_executing_flag::scope`. Between commands the fiber is parked in
	// `recv` on its inbox, and the inbox's waiter is the whole of the evidence.
	[[nodiscard]] bool has_execution_fiber_mid_command() const noexcept {
		return _execution != nullptr && !_exec_inbox.has_waiter();
	}
	// THE ONLY WAY A TEST CAN REACH THE FATAL-POLL PATH. `poll(2)` answers
	// POLLNVAL in `revents` for a closed descriptor and returns -1 only for
	// conditions a test cannot arrange with a real fd, so the next `n` polls in
	// `turn` answer -1 with EBADF instead - which is what a terminal that has gone
	// away looks like from here. Zero, the default, is every shell.
	void fail_next_polls(int n) noexcept { _fail_polls = n; }
	// Slices the execution fiber has been given, and waits outstanding right now -
	// a child's or a descriptor's, because since #211 §1.1 there is one table and
	// it does not distinguish. Both are counters a test reads; neither decides
	// anything, and a table that is not empty when a wait is over is a pointer
	// into a frame that has returned.
	[[nodiscard]] std::size_t execution_slices() const noexcept;
	[[nodiscard]] std::size_t awaited() const noexcept { return _await_count; }
	// THE SCHEDULER, and it is named for what it is: it holds the execution fiber
	// as well, and `reactors()` had not been true of it since #208.
	[[nodiscard]] const fiber::scheduler& scheduler() const noexcept { return _sched; }
	// Lanes, which is one per reactor the dispatch table has ever held.
	[[nodiscard]] std::size_t lanes() const noexcept { return _lanes.size(); }

	// Per-reactor counters, by name; zero for a name with no fiber. `slices` is
	// the scheduler's own count of resumes, which is what "a slice before AND
	// after the UI part" is asserted with; `computes` counts `recv`s served,
	// `abandoned` counts batches a supersede kept from being applied, and
	// `yields` counts the mid-compute yields the cancellation poll performed.
	[[nodiscard]] std::size_t reactor_slices(std::string_view reactor) const noexcept;
	[[nodiscard]] std::size_t reactor_computes(std::string_view reactor) const noexcept;
	[[nodiscard]] std::size_t reactor_abandoned(std::string_view reactor) const noexcept;
	[[nodiscard]] std::size_t reactor_yields(std::string_view reactor) const noexcept;
	// Sends into this reactor's slot, and the subset of them that dropped a
	// notification nobody had picked up - `slot`'s own debug counters (#198).
	[[nodiscard]] std::uint64_t reactor_sends(std::string_view reactor) const noexcept;
	[[nodiscard]] std::size_t reactor_superseded_sends(std::string_view reactor) const noexcept;
	[[nodiscard]] bool exiting() const noexcept { return _exiting; }
	[[nodiscard]] std::int32_t exit_status() const noexcept { return _exit_status; }

private:
	using clock = std::chrono::steady_clock;

	void drain_tty(leshper::input_instant now, turn_result& result);
	void drain_signal_topic(turn_result& result);
	void drain_watch_topic(turn_result& result);
	void fire_timers(leshper::input_instant now, turn_result& result);

	void handle(const leshper::event& incoming, turn_result& result);
	// Everything a turn's effects ask for, and the ONE place any of it happens
	// (#168): the repaint flag, the timer table, and the line outcomes that used
	// to be pulled off the editor as a latch.
	void carry_out(const leshper::effects& produced);
	// The effects an ABI verb queued on the registry with no dispatch to return
	// them through - `lesh_timer_start` from the wiring site is the whole of it
	// today. Taken at the top of a turn.
	void drain_registry_effects();
	// A buffer change, a cursor move or a selection change, SENT to every reactor
	// whose mask asked for it. The send is the cancellation (#90's rule): it bumps
	// the slot's counter, raises the lane's flag, and wakes the fiber.
	void notify_reactors(std::uint32_t kinds);
	// One slice for every runnable emitter, plus the batches those slices left.
	// Called twice per turn - the "before and after the UI part" the owner asked
	// for - and once more per pass while anything is still runnable.
	//
	// AND FOR THE EXECUTION FIBER TOO (#208), because the two sets are never
	// runnable at the same time and the scheduler already skips a parked group:
	// while a command runs the emitters' bit is down, and while a line is being
	// edited the execution fiber is parked on its inbox. One tick, one mask.
	//
	// `void`, because `tick`'s answer - "is anything still runnable" - is a
	// question the two call sites ask the scheduler for themselves, one turn later
	// and with the mask in hand.
	void tick_fibers();
	// The lanes this loop ever gives a slice to. Not derived from the phase - the
	// scheduler's own park bit is - so it is a constant, and it is a name only so
	// that the three places that need it cannot disagree.
	static constexpr std::uint8_t sliced_lanes =
		static_cast<std::uint8_t>(group_mask(fiber_group::emitters)
		                          | group_mask(fiber_group::execution));
	// Rebuilds `_dispatch_table` from the registry's reactor map. Called only
	// when the cheap staleness check in `notify_reactors` says the copy is out of
	// date; the steady state never reaches it.
	void refresh_dispatch_table();
	void take_batch(leshper::reactor_batch& answer);

	// --- The reactor fibers (#202) -------------------------------------------

	// The lane for `name`, created and its fiber spawned on first use. Lanes are
	// never removed: a fiber cannot be cancelled in v1 (destroying a parked one
	// does not unwind its stack), and a reactor is never unregistered, so a
	// rebuilt dispatch table re-points at the lanes it already has.
	reactor_lane& lane_for(std::string_view name);
	// THE FIBER BODY: `for(;;){ recv; run_reactor_here; apply }`. A static member
	// because `fiber::entry_fn` is a plain function pointer and a `void*`, which
	// is the same shape `lesh_reactor_fn` has, and for the same reason.
	static void reactor_body(fiber::scheduler& on, void* userdata);
	// What the reactor's cancellation poll yields to. See
	// `lesh_request::cooperate`.
	static void reactor_yield(void* userdata);
	// The receiver's half of the drop rule: apply the batch unless the token it
	// was computed under has been superseded.
	void apply_reactor_batch(reactor_lane& lane);
	// The lane whose name is `reactor`, or null.
	[[nodiscard]] const reactor_lane* lane_named(std::string_view reactor) const noexcept;
	// Every emitter superseded, and every one of them run out to its next poll so
	// that no fiber is left mid-compute. What the destructor calls, and the reason
	// it is not optional: a stack unmapped mid-compute takes whatever that stack
	// owned with it, which the leak gate would report and be right to.
	void drain_emitters();
	void refresh_size_from_terminal();

	// --- The execution fiber and the waiter table (#208) ---------------------

	// The two ways to run an accepted line, and the ONE place the choice is made.
	// `accept_current_line` and `finish_cancelled_line` both call this and neither
	// knows which it got.
	std::int32_t run_the_line(std::string_view line);
	// THE FIBER BODY: `for(;;){ line = inbox.recv(); done.send(shell.execute(line)); }`.
	// A static member for the reason `reactor_body` is one: `fiber::entry_fn` is a
	// plain function pointer and a `void*`.
	static void execution_body(fiber::scheduler& on, void* userdata);
	// ONE OUTSTANDING WAIT, ON THE WAITING FIBER'S OWN STACK - AND ONE TABLE FOR
	// EVERY KIND OF WAIT (#208, #209; made one by #211 §1.1). The entry is a local
	// in `await_child`'s or `await_readable`'s frame and the table holds its
	// address, which is safe for precisely the length of the wait: the fiber is
	// parked, so the frame is frozen, and the entry is removed before `complete`
	// lets it run again. (coost's `io_event` shape, and the reason the table needs
	// no allocation and no ownership rule.)
	//
	// WHAT IS BEING WAITED FOR LIVES IN THE CALLER'S OWN FRAME AND NOT HERE: a
	// pid, its flags and the executor's `wait_status`; or a descriptor. What this
	// loop keeps is a function that answers "has it happened, and with what" and
	// the `void*` to ask it about - the same trade `lesh_reactor_fn` and the
	// `watch` topic already make. It is what kept the second verb from being a
	// second table, a second servicer, a second accessor and a second guard in
	// `turn`, and it is what keeps the third from being those things either.
	struct awaiter {
		bool (*ready)(void* self, std::intptr_t& answer) noexcept = nullptr;
		void* self = nullptr;
		// The descriptor this wait needs in the turn's poll set, or -1 for a wait
		// whose wake arrives some other way - a child's does, as the SIGCHLD byte
		// on the signal topic.
		int fd = -1;
		fiber::await_slot slot;
	};

	// One entry into the table, and the probe that follows it. The `enlist` half of
	// both verbs, so the capacity check and the ask-once are written once.
	void enlist(awaiter& waiting) noexcept;

	// Every outstanding wait asked whether it is over, and every one that is
	// removed and its waiter woken. Run when the poll comes back - whatever made
	// it come back - and once more from inside each verb's own `enlist`, which is
	// how a thing that has ALREADY happened is noticed without a wake: the child
	// that exited turns ago, the regular file that is always readable.
	void service_awaits() noexcept;

	// A FIXED CAPACITY AND AN ASSERT, because both verbs are reached through a
	// `noexcept` interface and must not allocate on the way. EIGHT, where one is
	// what today's shapes can produce: the execution fiber waits for one thing at
	// a time and nothing else waits at all. The spare are for the nested read
	// `vared` will want and the providers phase 2 will add; an overflow asserts and
	// then DEGRADES to the no-op - the caller blocks, which is what it did before
	// either verb existed - rather than writing past the end of the table.
	static constexpr std::size_t kMaxAwaits = 8;

	// How far above the cursor the top of the frame the terminal is showing sits
	// (#185). `loop_options::assume_reflow` picks the model; the reflowing one
	// re-lays `_previous_input` at the current size and reads the cursor row off
	// it. Only ever asked on the repaint path, which is a resize and not a
	// keystroke, so the layout it mints is not on N-1's hot path.
	[[nodiscard]] leshper::cursor_placement frame_top_above_cursor();

	loop_fds _fds;
	loop_options _options;

	leshper::state _state;
	terminal _terminal;
	leshper::input_decoder _decoder;
	leshper::cluster_pool _pool;
	leshper::blitter _blitter;
	leshper::layout _previous;
	bool _have_previous = false;

	// THE PREVIOUS FRAME'S INPUT, kept beside the previous frame itself (#185).
	//
	// `_previous` says what was painted at the OLD width, and after a resize
	// that is no longer what the terminal is showing - a reflowing terminal has
	// rewrapped it. Re-laying the same input at the new width is the only way to
	// ask where that frame's top row has moved to, and re-laying needs the input
	// rather than the picture. Only the part `lay_out` reads from an owner is
	// here: `marks`, `theme` and `pager` are borrowed pointers into state that
	// has since moved on, and none of the three changes the GEOMETRY the start
	// row is read off - a span colours a cell, it does not move it.
	//
	// OWNING STRINGS, ASSIGNED IN PLACE. `layout_input` borrows views into the
	// editor's buffer and the caller's prompt, which are both gone by the time a
	// resize arrives. Assigning into strings that keep their capacity is what
	// stops N-2's per-render allocation pin from growing a term: the repaint
	// path is not per-keystroke, but this retention is.
	struct retained_input {
		std::string prompt;
		std::string continuation;
		std::string buffer;
		leshper::position cursor;
		grapheme::width_policy width{};
		leshper::style prompt_pen{};
		leshper::style text_pen{};
	};
	retained_input _previous_input;

	// What a span's interned semantic id looks like (#124, #141). Held by the
	// loop rather than by the state, for the reason #118 kept the registries
	// out: a theme is ENVIRONMENT - what the user has configured - and a copied
	// state must not fork it nor N-3's equality compare it. Refreshed from the
	// registry's intern table on the render path, where it costs a size
	// comparison once the names have stopped arriving.
	leshper::style_table _theme;

	leshper::registry* _registry = nullptr;
	shell_side* _shell = nullptr;
	// The executing shell's own door, stamped on every token the shell reactor is
	// given, and the flag raised around the two calls that write shell state.
	// Both arrive with the shell at `attach_shell`.
	const leshper::host* _shell_host = nullptr;
	// THE TRIPWIRE, AND THE LOOP ALWAYS HAS ONE. `_own_executing` when nothing
	// attached its own, exactly as `_own_signals` backs `_signals` - which is what
	// lets every raise be unconditional and `scope` take a reference (#211 §1.7).
	shell_executing_flag _own_executing;
	shell_executing_flag* _executing = &_own_executing;
	// THE SIGNAL TOPIC ALWAYS EXISTS, so this is never null: the constructor
	// builds `_own_signals` and points here at it, and `attach_signals` only ever
	// repoints. Nothing tests it.
	signal_hub* _signals = nullptr;
	// THE LOOP HAS NO DISPATCHER OF ITS OWN any more (#168). `_dispatch` was a
	// second `loop_harness`, minted at `attach_registry`, and it existed for one
	// caller: a timer expiry, which the loop invoked by name itself. A timer is an
	// event now (`timer_fired`), `step` dispatches it through the state's own
	// context, and the last place where two harnesses could disagree about what
	// had been applied (#144) is gone with it.
	// Set when this loop constructed its own hub, which is the ordinary case;
	// an attached one belongs to the caller.
	std::optional<signal_hub> _own_signals;

	// --- Reused storage. Every one of these is why a warm turn allocates. ---
	std::string _read_buffer;
	std::vector<leshper::event> _events;
	// THE BATCH BEING WALKED, swapped out of `_events` (#162). `handle` pushes
	// onto `_events` while the walk holds a reference into it - a shell message
	// drained inside a blocked wait was the original, and the in-place shell
	// reactor's `worker_result` was the one #201 re-pinned it on - which
	// reallocates the vector out from under the walk. Swapping first means the
	// push lands in an empty `_events` and is walked by the next pass instead. A
	// member, so the capacity survives the turn and the steady state allocates
	// nothing. Since #202 the reactor's own `worker_result` is pushed from a fiber
	// slice BETWEEN two walks rather than from inside one, which is a second
	// reason it cannot dangle rather than a reason to drop the first.
	std::vector<leshper::event> _carried_events;
	std::vector<int> _signal_numbers;
	// WHAT ARRIVED WHILE A COMMAND WAS RUNNING (#208), REPLAYED WHEN THE EDITOR IS
	// BACK. `_deferred` was deleted in #201 with the words "nothing polls during
	// an execution now"; something does again, so the smallest possible form of it
	// comes back - a list of signal NUMBERS, and nothing else.
	//
	// The fact it preserves is #201's own: nothing is dropped because the editor
	// was not there to receive it. Before this ticket the byte simply stayed in the
	// self-pipe for the length of the command and the next ordinary turn made an
	// event of it; now the byte is consumed during the command (it is what ends the
	// foreground wait), so the numbers are held here instead and the next ordinary
	// turn makes exactly the same events. What must NOT happen is dispatching them
	// while `executing`: a SIGINT turned into `cancel_line` there would call
	// `execute` from inside `execute`.
	//
	// DEDUPLICATED, because the hub's own `_pending` is a level and not a count -
	// so this is bounded by `kMaxTrackedSignal` however long the command runs.
	std::vector<int> _deferred_signals;
	std::string _out;
	std::string _accepted;
	leshper::effects _carried;         // what the registry queued, taken per turn

	// THE REACTOR TABLE, FLATTENED (the reorg cleanup). `notify_reactors` used to
	// walk `registry::reactors` - a red-black tree - and string-compare
	// `host_stamped_reactor` against every key, once per reactor per keystroke.
	// This is the same table contiguous, with that comparison already made, and
	// it is rebuilt only when the registry's `reactors_generation` moves (or the
	// registry, or the shell-thread reactor's name, changes underneath it). The
	// steady state is three scalar comparisons and a walk over a vector.
	//
	// `name` BORROWS the registry's map key, which is safe for the reason a
	// std::map key is: nodes are stable, and the only thing that adds one bumps
	// the generation that invalidates this.
	struct reactor_dispatch {
		std::string_view name;
		lesh_reactor_fn fn = nullptr;
		void* userdata = nullptr;
		std::uint32_t event_mask = 0;
		// Whether this reactor's tokens carry the shell's own host (#151). It was
		// `on_shell_thread`, which stopped being a distinction when there stopped
		// being a second thread.
		bool host_stamped = false;
		// THE FIBER'S LANE (#202). Owned by `_lanes`, which only ever grows, so
		// this stays valid across every rebuild of the table above.
		reactor_lane* lane = nullptr;
	};
	std::vector<reactor_dispatch> _dispatch_table;
	const leshper::registry* _dispatch_built_from = nullptr;
	std::uint64_t _dispatch_generation = 0;
	std::string _dispatch_host_stamped_reactor;
	bool _dispatch_valid = false;
	// THE TOPICS AND THE INTERESTS SHARE ONE ARRAY (#209). A turn's poll set is
	// the fixed topics plus whatever descriptors `_awaits` names, so the array is
	// sized for both and `turn` fills as many of its slots as this turn needs.
	std::array<struct pollfd, static_cast<std::size_t>(topic::count_) + kMaxAwaits>
		_poll{};

	// One armed timer, WHOLE (#168). The declaration - the action's name and the
	// interval - used to live in the registry and the due instant here, so arming
	// was the loop diffing two tables on every turn and neither side owned a
	// timer. The host owns them now: an `arm_timer` effect puts one here, a
	// `disarm_timer` takes it away, and the registry keeps nothing but the id.
	//
	// The action is the registry's INTERNED HANDLE and not its name, for the
	// reason the effect that delivered it carries one: nothing on the expiry path
	// allocates, and the driver has no use for the text except in a log line,
	// where it resolves it (`fire_timers`).
	struct timer_due {
		std::uint64_t id = 0;
		std::uint64_t interval_ms = 0;
		std::uint32_t action = 0;
		clock::time_point due{};
	};
	std::vector<timer_due> _timers;

	// The `watch` topic (#195). Borrowed, and -1 means the topic does not exist
	// this session - which is the ordinary state of every test that does not
	// attach one, and of a shell whose data directory would not give out a
	// notification descriptor.
	int _watch_fd = -1;
	void (*_watch_hook)(void*) = nullptr;
	void* _watch_userdata = nullptr;
	std::size_t _watch_drains = 0;

	unsigned _resizes_seen = 0;
	// PARKED, AND IT IS ONE BIT (#203). A depth counter until then; see `quiesce`
	// for why nothing nests. It is the terminal handoff's bookkeeping and nothing
	// else - the emitters group's own bit is the scheduler's, derived from
	// `_phase` - which is why `resume_after_execution` reads it three times and
	// each read is about the terminal, the size or the repaint.
	bool _quiesced = false;
	std::size_t _applied = 0;
	std::size_t _dropped = 0;
	std::size_t _timer_dispatches = 0;

	// THE SCHEDULER, AND THE FIBERS IN IT (#202). Declared after everything a
	// fiber's body reaches through `this`, and BEFORE nothing - a fiber never
	// outlives the loop, because `drain_emitters()` runs every one of them out to
	// its next poll before this member is destroyed.
	//
	// INSTANTIABLE AND A MEMBER, never a global (#198): a scheduler is per-thread
	// by construction, and the day history persistence gets a thread of its own
	// that thread builds its own.
	fiber::scheduler _sched;
	// One per reactor, keyed by name, APPEND-ONLY. Behind a `unique_ptr` because a
	// lane's address is what the fiber's userdata and the slot's parked-receiver
	// pointer both hold, and a vector that grew would move it.
	std::vector<std::unique_ptr<reactor_lane>> _lanes;
	// Where the session is. Two writers, named on `quiesce` and
	// `resume_after_execution`.
	phase _phase = phase::editing;

	// THE EXECUTION FIBER (#208), SPAWNED ON THE FIRST LINE AND NEVER AGAIN. Null
	// until then, and null for ever in `execution_mode::inline_`: a loop that
	// never accepts a line - which is most of `ui_loop_tests.cpp` - reserves no
	// stack at all.
	fiber::fiber* _execution = nullptr;
	// The two channels, and they are the only ones. `inbox` carries THE LINE
	// ITSELF and not a view of `_accepted` (#211 §2.3): a view into a member that
	// the next accept reassigns is safe only while `accept_current_line` cannot
	// re-enter, which is an unwritten rule `vared`'s nested read is going to
	// break. One move per accepted line, at human frequency, buys a message that
	// owns what it carries - which is ADR-0007's rule and what `slot` is for.
	// `done` carries the status back, and the host takes it with `try_recv`
	// because the host cannot park.
	fiber::slot<std::string> _exec_inbox{_sched};
	fiber::slot<std::int32_t> _exec_done{_sched};
	// WHO IS WAITING FOR WHAT. Pointers into the awaiting fibers' own frames - see
	// `awaiter` - in a fixed array, because both verbs are reached through a
	// `noexcept` interface with no constructor to reserve in. Nearly always empty,
	// which is the fast path: a turn with nothing outstanding builds the poll set
	// it always built and asks nobody anything.
	std::array<awaiter*, kMaxAwaits> _awaits{};
	std::size_t _await_count = 0;

	// THE POLL HAS FAILED AND A COMMAND IS STILL RUNNING (#211 §4.1). Set once,
	// never cleared: the turns that follow keep only the signal topic - the
	// SIGCHLD byte is what ends the foreground wait - and the loop leaves as soon
	// as the fiber has given the status back. See `turn` and `run_the_line`.
	bool _poll_failed = false;
	// Polls a test has asked to fail; see `fail_next_polls`.
	int _fail_polls = 0;

	bool _exiting = false;
	std::int32_t _exit_status = 0;
	bool _needs_render = false;
	// A PLAIN BOOL SINCE #201. It was an atomic because the shell thread set it
	// from inside `execute` and the loop thread read it; both of those are this
	// thread now.
	bool _stopping = false;
};

// THE RUNTIME'S HOST, AND THE WHOLE OF WHAT THE EXECUTOR LEARNS (#208).
//
// Five lines of body, and that is the shape the ticket asked for: the executor
// says `wait_child` because it has nothing to do until a child does something,
// this hands the question to the loop, and the loop hands the DECISION to
// `scheduler::block_or_park`. Nothing on this path branches on whether there is
// a fiber; exactly one place in the tree does.
//
// A SEPARATE CLASS RATHER THAN `event_loop : public cooperation`, because the
// direction of the dependency is the point: `src/runtime/` must be able to name
// this interface without naming a loop, a scheduler or a topic, and a loop that
// WAS a `cooperation` would put the runtime's vocabulary on the host's own
// public surface. It is also what keeps the seam swappable - a cord, a replay
// harness or a test can install a different one against the same `shell_state`.
//
// INSTALLED BY THE SESSION AND REMOVED BY IT (`session`'s constructor and
// destructor). `enter_subshell` puts the no-op back in every forked child, which
// is why a `( )`, a `$( )` and a non-exec pipeline stage all wait with a plain
// `::waitpid` on their own stack and never touch a scheduler that is not theirs.
class loop_cooperation final : public runtime::cooperation {
public:
	explicit loop_cooperation(event_loop& on) noexcept : _loop(&on) {}

	// STILL EMPTY, deliberately (ADR-0011 §Deferred: "reactors running during
	// execution (do not flip the bit)"). The boundary is the yield point the door
	// is priced against; opening it is a later ticket, and doing it here would
	// make this one about two things.
	void on_command_boundary() noexcept override {}

	pid_t wait_child(pid_t pid, int flags, int* status) noexcept override {
		return _loop->await_child(pid, flags, status);
	}

	// THE SECOND FIVE-LINE ADAPTER (#209), and it reads as the first one does on
	// purpose: the verb hands the question to the loop, and the loop hands the
	// DECISION to `scheduler::block_or_park`. Two verbs, one branch, one place.
	void await_readable(int fd) noexcept override { _loop->await_readable(fd); }

private:
	event_loop* _loop;
};

} // namespace lesh::ui
