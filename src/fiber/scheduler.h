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
// runnable when the tick began, in SPAWN ORDER, and reports whether anything is
// still runnable. Two consequences worth stating, because step 1's tick order
// depends on both:
//
//   - a fiber woken *during* a tick by an earlier fiber's slice does not run
//     until the next tick. The snapshot is taken up front, so a tick is a
//     bounded amount of work no matter what the fibers do to each other, and
//     the sequence is reproducible - which is what lets N-3's replay record it.
//   - a fiber that yields stays runnable, so `tick()` returning true means "call
//     me again with a zero poll timeout", exactly the shape the loop wants.
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

	// How many slices this fiber has been given. The yield-ordering test reads
	// it, and a reactor that never gets a slice is a step-1 bug this counts.
	[[nodiscard]] std::size_t slices() const noexcept { return _slices; }

	// Stack and guard extents, or all-zero once the fiber has finished and its
	// stack has been unmapped. `FiberGuardPage.*` writes just below `stack_base`.
	[[nodiscard]] stack_extents stack() const noexcept;

private:
	friend class scheduler;
	fiber(scheduler& on, entry_fn fn, void* userdata, const char* name) noexcept;

	scheduler* _sched = nullptr;
	entry_fn _fn = nullptr;
	void* _userdata = nullptr;
	const char* _name = "";
	mco_coro* _co = nullptr;
	fiber_state _state = fiber_state::ready;
	std::size_t _slices = 0;

	static void trampoline(mco_coro* co);
};

class scheduler {
public:
	explicit scheduler(scheduler_options with = {}) noexcept;
	~scheduler();
	scheduler(const scheduler&) = delete;
	scheduler& operator=(const scheduler&) = delete;

	// A new fiber, ready but not yet started. `name` must outlive the scheduler
	// (a literal, in practice): it is what the watchdog prints.
	//
	// NOT `[[nodiscard]]`: spawning is the effect, and a long-lived reactor that
	// nothing ever wakes by name is the common case - the scheduler owns the
	// handle either way.
	fiber& spawn(entry_fn fn, void* userdata, const char* name);

	// One slice for every fiber that is ready NOW, in spawn order. Returns
	// whether anything is still runnable afterwards.
	[[nodiscard]] bool tick();

	// One slice for one fiber. `tick()` is this in a loop; step 1 wants it
	// separately for the "reactors before and after the UI part" ordering.
	void run_one_slice(fiber& f);

	// Called from INSIDE a fiber. `yield` stays runnable; `park` does not, until
	// somebody calls `wake`.
	void yield();
	void park();

	// Callable from inside a fiber or from the host. Waking a fiber that is not
	// parked is a no-op, not an error: `slot::send` does not know or care
	// whether its receiver got as far as `recv`.
	void wake(fiber& f) noexcept;

	[[nodiscard]] bool runnable() const noexcept;
	[[nodiscard]] std::size_t fiber_count() const noexcept { return _fibers.size(); }
	[[nodiscard]] fiber* current() noexcept { return _current; }
	[[nodiscard]] const fiber* current() const noexcept { return _current; }

	// How many slices overran the watchdog budget. Always zero under NDEBUG,
	// where the watchdog does not exist.
	[[nodiscard]] std::size_t watchdog_overruns() const noexcept { return _overruns; }

	[[nodiscard]] const scheduler_options& options() const noexcept { return _options; }

private:
	scheduler_options _options;
	std::vector<std::unique_ptr<fiber>> _fibers;
	std::vector<fiber*> _slice_order;   // reused per tick, so a tick allocates nothing
	fiber* _current = nullptr;
	std::size_t _overruns = 0;
};

} // namespace lesh::fiber
