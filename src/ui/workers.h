#pragma once

// The worker pool: an arena per worker, a latest-wins slot per reactor, and a
// pool that parks at accept (#126; the resolutions of #90 and #91; architecture
// spec §5).
//
// THE HOST'S, AND IN `src/ui/` SINCE #168. Where a reactor runs is the host's
// private choice - threads today, #145's fibers tomorrow - and leshper is not
// told either way: it emits `worker_request` and applies whatever comes back
// that is still current.
//
// WHAT IS HERE AND WHAT IS NOT. This is the pool only. The event loop is
// #128's and is not written; posix_spawn of user providers is #94's; no reactor
// exists yet. The one place this file reaches toward the loop is
// `completion_queue`, which owns a pipe and hands out the read end - a plain fd,
// which is what #128's `worker` topic polls.
//
// THE FOUR PROPERTIES, and where each is enforced:
//
//   AN ARENA PER WORKER, RESET AT REQUEST END (#90). Each worker owns a
//   buffer_pool and rewinds it to its base after every task, so nothing a
//   reactor allocated survives its request. That is N-4 structurally rather
//   than by check: the memory behind a stale parse is GONE. Native code running
//   on a worker reaches its arena through current_worker_arena(); the ABI hands
//   no arena pointer across the C boundary, and this does not change that.
//
//   LATEST-WINS, ONE IN-FLIGHT PLUS ONE PENDING (#90, the owner's words: "if
//   something is waiting then we drop it, as we need only the latest"). A slot
//   holds one `pending` task that a later submit OVERWRITES, so queue depth <= 1
//   is the type and not a policy. Submitting over an in-flight task also sets
//   that task's superseded flag - the cooperative poll the ABI already has.
//
//   POOLED, GENERATION-TAGGED MESSAGES. A completion carries a `reactor_batch`,
//   which is the vocabulary registry.h already speaks, so a drained completion
//   goes straight into `loop_harness::apply()` and meets the drop rule there
//   rather than growing a second one. Batches come from a free list and return
//   to it on destruction, so their vectors keep capacity across keystrokes.
//
//   QUIESCE AT ACCEPT (#91). park_all() returns when every worker is idle at a
//   check-in and holds them there; a parked worker is blocked in
//   condition_variable::wait, which holds no mutex and is not inside malloc, so
//   a fork taken after park_all() returns gives a child born from a genuinely
//   single-threaded moment. resume() releases. Parking NESTS, so a fork site
//   reached from inside an already-parked scope is not a deadlock.
//   assert_quiesced() is the debug assertion #91 asked every fork site to carry
//   in place of pthread_atfork.
//
// PARK LATENCY IS TASK GRANULARITY, and that fact shapes everything above.
// #115 measured quiesce cost byte-identically across event-loop backends and at
// exactly the task length - 4.4 us for 1 us tasks, 100 us for 100 us tasks - so
// the lever is how finely a compute checks in, and the check-in is the one the
// ABI already has: PARKING SUPERSEDES WHAT IS IN FLIGHT. A reactor polling
// lesh_request_superseded sees 1 and returns LESH_ERR_SUPERSEDED, which lands it
// at the between-tasks check-in immediately. One flag, one poll, two reasons -
// and no new ABI function, which matters because abi.h is additive-only. It is
// also exactly F-22's rule: at accept, a pending highlight is abandoned anyway.
//
// THREADING SUMMARY. submit, park_all, resume, supersede_all and drain are the
// loop thread's; post is a worker's; the queue and the message free list carry
// their own mutexes. No public function here may be called while holding
// another lock this file owns, and park_all() must not be called from a worker.

#include "leshper/abi.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "substrate/arena.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lesh::ui {

// ---------------------------------------------------------------------------
// The snapshot a request is computed against.
//
// Buffer, cursor, selection and generation - what spec §6.1 says the token
// carries, and nothing else. Copied on the loop thread at submit and owned by
// the worker from there, so the editor may move on the instant submit returns.
// ---------------------------------------------------------------------------

struct request_snapshot {
	std::string buffer;
	std::size_t cursor = 0;
	std::size_t selection_start = 0;
	std::size_t selection_end = 0;
	bool selection_active = false;
	leshper::generation computed_against;
	std::uint32_t event_kind = 0;

	// What the shell knows (#135), for the reactor that asks - today only the
	// highlighter, and only for `command_kind`. NOT copied and not owned: a
	// pointer to the wiring site's adapter over `shell_state`, which outlives
	// every request.
	//
	// ADR-0009 is what makes a bare pointer safe here, and it is worth naming.
	// The shell is the main thread and owns `shell_state`; a highlight, a port
	// call that writes it, and an execution are serialized on that thread. So
	// this points at state that cannot change while the compute it belongs to is
	// running - which is why #130's copy-on-write definitions version was deleted
	// rather than kept as insurance.
	//
	// Null - the default - is "no host attached": every name classifies as
	// LESH_COMMAND_UNKNOWN. A submit that leaves it null is therefore honest
	// rather than broken, and is what every state-free reactor does.
	//
	// `leshper::host` since #168 Phase B, where this was a `shell_knowledge*`.
	// The tables are still what answers - `ui::editor_host` holds them - but the
	// `$PATH` sweep that used to run inside the editor moved behind the same
	// door, so what the snapshot carries is the door and not one room of it.
	const leshper::host* host = nullptr;
};

// The snapshot of an editor state, as the loop would take it.
//
// Selection reads as inactive because #96 has not landed the model; the fields
// exist so that filling them in later is a change here and nowhere else.
[[nodiscard]] request_snapshot snapshot_of(const leshper::state& target, std::uint32_t event_kind);

// The same, INTO STORAGE THE CALLER ALREADY HAS. `into.buffer` is assigned
// rather than replaced, so a snapshot taken into the same object twice allocates
// only when the line grows past what that object already holds. `snapshot_of` is
// this plus a fresh object, and the two agree field for field - `host` included,
// which is reset rather than left as the caller found it.
void take_snapshot(request_snapshot& into, const leshper::state& target,
                   std::uint32_t event_kind);

// ---------------------------------------------------------------------------
// Pooled messages.
// ---------------------------------------------------------------------------

class message_pool;

// One worker's answer, borrowed from the free list.
//
// Move-only and RAII: the batch returns to the pool when the last handle to it
// dies, which is what makes "pooled" a property of the type rather than of the
// loop remembering to give it back.
class completion {
public:
	completion() noexcept = default;
	~completion();

	completion(completion&& other) noexcept;
	completion& operator=(completion&& other) noexcept;
	completion(const completion&) = delete;
	completion& operator=(const completion&) = delete;

	[[nodiscard]] bool empty() const noexcept { return _batch == nullptr; }

	[[nodiscard]] const leshper::reactor_batch& batch() const noexcept { return *_batch; }
	[[nodiscard]] leshper::reactor_batch& batch() noexcept { return *_batch; }

	// Hands the message back early. Idempotent.
	void recycle() noexcept;

private:
	friend class message_pool;
	completion(message_pool* owner, leshper::reactor_batch* batch) noexcept
		: _owner(owner), _batch(batch) {}

	message_pool* _owner = nullptr;
	leshper::reactor_batch* _batch = nullptr;
};

// The free list behind those messages.
//
// Owns every batch it ever minted and frees them in its destructor (ADR-0007),
// which is why it must outlive every completion it handed out - the pool is
// declared before the queue in worker_pool for exactly that reason.
class message_pool {
public:
	message_pool() = default;
	~message_pool();

	message_pool(const message_pool&) = delete;
	message_pool& operator=(const message_pool&) = delete;

	[[nodiscard]] completion acquire();
	void release(leshper::reactor_batch* batch) noexcept;

	// Messages currently out on loan.
	[[nodiscard]] std::size_t live() const noexcept;
	// Messages this pool has ever minted - the number that must stay small if
	// "pooled" means anything.
	[[nodiscard]] std::size_t minted() const noexcept;

private:
	mutable std::mutex _mutex;
	std::vector<std::unique_ptr<leshper::reactor_batch>> _owned;
	std::vector<leshper::reactor_batch*> _free;
	std::size_t _live = 0;
};

// ---------------------------------------------------------------------------
// The completion queue: the one seam toward the event loop.
//
// Loop-agnostic by construction. It owns a pipe and hands out the read end,
// because a pipe read end is a plain fd - which is what #128's loop polls, and
// what an eventfd (Linux-only) or an EVFILT_USER poke (kqueue-only) would not
// have been. #115 measured wake-to-callback on exactly this shape at 1.9-2.2 us
// on every backend, indistinguishable, so the seam costs nothing to keep
// neutral.
//
// The fd is armed on the empty-to-non-empty transition and disarmed by drain,
// so N completions cost at most one byte and one loop turn.
//
// THE LEVEL-TRIGGERED, LOSSY CONTRACT (#128's `worker` topic): drain() IS the
// "consume the fd, then drain the queue" pair, done under one lock, and the
// loop's whole obligation is to call it when the fd polls readable. Reading the
// fd without draining is the way to lose a wakeup permanently - the queue stays
// armed, so no further byte is ever written - so do not. POLLHUP on this fd
// means the pool has been destroyed.
// ---------------------------------------------------------------------------

class completion_queue {
public:
	completion_queue();
	~completion_queue();

	completion_queue(const completion_queue&) = delete;
	completion_queue& operator=(const completion_queue&) = delete;

	// From a worker.
	void post(completion done);

	// From the loop thread. Appends everything queued to `out`, disarms the
	// wakeup fd, and answers how many arrived.
	std::size_t drain(std::vector<completion>& out);

	// What a test uses in place of a loop: blocks until at least `least` are
	// queued, then drains. The loop itself never calls this - it waits on
	// wakeup_fd() with everything else it waits on.
	std::size_t wait_and_drain(std::vector<completion>& out, std::size_t least);

	// The fd #128's `worker` topic registers for readability. Poll it; answer a
	// readable one with drain(), which consumes the fd itself. -1 only if the
	// pipe could not be created, which is asserted at construction.
	[[nodiscard]] int wakeup_fd() const noexcept { return _read_fd; }

	[[nodiscard]] bool empty() const;
	[[nodiscard]] std::size_t size() const;
	// Whether a wakeup byte is outstanding. Exposed so the arming rule can be
	// asserted directly instead of only through a poll() that happens to agree.
	[[nodiscard]] bool armed() const;

private:
	std::size_t drain_locked(std::vector<completion>& out);

	mutable std::mutex _mutex;
	std::condition_variable _arrived;
	std::vector<completion> _queue;
	int _read_fd = -1;
	int _write_fd = -1;
	bool _armed = false;
};

// ---------------------------------------------------------------------------
// The pool.
// ---------------------------------------------------------------------------

class worker_pool {
public:
	// min(4, hardware), never zero. Argued at the definition in workers.cpp.
	[[nodiscard]] static std::size_t default_worker_count() noexcept;

	explicit worker_pool(std::size_t workers = default_worker_count(),
	                     std::size_t arena_bytes = BUFFER_POOL_SIZE);
	~worker_pool();

	worker_pool(const worker_pool&) = delete;
	worker_pool& operator=(const worker_pool&) = delete;

	[[nodiscard]] std::size_t size() const noexcept { return _workers.size(); }

	// Submits a computation under `key` - the reactor or provider name, which is
	// also the name of its latest-wins slot.
	//
	// Overwrites whatever was pending under that key and supersedes whatever was
	// in flight under it. Never blocks on a worker and never grows a queue.
	void submit(std::string_view key, request_snapshot snapshot,
	            lesh_reactor_fn fn, void* userdata);

	// The same, TAKING THE SNAPSHOT IN PLACE - and this is the overload the loop
	// uses on every keystroke.
	//
	// The one above builds a `request_snapshot` at the call site and moves it
	// into the slot, which frees the string the slot was holding: one malloc and
	// one free per reactor per keystroke, for a line whose length barely changes.
	// This one assigns straight into the slot's retained buffer, so a warm round
	// reaches the heap only when the line outgrows what the slot already has.
	// `AllocationTest.SubmittingAWarmReactorRoundCostsNoHeap` is the pin.
	void submit(std::string_view key, const leshper::state& target, std::uint32_t event_kind,
	            lesh_reactor_fn fn, void* userdata);

	// Supersedes everything in flight and drops everything pending, without
	// parking. What the loop does when the buffer has moved on but nobody is
	// about to fork.
	void supersede_all() noexcept;

	[[nodiscard]] completion_queue& completions() noexcept { return _completions; }
	[[nodiscard]] const completion_queue& completions() const noexcept { return _completions; }
	[[nodiscard]] const message_pool& messages() const noexcept { return _messages; }

	// --- Quiesce (#91) --------------------------------------------------------

	// Blocks until every worker is idle at a check-in, and holds them there.
	//
	// The workers' half of #128's `loop.quiesce()`, whose other halves - the
	// terminal restore and the tcsetpgrp handoff - are the loop's and not this
	// file's. Only the loop thread forks, and only after this has returned.
	//
	// Nests: two calls need two resume()s. Loop thread only - a worker calling
	// this would wait for itself.
	void park_all();

	// Releases the hold taken by the matching park_all().
	void resume();

	[[nodiscard]] bool is_quiesced() const noexcept;

	// The assertion #91 put at every fork site. Compiled out of Release.
	void assert_quiesced() const noexcept;

	// --- Counters, for tests and for the loop's own instrumentation ----------

	[[nodiscard]] std::size_t started() const noexcept;
	[[nodiscard]] std::size_t completed() const noexcept;
	// Pending tasks a later submit overwrote: latest-wins, counted.
	[[nodiscard]] std::size_t dropped() const noexcept;

private:
	struct task {
		request_snapshot snapshot;
		lesh_reactor_fn fn = nullptr;
		void* userdata = nullptr;
		bool valid = false;
	};

	// One reactor or provider's latest-wins slot.
	//
	// Node-based storage below, so a slot's address is stable for the life of
	// the pool - the request token points its superseded poll straight at
	// `superseded`, and a rehash that moved it would dangle mid-compute.
	struct slot {
		std::string name;
		task pending;
		bool in_flight = false;
		bool queued = false;
		std::atomic<bool> superseded{false};
	};

	struct worker {
		explicit worker(std::size_t arena_bytes) : arena(arena_bytes), arena_base(arena.at()) {}

		std::thread thread;
		buffer_pool arena;
		char* arena_base;
	};

	// The slot for `key`, created on first use. `_mutex` HELD.
	[[nodiscard]] slot& slot_for(std::string_view key);
	// Marks `s` pending and makes it runnable. `_mutex` HELD, and the snapshot
	// already written - the two submit overloads differ only in how it got there.
	void arm(slot& s, lesh_reactor_fn fn, void* userdata);

	void run(worker& me);
	// `job` is consumed: the snapshot's buffer moves into the token rather than
	// being copied a second time on the worker.
	void compute(task& job, slot& owner, leshper::reactor_batch& into);

	mutable std::mutex _mutex;
	std::condition_variable _work;     // a worker waits here for something to do
	std::condition_variable _parked;   // park_all waits here for the last check-in
	std::condition_variable _released; // a parked worker waits here for resume

	std::map<std::string, slot, std::less<>> _slots;
	std::deque<slot*> _ready;
	std::vector<std::unique_ptr<worker>> _workers;

	std::size_t _park_requests = 0;
	std::size_t _parked_count = 0;
	bool _stopping = false;

	std::size_t _started = 0;
	std::size_t _completed = 0;
	std::size_t _dropped = 0;

	// The token's call_token, minted here rather than from lesh_registry::calls.
	// That counter is loop-thread-only by ADR-0008 and is not atomic; the ABI
	// asks only that a live token's value be non-zero, so a pool-local sequence
	// satisfies it without reaching into the registry from a worker.
	std::atomic<std::uint64_t> _call_tokens{1};

	// Declared before the queue: the queue holds completions, and a completion
	// must not outlive the pool that owns its storage (ADR-0007).
	message_pool _messages;
	completion_queue _completions;
};

// Parks for the duration of a scope. What a fork site holds.
class parked_scope {
public:
	explicit parked_scope(worker_pool& pool) : _pool(&pool) { _pool->park_all(); }
	~parked_scope() { _pool->resume(); }

	parked_scope(const parked_scope&) = delete;
	parked_scope& operator=(const parked_scope&) = delete;

private:
	worker_pool* _pool;
};

// The arena belonging to the worker this call is running on, or nullptr off a
// worker.
//
// The seam native reactor code uses: the highlighter (#124) parses into this,
// and the parse is gone when the request ends. A reactor reached through the C
// ABI never sees it - ADR-0006 hands no arena pointer across that boundary.
[[nodiscard]] buffer_pool* current_worker_arena() noexcept;

} // namespace lesh::ui
