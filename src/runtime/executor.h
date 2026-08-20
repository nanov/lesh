#pragma once

#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "syntax/ast.h"

namespace lesh::runtime {

// Runs a parse tree. See issue #12.
//
// AN INTERFACE, so the back end stays a genuine choice. A tree-walking
// implementation ships first; an own bytecode VM or lowering to the extension
// runtime remain open, and the spec defers that decision until the front end is
// solid. Nothing above this depends on which one is behind it.
class executor {
public:
	virtual ~executor() = default;

	// Returns the exit status of the last command, which is what `$?` and a
	// non-interactive shell's own exit status are defined to be.
	[[nodiscard]] virtual int run(const syntax::tree& t) = 0;
};

// The tree-walking implementation.
//
// PROCESS GROUPS FROM THE START. Every child is placed in its own process group
// and killed by group, even though job control is out of scope for Phases 0-5.
// This is not speculative generality: #4 established that of every shell test
// runner surveyed, only FreeBSD's kyua reaps process groups correctly, and a
// single hung test escaping a plain timeout cost this project a pegged CPU core
// for nineteen minutes. Whatever conformance corpus lesh runs, lesh supplies the
// reaping. Retrofitting process groups into an executor is far worse than
// building with them.
class tree_walking_executor final : public executor {
public:
	tree_walking_executor(buffer_pool& pool, shell_state& state) noexcept
		: _pool(pool), _state(state) {}

	[[nodiscard]] int run(const syntax::tree& t) override;

	// Runs a command and captures its output. This is the command_runner the
	// expander takes as a port - the executor supplies it, rather than the
	// expander depending on the executor.
	[[nodiscard]] command_runner& as_command_runner() noexcept;

private:
	int run_node(const syntax::tree& t, syntax::node_index n);
	int run_simple_command(const syntax::tree& t, syntax::node_index n);
	int run_pipeline(const syntax::tree& t, syntax::node_index n);
	int run_and_or(const syntax::tree& t, syntax::node_index n);

	buffer_pool& _pool;
	shell_state& _state;
};

} // namespace lesh::runtime
