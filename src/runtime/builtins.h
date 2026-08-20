#pragma once

#include "runtime/shell_state.h"

#include <string_view>

namespace lesh::runtime {

// The POSIX builtin set. See issue #24.
//
// A builtin runs in THIS process, which is the whole point: `cd` in a forked
// child would change the child's directory and then exit, achieving nothing.
// That is why dispatch happens before the fork rather than after.

// How a builtin finished, and what the shell should do about it.
//
// Exit status alone is not enough: `exit`, `return`, `break` and `continue` all
// need to unwind the executor rather than merely report a number, and POSIX makes
// a failing SPECIAL builtin fatal to a non-interactive shell while the same
// failure in a regular one is not. Encoding that in the return type means the
// caller cannot forget it.
enum class control_flow {
	normal,        // carry on
	exit_shell,    // `exit`, or a special builtin that failed
	return_from,   // `return` - needs functions (#25)
	break_loop,    // `break` - needs loops (#19)
	continue_loop, // `continue` - needs loops (#19)
};

struct builtin_result {
	int status = 0;
	control_flow flow = control_flow::normal;
	int level = 1;  // for break/continue N
};

// argv is NUL-terminated, as for exec. Returns false when the name is not a
// builtin at all, leaving the caller to fork and exec.
[[nodiscard]] bool try_run_builtin(shell_state& state, char** argv, builtin_result& out);

} // namespace lesh::runtime
