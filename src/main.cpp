#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>


const auto built_in_commands = std::unordered_set<std::string_view>{"echo", "exit", "type"};

int main() {
	int exit_code = -1;
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // Uncomment this block to pass the first stage

  std::string input = "";


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
			} else {
				std::cout << l << ": not found" << std::endl;
			}
			continue;
		}
	
		std::cout << input << ": command not found" << std::endl;
	}



	return exit_code; 
	
}
