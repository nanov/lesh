#pragma once

// The event loop: `poll(2)`, five topics, and quiesce (#129; #128's resolution;
// architecture spec §4 and §4.1; ADR-0009 as amended by #201).
//
// THE HOST'S, AND IN `src/ui/` SINCE #168. This file drives leshper; it is not
// part of it. The editor is `step(state, event, now) -> effects` and knows no
// thread, fd, poll, timer or mailbox; everything on this side of that sentence -
// the loop, the tty, the workers, the timers, the shell handoff - is the host,
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
// TOPICS ARE THE VOCABULARY (the owner's word). Five of them since #201 - the
// `shell` topic went with the thread it was a wakeup from - and the fd is each
// one's implementation detail:
//
//   `tty`     bytes from the terminal, decoded by #111's `input_decoder`.
//   `signal`  a self-pipe. The handler saves errno, bumps a counter, writes one
//             byte, and does nothing else that could be unsafe.
//   `worker`  #126's completion queue. Answered with `drain()`, NEVER by
//             reading the fd - see below, it is the one rule that loses a
//             wakeup permanently when broken.
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
// two members and three lines in `turn`, `shell` left the same way, and nothing
// that reasons about the tty or the workers had to learn about either.
//
// A TURN: poll -> drain the topics into events -> `editor.step` each -> if
// anything changed, `lay_out` -> `blit.update(previous, desired)` -> write. The
// loop keeps the previous surface, because #112's blitter deliberately keeps no
// "actual screen" of its own that could drift out of step with the terminal.
//
// THE CORE IS A FUNCTION OVER FDS PASSED IN. Nothing here opens `/dev/tty` or
// assumes `STDIN_FILENO`: the tests drive it over pipes and `openpty` pairs,
// never over the process's own terminal, and the N-3 replay file goes through
// the identical path. That is the test contract #128 wrote, and it is the
// reason the terminal rules live in tty.h where they can be exercised alone.
//
// ONE THREAD (#201, amending ADR-0009). This loop RUNS ON MAIN. `run()` is what
// `ui/session.cpp` calls, and everything ADR-0009 split across two owner threads
// is now serialized by being one: the loop owns editor state and the terminal,
// the shell owns `shell_state`, and the loop reaches the shell by CALLING
// `shell_side::execute` and `shell_side::port_call` where it used to fill a slot
// and block on a wakeup pipe. The highlighter runs in place, on this thread,
// which is the thread that owns the tables it reads. The helpers keep the
// state-free work - history search, the autosuggester, path checks - and are the
// only other threads in the process.
//
// ADR-0009 SEPARATED THEM ONLY BECAUSE WORKER THREADS READ SHELL STATE, and it
// stopped being true when #151 made the shell-thread reactor the one reader:
// with the reads serialized on the owning thread there is nothing left for a
// second thread to protect, and the condvar, the three slots, the reply pool and
// the `shell` topic were the whole cost of having one. `shell_writing_flag` stays
// as the assertion that says so - trivially true now, and still the tripwire if
// a future thread ever wants a read.
//
// QUIESCE, AND WHY IT IS TWO LAYERS. `quiesce()` is helpers parked plus the
// terminal restored and given up; the fork then happens inside the `execute`
// this loop calls, on this thread. Parking is the load-bearing layer
// because lesh forks to RUN SHELL CODE - subshells, `&`, non-external pipeline
// stages - which allocates in the child at once, and a child born beside a
// thread holding malloc's lock deadlocks on its first allocation. fish does not
// park and does not need to: every fish fork execs. The exec lanes take fish's
// async-signal-safe discipline anyway, and that half lives in
// substrate/fork_guard.h.
//
// ALLOCATION (N-2, and tests/unit/allocation_tests.cpp is the gate). Everything
// a turn touches is a member that keeps its capacity: the read buffer, the
// event vector, the completion vector, the shell reactor's snapshot and batch,
// the blitter's output string, the pollfd array. A warm turn allocates nothing.

#include "leshper/blit.h"
#include "leshper/decode.h"
#include "leshper/editor.h"
#include "leshper/layout.h"
#include "leshper/host.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "ui/reactor_call.h"
#include "ui/shell_knowledge.h"
#include "ui/shell_side.h"
#include "ui/tty.h"
#include "ui/workers.h"
#include "substrate/grapheme.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
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

// Blocks the hub's caught set - SIGINT, SIGCHLD, SIGWINCH - on the CALLING
// thread, and answers false if `pthread_sigmask` refused.
//
// DELIVERY IS PINNED TO MAIN, and this is the whole mechanism (#142). A
// process-directed signal is delivered to any one thread that does not block it,
// so before this existed a Ctrl-C could land on a helper thread, on the loop
// thread, or on main, chosen by the kernel afresh each time. Every thread the
// host spawns calls this first thing in its body; main does not, so main is the
// one thread left unblocked and the handler runs there - which is where it ran
// before leshper existed at all.
//
// THE INVARIANT, and it is the reason the block goes on the spawned threads
// rather than on main: A SIGNAL MASK SURVIVES `execve`. A thread that forks and
// execs must therefore stay unmasked, or every child inherits a shell's mask and
// a `kill -INT` on a pipeline does nothing. Main is the only thread that forks,
// so the invariant holds by construction - but a future thread that spawns
// children must not call this.
//
// THE CALLERS ARE THE HELPER POOL'S, AND ONLY THE POOL'S (#201). The loop thread
// used to call this first thing in its body; there is no loop thread now - the
// loop runs on main - and the block was DELETED rather than moved, because main
// forks. Main pays for the mask it does not take with an EINTR out of `poll`,
// which `turn` has always handled: the wakeup it acts on is the self-pipe byte,
// never the EINTR.
bool block_caught_signals_on_this_thread() noexcept;

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
	// `uninstall` all `sigaction`, which is process-wide state; the trap builtin
	// writes the same state; and `block_caught_signals_on_this_thread` leaves main
	// as the only thread a signal is delivered to. The writer, the other writer
	// and the handler are one thread - and the loop is on it too now, which is why
	// what the LOOP does with the hub is still only READING it: `drain`,
	// `resize_count`, and the byte in the pipe (`TheLoopNeverWritesADisposition`).
	//
	// The chain slots are atomic anyway (see `_chain`): a `sigprocmask` fence
	// would have been no fence at all, because it masks the calling thread and
	// says nothing about delivery elsewhere, and one lock-free pointer store
	// costs nothing and stays correct if a thread is ever left unmasked.
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
	// THIS RUNS ON THE SHELL THREAD in a real session, not on the loop's - the
	// spawned threads block the caught set, so main is the only thread a
	// process-directed signal reaches. The loop hears about it as a byte.
	//
	// A previous handler installed with SA_SIGINFO is NOT chained: this entry
	// point has only a signal number, and inventing a `siginfo_t` to pass it
	// would be worse than saying so. Nothing lesh installs uses SA_SIGINFO, and
	// the sanitizers' handlers are for signals this hub never takes.
	void deliver(int signo) noexcept;

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
	worker,
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

	// The reactor that READS SHELL STATE, and is therefore run in place on the
	// thread that owns it rather than submitted to the helper pool (ADR-0009's
	// keystone; #201 made "the shell thread" and "this thread" the same thread and
	// left the name alone). A name rather than an ABI flag, because #93's
	// registration tuple is fixed and adding a bit to it for one built-in would be
	// the side door the whole registry design exists to prevent.
	std::string shell_thread_reactor = "highlighter";

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

	// The paste rule (#128's trap 4, fish's `read_normal_chars`): keep reading
	// while a zero-timeout poll says the fd is still readable, so a paste is one
	// edit and one repaint while a typed character paints now. The cap keeps a
	// process piping a gigabyte into the editor from being read in one turn.
	std::size_t readahead_limit = 64 * 1024;
};

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

	void attach_helpers(worker_pool& pool) noexcept;
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
	// `writing` is ADR-0009's tripwire, raised around the two calls. Null is
	// "unchecked", which is what a test with no adapter to protect wants.
	//
	// None of the three is owned and all must outlive the loop.
	void attach_shell(shell_side& shell, const leshper::host* host = nullptr,
	                  shell_writing_flag* writing = nullptr) noexcept;

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
	// `on_readable` RUNS ON THE LOOP THREAD, inside the turn, and is responsible
	// for CONSUMING what made the fd readable - the same rule the worker topic
	// has, and for the same reason: a hook that leaves its descriptor readable
	// turns the poll into a spin. It must not block; the history's drain is a
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

	// --- Quiesce (#91, #128, ADR-0009) --------------------------------------

	// Helpers parked, the terminal restored and given up, the decoder's held
	// bytes dropped. After this returns, a fork on this thread is legal.
	//
	// NESTS, because `worker_pool::park_all` does: two calls need two resumes.
	void quiesce();

	// The other end: the terminal reclaimed, the editor's modes re-asserted, the
	// helpers resumed, and the next render a full repaint - the screen is
	// whatever the command left, so there is no `previous` to diff against.
	void resume_after_execution();

	// The debug assertion every fork-and-continue site carries (#91), asked of
	// the loop rather than only of the pool, because the terminal half of
	// quiesce is the loop's.
	void assert_quiesced() const noexcept;
	[[nodiscard]] bool quiesced() const noexcept { return _park_depth > 0; }

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

	// THERE IS NO `read_names` (#151). #139 gave the completer a round trip on
	// the actor's `enumerate` slot; the owner's reading of ADR-0009 removed the
	// need for one. The loop may read shell state directly while nothing
	// executes, and while the loop is running an action nothing CAN execute:
	// `execute` and `port_call` are the only writers and the loop is what CALLS
	// them, so an action dispatched by the loop is by construction not inside
	// either. So the completer holds a `const shell_knowledge*` and calls
	// `enumerate` on it, on this thread, and `shell_writing_flag` asserts the
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
	[[nodiscard]] bool exiting() const noexcept { return _exiting; }
	[[nodiscard]] std::int32_t exit_status() const noexcept { return _exit_status; }

private:
	using clock = std::chrono::steady_clock;

	void drain_tty(leshper::input_instant now, turn_result& result);
	void drain_signal_topic(turn_result& result);
	void drain_worker_topic(turn_result& result);
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
	void notify_reactors(std::uint32_t kinds);
	// Rebuilds `_dispatch_table` from the registry's reactor map. Called only
	// when the cheap staleness check in `notify_reactors` says the copy is out of
	// date; the steady state never reaches it.
	void refresh_dispatch_table();
	void take_batch(leshper::reactor_batch& answer);
	// The shell-state reactor, run HERE (#201). See the definition: a temporary
	// shape, replaced by a fiber in the next step of #145.
	void run_shell_reactor_here(std::string_view reactor, lesh_reactor_fn fn,
	                            void* userdata, std::uint32_t kinds);
	void refresh_size_from_terminal();

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

	worker_pool* _helpers = nullptr;
	leshper::registry* _registry = nullptr;
	shell_side* _shell = nullptr;
	// The executing shell's own door, stamped on every token the shell reactor is
	// given, and the flag raised around the two calls that write shell state.
	// Both arrive with the shell at `attach_shell`.
	const leshper::host* _shell_host = nullptr;
	shell_writing_flag* _writing = nullptr;
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
	// onto `_events` while the walk holds a reference into it - the shell
	// reactor's `worker_result` is one, and a shell message drained inside a
	// blocked wait was the original - which reallocates the vector out from
	// under the walk. Swapping first means the push lands in an empty `_events`
	// and is walked by the next pass instead. A member, so the capacity survives
	// the turn and the steady state allocates nothing.
	std::vector<leshper::event> _carried_events;
	std::vector<int> _signal_numbers;
	std::vector<completion> _completions;
	std::string _out;
	std::string _accepted;
	leshper::effects _carried;         // what the registry queued, taken per turn

	// THE REACTOR TABLE, FLATTENED (the reorg cleanup). `notify_reactors` used to
	// walk `registry::reactors` - a red-black tree - and string-compare
	// `shell_thread_reactor` against every key, once per reactor per keystroke.
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
		bool on_shell_thread = false;
	};
	std::vector<reactor_dispatch> _dispatch_table;
	const leshper::registry* _dispatch_built_from = nullptr;
	std::uint64_t _dispatch_generation = 0;
	std::string _dispatch_shell_reactor;
	bool _dispatch_valid = false;
	std::array<struct pollfd, static_cast<std::size_t>(topic::count_)> _poll{};

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
	std::size_t _park_depth = 0;
	std::size_t _applied = 0;
	std::size_t _dropped = 0;
	std::size_t _timer_dispatches = 0;

	// WHAT THE SHELL-STATE REACTOR IS RUN OUT OF (#201), and members for the
	// reason the actor's three slots were: the snapshot's buffer and the batch's
	// vectors keep their capacity, so the highlight every keystroke asks for
	// allocates nothing once the session is warm.
	// `AllocationTest.AWarmShellReactorRoundCostsNoHeap` is the pin.
	request_snapshot _shell_snapshot;
	leshper::reactor_batch _shell_batch;
	// The cooperative cancellation the highlighter polls, AND IT IS NEVER SET.
	// A reactor run in place cannot be superseded: the call returns before the
	// next keystroke can be read, so the overwrite that used to be the
	// cancellation has nothing to cancel. It exists because the token requires a
	// flag, and it is the one thing here the fiber step gives a meaning back to.
	std::atomic<bool> _shell_superseded{false};

	bool _exiting = false;
	std::int32_t _exit_status = 0;
	bool _needs_render = false;
	// A PLAIN BOOL SINCE #201. It was an atomic because the shell thread set it
	// from inside `execute` and the loop thread read it; both of those are this
	// thread now.
	bool _stopping = false;
};

} // namespace lesh::ui
