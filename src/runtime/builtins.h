#pragma once
#include "substrate/numeric.h"

#include "runtime/shell_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
constexpr std::array<builtin_descriptor, 31> kBuiltinRegistry = {{
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
	// `bind` is the rc surface for the keymap registry (#117). Reported by name
	// and never by pathname: there is no `/usr/bin/bind` it could stand in for,
	// and its whole purpose is to change the shell it runs in.
	{"bind", builtin_kind::regular, builtin_home::table},
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
	// `prompt` is `bind` one console over (#157): the rc surface for the prompt
	// engine, where `bind` is the rc surface for the keymap registry. Reported by
	// name on the same argument, and it is the same argument: there is no
	// `/usr/bin/prompt` it could stand in for, and its whole purpose is to change
	// the shell it runs in - a prompt set in a subprocess would die with it.
	{"prompt", builtin_kind::regular, builtin_home::table},
	{"read", builtin_kind::regular, builtin_home::table},
	{"test", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"[", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"true", builtin_kind::regular, builtin_home::table, builtin_report::pathname},
	{"unalias", builtin_kind::regular, builtin_home::table},
	{"wait", builtin_kind::regular, builtin_home::executor},
}};

// ---------------------------------------------------------------------------
// `bind`, and the link boundary it sits on (#117 decision 7, #118).
//
// THE PROBLEM, stated once so nobody re-discovers it: the keymap registry lives
// in leshper (spec §6.4) and `lesh_runtime` does not link `lesh_leshper` -
// CMakeLists puts `lesh` on `lesh_runtime lesh_syntax lesh_ui` and nothing else,
// which is the rule that keeps the editor out of the shell's dependency graph.
// A builtin in this file therefore CANNOT call a keymap function directly, and
// making it able to would mean linking the whole editor into every
// non-interactive `lesh -c` invocation.
//
// THE SHAPE, and it is the one A-5 already uses in the other direction: an
// interface declared on the side that needs it, implemented at the wiring site
// that links both. `leshper::host` is the editor's version - the editor declares
// the one shape it needs of whatever drives it, and `ui::editor_host` fills it in
// over shell state. This is the mirror image: the runtime declares what `bind`
// needs to say, and the session hands it a leshper-backed implementation when it
// starts an interactive one.
//
// Until that wiring exists there is no console installed, and `bind` says so
// rather than pretending: a non-interactive shell has no line editor, exactly as
// bash's `bind` warns when line editing is off.
class binding_console {
public:
	virtual ~binding_console();

	binding_console(const binding_console&) = delete;
	binding_console& operator=(const binding_console&) = delete;

	// How an operation ended. One space for all four verbs, because `bind`'s job
	// is to turn each into a message and a status, and a per-verb enum would make
	// that four switches that drift.
	enum class outcome {
		ok,
		no_such_keymap,
		no_such_action,
		bad_notation,
	};

	// `bind -l`: every keymap's name, sorted.
	virtual void keymap_names(std::vector<std::string>& into) const = 0;

	// `bind -N name [from]`: creates, optionally as a copy. `from` empty means an
	// empty keymap.
	virtual outcome create_keymap(std::string_view name, std::string_view from) = 0;

	// `bind [-m map] keys action`: binds `keys`, written in vim notation, to the
	// named action. An empty action unbinds.
	virtual outcome bind_key(std::string_view keymap, std::string_view notation,
	                         std::string_view action) = 0;

	// `bind [-m map] keys`: what those keys run, empty when nothing.
	virtual outcome lookup_key(std::string_view keymap, std::string_view notation,
	                           std::string& action_out) const = 0;

	// `bind [-m map]`: every binding, as notation and action, in table order.
	virtual outcome list_bindings(std::string_view keymap,
	                              std::vector<std::pair<std::string, std::string>>& into) const = 0;

protected:
	binding_console() = default;
};

// The console lives on `shell_state` (#134): `state.set_binding_console(&c)`
// installs it for that shell and `state.console()` is what `bind` asks. It moved
// off file scope the moment there was a loop to own it, exactly as the note that
// stood here said it would - so two shells in one process (a test's, and the
// one under it) can no longer be handed each other's keymaps.

// ---------------------------------------------------------------------------
// The prompt, and the same link boundary again (#157, spec §6.10).
// ---------------------------------------------------------------------------
//
// THE SAME PROBLEM AND THEREFORE THE SAME SHAPE - §6.10 says so in as many
// words: "`prompt_console` beside `binding_console`". The prompt registry, the
// composer and the tick wheel live in leshper (`src/leshper/prompt/prompt.h`), and
// `lesh_runtime` does not link `lesh_leshper`; a builtin in this file cannot
// call `engine::add_module` any more than it can call a keymap function. So the
// runtime declares what a configuration builtin needs to SAY, and the ui layer
// - `src/ui/session.cpp`, in the one target that links both halves - hands it a
// leshper-backed implementation for the life of a session.
//
// `builtin_prompt` IS THE CALLER, and it arrived after the seam rather than with
// it. §6.10 scoped v1 to "the registry, the composer, the tick wheel, a
// `constexpr` default prompt table, and configuration through the ABI" and left
// the `bind`-shaped builtin and the `{module:style:type}` template language as
// the recorded follow-up, because the builtin's operand grammar IS the template
// language's grammar and inventing half of it early would have been inventing
// the half that has to be kept. What v1 needed from this class was that the seam
// exist and be installed the day the session starts - and it paid off exactly
// there: the builtin found a console rather than a reason to re-open the
// argument, and adding `set` and `text` below was the whole of the change.
//
// The C ABI verbs (`lesh_prompt_set_placements` and friends) are the other
// caller, and the language-neutral one NG-4 says the Lua binding reuses unchanged. They
// reach the engine through the registry, not through here: this is the in-tree
// C++ door, and the two are deliberately not layered on one another.
class prompt_console {
public:
	virtual ~prompt_console();

	prompt_console(const prompt_console&) = delete;
	prompt_console& operator=(const prompt_console&) = delete;

	// Which prompt is being configured. §6.10's two v1 surfaces; the right and
	// transient prompts are #156's and arrive as new enumerators. It mirrors
	// leshper's own `surface_id` and is deliberately a SECOND enum rather than a
	// shared one - a shared one would be a header of leshper's included here,
	// which is the link the whole arrangement exists to avoid.
	enum class surface : std::uint8_t {
		left,
		continuation,
	};

	// How an operation ended, one space for every verb - `binding_console`'s
	// reasoning, and it applies unchanged: the caller's job is to turn each into
	// a message and a status, and a per-verb enum would make that several
	// switches that drift.
	//
	// TWO ROWS ARE GONE, and it is worth saying which and why. `no_such_module`
	// and `unbalanced_group` belonged to four ASSEMBLY verbs - `add_module`,
	// `add_literal`, `open_group`, `close_group`, one call per element - that
	// this console carried before the template language existed. The owner's
	// #157 ruling made the whole-surface forms the only configuration doors
	// (`set` here, `lesh_prompt_set_placements` on the ABI), both atomic and both
	// resolving a module through the identical rule, so the per-element verbs
	// had no caller left and were a second, weaker spelling of the first. They
	// were also the one place a refusal lost its reason: "unknown module" and
	// "the module refused its argument" collapsed into `no_such_module`, where
	// `set`'s sentence names the byte.
	enum class outcome {
		ok,
		bad_template,
	};

	// Every registered module's name, sorted. The listing verb, `bind -l`'s
	// opposite number.
	virtual void module_names(std::vector<std::string>& into) const = 0;

	// Empties a surface. A prompt configured from nothing starts here.
	virtual outcome clear(surface which) = 0;

	// Puts the shipped default table back, which is also how a user undoes a
	// configuration without restarting the shell.
	virtual outcome use_default(surface which) = 0;

	// THE PAIR THE `prompt` BUILTIN IS WRITTEN AGAINST (#157). A builtin holding a
	// single operand has a STRING, and the string is the template language;
	// somebody has to turn it into elements, and that somebody is on the far side
	// of this line, because only that side has the registry to resolve a module
	// name against.
	//
	// `set` PARSES ONCE, AT SET TIME, AND SWAPS ATOMICALLY. A template that will
	// not parse leaves the previous prompt standing - there is no half-applied
	// state to see, and no shell left with a prompt it cannot draw, which is the
	// failure mode a per-element interface makes reachable by construction. That
	// promise is the console side's to keep; the builtin only reports what it is
	// told.
	//
	// `error_out` IS A HUMAN SENTENCE, printed verbatim after "prompt: ". The
	// wording belongs to the console because the PARSER does: the template
	// language lives across the link boundary, in leshper, and only that side can
	// say which byte of `{git:*:branch` was the mistake. A runtime-side wording
	// would be a second, worse description of a failure it cannot see.
	virtual outcome set(surface which, std::string_view template_text,
	                    std::string& error_out) = 0;

	// The SOURCE STRING the surface was last set from - the template text itself,
	// not a rendering of it and not a walk of the elements it produced.
	//
	// This is what makes bare `prompt` printable at all. The alternative was an
	// introspection door - list the elements, ask each one its kind and argument,
	// spell the template back out - and §6.10 recorded that door closed: it would
	// put the element vocabulary on this side of the boundary, where every new
	// module kind becomes a new enumerator in a runtime header. Remembering the
	// bytes that were handed in costs one `std::string` per surface and keeps the
	// vocabulary where it belongs. The shipped default answers its own canonical
	// template text - `{path}> ` - because that string is what the default IS
	// (#157: it is compiled from that spelling), so bare `prompt` on a fresh shell
	// prints the prompt on screen rather than an empty line. Empty is reserved for
	// a surface assembled placement by placement, which has no template string at
	// all; that prints as an empty line rather than as an error.
	virtual void text(surface which, std::string& out) const = 0;

protected:
	prompt_console() = default;
};

// Installed the same way and in the same place: `state.set_prompt_console(&c)`,
// read back through `state.prompts()`. Null in every non-interactive shell, for
// the same reason `console()` is - a `lesh -c` has no line editor and therefore
// no prompt engine, and the honest answer to configuring one is that there is
// none.

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
//
// A ZERO-ROW SPEC DOES NOT REPLACE THIS, which #148 phase 2 assumed it would and
// measurement refuted. A spec with no rows gives `--` and the lone `-` for free,
// but it also REFUSES every other option word - `-1` is a well-formed option
// group whose letter no row matches - and that is precisely what these utilities
// must not do. POSIX XCU 1.4 gives a utility with no options exactly one rule
// about a leading hyphen, and it is this one; everything else is an operand.
// Measured at 634e4c8, with what a zero-row spec would have answered:
//
//     exit -1        lesh 255   bash 255   zsh 255   (dash refuses)   -> would be
//                    `exit: Illegal option -1`, breaking three shells' agreement
//     return -1      lesh -1    bash 255   zsh -1    (dash refuses)   -> the same
//     eval -x echo   lesh/dash/zsh run it and report `-x: not found`  -> the same
//
// So `exit`, `return`, `break`, `continue`, `shift`, `eval`, `.`, `wait` and
// `alias` keep this reading, and it is the RIGHT one for them rather than a
// leftover. zsh spells the same exemption as BINF_SKIPINVALID on its table row;
// growing the parser a flag for it would be bending the grammar to fit nine
// utilities that POSIX says have no grammar to fit.
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
