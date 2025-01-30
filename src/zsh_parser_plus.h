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
	};

	storage _storage;
	size_t _size = 0;
	size_t _heap_capacity = 0;
	size_t _capacity;
	T* _location;

	void allocate_heap() {
		T* new_storage = new T[StackSize];

		_storage.heap = new_storage;
		_heap_capacity = StackSize;
		_capacity += StackSize;
		_location = _storage.heap;
	}

	void grow_heap() {
		_heap_capacity += StackSize;
		T* new_storage = new T[_heap_capacity]();
		if (_storage.heap) {
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
		if (_storage.heap) {
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
		_location = _storage.stack;
	}
	~hybrid_vector() {
		clear();
		if (_storage.heap) {
			delete[] _storage.heap;
		}
	}

	template<size_t OtherStackSize>
	T& replace_at(const hybrid_vector<T, OtherStackSize>& other, size_t index) {
		size_t extra_elements = other.size() > 1 ? other.size() - 1 : 0;
		// First, ensure we have enough capacity
		size_t required_capacity = _size + extra_elements;  // -1 because we replace first element
		while (_capacity < required_capacity) {
			if (_storage.heap) {
				grow_heap();
			} else {
				allocate_heap();
			}
		}

		if (extra_elements) {
			// Shift existing elements to make space
			for (size_t i = _size - 1; i > index; --i) {
				size_t new_pos = i + other.size() - 1;
				if (!_storage.heap || new_pos < StackSize) {
					new (&_storage.stack[new_pos]) T(std::move(get_at_reference(i)));
					get_at_reference(i).~T();
				} else {
					new (&_storage.heap[new_pos - StackSize]) T(std::move(get_at_reference(i)));
					get_at_reference(i).~T();
				}
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
		new (_location -1) T(std::forward<Args>(args)...);
		return _location - 1;
	}
	template<typename... Args>
	T* emplace_back(Args&&... args) {
		if (_size == _capacity) {
			if (_storage.heap) {
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

namespace ZshParserPlus {
	class Parser {
	private:
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
		class alias_container {
		private:
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
		class built_in_commands {
			private:
				lesh_state &_state;
				// Define a type for builtin command functions
				using builtint = int (*)(lesh_state& state, const ASTCommand* cmd);
				std::unordered_map<std::string, builtint> builtins;

				static int builtin_echo(lesh_state& state, const ASTCommand* cmd) {
					// TODO: help
					if (cmd->children.size() < 2)
						return 0;

					bool display_return = true;
					auto argv = reinterpret_cast<char**>(const_cast<ASTWord*>(cmd->children.data())) + 1;

					if (strcmp(*argv,"-n") ==0) {
						argv++;
						display_return = false;
					}
					auto val = *argv++;
					if (!val)
						return 0;

					fputs(val, stdout);
					while ((val = *argv++)) {
						putchar(' ');
						fputs(val, stdout);
					}

					if (display_return)
						putchar('\n');

					return 0;
				}

				static int builtin_cd(lesh_state& state, const ASTCommand* cmd) {
					if (cmd->children.size() < 2) {
						std::filesystem::current_path(state.home());
						auto s = std::filesystem::current_path();
						state.set_path(s);
						return 0;
					}

					std::error_code e;
					std::filesystem::current_path(state.pwd() / cmd->children[1]->value, e);
					if (e) {
						std::cout << "cd: " << cmd->children[1]->value << ": " << e.message() << std::endl;
						return -1;
					}
					auto s = std::filesystem::current_path();
					state.set_path(s);
					return 0;
				}

				static int builtin_info(lesh_state& state, const ASTCommand* cmd) {
	#ifdef VERBOSE_POOL_DATA
					{
						const auto &b = state.global_pool();
						std::println(" - Global Buffer Pool");
						std::println("  [ ] Pool Size {}", b.pool_size());
						std::println("  [ ] Bytes Used {}", b.bytes_used());
						std::println("  [ ] Lifetime Bytes Used {}", b.lifetime_pool_bytes_used);
						std::println("  [ ] Lifetime Bytes Allocated {}", b.lifetime_pool_bytes_allocated);
					}
					{
						const auto &b = state.buffer_pool();
						std::println(" - Buffer Pool");
						std::println("  [ ] Pool Size {}", b.pool_size());
						std::println("  [ ] Bytes Used {}", b.bytes_used());
						std::println("  [ ] Lifetime Bytes Used {}", b.lifetime_pool_bytes_used);
						std::println("  [ ] Lifetime Bytes Allocated {}", b.lifetime_pool_bytes_allocated);
					}
	#endif
					return 0;
				}
			public:
				explicit built_in_commands(lesh_state& state): _state(state) {
					builtins.emplace("cd", builtin_cd);
					builtins.emplace("echo", builtin_echo);
					builtins.emplace("info", builtin_info);
				}

				bool try_execute_built_in(const ASTCommand* cmd, int input_fd, int output_fd) const {
					if (auto const it = builtins.find(cmd->children[0]->value); it != builtins.end()) {
						int saved_stdout = -1, saved_stdin = -1;
						if (output_fd != STDOUT_FILENO) {
							saved_stdout = dup(STDOUT_FILENO);
							dup2(output_fd, STDOUT_FILENO);
						}

						if (input_fd != STDIN_FILENO) {
							saved_stdin = dup(STDIN_FILENO);
							dup2(input_fd, STDIN_FILENO);
						}


						it->second(_state, cmd);

						if (saved_stdout != -1) {
							dup2(saved_stdout, STDOUT_FILENO);
							close(saved_stdout);
						}
						if (saved_stdin != -1) {
							dup2(saved_stdin, STDIN_FILENO);
							close(saved_stdin);
						}					return true;
					}
					return false;
				}
			};
		class executor {
		public:
			executor(lesh_state& lesh_state) : _built_ins({lesh_state}) {}

			void execute(const ASTPipe& pipe, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) const {
				switch (pipe.size()) {
					// TODO: maybe error handling
					case 0: break;;
					case 1: {
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
				}
			}
		private:
			built_in_commands _built_ins;

			std::vector<pid_t> execute_pipeline(const ASTPipe &pipeline, int p_input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) const {
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
			pid_t execute_command(ASTCommand *command, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) const {
				if (_built_ins.try_execute_built_in(command, input_fd, output_fd))
					return 0;

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

					// Execute command
					execvp(argv[0], argv);


					// If execvp fails
					perror("execvp");
					exit(EXIT_FAILURE);
				}

				// Parent process
				return pid;
			}
		};

		alias_container _aliases;
		lesh_state &_lesh_state;
		executor _executor;

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

		struct SimpleParsingStare {
		private:
			char* _input;
			bool _is_global;
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
			explicit SimpleParsingStare(bool is_global, buffer_pool& pool, char *input, size_t length) : _is_global(is_global), _buffer_pool(pool), _buffer_start(pool.at()), _input(input), _length(length+1), _current(_input) {}
			SimpleParsingStare(bool is_global, buffer_pool& pool, const char *input) : SimpleParsingStare(is_global, pool, strdup(input), strlen(input)) {}
			explicit SimpleParsingStare(bool is_global, buffer_pool& pool, std::string& input):  SimpleParsingStare(is_global, pool, input.data(), input.size()) {}

			~SimpleParsingStare() {
				if (_is_global)
					return;

				_buffer_pool.reset(_buffer_start);
				_to_free.foreach([](auto* ptr) { free(ptr); });
			}


			[[nodiscard]] SimpleParsingStare sub_state(char* data, size_t length) const {
				return SimpleParsingStare(false, _buffer_pool, data, length);
			}

			[[nodiscard]] SimpleParsingStare sub_state(std::string_view input) const {
				return sub_state(const_cast<char*>(input.data()), input.size());
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
				if (_position >= _length - 1)
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

			inline static special_char char_type(const char c) {
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
			inline static bool is_special(const char c) {
				return char_type(c) != special_char::NONE;
			}

		inline std::string_view match_charter(const char bracket = ')') {
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
			const char * word = _current;

			char c = *_current;
			while (_position < _length &&
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
			size_t _capacity = 0;

		public:
			transfarable_buffer(buffer_pool& pool, size_t inital_size): _buffer_pool(pool) {
				_capacity = inital_size;
				buffer_start = _buffer_pool.at();
				if (!_buffer_pool.allocate(inital_size, ptr))
					buffer_start = nullptr;
			};

			size_t capacity() const {
				return _capacity;
			}

			size_t trim_end(char c, size_t end) {
				auto p = ptr + end - 1;
				auto bp = p;
				while (*p==c) {
					*(p--) = '\0';
				}
				return bp - p;
			}

			void resize(size_t new_size) {
				if (new_size <= _capacity)
					return;
				if (!buffer_start) {
					ptr = static_cast<char *>(realloc(ptr, new_size * sizeof(char)));
					_capacity = new_size;
				} else if (!_buffer_pool.reallocate(buffer_start, new_size, ptr))
					buffer_start = nullptr;
				_capacity = new_size;
			}

			char* data() const { return ptr; }
		};
		class command_parser {
		private:
			lesh_state &_lesh_state;
			enum class parsing_state {
				none = 0,
				in_a_word = 1,
			};

			SimpleParsingStare &_state;
			ASTPipe &_pipe;
			const alias_container &_aliases;

			const executor &_executor;
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
					if (ASTPipe aliased_pipe; _is_command && _aliases.try_get_alias(word_begin, aliased_pipe)) {
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
			static ASTPipe& parse(lesh_state& lesh_state, SimpleParsingStare &state, const executor& bc, const alias_container &aliases, ASTPipe &pipe, ASTCommand* command) {
				auto p = command_parser(lesh_state, state, bc, aliases, pipe, command);
				return p.parse<is_executing>();
			}
			template<bool is_executing>
			static ASTPipe& parse(lesh_state& lesh_state, SimpleParsingStare &state,const executor& bc, const alias_container &aliases, ASTPipe &pipe) {
				auto p = command_parser(lesh_state, state, bc, aliases, pipe);
				return p.parse<is_executing>();
			}
			command_parser(lesh_state& lesh_state, SimpleParsingStare &state, const executor& executor, const alias_container &aliases, ASTPipe &pipe) :
			command_parser(lesh_state, state, executor, aliases, pipe, pipe.emplace_command()) {
			}

			command_parser(lesh_state& lesh_state, SimpleParsingStare &state, const executor& executor, const alias_container &aliases, ASTPipe &pipe, ASTCommand* command) :
					_state(state), _pipe(pipe), _aliases(aliases), _is_command(true), _lesh_state(lesh_state),
					_p_state(parsing_state::none), _word_beginning(nullptr), _bracket_start(nullptr), _command(command), _executor(executor) {
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
										std::string_view before =  (_p_state == parsing_state::in_a_word)? std::string_view(_word_beginning, c - _word_beginning) : std::string_view();
										_state.plusplus();
										_state.plusplus();
										int pipe_fd[2];
										{
											auto sub_input = _state.match_charter(')');
											auto sub_parsing_state = _state.sub_state(sub_input);
											auto sub_pipe = ASTPipe{};
											parse<is_executing>(_lesh_state, sub_parsing_state, _executor, _aliases, sub_pipe);
											pipe(pipe_fd); // todo handle error
											_executor.execute(sub_pipe, 0, pipe_fd[1]);

											// execute(_build_in_commands, sub_pipe, 0, pipe_fd[1]);
											close(pipe_fd[1]);
										}
										transfarable_buffer tb = { _state.pool(), SUBSHELL_BUFFER_INITIAL_SIZE };
										size_t total = 0;
										{
											size_t free_space = tb.capacity();
											size_t bytes_read = 0;

											while ((bytes_read = read(pipe_fd[0], tb.data() + total, free_space)) > 0) {
												total += bytes_read;
												free_space -= bytes_read;
												if (free_space < tb.capacity() - (SUBSHELL_BUFFER_INITIAL_SIZE/2))
													tb.resize(tb.capacity() * 2);
											}
										}
										close(pipe_fd[0]);
										// TODO: handle empty output or output without that doesnt end in a new line
										total -= tb.trim_end('\n', total);
										_state.plusplus();
										auto after = _state.parse_word();

										// detached
										if (before.empty() && after.empty()) {
											auto sub_result_input = _state.sub_state(tb.data(), total);
											auto sub_result_pipe = ASTPipe{};
											parse<false>(_lesh_state, sub_result_input, _executor, _aliases, _pipe, _command);
										} else {
											auto input_data = _state.rent(before, std::string_view(tb.data(), total), after);
											auto sub_result_input = _state.sub_state(input_data);
											auto sub_result_pipe = ASTPipe{};
											parse<false>(_lesh_state, sub_result_input, _executor, _aliases, _pipe, _command);
											// TODO: attached
										}
										_p_state = parsing_state::none;
										c = _state.current();


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
;

		void add_alias(const char* alias, char* value) {
			auto state = SimpleParsingStare(true, _lesh_state.global_pool(), value);
			ASTPipe pipe;
			command_parser::parse<false>(_lesh_state, state, _executor, _aliases, pipe);
			_aliases.emplace_alias(alias, pipe);
		}

		// Execute a single command
		pid_t static execute_command(const built_in_commands& built_ins, ASTCommand *command, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
			if (built_ins.try_execute_built_in(command, input_fd, output_fd))
				return 0;

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

				// Execute command
				execvp(argv[0], argv);


				// If execvp fails
				perror("execvp");
				exit(EXIT_FAILURE);
			}

			// Parent process
			return pid;
		}

		std::vector<pid_t> static execute_pipeline(const built_in_commands& built_ins, const ASTPipe &pipeline, int p_input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
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
				pids.push_back(execute_command(built_ins, pipeline.commands[i], input_fd, pipefd[1]));

				// Close write end of pipe
				close(pipefd[1]);

				if (input_fd != STDIN_FILENO)
					close(input_fd);

				// Next command will read from this pipe
				input_fd = pipefd[0];
			}

			pids.push_back(execute_command(built_ins, pipeline.commands[last_child_index], input_fd, output_fd));
			if (input_fd != STDIN_FILENO)
				close(input_fd);

			return pids;
		}

	public:
		explicit inline Parser(lesh_state& state): _lesh_state(state), _executor(state), _aliases() {}

		void init_aliases() {
			// add_alias("grep", "grep --color=auto --exclude-dir={.bzr,CVS,.git,.hg,.svn,.idea,.tox,.venv,venv}"),
			add_alias("l", "ls -lah");
			add_alias("ls", "ls -G");
			_aliases.normalize_aliases();
		}

		inline void parse_and_execute(std::string& input) {
			auto state = SimpleParsingStare{false, _lesh_state.buffer_pool(), input}; // ExpansionContainer{input};
			auto pipe = ASTPipe{};
			command_parser::parse<true>(_lesh_state, state, _executor, _aliases, pipe);
			_executor.execute(pipe);
		}

		inline static void a_execute(const built_in_commands& built_ins, const ASTPipe &pipe, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
			switch (pipe.size()) {
				// TODO: maybe error handling
				case 0: break;;
				case 1: {
					if (const auto pid = execute_command(built_ins, pipe.commands[0], input_fd, output_fd)) {
						int status;
						waitpid(pid, &status, 0);
					}
				} break;;
				default: {
					// Wait for all processes in pipeline
					for (const auto pids = execute_pipeline(built_ins, pipe, input_fd, output_fd); const pid_t pid: pids) {
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
