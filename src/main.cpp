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
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "leshper/read.h"
#include "runtime/executor.h"
#include "runtime/history_store.h"
#include "runtime/invocation.h"
#include "runtime/shell_state.h"
#include "substrate/log.h"
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

// Everything an INTERACTIVE shell has and a non-interactive one does not.
//
// #101's ordering, and it is the whole shape of this function: the state exists
// (main built it), the rc runs, the first read begins. An rc file may bind keys
// and set modes because the editing context is built before it - which is why
// the session is constructed first and started second.
//
// NON-INTERACTIVE SHELLS REACH NONE OF THIS. Not the rc, not the history file,
// not the editor, not a second thread. That is #101's decision and it is also
// what keeps `-c` fast and the conformance score honest: the two paths share
// `shell_state`, the parser and the executor, and nothing else.
int run_interactive(lesh::runtime::shell_state& state, lesh::buffer_pool& pool,
                    const char* term) {
	// #97 decision 3: below the floor, leshper never starts. One line, exit 2,
	// the shape this file already had. The owner explicitly held open the
	// courtesy of exec'ing /bin/sh instead; that is not taken here, because a
	// shell that silently becomes a different shell is worse to debug than one
	// that says why it stopped.
	if (!lesh::leshper::terminal_meets_floor(term)) {
		std::fprintf(stderr,
		             "lesh: %s is below the terminal floor (ANSI, 256 colours, "
		             "bracketed paste)\n",
		             term == nullptr || term[0] == '\0' ? "an unset $TERM" : term);
		return 2;
	}

	// #120 left this awaiting a caller, and this is it. `interactive = true`
	// FORBIDS the stderr sink outright (#98): a diagnostic written over the edit
	// line corrupts the one surface the user is looking at, and there is no
	// override because the message that corrupted it is the message they cannot
	// read. A log file that will not open costs diagnostics, not the session, so
	// the answer is deliberately not checked.
	lesh::log::options logging;
	logging.interactive = true;
	logging.tty = ttyname(STDIN_FILENO);
	logging.floor = "ansi+256+paste";
	(void)lesh::log::configure_from_environment(logging);

	// The four providers (#94). Owned HERE, so they outlive the session that
	// borrows them and are freed before main returns (ADR-0007).
	const lesh::leshper::shell_syntax_layer syntax;
	const lesh::leshper::shell_prompt_source prompt{state};

	// #101: a non-interactive shell touches no history file, and `history_store`
	// has no way to know which kind of shell built it - so the decision is made
	// here, once, by not building one. No `$HOME` means no path and no store,
	// which reads back as an empty history rather than as an error.
	std::optional<lesh::runtime::history_store> history;
	if (const std::optional<std::string> path = lesh::runtime::history_store::default_path())
		history.emplace(*path);
	const lesh::leshper::vector_history_source empty_history;
	std::optional<lesh::leshper::history_store_source> recorded;
	if (history.has_value())
		recorded.emplace(*history);

	lesh::leshper::provider_bundle providers;
	providers.syntax = &syntax;
	providers.prompt = &prompt;
	providers.history = recorded.has_value()
		? static_cast<const lesh::leshper::history_source*>(&*recorded)
		: static_cast<const lesh::leshper::history_source*>(&empty_history);
	providers.store = history.has_value() ? &*history : nullptr;
	// The completer is #94's fourth provider and v1 has none. Null, and the
	// bundle says so.
	providers.completion = nullptr;

	// WHICH rc, decided here; WHEN it runs is the session's (#101 decision 3).
	// `$ENV` wins when it is set, per POSIX for an interactive shell; otherwise
	// `~/.leshrc`, one file, no search - the neovim-shaped lookup arrives with
	// the Lua runtime.
	std::string rc;
	if (const char* env = std::getenv("ENV"); env != nullptr && env[0] != '\0') {
		rc = env;
	} else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
		rc = std::string{home} + "/.leshrc";
	}
	const int status = lesh::leshper::run_interactive_shell(state, pool, providers,
	                                                        STDIN_FILENO, STDOUT_FILENO, rc);
	// ADR-0007: the logger owns two descriptors, and this is where they go back.
	lesh::log::shutdown();
	return status;
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

	// THE USER'S `$TERM`, read BEFORE the line below overwrites it. #97's floor
	// check is about the terminal the user is actually on, and the `setenv` is a
	// conformance-corpus convenience that would otherwise answer the question for
	// every terminal in the world with the same string.
	const char* const term_on_entry = std::getenv("TERM");
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
	// a file, so only a terminal gets the editor.
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
	return run_interactive(state, pool, term_on_entry);
}
