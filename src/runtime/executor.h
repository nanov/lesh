#pragma once

#include "runtime/builtins.h"
#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "syntax/ast.h"

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
	bool consume_loop_flow(bool& should_break);

	// Expands a command's words into a NUL-terminated argv the arena owns.
	// Returns false when the command expanded to nothing at all, which is not an
	// error - `$empty` as an entire command is a no-op.
	bool build_argv(const syntax::tree& t, syntax::node_index n,
	                arena_array<char*>& argv,
	                arena_array<std::string_view>* assignments = nullptr);
	void apply_assignment(std::string_view text);

	// forks, execs, and returns the child's pid. Never returns in the child.
	[[nodiscard]] pid_t spawn(arena_array<char*>& argv, const spawn_context& ctx,
	                          const arena_array<std::string_view>* assignments = nullptr);

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
	// Set by `exit`, so the program loop stops rather than running the next command.
	bool _exit_requested = false;
	// Set by break, continue and return; consumed by the enclosing loop or
	// function. _flow_level implements `break 2`.
	control_flow _flow = control_flow::normal;
	int _flow_level = 0;

	// A runaway `while true` would otherwise take the machine down - which is
	// precisely how a hung test cost this project a pegged core once already.
	static constexpr uint64_t kMaxLoopIterations = 10'000'000;
};

} // namespace lesh::runtime
