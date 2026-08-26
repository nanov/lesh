#pragma once

// The event loop: `poll(2)`, five topics, and quiesce (#129; #128's resolution;
// architecture spec §4 and §4.1; ADR-0009).
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
// TOPICS ARE THE VOCABULARY (the owner's word). Five of them, and the fd is
// each one's implementation detail:
//
//   `tty`     bytes from the terminal, decoded by #111's `input_decoder`.
//   `signal`  a self-pipe. The handler saves errno, bumps a counter, writes one
//             byte, and does nothing else that could be unsafe.
//   `worker`  #126's completion queue. Answered with `drain()`, NEVER by
//             reading the fd - see below, it is the one rule that loses a
//             wakeup permanently when broken.
//   `timer`   no fd at all: the poll timeout is `min(deadlines) - now` on a
//             monotonic clock, and `lesh_timer_start` is the public door.
//   `shell`   ADR-0009's wakeup pipe from the shell thread, drained the same
//             way the worker topic is.
//
// §8's `fd-readable` configuration hook is one more topic when it arrives, and
// that is what "topic" buys over "a list of descriptors".
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
// TWO OWNER THREADS (ADR-0009). This loop is a SPAWNED thread. The shell is the
// main thread and owns `shell_state`; the loop owns editor state and the
// terminal while editing. It hosts neither the port nor the highlighter: an
// action's shell code is `shell.post(port_call) ; wait on the shell topic`, and
// the highlighter is submitted to the shell thread's `highlight` slot rather
// than to the helper pool. The helpers keep the state-free work - history
// search, the autosuggester, path checks.
//
// QUIESCE, AND WHY IT IS TWO LAYERS. `quiesce()` is helpers parked plus the
// terminal restored and given up; the fork then happens on the shell thread
// with the loop blocked in this same poll. Parking is the load-bearing layer
// because lesh forks to RUN SHELL CODE - subshells, `&`, non-external pipeline
// stages - which allocates in the child at once, and a child born beside a
// thread holding malloc's lock deadlocks on its first allocation. fish does not
// park and does not need to: every fish fork execs. The exec lanes take fish's
// async-signal-safe discipline anyway, and that half lives in
// substrate/fork_guard.h.
//
// ALLOCATION (N-2, and tests/unit/allocation_tests.cpp is the gate). Everything
// a turn touches is a member that keeps its capacity: the read buffer, the
// event vector, the completion vector, the shell inbox, the blitter's output
// string, the pollfd array. A warm turn allocates nothing.

#include "leshper/blit.h"
#include "leshper/decode.h"
#include "leshper/editor.h"
#include "leshper/layout.h"
#include "leshper/registry.h"
#include "leshper/shell_actor.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "leshper/tty.h"
#include "leshper/workers.h"
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
#include <thread>
#include <vector>

namespace lesh::leshper {

// ---------------------------------------------------------------------------
// The signal topic.
// ---------------------------------------------------------------------------

// Signal numbers this tracks. 32 spans every named signal on macOS and every
// non-real-time signal on Linux, which is every signal a shell's editor has a
// disposition for. It sizes an array a HANDLER writes, so it must be a
// compile-time constant and can never become a container (`runtime/signals.h`
// carries the same rule for the same reason).
inline constexpr int kMaxTrackedSignal = 32;

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
	//   SIGHUP   caught ONLY IF currently SIG_DFL, which is how `nohup` is
	//            respected rather than overridden.
	//   SIGPIPE, SIGQUIT, SIGTSTP, SIGTTOU, SIGTTIN ignored. fish's words on the
	//            last three: "We are a shell, we know what is best for the user."
	//
	// Answers false if any `sigaction` failed. `uninstall()` puts back exactly
	// what was saved, which is what lets a test install and restore without
	// disturbing the rest of the binary (`tests/unit/interactive_signal_guard.h`
	// is the same discipline for the shell's own dispositions).
	bool install() noexcept;
	void uninstall() noexcept;
	[[nodiscard]] bool installed() const noexcept { return _installed; }

	// Takes the dispositions again, keeping what was saved the FIRST time.
	//
	// #134's resolution of the ownership question #129 returned. The shell's own
	// `trap` machinery (`runtime/signals.cpp`) installs a handler of its own the
	// moment a user types `trap ... INT` at the prompt, and that `sigaction`
	// silently replaces ours - after which Ctrl-C sets `g_pending` and rings no
	// pipe, so the editor never hears it. The wiring site therefore re-asserts at
	// every read entry and after every command, which is where a `trap` can have
	// run.
	//
	// The SAVE IS ONCE PER SIGNAL, deliberately: what we chain to has to be the
	// handler that was there before leshper existed, and re-saving would either
	// record our own handler (an infinite chain) or record whatever the shell
	// installed a moment ago and lose the original for `uninstall` to put back.
	bool reassert() noexcept;

	// THE HANDLER'S WHOLE BODY, async-signal-safe, exposed so a test can deliver
	// a signal to this hub without one being raised at the process.
	//
	// Public because a free-function handler must reach it, and named for what
	// it is: a signal has been delivered to this hub.
	//
	// AND THEN IT CHAINS (#134). After the self-pipe work - and only after, so a
	// previous handler that never returns cannot cost us the wakeup - the
	// handler saved for this signal is called, when one was saved and it is a
	// real function rather than SIG_DFL or SIG_IGN. That is what keeps the
	// shell's `g_pending` being set while the editor owns the dispositions, so a
	// user's `trap INT` still fires (#98 decision 3, the zsh way) - during
	// editing AND during a command, when the shell thread is running the command
	// and draining no slots at all.
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
	// The body `install` and `reassert` share: catch, ignore, and save-once.
	bool take_dispositions() noexcept;

	int _read_fd = -1;
	int _write_fd = -1;
	bool _installed = false;

	// Written by the handler, read by the loop. `volatile sig_atomic_t` is what
	// the C standard permits a handler to touch, and the array is fixed for the
	// same reason `runtime/signals.h`'s is.
	volatile sig_atomic_t _pending[kMaxTrackedSignal]{};
	volatile sig_atomic_t _resizes = 0;

	// What was installed before us, put back by `uninstall`.
	struct sigaction _saved[kMaxTrackedSignal]{};
	bool _saved_valid[kMaxTrackedSignal]{};
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
	shell,
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

	std::chrono::milliseconds escape_timeout = default_escape_timeout;
	terminal_capabilities capabilities = terminal_capabilities::floor();
	grapheme::width_policy width{};
	style prompt_pen{};
	style text_pen{};

	// The reactor routed to the SHELL thread rather than to the helper pool
	// (ADR-0009: the highlighter reads shell state, and shell state has one
	// owner). A name rather than an ABI flag, because #93's registration tuple
	// is fixed and adding a bit to it for one built-in would be the side door
	// the whole registry design exists to prevent.
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

	// THE EDITOR STATE, owned here. ADR-0009: the loop thread owns it, and
	// nothing else may touch it while the loop is running.
	[[nodiscard]] state& editor() noexcept { return _state; }
	[[nodiscard]] const state& editor() const noexcept { return _state; }

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
	void attach_registry(registry& reg) noexcept;
	void attach_shell(shell_actor& shell) noexcept;
	void attach_signals(signal_hub& hub) noexcept;

	// What the shell knows (#135), put on the snapshot of every reactor that
	// runs ON THE SHELL THREAD and on no other.
	//
	// THE RESTRICTION IS THE POINT. `shell_knowledge` is a window into
	// `shell_state`, whose one owner is the shell thread (ADR-0009); a helper
	// reading through it would be reading tables another thread may be writing.
	// The shell-thread reactor is the only one for which the pointer is safe, so
	// it is the only one that gets it, and a state-free reactor keeps the null
	// that means "no shell attached". Never owned; must outlive this loop.
	void attach_shell_knowledge(const shell_knowledge* knowledge) noexcept;

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

	// Turns until the editor exits or `stop()` is called.
	void run();

	// --- The thread (#134 owns the sequencing; these are its two calls) ------

	// Spawns the loop thread and returns at once.
	void start();
	// Asks the loop to leave and joins it. Idempotent, and safe to call from any
	// thread: it sets a flag and rings the signal topic's pipe, which is the one
	// wakeup that always exists.
	void stop();
	// The same ask WITHOUT the join, for the one caller that must not join: the
	// shell thread, inside `execute`, having just run an `exit`. The loop is
	// blocked waiting for that execution's reply, so joining it from there would
	// be waiting for a thread that is waiting for us.
	void request_stop() noexcept;
	[[nodiscard]] bool running() const noexcept;

	// --- Quiesce (#91, #128, ADR-0009) --------------------------------------

	// Helpers parked, the terminal restored and given up, the decoder's held
	// bytes dropped. After this returns, a fork on the shell thread is legal.
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

	// The whole accept path: park, restore the terminal, post `execute`, block
	// on the `shell` and `signal` topics, then reclaim, re-assert, resume and
	// redraw with a fresh line.
	//
	// Answers the status the shell reported, or nothing when no shell is
	// attached - in which case the line is still finished and cleared, which is
	// what the editor-only tests exercise.
	std::optional<std::int32_t> accept_current_line();

	// The other half of `cancel_line`: the shell sets `$?` = 130 and runs the
	// user's INT trap (#98 decision 3). Posted through the `execute` slot as an
	// empty line, on the same park-restore-post path an accepted line takes,
	// because a trap body may fork. A no-op with no shell attached.
	void finish_cancelled_line();

	// #92's port, from the loop's side: fill the `port_call` slot and block on
	// the `shell` and `signal` topics until it answers. The action sees a
	// synchronous call; the terminal keeps the EDITOR's modes throughout (fish
	// #7770 - an action's shell code never gets ECHO back).
	port_result call_port(std::string_view code);

	// The enumeration read (#139, spec 6.9), from the loop's side: fill the
	// `enumerate` slot and block on the same two topics until the copy comes
	// back. Appends to `into`; answers false when there is no shell attached, or
	// when the wait ended without the reply (a stop while a Tab was in flight).
	//
	// THE SAME ROUND TRIP `call_port` MAKES, and reusing it rather than inventing
	// a mechanism is the point: ADR-0009 already has exactly one way for the loop
	// to ask the shell a question and wait for the answer, and a Tab is that
	// question with a different payload. What it does NOT reuse is the port
	// itself - a port call runs shell CODE, and running code to read a table
	// would put an arbitrary side effect on the completion path.
	bool read_names(name_domain which, std::vector<std::string>& into);

	// --- Rendering -----------------------------------------------------------

	// Lays out and blits. `previous` is kept here, and a full repaint is what
	// happens when the sizes disagree or the screen's contents are unknown.
	void render();
	// Forgets `previous`, so the next render repaints everything.
	void invalidate() noexcept { _have_previous = false; }
	[[nodiscard]] const layout& last_layout() const noexcept { return _previous; }
	// The bytes the last render wrote - what a test asserts instead of a screen.
	[[nodiscard]] std::string_view last_output() const noexcept { return _out; }

	// --- What the loop learned -----------------------------------------------

	// The latest batch each reactor emitted, applied under the generation drop
	// rule. Stands in for `state::decorations`, which is still #93's placeholder
	// with no fields; when it gains a type, the application lands here and this
	// accessor goes with it.
	[[nodiscard]] const std::vector<reactor_batch>& decorations() const noexcept {
		return _decorations;
	}
	[[nodiscard]] std::size_t applied_batches() const noexcept { return _applied; }
	// Batches dropped because their generation had moved on (N-4, counted).
	[[nodiscard]] std::size_t dropped_batches() const noexcept { return _dropped; }
	[[nodiscard]] std::size_t timer_dispatches() const noexcept { return _timer_dispatches; }
	[[nodiscard]] bool exiting() const noexcept { return _exiting; }
	[[nodiscard]] std::int32_t exit_status() const noexcept { return _exit_status; }

private:
	using clock = std::chrono::steady_clock;

	void drain_tty(input_instant now, turn_result& result);
	void drain_signal_topic(turn_result& result);
	void drain_worker_topic(turn_result& result);
	void drain_shell_topic(turn_result& result);
	void fire_timers(input_instant now, turn_result& result);

	void handle(const event& incoming, turn_result& result);
	void carry_out(const effects& produced, turn_result& result);
	void notify_reactors(std::uint32_t kinds);
	void take_batch(reactor_batch& answer);
	void apply_outcome(const action_result& what, turn_result& result);
	// Blocks in poll on the `shell` and `signal` topics only, until a message of
	// `until` (and, for a port call, of `sequence`) arrives.
	// `names`, when given, receives the `enumerate_done` payload - the one reply
	// kind that carries more than a status. Defaulted, so the two existing call
	// sites are untouched.
	std::optional<std::int32_t> wait_on_shell(shell_message::kind until, std::uint64_t sequence,
	                                          std::vector<std::string>* names = nullptr);
	void handle_shell_message(shell_message& answer);
	void refresh_size_from_terminal();

	loop_fds _fds;
	loop_options _options;

	state _state;
	terminal _terminal;
	input_decoder _decoder;
	cluster_pool _pool;
	blitter _blitter;
	layout _previous;
	bool _have_previous = false;

	worker_pool* _helpers = nullptr;
	registry* _registry = nullptr;
	shell_actor* _shell = nullptr;
	signal_hub* _signals = nullptr;
	const shell_knowledge* _knowledge = nullptr;
	// Minted when a registry is attached: the loop's own dispatch of an action
	// by name, which is what a timer expiry needs.
	std::optional<loop_harness> _dispatch;
	// Set when this loop constructed its own hub, which is the ordinary case;
	// an attached one belongs to the caller.
	std::optional<signal_hub> _own_signals;

	// --- Reused storage. Every one of these is why a warm turn allocates. ---
	std::string _read_buffer;
	std::vector<event> _events;
	std::vector<event> _deferred;      // signals that arrived while executing
	std::vector<int> _signal_numbers;
	std::vector<completion> _completions;
	std::vector<shell_message> _inbox;
	std::vector<reactor_batch> _decorations;
	std::string _out;
	std::string _accepted;
	std::array<struct pollfd, static_cast<std::size_t>(topic::count_)> _poll{};

	// One armed timer's next due instant, kept beside the registry's table
	// because the registry knows no clock.
	struct timer_due {
		std::uint64_t id = 0;
		clock::time_point due{};
	};
	std::vector<timer_due> _timers;

	unsigned _resizes_seen = 0;
	std::size_t _park_depth = 0;
	std::size_t _applied = 0;
	std::size_t _dropped = 0;
	std::size_t _timer_dispatches = 0;

	bool _exiting = false;
	std::int32_t _exit_status = 0;
	bool _needs_render = false;
	std::atomic<bool> _stopping{false};
	std::thread _thread;
};

} // namespace lesh::leshper
