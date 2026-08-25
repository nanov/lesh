#pragma once

#include "substrate/arena.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace lesh {

// Append-only array backed by an arena.
//
// Deliberately not a general-purpose vector. The parse tree needs exactly this
// and nothing more: append, index, iterate, and go away all at once when the
// arena rewinds. Issue #16 recommends boost::container::small_vector for the
// legacy containers on the merits, and that recommendation stands - but for the
// new front end a general small_vector is more machinery than the job requires,
// and the job is narrow enough to be provably correct in sixty lines.
//
// Growth abandons the old block when it came from the pool - the arena reclaims
// it on rewind, which is the whole point of a bump allocator, and tracking
// individual lifetimes there would defeat it.
//
// Blocks that came from the arena's HEAP FALLBACK are different, and this is the
// distinction an earlier version of this comment got wrong: the arena does not
// track those, so they are this array's to release. Under ADR-0007 the expected
// leak count is exactly zero, so "the arena will handle it" has to be true rather
// than assumed. LeakSanitizer found 13 KB across 17 blocks the first time this
// ran against a pool small enough to overflow.
template <typename T>
class arena_array {
	static_assert(std::is_trivially_destructible_v<T>,
	              "arena_array never runs destructors: the arena reclaims in one shot");
	static_assert(std::is_trivially_copyable_v<T>,
	              "growth relocates with memcpy");

public:
	explicit arena_array(buffer_pool& pool, size_t initial_capacity = 16) noexcept
		: _pool(pool) {
		reserve(initial_capacity);
	}

	arena_array(const arena_array&) = delete;
	arena_array& operator=(const arena_array&) = delete;

	// Hand-written rather than defaulted: a defaulted move would leave the source
	// holding the same pointer, and both destructors would free it.
	arena_array(arena_array&& other) noexcept
		: _pool(other._pool), _data(other._data), _size(other._size),
		  _capacity(other._capacity), _owns_block(other._owns_block) {
		other._data = nullptr;
		other._size = 0;
		other._capacity = 0;
		other._owns_block = false;
	}

	~arena_array() noexcept { release_block(); }

	uint32_t push(const T& value) noexcept {
		if (_size == _capacity)
			reserve(_capacity == 0 ? 16 : _capacity * 2);
		_data[_size] = value;
		return static_cast<uint32_t>(_size++);
	}

	// Drops everything past `keep`. Safe only because elements are trivially
	// destructible, which is a static assertion above. Used by parsers that
	// accumulate children on a scratch stack and unwind it per node.
	void truncate(size_t keep) noexcept {
		if (keep < _size)
			_size = keep;
	}

	[[nodiscard]] T& operator[](size_t i) noexcept { return _data[i]; }
	[[nodiscard]] const T& operator[](size_t i) const noexcept { return _data[i]; }

	[[nodiscard]] size_t size() const noexcept { return _size; }
	[[nodiscard]] bool empty() const noexcept { return _size == 0; }
	[[nodiscard]] size_t capacity() const noexcept { return _capacity; }
	[[nodiscard]] T* data() noexcept { return _data; }
	[[nodiscard]] const T* data() const noexcept { return _data; }

	[[nodiscard]] T* begin() noexcept { return _data; }
	[[nodiscard]] T* end() noexcept { return _data + _size; }
	[[nodiscard]] const T* begin() const noexcept { return _data; }
	[[nodiscard]] const T* end() const noexcept { return _data + _size; }

private:
	void reserve(size_t wanted) noexcept {
		if (wanted <= _capacity)
			return;
		char* raw = nullptr;
		// The arena's return value says where the block came from. Pooled blocks are
		// reclaimed on rewind; heap fallbacks are ours to free.
		const bool from_pool = _pool.allocate(wanted * sizeof(T), raw, alignof(T));
		T* fresh = reinterpret_cast<T*>(raw);
		if (_size > 0)
			std::memcpy(fresh, _data, _size * sizeof(T));
		release_block();
		_data = fresh;
		_capacity = wanted;
		_owns_block = !from_pool;
	}

	void release_block() noexcept {
		if (_owns_block && _data != nullptr)
			std::free(_data);
		_owns_block = false;
	}

	buffer_pool& _pool;
	T* _data = nullptr;
	size_t _size = 0;
	size_t _capacity = 0;
	// True when _data came from the arena's heap fallback, making it ours to free.
	bool _owns_block = false;
};

} // namespace lesh
