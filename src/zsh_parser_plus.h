#pragma once

#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

#include "utils.h"
#include "executor.h"
#include "lesh_string_utils.h"

#include <ranges>
#include <sol/as_args.hpp>
#include <sol/variadic_args.hpp>


namespace ZshParserPlus {
	// forward declaration
	class Parser;
	class built_in_commands {
	private:
		lesh_state &_state;
		// Define a type for builtin command functions
		using builtin_t = int (*)(lesh_state& state, const ASTCommand* cmd);
		std::unordered_map<std::string, builtin_t> builtins;

		static int builtin_typeset(lesh_state& state, const ASTCommand* cmd) {
			if (cmd->children.size() < 2)
				return 0;


			auto key = cmd->children[1]->value;
			int mode=0;
			if (cmd->children.size() > 2) {
				if (*key == '-') {
					if (*(key+1) == 'A' && *(key+2) == '\0') {
						mode = 1;
					} else if (*(key+1) == 'a' && *(key+2) == '\0') {
						mode = 2;
					} else {
						return 1;
					}
					key = cmd->children[2]->value;
				}
			}
			char* val = const_cast<char*>(key);
			while (*val != '=' && *val != '\0')
				val++;

			if (*val == '=')
				*val++ = '\0';

			auto s = state.scope();
			if (mode == 1)
				s->typeset_map(std::string_view(key), std::string_view(val));
			return 1;
		}

		static int builtin_export(lesh_state& state, const ASTCommand* cmd) {
			if (cmd->children.size() < 2)
				return 0;

			auto key = cmd->children[1]->value;
			char* val = const_cast<char*>(key);
			while (*val != '=')
				val++;

			*val++ = '\0';
			auto s = state.scope();
			s->typeset(std::string_view(key), std::string_view(val));
			return 1;
		}

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
		static int builtin_lua(lesh_state& state, const ASTCommand* cmd) {
			return 0;
		};

	public:
		explicit built_in_commands(lesh_state& state): _state(state) {
			builtins.emplace("cd", builtin_cd);
			builtins.emplace("echo", builtin_echo);
			builtins.emplace("export", builtin_export);
			builtins.emplace("typeset", builtin_typeset);
			builtins.emplace("info", builtin_info);
		}

		bool try_execute_built_in(const ASTCommand* cmd, int input_fd, int output_fd, int& status) const {
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


				status = it->second(_state, cmd);

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
	class lua_engine {
	public:
		lua_engine(lesh_state& state, Parser& parser);
		bool try_execute_lua_function(const ASTCommand* cmd, int input_fd, int output_fd) const;

	private:
		Parser& _parser;
		lesh_state& _lesh_state;
		sol::state _lua;
		sol::environment _base_env;
		void init();
	};

	class executor {
		public:
			explicit executor(lesh_state& lesh_state, Parser& parser) : _built_ins({lesh_state}), _lua_engine(lesh_state, parser) {}

			// Returns the exit status of the last command, which is what `$?` and a
			// non-interactive shell's own exit status are defined to be.
			int execute(const ASTPipe& pipe, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) const {
				int last_status = 0;
				switch (pipe.size()) {
					// TODO: maybe error handling
					case 0: break;
					case 1: {
						if (const auto pid = execute_command(pipe.commands[0], input_fd, output_fd, last_status)) {
							int status = 0;
							waitpid(pid, &status, 0);
							last_status = WIFEXITED(status) ? WEXITSTATUS(status)
							            : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
							            : 0;
						}
					} break;
					default: {
						// Wait for every process, but the pipeline's status is the last one's.
						for (const auto pids = execute_pipeline(pipe, last_status, input_fd, output_fd); const pid_t pid: pids) {
							if (pid) {
								int status = 0;
								waitpid(pid, &status, 0);
								last_status = WIFEXITED(status) ? WEXITSTATUS(status)
								            : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
								            : 0;
							}
						}
					}
				}
				return last_status;
			}
		private:
			built_in_commands _built_ins;
			lua_engine _lua_engine;

			// last_in_process_status receives the status of the final command when it
			// runs in this process (a built-in), where there is no pid to wait on.
			[[nodiscard]] std::vector<pid_t> execute_pipeline(const ASTPipe &pipeline, int& last_in_process_status, int p_input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) const {
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
					int ignored_status = 0;
					pids.push_back(execute_command(pipeline.commands[i], input_fd, pipefd[1], ignored_status));

					// Close write end of pipe
					close(pipefd[1]);

					if (input_fd != STDIN_FILENO)
						close(input_fd);

					// Next command will read from this pipe
					input_fd = pipefd[0];
				}

				pids.push_back(execute_command(pipeline.commands[last_child_index], input_fd, output_fd, last_in_process_status));
				if (input_fd != STDIN_FILENO)
					close(input_fd);

				return pids;
			}
			pid_t execute_command(ASTCommand *command, int input_fd, int output_fd, int& in_process_status) const {
				if (_built_ins.try_execute_built_in(command, input_fd, output_fd, in_process_status))
					return 0;
				if (_lua_engine.try_execute_lua_function(command, input_fd, output_fd)) {
					in_process_status = 0;
					return 0;
				}

				// Built-ins above run in *this* process, so their output sits in our
				// stdout buffer. Flush before forking, otherwise the child inherits a
				// copy of that buffer and prints it again when it exits.
				fflush(nullptr);

				pid_t pid = fork();

				if (pid == -1) {
					throw std::runtime_error("Fork failed");
				}

				if (pid == 0) {
					// Child process
					// Setup
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
					// auto argv = reinterpret_cast<char**>(const_cast<ASTWord*>(command->children.data_null_terminated()));

					command->assignments.foreach([](auto& asg) -> void {
						setenv(asg.key, asg.value, true);
					});

					auto argv = command->args_null_terminated();

					// Execute command
					execvp(argv[0], argv);


					// If execvp fails
					// important, in order to get, my)_stupid_command: not found
					perror(argv[0]);

					// POSIX: 127 if the command was not found, 126 if it was found but
					// could not be executed.
					// _exit, not exit: never flush stdio buffers inherited from the parent.
					_exit(errno == ENOENT ? 127 : 126);
				}

				// Parent process
				return pid;
			}
	};

	class Parser {
	private:

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

		struct SimpleParsingState {
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
			explicit SimpleParsingState(bool is_global, buffer_pool& pool, char *input, size_t length) : _is_global(is_global), _buffer_pool(pool), _buffer_start(pool.at()), _input(input), _length(length+1), _current(_input) {}
			SimpleParsingState(bool is_global, buffer_pool& pool, const char *input) : SimpleParsingState(is_global, pool, strdup(input), strlen(input)) {}
			explicit SimpleParsingState(bool is_global, buffer_pool& pool, std::string& input):  SimpleParsingState(is_global, pool, input.data(), input.size()) {}

			~SimpleParsingState() {
				if (_is_global)
					return;

				_buffer_pool.reset(_buffer_start);
				_to_free.foreach([](auto* ptr) { free(ptr); });
			}


			[[nodiscard]] SimpleParsingState sub_state(char* data, size_t length) const {
				return SimpleParsingState(false, _buffer_pool, data, length);
			}

			[[nodiscard]] SimpleParsingState sub_state(std::string_view input) const {
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

			char* plusplus() {
				_position++;
				return ++_current;
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

		inline std::string_view match_until(const char bracket = ')') {
			char* word = _current;

			char c = *_current;
			while (_position < _length &&
				     c != '\0' &&
				     c != bracket) {
				plusplus();
				c = *_current;
			}
			*_current = '\0';

			return std::string_view(word, _current-word);
		}
		bool parse_var_substitution_until(const char bracket, std::string_view* output) {
			const char * word = _current;

			char c = *_current;
			if (!lesh::string_utils::is_valid_var_name_first_char(c))
				return false;

			plusplus();
			c = *_current;

			while (c != bracket) {
				if (_position == _length)
					return false;
				if (!lesh::string_utils::is_valid_var_name_non_first_char(c))
					return false;
				plusplus();
				c = *_current;
			}
			*_current = '\0';

			*output = std::string_view(word, _current-word);
			return true;
		}

		bool parse_word_until_key(std::string_view* output) {
			const char * word = _current;

			char c = *_current;
			while (_position < _length &&
				     c != '\0' &&
			       !std::isspace(c) &&
			       !is_special(c) &&
			       c != '[') {
				plusplus();
				c = *_current;
			}
			*_current = '\0';

			*output = std::string_view(word, _current-word);
			return (c=='[');
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
		class transferable_buffer {
		private:
			buffer_pool& _buffer_pool;
			char* buffer_start = nullptr;
			char* ptr = nullptr;
			size_t _capacity = 0;

		public:
			transferable_buffer(buffer_pool& pool, size_t initial_size): _buffer_pool(pool) {
				_capacity = initial_size;
				buffer_start = _buffer_pool.at();
				if (!_buffer_pool.allocate(initial_size, ptr))
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
				had_word = 2,
			};

			SimpleParsingState &_state;
			ASTPipe &_pipe;

			const executor &_executor;
			ASTCommand* _command;
			bool _is_command;
			parsing_state _p_state;
			char* _word_beginning;
			char* _word_end;
			std::string_view _last_word;
			char _expected_closing_bracket = 0;
			char* _bracket_start;
			bool _is_escape = false;

			//
		private:
			void ensure_last_word(char* c) {
				if (_p_state == parsing_state::in_a_word && _last_word.empty()) {
					*c = '\0';
					_last_word = std::string_view(_word_beginning,  c - _word_beginning);
				}
			}
			void add_to_last_word(std::string_view value) {
				if (!_last_word.empty()) {
					auto con = _state.rent(_last_word, value);
					_last_word = con;
				} else {
					_last_word = value;
				}
			}
			void commit_last_word(char* c) {
				if (_p_state == parsing_state::in_a_word && _last_word.empty()) {
					*c = '\0';
					_last_word = std::string_view(_word_beginning,  c - _word_beginning);
				}
				if (_last_word.empty())
					return;
				// _is_command = false;
				_command->emplace_child(_last_word.data());
				_last_word = std::string_view();
				_word_beginning = c;
				_p_state = parsing_state::none;
			}
			template<bool is_executing>
			void ensure_word(const char* word_begin, char *word_end) {
				if (_p_state != parsing_state::in_a_word)
					return;

				_p_state = parsing_state::none;
				*word_end = '\0';
				if constexpr (is_executing) {
					if (ASTPipe const * aliased_pipe; _is_command && _lesh_state.try_get_alias(word_begin, &aliased_pipe)) {
						_command = _pipe.merge(aliased_pipe);
					} else {
						// _word = word_begin;
						// _command->emplace_child(word_begin);
						_last_word = word_begin;
					}
					_is_command = false;
				} else {
					// _word = word_begin;
					_command->emplace_child(word_begin);
					_last_word = word_begin;
				}
				word_begin = nullptr;
			}
			std::string_view get_last_word() const {
				if (_p_state == parsing_state::in_a_word)
					return std::string_view(_word_beginning, _state.current() - _word_beginning);

				return _last_word;
			}
			static bool is_separator(char c) {
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
			static ASTPipe& parse(lesh_state& lesh_state, SimpleParsingState &state, const executor& bc, ASTPipe &pipe, ASTCommand* command) {
				auto p = command_parser(lesh_state, state, bc, pipe, command);
				return p.parse<is_executing>();
			}
			template<bool is_executing>
			static ASTPipe& parse(lesh_state& lesh_state, SimpleParsingState &state,const executor& bc, ASTPipe &pipe) {
				auto p = command_parser(lesh_state, state, bc, pipe);
				return p.parse<is_executing>();
			}
			command_parser(lesh_state& lesh_state, SimpleParsingState &state, const executor& executor, ASTPipe &pipe) :
			command_parser(lesh_state, state, executor, pipe, pipe.emplace_command()) {
			}

			command_parser(lesh_state& lesh_state, SimpleParsingState &state, const executor& executor, ASTPipe &pipe, ASTCommand* command) :
					_state(state), _pipe(pipe), _is_command(true), _lesh_state(lesh_state),
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
									if (char *next; !_state.peek(next) || is_separator(*next)) {
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
							case '(': {
								if (_p_state == parsing_state::in_a_word) {
									char *p;
									if (_state.peek(p) && *p == ')') {
										*c = '\0';
										_state.plusplus();
										if (*_state.plusplus() == '{') {
											_state.plusplus();
											auto sub_input = _state.match_until('}');
											auto sub_parsing_state = _state.sub_state(sub_input);
											auto sub_pipe = ASTPipe{};
											parse<false>(_lesh_state, sub_parsing_state, _executor, sub_pipe);

										}
									}
								}
							} break;
							case ' ':
							case '\0': {
								commit_last_word(c);
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
								auto words = _state.match_until('}');
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
										/*
										auto sub_input = _state.match_until(')');
										auto sub_parsing_state = _state.sub_state(sub_input);
										_state.plusplus();
										auto after = _state.parse_word();
										_p_state = parsing_state::none;
										c = _state.current();
										auto cmd = _command->emplace_child(nullptr);
										auto sub = _command->substitutions.emplace_back(before, after, sub_input.data(), new ASTPipe, _command->children.size() - 1);
										*/




										int pipe_fd[2];
										{
											auto sub_input = _state.match_until(')');
											auto sub_parsing_state = _state.sub_state(sub_input);
											auto sub_pipe = ASTPipe{};
											parse<is_executing>(_lesh_state, sub_parsing_state, _executor, sub_pipe);
											pipe(pipe_fd); // todo handle error
											_executor.execute(sub_pipe, 0, pipe_fd[1]);

											// execute(_build_in_commands, sub_pipe, 0, pipe_fd[1]);
											close(pipe_fd[1]);
										}
										transferable_buffer tb = { _state.pool(), SUBSHELL_BUFFER_INITIAL_SIZE };
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
											parse<false>(_lesh_state, sub_result_input, _executor, _pipe, _command);
										} else {
											auto input_data = _state.rent(before, std::string_view(tb.data(), total), after);
											auto sub_result_input = _state.sub_state(input_data);
											parse<false>(_lesh_state, sub_result_input, _executor, _pipe, _command);
											// TODO: attached
										}
										_p_state = parsing_state::none;
										c = _state.current();


										// TODO: handle subshell
									} else
									if (*p == '{') {
										std::string_view before =  (_p_state == parsing_state::in_a_word)? std::string_view(_word_beginning, c - _word_beginning) : std::string_view();
										_state.plusplus();
										_state.plusplus();
										std::string_view w; // = _state.parse_word();
										// TOTO: error_handling
										_state.parse_var_substitution_until('}',&w);
										_state.plusplus();
										auto after = _state.parse_word();
										std::string_view val;
										if (!_lesh_state.try_get_env(w, val))
											val = "";

										c = _state.current();
										if (before.empty() && after.empty()) {
											if (!val.empty()) {
												_p_state = parsing_state::in_a_word;
												ensure_word<false>(val.data(), c);
											}
										} else {
											auto input_data = _state.rent(before, val, after); // std::string_view(val.data(), val.size()));
											_p_state = parsing_state::in_a_word;
											ensure_word<is_executing>(input_data, c);
										}
									} else { // optimistic variable expansion
										std::string_view p =  (_p_state == parsing_state::in_a_word)? std::string_view(_word_beginning, c - _word_beginning) : std::string_view();
										_state.plusplus();
										std::string_view w; // = _state.parse_word();
										std::string_view k;
										auto has_key = _state.parse_word_until_key(&w);
										if (has_key) {
											_state.plusplus();
											// TODO: error handling
											k = _state.match_until(']');
										}

										// TODO: error handling
										if (std::string_view val; has_key ? _lesh_state.try_get_env(w, k, val) : _lesh_state.try_get_env(w, val)) {
											*c = '\0';
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
							// assigment
							case '=': {
								if (_is_command) {
									*c = '\0';
									_state.plusplus();
									if constexpr (is_executing) {
										if (_p_state == parsing_state::in_a_word) {
											auto var = _word_beginning;
											auto val = _state.parse_word();
											// _state.plusplus();
											_command->add_assignment(var, val.data());
											_p_state = parsing_state::none;
										}
									}
								}
							} break;

							// we are somewhere in a word
							default: {
								if (_p_state == parsing_state::none) {
									// commit word
									commit_last_word(c);
									_p_state = parsing_state::in_a_word;
									_word_beginning = c;
								}
							}
						}
					}
					if (!_state.plusplus_s()) {
						// TODO: handle non closing brackets
						// NOTE: end of input shouldn't be inside of a word as input is expceted to be null-terminated
						commit_last_word(c);
						// ensure_word<is_executing>(_word_beginning, c);
						break;
					}
				}
				return _pipe;
			}
		};
;

		void add_alias(const char* alias, const char* value, bool normalize_after = false) {
			auto state = SimpleParsingState(true, _lesh_state.global_pool(), value);
			ASTPipe& pipe = _lesh_state.emplace_alias(alias);
			command_parser::parse<false>(_lesh_state, state, _executor, pipe);
			if (normalize_after)
				_lesh_state.normalize_aliases();
		}

	public:
		explicit inline Parser(lesh_state& state): _lesh_state(state), _executor(state, *this) {}


		void init_aliases() {
			add_alias("l", "ls -lah");
			add_alias("ls", "ls -G", true);
		}

		void set_alias(const char* alias, const char* value) {
			add_alias(alias, value, true);
		}

		void set_alias_lazy(const char* alias, const char* value) {
			add_alias(alias, value);
		}

		inline int parse_and_execute(std::string& input) const {
			auto state = SimpleParsingState{false, _lesh_state.buffer_pool(), input}; // ExpansionContainer{input};
			auto pipe = ASTPipe{};
			command_parser::parse<true>(_lesh_state, state, _executor, pipe);
			return _executor.execute(pipe);
		}
	};

	inline lua_engine::lua_engine(lesh_state &state, Parser &parser):
		_lesh_state(state), _parser(parser) {
		auto st = sol::create;
		_base_env = sol::environment(_lua, sol::create);
		init();
	}
	inline bool lua_engine::try_execute_lua_function(const ASTCommand *cmd, int input_fd, int output_fd) const {
		const auto f_o = _base_env.get<std::optional<sol::function>>(cmd->children[0]->value);
		if (!f_o.has_value())
			return false;
		auto f = f_o.value();
		if (cmd->children.size() == 1) {
			f();
			return true;
		}
		const auto s = cmd->children.size();
		const auto argv = reinterpret_cast<char**>(const_cast<ASTWord*>(cmd->children.data())) + 1;
		auto it = char_iterable(argv, argv + s - 1);
		f(sol::as_args(it));
		return true;
	}


	static void hi(const char* in) {
		std::println("lua: {}", in);
	}
	static void hi_you(sol::variadic_args args) {
		std::print("lua_v: ");

		auto arg = args.begin();
		std::print("{}", arg->as<const char*>());
		for (++arg; arg != args.end(); ++arg)
			std::print(", {}", arg->as<const char*>());

		std::println("");
	}
	inline void lua_engine::init() {
		_base_env.set_function("hi", hi);
		_base_env.set_function("hi_you", hi_you);
		_base_env.create_named("lesh",
				"set_alias",  [&](const char* k, const char* v) { _parser.set_alias(k, v); },
				"set_alias_lazy",  [&](const char* k, const char* v) { _parser.set_alias_lazy(k, v); }
		);
	}


} // namespace ZshParserPlus
