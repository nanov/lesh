#include "fiber/scheduler.h"

#include "minicoro.h"
#include "substrate/assert.h"
#include "substrate/log.h"

#include <algorithm>
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

// ---------------------------------------------------------------------------
// ASan AND THE STACK THE HOST COMES BACK TO (#202)
// ---------------------------------------------------------------------------
//
// A DEFECT IN THE VENDORED LIBRARY'S ASan INTEGRATION, worked around from this
// side rather than by patching `minicoro.h` - #198 chose to keep that file
// `curl | shasum`-verifiable, and this is exactly the kind of thing that keeps
// being worth what it costs.
//
// The shape of it. ASan tracks ONE current stack per thread, told to it by
// `__sanitizer_start_switch_fiber` (announce, on the old stack) and
// `__sanitizer_finish_switch_fiber` (commit, on the new one). minicoro's
// `_mco_prepare_jumpin` announces the switch INTO the coroutine and
// `_mco_prepare_jumpout` commits it - and then announces the switch BACK only
// `if(prev_co)`, i.e. only when the thing being returned to is another
// coroutine. When a coroutine yields to a plain function - which under #145's
// pinned rule ("no fiber call stack": the host is the sole resumer and every
// yield returns to the host) is EVERY yield lesh takes - nothing tells ASan the
// thread is back on its own stack. Its recorded bounds stay the fiber's from the
// first yield onward.
//
// What that costs, and why it is not cosmetic: `__asan_handle_no_return` - which
// clang emits before every `noreturn` call, so before every `_exit` in a forked
// child, every `abort` and every throw - unpoisons the stack from the current
// frame to the recorded top. With the bounds wrong it declines, printing
// "ASan is ignoring requested __asan_handle_no_return ... False positive error
// reports may follow", and the poison it did not clear is a use-after-scope
// report waiting for whoever next reuses those bytes. `UiReactorFiber`'s fork
// test is where it showed up first, because a forked child is a `_exit`.
//
// The fix is one round trip after every resume: announce a switch back to the
// host's own bounds and commit it. The fake stack is saved and restored across
// the pair (`detect_stack_use_after_return` keeps one there when it is on), so
// this corrects the bounds and touches nothing else. Compiled out entirely
// without ASan, and it is deliberately the same feature test `minicoro.h` uses -
// two different answers to "is this an ASan build" would be worse than none.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LESH_FIBER_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define LESH_FIBER_ASAN 1
#endif

#if LESH_FIBER_ASAN
extern "C" void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom,
                                               std::size_t size);
extern "C" void __sanitizer_finish_switch_fiber(void* fake_stack_save,
                                                const void** bottom_old,
                                                std::size_t* size_old);

// What ASan currently believes this thread's stack is - asked of ASan itself
// rather than computed from `pthread_get_stackaddr_np`, so there is one source of
// truth and no second platform table to drift.
//
// The mechanism is the round trip: `start` records a pending switch and `finish`
// commits it AND reports the bounds it is leaving. So a pair that announces a
// switch to a throwaway region and immediately commits it hands back the bounds
// we wanted to read, and the second pair puts them straight back. Called once per
// scheduler, BEFORE any fiber has run, which is the one moment ASan's answer is
// certainly the host's.
void read_host_stack(const void*& bottom, std::size_t& size) noexcept {
	void* saved = nullptr;
	const void* asked = nullptr;
	std::size_t asked_size = 0;
	char here = 0;
	__sanitizer_start_switch_fiber(&saved, &here, sizeof(here));
	__sanitizer_finish_switch_fiber(saved, &asked, &asked_size);
	bottom = asked;
	size = asked_size;
	// And back, before anything can touch the stack ASan now thinks is one byte
	// wide.
	saved = nullptr;
	__sanitizer_start_switch_fiber(&saved, bottom, size);
	__sanitizer_finish_switch_fiber(saved, nullptr, nullptr);
}

// ASan is on the host's stack again. Idempotent, and safe whether or not the
// bounds were already right.
void back_on_the_host_stack(const void* bottom, std::size_t size) noexcept {
	if (bottom == nullptr || size == 0)
		return;
	void* saved = nullptr;
	__sanitizer_start_switch_fiber(&saved, bottom, size);
	__sanitizer_finish_switch_fiber(saved, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// LeakSanitizer AND A PARKED FIBER'S STACK (#202)
// ---------------------------------------------------------------------------
//
// A FIBER'S STACK IS A LEAK-SCAN ROOT, AND IT HAS TO BE SAID OUT LOUD.
//
// LeakSanitizer's root set is the thread stacks, the registers, the globals and
// the TLS. A fiber's stack is none of those: it is an anonymous `mmap` this file
// made, and nothing in LSan knows a coroutine exists. So a heap block whose only
// pointer is a local in a parked fiber's frame - which is EVERY reactor's
// snapshot buffer while it is mid-compute, since `run_reactor_here` moves the
// string into the token - is unreachable as far as LSan can tell, and reported.
//
// #198 CONCLUDED THE OPPOSITE, and the reason it looked that way is the ASan
// defect above. `FiberLsan.ABlockHeldOnlyByAParkedFiberStackIsNotReported`
// passed on Darwin because minicoro left ASan's record of the THREAD's stack
// pointing at the fiber's stack after the yield - so the leak check scanned the
// fiber stack as if it were the thread's, and found the block. Correcting the
// bounds so that `__asan_handle_no_return` works removed that accident, the
// negative control went red, and what it had been proving was that a bug was
// still there. The research note called this "the single most likely way fibers
// break the gate on CI", and it was right about the shape and wrong about only
// the platform.
//
// The fix is the documented interface, not a suppression: every live fiber stack
// is registered as an LSan ROOT REGION, so blocks held from it are TRACED - which
// keeps them, and everything they in turn point at, honestly reachable. An
// `__lsan_ignore_object` on the block would have hidden one allocation and left
// its graph unreachable; a suppression would have hidden the whole class. And the
// registration is UNDONE when the stack is unmapped, which is what keeps
// `FiberLsanPositiveControl` meaningful: a scheduler destroyed with a fiber still
// parked really does lose whatever that stack owned, and LSan must still say so.
//
// Whole-stack rather than up-to-the-stack-pointer, which is what ASan itself does
// for every thread but the current one: the cost is that a pointer in a dead
// frame keeps a block alive, so a leak can be missed. The alternative is reading
// a suspended coroutine's saved stack pointer out of the vendored struct, which
// would be a second thing to re-verify at every re-vendor for a sharpness the
// gate does not need.
extern "C" void __lsan_register_root_region(const void* begin, std::size_t size);
extern "C" void __lsan_unregister_root_region(const void* begin, std::size_t size);

void watch_stack_for_leaks(const stack_extents& where, bool watching) noexcept {
	if (where.stack_base == nullptr || where.stack_size == 0)
		return;
	if (watching)
		__lsan_register_root_region(where.stack_base, where.stack_size);
	else
		__lsan_unregister_root_region(where.stack_base, where.stack_size);
}
#else
void watch_stack_for_leaks(const stack_extents&, bool) noexcept {}
#endif

} // namespace

// ---------------------------------------------------------------------------
// fiber
// ---------------------------------------------------------------------------

fiber::fiber(scheduler& on, entry_fn fn, void* userdata, const char* name,
             std::uint8_t group) noexcept
	: _sched(&on), _fn(fn), _userdata(userdata), _name(name != nullptr ? name : ""),
	  _group(group) {}

fiber::~fiber() {
	if (_co == nullptr)
		return;
	// The stack stops being a leak-scan root the moment it stops existing. See the
	// LeakSanitizer note above: this is what keeps a fiber destroyed mid-compute a
	// REPORTED leak rather than a silently traced one.
	watch_stack_for_leaks(extents_of(*_co), false);
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

fiber& scheduler::spawn(entry_fn fn, void* userdata, const char* name, std::uint8_t group,
                        std::size_t stack_bytes) {
	LESH_ASSERT(fn != nullptr);
	LESH_ASSERT(group < group_count && "up to 8 groups: the park set is one byte");

	auto born = std::unique_ptr<fiber>(new fiber(*this, fn, userdata, name, group));
	born->_ready_at = _next_ready_at++;

	// The per-spawn override, or the scheduler's default when it is 0. Rounded to
	// a page and given its guard by `install_guarded_allocator` either way.
	mco_desc desc = mco_desc_init(&fiber::trampoline,
	                              stack_bytes != 0 ? stack_bytes : _options.stack_bytes);
	desc.user_data = born.get();
	install_guarded_allocator(desc);

	mco_coro* co = nullptr;
	must(mco_create(&co, &desc), "create", born->_name);
	verify_guard_placement(*co);
	// This stack is a leak-scan root for as long as it exists (see the
	// LeakSanitizer note above). Registered before the first slice, because a fiber
	// can allocate on its very first one.
	watch_stack_for_leaks(extents_of(*co), true);
	born->_co = co;

	LESH_LOG(log::level::debug, log::category::worker,
	         "fiber spawned: %s group=%u stack=%zu guard=%zu", born->_name,
	         static_cast<unsigned>(group), co->stack_size, page_size());

	_fibers.push_back(std::move(born));
	// Deduplicated and at most one entry per fiber, so reserving here is what
	// lets `wake` queue without allocating and stay `noexcept`.
	_pending_wakes.reserve(_fibers.size());
	return *_fibers.back();
}

void scheduler::run_one_slice(fiber& f) {
	LESH_ASSERT(_current == nullptr && "slices do not nest: park, do not recurse");
	LESH_ASSERT(!group_parked(f._group) &&
	            "a fiber in a parked group is not runnable: resume the group first");
	if (f._state == fiber_state::finished)
		return;

	_current = &f;
	f._state = fiber_state::running;
	++f._slices;

#ifndef NDEBUG
	const auto started = std::chrono::steady_clock::now();
#endif

#if LESH_FIBER_ASAN
	if (_host_stack_bottom == nullptr)
		read_host_stack(_host_stack_bottom, _host_stack_size);
#endif

	const mco_result res = mco_resume(f._co);

#if LESH_FIBER_ASAN
	// FIRST THING AFTER THE RESUME, before the watchdog's clock read and before
	// anything else touches this stack: see the ASan note at the top of this file.
	back_on_the_host_stack(_host_stack_bottom, _host_stack_size);
#endif

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
		watch_stack_for_leaks(extents_of(*f._co), false);
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

bool scheduler::tick() { return tick(all_groups); }

bool scheduler::tick(std::uint8_t group_mask) {
	LESH_ASSERT(_current == nullptr && "tick() is the host's, not a fiber's");

	// Snapshot first, run second. See the header: a fiber woken by an earlier
	// slice waits for the next tick, which is what makes a tick bounded and its
	// order reproducible.
	_slice_order.clear();
	for (const auto& f : _fibers)
		if (is_runnable(*f, group_mask))
			_slice_order.push_back(f.get());

	// Arrival order, not spawn order - and the two coincide for every fiber that
	// has never parked. `_ready_at` is unique per stamp, so this is a strict
	// total order and an unstable sort is enough; nothing here allocates.
	std::sort(_slice_order.begin(), _slice_order.end(),
	          [](const fiber* a, const fiber* b) noexcept { return a->_ready_at < b->_ready_at; });

	// Re-checked, not assumed: an earlier slice may have parked this fiber or
	// parked its whole group. Decision 2 in the header - the snapshot bounds
	// which fibers may run and in what order, never that they will.
	for (fiber* const f : _slice_order)
		if (is_runnable(*f, group_mask))
			run_one_slice(*f);

	return runnable(group_mask);
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

	if (group_parked(f._group)) {
		// Queued, not applied. Idempotent, and the dedup is what keeps the first
		// arrival's position: `slot::send` may fire many times while a group is
		// parked and the record's promise is about ARRIVAL, not about the last
		// mention.
		for (const fiber* const queued : _pending_wakes)
			if (queued == &f)
				return;
		_pending_wakes.push_back(&f);
		return;
	}

	f._state = fiber_state::ready;
	f._ready_at = _next_ready_at++;
}

void scheduler::park_group(std::uint8_t group) {
	LESH_ASSERT(group < group_count && "up to 8 groups: the park set is one byte");
	LESH_ASSERT((_current == nullptr || _current->_group != group) &&
	            "a fiber cannot park the group it is running in");
	_parked_groups = static_cast<std::uint8_t>(_parked_groups | group_mask_of(group));
}

std::size_t scheduler::resume_group(std::uint8_t group) {
	LESH_ASSERT(group < group_count && "up to 8 groups: the park set is one byte");
	_parked_groups = static_cast<std::uint8_t>(_parked_groups & ~group_mask_of(group));

	// Front to back, so each `wake` below stamps a larger arrival than the last
	// and the next tick's sort hands out slices in exactly this order. Everything
	// belonging to another group keeps its place in the queue, and its relative
	// order, for its own resume.
	std::size_t replayed = 0;
	auto keep = _pending_wakes.begin();
	for (auto seen = _pending_wakes.begin(); seen != _pending_wakes.end(); ++seen) {
		if ((*seen)->_group == group) {
			wake(**seen);
			++replayed;
			continue;
		}
		*keep++ = *seen;
	}
	_pending_wakes.erase(keep, _pending_wakes.end());

	if (replayed != 0)
		LESH_LOG(log::level::debug, log::category::worker,
		         "fiber group %u resumed: %zu queued wakes replayed",
		         static_cast<unsigned>(group), replayed);
	return replayed;
}

bool scheduler::group_parked(std::uint8_t group) const noexcept {
	return (_parked_groups & group_mask_of(group)) != 0;
}

std::size_t scheduler::queued_wakes(std::uint8_t group) const noexcept {
	std::size_t n = 0;
	for (const fiber* const queued : _pending_wakes)
		if (queued->_group == group)
			++n;
	return n;
}

bool scheduler::is_runnable(const fiber& f, std::uint8_t group_mask) const noexcept {
	return f._state == fiber_state::ready && (group_mask & group_mask_of(f._group)) != 0 &&
	       !group_parked(f._group);
}

bool scheduler::runnable() const noexcept { return runnable(all_groups); }

bool scheduler::runnable(std::uint8_t group_mask) const noexcept {
	for (const auto& f : _fibers)
		if (is_runnable(*f, group_mask))
			return true;
	return false;
}

} // namespace lesh::fiber
