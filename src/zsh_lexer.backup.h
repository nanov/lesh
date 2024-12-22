#ifndef ZSH_LEXER_H
#define ZSH_LEXER_H

#include <string>
#include <string_view>
#include <vector>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include "utils.h"

enum class TokenType {
    WORD,           // Regular words/arguments
    REDIRECTION,    // IO redirections like >, <, >>
    PIPE,           // |
    BACKGROUND,     // &
    SEMICOLON,      // ;
    COMMAND_SUB,    // $() or `command`
    VARIABLE,       // $VAR or ${VAR}
    GLOB_PATTERN,   // Wildcard patterns like *, ?, []
    QUOTE,          // Single or double quotes
    ESCAPE,         // Backslash escapes
    SUBSHELL_START, // (
    SUBSHELL_END,   // )
    BRACE_START,    // {
    BRACE_END,      // }
    ASSIGNMENT,     // VAR=value
    WHITESPACE,     // Spaces, tabs
    EOL             // End of line
};

struct Token {
    TokenType type;
    std::string value;
    size_t line;
    size_t column;

		std::string type_as_string() {
			switch (type) {
				case TokenType::WORD: return "WORD";
				case TokenType::REDIRECTION: return "REDIRECTION";
				case TokenType::PIPE  : return "PIPE,    ";
				case TokenType::BACKGROUND: return "BACKGROUND";
				case TokenType::SEMICOLON: return "SEMICOLON";
				case TokenType::COMMAND_SUB: return "COMMAND_SUB";
				case TokenType::VARIABLE: return "VARIABLE";
				case TokenType::GLOB_PATTERN: return "GLOB_PATTERN";
				case TokenType::QUOTE: return "QUOTE";
				case TokenType::ESCAPE: return "ESCAPE";
				case TokenType::SUBSHELL_START: return "SUBSHELL_START";
				case TokenType::SUBSHELL_END: return "SUBSHELL_END";
				case TokenType::BRACE_START: return "BRACE_START";
				case TokenType::BRACE_END: return "BRACE_END";
				case TokenType::ASSIGNMENT: return "ASSIGNMENT";
				case TokenType::WHITESPACE: return "WHITESPACE";
				case TokenType::EOL: return "EOL";
			}
		}
};

class ZshLexer {
private:
    std::string& input;
    size_t position;
    size_t line;
    size_t column;
		const alias_container &aliases;
		bool at_command_start;
    std::unordered_set<std::string, transparent_string_hash, std::equal_to<>> expanding_aliases;

    char peek(int offset = 0) const {
        if (position + offset < input.length()) {
            return input[position + offset];
        }
        return '\0';
    }

    void advance(size_t steps = 1) {
        for (size_t i = 0; i < steps; ++i) {
            if (position < input.length()) {
                if (input[position] == '\n') {
                    line++;
                    column = 1;
                } else {
                    column++;
                }
                position++;
            }
        }
    }

    bool isSpecialCharacter(char c) const {
        return strchr("|&;()<> \t\n", c) != nullptr;
    }

    bool isValidWordChar(char c) const {
        return !isSpecialCharacter(c) && 
               c != '\'' && c != '"' && 
               std::isgraph(c);
    }

    Token lexWord() {
			std::string_view word;
			size_t start_line = line;
			size_t start_column = column;
			size_t word_start = position;
			size_t word_length;

			while(true) {
				word_length = 0;
				
        // Capture the word
				// TODO: Optimize peek() calls
        while (position < input.length() && 
               (isValidWordChar(peek()) || peek() == '-' || 
                peek() == '.' || peek() == '/')) {
						word_length++;
            advance();
        }
				word = std::string_view(input).substr(word_start, word_length);

			 // Check for alias expansion only at command start
				std::string expansion;
				if(!at_command_start
						|| expanding_aliases.find(word) != expanding_aliases.end()
						|| !aliases.try_get_expansion(word, expansion))
					break;

				expanding_aliases.emplace(word); 				

				//  TODO: Work with std::string_view
				input.replace(word_start, word_length, expansion);

				// Reset position to start of expansion
				position = word_start;
      }
			expanding_aliases.clear();
			at_command_start = false;
			return {TokenType::WORD,  input.substr(word_start, word_length), start_line, start_column};
		}

    Token lexSpecialCharacter() {
        size_t startLine = line;
        size_t startColumn = column;
        TokenType type;
        std::string value(1, peek());

        switch (peek()) {
            case '|': 
                type = TokenType::PIPE;
                at_command_start = true;
                break;
            case '&': 
                type = TokenType::BACKGROUND;
                at_command_start = true;
                break;
            case ';': 
                type = TokenType::SEMICOLON;
                at_command_start = true;
                break;
            case '(': 
                type = TokenType::SUBSHELL_START;
                at_command_start = true;
                break;
            case ')': 
                type = TokenType::SUBSHELL_END;
                at_command_start = false;
                break;
            case '{': 
                type = TokenType::BRACE_START;
                break;
            case '}': 
                type = TokenType::BRACE_END;
                break;
            case '>':
            case '<':
                type = TokenType::REDIRECTION;
                if (peek(1) == peek()) {
                    value += peek();
                    advance();
                }
                break;
            case ' ':
            case '\t':
                type = TokenType::WHITESPACE;
                while (position < input.length() && 
                      (peek() == ' ' || peek() == '\t')) {
                    advance();
                }
                break;
            case '\n':
                type = TokenType::EOL;
                at_command_start = true;
                break;
            default:
                throw std::runtime_error("Unexpected special character");
        }

        advance();
        return {type, value, startLine, startColumn};
    }

public:
    ZshLexer(std::string& input, const alias_container& aliases) 
        : input(input), position(0), line(1), column(1), aliases(aliases), at_command_start(true) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        at_command_start = true;
        expanding_aliases.clear();

        while (position < input.length()) {
            // Skip whitespace but preserve at_command_start
            while (position < input.length() && 
                   (peek() == ' ' || peek() == '\t')) {
                advance();
            }

            if (position >= input.length()) break;

            if (isSpecialCharacter(peek())) {
                tokens.push_back(lexSpecialCharacter());
            } else {
                tokens.push_back(lexWord());
            }
        }

        tokens.push_back({TokenType::EOL, "", line, column});
        return tokens;
    }
};

/*
	class CommandPool {
	public:
		// Pool sizes tuned for common shell command patterns
		static constexpr size_t SMALL_CMD_SIZE = 128; // e.g., "ls -la"
		static constexpr size_t MEDIUM_CMD_SIZE = 512; // e.g., "find . -name "*.txt""
		static constexpr size_t LARGE_CMD_SIZE = 2048; // e.g., long pipelines
		static constexpr size_t HUGE_CMD_SIZE = 8192; // e.g., very long commands

		struct BufferView {
			char *data;
			size_t capacity;
			size_t size;
			void *pool_ref;

			inline operator std::string_view() const {
				return std::string_view(data, size);
			}
		};

	private:
		struct Pool {
			char *buffer;
			size_t size;
			bool in_use;
		};

		struct PoolBlock {
			static constexpr size_t NUM_SMALL = 32;
			static constexpr size_t NUM_MEDIUM = 16;
			static constexpr size_t NUM_LARGE = 8;
			static constexpr size_t NUM_HUGE = 4;

			std::array<Pool, NUM_SMALL> small;
			std::array<Pool, NUM_MEDIUM> medium;
			std::array<Pool, NUM_LARGE> large;
			std::array<Pool, NUM_HUGE> huge;

			inline PoolBlock() {
				for (auto &p: small) p = {new char[SMALL_CMD_SIZE], SMALL_CMD_SIZE, false};
				for (auto &p: medium) p = {new char[MEDIUM_CMD_SIZE], MEDIUM_CMD_SIZE, false};
				for (auto &p: large) p = {new char[LARGE_CMD_SIZE], LARGE_CMD_SIZE, false};
				for (auto &p: huge) p = {new char[HUGE_CMD_SIZE], HUGE_CMD_SIZE, false};
			}

			inline ~PoolBlock() {
				for (auto &p: small) delete[] p.buffer;
				for (auto &p: medium) delete[] p.buffer;
				for (auto &p: large) delete[] p.buffer;
				for (auto &p: huge) delete[] p.buffer;
			}
		};

		std::unique_ptr<PoolBlock> pools_;

	public:
		inline CommandPool() : pools_(std::make_unique<PoolBlock>()) {
		}

		inline BufferView acquire(size_t needed_size) {
			auto try_acquire = [](auto &pool_array, size_t buf_size) -> BufferView {
				for (auto &pool: pool_array) {
					if (!pool.in_use) {
						pool.in_use = true;
						return {pool.buffer, buf_size, 0, &pool};
					}
				}
				return {nullptr, 0, 0, nullptr};
			};

			if (needed_size <= SMALL_CMD_SIZE) return try_acquire(pools_->small, SMALL_CMD_SIZE);
			else if (needed_size <= MEDIUM_CMD_SIZE) return try_acquire(pools_->medium, MEDIUM_CMD_SIZE);
			else if (needed_size <= LARGE_CMD_SIZE) return try_acquire(pools_->large, LARGE_CMD_SIZE);
			else if (needed_size <= HUGE_CMD_SIZE) return try_acquire(pools_->huge, HUGE_CMD_SIZE);
			return {nullptr, 0, 0, nullptr};
		}

		inline void release(BufferView &view) {
			if (view.pool_ref) {
				static_cast<Pool *>(view.pool_ref)->in_use = false;
				view = {nullptr, 0, 0, nullptr};
			}
		}

		// Non-copyable
		CommandPool(const CommandPool &) = delete;

		CommandPool &operator=(const CommandPool &) = delete;

		// Movable
		CommandPool(CommandPool &&) = default;

		CommandPool &operator=(CommandPool &&) = default;
	};
*/

#endif // ZSH_LEXER_H
