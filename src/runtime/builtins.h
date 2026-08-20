#pragma once

#include "runtime/shell_state.h"

#include <array>
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

// Where a builtin's implementation lives.
//
// Not a detail: a name the shell CLASSIFIES as a builtin is never looked up on
// PATH, so a classified name with no implementation anywhere is a command that
// cannot run and does not say so. Recording the answer in the registry is what
// lets the compiler check it.
enum class builtin_home {
	table,     // the handler table in builtins.cpp
	executor,  // runtime/executor.cpp - needs the front end, the process, or jobs
};

struct builtin_descriptor {
	std::string_view name;
	builtin_kind kind;
	builtin_home home;
};

// THE registry: every name POSIX gives this shell as a builtin, its kind, and
// where it is implemented. `classify_builtin` reads this, and builtins.cpp
// static_asserts the handler table against it, so a name cannot be classified
// without an implementation.
//
// This existed as two lists in shell_state.cpp with no relation to the handler
// table, and they disagreed: `test` and `readonly` were classified with no
// handler, which made the search order stop before PATH and left
// `test 1 = 2; echo $?` reporting 0 (#35).
//
// `[` is here with `test` and runs the same handler. POSIX lists it as a utility
// of its own with the same operators, dash and every other shell make it a
// builtin, and leaving it out meant `[ 1 = 2 ]` forked /bin/[ - which answered
// correctly and hid the fact that `test` did not.
//
// `getopts` is regular and not special: POSIX 2.14 lists it nowhere in the
// special set, so its failure must not exit a non-interactive shell - `getopts`
// with too few operands has to report and carry on, the way dash does.
constexpr std::array<builtin_descriptor, 29> kBuiltinRegistry = {{
	// POSIX XCU 2.14, the special builtins. The list is closed and the membership
	// matters: a failure in one of these exits a non-interactive shell, and
	// assignments preceding one persist.
	{"break", builtin_kind::special, builtin_home::table},
	{":", builtin_kind::special, builtin_home::table},
	{"continue", builtin_kind::special, builtin_home::table},
	{".", builtin_kind::special, builtin_home::executor},
	{"eval", builtin_kind::special, builtin_home::executor},
	{"exec", builtin_kind::special, builtin_home::executor},
	{"exit", builtin_kind::special, builtin_home::table},
	{"export", builtin_kind::special, builtin_home::table},
	{"readonly", builtin_kind::special, builtin_home::table},
	{"return", builtin_kind::special, builtin_home::table},
	{"set", builtin_kind::special, builtin_home::table},
	{"shift", builtin_kind::special, builtin_home::table},
	{"times", builtin_kind::special, builtin_home::table},
	{"trap", builtin_kind::special, builtin_home::table},
	// `unset` is here, but the `-f` FORM is intercepted by the executor: removing
	// a function means reaching the function table, which lives there.
	{"unset", builtin_kind::special, builtin_home::table},
	// The regular builtins lesh implements.
	{"alias", builtin_kind::regular, builtin_home::table},
	{"cd", builtin_kind::regular, builtin_home::table},
	{"command", builtin_kind::regular, builtin_home::table},
	{"echo", builtin_kind::regular, builtin_home::table},
	{"false", builtin_kind::regular, builtin_home::table},
	{"getopts", builtin_kind::regular, builtin_home::table},
	{"kill", builtin_kind::regular, builtin_home::table},
	{"pwd", builtin_kind::regular, builtin_home::table},
	{"read", builtin_kind::regular, builtin_home::table},
	{"test", builtin_kind::regular, builtin_home::table},
	{"[", builtin_kind::regular, builtin_home::table},
	{"true", builtin_kind::regular, builtin_home::table},
	{"unalias", builtin_kind::regular, builtin_home::table},
	{"wait", builtin_kind::regular, builtin_home::executor},
}};

// argv is NUL-terminated, as for exec. Returns false when the name is not
// implemented HERE - either it is no builtin at all, or the registry says the
// executor owns it. Either way the caller must not treat the call as having run:
// the false return was discarded with a `(void)`, and that is why an
// unimplemented `test` reported success (#35).
[[nodiscard]] bool try_run_builtin(shell_state& state, char** argv, builtin_result& out);

// Whether the handler table has an entry for this name, WITHOUT running it.
//
// The runtime half of the registry guard: a test can ask what the dispatch would
// do for every registered name, which calling them would not allow - `read` with
// no arguments blocks on standard input and `cd` would move the test process.
[[nodiscard]] bool builtin_has_handler(std::string_view name) noexcept;

// True when `unset`'s options select FUNCTIONS rather than variables. One parser,
// because the executor decides whether to intercept the call and the builtin
// decides what to do with the operands, and two readings of `-fv` would disagree.
[[nodiscard]] bool unset_selects_functions(char** argv) noexcept;

// Where the registry says this name is implemented. `builtin_home::table` for a
// name that is not a builtin at all - the caller has classify_builtin for that
// question, and this one only says "not the executor's".
[[nodiscard]] builtin_home builtin_home_of(std::string_view name) noexcept;

} // namespace lesh::runtime
