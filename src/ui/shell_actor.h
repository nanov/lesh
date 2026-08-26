#pragma once

// The shell thread's side of the two-owner split (ADR-0009, #136, #129).
//
// THE HOST'S, AND IN `src/ui/` SINCE #168. The handoff between a shell and an
// editor is the binding layer's whole job, so this is the one of the four driver
// files that was always going to end up here.
//
// THE ARRANGEMENT, in one paragraph. The shell is the main thread and owns
// `shell_state`; the host's loop is a spawned thread and owns editor state and
// the terminal while editing. The shell reaches the loop through one wakeup
// pipe - the loop's fifth topic - and the loop reaches the shell by filling one
// of three LATEST-WINS SLOTS, checked in priority order: `execute`, `port_call`
// (an action's shell code, #92), `highlight`. Each slot holds at most one item;
// a newer highlight overwrites a pending one, and that overwrite IS the
// cancellation. The shell thread waits on a condition variable, because while a
// command line is being edited it has no descriptors to watch.
//
// THERE WAS A FOURTH (#139's `enumerate`, the completer's name list) AND #151
// DELETED IT. A read that changes nothing does not need a slot, a sequence
// number and a blocked loop: ADR-0009 already says the loop may read shell
// state directly while nothing executes, and nothing can execute while the loop
// is the thing that would have to request it. The completer now calls
// `shell_knowledge::enumerate` on the loop thread; `shell_writing_flag` is the
// tripwire that keeps "while nothing executes" honest.
//
// THIS FILE IS BOTH HALVES OF THE SEAM, and they are one ticket because they
// are one protocol: `shell_actor` is what runs on the shell thread, and
// `shell_channel` is the loop's `shell` topic - a pipe read end and a queue,
// with the same level-triggered, lossy contract #126 wrote for the worker
// topic. Splitting them across two tickets would have meant agreeing on a wire
// format in prose.
//
// WHY NO VERSIONS. ADR-0009's keystone: on the shell thread everything is
// serialized. A port call that writes shell state, a highlight that reads it
// and an execution never overlap, because there is no second thread that can
// touch shell state. So the highlighter reads the alias, function and builtin
// tables directly and #130's copy-on-write definitions version is deleted. The
// only version left is the EDITOR'S GENERATION, which rides on every message in
// both directions; the loop drops what does not match, which is the request
// token's existing rule pointed the other way.
//
// WHAT THE SHELL SIDE PROVIDES is `shell_side` below - the A-5 interface the
// host declares and `src/ui/session.cpp` implements over the real `shell_state`.
// It stays an interface now that both halves are in `lesh_ui`, and the reason is
// the same one it was written for: the loop must be drivable with no shell
// behind it at all, which is what every test in `ui_loop_tests.cpp` does. Tests
// fake it in twelve lines.
//
// WHAT THE SHELL KNOWS IS THE ACTOR'S, NOT THE LOOP'S (#151). The actor is
// constructed with the session's `shell_knowledge*` and stamps it on EVERY token
// it services, so the executing shell's tables reach the highlighter by
// construction. Before this the LOOP put the pointer on each snapshot - the loop
// telling the shell where the shell's own state is - and the token this file
// builds from that snapshot copied every field except that one, so `exit`,
// `bind`, aliases and functions all painted `command.unknown`. A field that has
// to be copied on the far side is a field that can be dropped there; a member
// read at the point of use cannot.
//
// ALLOCATION. Messages are recycled: `drain` hands the loop a batch of them and
// `recycle` hands them back with their vectors cleared but their capacity
// intact, which is #126's `message_pool` idea with the ownership made local -
// so a keystroke's highlight round trip allocates nothing once the session is
// warm. The three slots are members and their strings keep their capacity for
// the same reason.

#include "leshper/abi.h"
#include "leshper/registry.h"
#include "ui/shell_knowledge.h"
#include "leshper/state.h"
#include "ui/workers.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::ui {

// ---------------------------------------------------------------------------
// What comes back.
// ---------------------------------------------------------------------------

// One answer from the shell thread.
//
// THREE KINDS AND NOT A VARIANT, because the loop switches on `which` once and
// the payload fields are cheap to carry unused - and because a `reactor_batch`
// inside a variant would be moved between alternatives on every recycle, which
// is exactly the vector capacity the recycling exists to keep.
struct shell_message {
	enum class kind : std::uint8_t {
		// A reactor the shell thread ran - the highlighter, and in v1 only it.
		// `batch` is the answer, subject to the generation drop rule.
		highlight_done,
		// #92's port: the action's shell code has run. The action is blocked in
		// the loop's `call_port`, which matches on `sequence`.
		port_call_done,
		// An accepted line has finished executing. The loop reclaims the
		// terminal, re-asserts its modes, resumes the helpers and redraws.
		execute_done,
	};

	kind which = kind::highlight_done;

	// The generation the work was computed against. The loop DROPS a message
	// whose generation is not the one the editor is at - ADR-0009's one version,
	// and the request token's rule pointed from the shell back at the loop.
	//
	// `execute_done` carries the generation the line was accepted at, which is
	// necessarily stale by the time it lands; the loop does not apply it to
	// anything, so the drop rule is not consulted for that kind.
	leshper::generation computed_against;

	// The status the shell reported: an exit status for `execute_done` and
	// `port_call_done`, a LESH_* status for `highlight_done`.
	std::int32_t status = LESH_OK;

	// Which port call this answers. Zero for the other two kinds.
	std::uint64_t sequence = 0;

	// `highlight_done` only.
	leshper::reactor_batch batch;
};

// The loop's `shell` topic: a pipe read end and a queue behind one mutex.
//
// THE LEVEL-TRIGGERED, LOSSY CONTRACT, identical to #126's worker topic and for
// the same reason: the fd is armed on the empty-to-non-empty transition and
// disarmed by `drain`, which consumes the byte AND empties the queue under one
// lock. Answering a readable fd by reading it without draining loses the wakeup
// permanently - the queue stays armed, so no further byte is ever written.
// `drain()` is therefore the loop's whole obligation, and there is no public
// way to read the descriptor.
//
// POLLHUP with POLLIN, as always for a pipe: "If a pipe is widowed with no
// data, Linux sets POLLHUP but not POLLIN, so test for both" (fish `fds.cpp`).
class shell_channel {
public:
	shell_channel();
	~shell_channel();

	shell_channel(const shell_channel&) = delete;
	shell_channel& operator=(const shell_channel&) = delete;

	// The fd the loop's `shell` topic polls for readability. -1 only if the pipe
	// could not be created, which is asserted at construction.
	[[nodiscard]] int wakeup_fd() const noexcept { return _read_fd; }

	// From the shell thread: a recycled message, or a fresh one when the spare
	// list is empty. Filled in and handed back to `post`.
	[[nodiscard]] shell_message acquire();

	// From the shell thread.
	void post(shell_message&& answer);

	// From the loop thread. Appends everything queued to `out`, consumes the
	// wakeup byte, and answers how many arrived.
	std::size_t drain(std::vector<shell_message>& out);

	// From the loop thread, once it has finished with what `drain` gave it.
	// Empties `used` and keeps the storage for the next round trip.
	void recycle(std::vector<shell_message>& used);

	[[nodiscard]] bool empty() const;
	[[nodiscard]] std::size_t size() const;
	// Whether a wakeup byte is outstanding - exposed so the arming rule can be
	// asserted directly rather than only through a poll that happens to agree.
	[[nodiscard]] bool armed() const;

private:
	mutable std::mutex _mutex;
	std::vector<shell_message> _queue;
	std::vector<shell_message> _spare;
	int _read_fd = -1;
	int _write_fd = -1;
	bool _armed = false;
};

// ---------------------------------------------------------------------------
// A-5: what the editing session needs from the shell, and nothing more.
// ---------------------------------------------------------------------------

// The interface the host DECLARES and `ui/session.cpp` implements (#134), the
// same shape `history_source` (#125) has. The reason is no longer a link rule -
// `lesh_ui` links `lesh_runtime` - but the one that outlived it: the actor and
// the loop must be drivable with no shell behind them, which is what every test
// in `ui_loop_tests.cpp` and `ui_knowledge_tests.cpp` does, and a direct call
// into `shell_state` would make a shell a prerequisite for editing at all.
//
// TWO METHODS, and their narrowness is the decision. Everything else the shell
// knows reaches leshper through `shell_knowledge` - stamped on the token for a
// reactor, read directly by the completer - and never through a call. What is
// here is only what has to BE a call, and the rule that decides it is that both
// of these RUN CODE: they change the world, and the world is the shell's. #139
// added a third for the completer's name list and #151 took it away again, once
// the owner's reading of ADR-0009 made the direct read legal - which is the rule
// holding rather than being bent.
//
// Both run ON THE SHELL THREAD, synchronously, with nothing else of the
// shell's running. Neither may touch the terminal: the loop has restored the
// modes and given up the foreground group before `execute` is posted, and an
// action's `port_call` runs with the EDITOR'S modes still in force - fish #7770,
// never re-enable ECHO around an action's shell code.
class shell_side {
public:
	virtual ~shell_side() = default;

	// Runs an accepted command line to completion. Answers its exit status.
	//
	// The fork happens inside here, on this thread, which is the main thread -
	// and it is legal because the loop has already parked the helpers and is
	// blocked in its poll. The child claims the terminal itself between fork and
	// exec, so nothing here needs the loop's cooperation on process groups.
	virtual std::int32_t execute(std::string_view line) = 0;

	// #92's port: runs an action's shell code. Answers its status.
	//
	// Lane discipline is the implementation's, not this interface's: builtins
	// and functions in-process, externals through spawn, fork-requiring forms
	// refused loudly. What this seam fixes is only that the call is synchronous
	// from the action's point of view, which is #92's contract unchanged.
	virtual std::int32_t port_call(std::string_view code) = 0;
};

// ---------------------------------------------------------------------------
// The actor.
// ---------------------------------------------------------------------------

// The shell thread's three latest-wins slots and its condition-variable loop.
//
// THREADING, and it is not advisory. `post_*` and `stop` are the LOOP THREAD's;
// `run` and `serve_one` are the SHELL THREAD's. The channel carries its own
// mutex; nothing here is called while holding it.
class shell_actor {
public:
	// `host` is the door to what the shell KNOWS (`leshper::host`, #168 Phase B;
	// it was a `shell_knowledge*` here), and it is required rather than defaulted
	// for one reason (#151): the field it fills used to arrive on each snapshot
	// from the loop, and the token this file builds forgot to copy it for a whole
	// wave. A parameter with no default cannot be forgotten. Null is still legal
	// and still means "no host attached" - every name classifies as
	// LESH_COMMAND_UNKNOWN - but it now has to be WRITTEN, which is a different
	// act from omitting an assignment on a struct.
	//
	// `writing` is ADR-0009's tripwire, raised around `execute` and `port_call`.
	// Null is "unchecked", which is what a test with no adapter to protect wants.
	// Neither pointer is owned; both must outlive the actor.
	shell_actor(shell_side& shell, const leshper::host* host,
	            shell_writing_flag* writing = nullptr) noexcept
		: _shell(&shell), _host(host), _writing(writing) {}

	// What every token this actor mints reads through. Exposed so the wiring site
	// can assert the actor and the registry are looking at one object.
	[[nodiscard]] const leshper::host* host() const noexcept { return _host; }

	shell_actor(const shell_actor&) = delete;
	shell_actor& operator=(const shell_actor&) = delete;

	// The loop's `shell` topic, owned here so that a message can never outlive
	// the storage it was recycled into.
	[[nodiscard]] shell_channel& replies() noexcept { return _replies; }
	[[nodiscard]] const shell_channel& replies() const noexcept { return _replies; }

	// --- The loop thread's side ---------------------------------------------

	// Fills the `execute` slot: the highest priority, and the one that makes the
	// shell thread stop serving highlights until it is done.
	void post_execute(std::string_view line, leshper::generation computed_against);

	// Fills the `port_call` slot and answers the sequence number the reply will
	// carry, which is what the blocked action matches on.
	[[nodiscard]] std::uint64_t post_port_call(std::string_view code,
	                                           leshper::generation computed_against);

	// Fills the `highlight` slot, OVERWRITING whatever was pending and
	// superseding whatever is in flight. That overwrite is the cancellation
	// ADR-0009 asks for - there is no cancel call, because a newer question
	// arriving is the only reason the older one would ever be cancelled.
	void post_highlight(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
	                    request_snapshot snapshot);

	// Asks `run` to return once it has finished what it is doing.
	void stop();

	// --- The shell thread's side --------------------------------------------

	// Serves slots until `stop()`. What #134 calls from `main` after spawning
	// the loop thread.
	void run();

	// One item, if there is one; false when all three slots are empty. The seam
	// a test drives instead of starting a thread, and what `run` is written in
	// terms of, so the two cannot diverge.
	bool serve_one();

	// --- Counters ------------------------------------------------------------

	[[nodiscard]] std::size_t served() const noexcept;
	// Highlights a later post overwrote before they ever ran: latest-wins,
	// counted.
	[[nodiscard]] std::size_t dropped() const noexcept;
	[[nodiscard]] bool idle() const noexcept;

private:
	struct execute_slot {
		std::string line;
		leshper::generation computed_against;
		bool filled = false;
	};

	struct port_slot {
		std::string code;
		leshper::generation computed_against;
		std::uint64_t sequence = 0;
		bool filled = false;
	};

	struct highlight_slot {
		std::string reactor;
		lesh_reactor_fn fn = nullptr;
		void* userdata = nullptr;
		request_snapshot snapshot;
		bool filled = false;
	};

	void serve_execute(execute_slot& job);
	void serve_port_call(port_slot& job);
	void serve_highlight(highlight_slot& job);

	shell_side* _shell;
	// The executing shell's own door, stamped on every token served below.
	const leshper::host* _host;
	shell_writing_flag* _writing;
	shell_channel _replies;

	mutable std::mutex _mutex;
	std::condition_variable _work;

	execute_slot _execute;
	port_slot _port;
	highlight_slot _highlight;

	// The cooperative cancellation the highlighter polls. Its address is stable
	// for the life of the actor, which is what lets the token point straight at
	// it the way #126's slots do.
	std::atomic<bool> _superseded{false};

	std::uint64_t _sequence = 0;
	std::size_t _served = 0;
	std::size_t _dropped = 0;
	bool _stopping = false;
	bool _busy = false;
};

// Runs one reactor against a snapshot ON THE CALLING THREAD, into `into`.
//
// The token minting the shell thread needs, factored out so it is written once:
// #135 wants it for the highlighter, `shell_actor` uses it for the `highlight`
// slot, and a test uses it to run a reactor with no thread at all. What it is
// NOT is a second reactor path - it builds exactly the `request_token`
// registry.cpp and workers.cpp build, and asserts `token_is_live` on it for the
// same reason workers.cpp does: the thread key is computed in three places now,
// and a disagreement would make every accessor on the token refuse.
//
// `superseded` is the flag the reactor's cooperative poll reads; it must outlive
// the call.
void run_reactor_here(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
                      request_snapshot snapshot, const std::atomic<bool>& superseded,
                      leshper::reactor_batch& into);

} // namespace lesh::ui
