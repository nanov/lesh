#include "fiber/scheduler.h"

#include "minicoro.h"
#include "substrate/assert.h"
#include "substrate/log.h"

#include <cstdio>
#include <cstdlib>

namespace lesh::fiber {
namespace {

// Every minicoro call in this file goes through here. A failed switch is not a
// condition a shell can carry on from - the alternative to switching is running
// on a stack we have just decided is invalid - so it is a hard stop with the
// upstream description attached, rather than an error code for a caller to
// forget to check.
void must(mco_result res, const char* what, const char* who) {
	if (res == MCO_SUCCESS) [[likely]]
		return;
	std::fprintf(stderr, "lesh: fiber %s: %s failed: %s\n", who, what,
	             mco_result_description(res));
	std::abort();
}

} // namespace

// ---------------------------------------------------------------------------
// fiber
// ---------------------------------------------------------------------------

fiber::fiber(scheduler& on, entry_fn fn, void* userdata, const char* name) noexcept
	: _sched(&on), _fn(fn), _userdata(userdata), _name(name != nullptr ? name : "") {}

fiber::~fiber() {
	if (_co == nullptr)
		return;
	// A parked fiber's stack is unmapped here WITHOUT unwinding it: see the
	// header's "there is no cancellation in v1". `mco_destroy` accepts a
	// suspended coroutine, which is exactly what a parked fiber is.
	must(mco_destroy(_co), "destroy", _name);
	_co = nullptr;
}

stack_extents fiber::stack() const noexcept {
	if (_co == nullptr)
		return {};
	return extents_of(*_co);
}

void fiber::trampoline(mco_coro* co) {
	auto* const self = static_cast<fiber*>(mco_get_user_data(co));
	LESH_ASSERT(self != nullptr && self->_fn != nullptr);
	self->_fn(*self->_sched, self->_userdata);
	// Returning from here returns into minicoro, which marks the coroutine dead
	// and switches back. `run_one_slice` reads that and reaps the stack.
}

// ---------------------------------------------------------------------------
// scheduler
// ---------------------------------------------------------------------------

scheduler::scheduler(scheduler_options with) noexcept : _options(with) {
	if (_options.stack_bytes == 0)
		_options.stack_bytes = default_stack_size();
}

scheduler::~scheduler() {
	LESH_ASSERT(_current == nullptr && "a scheduler cannot be destroyed from inside its own fiber");
	// Reverse spawn order, so that a fiber holding a reference to an earlier one
	// is gone before the earlier one is.
	while (!_fibers.empty())
		_fibers.pop_back();
}

fiber& scheduler::spawn(entry_fn fn, void* userdata, const char* name) {
	LESH_ASSERT(fn != nullptr);

	auto born = std::unique_ptr<fiber>(new fiber(*this, fn, userdata, name));

	mco_desc desc = mco_desc_init(&fiber::trampoline, _options.stack_bytes);
	desc.user_data = born.get();
	install_guarded_allocator(desc);

	mco_coro* co = nullptr;
	must(mco_create(&co, &desc), "create", born->_name);
	verify_guard_placement(*co);
	born->_co = co;

	LESH_LOG(log::level::debug, log::category::worker, "fiber spawned: %s stack=%zu guard=%zu",
	         born->_name, co->stack_size, page_size());

	_fibers.push_back(std::move(born));
	return *_fibers.back();
}

void scheduler::run_one_slice(fiber& f) {
	LESH_ASSERT(_current == nullptr && "slices do not nest: park, do not recurse");
	if (f._state == fiber_state::finished)
		return;

	_current = &f;
	f._state = fiber_state::running;
	++f._slices;

#ifndef NDEBUG
	const auto started = std::chrono::steady_clock::now();
#endif

	const mco_result res = mco_resume(f._co);

#ifndef NDEBUG
	// THE WATCHDOG. A slice that runs 50 ms without yielding is the thing this
	// whole architecture can get wrong: on one thread, a fiber that blocks is a
	// terminal that has stopped answering. Compiled out under NDEBUG entirely -
	// no clock read, no branch, no member touched - because the cost rule that
	// governs the logger governs this too.
	const auto elapsed = std::chrono::steady_clock::now() - started;
	if (elapsed > _options.watchdog_budget) [[unlikely]] {
		++_overruns;
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
		if (_options.on_overrun == watchdog_action::abort_) {
			std::fprintf(stderr,
			             "lesh: fiber watchdog: slice %zu of fiber '%s' ran %lldms "
			             "without yielding (budget %lldms)\n",
			             f._slices, f._name, static_cast<long long>(ms),
			             static_cast<long long>(
			                 std::chrono::duration_cast<std::chrono::milliseconds>(
			                     _options.watchdog_budget)
			                     .count()));
			std::abort();
		}
		LESH_LOG(log::level::warn, log::category::worker,
		         "fiber watchdog: slice %zu of fiber '%s' ran %lldms without yielding",
		         f._slices, f._name, static_cast<long long>(ms));
	}
#endif

	_current = nullptr;
	must(res, "resume", f._name);

	if (mco_status(f._co) == MCO_DEAD) {
		// Reap now rather than at scheduler destruction: the stack is half a
		// megabyte of mapping and the fiber object is four words.
		must(mco_destroy(f._co), "destroy", f._name);
		f._co = nullptr;
		f._state = fiber_state::finished;
		LESH_LOG(log::level::debug, log::category::worker, "fiber finished: %s after %zu slices", f._name, f._slices);
		return;
	}
	// `park()` already wrote `parked`; anything still `running` yielded.
	if (f._state == fiber_state::running)
		f._state = fiber_state::ready;
}

bool scheduler::tick() {
	LESH_ASSERT(_current == nullptr && "tick() is the host's, not a fiber's");

	// Snapshot first, run second. See the header: a fiber woken by an earlier
	// slice waits for the next tick, which is what makes a tick bounded and its
	// order reproducible.
	_slice_order.clear();
	for (const auto& f : _fibers)
		if (f->_state == fiber_state::ready)
			_slice_order.push_back(f.get());

	for (fiber* const f : _slice_order)
		if (f->_state == fiber_state::ready)   // an earlier slice may have parked it
			run_one_slice(*f);

	return runnable();
}

void scheduler::yield() {
	LESH_ASSERT(_current != nullptr && "yield() is called from inside a fiber");
	mco_yield(_current->_co);
}

void scheduler::park() {
	LESH_ASSERT(_current != nullptr && "park() is called from inside a fiber");
	_current->_state = fiber_state::parked;
	mco_yield(_current->_co);
	// Resumed: `wake` put us back to `ready` and `run_one_slice` to `running`.
}

void scheduler::wake(fiber& f) noexcept {
	if (f._state != fiber_state::parked)
		return;
	f._state = fiber_state::ready;
}

bool scheduler::runnable() const noexcept {
	for (const auto& f : _fibers)
		if (f->_state == fiber_state::ready)
			return true;
	return false;
}

} // namespace lesh::fiber
