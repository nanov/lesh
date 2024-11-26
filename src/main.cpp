#include <iostream>
#include <ostream>

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
	
		std::cout << input << ": command not found" << std::endl;

	}

	return exit_code; 
	
}
