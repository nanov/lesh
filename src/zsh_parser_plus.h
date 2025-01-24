#pragma once

#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

#include "util.h"

#include <__ranges/split_view.h>
#include <__ranges/views.h>

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
		T* new_storage = new T[_capacity];
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

	[[nodiscard]] const T* data_null_terminated() {
		// emplace_back(nullptr);
		return _using_heap? _storage.heap : _storage.stack;
	}
	[[nodiscard]] const T* data() const { return _using_heap? _storage.heap : _storage.stack; }

	[[nodiscard]] const T* get_at(const size_t idx) const { return (_using_heap? _storage.heap : _storage.stack) + idx; }
	[[nodiscard]] const T* operator [](const size_t idx) const {return get_at(idx);}


	[[nodiscard]] size_t size() const { return _size; }
	[[nodiscard]] size_t capacity() const { return _capacity; }
	[[nodiscard]] bool is_on_heap() const { return _using_heap; }
};
template<typename T, size_t StackSize>
class hybrid_vector {
	struct  storage {
		T stack[StackSize];
		T* heap;
	};

	storage _storage;
	size_t _size = 0;
	size_t _heap_capacity = 0;
	size_t _capacity;
	T* _location;
	bool _using_heap;

	void allocate_heap() {
		T* new_storage = new T[StackSize];

		_storage.heap = new_storage;
		_using_heap = true;
		_heap_capacity = StackSize;
		_capacity += StackSize;
		_location = _storage.heap;
	}

	void grow_heap() {
		_heap_capacity += StackSize;
		T* new_storage = new T[_heap_capacity];
		if (_using_heap) {
			for (size_t i = 0; i < (StackSize - _size); ++i)
				new_storage[i] = std::move(_storage.heap[i]);
			delete[] _storage.heap;
		}

		_storage.heap = new_storage;
		_capacity += StackSize;
		_location = _storage.heap + (StackSize - _size);
	}
	void clear() {
		size_t stack_size;
		size_t heap_size;
		if (_using_heap) {
			stack_size = StackSize;
			heap_size = _size - stack_size;
		} else {
			heap_size = 0;
			stack_size = _size;
		}
		for (size_t i = 0; i < stack_size; ++i)
			_storage.stack[i].~T();
		for (size_t i = 0; i < heap_size; ++i)
			_storage.heap[i].~T();
	}
public:
	void foreach(std::function<void(const T&)> callback) {
		if (_using_heap) {
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

	hybrid_vector() : _size(0), _capacity(StackSize), _using_heap(false) {
		// Initialize stack array using placement new
		new (_storage.stack) T[StackSize];
		_location = _storage.stack;
	}
	~hybrid_vector() {
		clear();
		if (_using_heap) {
			delete[] _storage.heap;
		}
	}

	template<size_t OtherStackSize>
	T& replace_at(const hybrid_vector<T, OtherStackSize>& other, size_t index) {
		size_t extra_elements = other.size() > 1 ? other.size() - 1 : 0;
		// First, ensure we have enough capacity
		size_t required_capacity = _size + extra_elements;  // -1 because we replace first element
		while (_capacity < required_capacity) {
			if (_using_heap) {
				grow_heap();
			} else {
				allocate_heap();
			}
		}

		if (extra_elements) {
			// Shift existing elements to make space
			for (size_t i = _size - 1; i > index; --i) {
				size_t new_pos = i + other.size() - 1;
				if (!_using_heap || new_pos < StackSize) {
					new (&_storage.stack[new_pos]) T(std::move(get_at_reference(i)));
					get_at_reference(i).~T();
				} else {
					new (&_storage.heap[new_pos - StackSize]) T(std::move(get_at_reference(i)));
					get_at_reference(i).~T();
				}
			}
		}

		if (!_using_heap || index < StackSize) {
			new (&_storage.stack[index]) T(other.get_at_reference(0));
		} else {
			new (&_storage.heap[index - StackSize]) T(other.get_at_reference(0));
		}

		// Insert remaining elements
		for (size_t i = 1; i < other.size(); ++i) {
			size_t insert_pos = index + i;
			if (!_using_heap || insert_pos < StackSize) {
				new (&_storage.stack[insert_pos]) T(other.get_at_reference(i));
			} else {
				new (&_storage.heap[insert_pos - StackSize]) T(other.get_at_reference(i));
			}
		}

		_size = _size + extra_elements;
		_location = (!_using_heap || _size < StackSize) ?
				&_storage.stack[_size] :
				&_storage.heap[_size - StackSize];

		return get_at_reference(index+extra_elements);
	}

	template<typename... Args>
	T* place(Args&&... args) {
		new (_location -1) T(std::forward<Args>(args)...);
		return _location - 1;
	}
	template<typename... Args>
	T* emplace_back(Args&&... args) {
		if (_size == _capacity) {
			if (_using_heap) {
				grow_heap();
			} else {
				allocate_heap();
			}
		}

		new (_location) T(std::forward<Args>(args)...);
		_size++;
		return _location++;
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

	[[nodiscard]] T& get_at_reference(size_t idx) const {
		return  (!_using_heap || idx < StackSize) ?  const_cast<T&>(_storage.stack[idx]) : _storage.heap[idx - StackSize];
	}

	[[nodiscard]] T* get_at(size_t idx) const {
		return  (!_using_heap || idx < StackSize) ? const_cast<T*>(&_storage.stack[idx]) : &_storage.heap[idx - StackSize];
	}

	T* operator [](size_t idx) const {return get_at(idx);}

	[[nodiscard]] size_t size() const { return _size; }
	[[nodiscard]] size_t capacity() const { return _capacity; }
	[[nodiscard]] bool is_on_heap() const { return _using_heap; }
};

namespace ZshParserPlus {
	class buffer_pool {
	public:
		buffer_pool(size_t size = 1024) {
			_data = new char[size];
			_end = _data + size;
			_current = _data;
		}

		~buffer_pool() {
			delete[] _data;
		}

		bool reallocate(char* at, size_t new_size,  char*& result) {
			if (at+new_size > _end) {
				result = static_cast<char *>(malloc(sizeof(char) * new_size));
				memcpy(result, at, _current - at);
				_current = at;
				return false;
			}
			_current = at + new_size;
			result = at;
			return true;
		}

		bool allocate(size_t size, char*& result) {
			if (_current+size > _end) {
				result = new char[size];
				return false;
			}
			result = _current;
			_current += size;
			return true;
		}

		void reset(char* to) {
			_current = to;
		}

		char* at() const { return _current; }
	private:
		char* _data;
		char* _current;
		const char* _end;
	};
	class Parser {
	public:
		struct ASTNode {
			enum class Type : uint8_t {
				COMMAND,
				PIPE,
				REDIRECT,
				VARIABLE,
				WORD
			};

			static constexpr size_t MAX_CHILDREN = 32;

			Type type;
			const char *value; // Points to null-terminated string in buffer
			uint8_t num_children;

			explicit ASTNode(Type t, const char *v = nullptr)
				: type(t), value(v), num_children(0) {
			}
		};


		// used for command parts (ex: ls -l  => [ls] [-l] )
		struct ASTWord {
			// null-terminated, not-owned
			const char *value;
		};
		struct ASTSubShell;
		struct ASTCommand {
			static constexpr size_t MAX_CHILDREN = 32;
			static constexpr size_t INITIAL_REFERENCES = 3;
			// those are essentially parameters
			hybrid_continuous_vector<ASTWord, MAX_CHILDREN> children;
			// used for subshell results, to keep alive and be able to free
			hybrid_vector<const char*, INITIAL_REFERENCES> references;

			size_t num_references = 0;

			ASTWord& emplace_child(const char* value) { return children.emplace_back(value); }
			void push_child(ASTWord&& value) { children.push_back(value); }
			[[nodiscard]] size_t number_of_children() const { return children.size(); }

			inline void add_reference(const char* ref) { references.push_back(ref);}

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

		class alias_container_plus {
		private:
			struct alias {
				const ASTPipe original;
				ASTPipe expanded;
			};
			std::unordered_map<std::string, alias> _aliases;


		public:
			alias_container_plus() : _aliases() {}
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
			}

			void normalize_aliases() {
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
	private:
		buffer_pool _buffer_pool = { BUFFER_POOL_SIZE };
		alias_container _aliases;
		lesh_state &_lesh_state;
	class built_in_commands {
		private:
		lesh_state &_state;
			// Define a type for builtin command functions
			using builtint = int (*)(lesh_state& state, const ASTCommand* cmd);
		  std::unordered_map<std::string, builtint> builtins;

			static int builtin_echo(lesh_state& state, const ASTCommand* cmd) {
				if (cmd->children.size() < 2)
					return 0;

				std::cout << cmd->children[1]->value;
				for (size_t i = 2; i < cmd->children.size(); ++i) {
					std::cout << ' ' << cmd->children[i]->value;
				}
				std::cout << std::endl;
			}
			static int builtin_cd(lesh_state& state, const ASTCommand* cmd) {
				if (cmd->children.size() < 2) {
					std::filesystem::current_path(state.home());
					state.set_path(std::filesystem::current_path());
					return 0;
				}

				std::error_code e;
				std::filesystem::current_path(state.pwd() / cmd->children[1]->value, e);
				if (e) {
					std::cout << "cd: " << cmd->children[1]->value << ": " << e.message() << std::endl;
					return -1;
				}
				state.set_path(std::filesystem::current_path());
				return 0;
			}
		public:
			explicit built_in_commands(lesh_state& state): _state(state) {
				builtins.emplace("cd", builtin_cd);
				builtins.emplace("echo", builtin_echo);
			}

			bool try_execute_built_in(const ASTCommand* cmd) {
				if (auto const it = builtins.find(cmd->children[0]->value); it != builtins.end()) {
					it->second(_state, cmd);
					return true;
				}
				return false;
			}
		};
		built_in_commands built_ins;

		static constexpr uint16_t MAX_ALIAS_DEPTH = 32;
		static constexpr size_t MAX_STATES = 64;

		inline static bool isSpecial(char c) {
			static constexpr bool special[256] = {
				['|'] = true, ['>'] = true, ['<'] = true,
				['&'] = true, [';'] = true,
				['('] = true, [')'] = true, ['$'] = true,
			};
			return special[static_cast<unsigned char>(c)];
		}


		struct ExpansionPart {
			char* value;
			size_t size;
			size_t capacity;
		};


	public:
		struct SimpleParsingStare {
		private:
			char* _input;
			size_t _length;
			size_t _position = 0;

			char* _current;

			buffer_pool& _buffer_pool;
			char* _buffer_start;
			hybrid_vector<char*, 12> _to_free;

		public:
			[[nodiscard]] buffer_pool& pool() const { return _buffer_pool; }
			enum class special_char {
				NONE = 0,
				PIPE,
				DOLLAR,
				COUNT,
			};
			explicit SimpleParsingStare(buffer_pool& pool, char *input, size_t length) : _buffer_pool(pool), _buffer_start(pool.at()), _input(input), _length(length), _current(_input) {}
			SimpleParsingStare(buffer_pool& pool, const char *input) : SimpleParsingStare(pool, strdup(input), strlen(input)) {}
			explicit SimpleParsingStare(buffer_pool& pool, std::string& input):  SimpleParsingStare(pool, input.data(), input.size()) {}
			~SimpleParsingStare() {
				_buffer_pool.reset(_buffer_start);
				_to_free.foreach([](auto* ptr) { delete ptr; });
			}


			SimpleParsingStare sub_state(char* data, size_t length) {
				return SimpleParsingStare(_buffer_pool, data, length);
			}
			SimpleParsingStare sub_state(std::string_view input) {
				return SimpleParsingStare(_buffer_pool, const_cast<char*>(input.data()), input.size()+1);
			}

			void remove_brackets(char* brackets_open, char* brackets_close) {
				auto brackets_open_pos = brackets_open - _input;
				auto brackets_close_pos = brackets_close - _input;
				memmove(brackets_open, brackets_open+1, brackets_close_pos - brackets_open_pos - 1);
				memmove(brackets_close-1, brackets_close+1, _length - brackets_close_pos - 1);
				_length -= 2;
				_position -= 2;
				_current = _input + _position;
			}

			bool peek(char*& c) const {
				if (_position == _length)
					return false;
				c = _current + 1;
				return true;
			}
			[[nodiscard]] char* current() const { return _current; }
			bool plusplus_s() {
				if (_position >= _length)
					return false;
				_current++;
				_position++;
				return true;
			}

			bool skip_whitespace(char& c_out) {
				char c = *_current;
				while (	_position < _length
								&& (c == '\0' || std::isspace(c))
								) {
					*_current = '\0';
					plusplus();
					c = *_current;
				}
				c_out = c;
				return _position < _length;
			}

			void plusplus() {
				_current++;
				_position++;
			}

			char* rent(std::string_view a, std::string_view b) {
				char* res;
				if (!_buffer_pool.allocate(a.size() + b.size() + 1, res))
					_to_free.push_back(res);
				memcpy(res, a.data(), a.size());
				memcpy(res+a.size(), b.data(), b.size());
				res[a.size()+b.size()]= '\0';
				return res;
			}

			char* rent(std::string_view a, std::string_view b, std::string_view c) {
				char* res;
				if (!_buffer_pool.allocate(a.size() + b.size() + c.size() + 1, res))
					_to_free.push_back(res);
				memcpy(res, a.data(), a.size());
				memcpy(res+a.size(), b.data(), b.size());
				memcpy(res+a.size()+b.size(), c.data(), c.size());
				res[a.size()+b.size()+c.size()]= '\0';
				return res;
			}
			bool skip_whitespace() {
				char _;
				return skip_whitespace(_);
			}

			inline static special_char char_type(char c) {
				static constexpr special_char special[256] = {
					['|'] = special_char::PIPE,
					['>'] = special_char::NONE, ['<'] = special_char::NONE,
					['&'] = special_char::NONE,
					[';'] = special_char::NONE,
					['('] = special_char::NONE, [')'] = special_char::NONE,
					['$'] = special_char::DOLLAR,
				};
				return special[static_cast<unsigned char>(c)];
			}
			inline static bool is_special(char c) {
				return char_type(c) != special_char::NONE;
			}

		inline std::string_view match_charter(char bracket = ')') {
			char* word = _current;

			char c = *_current;
			auto l = _length-1;
			while (_position < l &&
				     c != bracket) {
				plusplus();
				c = *_current;
			}
			*_current = '\0';

			return std::string_view(word, _current-word);
		}
		inline std::string_view parse_word() {
			char* word = _current;

			char c = *_current;
			auto l = _length-1;
			while (_position < l &&
				     c != '\0' &&
			       !std::isspace(c) &&
			       !is_special(c)) {
				plusplus();
				c = *_current;
			}
			*_current = '\0';

			return std::string_view(word, _current-word);
		}

		};

		class transfarable_buffer {
		private:
			buffer_pool& _buffer_pool;
			char* buffer_start = nullptr;
			char* ptr = nullptr;
			size_t capacity = 0;

		public:
			transfarable_buffer(buffer_pool& pool, size_t inital_size): _buffer_pool(pool) {
				capacity = inital_size;
				buffer_start = _buffer_pool.at();
				if (!_buffer_pool.allocate(inital_size, ptr))
					buffer_start = nullptr;
			};

			size_t trim_end(char c, size_t end) {
				auto p = ptr + end - 1;
				auto bp = p;
				while (*p==c) {
					*(p--) = '\0';
				}
				return bp - p;
			}

			void resize(size_t new_size) {
				if (new_size <= capacity)
					return;
				if (!buffer_start) {
					ptr = static_cast<char *>(realloc(ptr, new_size * sizeof(char)));
					capacity = new_size;
				} else if (!_buffer_pool.reallocate(buffer_start, new_size, ptr))
					buffer_start = nullptr;
				capacity = new_size;
			}

			char* data() const { return ptr; }
		};

		alias_container_plus _aliases_plus;
		void add_alias(const char* alias, char* value) {
			auto state = SimpleParsingStare(_buffer_pool, value);
			ASTPipe pipe;
			parse_command<false>(state, pipe);
			_aliases_plus.emplace_alias(alias, pipe);
		}


		class command_parser {
		private:
			lesh_state &_lesh_state;
			enum class parsing_state {
				none = 0,
				in_a_word = 1,
			};

			SimpleParsingStare &_state;
			ASTPipe &_pipe;
			const alias_container_plus &_aliases_plus;

			const built_in_commands& _build_in_commands;
			ASTCommand* _command;
			bool _is_command;
			parsing_state _p_state;
			char* _word_beginning;
			char _expected_closing_bracket = 0;
			char* _bracket_start;
			bool _is_escape = false;

			//
		private:
			template<bool is_executing>
			void ensure_word(const char* word_begin, char *word_end) {
				if (_p_state != parsing_state::in_a_word)
					return;

				_p_state = parsing_state::none;
				*word_end = '\0';
				if constexpr (is_executing) {
					if (ASTPipe aliased_pipe; _is_command && _aliases_plus.try_get_alias(word_begin, aliased_pipe)) {
						_command = _pipe.merge(_command, aliased_pipe);
					} else {
						_command->emplace_child(word_begin);
					}
					_is_command = false;
				} else {
					_command->emplace_child(word_begin);
				}
				word_begin = nullptr;
			}
			static bool is_seperator(char c) {
				static constexpr bool special[256] = {
					['|'] = true,
					['\0'] = true,
					[' '] = true,
					// TODO:: Add more
				};
				return special[static_cast<unsigned char>(c)];
			}
		public:

			template<bool is_executing>
			static ASTPipe& parse(lesh_state& lesh_state, SimpleParsingStare &state, const built_in_commands& bc, const alias_container_plus &aliases, ASTPipe &pipe, ASTCommand* command) {
				auto p = command_parser(lesh_state, state, bc, aliases, pipe, command);
				return p.parse<is_executing>();
			}
			template<bool is_executing>
			static ASTPipe& parse(lesh_state& lesh_state, SimpleParsingStare &state,const built_in_commands& bc, const alias_container_plus &aliases, ASTPipe &pipe) {
				auto p = command_parser(lesh_state, state, bc, aliases, pipe);
				return p.parse<is_executing>();
			}
			command_parser(lesh_state& lesh_state, SimpleParsingStare &state, const built_in_commands& bc, const alias_container_plus &aliases, ASTPipe &pipe) :
			command_parser(lesh_state, state, bc, aliases, pipe, pipe.emplace_command()) {
			}

			command_parser(lesh_state& lesh_state, SimpleParsingStare &state, const built_in_commands& bc, const alias_container_plus &aliases, ASTPipe &pipe, ASTCommand* command) :
					_state(state), _pipe(pipe), _aliases_plus(aliases), _is_command(true), _lesh_state(lesh_state),
					_p_state(parsing_state::none), _word_beginning(nullptr), _bracket_start(nullptr), _command(command), _build_in_commands(bc) {
			}
			template<bool is_executing>
			ASTPipe& parse() {
				while (true) {
					auto c = _state.current();
					if (_expected_closing_bracket) {
						char ch = *c;
						if (ch == _expected_closing_bracket) {
							if (_is_escape) {
								_is_escape = false;
							} else {
								_expected_closing_bracket = 0;
								// better case - not in a middle of a word
								if (_p_state == parsing_state::none) {
									// but wait a minute - we may be at the beginning of one :(
									if (char *next; !_state.peek(next) || is_seperator(*next)) {
										_p_state = parsing_state::in_a_word;
										ensure_word<is_executing>(++_bracket_start, c);
									} else {
										_state.remove_brackets(_bracket_start, c);
										_p_state = parsing_state::in_a_word;
										_word_beginning = _bracket_start;
									}
								} else { // NOTE: for now mainuplating string seems simpler option
									_state.remove_brackets(_bracket_start, c);
								}
							}
						} else if (ch == '/') {
							_is_escape = true;
						}
					} else {
						switch (*c) {
							case '\'':
							case '"': {
								_expected_closing_bracket = *c;
								_bracket_start = c;
								break;
							}
							case ' ':
							case '\0': {
								ensure_word<is_executing>(_word_beginning, c);
							} break;

							// pipe process one to another
							case '|': {
								*c = '\0';
								ensure_word<is_executing>(_word_beginning, c);
								_command = _pipe.emplace_command();
								_is_command = true;
							} break;
							case '{': {
								std::string_view p =  (_p_state == parsing_state::in_a_word)? std::string_view(_word_beginning, c - _word_beginning) : std::string_view();
								_state.plusplus();
								auto words = _state.match_charter('}');
								_state.plusplus();
								auto a = _state.parse_word();
								for (auto wr : std::ranges::views::split(words, ',')) {
									auto w = std::string_view(wr);
									if (p.empty() && a.empty()) {
										_p_state = parsing_state::in_a_word;
										ensure_word<is_executing>(w.data(), const_cast<char*>(w.data()+w.size()));
									} else {
										auto wor = _state.rent(p, w, a);
										_p_state = parsing_state::in_a_word;
										c = _state.current();
										ensure_word<is_executing>(wor, c);
									}
								}
							} break;

							case '$': {
								char *p;
								if (_state.peek(p)) {
									if (*p == '(') {
										std::string_view p =  (_p_state == parsing_state::in_a_word)? std::string_view(_word_beginning, c - _word_beginning) : std::string_view();
										_state.plusplus();
										_state.plusplus();
										auto sub_input = _state.match_charter(')');
										auto sub_parsing_state = _state.sub_state(sub_input);
										auto sub_pipe = ASTPipe{};
										parse<is_executing>(_lesh_state, sub_parsing_state, _build_in_commands, _aliases_plus, sub_pipe);
										int pipe_fd[2];
										pipe(pipe_fd); // todo handle error

										execute(sub_pipe, 0, pipe_fd[1]);
										close(pipe_fd[1]);
										transfarable_buffer tb = { _state.pool(), SUBSHELL_BUFFER_INITIAL_SIZE };
										size_t total = 0;
										size_t bytes_read = 0;

										while ((bytes_read = read(pipe_fd[0], tb.data() + total, SUBSHELL_BUFFER_INITIAL_SIZE)) > 0) {
											total += bytes_read;
											tb.resize(total + SUBSHELL_BUFFER_INITIAL_SIZE);
										}
										close(pipe_fd[0]);
										total -= tb.trim_end('\n', total);

										// detached
										if (char* next; _p_state == parsing_state::none && (!_state.peek(next) || is_seperator(*next))){
											auto sub_result_input = _state.sub_state(tb.data(), total);
											auto sub_result_pipe = ASTPipe{};
											parse<false>(_lesh_state, sub_result_input, _build_in_commands, _aliases_plus, _pipe, _command);
										}


										// TODO: handle subshell
									} else { // optimistic variable expansion
										std::string_view p =  (_p_state == parsing_state::in_a_word)? std::string_view(_word_beginning, c - _word_beginning) : std::string_view();
										*c = '\0';
										_state.plusplus();
										auto w = _state.parse_word();
										if (std::string_view val; _lesh_state.try_get_env(w, val)) {
											if (!p.empty()) {
												auto con = _state.rent(p, val);
												ensure_word<is_executing>(con, c);
											} else if (_p_state == parsing_state::none) {
												_p_state = parsing_state::in_a_word;
												c = _state.current();
												ensure_word<is_executing>(val.data(), c);
											}
										}

									}

								}
								// todo: maybe subshell, maybe variable
							} break;

							// we are somewhere in a word
							default: {
								if (_p_state == parsing_state::none) {
									_p_state = parsing_state::in_a_word;
									_word_beginning = c;
								}
							}
						}
					}
					if (!_state.plusplus_s()) {
						// TODO: handle non closing brackets
						// NOTE: end of input shouldn't be inside of a word as input is expceted to be null-terminated
						ensure_word<is_executing>(_word_beginning, c);
						break;
					}
				}
				return _pipe;
			}
		};

		template<bool is_executing>
		inline void parse_command(SimpleParsingStare& state, ASTPipe& ast_pipe) const {
			command_parser parser(_lesh_state, state, built_ins, _aliases_plus, ast_pipe);
			parser.parse<is_executing>();
		}

		// Execute a single command
		pid_t static execute_command(ASTCommand *command, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
			// if (built_ins.try_execute_built_in(command))
			// 	return 0;

			pid_t pid = fork();

			if (pid == -1) {
				throw std::runtime_error("Fork failed");
			}

			if (pid == 0) {
				// Child process
				// Set up input/output pipes if provided
				if (input_fd != STDIN_FILENO) {
					dup2(input_fd, STDIN_FILENO);
					close(input_fd);
				}

				if (output_fd != STDOUT_FILENO) {
					dup2(output_fd, STDOUT_FILENO);
					close(output_fd);
				}

				// TODO: Add error handling and bounds checking
				auto argv = reinterpret_cast<char**>(const_cast<ASTWord*>(command->children.data_null_terminated()));
				// command->print();

				// Execute command
				execvp(argv[0], argv);


				// If execvp fails
				perror("execvp");
				exit(EXIT_FAILURE);
			}

			// Parent process
			return pid;
		}

		std::vector<pid_t> static execute_pipeline(const ASTPipe &pipeline, int p_input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
			// TODO: use pool
			std::vector<pid_t> pids;
			int input_fd = p_input_fd;


			// TODO: Handle no size 0
			size_t last_child_index = pipeline.size() - 1;//  num_children - 1;

			for (size_t i = 0; i < last_child_index; ++i) {
				int pipefd[2];
				// Create pipe
				if (pipe(pipefd) == -1) {
					throw std::runtime_error("Pipe creation failed");
				}

				// Execute command with current input and pipe output
				pids.push_back(execute_command(pipeline.commands[i], input_fd, pipefd[1]));

				// Close write end of pipe
				close(pipefd[1]);

				if (input_fd != STDIN_FILENO)
					close(input_fd);

				// Next command will read from this pipe
				input_fd = pipefd[0];
			}

			pids.push_back(execute_command(pipeline.commands[last_child_index], input_fd, output_fd));
			if (input_fd != STDIN_FILENO)
				close(input_fd);

			return pids;
		}

	public:
		explicit inline Parser(lesh_state& state): _lesh_state(state), built_ins(built_in_commands(state)), _aliases_plus() {}


		void init_aliases() {
			// add_alias("m", "l | grep");
			add_alias("l", "ls -lah");
			add_alias("ls", "ls -G");
			_aliases_plus.normalize_aliases();
		}
		inline void parse_and_execute(std::string& input) {
			auto state = SimpleParsingStare{_buffer_pool, input}; // ExpansionContainer{input};
			auto pipe = ASTPipe{};
			parse_command<true>(state, pipe);
			execute(pipe);
		}

		inline static void execute(const ASTPipe &pipe, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
			switch (pipe.size()) {
				// TODO: maybe error handling
				case 0: break;;
				case 1: {
					pipe.print();
					if (const auto pid = execute_command(pipe.commands[0], input_fd, output_fd)) {
						int status;
						waitpid(pid, &status, 0);
					}
				} break;;
				default: {
					// Wait for all processes in pipeline
					for (const auto pids = execute_pipeline(pipe, input_fd, output_fd); const pid_t pid: pids) {
						if (pid) {
							int status;
							waitpid(pid, &status, 0);
						}
					}
				}
					return;
			}
		}
	};
} // namespace ZshParserPlus
