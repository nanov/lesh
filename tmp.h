template<typename T, size_t StackSize>
class hybrid_vector {
	struct  storage {
		T stack[StackSize];
		T* heap = nullptr;
		T* stack_end;
		public:
			storage(): heap(nullptr), stack_end(stack + StackSize - 1) {}
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
		if constexpr (StackSize > 0) {
			T* stack_end = _storage.heap ? _storage.stack + (StackSize - 1) : _location;
			for (auto b = _storage.stack; b <= stack_end; ++b)
				b->~T();
		}
		if (_storage.heap &&
			_size > StackSize) {
				for (auto b = _storage.heap; b <= _location; ++b)
					b->~T();
			}
	}
public:
	size_t size() const { return _size; }
	void reserve_free_slots(size_t requiered_free_slots) {
		if (_capacity - _size >= requiered_free_slots)
			return;
		reserve(requiered_free_slots - (_capacity - _size));
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

	hybrid_vector() : _size(0), _capacity(StackSize) {
		_location = _storage.stack - 1;
	}

	~hybrid_vector() {
		clear();
		if (_storage.heap) {
			::operator delete[](_storage.heap);
			_storage.heap = nullptr;
		}
	}
};


