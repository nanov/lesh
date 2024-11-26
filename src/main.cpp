#include <iostream>
#include <ostream>
#include <string_view>

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
		if (input.rfind("echo", 0) == 0) {
			std::cout << std::string_view(input).substr(5) << std::endl;
			continue;
		} 

	
		std::cout << input << ": command not found" << std::endl;
	}



	return exit_code; 
	
}
