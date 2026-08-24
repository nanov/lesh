#pragma once

#include "runtime/builtins.h"
#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "syntax/ast.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/types.h>

namespace lesh::runtime {

// Runs a parse tree. See issues #12 and #18.
//
// AN INTERFACE, so the back end stays a genuine choice. A tree-walking
// implementation ships first; an own bytecode VM or lowering to the extension
// runtime remain open, and nothing above this depends on which is behind it.
class executor {
public:
	virtual ~executor() = default;

	// Returns the exit status of the last command, which is what `$?` and a
	// non-interactive shell's own exit status are defined to be.
	[[nodiscard]] virtual int run(const syntax::tree& t) = 0;
};

class tree_walking_executor final : public executor {
public:
	tree_walking_executor(buffer_pool& pool, shell_state& state) noexcept
		: _pool(pool), _state(state), _runner(*this) {}

	[[nodiscard]] int run(const syntax::tree& t) override;

	// Reads and runs a whole input the way POSIX says a shell reads one: ONE
	// COMPLETE COMMAND at a time, each executed before the next is read.
	//
	// That timing is observable, and not only by aliases: `alias e=echo` takes
	// effect for the line after it and not for the rest of its own line, `set -v`
	// echoes each command as it is read, and a syntax error halfway down a script
	// runs everything above it. Parsing the whole input first got all three wrong -
	// alias-p.tst scored 13/67 for that one reason, with a working substitution
	// mechanism behind it (#40).
	//
	// Takes the source rather than a tree, because it does the parsing itself: it
	// is the read loop, and the trees it produces have to outlive it.
	[[nodiscard]] int run_input(std::string_view source, bool echo_when_verbose = true);

	// The port the expander takes (#11). The executor supplies it, rather than the
	// expander depending on the executor - which is what breaks the cycle every
	// shell surveyed in #14 has.
	[[nodiscard]] command_runner& as_command_runner() noexcept { return _runner; }

private:
	// Where a spawned process sends its output and takes its input, and which
	// process group it joins.
	//
	// PROCESS GROUPS. Every child goes in a group, and every member of a PIPELINE
	// goes in the SAME group - that is what makes Ctrl-C kill a whole pipeline
	// rather than one stage, and it is most of the machinery job control would
	// need. Job control is out of scope, but reaping is not: #4 established that
	// lesh must supply its own, whatever conformance corpus it runs.
	// An fd displaced by a redirection, so a builtin's redirections can be undone.
	//
	// `saved` holds `closed` when the displaced fd was NOT OPEN. Recording that
	// is the whole point of the field being a sentinel rather than just a copy:
	// with `{original, saved}` alone, a dup that failed because there was nothing
	// to dup was indistinguishable from "nothing needed saving", so nothing was
	// pushed and restore_fds never closed the fd the redirection had just opened.
	// That is issue #34 - `{ :; } 3>&2; echo foo >&3` printed foo instead of
	// failing, because fd 3 outlived the brace group that opened it.
	struct saved_fd {
		static constexpr int closed = -1;
		int original;
		int saved;
	};

	struct spawn_context {
		int input_fd = 0;
		int output_fd = 1;
		pid_t group = 0;  // 0 means "this child becomes the group leader"
	};

	// A variable's value before an assignment prefix overwrote it, so the command
	// it preceded can put it back. Owns its strings: the state's own storage is
	// what is being overwritten, so a view into it would dangle.
	struct saved_variable {
		std::string name;
		std::string value;
		bool was_set = false;
	};

	// What a `command` prefix asked for, POSIX XCU `command`:
	// `command [-p] name [argument...]` runs a command with function lookup
	// bypassed, and `command [-p] -v|-V name` describes one instead of running it.
	struct command_prefix {
		bool present = false;        // a prefix was stripped: bypass functions, demote
		bool standard_path = false;  // -p: search the standard path, not $PATH
		char describe = '\0';        // -v or -V, or neither
		char bad_option = '\0';
		size_t operand = 1;          // where the operands begin in argv
	};

	// Reads a `command` prefix's options WITHOUT removing anything.
	//
	// One reading, shared by the caller that strips the prefix to run what follows
	// and by the describing form that reports on its operand. Two readings of
	// `-pv` would eventually disagree, which is the drift the builtin registry
	// exists to prevent (#35).
	[[nodiscard]] static command_prefix read_command_options(char* const* argv) noexcept;

	// Strips every `command` prefix off the front of argv, leaving the command to
	// run. argv is left ALONE when the prefix asked to DESCRIBE a name, because
	// then there is nothing to run and describe_command reads the operand itself.
	command_prefix take_command_prefix(arena_array<char*>& argv);

	// `command -v` and `command -V`. See the definition for why it is here and not
	// in builtins.cpp.
	int describe_command(const command_prefix& opts, char* const* argv);

	// The absolute pathname the command search finds for `name`, or false when
	// there is no executable of that name on the path searched. `standard` picks
	// the path `command -p` guarantees over $PATH.
	[[nodiscard]] bool search_path_for(std::string_view name, bool standard,
	                                   std::string& out) const;

	// Runs one already-parsed tree, WITHOUT the EXIT trap: read a command at a
	// time, the trap belongs at the end of the input rather than at the end of
	// every command in it.
	int run_parsed(const syntax::tree& t);
	void echo_if_verbose(std::string_view unit, bool enabled);
	int run_node(const syntax::tree& t, syntax::node_index n);
	int run_simple_command(const syntax::tree& t, syntax::node_index n);
	int run_pipeline(const syntax::tree& t, syntax::node_index n);
	int run_and_or(const syntax::tree& t, syntax::node_index n);
	int run_compound_list(const syntax::tree& t, syntax::node_index n);
	int run_if(const syntax::tree& t, syntax::node_index n);
	int run_loop(const syntax::tree& t, syntax::node_index n, bool until);
	int run_for(const syntax::tree& t, syntax::node_index n);
	int run_case(const syntax::tree& t, syntax::node_index n);
	int run_subshell(const syntax::tree& t, syntax::node_index n);
	int run_async(const syntax::tree& t, syntax::node_index n);
	int run_function_definition(const syntax::tree& t, syntax::node_index n);
	// Returns false when the name is not a function, so the caller execs instead.
	bool try_run_function(const syntax::tree& t, arena_array<char*>& argv, int& status);
	bool consume_loop_flow(bool& should_break);
	// Parses and runs source in this environment. Used by `eval` and `.`.
	int run_source(std::string_view source);
	// `exec`. Returns only when there is no command to become, or the exec failed;
	// on success this process IS the command. See the definition for why it is not
	// a builtins.cpp builtin.
	int run_exec(const syntax::tree& t, syntax::node_index n,
	             arena_array<char*>& argv,
	             const arena_array<std::string_view>& assignments, bool demoted);
	// The builtins the executor owns - `command`, `exec`, `wait`, `eval` and `.`.
	// False when the name is none of them. See the definition, and
	// builtin_home::executor in runtime/builtins.h.
	[[nodiscard]] bool try_run_executor_builtin(
		const syntax::tree& t, syntax::node_index n, arena_array<char*>& argv,
		const arena_array<std::string_view>& assignments, const command_prefix& cmd,
		int& status);
	// `wait`, which needs the executor's record of background jobs.
	int run_wait(char* const* argv);
	// `unset -f`: the function form, which needs the function table.
	builtin_result run_unset_functions(char** argv);
	int run_negation(const syntax::tree& t, syntax::node_index n);

	// Builds an expander wired to this executor AND to this shell's options.
	//
	// One place, so an option cannot be honoured by three of the four expansion
	// sites and forgotten by the fourth. `set -u` was recorded and inert for
	// exactly that kind of reason: nothing ever read the flag.
	[[nodiscard]] expander make_expander() noexcept {
		return expander{_pool, _state, &_runner, !_state.opts().no_glob, &_state,
		                &_state, _state.opts().error_on_unset};
	}
	// True when `ex` reported an expansion error POSIX makes fatal, having arranged
	// for a non-interactive shell to stop. `${x?}` and `set -u` on an unset
	// parameter are the two cases; an interactive shell reports and carries on.
	bool expansion_failed(const expander& ex) noexcept {
		if (!ex.fatal_error())
			return false;
		_expansion_error = true;
		if (!_state.interactive())
			_exit_requested = true;
		return true;
	}

	// Expands an assignment's value and returns `NAME=value` with the value
	// expanded, WITHOUT applying it. Split out from apply_assignment because
	// `set -x` has to print the value the assignment takes, and expanding a second
	// time to print it would run a command substitution in the value twice.
	std::string_view expand_assignment(std::string_view text);
	// False when the name is READONLY, having reported it. See the definition.
	[[nodiscard]] bool apply_expanded_assignment(std::string_view expanded);
	// POSIX 2.8.1's variable assignment error: arranges for a non-interactive
	// shell to exit and returns the status to report.
	int assignment_error();
	// One `set -x` trace line: the expanded PS4, then the command as it will run.
	void trace_command(const arena_array<std::string_view>& prefix,
	                   char* const* argv);

	// True when a failing command must exit the shell rather than merely end a list.
	[[nodiscard]] bool errexit_fires(int status) const noexcept {
		return status != 0 && _state.opts().exit_on_error &&
		       _errexit_suppressed == 0 && !_status_tested;
	}

	// Suppresses `set -e` for the duration of a scope. RAII rather than
	// increment/decrement by hand, so an early return cannot leave the count
	// raised - which would silently disable errexit for the rest of the run.
	class errexit_suppression {
	public:
		explicit errexit_suppression(tree_walking_executor& owner) noexcept
			: _owner(owner) { ++_owner._errexit_suppressed; }
		~errexit_suppression() { --_owner._errexit_suppressed; }
		errexit_suppression(const errexit_suppression&) = delete;
		errexit_suppression& operator=(const errexit_suppression&) = delete;
	private:
		tree_walking_executor& _owner;
	};
	void run_pending_traps();
	void run_exit_trap();
	// Finds and runs a dot script, `.`'s whole job. False when the script could not
	// be found or read, having reported it: the caller decides what that costs, the
	// answer being different for an interactive shell and for `command .`.
	bool run_dot_script(std::string_view operand, int& status);
	// The pathname $PATH gives for a dot script. Separate from search_path_for
	// because a dot script need only be READABLE - see the definition.
	[[nodiscard]] bool search_path_for_dot(std::string_view name,
	                                       std::string& out) const;
	// Records what fd `n` holds before a redirection displaces it. Returns false
	// only when the copy could not be made for a reason other than "it was not
	// open", which is recorded rather than treated as an error.
	bool save_fd(int fd, arena_array<saved_fd>* restore);
	bool apply_redirection(const syntax::tree& t, syntax::node_index n,
	                       arena_array<saved_fd>* restore);
	// A token's text with its line continuations removed. See the definition.
	[[nodiscard]] std::string_view joined_text(std::string_view text);
	bool apply_here_doc(const syntax::tree& t, syntax::node_index n,
	                    arena_array<saved_fd>* restore);
	bool apply_redirections(const syntax::tree& t, syntax::node_index command,
	                        arena_array<saved_fd>* restore);
	void restore_fds(arena_array<saved_fd>& saved);
	// True when a builtin's output could not be written, having reported it and
	// thrown away whatever stdio still holds.
	bool drop_unwritable_output(const char* name);
	// True when a command carries any redirection at all, so the no-command-name
	// case can skip forking when there is nothing to perform.
	[[nodiscard]] static bool has_redirections(const syntax::tree& t,
	                                           syntax::node_index command) noexcept;
	// Performs the redirections of a command that has NO command name, in a
	// subshell, and returns the status POSIX gives that command.
	int run_redirections_only(const syntax::tree& t, syntax::node_index n);

	// Expands a command's words into a NUL-terminated argv the arena owns.
	// Returns false when the command expanded to nothing at all, which is not an
	// error - `$empty` as an entire command is a no-op.
	bool build_argv(const syntax::tree& t, syntax::node_index n,
	                arena_array<char*>& argv,
	                arena_array<std::string_view>* assignments = nullptr);
	[[nodiscard]] bool apply_assignment(std::string_view text);

	// Applies an assignment PREFIX, remembering what to put back.
	//
	// POSIX 2.9.1: a prefix on a SPECIAL builtin persists after the command - which
	// is why `x=1 . /dev/null; echo $x` prints 1 in dash - and a prefix on anything
	// else lasts only for its duration. `command` DEMOTES a special builtin to a
	// regular one, and then the prefix is restored too: `a=b command :; echo $a`
	// prints `a`.
	//
	// `restore` is left EMPTY when the prefix persists, so one call site can serve
	// both rules. Returns false when a name was refused as readonly, having
	// reported it - and the caller must still run the restores, or a redirection it
	// applied would leak into the shell's own fds.
	//
	// One function because four call sites apply a prefix - a function call, a
	// builtin in the table, one of the executor's own, and `exec` - and two of them
	// simply forgot to. `x=1 eval 'echo $x'` printed a blank line for that reason.
	[[nodiscard]] bool apply_prefix(const arena_array<std::string_view>& assignments,
	                                bool persist, std::vector<saved_variable>& restore);
	// Puts back what apply_prefix displaced.
	void restore_prefix(const std::vector<saved_variable>& restore);
	// Expands the values of an assignment prefix, leaving the names alone, so the
	// child of a fork is handed text it only has to split and set.
	//
	// The expansion belongs to the shell that READ the command, and doing it in the
	// child was a bug rather than an optimisation: the child built its expander with
	// no command runner, so a substitution in a prefix value had nothing to run it
	// and `x=$(echo z) sh -c 'echo $x'` exported x empty (#31).
	[[nodiscard]] bool expand_prefix(const arena_array<std::string_view>& assignments,
	                                 arena_array<std::string_view>& expanded);

	// forks, execs, and returns the child's pid. Never returns in the child.
	[[nodiscard]] pid_t spawn(arena_array<char*>& argv, const spawn_context& ctx,
	                          const arena_array<std::string_view>* assignments = nullptr,
	                          const syntax::tree* t = nullptr,
	                          syntax::node_index command = 0,
	                          bool standard_path = false);

	// Applies a command's redirections and its assignment prefix and BECOMES it.
	//
	// Split out of `spawn`, which is now this with a fork in front of it, so a
	// pipeline stage - already running in the process forked for it - can exec
	// without forking a second time.
	//
	// `assignments` are ALREADY EXPANDED - see expand_prefix. There is no expander
	// here on purpose: the one that used to be built here had no command runner, so
	// a command substitution in a prefix value expanded to nothing.
	// `standard_path` is what `command -p` asks for: the search uses the path POSIX
	// guarantees finds the standard utilities rather than $PATH, so
	// `PATH= command -p cat` still runs cat.
	[[noreturn]] void become_command(arena_array<char*>& argv,
	                                 const arena_array<std::string_view>* assignments,
	                                 const syntax::tree* t, syntax::node_index command,
	                                 bool standard_path_wanted = false);

	// Runs one SIMPLE-COMMAND pipeline stage, in the process forked for it and
	// after its pipe is on its fds. Returns the status to _exit with, or never
	// returns because the stage became an external command.
	int run_pipeline_stage(const syntax::tree& t, syntax::node_index stage);

	// Runs a whole tree with stdout captured. Used by command substitution.
	[[nodiscard]] bool capture(std::string_view code, arena_array<char>& out);

	// Adapts this executor to the expander's port. A member rather than a global,
	// so nesting works: `$(a $(b) c)` needs the inner capture to reach the same
	// executor as the outer one.
	class runner_adapter final : public command_runner {
	public:
		explicit runner_adapter(tree_walking_executor& owner) noexcept : _owner(owner) {}
		bool run_and_capture(std::string_view code, arena_array<char>& out) override {
			return _owner.capture(code, out);
		}
	private:
		tree_walking_executor& _owner;
	};

	buffer_pool& _pool;
	shell_state& _state;
	runner_adapter _runner;
	// How many command substitutions this executor has completed. Only the DELTA
	// is ever read: it tells a command with no command name whether it performed a
	// substitution whose status it must adopt (POSIX 2.9.1).
	uint64_t _substitutions = 0;
	// Set by `exit`, so the program loop stops rather than running the next command.
	bool _exit_requested = false;
	// POSIX suppresses `set -e` wherever a status is being TESTED rather than acted
	// on: the condition of if/while/until, every operand of an and-or list but the
	// last, and a pipeline negated with `!`. A depth counter, not a flag, because
	// the suppression covers the whole subtree - a command nested three deep inside
	// an `if` condition must not exit the shell either.
	uint32_t _errexit_suppressed = 0;
	// True when the status just returned came from a construct whose LAST command
	// never ran: a short-circuited and-or list, or a `!` pipeline. POSIX exempts
	// those from `set -e`, and the exemption travels outward through brace groups,
	// `if` bodies and loop bodies - `set -e; { false && echo a; }` does not exit -
	// but NOT across a subshell or a function call, which are commands of their own
	// and do exit. Verified against both dash and bash.
	bool _status_tested = false;
	// Set when the expansion of the command being run failed fatally. Cleared at
	// the start of every simple command, so it answers about THIS command and not
	// about one three constructs ago.
	bool _expansion_error = false;
	bool _exit_trap_ran = false;
	// Guards against a trap body triggering its own trap recursively.
	bool _in_trap = false;
	// Set by break, continue and return; consumed by the enclosing loop or
	// function. _flow_level implements `break 2`.
	control_flow _flow = control_flow::normal;
	int _flow_level = 0;

	// Defined functions, by name.
	//
	// A body is stored as a node in the tree it was parsed from, so the tree must
	// outlive the definition. That holds for one invocation - `-c 'f() {...}; f'`
	// is a single parse - and is the ONLY case that works today. Persisting a
	// function across invocations, which an interactive shell needs, requires
	// copying the body into storage the function owns; that is ADR-0007 work and
	// lands with the line editor. Recorded rather than pretended.
	struct defined_function {
		const syntax::tree* tree;
		syntax::node_index body;
	};
	std::unordered_map<std::string, defined_function> _functions;

	// Every tree run_input has parsed, kept alive for the whole run.
	//
	// A deque, not a vector: _functions points INTO these trees, and a vector
	// reallocating would move them out from under it. Reading one command at a time
	// is what makes this necessary - a whole-input parse had exactly one tree and
	// the question never came up.
	std::deque<syntax::tree> _input_trees;

	// Guards runaway recursion. A shell function that calls itself unconditionally
	// would otherwise exhaust the stack rather than reporting anything.
	static constexpr int kMaxFunctionDepth = 256;
	int _function_depth = 0;
	// Background jobs, so `wait` can reap them and they do not become zombies.
	std::vector<pid_t> _background;

	// A runaway `while true` would otherwise take the machine down - which is
	// precisely how a hung test cost this project a pegged core once already.
	static constexpr uint64_t kMaxLoopIterations = 10'000'000;
};

} // namespace lesh::runtime
