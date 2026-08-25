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


#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "runtime/executor.h"
#include "runtime/invocation.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

namespace {

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

// Runs input through the shell: read, parse, execute, one complete command at a
// time.
//
// The read loop lives in the executor, not here. The TREES it parses do not: a
// function defined by one command is a node in the tree that command was parsed
// from, and shell state owns the function, so shell state holds the tree (#106).
// See tree_walking_executor::run_input and shell_state::retain_tree.
//
// THE POOL IS THE CALLER'S, and that is the whole reason it is a parameter. Those
// trees' nodes live in it, so it has to outlive the state that holds them - which
// it cannot do when it is a local here and the state is main's.
//
// `echo_when_verbose` is false for `-c`: POSIX makes `set -v` a property of
// reading INPUT, and dash prints nothing for `dash -v -c 'echo hi'`.
int run_shell(std::string_view source, lesh::buffer_pool& pool,
              lesh::runtime::shell_state& state,
              bool echo_when_verbose = true,
              lesh::runtime::script_input* input = nullptr) {
	lesh::runtime::tree_walking_executor executor{pool, state};
	return executor.run_input(source, echo_when_verbose, input);
}

// Reads a whole stream. The bytes are still slurped up front - stdin is drained
// so `read` can share the descriptor with the script (#31) - but they are PARSED
// one command at a time, which is what makes an alias defined on one line take
// effect on the next.
//
// Slurping is no longer visible to anything else on a SEEKABLE input: the read
// loop hands the descriptor back at each command boundary, so what the shell
// holds in memory and what the file offset says are two different questions
// (#67, and see runtime::script_input).
std::string read_all(std::istream& in) {
	std::string out;
	std::string line;
	while (std::getline(in, line)) {
		out += line;
		out += '\n';
	}
	return out;
}

// There is ONE front end (#28). The strangler seam ADR-0002 opened - two front
// ends compiled in and chosen by LESH_FRONTEND - closed when src/legacy/ was
// deleted, so the variable is no longer read and setting it does nothing.
int main(int argc, char **argv) {
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

	// BEFORE the state, so it outlives it: the state holds the trees a function
	// body is a node in, and those nodes are allocated here (#106).
	lesh::buffer_pool pool{BUFFER_POOL_SIZE};
	lesh::runtime::shell_state state;
	state.set_interactive(interactive);
	state.opts() = inv.options;
	// $0. With -c the operand after the command string is the command name, not
	// the first positional parameter - which is why first_argument is past it.
	// parse_invocation falls back to argv[0] when no operand names $0, so this
	// is set for every real invocation and shell_state's literal default never
	// reaches a running shell (issue #43). The guard is for an empty argv, not
	// for an ordinary one.
	if (inv.command_name != nullptr)
		state.set_script_name(inv.command_name);
	std::vector<std::string> args;
	for (int a = i; a < argc; ++a)
		args.emplace_back(argv[a]);
	state.set_positional(std::move(args));

	if (command_string)
		// `-c` does not echo under `set -v`; see run_shell.
		return run_shell(command_string, pool, state, false);
	if (script_path) {
		std::ifstream script{script_path};
		if (!script) {
			std::fprintf(stderr, "lesh: %s: cannot open\n", script_path);
			return 127;
		}
		const std::string source = read_all(script);
		return run_shell(source, pool, state);
	}
	// `-i` with input that is not a terminal is an ordinary POSIX invocation:
	// interactive is about SEMANTICS - which signals are fatal, whether an error
	// exits - not about line editing. There is nothing to edit when the input is
	// a file, so only a terminal needs the editor lesh does not have yet.
	//
	// Refusing it outright cost 1,800 conformance assertions: ten of the signal
	// files run the shell under test as `sh -i` with the case piped in.
	if (!interactive || !isatty(STDIN_FILENO)) {
		// Bound BEFORE a byte is read: what it records is where the script
		// begins, and read_all drains the descriptor to EOF on the next line.
		// A pipe or a terminal leaves it inert and nothing below changes (#67).
		lesh::runtime::script_input input{STDIN_FILENO};
		const std::string source = read_all(std::cin);
		return run_shell(source, pool, state, true, &input);
	}
	// A terminal, and nothing to drive it with. Deleting legacy took replxx and
	// its prompt with it, and NOTHING replaces them in the interim: leshper is
	// built without a TTY and binds at the end (decision #86). So this stays the
	// answer at a terminal until the line editor lands, deliberately.
	std::fprintf(stderr, "lesh: no interactive mode yet\n");
	return 2;
}
