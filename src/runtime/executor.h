#pragma once

#include "runtime/builtins.h"
#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "syntax/ast.h"

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
	int run_negation(const syntax::tree& t, syntax::node_index n);

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
	int run_file(std::string_view path);
	// Records what fd `n` holds before a redirection displaces it. Returns false
	// only when the copy could not be made for a reason other than "it was not
	// open", which is recorded rather than treated as an error.
	bool save_fd(int fd, arena_array<saved_fd>* restore);
	bool apply_redirection(const syntax::tree& t, syntax::node_index n,
	                       arena_array<saved_fd>* restore);
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
	void apply_assignment(std::string_view text);

	// forks, execs, and returns the child's pid. Never returns in the child.
	[[nodiscard]] pid_t spawn(arena_array<char*>& argv, const spawn_context& ctx,
	                          const arena_array<std::string_view>* assignments = nullptr,
	                          const syntax::tree* t = nullptr,
	                          syntax::node_index command = 0);

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
