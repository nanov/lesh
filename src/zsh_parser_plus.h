#pragma once

#include <array>
#include <sys/_types/_ssize_t.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

namespace ZshParserPlus {
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

			inline ASTNode(Type t, const char *v = nullptr)
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
			std::array<ASTWord, MAX_CHILDREN> children;
			std::array<size_t, MAX_CHILDREN> children_idx;
			std::array<const char*, MAX_CHILDREN> references;

			size_t num_children;
			size_t num_references;

			inline void addChild(ASTWord child, size_t idx) {
				// TODO: Handle bigger number
				if (num_children < MAX_CHILDREN) {
					children[num_children++] = child;
					children_idx[num_children] = idx;
				}
			}

			inline void add_reference(const char* ref) {
				// TODO: Handle bigger number
				if (num_references < MAX_CHILDREN)
					references[num_references++] = ref;
			}
		};

		struct ASTPipe {
			static constexpr size_t MAX_CHILDREN = 32;
			std::array<ASTCommand *, MAX_CHILDREN> commands;
			size_t num_children;

			inline void addChild(ASTCommand *child) {
				// TODO: Handle bigger number
				if (num_children < MAX_CHILDREN)
					commands[num_children++] = child;
			}
		};

	private:
		/*
		char *input_;
		size_t length_;
		size_t pos_;
		*/

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


		struct ExpansionContainer {
		public:
			inline ExpansionContainer(
				char* input, size_t input_length, size_t buffer_size, size_t original_idx, size_t original_length):
					input(input), length(input_length), start_idx(original_idx), original_length(original_length), pos(0), current(&input[0]), capacity(buffer_size) {
				ropes_size = 0;
				ropes[ropes_size++] = {input, input_length, capacity };
				rope_current = &input[0];
			}
			inline ExpansionContainer(const std::string& input, size_t original_idx, size_t original_length): ExpansionContainer(const_cast<char*>(input.data()), input.capacity(), input.length()+1, original_idx, original_length) {}
			inline ExpansionContainer(char* input, size_t input_length, size_t buffer_size): ExpansionContainer(input, input_length, buffer_size, 0, input_length) {}
			inline explicit ExpansionContainer(const std::string& input): ExpansionContainer(const_cast<char*>(input.data()), input.length(), input.capacity()) {}

		protected:
			inline size_t main_pos() {
				return pos_main_value;
			}
		private:
			// rope-stuff, welcome to a new world
			std::array<ExpansionPart, MAX_STATES> ropes;
			size_t ropes_size = 0;
			size_t current_rope = 0;

			size_t rope_pos = 0;
			size_t current_rope_pos = 0;
			char* rope_current;

			// -----
			[[nodiscard]] const ExpansionPart& rope_current_rope() const noexcept { return ropes[current_rope]; }

			char *input;

			size_t start_idx = 0;
			size_t length;

			size_t original_length;
			size_t capacity;

			size_t pos;
			char* current;

			size_t pos_main_value = 0;
			size_t pos_inside_expansion = 0;

			size_t current_expansion = SIZE_MAX;
			bool inside_expansion = false;
			std::array<ExpansionContainer*, 32> expansions;
			size_t number_of_expansions = 0;

		public:
			struct collide_result {
					enum class collide_type: bool {
						FULL,
						PARTIAL,
					};
				collide_type type: 1 = collide_type::FULL;
				ssize_t collide_info: sizeof(size_t) - 3 = 0;
			};

			bool collide(size_t pos1, size_t len1, size_t pos2, size_t len2, collide_result &result) {
				size_t end1 = pos1 + len1;
				size_t end2 = pos2 + len2;

				// If first object ends before second starts
				// or second object ends before first starts
				if (end1 <= pos2 || end2 <= pos1)
					return false;

				// If object2 is fully contained in object1
				if (pos2 >= pos1 && end2 <= end1)
					return false;

				// If object1 is fully contained in object2
				if (pos1 >= pos2 && end1 <= end2) {
					result = collide_result(collide_result::collide_type::FULL, pos1-pos2);
					return true;
				}

				// Partial overlap cases
				if (pos2 >= pos1) {
					// obj2 starts inside obj1 but ends after
					result = collide_result(collide_result::collide_type::PARTIAL, (ssize_t)(end2 - end1));
					return true;
				}

				// obj1 starts before obj2 but ends inside
				result = collide_result(collide_result::collide_type::PARTIAL, (ssize_t)(pos1 - pos2));
				return true;
			}

			bool skip_whitespace() {
				auto c = *at();
				while (c == '\0' || std::isspace(c)) {
					set_null_terminator();
					if (!advance(1, c))
						return false;
				}
				return is_end();
			}

			void rope_add_expansion(char* expansion, size_t expansion_size, size_t capacity, size_t insert_at, size_t replace_size) {
				// Find starting segment
				size_t part_idx = 0;
				size_t start_at = 0;
				while (part_idx < ropes_size &&
				       (start_at + ropes[part_idx].size) <= insert_at)
					start_at += ropes[part_idx++].size;

				return rope_add_expansion(expansion, expansion_size, capacity, replace_size, part_idx, insert_at - start_at);
			}

			inline void rope_add_expansion(const std::string& expansion, size_t replace_size) {
				return rope_add_expansion(const_cast<char*>(expansion.data()), expansion.size(), expansion.capacity(), replace_size);
			}
			inline void rope_add_expansion(char* expansion, size_t expansion_size, size_t capacity, size_t replace_size) {
				return rope_add_expansion(expansion, expansion_size, capacity, replace_size, current_rope, current_rope_pos);
			}
			// TODO: Keep somehow references for free() calls
			// TODO: cover all cases
			void rope_add_expansion(char* expansion, size_t expansion_size, size_t capacity, size_t replace_size, size_t part_idx, size_t local_pos) {
				// Find how many segments we need to remove
				size_t remove_count = 0;
				ssize_t end_pos = (ssize_t)ropes[part_idx].size - local_pos - replace_size;
				size_t scan_idx = part_idx;
				while (end_pos < 0) {
					end_pos += ropes[scan_idx++].size;
					remove_count++;
				}


				ExpansionPart& current = ropes[part_idx];
				auto new_slots = 0;

				if (local_pos > 0) {
					current.size = local_pos;
					new_slots++;
				}
				if (end_pos > 0) {
					new_slots++;
				}

				// Then make space and insert new part
				if (new_slots) {
					memmove(&ropes[part_idx + new_slots], &ropes[part_idx], (ropes_size - part_idx) * sizeof(ExpansionPart));
					part_idx += new_slots - 1;
				}


				ropes[part_idx] = {expansion, expansion_size,  capacity};
				ropes_size += new_slots;
				length += expansion_size - replace_size;

				current_rope = part_idx;
				current_rope_pos = 0;
				rope_current = expansion;

				// Adjust next part to skip replace_size
				if (part_idx + 1 < ropes_size) {
					ExpansionPart& next = ropes[part_idx + 1];
					next.value += replace_size + local_pos;
					next.size = end_pos;
					next.capacity -= local_pos;
				}
			}

			void move_to(size_t position) {
				rope_pos = position;
			}

			ssize_t add_expansion(char* expansion, size_t expansion_length, size_t capacity, size_t insert_at, size_t replace_size) {
				insert_at = insert_at - start_idx;

				// TODO: Code to adjust buffer
				if (number_of_expansions >= 32)
					return 0;

				ExpansionContainer* in_expansion;
				if (try_expansion(&in_expansion)) {
					auto adjustments = in_expansion->add_expansion(expansion, expansion_length, capacity, insert_at, replace_size);
					length += adjustments;
					current = at();
					return adjustments;
				}


				collide_result result;
				if (in_expansion && collide(insert_at, expansion_length, in_expansion->pos, in_expansion->length, result)) {
					switch (result.type) {
						case (collide_result::collide_type::FULL): {
							// replace one with the other
							return 0;
						} break;
						case (collide_result::collide_type::PARTIAL): {
							// handle
							return 0;
						} break;
					}
				}

				ExpansionContainer* ex = (expansions[number_of_expansions++]
					= new ExpansionContainer(expansion, expansion_length, capacity, insert_at, replace_size));

				auto adjustments = static_cast<ssize_t>(expansion_length - replace_size);
				length += adjustments;
				try_next_expansion(&ex, pos_main_value);
				current = at();
				return adjustments;
			}

			bool is_end() {
				return pos <= length - 1;
			}

			char* at() {
				ExpansionContainer* ex;
				if (try_expansion(&ex))
					return ex->at();

				return current;
			}

			inline size_t rope_advance(size_t by_how_much, char*& at) {
				auto cur = rope_current_rope();
				// we are still at the same rope
				if (current_rope_pos + by_how_much < cur.size) {
					current_rope_pos += by_how_much;
					rope_pos += by_how_much;
					at = (rope_current += by_how_much);
					return by_how_much;
				}
				size_t left_in_rope = cur.size - current_rope_pos;
				current_rope_pos=0;
				// let's find in which rope we land
				do {
					by_how_much -= left_in_rope;
					rope_pos += left_in_rope;
					cur = ropes[++current_rope];

					if (by_how_much < cur.size) {
						current_rope_pos = by_how_much;
						rope_pos += by_how_much;
						at = rope_current = cur.value + current_rope_pos;
						return by_how_much;
					}

					left_in_rope = cur.size;
				} while (true);
				return 0;
			}

		private:
			inline size_t advance_expansion(size_t by_how_much, char*& c) noexcept {
				if (by_how_much+pos >= length)
					return false;

				c = current;

				if (!number_of_expansions) {
					pos_main_value += by_how_much;
					pos += by_how_much;
					current = &input[pos_main_value];
					c=current;
					return by_how_much;
				}

				ExpansionContainer* expansion;
				auto ex_opt = try_expansion(&expansion);
				while (by_how_much > 0) {
					// no more extensions
					if (!ex_opt) {
						if (!expansion) {
							pos += by_how_much;
							pos_main_value += by_how_much;
							current = &input[pos_main_value];
							c = current;
							return by_how_much;
						}

						auto raw_index = pos_main_value + by_how_much;
						if (raw_index < expansion->start_idx) {
							pos_main_value = raw_index;
							pos += by_how_much;
							current = input + raw_index;
							c = current;

							return by_how_much;
						}

						if (raw_index >= expansion->start_idx + expansion->original_length) {
							auto d1 = (expansion->start_idx - pos_main_value) + expansion->length;
							by_how_much -= d1;
							pos += d1;
							pos_main_value = expansion->start_idx + expansion->original_length;
							ex_opt = try_next_expansion(&expansion, pos_main_value); // let's try again
							continue;
						}
						// the only thing left is that we are inside an expansion
						pos_inside_expansion = raw_index - expansion->start_idx;
						by_how_much -= pos_inside_expansion;
						pos += by_how_much;
						pos_main_value += by_how_much;
						inside_expansion = true;
						ex_opt = true;
					}

					if (pos_inside_expansion + by_how_much < expansion->length) {
						pos_inside_expansion += by_how_much;
						pos += by_how_much;
						return expansion->advance_expansion(by_how_much,  c);
					} else {
						auto remaning_inside_expansion = expansion->length - pos_inside_expansion;
						pos += remaning_inside_expansion;
						pos_main_value = expansion->start_idx + expansion->original_length;
						by_how_much -= remaning_inside_expansion;
						ex_opt = try_next_expansion(&expansion, pos_main_value);
						if (!ex_opt) {
							current = &input[pos_main_value];
						} else {
							current = expansion->at();
						}
						c=current;
					}
				}
				return true;
			}

			inline bool try_expansion(ExpansionContainer** expansion) {
				if (current_expansion >= number_of_expansions) {
					*expansion = nullptr;
					return false;
				}
				*expansion = expansions[current_expansion];
				return inside_expansion;
			}

			inline bool try_next_expansion(ExpansionContainer** expansion, size_t raw_pos) {
				current_expansion++;
				inside_expansion = false;
				if (current_expansion >= number_of_expansions) {
					*expansion = nullptr;
					return false;
				}
				*expansion = expansions[current_expansion];
				inside_expansion = (*expansion)->start_idx <= raw_pos && (*expansion)->start_idx + (*expansion)->original_length > raw_pos;
				if (inside_expansion)
					pos_inside_expansion = raw_pos - (*expansion)->start_idx;
				return inside_expansion;
			}


		public:
			inline void set_null_terminator() {
				*at() = '\0';
			}

			inline bool advance(size_t by_how_much, char*& c) noexcept {
				return advance_expansion(by_how_much, c);
			}

			inline bool advance(size_t by_how_much, char& c) noexcept {
				char* p;
				auto res = ExpansionContainer::advance_expansion(by_how_much, p);
				c = *p;
				return res;
			}

			inline bool advance(char& c) noexcept {
				return advance(1, c);
			}

			inline bool advance(char*& c) noexcept {
				return advance(1, c);
			}


		private:
			static size_t trim_end(std::string& str) {
				str.resize(trim_end(str.data(), str.length()));
				return str.size();
			}
			static size_t trim_end(char* str, size_t length) {
				auto s_end = str + (length - 1);
				char* c;
				for (c = s_end; c >= str && *c == '\0'; --c) {}
				return length - (s_end - c);
			}

		};

		struct ParsingState: public ExpansionContainer {

			constexpr static  size_t MAX_CHILDREN = 32;
			// char* input;
			// char current;
			// size_t length;
			// size_t capacity;
		};

		inline static bool skipWhitespace(ExpansionContainer& state) {
			char c = *state.at();
			while (c == '\0' || std::isspace(c)) {
				state.set_null_terminator();
				if (!state.advance(1, c))
					return false;
			}
			return state.is_end();
		}

		inline ASTNode *createNode(ASTNode::Type type, const char *value = nullptr) {
			return new ASTNode(type, value); // Memory from pool in full implementation
		}

		inline static ASTWord parse_word(ExpansionContainer& state) {
			// size_t word_start = state.pos;
			char* word = state.at();

			char c = state.at()[0];
			while (
			       !std::isspace(c) &&
			       !isSpecial(c))
				if (!state.advance( c))
					break;

			state.set_null_terminator();
			/*
			if (pos_ < length_)
			    input_[pos_] = '\0';
			    */

			return ASTWord{word};
		}


		inline size_t next_closing_paren(ParsingState& state) {
			// auto pos = state.pos;
			// TODO: Handle Nested
			size_t nested = 0;
			// while (++pos < state.length)
			// 	// if (state.input[pos] == '(')
			// 	if (state.input[pos] == ')')
			// 		return pos;
			return 0;
		}

		class TransferableBuffer {
			char* ptr = nullptr;
			size_t capacity = 0;

		public:
			TransferableBuffer(size_t initial_size) {
				ptr = (char*)malloc(initial_size * sizeof(char));
				capacity = initial_size;
			};

			void resize(size_t new_size) {
				if (new_size > capacity) {
					ptr = (char*)realloc(ptr, new_size * sizeof(char));
					capacity = new_size;
				}
			}

			char* data() { return ptr; }

			// Prevent copying
			TransferableBuffer(const TransferableBuffer&) = delete;
			TransferableBuffer& operator=(const TransferableBuffer&) = delete;
		};


		inline ASTPipe *parseCommand(ExpansionContainer& state) {
			auto ast_pipe = new ASTPipe();
			auto command = new ASTCommand();

			while (skipWhitespace(state)) {
				auto c = *state.at();// state.input[state.pos];
				if (isSpecial(c)) {
					switch (c) {
						case '|': {
							ast_pipe->addChild(command);
							command = new ASTCommand();
						} break;
						case '$': {
							// // you need at least opening bracket and closing one plus null terminator
							// if (state.pos + 3 >= state.length)
							// 	break;
							//
							// if (state.input[++state.pos] != '(')
							// 	break;
							//
							// state.pos++;
							//
							// auto closing_paren_pos = next_closing_paren(state);
							// auto sub_shell_parsing_state = ParsingState((state.input + state.pos), closing_paren_pos - state.pos - 1);//   { .input = (state.input + state.pos), .length = (), .pos = 0u };
							//
							// state.input[closing_paren_pos] = '\0';
							//
							// auto sub_shell_pipe = parseCommand(sub_shell_parsing_state);
							//
							// int pipefd[2];
							// pipe(pipefd);
							// execute(sub_shell_pipe, STDIN_FILENO, pipefd[1]); // executeCommand(sub_shell_pipe->commands[0], STDIN_FILENO, pipefd[1]);
							// close(pipefd[1]);
							//
							//
							// constexpr static size_t CHUNK_SIZE = 128;
							//
							// auto transferable_buffer = TransferableBuffer(CHUNK_SIZE + 1); // +1 null terminator
							// ssize_t bytes_read = 0;
							// ssize_t total_bytes = 0;
							// while (true) {
							// 	bytes_read = read(pipefd[0], transferable_buffer.data() + total_bytes, CHUNK_SIZE);
							// 	// TODO: Handle error
							// 	if (bytes_read < 0) break;
							// 	if (bytes_read == 0) break;
							// 	total_bytes += bytes_read;
							// 	if (bytes_read < static_cast<ssize_t>(CHUNK_SIZE)) break;
							// }
							// close(pipefd[0]);
							//
							// transferable_buffer.data()[total_bytes] = '\0';
							// command->add_reference(transferable_buffer.data());
							//
							// command->addChild({  transferable_buffer.data() }, state.pos);
							// state.pos = closing_paren_pos;
						}
					}

					// if (c == '>' || c == '<') return parseRedirect(node);
					state.advance(1, c);
					continue;
				}

				// auto idx = state.pos;
				command->addChild(parse_word(state), 0);
			}

			ast_pipe->addChild(command);

			return ast_pipe;
		}

		// Execute a single command
		pid_t executeCommand(ASTCommand *command, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
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

				// // Handle redirections
				// handleRedirections(cmd->redirections);
				//
				// // Prepare arguments
				// ArgVector argv(cmd->command, cmd->arguments);

				// TODO: Add error handling and bounds checking
				auto command_list = reinterpret_cast<char **>(command->children.data());

				// Execute command
				execvp(command_list[0], command_list);

				// If execvp fails
				perror("execvp");
				exit(EXIT_FAILURE);
			}

			// Parent process
			return pid;
		}



		std::vector<pid_t> execute_pipeline(const ASTPipe *pipeline, int p_input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
			// TODO: use pool
			std::vector<pid_t> pids;
			int input_fd = p_input_fd;
			int pipefd[2];


			// TODO: Handle no size 0
			size_t last_child_index = pipeline->num_children - 1;

			for (size_t i = 0; i < last_child_index; ++i) {
				// Create pipe
				if (pipe(pipefd) == -1) {
					throw std::runtime_error("Pipe creation failed");
				}

				// Execute command with current input and pipe output
				pids.push_back(executeCommand(pipeline->commands[i], input_fd, pipefd[1]));

				// Close write end of pipe
				close(pipefd[1]);

				if (input_fd != STDIN_FILENO)
					close(input_fd);

				// Next command will read from this pipe
				input_fd = pipefd[0];
			}

			pids.push_back(executeCommand(pipeline->commands[last_child_index], input_fd, output_fd));
			if (input_fd != STDIN_FILENO)
				close(input_fd);

			return pids;
		}

	public:
		explicit inline Parser() {}

		inline ASTPipe *parse(const std::string& input) {
			auto si = std::string("l | cat | mitko");
			auto ex = std::string("ls");
			auto s = ExpansionContainer(si);
			char* c;
			s.rope_advance(2,c);
			s.rope_add_expansion("|ls|", 1);
			s.rope_advance(2,c);






			auto state = ExpansionContainer{input};
			return parseCommand(state);
		}

		inline void execute(ASTPipe *pipe, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
			auto si = std::string("l | cat | mitko");
			auto ex = std::string("ls");
			auto s = ExpansionContainer(si);
			s.rope_add_expansion(ex.data(), ex.size(), ex.capacity(), 1, 1);

			char c;
			while (s.advance(1, c)) {
				printf("%c", c);
			}
			printf("\n");
			printf("------\n");
			printf("------\n");
			printf("------\n");

			switch (pipe->num_children) {
				// TODO: maybe error handling
				case 0: return;
				case 1: {
					auto pid = executeCommand(pipe->commands[0], input_fd, output_fd);
					int status;
					waitpid(pid, &status, 0);
				}
					return;
				default: {
					auto pids = execute_pipeline(pipe, input_fd, output_fd);

					// Wait for all processes in pipeline
					for (pid_t pid: pids) {
						int status;
						waitpid(pid, &status, 0);
					}
				}
					return;
			}
		}
	};
} // namespace ZshParserPlus
