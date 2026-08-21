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
#include <vector>
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

#include "legacy/shell_state.h"
#include "legacy/zsh_parser_plus.h"

#include "runtime/executor.h"
#include "runtime/invocation.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

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


#include "ui/lolcat.h"

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

// The strangler seam (issue #8, ADR-0002).
//
// Both front ends are compiled in and chosen at runtime, so neither rots while
// the other is being worked on - a compile-time switch guarantees the idle one
// breaks silently. Selection is by environment rather than a flag because POSIX
// constrains argv, and because it lets the differential harness compare the two
// by spawning the same binary twice with LESH_FRONTEND set differently.
//
// Parity is measured against the reference shell, not against the legacy parser:
// legacy is wrong in known ways, and bug-compatibility would be the wrong target.
// Legacy's score is the baseline to beat.
enum class front_end { legacy, next };

front_end select_front_end() {
	const char* choice = std::getenv("LESH_FRONTEND");
	if (choice == nullptr || *choice == '\0' || std::string_view{choice} == "legacy")
		return front_end::legacy;
	if (std::string_view{choice} == "next")
		return front_end::next;
	std::fprintf(stderr, "lesh: LESH_FRONTEND must be 'legacy' or 'next', got '%s'\n", choice);
	std::exit(2);
}

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

[[noreturn]] void usage_error(const char* message, const char* operand = nullptr) {
	if (operand != nullptr)
		std::fprintf(stderr, "lesh: %s: %s\n", message, operand);
	else
		std::fprintf(stderr, "lesh: %s\n", message);
	std::fprintf(stderr,
	             "usage: lesh [-abCefhimnsuvx] [-o option]... "
	             "[-c command | script] [args...]\n");
	// POSIX: a shell that cannot parse its own invocation exits >0; 2 is the
	// conventional choice, matching a syntax error.
	std::exit(2);
}

} // namespace

// Runs input through the replacement front end: read, parse, execute, one
// complete command at a time.
//
// The read loop lives in the executor, not here: it owns the trees, and a
// function defined by one command is a node in the tree that command was parsed
// from. See tree_walking_executor::run_input.
//
// `echo_when_verbose` is false for `-c`: POSIX makes `set -v` a property of
// reading INPUT, and dash prints nothing for `dash -v -c 'echo hi'`.
int run_next_front_end(std::string_view source, lesh::runtime::shell_state& state,
                       bool echo_when_verbose = true) {
	lesh::buffer_pool pool{BUFFER_POOL_SIZE};
	lesh::runtime::tree_walking_executor executor{pool, state};
	return executor.run_input(source, echo_when_verbose);
}

// Reads a whole stream. The bytes are still slurped up front - stdin is drained
// so `read` can share the descriptor with the script (#31) - but they are PARSED
// one command at a time, which is what makes an alias defined on one line take
// effect on the next.
std::string read_all(std::istream& in) {
	std::string out;
	std::string line;
	while (std::getline(in, line)) {
		out += line;
		out += '\n';
	}
	return out;
}

int main(int argc, char **argv, char **envp) {
	const bool use_next = select_front_end() == front_end::next;

	// Command-line parsing lives in runtime/invocation.h, not here: main() cannot
	// be unit-tested, and getting this wrong silently gated 3,600 conformance
	// assertions (issue #33).
	const lesh::runtime::invocation inv = lesh::runtime::parse_invocation(argc, argv);
	if (inv.error != nullptr)
		usage_error(inv.error, inv.error_operand);

	const char* const command_string = inv.command_string;
	const char* const script_path = inv.script_path;
	const int i = inv.first_argument;

	// POSIX: interactive means -i, or no operands with both stdin and stderr
	// attached to a terminal. Everything user-facing hangs off this one decision
	// rather than off separate ad-hoc checks. `+i` says NOT interactive explicitly,
	// which is why the flag is a tri-state rather than a bool.
	const bool interactive = inv.interactive.value_or(
		!command_string && !script_path && isatty(STDIN_FILENO) && isatty(STDERR_FILENO));

	setenv("TERM", "xterm-256color", 1);

	// Flush after every std::cout / std::cerr
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;

	// The new front end is chosen BEFORE any legacy object is built. Constructing
	// the legacy parser first ran init_aliases(), whose strdup'd text is
	// deliberately never freed (see SimpleParsingState) - so a `next` run reported
	// 14 bytes leaked from a front end it was not even using. ADR-0007 expects
	// exactly zero.
	if (use_next) {
		// The new front end handles every non-interactive form. Interactive is the
		// line editor's business and stays on legacy until Phase 4.
		lesh::runtime::shell_state next_state;
		next_state.set_interactive(interactive);
		next_state.opts() = inv.options;
		// $0. With -c the operand after the command string is the command name, not
		// the first positional parameter - which is why first_argument is past it.
		if (inv.command_name != nullptr)
			next_state.set_script_name(inv.command_name);
		std::vector<std::string> args;
		for (int a = i; a < argc; ++a)
			args.emplace_back(argv[a]);
		next_state.set_positional(std::move(args));

		if (command_string)
			// `-c` does not echo under `set -v`; see run_next_front_end.
			return run_next_front_end(command_string, next_state, false);
		if (script_path) {
			std::ifstream script{script_path};
			if (!script) {
				std::fprintf(stderr, "lesh: %s: cannot open\n", script_path);
				return 127;
			}
			const std::string source = read_all(script);
			return run_next_front_end(source, next_state);
		}
		// `-i` with input that is not a terminal is an ordinary POSIX invocation:
		// interactive is about SEMANTICS - which signals are fatal, whether an error
		// exits - not about line editing. There is nothing to edit when the input is
		// a file, so only a terminal needs the editor lesh does not have yet.
		//
		// Refusing it outright cost 1,800 conformance assertions: ten of the signal
		// files run the shell under test as `sh -i` with the case piped in.
		if (!interactive || !isatty(STDIN_FILENO)) {
			const std::string source = read_all(std::cin);
			return run_next_front_end(source, next_state);
		}
		std::fprintf(stderr, "lesh: LESH_FRONTEND=next has no interactive mode yet\n");
		return 2;
	}

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
