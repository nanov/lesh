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

#include "utils.h"
#include "zsh_parser_plus.h"

#include "replxx.hxx"

/*
const std::optional<std::string> file_found(const std::vector<std::filesystem::path> &paths, const std::string_view &file) noexcept{
	for (auto p:paths) {
		auto ap = p / file;
		if (std::filesystem::exists(ap))
			return std::string(ap);
	}
	return std::nullopt;
}
*/

// end tools


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


#include <replxx.h>


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

std::vector<std::string> hint_callback(replxx::Replxx& replxx, const std::string &input, int& contextLen, replxx::Replxx::Color &color) {
	std::vector<std::string> result;
	auto hs = replxx.history_scan();
	while (hs.next()) {
		auto e = hs.get().text();
		if (e.starts_with(input))
			result.push_back(e);
	}
	color = replxx::Replxx::Color::GRAY;
	return result;
}

int main(int argc, char **argv, char **envp) {
	setenv("TERM", "xterm-256color", 1);


	int exit_code = -1;
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

	lesh_state state {std::filesystem::current_path(), envp};

	/*
	alias_container aliases {
		{ ":q", "exit" },
		{ "ls", "ls -G" },
		{ "l", "ls -lah" },
		{ "ll", "ls -lh" },
		{ "grep", "grep --color=auto" }, // --exclude-dir={.bzr,CVS,.git,.hg,.svn,.idea,.tox,.venv,venv}" },
		{ "lvim", "~/.local/bin/lvim"}
	};
	*/

	print_lesh();


	auto zsh_parser = ZshParserPlus::Parser(state);

	replxx::Replxx rx;
	rx.bind_key_internal(replxx::Replxx::KEY::UP,  "history_previous");
	rx.bind_key_internal(replxx::Replxx::KEY::DOWN,  "history_next");
	rx.bind_key_internal(replxx::Replxx::KEY::meta(replxx::Replxx::KEY::DOWN),  "hint_previous");
	rx.bind_key_internal(replxx::Replxx::KEY::meta(replxx::Replxx::KEY::UP),  "hint_next");
	rx.set_hint_callback([&](const std::string &input, int& contextLen, replxx::Replxx::Color &color) { return hint_callback(rx, input, contextLen, color); });
	rx.set_max_hint_rows(1);
	std::string history_file = ".lesh_history";
	rx.history_load(history_file);

	zsh_parser.init_aliases();
	while(true) {
		// std::string dbg ="cd ..";
		// zsh_parser.parse_and_execute(dbg);
		// display the prompt and retrieve input from the user
		char const* cinput{ nullptr };

		do {
			cinput = rx.input(state.pmt());
		} while ( ( cinput == nullptr ) && ( errno == EAGAIN ) );

		if (cinput == nullptr) {
			break;
		}

		// TODO: prevent copy
		std::string input = {cinput};

		if (input.empty())
			continue;
		rx.history_add(cinput);

		if (input == "exit") {
			exit_code = 0;
			break;
		}

		zsh_parser.parse_and_execute(input);
	}

	rx.history_sync(history_file);
	return exit_code;
}
