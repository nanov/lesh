#pragma once

// Contiguous vector with inline capacity, spilling to the heap when it grows.
#include <cstddef>
#include <new>
#include <utility>

namespace lesh {

template<typename T, size_t StackSize>
class hybrid_continuous_simple_vector {
	alignas(T) char _stack_buffer[sizeof(T) * StackSize];
	T* _storage;

	// storage _storage;
	size_t _size;
	size_t _capacity;
	T* _location;
	bool _using_heap;


	void grow_heap(size_t by_size = StackSize) {
		_capacity += by_size;
		auto old_storage = _storage;
		_storage = static_cast<T*>(::operator new[](_capacity * sizeof(T)));
		for (size_t i = 0; i < _size; ++i) {
			new (&_storage[i]) T(std::move(old_storage[i]));
			old_storage[i].~T();
		}

		if (_using_heap)
			  ::operator delete[](old_storage);
		_using_heap = true;
		_location = _storage + _size;
	}
	// NOTE: there is deliberately no clear() that runs destructors - elements are
	// assumed trivially destructible. That assumption has never been verified;
	// the substrate hardening phase either enforces it with a static_assert or
	// makes destructors run.

public:
	T* data() {
		return _storage; //  _using_heap ? _storage.heap : _storage.stack;
	}

	hybrid_continuous_simple_vector() :
			_stack_buffer{},
			_storage(reinterpret_cast<T *>(_stack_buffer)),
			_size(0),
	    _capacity(StackSize),
			_location(_storage), _using_heap(false) {}

	hybrid_continuous_simple_vector(const hybrid_continuous_simple_vector& other):
			_stack_buffer{},
			// _storage(reinterpret_cast<T *>(_stack_buffer)),
			_size(other._size),
	    _capacity(other._capacity),
			_using_heap(other._using_heap)
	{
		if (_using_heap)
			_storage = static_cast<T*>(::operator new[](_capacity * sizeof(T)));
		else
			_storage = reinterpret_cast<T *>(_stack_buffer);
		for (size_t i = 0; i < _size; ++i)
			new (&_storage[i]) T(other._storage[i]);
		_location = _storage + _size;
	};

	~hybrid_continuous_simple_vector() {
		// clear();
		if (_using_heap) { ::operator delete[](_storage); }
	}

	template<typename... Args>
	T& emplace_back(Args&&... args) {
		if (_size == _capacity)
			grow_heap();

		new (_location) T(std::forward<Args>(args)...);
		_size++;
		return *_location++;
	}

	[[nodiscard]] bool is_full() const { return _size == _capacity; }

	T& next_unsafe() {
		return *(_location);
	}

	template<typename... Args>
	T& temporary_emplace_once_back(Args&&... args) {
		if (_size == _capacity)
			grow_heap(1);

		new (_location) T(std::forward<Args>(args)...);
		return *_location++;
	}

	// copy
	void push_back(const T& value) {
		if (_size == _capacity)
			grow_heap();
		new (_location) T(value);
		_size++;
	}


	// move
	void push_back(T&& value) {
		if (_size == _capacity)
			grow_heap();
		new (_location) T(std::move(value));
		_size++;
	}

	void replace_front(const T * addition, size_t addition_size) {
		size_t extra_elements = addition_size > 1 ? addition_size - 1 : 0;

		// Check if we need to grow
		if (_size + extra_elements > _capacity)
			grow_heap();

		T* storage = _storage; //_using_heap ? _storage.heap : _storage.stack;
		const T* other_storage = addition;

		if (extra_elements > 0) {
			for (size_t i = _size - 1; i >= 1; --i) {
				new (storage + i + extra_elements) T(std::move(storage[i]));
				storage[i].~T();
			}
		}

		for (size_t i = 0; i < addition_size; ++i) {
			if (i < _size) {
				storage[i].~T();
			}
			new (storage + i) T(other_storage[i]);
		}

		_size += extra_elements;
		_location = _storage + _size; //  : _storage.stack + _size;
	}



	[[nodiscard]] const T* data() const { return _storage; }

	[[nodiscard]] const T* get_at(const size_t idx) const { return ( _storage + idx ); }
	[[nodiscard]] const T* operator [](const size_t idx) const {return get_at(idx);}


	[[nodiscard]] size_t size() const { return _size; }
	[[nodiscard]] size_t capacity() const { return _capacity; }
	[[nodiscard]] bool is_on_heap() const { return _using_heap; }
	bool operator==(const hybrid_continuous_simple_vector& other) const {
		if (_size != other._size) return false;
		for (size_t i = 0; i < _size; ++i)
			if (get_at(i) != other.get_at(i))
				return false;
		return true;
	}
};

} // namespace lesh
