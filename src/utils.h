#pragma once

#include <__ranges/split_view.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <sol/state.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define BUFFER_POOL_SIZE (1024*32)
#define SUBSHELL_BUFFER_INITIAL_SIZE 1024

#define VERBOSE_POOL_DATA

// begin - alias shit double hash == https://www.reddit.com/r/cpp_questions/comments/12xw3sn/find_stdstring_view_in_unordered_map_with/ == https://godbolt.org/z/789xv8Eeq
template<typename ... Bases>
struct overload : Bases ...{
    using is_transparent = void;
    using Bases::operator() ... ;
};
struct char_pointer_hash
{
	auto operator()( const char* ptr ) const noexcept
	{
		return std::hash<std::string_view>{}( ptr );
	}
};
using transparent_string_hash = overload<
    std::hash<std::string>,
    std::hash<std::string_view>,
    char_pointer_hash
>;
struct char_iteratable {
private:
	char** _stack;
	char** _end;
public:
	char_iteratable(char** self, char** end): _stack(self), _end(end) {}
	struct iterator {
	private:
		char** _location;
	public:
		explicit iterator(char** begin) : _location(begin) {}

		char*& operator*() const { return *_location; }

		iterator& operator++() {
			++_location;
			return *this;
		}

		bool operator!=(const iterator& other) const { return _location != other._location; }
	};

	iterator begin() { return iterator(this->_stack); }
	iterator end() { return iterator(this->_end);}
};

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
	/*
	void clear() {
		/* it's simple no need to iterate and call, it is belived emelents won't have destructor
		auto storage = _storage;
		for (size_t i = 0; i < _size; ++i, ++storage)
			storage->~T();
	}
	*/

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
struct ASTWord {
    // null-terminated, not-owned
    const char *value;
		explicit ASTWord(const char* inval): value(inval) {
		}
		ASTWord(const ASTWord& other): value(other.value) {}
		ASTWord(const ASTWord&& other) noexcept : value(other.value) {
			printf("move ASTWord::ASTWord()\n");
		}
		~ASTWord() {
			printf("~ASTWord::ASTWord()\n");
		}
};
struct ASTPipe;
//
struct ASTCommandSubstitution {
	char *before;
	char *after;
	char *command;
	const ASTPipe* pipe;
};
struct ASTAssigment {
	const char *key;
	const char *value;
};

static size_t command_id=0;

// command is a part exectued by itself, it conatins a colletions of wrods whics are it's arhuments ( ls -l -gAH )
struct ASTCommand {
	static constexpr size_t MAX_CHILDREN = 32;
	static constexpr size_t INITIAL_ASSIGNMENTS = 0;

	// those are essentially parameters
	hybrid_continuous_simple_vector<ASTWord, MAX_CHILDREN> children;
	hybrid_vector<ASTAssigment, INITIAL_ASSIGNMENTS> assignments;

	size_t _id;
	bool is_sealed = false;

	ASTCommand(): _id(command_id++) {
		printf("command const called %d\n", _id);
	}

	// copy
	ASTCommand(const ASTCommand& other):
	children(other.children),
	assignments(other.assignments),
	_id(command_id++) {
		printf("copy ASTCommand::ASTCommand() %lu -> %lu\n", other._id, _id);
	}
	// move
	ASTCommand(const ASTCommand&& other) noexcept : children(other.children), assignments(other.assignments), _id(other._id) {
		printf("move ASTCommand::ASTCommand()\n");
	}

	~ASTCommand() {
		printf("command dest called %d\n", _id);
	}

	char** args_null_terminated() {
		if (children.is_full() || children.next_unsafe().value != nullptr)
			children.temporary_emplace_once_back(nullptr);
		return  reinterpret_cast<char**>(children.data());
	}

	ASTWord& emplace_child(const char* value) { return children.emplace_back(value); }
	void push_child(ASTWord&& value) { children.push_back(value); }

	[[nodiscard]] size_t number_of_children() const { return children.size(); }
	[[nodiscard]] size_t size() const { return number_of_children(); }

		void add_assignment(const char* key, const char* value) {
	    assignments.emplace_back(key, value);
    }

    void expand_with(ASTCommand& other) {
        children.replace_front(other.children.data(), other.children.size());
    }

    void enrich_wth(const ASTCommand & other) {
        for (size_t i = 1; i < other.children.size(); ++i)
            children.push_back(*other.children[i]);
    }

    void print() const {
        std::cout << '[' << children.size() << "]: ";
        for (size_t i = 0; i < children.size(); ++i) {
            std::cout << ((*children[i]->value == '\0') ? "0" : children[i]->value) << ' ';
        }
        std::cout << std::endl;
    }
};
// a pipe is a collection/conatiner of commands which needs to be executed separate or by itslef and connected somehow ( pipe, redirect )
// essentially pipe is the main execution unit
//             PIPE
//              |
//           /     \
//     Command    Command
//      | | |      | | |
//      W W W      W W W
struct ASTPipe {
    hybrid_vector<ASTCommand, 1> commands;

    ASTPipe() : commands() {}
    ASTPipe(const ASTPipe& other) {
        commands = other.commands;
    }

    void print() const {
        for (size_t i = 0; i < commands.size(); ++i) {
            const auto c = commands[i];
            c->print();
        }
    }

    template<typename... Args>
    ASTCommand* emplace_command(Args&&... args) { return commands.emplace_back(std::forward<Args>(args)...); }

    ASTCommand* expand_with_at(const ASTPipe& other, size_t idx) {
        auto first_command = commands[idx];
        auto& last_command = commands.replace_at(other.commands, idx);
        last_command.enrich_wth(last_command);
        return &last_command;
    }

    ASTCommand* merge(const ASTPipe * other) {
    		ASTCommand* result = nullptr;
        result = commands.place(other->commands.get_at_reference(0));

        for (size_t i = 1; i < other->size(); ++i) {
            auto& cmd = other->commands.get_at_reference(i);
            result = commands.push_back(cmd);
        }
        return result;
    }


    [[nodiscard]] size_t size() const {
    	if (commands.size() == 1)
    		return commands[0]->children.size() ? 1 : 0;
    	return commands.size();
    }
};
struct string_part {
private:
	char* _data = nullptr;
	size_t _size = 0;
public:
	string_part(char* data, size_t size) : _data(data), _size(size) {}
};
class alias_container {
private:
	bool normalized = true;
	struct alias {
		ASTPipe original;
		ASTPipe expanded;
	};
	std::unordered_map<std::string, alias> _aliases;


public:
	alias_container() : _aliases() {}
	bool try_get_alias(const char * command, ASTPipe const ** alias) const {
		if (const auto it = _aliases.find(command); it != _aliases.end()) {
			*alias = &it->second.expanded;
			return true;
		}
		return false;
	}

	ASTPipe& emplace_alias(const char* alias_key) {
		alias a;
		auto [it, op] = _aliases.insert_or_assign(alias_key, a);
		normalized = false;
		return it->second.original;
	}
	void emplace_alias_o(const char* alias, const ASTPipe & pipe) {
		auto p = pipe;
		_aliases.emplace(alias, p);
		normalized = false;
	}

	void normalize_aliases() {
		if (normalized)
			return;
		std::unordered_set<std::string> expaded_alises;
		std::unordered_map<std::string, alias>::iterator expanded;

		for (auto defined_alias = _aliases.begin(); defined_alias != _aliases.end(); ++defined_alias) {
			defined_alias->second.expanded = defined_alias->second.original;

			size_t cmd_idx = 0;
			do {
				expaded_alises.clear();
				auto command = defined_alias->second.expanded.commands[cmd_idx];
				auto command_str = command->children[0]->value;
				expaded_alises.emplace(defined_alias->first);
				while (
					!expaded_alises.contains(command_str)
					&& ((expanded = _aliases.find(command_str)) != _aliases.end())
					) {
					if (expanded->second.original.size() == 1) {
						command->expand_with(*expanded->second.original.commands[0]);
					} else {
						command = defined_alias->second.expanded.expand_with_at(expanded->second.original, cmd_idx);
						// cmd_idx += expanded->second.original.size();
						// todo
					}
					expaded_alises.emplace(expanded->first);
					command_str = command->children[0]->value;
					}
				cmd_idx++;
			} while (cmd_idx < defined_alias->second.expanded.size());
		}

	}
};

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
		bool allocate(size_t size, char*& result) {
			if (_current+size > _end) {
#ifdef VERBOSE_POOL_DATA
				lifetime_pool_bytes_allocated += size;
#endif
				result = new char[size];
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

class lesh_scope_variable {
	public:
	virtual ~lesh_scope_variable() = default;
	virtual const char* value(const std::string_view& key) const = 0;
		virtual void set(const std::string_view& key, std::string_view& value) = 0;
};
class lesh_scope_map_variable: public lesh_scope_variable {
private:
	std::unordered_map<std::string, const char*, transparent_string_hash, std::equal_to<>> _data;
public:
	~lesh_scope_map_variable() override {
		for (const auto &it : _data)
			free(const_cast<char*>(it.second));
	}
	const char* value(const std::string_view& key) const override {
		if (const auto &it = _data.find(key); it != _data.end()) {
			if (it->second == nullptr)
				return "";
			return it->second;
		}
		return "";
	}
	void set(const std::string_view& key, std::string_view& value) override {
		_data[std::string(key)] = strdup(value.data());
	}
	size_t size() const { return _data.size(); }
};
class lesh_scope_array_variable: public lesh_scope_variable {
private:
	std::vector<const char *> _data;
	bool try_get_size(const char* t, size_t& res) const {
		char* p = const_cast<char*>(t);
		char c = *p;
		size_t i = 0;
		res = 0;
		while (c != '\0') {
			if (!isdigit(c))
				return false;
			res += (c - '0')*(i++);
			c = *++p;
		}
		return true;
	}
public:
	lesh_scope_array_variable() : lesh_scope_variable() {}
	void push(const char* value) {
		_data.push_back(value);
	}

	const char* value(const std::string_view& key) const override {
		size_t i = 0;
		if (!try_get_size(key.data(), i))
			return "";
		if (i >= _data.size())
			return "";
		const auto v = _data[i];
		if (v == nullptr)
			return "";
		return v;
	}
	void set(const std::string_view& key, std::string_view& value) override {
		size_t i = 0;
		if (!try_get_size(key.data(), i))
			return;
		if (i >= _data.capacity())
			_data.reserve(i+1);
		if (i >= _data.size())
			for (auto c = i; c < _data.size(); c++)
				_data.emplace_back(nullptr);
		_data[i] = const_cast<char*>(value.data());
	}
	size_t size() const { return _data.size(); }
};
class lesh_state_scope {
private:
		const lesh_state_scope* _parent;
		const lesh_state_scope* _exporting_parent;
		const bool _in_export_mode = false;
		std::unordered_map<const std::string, std::unique_ptr<lesh_scope_variable>, transparent_string_hash, std::equal_to<>> _vars;

public:
	lesh_state_scope(lesh_state_scope* parent, lesh_state_scope* exporting_parent) : _parent(parent), _exporting_parent(exporting_parent) {}


	bool try_get_var_value(const std::string_view& var, const std::string_view& key, std::string_view& val) const {
		auto s = this;
		while (s) {
			if (const auto &it = s->_vars.find(var); it != s->_vars.end()) {
				val = it->second->value(key);
				std::println("found {} in {} is {}", key, var, val);
				return true;
			}
			s = s->_parent;
		}
		return false;
	}

	bool get_map_values(char* v, size_t len, std::function<void(std::string_view, std::string_view)> callback) const {
		size_t i = 0;
		char* c = const_cast<char*>(v);
		char* lw = c;
		size_t wl = 0;
		std::string_view k;
		char mode = 0;
		for (i = 0; *c != ')' && i < len; c++, i++) {
			if (isspace(*c)) {
				if (mode == 0) {
					lw++;
					continue;
				}

				if (mode == 1 || mode == 2)
					return false;

				if (mode == 3) {
					*c='\0';
					callback(k, std::string_view(lw, wl));
					mode = 0;
				}
			}
			else if (*c == '[') {
				if (mode == 0) {
					mode = 1;
					lw = c + 1;
					wl = 0;
				}
			} else if (*c == ']') {
				if (mode == 1) {
					*c= '\0';
					k = std::string_view(lw, wl);
					mode = 2;
				}
			} else if (*c == '=') {
				if (mode == 2) {
					mode = 3;
					wl = 0;
					lw = c+1;
				}
			} else {
				wl++;
			}
		}
		if (*c != ')')
			return false;

		*c = '\0';
		if (mode== 1 || mode == 2) {
			callback(k, std::string_view());
		} else if (mode == 3) {
			callback(k, std::string_view(lw));
		}
	}
	bool get_array_values(char* v, size_t len, std::function<void(char*, size_t)> callback) const {
		size_t i = 0;
		char* c = const_cast<char*>(v);
		char* lw = c;
		size_t word_len = 0;
		for (i = 0; *c != ')' && i < len; c++, i++) {
			if (isspace(*c)) {
				if (word_len == 0) {
					lw++;
					continue;
				}
				*c = '\0';
				callback(lw, word_len);
				lw = c+1;
				word_len = 0;
			} else {
				word_len++;
			}
		}
		if (*c != ')')
			return false;
		if (word_len>0) {
			*c = '\0';
			callback(lw, word_len);
		}
		return true;
	}

	void typeset_map(const std::string_view& key, const std::string_view& val) {
		char* v = const_cast<char*>(val.data());
		auto s =std::make_unique<lesh_scope_map_variable> ();
		if (*v == '(') {
			auto p = get_map_values(v+1, val.size()-1, [s=s.get()](auto k, auto v) {
				s->set(k, v);
			});
		}
		_vars.emplace(key, std::move(s));
	}
	void typeset(const std::string_view& key, const std::string_view& val) {
		char* v = const_cast<char*>(val.data());
		if (*v == '(') {// array
			auto s =std::make_unique<lesh_scope_array_variable> ();
			auto p = get_array_values(v+1, val.size()-1, [s = s.get()](const char * word, size_t len) {
				auto const ws = new char[len+1];
				memcpy(ws, word, len+1);
				s->push(ws);
			});
			if (p)
				_vars.emplace(key, std::move(s));
		} else { // env variable

		}
	}
};
class lesh_state {
	// https://gist.github.com/ClementNerma/1dd94cb0f1884b9c20d1ba0037bdcde2

private:
	alias_container _aliases;
	buffer_pool _buffer_pool;
	buffer_pool _global_pool;
	lesh_state_scope _lesh_scope;

	char** _envp;
	std::filesystem::path _pwd;
	std::string _display_pwd;
	std::string prompt;
	std::vector<std::filesystem::path> _path_env;
	std::string _home;
	std::unordered_map<std::string_view, std::string_view> _env;

public:
	lesh_state(std::filesystem::path current_path, char** envp) noexcept : _lesh_scope(nullptr, nullptr), _envp(envp), _buffer_pool(BUFFER_POOL_SIZE), _global_pool(0), _aliases() {
		for (auto it = _envp; *it; it++) {
			const auto e = *it;
			std::string_view v = {e};
			const auto idx = v.find('=');
			_env.emplace(v.substr(0, idx), (e +idx + 1));
		}

		load_home_directory();
		set_path(current_path);
		load_env_path();
	}

	[[nodiscard]] const char* pmt() const noexcept { return prompt.c_str(); };
	[[nodiscard]] const std::filesystem::path& pwd() const noexcept { return _pwd; }
	[[nodiscard]] const std::string& home() const noexcept { return _home; }
	[[nodiscard]] const std::string& display_pwd() const noexcept { return _display_pwd; }
	[[nodiscard]] const std::vector<std::filesystem::path>& path_env() const noexcept { return _path_env; }

	buffer_pool& global_pool() noexcept { return _global_pool; }
	buffer_pool& buffer_pool() noexcept { return _buffer_pool; }


	size_t adjust_home(std::string& input, size_t from) {
		size_t added = 0;
		auto portion =  std::string_view(input).substr(from);
		size_t n = 0;
		while((n = portion.find("~", n)) != std::string_view::npos) {
			input.replace(from + n, 1, _home);
			added += _home.size() - 1;
			portion = std::string_view(input).substr(from);
			n += _home.size();
		}
		return  added;
	}

	size_t adjust_home(std::string& input, size_t from, size_t len) {
		size_t added = 0;
		auto portion =  std::string_view(input).substr(from, len);
		size_t n = 0;
		while((n = portion.find("~", n)) != std::string_view::npos) {
			input.replace(from + n, 1, _home);
			len = len - 1 + _home.size();
			added += _home.size() - 1;
			n++;
			portion = std::string_view(input).substr(from, len);
		}
		return  added;
	}

	void adjust_home(std::string &input) noexcept {
		while (true) {
				std::string::size_type n = 0;
				while((n = input.find("~", n)) != std::string::npos) {
						input.replace(n, 1, _home);
				    n+=1;
			 	}
		}
	}

	void set_path() {
		std::filesystem::path h = _home;
		set_path(h);
	}

	void set_path(std::filesystem::path& p) {
		if (!p.compare(_pwd))
			return;

		_pwd = p;
		_display_pwd = std::string(p);
		auto h = _home;
		if (_display_pwd.starts_with(h))
			_display_pwd.replace(0, h.size(), "~");
		prompt = std::string(_display_pwd);
		prompt.append(" > ");
	}

	bool try_get_env(std::string_view key, std::string_view index, std::string_view& val) const {
		if (index.empty())
			return try_get_env(key, val);
		return _lesh_scope.try_get_var_value(key, index, val);
	}
	bool try_get_env(std::string_view key, std::string_view& val) const {
		if (auto it = _env.find(key); it != _env.end()) {
			val = it->second;
			return true;
		}
		return false;
	}

	lesh_state_scope* scope() noexcept { return &_lesh_scope; }

	bool try_get_alias(const char* key, ASTPipe const ** val) const {
		return _aliases.try_get_alias(key, val);
	}

	void normalize_aliases() {
		_aliases.normalize_aliases();
	}
	ASTPipe& emplace_alias(const char* key) {
		return _aliases.emplace_alias(key);
	}
	// void emplace_alias_o(const char* key, const ASTPipe& val, bool normalize_after = false) {
	// 	_aliases.emplace_alias(key, val);
	// 	if (normalize_after)
	// 		_aliases.normalize_aliases();
	// }

private:
	void load_env_path() {
		_path_env.clear();
		std::string_view p;
		if (!try_get_env("PATH", p))
			return;

		_path_env.reserve(20);

		// TODO: use strstr
		const auto pa = std::string(p);
		size_t pr = 0;
		size_t in = pa.find(':');
		while(in != std::string::npos) {
			auto pd = pa.substr(pr, in - pr);
			_path_env.push_back(pd);
			pr = in+1;
			in = pa.find(':', pr);
		}
		_path_env.push_back(pa.substr(pr));
	}

	void load_home_directory() {
		std::string_view home_dir;
		if (!try_get_env("HOME", home_dir))
			home_dir = "/";
		_home = std::string(home_dir);
	}
};


