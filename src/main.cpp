/*
 *

      ..               .x+=:.
x .d88"               z`    ^%    .uef^"
 5888R                   .   <k :d88E
 '888R        .u       .@8Ned8" `888E
  888R     ud8888.   .@^%8888"   888E .z8k
  888R   :888'8888. x88:  `)8b.  888E~?888L
  888R   d888 '88%" 8888N=*8888  888E  888E
  888R   8888.+"     %8"    R88  888E  888E
  888R   8888L        @8Wou 9%   888E  888E
 .888B . '8888c. .+ .888888P`    888E  888E
 ^*888%   "88888%   `   ^"F     m888N= 888>
   "%       "YP'                 `Y"   888
                                      J88"
                                      @%
                                    :"
*/
	/*
┓   ┓ 
┃┏┓┏┣┓
┗┗ ┛┛┗
	*/


#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <sys/unistd.h>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "zsh_executor.h"

#include <linenoise.hpp>
#include <replxx.h>

#include "utils.h"
#include "zsh_parser_plus.h"

#include "replxx.hxx"
// #include <>

// #include <absl/container/flat_hash_map.h>


using alias_map = std::unordered_map<std::string, std::string, transparent_string_hash, std::equal_to<>>;
// end alias shit

// begin - tools
int to_int(char const *s, int &r) {
	 if (!s || *s == '\0')
				return -1;

		r = 0;
		int sign = 1;
		if (*s == '-') {
				sign = -1;
				++s;
		} else if (*s == '+') {
				++s;
		}

		while (*s) {
				if (*s < '0' || *s > '9')
						return -1;
				r = r * 10 + (*s - '0');
				++s;
		}

		r *= sign;
		return 0;
}

const std::optional<std::string> file_found(const std::vector<std::filesystem::path> &paths, const std::string_view &file) noexcept{
	for (auto p:paths) {
		auto ap = p / file;
		if (std::filesystem::exists(ap))
			return std::string(ap);
	}
	return std::nullopt;
}

// end tools



class built_in_command {
		public:
			virtual int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				return -1;
			}
};

class pwd_built_in_command : public built_in_command {
	private:
		const lesh_state &_state;
	public:
			pwd_built_in_command(const lesh_state &state): _state(state) {}

			int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				if (!params.empty()) {
					std::cout << "pwd: too many arguments" << std::endl;
					return -1;
				}
				std::cout << _state.pwd().c_str() << std::endl;
				return -1;
			}
};

class cd_built_in_command : public built_in_command {
	private:
		lesh_state &_state;
	public:
		cd_built_in_command(lesh_state &state): _state(state) {}

		int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
			if (params.empty()) {
				std::cout << "cd: too little arguments" << std::endl;
				return -1;
			}
			if (params.find(' ') != std::string::npos) {
				std::cout << "cd: too many arguments" << std::endl;
				return -1;
			}


			// needs to be outside of the conditon in oder to satisfy view
			auto t = std::string();
			if (params.contains('~')) {
				t = std::string(params);
				std::string::size_type n = 0;
				while((n = t.find("~", n)) != std::string::npos) {
						t.replace(n, 1, _state.home());
				    n+=1;
			 	}
				params = t;
			}


			std::error_code e;
			std::filesystem::current_path(_state.pwd() / params, e);
			if (e) {
				std::cout << "cd: " << params << ": " << e.message() << std::endl;
				return -1;
			}
			_state.set_path(std::filesystem::current_path());
			return -1;
		}
};

class c_echo_built_in_command : public built_in_command {
	public:
			constexpr c_echo_built_in_command() {}
			virtual int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				std::cout << params << std::endl;
				return -1;
			}
};

class echo_built_in_command : public built_in_command {
	public:
			virtual int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				std::cout << params << std::endl;
				return -1;
			}
};

class exit_built_in_command : public built_in_command {
	public:
			virtual int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				int code = 0;
				if (params.empty())
					return 0;
				if (params.find(' ') != std::string::npos) {
					std::cout << "exit: too many arguments" << std::endl;
					return -1;
				}
				if (to_int(params.data(), code) != 0) {
					std::cout << "exit: argument: " << params << " is not a valid exit code" << std::endl;
					return -1;
				}
				// std::cout << "code: " << code << std::endl;
				return code;
			}
};
class type_built_in_command : public built_in_command {
	private:
		const std::unordered_map<std::string_view, std::unique_ptr<built_in_command>> &_commands;
		const lesh_state &_state;
	public:
			type_built_in_command(const lesh_state &state, const std::unordered_map<std::string_view, std::unique_ptr<built_in_command>> &commands): _commands(commands), _state(state){}

			virtual int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				// is it a builtin command?
				if (_commands.contains(params)) {
					std::cout << params << " is a shell builtin" << std::endl;
					return -1;
				}


				// somewhere in path?
				if (auto f_o = file_found(_state.path_env(), params)) {
					std::cout << params << " is " << f_o.value() << std::endl;
					return  -1;
				}

				// idk
				std::cout << params << ": not found" << std::endl;
				return -1;
			}
};

class alias_build_in_command : public built_in_command {
	private:
		alias_map &_aliases;
		const lesh_state &_state;
	public:
			alias_build_in_command(const lesh_state &state, alias_map &aliases): _aliases(aliases), _state(state){}

			virtual int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
			    if (params.empty()) {
					for(auto &v : _aliases)
                  		std::cout << v.first << "=" << v.second << std::endl;
                    return -1;
				}

				// is it a builtin command?
				auto split_idx = params.find('=');
				if (split_idx == std::string_view::npos || split_idx + 1 == params.length()) {
    				std::cout << "invalid arguments" << std::endl;
    				return -1;
				}

				auto key_v = params.substr(0, split_idx);
				auto val_v = params.substr(split_idx + 1);
				std::string key = std::string(key_v);
				auto value = std::string(val_v);

				_aliases.insert_or_assign(key, value);
				// _aliases[std::move(std::string(key_v))] = std::string(val_v);

				std::cout << key_v << " is " << val_v << " (" << _aliases.size() << ")" <<  std::endl;
				return -1;
			}
};

#include <unistd.h>

/*
void split_string(char* actual_command, char* input, std::vector<char*>& target) {
	target.clear();

	if (!input || !*input) return;

	char* p = input;
	bool in_space = true;
	char wait_closing = '\0';

	for (char* mp = input; *mp; mp++) {
			if (*mp == '"') {
				if (wait_closing==*mp) {
					*mp='\0';
					wait_closing='\0';
				} else {
					wait_closing=*mp;
				}
			} else if (*mp == ' ') {
					if (in_space)
						continue;
					*mp = '\0';
					in_space = true;
					target.push_back(p);
			} else if (in_space) {
					p = mp;
					in_space = false;
			}
	}

	if (!in_space)
			target.push_back(p);
	target[0] = actual_command;
	target.push_back(nullptr);
}

int gogo2(std::vector<std::vector<char*>>& commands, char** envp) {
    // Total number of commands
    int num_commands = commands.size();

    // Array to store pipe file descriptors
    std::vector<int> pipefd(2 * (num_commands - 1));

    // Create pipes between commands
    for (int i = 0; i < num_commands - 1; i++) {
        if (pipe(pipefd.data() + i * 2) == -1) {
            throw std::runtime_error("Pipe creation failed: " + std::string(std::strerror(errno)));
        }
    }

    // Vector to store child process IDs
    std::vector<pid_t> pids(num_commands);

    for (int i = 0; i < num_commands; i++) {
        pids[i] = fork();

        if (pids[i] == -1) {
            throw std::runtime_error("Fork failed: " + std::string(std::strerror(errno)));
        }

        if (pids[i] == 0) {  // Child process
            // Redirect input if not the first command
            if (i > 0) {
                if (dup2(pipefd[(i-1) * 2], STDIN_FILENO) == -1) {
                    std::cerr << "Input redirection failed: " << std::strerror(errno) << std::endl;
                    exit(EXIT_FAILURE);
                }
            }

            // Redirect output if not the last command
            if (i < num_commands - 1) {
                if (dup2(pipefd[i * 2 + 1], STDOUT_FILENO) == -1) {
                    std::cerr << "Output redirection failed: " << std::strerror(errno) << std::endl;
                    exit(EXIT_FAILURE);
                }
            }

            // Close all pipe file descriptors in child
            for (int j = 0; j < pipefd.size(); j++) {
                close(pipefd[j]);
            }

            // Execute the command
            execve(commands[i][0], commands[i].data(), envp);

            // If execve fails
            std::cerr << "Error executing command: " << commands[i][0]
                      << " " << std::strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    // Close all pipe file descriptors in parent
    for (int j = 0; j < pipefd.size(); j++) {
        close(pipefd[j]);
    }

    // Wait for last command and return its exit status
    int status;
    waitpid(pids[num_commands - 1], &status, 0);

    // Optionally wait for other processes to avoid zombies
    for (int i = 0; i < num_commands - 1; i++) {
        waitpid(pids[i], nullptr, 0);
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return -1;
}

int gogo(std::vector<char*> &a, char** envp) {
	std::cerr << "executing: " << a[0] << std::endl;
	id_t pid = fork();

	if (pid == -1) {
			//TODO : NO EXPECTIOMS
			throw std::runtime_error("Fork failed: " + std::string(std::strerror(errno)));
	}
	if (pid != 0) {
			int status;
			waitpid(pid, &status, 0);

			if (WIFEXITED(status))
					return WEXITSTATUS(status);

			return -1;
	}

	execve(a[0], a.data(), envp);
	std::cerr << "Error executing command: " << a[0] << " " << std::strerror(errno) << std::endl;
	exit(EXIT_FAILURE);
	return 0;
}


/* class aliases {
	private:
		class alias_record {
			const size_t _id;
			std::string _value;
			public:
				alias_record(size_t id, const std::string value) :  _id(id), _value(value) {}
				void update_value(const std::string value) { _value = value; }
				size_t id() const { return  _id; }
				const std::string value() const { return  _value; }
		};
private:
		std::unordered_map<std::string_view, alias_record> _aliases;

	public:
		aliases() {
			_aliases.reserve(100);
		}

		void add(std::string_view key, std::string value) {
			if (auto it = _aliases.find(key); it != _aliases.end())
				it->second.update_value(value);
			else
				_aliases.emplace(key, _aliases.size(), value);
		}

		size_t count()  {
			return  _aliases.size();
		}

		const std::string& find_alias(std::string_view key) {
		std::bitset<sizeof(size_t)> a;
			auto m = a[0];
			if (auto s = _aliases.find(key); s != _aliases.end()) {
				s->second;
			}
			return std::nullopt;
		}
};*/


#include "lolcat.h"

void print_lesh(double gradient = 0.6) {
	std::stringstream ss;
	ss << "      ..               .x+=:." << std::endl;
	ss << "x .d88\"               z`    ^%    .uef^\"" << std::endl;
	ss << " 5888R                   .   <k :d88E" << std::endl;
	ss << " '888R        .u       .@8Ned8\" `888E" << std::endl;
	ss << "  888R     ud8888.   .@^%8888\"   888E .z8k" << std::endl;
	ss << "  888R   :888'8888. x88:  `)8b.  888E~?888L" << std::endl;
	ss << "  888R   d888 '88%\" 8888N=*8888  888E  888E" << std::endl;
	ss << "  888R   8888.+\"     %8\"    R88  888E  888E" << std::endl;
	ss << "  888R   8888L        @8Wou 9%   888E  888E" << std::endl;
	ss << " .888B . '8888c. .+ .888888P`    888E  888E" << std::endl;
	ss << " ^*888%   \"88888%   `   ^\"F     m888N= 888>" << std::endl;
	ss << "   \"%       \"YP'                 `Y\"   888" << std::endl;
	ss << "                                      J88\"" << std::endl;
	ss << "  v0.01α                              @%" << std::endl;
	ss << "  <nanov/>                          :\"" << std::endl;
	lolfilter(ss, gradient);
}

int main(int argc, char **argv, char **envp) {
	setenv("TERM", "xterm-256color", 1);


	int exit_code = -1;
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;


  std::string input = "";
	lesh_state state {envp};

	alias_container aliases {
		{ ":q", "exit" },
		{ "ls", "ls -G" },
		{ "l", "ls -lah" },
		{ "ll", "ls -lh" },
		{ "grep", "grep --color=auto" }, // --exclude-dir={.bzr,CVS,.git,.hg,.svn,.idea,.tox,.venv,venv}" },
		{ "lvim", "~/.local/bin/lvim"}
	};

	std::string in = ("cat test");
	// std::string in = ("cat test");
	// std::string in = ("cat $(cat test) | grep shell"); // ("ls -l | time | ago | i | can | still | remmber");
	// std::string in = ("cat CMakeCache.txt | grep shell"); // ("ls -l | time | ago | i | can | still | remmber");
	// std::string in = ("ls -l | time | ago | i | can | still | remmber");
	// std::string in = ("ls -l");


	std::unordered_map<std::string_view, std::unique_ptr<built_in_command>> built_in_commands_executors;
	built_in_commands_executors.insert({"cd", std::make_unique<cd_built_in_command>(state)});
	built_in_commands_executors.insert({"pwd", std::make_unique<pwd_built_in_command>(state)});
	built_in_commands_executors.insert({"echo", std::make_unique<echo_built_in_command>()});
	built_in_commands_executors.insert({"exit", std::make_unique<exit_built_in_command>()});
	built_in_commands_executors.insert({"type", std::make_unique<type_built_in_command>(state, built_in_commands_executors)});
	// built_in_commands_executors.insert({"alias", std::make_unique<alias_build_in_command>(state, aliases)});

	std::vector<char*> repl_args = {};
	std::vector<std::vector<char*>> repl_chain = {};
	repl_args.reserve(5);


	linenoise::SetHistoryMaxLen(12);
	linenoise::LoadHistory(".lesh_history");
	print_lesh();


	auto zsh_parser = ZshParserPlus::Parser(state);

	replxx::Replxx rx;
	rx.bind_key_internal(replxx::Replxx::KEY::UP,  "history_previous");
	rx.bind_key_internal(replxx::Replxx::KEY::DOWN,  "history_next");
	std::string history_file = ".lesh_history_new";
	rx.history_load(history_file);

	zsh_parser.init_aliases();
	while(true) {
		input.clear();
		state.tick();
		// std::string dbg = "$(echo -n ls)";
		// zsh_parser.parse_and_execute(dbg);
		// display the prompt and retrieve input from the user
		char const* cinput{ nullptr };

		do {
			cinput = rx.input(state.pmt());
		} while ( ( cinput == nullptr ) && ( errno == EAGAIN ) );

		if (cinput == nullptr) {
			break;
		}

		input.append(cinput);

		if (input.empty())
			continue;
		rx.history_add(cinput);

		std::string input = {cinput};


		if (input == "exit") {
			exit_code = 0;
			break;
		}

		zsh_parser.parse_and_execute(input);

		// executeZshCommand(input, aliases);


		// auto number_of_commands = prepare_commands(input, aliases, state, repl_chain);
		// gogo2(repl_chain, envp);

		//

		// break;


	/* parse_command:
	  auto dl = input.find(' ');
		auto c_v = std::string_view(input);
		auto command = c_v.substr(0, dl);


		if (!executed_aliases.contains(command))
			if(auto alias = aliases.find(command); alias != aliases.end()) {
				std::cout << command << "=" << alias->second << std::endl;
				executed_aliases.insert(alias->first);
				input.replace(0, command.size(), alias->second);
				goto parse_command;
			}

		executed_aliases.clear();

		state.adjust_home(input); */

		/*
		dl = input.find(' ');
		auto params = dl == std::string::npos ? std::string_view() : c_v.substr(dl+1);

		if (auto ex_search = built_in_commands_executors.find(command); ex_search != built_in_commands_executors.end()) {
			auto e_c = ex_search->second->execute(input, command, params);
			if (e_c >=0)
				return e_c;
			continue;
		}
		*/

		// if (auto ff = file_found(state.path_env(), command)) {
		// 	auto e_p = ff->c_str();
		// 	if (!access(e_p, X_OK)) {
		// 		std::cerr << "command ok: " << e_p << std::endl;
		// 		split_string((char*)e_p, (char*)input.c_str(), repl_args);
		// 		// TODO: handle here exceptions
		// 		auto res = gogo(repl_args, envp);
		// 		continue;
		// 	}
		// }

		// std::cout << command << ": command not found" << std::endl;
	}

	// linenoise::SaveHistory(".lesh_history");

	rx.history_sync(history_file);
	return exit_code;


}
