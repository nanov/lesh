#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// begin - tools
int to_int(char const *s, int &r) {
	if (s == NULL || *s == '\0')
		return -1;

	bool negate = (s[0] == '-');
	if ( *s == '+' || *s == '-' ) 
		++s;

	if ( *s == '\0')
		return -1;

	while(*s) {
    if ( *s < '0' || *s > '9' )
			return -1;
		r = r * 10  - (*s - '0');  //assume negative number
		++s;
 	}
	if (!negate)
		r *= -1;

	return 0;
}

const std::optional<std::string> file_found(const std::vector<std::string> &paths, const std::string_view &file) noexcept{
	for (auto p:paths) {
		std::filesystem::path ap = p;
		ap.append(file);
		if (std::filesystem::exists(ap))
			return std::string(ap);
	}
	return std::nullopt;
}

// end tools


struct shell_state {
	std::filesystem::path pwd;
};


class built_in_command {
		public:
			virtual int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				return -1;
			}
};

class pwd_built_in_command : public built_in_command {
	private:
		const shell_state &_state;
	public:
			pwd_built_in_command(const shell_state &state): _state(state) {}

			int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				if (!params.empty()) {
					std::cout << "pwd: too many arguments" << std::endl;
					return -1;
				}
				std::cout << _state.pwd.c_str() << std::endl;
				return -1;
			}
};

class cd_built_in_command : public built_in_command {
	private:
		shell_state &_state;
	public:
		cd_built_in_command(shell_state &state): _state(state) {}

		int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
			if (params.empty()) {
				std::cout << "cd: too little arguments" << std::endl;
				return -1;
			}
			if (params.find(' ') != std::string::npos) {
				std::cout << "cd: too many arguments" << std::endl;
				return -1;
			}

		
			auto t = std::string();
			if (params.contains('~')) {
				t = std::string(params);
				std::string::size_type n = 0;
				while((n = t.find("~", n)) != std::string::npos) {
						t.replace(n, 1, getenv("HOME"));	
				    n+=1;
			 	}
				params = t;
			}
		

			std::error_code e;
			std::filesystem::current_path(_state.pwd / params, e);	
			if (e) {
				std::cout << "cd: " << params << ": " << e.message() << std::endl;
				return -1;
			}
			_state.pwd = std::filesystem::current_path();
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
		const std::unordered_map<std::string_view, built_in_command*> &_commands;
		const std::vector<std::string> &_paths;
	public:
			type_built_in_command(const std::unordered_map<std::string_view, built_in_command*> &commands, const std::vector<std::string> &paths): _commands(commands), _paths(paths){}

			virtual int execute(const std::string &raw_input, std::string_view &command, std::string_view &params) {
				// is it a builtin command?
				if (_commands.contains(params)) {
					std::cout << params << " is a shell builtin" << std::endl;
					return -1;
				}
				

				// somewhere in path?
				if (auto f_o = file_found(_paths, params)) {
					std::cout << params << " is " << f_o.value() << std::endl;
					return  -1;
				}
				
				// idk
				std::cout << params << ": not found" << std::endl;
				return -1;
			}
};

const std::vector<std::string>& get_env_path() {
	static std::vector<std::string> res;
	if (!res.empty())
		return res;
	const auto p = std::getenv("PATH");
	if (!p)
		return res;

	const auto pa = std::string(p);
	size_t pr = 0;
	size_t in = pa.find(':');
	while(in != std::string::npos) {
		auto pd = pa.substr(pr, in - pr);
		res.push_back(pd);
		pr = in+1;
		in = pa.find(':', pr);
	}
	res.push_back(pa.substr(pr));
	return  res;
}

static const auto env_p = get_env_path();


#include <unistd.h>
int main() {
	int exit_code = -1;
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // Uncomment this block to pass the first stage

  std::string input = "";
	auto paths = get_env_path(); 
	shell_state state = { 
		.pwd = std::filesystem::current_path()
	};

	static const std::unordered_map<std::string_view, built_in_command*> built_in_commands_executors = {
		{"pwd", new pwd_built_in_command(state)},
		{"cd", new cd_built_in_command(state)},
		{"echo", new echo_built_in_command},
		{"exit", new exit_built_in_command},
		{"type", new type_built_in_command(built_in_commands_executors, env_p)}
	};


	while(true) {
		std::cout << "$ ";
		std::getline(std::cin, input);
	  auto dl = input.find(' ');
		auto c_v = std::string_view(input);

		auto command = c_v.substr(0, dl);
		auto params = dl == std::string::npos ? std::string_view() : c_v.substr(dl+1);

		if (auto ex_search = built_in_commands_executors.find(command); ex_search != built_in_commands_executors.end()) {
			auto e_c = ex_search->second->execute(input, command, params);
			if (e_c >=0)
				return e_c;
			continue;
		}
	
		if (auto ff = file_found(env_p, command)) {
			auto e_p = ff->c_str();
			if (!access(e_p, X_OK)) {
				system(input.c_str());
				continue;
			}
		}


		std::cout << command << ": command not found" << std::endl;
	}

	return exit_code; 
	
}
