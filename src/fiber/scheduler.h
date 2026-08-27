#pragma once

// THE SCHEDULER. Instantiable, never global.
//
// The house rule the ticket names is `signal_hub`'s: "INSTANTIABLE, only
// install() is process-global". Here there is not even an install(). A
// `scheduler` owns its fibers, its stacks and its run order, and it holds no
// process state at all - which is what keeps the cord door open. The day
// history persistence gets its own thread, that thread constructs its own
// scheduler and nothing has to be untangled first.
//
// WHAT IT IS NOT, in this step: it does not own `poll(2)`, it has no phase, it
// has no timers, and it does not know what a file descriptor is. `lesh_fiber`
// links `lesh_substrate` and nothing else, which is the same argument that makes
// `Fiber*` sweep-exempt: a change here provably cannot reach syntax, the runtime
// or an fd. Step 1 (#145) makes `event_loop::run` slice this from the host loop;
// `tick()` is the seam it will call.
//
// THE SLICE IS THE UNIT. One slice = one `mco_resume`: the fiber runs until it
// yields, parks, or returns. `tick()` runs one slice for each fiber that was
// runnable when the tick began, IN THE ORDER THEY BECAME RUNNABLE, and reports
// whether anything is still runnable. Two consequences worth stating, because
// step 1's tick order depends on both:
//
//   - a fiber woken *during* a tick by an earlier fiber's slice does not run
//     until the next tick. The snapshot is taken up front, so a tick is a
//     bounded amount of work no matter what the fibers do to each other, and
//     the sequence is reproducible - which is what lets N-3's replay record it.
//   - a fiber that yields stays runnable, so `tick()` returning true means "call
//     me again with a zero poll timeout", exactly the shape the loop wants.
//
// "IN THE ORDER THEY BECAME RUNNABLE" READ "IN SPAWN ORDER" UNTIL #200, and for
// the fibers step 1 actually has, it is the same sentence: a fiber that has
// never parked became runnable at `spawn`, so spawn order IS arrival order and
// `FiberSwitch.ThreeFibersGetOneSlicePerTickInSpawnOrder` reads identically
// before and after. Yielding does NOT re-stamp the arrival - a reactor yielding
// at every `kPollEvery` point would otherwise reshuffle the tick on every slice,
// and the grilling record wants the sequence FIXED. Only the parked->ready
// transition stamps a new arrival, which is what makes a group's replayed wakes
// observable in arrival order. See GROUPS.
//
// ---------------------------------------------------------------------------
// GROUPS (#200) - PARKING A SET WITH ONE BIT
// ---------------------------------------------------------------------------
//
// A group is a scheduler TAG, and the scheduler is deliberately agnostic about
// what the tags mean: `spawn`'s trailing argument is a plain `std::uint8_t`
// index, there is no `enum class group` here, and `src/ui/` names its own lanes
// when step 1 derives them from the phase. Eight of them, because the park set
// is one byte and one byte covers every lane the design names (emitters,
// observers, io-waiters, execution) with room left.
//
// The point, in #145's words: "parking a set is not something done TO the fibers
// - it is a scheduler bit. Park = mark the group unrunnable; wake events for its
// members QUEUE instead of scheduling. Resume = clear the bit, replay the queued
// wakes in arrival order." O(1), no handshake, no race, because under
// cooperative scheduling suspended is every fiber's default state.
//
// While a group is parked, its fibers are NOT RUNNABLE. `runnable()` says so,
// `tick()` skips them, and `run_one_slice` on one of them asserts - the host
// cannot slice a fiber it has just declared unrunnable, and quietly running it
// would break the structural-quiesce property the whole parking story exists
// for. `wake(f)` for a member is queued instead of applied, and `resume_group`
// replays the queue.
//
// TWO DECISIONS #200 LEFT TO THIS FILE - the first exactly as the ticket
// recommended, the second as far as the ticket's own other requirements leave
// room for:
//
//   1. REPLAYED WAKES RUN IN WAKE ORDER, NOT SPAWN ORDER. "Replay in arrival
//      order" is the record's determinism promise, and a promise nothing can
//      observe is not one - so it had to reach the tick, not just the queue.
//      The mechanism is the arrival stamp above: `resume_group` walks its queue
//      front to back and each `wake` stamps a fresh, larger arrival, so the next
//      tick's snapshot - sorted by arrival - hands out slices in exactly the
//      order the wakes came in. The alternative (queue in arrival order, then
//      slice in spawn order) makes the queue's order unobservable and the
//      documented promise untestable; rejected for that reason.
//
//   2. A GROUP PARKED MID-TICK TAKES EFFECT AT ONCE FOR ITS OWN MEMBERS; THE
//      REST OF THE SNAPSHOT FINISHES. A tick's snapshot bounds WHICH fibers may
//      run and in WHAT ORDER; it never promised that each of them WILL run.
//      Runnability - the fiber's own state and its group's bit - is re-checked
//      immediately before every slice, which is what `tick()` already did for
//      individual state ("an earlier slice may have parked it"). The group bit
//      is one more way of not being runnable and is checked at the same place,
//      so there is one rule rather than two.
//
//      The ticket's recommendation - "a park takes effect for the next tick; the
//      current snapshot finishes" - is honoured in the part that matters: the
//      tick does not bail out, and every fiber outside the parked group still
//      gets its slice. It is NOT honoured in the reading where the newly parked
//      group's own remaining snapshot entries run anyway, and that reading is
//      unavailable: the same ticket asks `run_one_slice` to ASSERT on a
//      parked-group fiber, so a tick that sliced one would trip its own gate.
//      The two requirements only fit this way round.
//
//      Consequently `park_group` is legal from inside a fiber - the grilling
//      record's phase writer is "the execution fiber on return", which is inside
//      a slice - and the assert it carries is the narrow one that preserves the
//      ticket's stated invariant exactly: A FIBER MAY NOT PARK THE GROUP IT IS
//      RUNNING IN, so "a fiber that was mid-slice cannot be in a parked group"
//      holds. The blanket `_current == nullptr` the ticket sketched would also
//      hold it, and would additionally forbid the mid-tick case it asks to be
//      tested; the narrow form keeps the invariant and the test.
//
// THERE IS NO CANCELLATION IN v1. Destroying a parked fiber does not unwind its
// stack - minicoro cannot, and neither could a stackless design without
// exceptions - so whatever that stack owned is lost. v1's cancellation is the
// `slot`'s superseded flag: the fiber notices, abandons its work and loops back
// to `recv`, on its own stack, in its own time. `FiberLsanPositiveControl` is
// that fact written down as a test.

#include "fiber/stack.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct mco_coro;

namespace lesh::fiber {

class scheduler;

// A fiber body. Runs on its own stack; returning from it finishes the fiber.
// A plain function pointer plus userdata rather than a `std::function`, on
// ADR-0007's ownership discipline and because the reactor ABI is already this
// shape - `lesh_reactor_fn` is `void (*)(handle, void*)`.
using entry_fn = void (*)(scheduler& on, void* userdata);

enum class fiber_state : std::uint8_t {
	ready,      // has not started, or yielded: a tick will give it a slice
	running,    // this is the fiber whose slice is executing right now
	parked,     // waiting for a `wake()`; a tick skips it
	finished,   // its body returned; its stack is gone
};

// How many groups a scheduler has, and the mask that means "all of them". The
// index is what `spawn`/`park_group`/`resume_group` take; the MASK is what
// `tick`/`runnable` take, because those select a SET.
inline constexpr std::uint8_t group_count = 8;
inline constexpr std::uint8_t all_groups = 0xff;

[[nodiscard]] inline constexpr std::uint8_t group_mask_of(std::uint8_t group) noexcept {
	return static_cast<std::uint8_t>(1u << group);
}

// What the debug watchdog does when a slice runs past its budget.
enum class watchdog_action : std::uint8_t {
	log,      // one line to the log facility, category `worker`
	abort_,   // die, with the fiber's name - what `lesh_tests` asks for
};

struct scheduler_options {
	// Per-fiber stack. 512 KB, 1 MB under ASan; see `stack.h`.
	std::size_t stack_bytes = 0;   // 0 = default_stack_size()

	// A slice that runs this long without yielding is a bug: something in a
	// fiber is doing blocking work, and on the host thread that is a frozen
	// terminal. 50 ms is the ticket's number.
	std::chrono::nanoseconds watchdog_budget = std::chrono::milliseconds{50};

	// WHY THIS IS A RUNTIME KNOB AND NOT `#ifdef LESH_TESTS`.
	//
	// The ticket asks the watchdog to assert "under `lesh_tests`" and log
	// otherwise. `lesh_fiber` is ONE archive, compiled once, linked into both
	// binaries, so a compile definition on the `lesh_tests` target cannot reach
	// this translation unit - the switch has to be a value the test binary
	// passes in. The fiber tests set `abort_`; the shell will keep `log`, where
	// a frozen prompt is bad and a dead prompt is worse.
	watchdog_action on_overrun = watchdog_action::log;
};

// One fiber. Created only by `scheduler::spawn`, which returns a reference that
// stays valid for the scheduler's life - the fibers are held indirectly, so the
// vector growing does not move them, and a `slot`'s parked-receiver pointer
// cannot dangle.
class fiber {
public:
	fiber(const fiber&) = delete;
	fiber& operator=(const fiber&) = delete;
	~fiber();

	[[nodiscard]] const char* name() const noexcept { return _name; }
	[[nodiscard]] fiber_state state() const noexcept { return _state; }
	[[nodiscard]] bool ready() const noexcept { return _state == fiber_state::ready; }
	[[nodiscard]] bool parked() const noexcept { return _state == fiber_state::parked; }
	[[nodiscard]] bool finished() const noexcept { return _state == fiber_state::finished; }

	// Which group this fiber was spawned into. Fixed for its life: a fiber's lane
	// is a property of what it IS, and a movable membership would make "the set
	// is quiesced the instant the bit flips" a lie.
	[[nodiscard]] std::uint8_t group() const noexcept { return _group; }

	// How many slices this fiber has been given. The yield-ordering test reads
	// it, and a reactor that never gets a slice is a step-1 bug this counts.
	[[nodiscard]] std::size_t slices() const noexcept { return _slices; }

	// Stack and guard extents, or all-zero once the fiber has finished and its
	// stack has been unmapped. `FiberGuardPage.*` writes just below `stack_base`.
	[[nodiscard]] stack_extents stack() const noexcept;

private:
	friend class scheduler;
	fiber(scheduler& on, entry_fn fn, void* userdata, const char* name,
	      std::uint8_t group) noexcept;

	scheduler* _sched = nullptr;
	entry_fn _fn = nullptr;
	void* _userdata = nullptr;
	const char* _name = "";
	mco_coro* _co = nullptr;
	fiber_state _state = fiber_state::ready;
	std::uint8_t _group = 0;
	std::size_t _slices = 0;

	// WHEN THIS FIBER BECAME RUNNABLE - stamped at `spawn` and at every
	// parked->ready transition, never at a yield. The tick sorts its snapshot by
	// it; see the header's decision 1.
	std::uint64_t _ready_at = 0;

	static void trampoline(mco_coro* co);
};

class scheduler {
public:
	explicit scheduler(scheduler_options with = {}) noexcept;
	~scheduler();
	scheduler(const scheduler&) = delete;
	scheduler& operator=(const scheduler&) = delete;

	// A new fiber, ready but not yet started, tagged with a group index (0 is the
	// default group and the only one step 1 needs before the phase exists).
	// `name` must outlive the scheduler (a literal, in practice): it is what the
	// watchdog prints.
	//
	// Spawning into a PARKED group is legal and does nothing surprising: the
	// fiber is `ready`, is not runnable, and gets its first slice when the group
	// resumes. No wake is queued for it - it has never parked, so there is
	// nothing to replay.
	//
	// NOT `[[nodiscard]]`: spawning is the effect, and a long-lived reactor that
	// nothing ever wakes by name is the common case - the scheduler owns the
	// handle either way.
	fiber& spawn(entry_fn fn, void* userdata, const char* name, std::uint8_t group = 0);

	// One slice for every fiber that is runnable NOW, in arrival order. Returns
	// whether anything is still runnable afterwards.
	//
	// The mask overload restricts the tick to the groups whose bits are set -
	// this is what lets step 1's host run "emitters" and "observers" as different
	// sets in the two positions of its loop. `tick()` is `tick(all_groups)`, and
	// `tick(mask)` reports `runnable(mask)`: "call me again with a zero poll
	// timeout" is a question about the set the caller just ran, not about groups
	// it did not ask for.
	[[nodiscard]] bool tick();
	[[nodiscard]] bool tick(std::uint8_t group_mask);

	// One slice for one fiber. `tick()` is this in a loop; step 1 wants it
	// separately for the "reactors before and after the UI part" ordering.
	// Asserts if the fiber's group is parked: see GROUPS.
	void run_one_slice(fiber& f);

	// Called from INSIDE a fiber. `yield` stays runnable; `park` does not, until
	// somebody calls `wake`.
	void yield();
	void park();

	// Callable from inside a fiber or from the host. Waking a fiber that is not
	// parked is a no-op, not an error: `slot::send` does not know or care
	// whether its receiver got as far as `recv`.
	//
	// If the fiber's group is parked the wake is QUEUED, in arrival order, and
	// `resume_group` replays it. Queueing is idempotent - a second wake for a
	// fiber already in the queue keeps its FIRST arrival position, because a wake
	// is a level and not an edge, exactly as the direct path is idempotent.
	void wake(fiber& f) noexcept;

	// PARK A SET WITH ONE BIT. Idempotent: parking an already-parked group is a
	// no-op, so a host deriving group bits from a phase can write the same bits
	// twice without bookkeeping.
	//
	// Legal from the host between slices and from inside a fiber's slice; the one
	// thing it may not do is park the group the running fiber belongs to. See the
	// header's decision 2 for why the assert is that shape and not `_current ==
	// nullptr`.
	void park_group(std::uint8_t group);

	// Clear the bit and replay the queued wakes IN ARRIVAL ORDER. Returns how
	// many it replayed. Idempotent likewise: resuming a group that is not parked
	// clears nothing and replays nothing, and returns 0.
	//
	// A replayed wake for a fiber that is somehow no longer parked is delivered
	// anyway - `wake` makes it the no-op it already is - and still counts as
	// replayed, because the count answers "how much queued work did this resume
	// hand back", not "how many state transitions happened".
	std::size_t resume_group(std::uint8_t group);

	[[nodiscard]] bool group_parked(std::uint8_t group) const noexcept;

	// Queued wakes waiting for a resume: one group's, or every group's. The
	// debug counter the design asks channels and the scheduler to carry - a
	// number that only grows is a group somebody forgot to resume.
	[[nodiscard]] std::size_t queued_wakes(std::uint8_t group) const noexcept;
	[[nodiscard]] std::size_t queued_wakes() const noexcept { return _pending_wakes.size(); }

	[[nodiscard]] bool runnable() const noexcept;
	[[nodiscard]] bool runnable(std::uint8_t group_mask) const noexcept;
	[[nodiscard]] std::size_t fiber_count() const noexcept { return _fibers.size(); }
	[[nodiscard]] fiber* current() noexcept { return _current; }
	[[nodiscard]] const fiber* current() const noexcept { return _current; }

	// How many slices overran the watchdog budget. Always zero under NDEBUG,
	// where the watchdog does not exist.
	[[nodiscard]] std::size_t watchdog_overruns() const noexcept { return _overruns; }

	[[nodiscard]] const scheduler_options& options() const noexcept { return _options; }

private:
	// True when `f` is in `group_mask` AND its group is not parked AND it is
	// `ready`. The one definition of runnable, so the tick, the snapshot's
	// re-check and `runnable()` cannot drift apart.
	[[nodiscard]] bool is_runnable(const fiber& f, std::uint8_t group_mask) const noexcept;

	scheduler_options _options;
	std::vector<std::unique_ptr<fiber>> _fibers;
	std::vector<fiber*> _slice_order;   // reused per tick, so a tick allocates nothing
	// Wakes that arrived for members of a parked group, oldest first. ONE list
	// for the whole scheduler rather than one per group, so a wake's position is
	// its arrival among all wakes and not merely among its group's. Reserved to
	// `fiber_count()` at every spawn, and deduplicated, so it holds at most one
	// entry per fiber and `wake` can stay `noexcept` without allocating.
	std::vector<fiber*> _pending_wakes;
	fiber* _current = nullptr;
	// WHAT ASan THINKS THIS THREAD'S STACK IS, read once before the first slice.
	// The vendored library does not tell ASan the thread is back on its own stack
	// when a coroutine yields to something that is not a coroutine - which is every
	// yield lesh takes - so `run_one_slice` says so itself. Always null and unused
	// without ASan; see the note at the top of scheduler.cpp.
	const void* _host_stack_bottom = nullptr;
	std::size_t _host_stack_size = 0;
	std::size_t _overruns = 0;
	std::uint8_t _parked_groups = 0;
	std::uint64_t _next_ready_at = 0;   // stamps `fiber::_ready_at`; see decision 1
};

} // namespace lesh::fiber
