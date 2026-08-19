#pragma once

// Vector with inline capacity. NOTE: elements live across the inline buffer
// AND the heap, so data() is not contiguous - see issue #13.
#include <algorithm>
#include <cstddef>
#include <functional>
#include <new>
#include <utility>

namespace lesh {

template<typename T, size_t StackSize>
class hybrid_vector {

	struct  storage {
	private:
		alignas(T) char stack_buffer[sizeof(T) * StackSize];
	public:
		T* stack = reinterpret_cast<T*>(stack_buffer);
		T* heap = nullptr;
		T* stack_end;
		public:
			storage() : stack_buffer{}, heap(nullptr), stack_end(stack + StackSize - 1) {}
			storage(const storage &other) :
					stack_buffer{}, stack(reinterpret_cast<T *>(stack_buffer)), heap(nullptr), stack_end(stack + StackSize - 1) {}
	};
	constexpr static auto grow_by_default = std::max(StackSize, static_cast<size_t>(1));

	storage _storage;
	size_t _size = 0;
	size_t _heap_capacity = 0;
	size_t _capacity;
	T* _location;

	T* grow_heap() { return grow_heap(grow_by_default); };
	T* grow_heap(const size_t by_amount) {
		_heap_capacity += by_amount;
		auto old_store = _storage.heap;
		_storage.heap = static_cast<T*>(::operator new[](_heap_capacity * sizeof(T)));
		if (old_store) {
			if (_size > StackSize) {
				const size_t heap_elements = _size - StackSize;
				for (size_t i = 0; i < heap_elements; ++i) {
					// Placement new to move construct only the existing elements
					new (&_storage.heap[i]) T(std::move(old_store[i]));
					// Properly destruct the old elements
					old_store[i].~T();
				}
			  // Free the old raw memory
			  ::operator delete[](old_store);
			}
		}

		_capacity += by_amount;
		return  _storage.heap + (_size - StackSize);
	}

	void clear() {
		if (_size < 1)
			return;
		if (_size > StackSize) {
				if constexpr (StackSize > 0) {
					for (auto b = _storage.stack; b <= _storage.stack_end; ++b)
						b->~T();
				}
				for (auto b = _storage.heap; b <= _location; ++b)
					b->~T();
		} else {
				if constexpr (StackSize > 0) {
					for (auto b = _storage.stack; b <= _location; ++b)
						b->~T();
				}
		}
	}
public:
	size_t size() const { return _size; }
	void reserve_free_slots(size_t required_free_slots) {
		if (_capacity - _size >= required_free_slots)
			return;
		reserve(required_free_slots - (_capacity - _size));
	}
	void reserve(size_t new_cap) {
		if (new_cap <= _capacity)
			return;
		grow_heap(new_cap - _capacity);
	}

	void foreach(std::function<void(const T&)> callback) {
		if (_size == 0)
			return;
		if (_size > StackSize) {
			if constexpr (StackSize > 0) {
				auto stack_end = _storage.stack + StackSize - 1;
				for (auto s = _storage.stack; s <= stack_end; ++s)
					callback(*s);
			}
			for (auto h = _storage.heap; h <= _location; ++h)
				callback(*h);
			return;
		}
		if constexpr (StackSize > 0) {
			for (auto s = _storage.stack; s <= _location; ++s)
				callback(*s);
		}

	}

	hybrid_vector() : _size(0), _capacity(StackSize){
		_location = _storage.stack - 1;
	}

	hybrid_vector<T, StackSize>& operator=(const hybrid_vector<T, StackSize>& other) {
	// hybrid_vector(const hybrid_vector<T, StackSize>& other) : _size(other._size), _capacity(other._capacity) {
		_size = other._size;
		_capacity = other._capacity;
		_heap_capacity = other._heap_capacity;
		if (other._size < 1) {
			_location = _storage.stack - 1;
			return *this;
		}
		if (other._storage.heap)
			_storage.heap = static_cast<T*>(::operator new[](_heap_capacity * sizeof(T)));

		if (_size > StackSize) {
				if constexpr (StackSize > 0) {
					for (auto b = other._storage.stack, bc = _storage.stack; b <= other._storage.stack_end; ++b, ++bc)
						new (bc) T(*b);
				}
				for (auto b = other._storage.heap, bc = _storage.heap; b <= other._location; ++b, ++bc)
						_location = new (bc) T(*b);
		} else {
				if constexpr (StackSize > 0) {
					_location = _storage.stack - 1;
					// auto bc = _storage.stack;
					for (auto b = other._storage.stack, bc = _storage.stack; b <= other._location; ++b, ++bc)
						_location = new (bc) T(*b);
				}
		}
		return *this;


	}

	~hybrid_vector() {
		clear();
		if (_storage.heap) {
			::operator delete[](_storage.heap);
			_storage.heap = nullptr;
		}
	}

	template<size_t OtherStackSize>
	T& replace_at(const hybrid_vector<T, OtherStackSize>& other, size_t index) {
		size_t other_len = other.size() - 1;
		size_t extra_elements = other.size() > 1 ? other_len : 0;
		reserve_free_slots(extra_elements);

		if (extra_elements) {
			// Shift existing elements to make space
			for (size_t i = _size - 1; i > index; --i) {
				size_t new_pos = i + other_len;
				auto& el = get_at_reference(i);
				if (!_storage.heap || new_pos < StackSize) {
					new (&_storage.stack[new_pos]) T(std::move(el));
				} else {
					new (&_storage.heap[new_pos - StackSize]) T(std::move(el));
				}
				// we move - not sure we need destructor at all :/
				el.~T();
			}
		}

		if (!_storage.heap || index < StackSize) {
			new (&_storage.stack[index]) T(other.get_at_reference(0));
		} else {
			new (&_storage.heap[index - StackSize]) T(other.get_at_reference(0));
		}

		// Insert remaining elements
		for (size_t i = 1; i < other.size(); ++i) {
			size_t insert_pos = index + i;
			if (!_storage.heap || insert_pos < StackSize) {
				new (&_storage.stack[insert_pos]) T(other.get_at_reference(i));
			} else {
				new (&_storage.heap[insert_pos - StackSize]) T(other.get_at_reference(i));
			}
		}

		_size = _size + extra_elements;
		_location = (!_storage.heap || _size < StackSize) ?
				&_storage.stack[_size] :
				&_storage.heap[_size - StackSize];

		return get_at_reference(index+extra_elements);
	}

	template<typename... Args>
	T* place(const T& val) {
		new (_location) T(val);
		return _location;
	}

	template<typename... Args>
	T* emplace_back(Args&&... args) {
		if (_size == _capacity) {
			_location = grow_heap();
		} else if(_size == StackSize) {
			_location = _storage.heap;
		} else {
			_location += 1;
		}

		new (_location) T(std::forward<Args>(args)...);
		_size++;
		return _location;
	}

	T* push_back(const T& value) {
		if (_size == _capacity) {
			_location = grow_heap();
		} else if(_size == StackSize) {
			_location = _storage.heap;
		} else {
			_location += 1;
		}

		new (_location) T(value);
		_size++;
		return _location;
	}
	// T& push_back(T&& value) { return emplace_back(std::move(value)); }

	void pop_back() {
		if (_size < 1)
			return;

		_size--;
		// destroy element
		_location.~T();

		// is size is exactly the stack size, it means we need to downgrade from heap location to stack one
		if constexpr (StackSize > 0) {
			if (_size == StackSize) {
				_location = _storage.stack + StackSize -1;
				return;
			}
		}

		_location -= 1;
	}

	[[nodiscard]] T& get_at_reference(size_t idx) const {
		return  (!_storage.heap || idx < StackSize) ?  const_cast<T&>(_storage.stack[idx]) : _storage.heap[idx - StackSize];
	}

	[[nodiscard]] T* get_at(size_t idx) const {
		return  (!_storage.heap || idx < StackSize) ? const_cast<T*>(&_storage.stack[idx]) : &_storage.heap[idx - StackSize];
	}

	T* operator [](size_t idx) const {return get_at(idx);}
	bool operator==(const hybrid_vector<T, StackSize>& other) const {
		if (_size != other._size)
			return false;
		for (size_t i = 0; i < _size; ++i)
			if (get_at_reference(i) != other.get_at_reference(i))
				return false;
		return true;
	}

	[[nodiscard]] size_t capacity() const { return _capacity; }
	[[nodiscard]] bool is_on_heap() const { return _storage.heap; }
};

// used for command parts (ex: ls -l  => [ls] [-l] )

} // namespace lesh
