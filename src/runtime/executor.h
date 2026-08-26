#pragma once

#include "runtime/builtins.h"
#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "syntax/ast.h"

#include <string>
#include <vector>

#include <sys/types.h>

namespace lesh::runtime {

// How much of its own input the shell is allowed to keep. See issue #67.
//
// POSIX 1.4, "Input Files": when a shell's input is a SEEKABLE file, the shell
// shall not consume more of it than the command it is about to run needs. That
// is not a quality-of-implementation note - four assertions turn on it. A
// command reading fd 0 must get the bytes that FOLLOW it, so `read a` takes the
// next line and `cat | tail -n 1` is fed the lines under it rather than watching
// the shell swallow them and then try to EXECUTE them.
//
// THE SHELL STILL BUFFERS THE WHOLE FILE, and that is deliberate. Spans are
// offsets into one contiguous source, and #21 has the parser collect a
// here-document body as a RANGE of that source precisely so the lexer never
// reads and never seeks; handing the parser a line at a time would break both.
// What is restored instead is the only thing any other reader of the descriptor
// can observe - the FILE OFFSET, put back to the end of the command about to run
// and picked up again afterwards, because the command may have read some itself.
// Granularity of CONSUMPTION therefore lives in the read loop, and the front end
// is untouched.
//
// A NON-SEEKABLE input - a pipe, a terminal, a socket - leaves this inert, and
// the shell reads ahead exactly as it did before. That is not a gap: POSIX
// permits it, for the reason that a pipe cannot be un-read. The path is explicit
// rather than accidental, so `printf 'read x\ndata\n' | lesh` and
// `lesh < script` are two documented behaviours rather than one that happens to
// depend on what fstat said.
//
// dash and zsh do not implement any of this; bash and yash do. ADR-0001 makes
// dash authoritative for the POSIX floor and this is the case it cannot settle,
// so the argument is POSIX plus the two shells that follow it.
class script_input {
public:
	// Binds to `fd` if the shell may hand input back on it. Must be constructed
	// BEFORE a byte is read, because the offset it records is where the script
	// begins - the shell reads to EOF immediately afterwards.
	explicit script_input(int fd) noexcept;

	// Puts back everything from `consumed` on, so the command about to run reads
	// the bytes after itself. Nothing is written and nothing is re-read: the
	// bytes are already in memory and only the offset moves.
	void hand_back(size_t consumed) noexcept;

	// Where the command that just ran left the descriptor, as an offset into the
	// source. `fallback` - the position the shell parsed up to - is returned
	// whenever the descriptor cannot answer, which is what keeps a non-seekable
	// input on the old path.
	[[nodiscard]] size_t resume_at(size_t fallback, size_t limit) noexcept;

private:
	// -1 once the input is known not to be seekable, or once it has stopped being
	// the file the script came from. One field for both, because the shell's
	// answer to either is the same: stop tracking and read on from memory.
	int _fd = -1;
	off_t _origin = 0;
	// Which file the script arrived on. A script may run `exec < other`, and an
	// offset read back from THAT file measured against this origin would send the
	// shell to an arbitrary point in its own source. Checked rather than assumed:
	// the failure would be silent and would look like a parser bug.
	dev_t _device = 0;
	ino_t _inode = 0;

	// True while fd still refers to the file the script was read from. Latches
	// the descriptor closed when it does not, so the question is asked once.
	[[nodiscard]] bool still_the_script() noexcept;
};

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
	//
	// `input`, when given, is the descriptor the source was read from, and this
	// loop is where the shell hands back what it has not run (#67). Null for
	// `-c`, which has no input file, and for a script named as an operand, whose
	// descriptor nothing else shares.
	[[nodiscard]] int run_input(std::string_view source, bool echo_when_verbose = true,
	                            script_input* input = nullptr);

	// The port the expander takes (#11). The executor supplies it, rather than the
	// expander depending on the executor - which is what breaks the cycle every
	// shell surveyed in #14 has.
	[[nodiscard]] command_runner& as_command_runner() noexcept { return _runner; }

	// --- The interactive caller (#134) ---------------------------------------
	//
	// An INTERACTIVE shell reads many inputs through ONE executor - one per line
	// the user accepts - where `-c` and a script read exactly one. The three
	// members below are the whole of what that difference costs; everything else
	// about running a command is identical, which is the point.

	// Whether `exit` has been asked for. The interactive session ends when it
	// answers true; a script's `run_input` has already stopped by then.
	[[nodiscard]] bool exit_requested() const noexcept { return _exit_requested; }

	// The EXIT trap belongs to the SESSION, not to any one line.
	//
	// `run_input` and `run` both run it on the way out of the input they were
	// given, which is right for a script and wrong for a prompt: it would fire
	// after the first line and, being once-only, never again. The interactive
	// caller defers it and runs it itself when the session ends. Off by default,
	// so nothing about `-c` or a script changes.
	void defer_exit_trap(bool v) noexcept { _defer_exit_trap = v; }

	// Runs the deferred EXIT trap, once. Answers the status the shell should
	// leave with when the trap BODY exited, and `status` unchanged otherwise.
	[[nodiscard]] int finish(int status);

	// Ctrl-C at the prompt (#98 decisions 2 and 3, the zsh way).
	//
	// `$?` becomes 130 and any INT trap fires - at a command boundary, which is
	// where a trap body belongs (#33) and is exactly what the prompt is. The
	// unwind an UNCAUGHT interrupt raises is consumed here rather than left
	// standing: `run_input`'s loop normally consumes it after the command it
	// abandoned, and there is no command here to abandon, only a prompt to
	// return to.
	void interrupt_at_prompt();

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
		// Whether this child is the FOREGROUND job, and so takes the terminal on
		// its way to exec (#158 decision 1). Default false, because the default
		// must be the one that cannot steal the tty from the editor: a background
		// job, a command substitution and a helper child all leave it alone.
		bool foreground = false;
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
	//
	// `kind` is the ONE thing the shell's own input and an `eval` operand disagree
	// about, and it is a parameter rather than a second loop because a second loop
	// is what #74 was opened to remove: run_source carried a near-copy of this
	// function, and a guard added here - `run_pending_traps` - was silently absent
	// there for as long as the path existed.
	//
	// The syntax-error exit and the empty-tree answer are this function's; the
	// per-command work below is run_command_list's, which every compound body
	// shares (#77).
	int run_parsed(const syntax::tree& t, source_kind kind = source_kind::shell_input);
	// THE COMMAND LOOP, and after #77 the only one in the shell. Runs the children
	// of `list` one at a time, applying every guard exactly once for every reader:
	// `set -n`, pending traps, the exit and interrupt unwinds, and `set -e`.
	//
	// Three parameters carry everything its readers disagree about. `kind` decides
	// whether an escaping `break`, `continue` or `return` is consumed here - the
	// shell's own input has nothing left to hand it to - or travels outward to the
	// construct that owns it, which is what an `eval` operand and every compound
	// BODY need. `status` is what an empty list answers with: `$?` for the shell's
	// own input, and zero for a body, since POSIX gives an empty `case` item that
	// answer regardless of what ran before it.
	//
	// Three parameters rather than three loops, because three loops is what this
	// shell had: each of the two copies was missing a guard the original had, and
	// in both cases the drift was wider than the symptom anyone had reported.
	int run_command_list(const syntax::tree& t, syntax::node_index list,
	                     source_kind kind, int status);
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
	// Parses and runs source in this environment. Used by `eval`, `.` and a trap
	// body.
	//
	// `echo_as_read` is `set -v`, and only `.` asks for it: POSIX makes the option
	// write INPUT to standard error as it is read, and dash echoes a dot script's
	// text (per command, as this reads it) while echoing neither an `eval` operand
	// nor a trap body - both of which it has already echoed once as part of the
	// line that carried them.
	int run_source(std::string_view source, bool echo_as_read = false,
	               std::string_view file = {});
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
	int run_negation(const syntax::tree& t, syntax::node_index n);

	// Builds an expander wired to this executor AND to this shell's options.
	//
	// One place, so an option cannot be honoured by three of the four expansion
	// sites and forgotten by the fourth. `set -u` was recorded and inert for
	// exactly that kind of reason: nothing ever read the flag.
	// GLOBBING IS A PARAMETER, `set -u` IS NOT. The two are independent properties,
	// and conflating them is what #39 cost: a case pattern must not be pathname-
	// expanded, so run_case built its own expander to turn globbing off - and in
	// doing so silently dropped error_on_unset, which is the only argument it was
	// not passing. `sh -u -c 'case $nope in *) echo x;; esac'` could not even
	// REPORT the unset parameter, let alone stop for it. A caller may choose
	// whether a pattern globs; no caller chooses whether `set -u` applies.
	[[nodiscard]] expander make_expander(bool glob_enabled) noexcept {
		return expander{_pool, _state, &_runner, glob_enabled, &_state,
		                &_state, _state.opts().error_on_unset};
	}
	[[nodiscard]] expander make_expander() noexcept {
		return make_expander(!_state.opts().no_glob);
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
	// Raises the loop depth for as long as one loop is running, condition
	// included: `while break; do ...; done` leaves the loop in dash, so the
	// condition counts as inside it. RAII for the same reason errexit_suppression
	// is - run_loop returns from six places, and a depth left raised would make
	// every later `break` at the top level unwind instead of doing nothing.
	class loop_scope {
	public:
		explicit loop_scope(tree_walking_executor& owner) noexcept
			: _owner(owner) { ++_owner._loop_depth; }
		~loop_scope() { --_owner._loop_depth; }
		loop_scope(const loop_scope&) = delete;
		loop_scope& operator=(const loop_scope&) = delete;
	private:
		tree_walking_executor& _owner;
	};
	void run_pending_traps();
	// Runs the EXIT trap. True when the BODY itself called `exit`, in which case
	// `status` is the status the shell must exit with - `trap 'exit 7' EXIT` wins
	// over the `exit 1` that reached the trap, and a body that merely ran commands
	// does not change the status at all.
	bool run_exit_trap(int& status);
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

	// THE CHILD'S OWN HANDOFF, and the one place in the runtime that is allowed to
	// touch the terminal from a process that is not the shell (#158 decisions 1
	// and 4, #159). Runs IN THE CHILD, after its `setpgid` and before its exec.
	//
	// CHILD-SIDE, not loop-side, and that is fish's pattern and zsh's: the child
	// races nothing, because it touches the tty only after owning it. `Src/exec.c`
	// (`entersubsh`) says why the shell cannot do it any earlier - *"This only
	// works if we are still ignoring SIGTTOU at this point; in this case ignoring
	// the signal has the special effect that the operation is allowed to work"*.
	//
	// SO THE ORDER IS THE WHOLE CONTRACT. `tcsetpgrp` from a background process
	// group is legal only while the SIGTTOU-ignore inherited from
	// `ignore_background_write_signals` still stands; reset the signals first and
	// the child stops itself trying to take the terminal it was given. Handoff,
	// then reset.
	//
	// THE RESET IS UNCONDITIONAL AND IS NOT `drop_interactive_defaults`. That
	// function's #37 rule - an inherited SIG_IGN stands - is exactly wrong for
	// these three: SIGTTOU, SIGTTIN and SIGTSTP are ignored because THIS shell's
	// loop ignored them for its own editing, not because the invoker handed them
	// over, and SIG_IGN is the one disposition that survives execve. Left alone,
	// every command lesh runs is a command that cannot be suspended and whose
	// background tty access silently succeeds - which is the #158 defect: nvim's
	// background `tcsetattr` worked, its read returned EIO instead of stopping,
	// and the terminal's unread query replies came back as a command line.
	//
	// `tty_fd < 0` DOES BOTH NOTHINGS. No terminal to hand over means no
	// self-inflicted ignores to undo either - a non-interactive shell's
	// dispositions are its invoker's, and #37 says they stand.
	//
	// Static because it runs after a fork and must reach nothing but its argument:
	// only `tcsetpgrp` and `getpgrp`, both async-signal-safe.
	//
	// THE HANDOFF ONLY, since #160. It used to also reset SIGTTOU/SIGTTIN/SIGTSTP,
	// and the two halves have different arities: the handoff is ONE PER PROCESS
	// GROUP - a pipeline's leader makes it on behalf of every stage - while the
	// reset is ONE PER PROCESS THAT EXECS. Fused, the leader-only call left stages
	// 2..N exec'ing with all three signals still ignored, so `less` at the end of a
	// pipeline could not be suspended. `reset_job_control_signals` is the other
	// half and `become_command` is where it runs.
	//
	// Splitting them is also what lets a foreground `( )` call this: a subshell
	// wants the terminal but must NOT drop its own SIGTTOU ignore, because it will
	// hand the terminal to commands of its own and take it back afterwards.
	static void take_terminal_in_child(int tty_fd) noexcept;

	// The other half of the old fused call (#158 decision 4): the dispositions this
	// shell's editing loop imposed on itself, dropped before execve carries them
	// into the command. SIG_IGN is the one disposition that survives exec.
	//
	// PER PROCESS THAT EXECS, which is why it lives on the exec path in
	// `become_command` and not beside the handoff at a fork. Every process that
	// becomes a command needs it - a simple command's child and each pipeline stage
	// that execs in place - and every process that goes on running SHELL code must
	// not have it, because it will hand the terminal over and take it back and needs
	// the SIGTTOU ignore to do so. A compound stage and a foreground `( )` are both
	// the second kind, and both are excluded by construction rather than by a test.
	//
	// Same `tty_fd < 0` gate as the handoff, and for the reason #37 gives: with no
	// terminal there were no self-inflicted ignores to undo, and a non-interactive
	// shell's children keep the dispositions their invoker chose.
	static void reset_job_control_signals(int tty_fd) noexcept;

	// The other half, on the shell thread: takes the terminal back the instant a
	// foreground job's wait returns (#158 decision 2).
	//
	// PER JOB, not per line, and that is the point. The loop reclaims in
	// `resume_after_execution` when the whole line is done, which is the backstop;
	// without this one `nvim .; read x` breaks, because between nvim exiting and
	// the end of the line the shell is not the foreground group and the `read`
	// builtin's stdin read gets EIO rather than the user's typing.
	//
	// Unconditional when there is a terminal, fish #9181's reasoning: a command
	// may have taken the tty whether or not we gave it, so do not ask.
	void reclaim_terminal() noexcept;

	// The third thing a foreground wait owes, and it exists only because of the
	// first two: the SIGINT the shell was excluded from by its own handoff.
	//
	// WHY THERE IS ANYTHING TO DO. Before the handoff the shell WAS the terminal's
	// foreground group, so a keyboard interrupt was delivered to it as well and
	// the ordinary handler-plus-`run_pending_traps` path did whatever the
	// disposition said. Handing the terminal to the child excludes the shell from
	// that group, so the signal now reaches the child alone - #98 decision 2's
	// *"the shell never sees it"* arriving literally - and BOTH of that path's
	// answers silently stop happening.
	//
	// BOTH, and the second one is the one that is easy to miss. A user's INT trap
	// stops firing: dash fires it, zsh fires it, bash does not, and ADR-0001 plus
	// #98 decision 3 - the owner's override adopting zsh's INT-trap visibility and
	// declining bash's silence by name - compose to "fires". And with NO trap set,
	// the rest of the line stops being abandoned: `sleep 5; echo after` prints
	// `after`, which dash, zsh AND bash all refuse to do, and which lesh itself
	// refused to do before the handoff existed. Unanimous, including bash this
	// time, and matching lesh's own prior behaviour - so a synthesis that covered
	// only the trap was under-correcting rather than being careful.
	//
	// SO IT DECIDES ALMOST NOTHING. It records a pending SIGINT and lets the
	// between-commands machinery answer exactly as it answers a real arrival: the
	// trap body, or #52's `interrupts_command` unwind. That is #33's discipline,
	// and a second route would be a second timing to get wrong.
	//
	// THE ONE THING IT DECIDES is what to do in a shell where that machinery will
	// answer NOTHING, which #160 made reachable by giving a foreground `( )` the
	// terminal. A subshell has no interactive default and, absent a trap, no
	// disposition that acts on the flag - so it takes SIGINT's real default action
	// and dies of it, which is both what the kernel would have done and the only
	// thing the waiting parent can tell apart from `exit 130`. A shell that reads
	// commands never reaches that branch: it always has a trap or #52's default.
	//
	// GATED ON THE HANDOFF HAVING HAPPENED - the same `tty_fd` test
	// reclaim_terminal makes, at the same point, and that gate is load bearing
	// rather than tidy. Where the terminal never moved the kernel still delivers
	// to the shell and the normal machinery already answered, so synthesizing
	// there would double-fire interactively and fire at all non-interactively,
	// where dash does not (`trap 'echo x' INT; sh -c 'kill -INT $$'` prints
	// nothing and reports 130 - lesh matches it byte for byte). That is the whole
	// reason the conformance corpus cannot see any of this.
	void note_interrupt_after_handoff(int wait_status);

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

	// Parses and runs `code` with stdout captured. Used by command substitution.
	//
	// Three answers rather than two: see substitution_result, and #57 for what the
	// bool could not say.
	[[nodiscard]] substitution_result capture(std::string_view code,
	                                          arena_array<char>& out);

	// Adapts this executor to the expander's port. A member rather than a global,
	// so nesting works: `$(a $(b) c)` needs the inner capture to reach the same
	// executor as the outer one.
	class runner_adapter final : public command_runner {
	public:
		explicit runner_adapter(tree_walking_executor& owner) noexcept : _owner(owner) {}
		substitution_result run_and_capture(std::string_view code,
		                                    arena_array<char>& out) override {
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
	// See defer_exit_trap. False everywhere but an interactive session.
	bool _defer_exit_trap = false;
	// Set by a `return` that reached the top level - outside any function and any
	// dot script. POSIX leaves that unspecified; dash and zsh both END THE INPUT,
	// so `return; echo x` prints nothing. Only run_input reads it, and it is
	// deliberately NOT `_exit_requested`: that one is `exit`, and folding the two
	// together would make every construct that stops early for an exit stop for a
	// `return` as well.
	bool _input_ended = false;
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
	// Set by break, continue and return; consumed by the enclosing loop or
	// function. _flow_level implements `break 2`.
	control_flow _flow = control_flow::normal;
	int _flow_level = 0;
	// How many loops THIS PROCESS is currently inside. `break` and `continue` are
	// defined only inside a loop, and POSIX leaves them unspecified outside one;
	// dash makes them a silent no-op and carries on, which is what lesh follows
	// (ADR-0001: dash is authoritative for the POSIX floor).
	//
	// A counter and not a bool, because it also answers "is this the OUTERMOST
	// loop", which is where `break 2` with one loop around it has to stop rather
	// than leaving an unwind nothing will ever consume.
	//
	// Per PROCESS: a subshell inherits the count, so a `break` inside `(...)` in a
	// loop unwinds out of the subshell rather than doing nothing, and the loop the
	// parent is running is untouched because the parent never sees the flow.
	//
	// A FUNCTION CALL resets it - see try_run_function - and `.` and `eval` do
	// not, which is dash's answer for all three.
	int _loop_depth = 0;

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
