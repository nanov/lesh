#pragma once

// Bump allocator. Hands out memory and reclaims it in one shot.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace lesh {

#define BUFFER_POOL_SIZE (1024*32)
#define VERBOSE_POOL_DATA

// Allocation counters, compiled in outside Release.
//
// Counted rather than timed, deliberately: a count is deterministic, so it can be
// asserted rather than eyeballed, and it needs no quiet machine. The binding
// constraint is "near-zero allocation on the command path", and an allocation
// count is that constraint stated as a number.
//
// A heap_allocations increase is the one that matters - it means the arena
// overflowed and fell back to malloc, which is the failure this design exists to
// avoid.
namespace metrics {

struct allocation_counters {
	size_t pool_allocations = 0;   // served from arena storage
	size_t heap_allocations = 0;   // fell back to malloc: the number to watch
	size_t bytes_from_pool = 0;
	size_t bytes_from_heap = 0;

	void reset() noexcept { *this = {}; }
};

// ONE INSTANCE PER THREAD (#90). lesh has threads now - the host's workers, each
// with an arena of its own (#126) - and the alternative to thread_local was an
// atomic, which would cost more than the counter measures.
//
// Per-thread is also the only shape that keeps the gate meaning what
// tests/unit/allocation_tests.cpp asserts it means: those tests count the
// COMMAND PATH's allocations on the thread running them, and a worker parsing a
// snapshot at the same instant would otherwise land in the same number. A
// worker's own counts are still there, on the worker's own thread, for anyone
// who wants to assert about a worker.
//
// Compiled out of Release along with the macros below, so the cost is a
// debug-build one.
inline allocation_counters& allocations() noexcept {
	static thread_local allocation_counters counters;
	return counters;
}

} // namespace metrics

#ifdef LESH_ENABLE_ASSERTS
#define LESH_COUNT_POOL_ALLOC(bytes)                                           \
	do {                                                                       \
		lesh::metrics::allocations().pool_allocations++;                       \
		lesh::metrics::allocations().bytes_from_pool += (bytes);               \
	} while (0)
#define LESH_COUNT_HEAP_ALLOC(bytes)                                           \
	do {                                                                       \
		lesh::metrics::allocations().heap_allocations++;                       \
		lesh::metrics::allocations().bytes_from_heap += (bytes);               \
	} while (0)
#else
#define LESH_COUNT_POOL_ALLOC(bytes) ((void)0)
#define LESH_COUNT_HEAP_ALLOC(bytes) ((void)0)
#endif

class buffer_pool {
	public:
		explicit buffer_pool(size_t size = 1024) : _data(new char[size]) {
			_end = _data + size;
			_current = _data;
		}

		~buffer_pool() {
			delete[] _data;
		}

		bool reallocate(char* at, size_t new_size, char*& result) {
			if (at+new_size > _end) {
#ifdef VERBOSE_POOL_DATA
				lifetime_pool_bytes_allocated += new_size;
#endif
				LESH_COUNT_HEAP_ALLOC(new_size);
				result = static_cast<char *>(malloc(sizeof(char) * new_size));
				memcpy(result, at, _current - at);
				_current = at;
				return false;
			}
			LESH_COUNT_POOL_ALLOC(new_size);
			_current = at + new_size;
			result = at;
#ifdef VERBOSE_POOL_DATA
				lifetime_pool_bytes_allocated += _current - at;
#endif
			return true;
		}
		// Bump-allocates `size` bytes aligned to `alignment`.
		//
		// Alignment defaults to the strictest the platform requires, because an
		// allocator that returns suitably-aligned memory by default is the only
		// kind that is safe to use generically. Without it this returned raw bump
		// positions, which is fine for byte buffers and undefined for everything
		// else - UBSan caught a token landing on an odd address. Callers that
		// genuinely want packed bytes can ask for alignment 1.
		bool allocate(size_t size, char*& result, size_t alignment = alignof(std::max_align_t)) {
			const auto misalign = reinterpret_cast<uintptr_t>(_current) % alignment;
			if (misalign != 0)
				_current += alignment - misalign;
			if (_current+size > _end) {
#ifdef VERBOSE_POOL_DATA
				lifetime_pool_bytes_allocated += size;
#endif
				// malloc, not new char[]: returning false means "this came from the
				// heap, the caller owns it", and every such caller releases it with
				// free() or grows it with realloc(). Allocating with new[] here made
				// both of those undefined. reallocate()'s overflow path below already
				// uses malloc; this keeps one family behind one protocol.
				LESH_COUNT_HEAP_ALLOC(size);
				result = static_cast<char *>(malloc(sizeof(char) * size));
				return false;
			}
#ifdef VERBOSE_POOL_DATA
			lifetime_pool_bytes_used += size;
#endif
			LESH_COUNT_POOL_ALLOC(size);
			result = _current;
			_current += size;
			return true;
		}
		void reset(char* to) {
			_current = to;
		}

		char* at() const { return _current; }

#ifdef VERBOSE_POOL_DATA
	  size_t lifetime_pool_bytes_used;
		size_t lifetime_pool_bytes_allocated;
		[[nodiscard]] size_t bytes_used() const {
			return reinterpret_cast<size_t>(_current) - reinterpret_cast<size_t>(_data);
		}
		[[nodiscard]] size_t pool_size() const {
			return reinterpret_cast<size_t>(_end) - reinterpret_cast<size_t>(_data);
		}
#endif
	private:
		char* _data;
		char* _current;
		const char* _end;
};

} // namespace lesh
