#pragma once

#include <array>
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
class hybrid_continuous_vector {
	union storage {
		T stack[StackSize];
		T* heap;

		storage() { /* Default constructor needed for union */ }
		~storage() { /* Destructor needed for union */ }
	};

	storage _storage;
	size_t _size;
	size_t _capacity;
	T* _location = _storage.stack;
	bool _using_heap;

	void allocate_heap() {
		_capacity += StackSize;
		T* new_storage = static_cast<T*>(::operator new(_capacity * sizeof(T)));
		for (size_t i = 0; i < _size; ++i) {
			new (new_storage + i) T(std::move(_storage.stack[i]));
			_storage.stack[i].~T();
		}
		_storage.heap = new_storage;
		_using_heap = true;
		_location = _storage.heap + _size;
	}

	void grow_heap() {
		_capacity += StackSize;
		T* new_storage = new T[_capacity]();
		for (size_t i = 0; i < _size; ++i)
			new_storage[i] = std::move(_storage.heap[i]);

		delete[] _storage.heap;
		_storage.heap = new_storage;
		_location = _storage.heap + _size;
	}
	void clear() {
		auto storage = _using_heap ? _storage.heap : _storage.stack;
		for (size_t i = 0; i < _size; ++i, ++storage)
			storage->~T();
		if (_using_heap)
			delete[] _storage.heap;
	}

public:
	hybrid_continuous_vector(const hybrid_continuous_vector& other) {
		_size = other._size;
		_capacity = other._capacity;
		_using_heap = other._using_heap;
		// TODO: proper storage copy
		if (_using_heap) {
			_storage.heap = static_cast<T *>(memcpy(new T[other._capacity], other._storage.heap, other._capacity * sizeof(T)));
		} else {
			memcpy(&_storage.stack[0], other._storage.stack, other.size() * sizeof(T));
		}
		_location = (_using_heap ? _storage.heap : _storage.stack) + _size;
	}

	T* data() {
		return _using_heap ? _storage.heap : _storage.stack;
	}

	hybrid_continuous_vector() : _size(0), _capacity(StackSize), _using_heap(false) {
		// Initialize stack array using placement new
		new (_storage.stack) T[StackSize];
	}
	~hybrid_continuous_vector() {
		clear();
		if (_using_heap) { delete[] _storage.heap;}
	}
	template<typename... Args>
	T& emplace_back(Args&&... args) {
		if (_size == _capacity) {
			if (_using_heap) {
				grow_heap();
			} else {
				allocate_heap();
			}
		}

		new (_location) T(std::forward<Args>(args)...);
		_size++;
		return *_location++;
	}

	void replace_front(const T * addition, size_t addition_size) {
		size_t extra_elements = addition_size > 1 ? addition_size - 1 : 0;

		// Check if we need to grow
		if (_size + extra_elements > _capacity) {
			if (_using_heap) {
				grow_heap();
			} else {
				allocate_heap();
			}
		}

		T* storage = _using_heap ? _storage.heap : _storage.stack;
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
		_location = _using_heap ? _storage.heap + _size : _storage.stack + _size;
	}


	void push_back(const T& value) { emplace_back(value); }
	void push_back(T&& value) { emplace_back(std::move(value)); }

	// TODO: ensure it's always null terminated :/
	[[nodiscard]] const T* data_null_terminated() {
		return _using_heap? _storage.heap : _storage.stack;
	}
	[[nodiscard]] const T* data() const { return _using_heap? _storage.heap : _storage.stack; }

	[[nodiscard]] const T* get_at(const size_t idx) const { return (_using_heap? _storage.heap : _storage.stack) + idx; }
	[[nodiscard]] const T* operator [](const size_t idx) const {return get_at(idx);}


	[[nodiscard]] size_t size() const { return _size; }
	[[nodiscard]] size_t capacity() const { return _capacity; }
	[[nodiscard]] bool is_on_heap() const { return _using_heap; }
	bool operator==(const hybrid_continuous_vector& other) const {
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
		T stack[StackSize];
		T* heap = nullptr;
		T* stack_end;
		public:
			storage(): heap(nullptr), stack_end(stack + StackSize - 1) {}
	};

	storage _storage;
	size_t _size = 0;
	size_t _heap_capacity = 0;
	size_t _capacity;
	T* _location;

	[[nodiscard]] T* grow_heap(size_t by_amount = StackSize) {
		_heap_capacity += by_amount;
		auto old_store = _storage.heap;
		_storage.heap = new T[_heap_capacity]();
		if (old_store) {
			std::move(old_store, _location, _storage.heap);
			delete[] old_store;
		}

		_capacity += by_amount;
		return  _storage.heap + (StackSize - _size);
	}

	void clear() {
		T* stack_end = nullptr;
		if (_storage.heap) {
			stack_end = _storage.stack + (StackSize -1);
			for (auto b = _storage.heap; b <= _location; ++b)
				b->~T();
		} else {
			stack_end = _location;
		}
		for (auto b = _storage.stack; b <= stack_end; ++b)
			b->~T();
	}
public:
	void reserve_free_slots(size_t requiered_free_slots) {
		if (_capacity - _size >= requiered_free_slots)
			return;
		reserve(requiered_free_slots - (_capacity - _size));
	}
	void reserve(size_t new_cap) {
		if (new_cap <= _capacity)
			return;
		_location = grow_heap(new_cap - _capacity);
	}
	void foreach(std::function<void(const T&)> callback) {
		if (_storage.heap) {
			for (size_t i = 0; i < StackSize; ++i)
				callback(_storage.stack[i]);
			for (size_t i = 0; i < (StackSize - _size); ++i)
				callback(_storage.heap[i]);
			return;
		}
		for (size_t i = 0; i<_size; ++i)
			callback(_storage.stack[i]);

	}
	struct iterator {
	private:
		hybrid_vector<T, StackSize> _that;
		T* location;
		size_t _index;
	public:
		iterator(hybrid_vector<T, StackSize>& that) : _that(that), _index(0) {
			location = that._storage[0];
		}
		T& operator*() {
			return *_that.get_at(_index);
		}
		iterator& operator++() {
			++_index;
			return *this;
		}

		bool operator!=(const iterator& other) const {
			return _index != other._index;
		}

	};

	iterator begin() { return iterator(*this, 0); }
	iterator end() { return iterator(*this, _size); }

	hybrid_vector() : _size(0), _capacity(StackSize) {
		// Initialize stack array using placement new
		new (_storage.stack) T[StackSize];
		_location = _storage.stack - 1;
	}
	~hybrid_vector() {
		clear();
		if (_storage.heap) {
			delete[] _storage.heap;
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
	T* place(Args&&... args) {
		new (++_location) T(std::forward<Args>(args)...);
		return _location;
	}
	template<typename... Args>
	T* emplace_back(Args&&... args) {
		if (_size == _capacity) {
			_location = grow_heap();
		} else if ( _size == StackSize ) {
			_location = _storage.heap;
		} else {
			_location += 1;
		}

		new (_location) T(std::forward<Args>(args)...);
		_size++;
		return _location;
	}

	T& push_back_copy(const T& value) {
		T copy = value;  // Make explicit copy
		return *emplace_back(std::move(copy));
	}

	T& place_copy(const T& value) {
		T copy = value;  // Make explicit copy
		return *place(std::move(copy));
	}
	T& push_back(const T& value) { return *emplace_back(value); }
	T& push_back(T&& value) { return emplace_back(std::move(value)); }

	void pop_back() {
		if (_size < 0)
			return;

		_size--;
		// destroy element
		_location.~T();

		// is size is exactly the stack size, it means we need to downgrade from heap location to stack one
		if (_size == StackSize) {
			_location = _storage.stack + StackSize -1;
			return;
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

	[[nodiscard]] size_t size() const { return _size; }
	[[nodiscard]] size_t capacity() const { return _capacity; }
	[[nodiscard]] bool is_on_heap() const { return _storage.heap; }
};

// used for command parts (ex: ls -l  => [ls] [-l] )
struct ASTWord {
    // null-terminated, not-owned
    const char *value;
};
// command is a part exectued by itself, it conatins a colletions of wrods whics are it's arhuments ( ls -l -gAH )
struct ASTCommand {
    static constexpr size_t MAX_CHILDREN = 32;
    static constexpr size_t INITIAL_REFERENCES = 3;
    // those are essentially parameters
    hybrid_continuous_vector<ASTWord, MAX_CHILDREN> children;

    size_t num_references = 0;

    ASTWord& emplace_child(const char* value) { return children.emplace_back(value); }
    void push_child(ASTWord&& value) { children.push_back(value); }
    [[nodiscard]] size_t number_of_children() const { return children.size(); }


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
    hybrid_vector<ASTCommand, 3> commands;

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
        auto first_command = *commands[idx];
        auto& last_command = commands.replace_at(other.commands, idx);
        last_command.enrich_wth(last_command);
        return &last_command;
    }

    ASTCommand* merge(ASTCommand*& current_command, const ASTPipe & other) {
        ASTCommand* c = current_command;
        current_command = &other.commands.get_at_reference(0);
        commands.place_copy(*current_command);

        for (size_t i = 1; i < other.size(); ++i) {
            auto cmd = other.commands.get_at_reference(i);
            c = &commands.push_back_copy(cmd);
        }
        return c;
    }


    [[nodiscard]] size_t size() const { return commands.size(); }
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
		const ASTPipe original;
		ASTPipe expanded;
	};
	std::unordered_map<std::string, alias> _aliases;


public:
	alias_container() : _aliases() {}
	bool try_get_alias(const char * command, ASTPipe& alias) const {
		if (const auto it = _aliases.find(command); it != _aliases.end()) {
			alias = it->second.expanded;
			return true;
		}
		return false;
	}

	void emplace_alias(const char* alias, const ASTPipe & pipe) {
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

struct char_pointer_hash{
    auto operator()( const char* ptr ) const noexcept{ return std::hash<std::string_view>{}( ptr );}
};

using transparent_string_hash = overload<
    std::hash<std::string>,
    std::hash<std::string_view>,
    char_pointer_hash
>;
class lesh_state {
private:
	alias_container _aliases;
	buffer_pool _buffer_pool;
	buffer_pool _global_pool;

	char** _envp;
	std::filesystem::path _pwd;
	std::string _display_pwd;
	std::string prompt;
	std::vector<std::filesystem::path> _path_env;
	std::string _home;
	std::unordered_map<std::string_view, std::string_view> _env;

public:
	lesh_state(std::filesystem::path current_path, char** envp) noexcept : _envp(envp), _buffer_pool(BUFFER_POOL_SIZE), _global_pool(0), _aliases() {
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

	bool try_get_env(std::string_view key, std::string_view& val) const {
		if (auto it = _env.find(key); it != _env.end()) {
			val = it->second;
			return true;
		}
		return false;
	}

	bool try_get_alias(const char* key, ASTPipe& val) const {
		return _aliases.try_get_alias(key, val);
	}

	void emplace_alias(const char* key, const ASTPipe& val, bool normalize_after = false) {
		_aliases.emplace_alias(key, val);
		if (normalize_after)
			_aliases.normalize_aliases();
	}

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


