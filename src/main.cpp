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
#include <fstream>
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

#include "utils.h"
#include "zsh_parser_plus.h"

#include "replxx.hxx"
#include <sol/sol.hpp>

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

// NOTICE : the wgole hint system is flawed in replxx - replace with something non allocating
std::vector<std::string> _hint_callback_results = {};
std::vector<std::string> hint_callback(replxx::Replxx& replxx, const std::string &input, int& contextLen, replxx::Replxx::Color &color) {
	_hint_callback_results.clear();
	auto hs = replxx.history_scan();
	while (hs.next()) {
		if (auto& e = hs.get().text();e != input && e.starts_with(input))
			_hint_callback_results.push_back(e);
	}
	color = replxx::Replxx::Color::GRAY;
	return _hint_callback_results;
}

class lesh_lua_api {
	public:
		lesh_lua_api(lesh_state& lesh_state, ZshParserPlus::Parser& parser): _lesh_state(lesh_state), _parser(parser) {}
		void init(sol::state& lua, sol::environment& env) {
			env.create_named("lesh",
			// lua.create_named_table("lesh",
				"set_alias",  [&](const char* k, const char* v) { _parser.set_alias(k, v); },
				"set_alias_lazy",  [&](const char* k, const char* v) { _parser.set_alias_lazy(k, v); },
				"echo",  [&](const char* k) { printf("%s\n", k ); });
		}
private:
	lesh_state &_lesh_state;
	ZshParserPlus::Parser& _parser;
};

namespace {

// One command line, from whatever source. Returns its exit status.
// `exit` is still matched here as a string rather than being a real built-in;
// that moves when the executor gains proper built-in dispatch.
int run_line(ZshParserPlus::Parser& parser, std::string& line, bool& should_exit, int& last_status) {
	if (line.empty())
		return last_status;
	if (line == "exit") {
		should_exit = true;
		return last_status;
	}
	last_status = parser.parse_and_execute(line);
	return last_status;
}

// Feed a script line by line.
//
// NOTE: this reads through a buffered stream, so a script that itself consumes
// stdin cannot see what the shell has already buffered ahead. POSIX requires a
// script to be consumed incrementally for exactly that reason. Fixing it needs a
// read(2)-based input source, which belongs with the lexer's input model rather
// than here.
int run_stream(ZshParserPlus::Parser& parser, std::istream& in) {
	std::string line;
	bool should_exit = false;
	int last_status = 0;
	while (!should_exit && std::getline(in, line))
		run_line(parser, line, should_exit, last_status);
	return last_status;
}

[[noreturn]] void usage_error(const char* message) {
	std::fprintf(stderr, "lesh: %s\n", message);
	std::fprintf(stderr, "usage: lesh [-i] [-c command | script] [args...]\n");
	// POSIX: a shell that cannot parse its own invocation exits >0; 2 is the
	// conventional choice, matching a syntax error.
	std::exit(2);
}

} // namespace

int main(int argc, char **argv, char **envp) {
	const char* command_string = nullptr;
	const char* script_path = nullptr;
	bool force_interactive = false;

	int i = 1;
	for (; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "--") { ++i; break; }
		if (arg.size() < 2 || arg[0] != '-') break;
		if (arg == "-c") {
			if (++i >= argc) usage_error("-c requires an argument");
			command_string = argv[i];
		} else if (arg == "-i") {
			force_interactive = true;
		} else if (arg == "-s") {
			// Read commands from stdin. This is the default already; accepted so
			// harnesses that pass it explicitly are not rejected.
		} else {
			usage_error("unknown option");
		}
	}
	if (!command_string && i < argc)
		script_path = argv[i++];

	// POSIX: interactive means -i, or no operands with both stdin and stderr
	// attached to a terminal. Everything user-facing hangs off this one decision
	// rather than off separate ad-hoc checks.
	const bool interactive = force_interactive ||
		(!command_string && !script_path && isatty(STDIN_FILENO) && isatty(STDERR_FILENO));

	setenv("TERM", "xterm-256color", 1);

	// Flush after every std::cout / std::cerr
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;

	lesh_state state {std::filesystem::current_path(), envp};
	auto zsh_parser = ZshParserPlus::Parser(state);
	zsh_parser.init_aliases();

	if (command_string) {
		std::string line{command_string};
		bool should_exit = false;
		int last_status = 0;
		return run_line(zsh_parser, line, should_exit, last_status);
	}

	if (script_path) {
		std::ifstream script{script_path};
		if (!script) {
			std::fprintf(stderr, "lesh: %s: cannot open\n", script_path);
			return 127;
		}
		return run_stream(zsh_parser, script);
	}

	if (!interactive)
		return run_stream(zsh_parser, std::cin);

	// Interactive from here down. The banner, the prompt, history and replxx
	// itself all belong to this branch and must never touch a pipe or a script.
	_hint_callback_results.reserve(10);
	print_lesh();

	replxx::Replxx rx;
	rx.bind_key_internal(replxx::Replxx::KEY::UP,  "history_previous");
	rx.bind_key_internal(replxx::Replxx::KEY::DOWN,  "history_next");
	rx.bind_key_internal(replxx::Replxx::KEY::meta(replxx::Replxx::KEY::DOWN),  "hint_previous");
	rx.bind_key_internal(replxx::Replxx::KEY::meta(replxx::Replxx::KEY::UP),  "hint_next");
	rx.set_hint_callback([&](const std::string &input, int& contextLen, replxx::Replxx::Color &color) { return hint_callback(rx, input, contextLen, color); });
	rx.set_max_hint_rows(1);
	std::string history_file = ".lesh_history";
	rx.history_load(history_file);

	int last_status = 0;
	bool should_exit = false;
	while (!should_exit) {
		char const* cinput{ nullptr };
		do {
			cinput = rx.input(state.pmt());
		} while ((cinput == nullptr) && (errno == EAGAIN));

		if (cinput == nullptr)
			break;

		std::string input = {cinput};
		if (input.empty())
			continue;
		rx.history_add(cinput);
		run_line(zsh_parser, input, should_exit, last_status);
	}

	rx.history_sync(history_file);
	return last_status;
}
