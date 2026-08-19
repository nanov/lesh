#pragma once

// Bump allocator. Hands out memory and reclaims it in one shot.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace lesh {

#define BUFFER_POOL_SIZE (1024*32)
#define VERBOSE_POOL_DATA

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
				result = static_cast<char *>(malloc(sizeof(char) * new_size));
				memcpy(result, at, _current - at);
				_current = at;
				return false;
			}
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
				result = static_cast<char *>(malloc(sizeof(char) * size));
				return false;
			}
#ifdef VERBOSE_POOL_DATA
			lifetime_pool_bytes_used += size;
#endif
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
