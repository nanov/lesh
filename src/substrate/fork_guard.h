#pragma once

// The fork-child discipline, enforced rather than commented (#129, the owner's
// scope note on #128's correction; architecture spec §4).
//
// TWO LAYERS, AND THIS IS THE SECOND ONE. The first layer was PARKING - #91's
// `worker_pool::park_all()` before a fork, so a child that goes on to run shell
// code (a subshell, `&` on shell code, a non-external pipeline stage) was born
// from a genuinely single-threaded moment and could allocate. #202 made that
// property STRUCTURAL: there are no helper threads left, both reactors are
// fibers on the forking thread, and a lesh process has one thread. What is left
// of the layer is `event_loop::quiesce()`, which parks the emitters group so that
// nothing computes for a line that has already run, and
// `event_loop::assert_quiesced()`, which is still the assertion every
// fork-and-continue site carries.
//
// This file is the OTHER layer, applied to the exec lanes anyway as defense in
// depth: between `fork()` and `execve()` the child touches only raw libc, stack
// buffers and strings narrowed in the parent. fish enforces that with comments
// and caller discipline ("Called in a forked child! Do not allocate memory");
// the owner asked for it enforced, so:
//
//   `fork_child_guard` asserts ZERO allocations from fork to exec, using the
//   thread_local counters #90 already put in substrate/arena.h. Debug only -
//   `LESH_ENABLE_ASSERTS` is what arms the counters in the first place, so in
//   Release this class is an empty object with no members and no cost.
//
//   `is_forked_child()` answers from ONE `pthread_atfork` child hook, and is
//   consulted ONLY by debug assertions. It is deliberately not the mechanism
//   any correctness decision rests on, because atfork handlers "may have not
//   yet run" - fish's own comment, and the reason its signal handler compares
//   `getpid()` to the main pid instead. The signal path in the host's loop does
//   the same; this flag is for `assert_no_alloc_in_child()` and nothing else.
//
// WHY A HEADER IN THE SUBSTRATE. The exec lanes are the shell's - the executor
// forks, and those lanes never enter the editor - while the quiesce-boundary
// assertion is the host's (`src/ui/loop.cpp`). Both sides need the same
// guard, and the substrate is the one layer below both. It depends on POSIX and
// on substrate/arena.h, and on nothing above itself.

#include "substrate/arena.h"
#include "substrate/assert.h"

#include <atomic>
#include <pthread.h>
#include <unistd.h>

namespace lesh {

namespace detail {

// Set by the atfork CHILD hook and never cleared: a forked child that does not
// exec never becomes a parent again in any sense this flag cares about.
inline std::atomic<bool> g_in_forked_child{false};

inline void mark_forked_child() noexcept {
	g_in_forked_child.store(true, std::memory_order_relaxed);
}

} // namespace detail

// Installs the one `pthread_atfork` child hook. Idempotent, and safe to call
// from any thread at any time - `pthread_atfork` handlers cannot be removed, so
// the guard against installing twice is a flag rather than a deregistration.
//
// The parent and prepare hooks are deliberately NOT installed. #91 settled that
// `pthread_atfork` cannot reach libc's own locks and is therefore not a
// substitute for parking; registering prepare/parent handlers here would
// suggest it was.
inline void install_fork_child_detection() noexcept {
	static const bool once = [] {
		pthread_atfork(nullptr, nullptr, &detail::mark_forked_child);
		return true;
	}();
	(void)once;
}

// True in a process that has been forked and has not exec'd.
//
// FOR DEBUG ASSERTIONS ONLY. Anything that must be right in the child - the
// signal handler's re-raise, above all - compares `getpid()` against the pid it
// recorded at startup, because that comparison cannot be early and this flag
// can (fish: "Don't use is_forked_child: it relies on atfork handlers which may
// have not yet run").
[[nodiscard]] inline bool is_forked_child() noexcept {
	return detail::g_in_forked_child.load(std::memory_order_relaxed);
}

// The assertion the spawn-without-fork paths carry: this code is NOT running in
// a forked child. The mirror of `event_loop::assert_quiesced()`, and the other
// half of the lane-aware pair the owner's scope note names -
// `assert_quiesced()` at `fork_continue` sites, this on `fork_exec` paths.
inline void assert_not_in_forked_child() noexcept {
	LESH_ASSERT(!is_forked_child());
}

// ZERO ALLOCATIONS FROM FORK TO EXEC, asserted.
//
// Constructed as the first statement in the child's arm of a `fork()`, and
// released by `exec_reached()` immediately before `execve`. Between those two
// points every allocation the process makes on this thread is counted by #90's
// thread_local counters, and the destructor asserts the count did not move.
//
// WHAT IT CAN AND CANNOT SEE. It sees arena allocations and the arena's
// fallbacks to malloc, which is what `metrics::allocation_counters` counts -
// every container leshper and the runtime build through the arena. It does NOT
// see a bare `new` or a `std::string` growing on the general heap; catching
// those needs an allocator hook, and under the sanitized gate ASan's own
// interceptors already turn most post-fork heap traffic into a deadlock or a
// report. So this is the cheap, always-on half, and the comment fish relies on
// is the expensive half made mechanical for the code that has an arena.
//
// Release builds compile it to nothing: the counters are `LESH_ENABLE_ASSERTS`
// -only, so an empty constructor and an empty destructor are all that is left,
// and the object never appears in the binary N-1 measures.
class fork_child_guard {
public:
	fork_child_guard() noexcept {
#ifdef LESH_ENABLE_ASSERTS
		const auto& counters = metrics::allocations();
		_pool_at_entry = counters.pool_allocations;
		_heap_at_entry = counters.heap_allocations;
#endif
	}

	fork_child_guard(const fork_child_guard&) = delete;
	fork_child_guard& operator=(const fork_child_guard&) = delete;

	// Called immediately before `execve`. After this the child's address space
	// is about to be replaced and the rule no longer applies, so the destructor
	// stops checking - which matters because the destructor of a guard on the
	// stack of a function that goes on to `_exit` never runs anyway.
	void exec_reached() noexcept { _checking = false; }

	// The count so far, for the test that proves the guard sees an allocation
	// rather than only for the assertion that proves it does not happen.
	[[nodiscard]] std::size_t allocations_since_entry() const noexcept {
#ifdef LESH_ENABLE_ASSERTS
		const auto& counters = metrics::allocations();
		return (counters.pool_allocations - _pool_at_entry)
		     + (counters.heap_allocations - _heap_at_entry);
#else
		return 0;
#endif
	}

	~fork_child_guard() {
		if (!_checking)
			return;
		LESH_ASSERT(allocations_since_entry() == 0);
	}

private:
	bool _checking = true;
#ifdef LESH_ENABLE_ASSERTS
	std::size_t _pool_at_entry = 0;
	std::size_t _heap_at_entry = 0;
#endif
};

} // namespace lesh
