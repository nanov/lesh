#pragma once

// The parse tree the single-pass parser produces, plus the alias table.
// Replaced wholesale by ADR-0002; see issues #9 and #10.
#include "legacy/substrate_bridge.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define SUBSHELL_BUFFER_INITIAL_SIZE 1024

struct ASTWord {
    // null-terminated, not-owned
    const char *value;
		explicit ASTWord(const char* inval): value(inval) {
		}
		ASTWord(const ASTWord& other): value(other.value) {}
		// DEFECT, frozen deliberately (issue #13): `const T&&` is not a move
		// constructor. You cannot move out of a const object, so this performs a
		// copy - and every other "move" below does the same. Correcting the
		// signature would make hybrid_vector genuinely move, which invokes a copy
		// path that is itself broken, so it would change behaviour in code ADR-0002
		// deletes, for no gain. The replacement AST (#10) gets this right by
		// construction.
		ASTWord(const ASTWord&& other) noexcept : value(other.value) {}
};
struct ASTPipe;
//
struct ASTCommandSubstitution {
	char *before;
	char *after;
	char *command;
	const ASTPipe* pipe;
};
struct ASTAssignment {
	const char *key;
	const char *value;
};

// command is a part exectued by itself, it conatins a colletions of wrods whics are it's arhuments ( ls -l -gAH )
struct ASTCommand {
	static constexpr size_t MAX_CHILDREN = 32;
	static constexpr size_t INITIAL_ASSIGNMENTS = 0;

	// those are essentially parameters
	hybrid_continuous_simple_vector<ASTWord, MAX_CHILDREN> children;
	hybrid_vector<ASTAssignment, INITIAL_ASSIGNMENTS> assignments;

	bool is_sealed = false;

	ASTCommand() = default;

	// copy
	ASTCommand(const ASTCommand& other):
	children(other.children),
	assignments(other.assignments) {
	}
	// move
	// See the note on ASTWord's move constructor: `const T&&` cannot move.
	ASTCommand(const ASTCommand&& other) noexcept : children(other.children), assignments(other.assignments) {
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

    void enrich_with(const ASTCommand & other) {
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
        last_command.enrich_with(last_command);
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
		std::unordered_set<std::string> expanded_aliases;
		std::unordered_map<std::string, alias>::iterator expanded;

		for (auto defined_alias = _aliases.begin(); defined_alias != _aliases.end(); ++defined_alias) {
			defined_alias->second.expanded = defined_alias->second.original;

			size_t cmd_idx = 0;
			do {
				expanded_aliases.clear();
				auto command = defined_alias->second.expanded.commands[cmd_idx];
				auto command_str = command->children[0]->value;
				expanded_aliases.emplace(defined_alias->first);
				while (
					!expanded_aliases.contains(command_str)
					&& ((expanded = _aliases.find(command_str)) != _aliases.end())
					) {
					if (expanded->second.original.size() == 1) {
						command->expand_with(*expanded->second.original.commands[0]);
					} else {
						command = defined_alias->second.expanded.expand_with_at(expanded->second.original, cmd_idx);
						// cmd_idx += expanded->second.original.size();
						// todo
					}
					expanded_aliases.emplace(expanded->first);
					command_str = command->children[0]->value;
					}
				cmd_idx++;
			} while (cmd_idx < defined_alias->second.expanded.size());
		}

	}
};
