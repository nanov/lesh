#pragma once
#include "substrate/numeric.h"

#include "runtime/shell_state.h"

#include <array>
#include <cstddef>
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
	// SIGINT caught by an interactive shell's default action (#52). Not something a
	// builtin can produce - it arrives asynchronously - but it unwinds exactly the
	// way `break` and `return` do, so every construct that already lets those out
	// lets this out, and none of them had to learn a second mechanism.
	interrupted,
};

// WHICH INPUT A COMMAND LOOP IS READING (#74).
//
// The shell reads commands in three situations - its own input; a nested source
// an `eval`, a `.`, a trap body or a command substitution handed it; and the body
// of a compound command - and the loop is the SAME loop for all three
// (`run_command_list`, reached from `run_parsed` and `run_compound_list`). Every
// guard it applies is shared: the syntax-error exit, `set -n`, pending traps, the
// interrupt unwind, `set -e`.
//
// One rule is not, and this enum exists to carry exactly that one. A `return`,
// `break` or `continue` that escapes every construct around it ENDS the shell's
// own input - dash and zsh both stop there - whereas out of an `eval` it must
// TRAVEL, because `eval return` inside a function returns from the function
// (return-p.tst's 'returning out of eval'). One flag, named, beats two loops that
// drift: the copy this replaced went without `run_pending_traps` for its whole
// life, and the moment #67 routed a command substitution through it that cost
// fifteen signal files three assertions each.
enum class source_kind : uint8_t {
	shell_input, // the shell's own input: an escaping unwind ends it
	nested,      // an `eval` operand, a dot script, a trap body, a substituted body
};

// WHICH OF POSIX XCU 2.8.1'S ERROR CLASSES A NON-ZERO STATUS BELONGS TO (#66).
//
// That table is what makes a special builtin's failure fatal to a non-interactive
// shell, and the rows it lists are closed: a shell language syntax error, an
// expansion error, a redirection error, a variable assignment error, and a
// UTILITY SYNTAX ERROR - which the table itself parenthesises as "option or
// operand error", meaning the command line was not the shape the utility accepts.
// A command line that WAS the right shape, whose operation then failed, is on none
// of those rows.
//
// The distinction had no representation here, so try_run_builtin applied the rule
// to any non-zero status at all and `trap "" "" || echo reached` killed the script
// at a line whose author had written down that the failure was expected. dash,
// bash, zsh and yash all draw the line; lesh collapsed it.
//
// `usage` IS THE DEFAULT, and deliberately so. Nearly every failure a special
// builtin can report is a malformed command line, that is the answer #34 built
// error-p.tst's 220 assertions on, and a new builtin that says nothing therefore
// keeps the answer this shell already gives. The exceptions name themselves, which
// is the direction that fails safe: forgetting the marker leaves a shell that
// exits too eagerly, where the inverted default would leave one that runs on past
// an error POSIX requires it to die on.
enum class failure_kind {
	usage,        // the command line was wrong: fatal in a special builtin
	operational,  // the command line was right and the operation failed: never fatal
};

struct builtin_result {
	int status = 0;
	control_flow flow = control_flow::normal;
	int level = 1;  // for break/continue N
	failure_kind failure = failure_kind::usage;
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

// What `command -v` writes for a builtin: its own name, or the pathname the
// search would find for it.
//
// POSIX XCU `command` requires a bare name for the built-ins that must BE built
// in - a special builtin, and the ones whose whole purpose is to touch the
// shell's own state - and an absolute pathname for a REGULAR built-in utility,
// which is a utility the shell could equally have found on PATH and chose to
// implement itself. So `command -v echo` writes /bin/echo and `command -v read`
// writes read.
//
// A field rather than something derived from `kind`: kind decides the two things
// POSIX attaches to a SPECIAL builtin (a failure exits the shell, assignments
// persist), and every name here that is not special shares that answer while
// splitting two ways on this one.
//
// dash writes the bare name for both and fails command-p.tst's 'output of
// describing non-special built-in (-v)' for it. The divergence is recorded in
// tests/spec/posix_gaps.spec rather than chosen quietly.
enum class builtin_report {
	name,      // written as itself: a special or must-be-built-in utility
	pathname,  // written as the absolute pathname the search finds
};

struct builtin_descriptor {
	std::string_view name;
	builtin_kind kind;
	builtin_home home;
	builtin_report report = builtin_report::name;
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
	// `unset` is here for BOTH forms. The `-f` form used to be intercepted by the
	// executor, because the function table was the executor's; #106 moved it to
	// shell state, which is what try_run_builtin is handed.
	{"unset", builtin_kind::special, builtin_home::table},
	// The regular builtins lesh implements. `builtin_report` splits them: the ones
	// that exist only to change the shell it runs in are reported by name, and the
	// ones that are also utilities on PATH are reported by pathname. `alias`,
	// `cd`, `command`, `getopts`, `read`, `unalias` and `wait` are in the first
	// group because none of them could do its job in a separate process at all.
	{"alias", builtin_kind::regular, builtin_home::table},
	{"cd", builtin_kind::regular, builtin_home::table},
	// `command` is the executor's: the RUNNING form has to bypass function lookup
	// on its way to a command the executor alone knows how to run, and the
	// DESCRIBING form needs the reserved words the parser holds.
	{"command", builtin_kind::regular, builtin_home::executor},
	{"echo", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"false", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"getopts", builtin_kind::regular, builtin_home::table},
	{"kill", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"pwd", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"read", builtin_kind::regular, builtin_home::table},
	{"test", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"[", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"true", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"unalias", builtin_kind::regular, builtin_home::table},
	{"wait", builtin_kind::regular, builtin_home::executor},
}};

// Where a utility's OPERANDS begin, having discarded a leading `--`.
//
// POSIX XCU 1.4 Utility Description Defaults: a standard utility that accepts
// operands and no options "shall recognize `--` as a first argument to be
// discarded". `break`, `continue`, `.`, `eval`, `exit` and `return` are every one
// of that shape, and every one of them read argv[1] as its operand directly - so
// `return -- 56` returned 0, `exit -- 56` exited 0, and `eval -- 'echo foo'`
// looked for a command named `--`.
//
// ONE reading, not a sixth copy: `exec` (#31), `trap` (#33), `command` (#31),
// `read`, `export`, `readonly` and `unset` each grew their own, and seven
// readings of the same two bytes is how one of them comes to disagree with the
// rest - the drift the builtin registry exists to prevent (#35).
//
// dash rejects all four of these - `dash -c 'exit -- 56'` says
// `exit: Illegal number: --` - and fails 'separator preceding operand' in
// return-p.tst, exit-p.tst and eval-p.tst for it. Divergence from the reference
// shell recorded rather than copied, exactly as for `exec --`: dash is behind the
// standard here.
[[nodiscard]] constexpr size_t first_operand(char* const* argv) noexcept {
	return argv[1] != nullptr && std::string_view{argv[1]} == "--" ? 2 : 1;
}

// A NUMERIC OPERAND A BUILTIN WILL NOT TAKE. Reports it and answers the status to
// return - 2, as for any other builtin's usage error, and what dash answers for
// all four of the sites that use it. `exit`, `shift` and `return` are SPECIAL, so
// the executor turns that into an exit for a non-interactive shell, which is what
// dash does too; `wait` is regular and merely reports.
//
// ONE WORDING for the four, because four wordings for one refusal is how four
// calls to `std::atoi` came to answer four different ways to begin with. dash
// writes `Illegal number` for both failures; saying WHICH WAY it failed is the
// whole content of numeric_parse, so the two are told apart here.
[[nodiscard]] int report_bad_number(std::string_view builtin, std::string_view operand,
                                    numeric_parse why);

// argv is NUL-terminated, as for exec. Returns false when the name is not
// implemented HERE - either it is no builtin at all, or the registry says the
// executor owns it. Either way the caller must not treat the call as having run:
// the false return was discarded with a `(void)`, and that is why an
// unimplemented `test` reported success (#35).
//
// `demoted` is the caller's `command` prefix. POSIX XCU `command` says that when
// the name is a special builtin "the special properties in XCU 2.14 shall not
// occur", and 2.14 names exactly two - the abort and the persisting assignment.
// The executor demoted the assignment and the redirection failure and could not
// demote the status, because this function was handed nothing but shell state and
// argv, so `command set -Z` still ended the shell where dash and bash report and
// carry on. Not defaulted: the two call sites both have a `command` prefix to
// hand, and a default would let a third forget to pass it (#66).
[[nodiscard]] bool try_run_builtin(shell_state& state, char** argv, builtin_result& out,
                                   bool demoted);

// Whether the handler table has an entry for this name, WITHOUT running it.
//
// The runtime half of the registry guard: a test can ask what the dispatch would
// do for every registered name, which calling them would not allow - `read` with
// no arguments blocks on standard input and `cd` would move the test process.
[[nodiscard]] bool builtin_has_handler(std::string_view name) noexcept;

// True when `unset`'s options select FUNCTIONS rather than variables. Exposed
// rather than file-local because it is the one reading of `-fv` the builtin makes,
// and a second reading anywhere would be free to disagree with it.
[[nodiscard]] bool unset_selects_functions(char** argv) noexcept;

// Where the registry says this name is implemented. `builtin_home::table` for a
// name that is not a builtin at all - the caller has classify_builtin for that
// question, and this one only says "not the executor's".
[[nodiscard]] builtin_home builtin_home_of(std::string_view name) noexcept;

// Prints `name='value'`, the form POSIX gives an alias definition: quoted so the
// shell reads it back as exactly these bytes. `alias` lists with it and
// `command -v` describes with it, because POSIX writes an alias as a command line
// that re-creates it - `eval "$(command -v abc)"` has to define the same alias.
void print_alias(std::string_view name, std::string_view value);

// How `command -v` must write this builtin's name. `builtin_report::name` for a
// name that is not a builtin at all, which no caller asks about: classify_builtin
// answers that question first.
[[nodiscard]] builtin_report builtin_report_of(std::string_view name) noexcept;

} // namespace lesh::runtime
