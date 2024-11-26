#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>


const auto built_in_commands = std::unordered_set<std::string_view>{"echo", "exit", "type"};

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
		res.push_back(pa.substr(pr, in));
		pr = in+1;
		in = pa.find(':', pr);
	}
	res.push_back(pa.substr(pr));
	return  res;
}

static auto env_p = get_env_path();

const std::optional<std::string> file_found(std::vector<std::string> &paths, std::string_view &file) {
	for (auto p:paths) {
		std::filesystem::path ap = p;
		ap.append(file);
		if (std::filesystem::exists(ap))
			return std::string(ap);
	}
	return std::nullopt;
}

int main() {
	int exit_code = -1;
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // Uncomment this block to pass the first stage

  std::string input = "";
	auto paths = get_env_path(); 


	while(true) {
		std::cout << "$ ";
		std::getline(std::cin, input);
		if (input == "exit 0")
			return 0;
		if (input.rfind("echo ", 0) == 0) {
			std::cout << std::string_view(input).substr(5) << std::endl;
			continue;
		} 
		if (input.rfind("type ") == 0) {
			auto l = std::string_view(input).substr(5);
			if (built_in_commands.contains(l)) {
				std::cout << l << " is a shell builtin" << std::endl;
			} else if (auto f_o = file_found(env_p, l)) {
				std::cout << l << " is " << f_o.value() << std::endl;

			}else {
				std::cout << l << ": not found" << std::endl;
			}
			continue;
		}
	
		std::cout << input << ": command not found" << std::endl;
	}



	return exit_code; 
	
}
