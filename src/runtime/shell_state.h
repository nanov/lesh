#pragma once

#include "runtime/expander.h"
#include "runtime/signals.h"
#include "syntax/parser.h"
#include "syntax/source_map.h"
#include "substrate/traits.h"

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lesh::runtime {

// Shell state: variables, scopes, options, and $?. See issue #12.
//
// OWNERSHIP IS EXPLICIT, per ADR-0007. Every allocation has an owner that
// releases it before main returns, so the leak gate's expected result is exactly
// zero with no suppression file. Legacy got this wrong in a way worth recording:
// SimpleParsingState strdup()s its input and never frees it, alias ASTs point
// into that buffer, and freeing it produces a use-after-free. That is what
// implicit ownership costs. Here, values are owned by the map that holds them.

// POSIX distinguishes special builtins from regular ones, and the distinction is
// not cosmetic: a failure in a special builtin exits a non-interactive shell,
// and its variable assignments persist after the command completes.
enum class builtin_kind {
	none,
	special,   // break : continue . eval exec exit export readonly return set shift times trap unset
	regular,   // cd echo false pwd true and the rest
};

// Defined in builtins.cpp, from the ONE registry that also carries the handlers.
// It used to be defined here, over two name lists of its own, and the two tables
// disagreed: `test` and `readonly` were classified with no handler anywhere, so
// the command search never reached PATH and `test 1 = 2` silently succeeded (#35).
[[nodiscard]] builtin_kind classify_builtin(std::string_view name) noexcept;

// Declared in runtime/builtins.h; only POINTED AT here, so shell_state can hold
// the `bind` seam (#134) without this header depending on the builtin table.
class binding_console;
// The prompt's, on the same terms (#157). Two consoles, one rule.
class prompt_console;

// What a forked child is to the shell that forked it, and the ONE thing that
// answer decides: whether the child keeps the saved terminal fd (#158 decision 3,
// scoped by #160). See shell_state::enter_subshell, which is the only reader.
//
// Two values rather than a bool because the question at each fork site is "what
// is this child", not "should the fd be cleared" - the sites know the first and
// would have to re-derive the second. `detached` is the DEFAULT: every fork that
// does not claim to be a foreground job gets no terminal.
enum class subshell_role {
	// `&`, `$( )`, and every helper fork. No terminal: an asynchronous job or a
	// command substitution taking the terminal from the line editor would be its
	// own bug, which is #158 decision 3's whole point.
	detached,
	// A `( )` the shell is WAITING ON, or a stage of a foreground pipeline. Keeps
	// the fd, which two things then read: the handoff at the inner fork, so
	// `(nvim .)` becomes the terminal's foreground group, and the per-process
	// signal reset on the exec path, so every stage of `ls | less` execs with
	// SIGTSTP at its default rather than only the one that made the handoff.
	foreground_job,
};

class shell_state final : public parameter_source,
                          public arithmetic_variables,
                          public parameter_assigner,
                          public syntax::alias_source {
public:
	shell_state();
	~shell_state() override;

	shell_state(const shell_state&) = delete;
	shell_state& operator=(const shell_state&) = delete;

	// --- parameter_source -----------------------------------------------------

	[[nodiscard]] bool lookup(std::string_view name, std::string_view& value) const override;
	[[nodiscard]] std::string_view home_directory() const override;
	[[nodiscard]] bool home_directory_of(std::string_view user,
	                                     std::string_view& out) const override;
	[[nodiscard]] std::string_view ifs() const override;
	[[nodiscard]] int last_status_value() const override { return _last_status; }
	[[nodiscard]] int process_id_value() const override { return _pid; }
	[[nodiscard]] size_t positional_count() const override { return _positional.size(); }
	[[nodiscard]] bool positional_at(size_t index, std::string_view& out) const override;
	[[nodiscard]] std::string_view script_name_value() const override { return _script_name; }
	[[nodiscard]] std::string_view option_flags() const override;

	// --- arithmetic_variables -------------------------------------------------

	[[nodiscard]] int64_t get(std::string_view name) const override;
	bool set(std::string_view name, int64_t value) override;
	[[nodiscard]] bool defined(std::string_view name) const override {
		std::string_view ignored;
		return lookup(name, ignored);
	}

	// --- variables ------------------------------------------------------------

	// Copies the value in. The state owns it from here, which is what makes the
	// zero-leak gate reachable - the alternative is what legacy does.
	//
	// Returns FALSE when the name is readonly, having written nothing. The result
	// is [[nodiscard]] because a refused assignment that the caller ignores is the
	// bug #35 is about: POSIX makes a variable assignment error fatal to a
	// non-interactive shell, so `readonly a=1; a=2; echo not reached` must print
	// nothing, and every writer has to decide what its own failure means. The
	// DIAGNOSTIC is the caller's too, because dash prefixes it with the builtin
	// that refused: `export: a: is read only` but a bare `a: is read only`.
	[[nodiscard]] bool set(std::string_view name, std::string_view value);
	[[nodiscard]] bool set_exported(std::string_view name, std::string_view value);
	bool assign_parameter(std::string_view name, std::string_view value) override;
	[[nodiscard]] bool unset(std::string_view name);
	[[nodiscard]] bool is_exported(std::string_view name) const;

	// --- the working directory, logical and physical --------------------------
	//
	// POSIX gives `cd`, `pwd` and the shell's own initialization ONE rule, and it
	// lives here rather than in the two builtins because a shell whose `cd` and
	// `pwd` disagree about which directory it is in is worse than one that is wrong
	// in both. #46 gave `cd` its -L/-P pair; #51 gave `pwd` the same pair and made
	// the constructor decide the starting value, and all three ask this question.

	// Whether `path` may be believed as a LOGICAL pathname of the current working
	// directory: absolute, no component that is dot or dot-dot, and naming the
	// directory the process is actually in.
	//
	// The last test is DEVICE AND INODE, not text. Comparing against getcwd's answer
	// would reject `/tmp` on a system where /tmp is a symlink to /private/tmp - and
	// that value is precisely what the logical working directory is for, so a string
	// comparison here would undo #46 at startup and again on every `pwd`.
	[[nodiscard]] static bool names_current_directory(std::string_view path);

	// getcwd, as a string. Empty when the kernel cannot say - a directory that has
	// been removed under the shell, which is a state `pwd -P` and `cd -e` both have
	// to report rather than paper over.
	[[nodiscard]] static std::string physical_working_directory();

	// The LOGICAL working directory: $PWD when it still names the current directory,
	// and the physical one otherwise. `pwd` prints it and `cd` extends a relative
	// operand onto it, which is why a stale $PWD - what `readonly PWD; cd sub` leaves
	// behind - must not be believed by either.
	[[nodiscard]] std::string logical_working_directory() const;

	// --- marking a name: export and readonly ----------------------------------
	//
	// ONE RULE, because POSIX asks both builtins the same question and answers it
	// the same way: an attribute belongs to the NAME, so marking a name neither
	// requires nor creates a value. `export x` marks it "whether or not it is set";
	// a readonly variable cannot be assigned to or unset, set or not.
	//
	// Both flags therefore live ON THE VARIABLE, beside an `assigned` bit, rather
	// than in a set of names off to the side. An entry that is marked but not
	// assigned is ABSENT to every reader - `${x-unset}` says unset, `set -u` errors,
	// the environment block skips it - and the mark is still there to catch the
	// value when an assignment finally arrives.
	//
	// They were split once and drifted (#71): `readonly x` recorded the flag while
	// `export x` fabricated an assignment of the empty string, so `${x-unset}` and
	// `${x:-empty}` stopped being distinguishable after a bare `export x`, a child
	// saw `x=` where dash exports nothing, and `export -p` listed a variable that
	// did not exist - a listing POSIX requires to be re-inputtable, which is the
	// defect #40 and #38 both hit. Sharing the mechanism is what stops that
	// recurring; #24 chose it for readonly and export now uses it too.
	//
	// Marking is NOT an assignment, which is also why neither routes through set():
	// `export x` on a readonly x is a command dash allows and POSIX permits, and
	// set() would refuse it.

	void mark_exported(std::string_view name);
	void mark_readonly(std::string_view name);
	[[nodiscard]] bool is_readonly(std::string_view name) const;

	// The one wording for a refused write. `who` is the builtin that refused, or
	// empty for a plain assignment; dash prints `export: a: is read only` for the
	// first and `a: is read only` for the second, and one function means the six
	// call sites cannot word it six ways.
	static void report_readonly(std::string_view who, std::string_view name);

	// Every variable, sorted by name, for the listing forms of `export` and
	// `readonly`. Sorted because the map is unordered and POSIX leaves the order
	// unspecified: a stable order is what makes `export -p` output diffable.
	struct variable_row {
		std::string_view name;
		std::string_view value;
		bool assigned = false;   // false for a name marked but never assigned
		bool exported = false;
		bool readonly = false;
	};
	[[nodiscard]] std::vector<variable_row> variables() const;

	// Builds the envp array for a child process. The returned pointers are owned
	// by this object and stay valid until the next call.
	[[nodiscard]] char** environment_block();

	// --- special and positional parameters ------------------------------------

	[[nodiscard]] int last_status() const noexcept { return _last_status; }
	void set_last_status(int status) noexcept { _last_status = status; }

	// The `$?` a TRAP ACTION was entered with, held for as long as one is running.
	//
	// POSIX XCU `exit`: "when exit is executed in a trap action, the last command
	// is considered to be the command that executed immediately preceding the trap
	// action". So `exit` with no operand inside a trap reports the status the trap
	// INTERRUPTED and not the status of whatever the body itself last ran, which is
	// why this cannot be `last_status()`: the body's own commands do update `$?`,
	// and `trap '(exit 2); echo $?' EXIT` prints 2.
	//
	// An optional, not an int, because zero is a status a trap can genuinely be
	// entered with - the value has to be able to say "no trap action is running"
	// rather than encoding that as a number a real trap could carry. Held here
	// rather than in the executor because the builtins that read it - `exit` and
	// `return` - are handed nothing but this object.
	void set_trap_entry_status(std::optional<int> status) noexcept {
		_trap_entry_status = status;
	}
	[[nodiscard]] std::optional<int> trap_entry_status() const noexcept {
		return _trap_entry_status;
	}

	// $0 is the shell or script name; $1.. are the positional parameters. Held
	// separately from variables because they are not variables: they have no
	// names, `shift` renumbers them, and a function replaces them for the
	// duration of a call (#25).
	void set_positional(std::vector<std::string> args) { _positional = std::move(args); }
	[[nodiscard]] const std::vector<std::string>& positional() const noexcept {
		return _positional;
	}
	void set_script_name(std::string name) { _script_name = std::move(name); }
	[[nodiscard]] std::string_view script_name() const noexcept { return _script_name; }

	// --- where the shell is (#76) ----------------------------------------------
	//
	// WHICH COMMAND IS RUNNING, as a tree and a byte offset into it. `$LINENO`
	// reads it, and so does every runtime diagnostic.
	//
	// AN OFFSET, NOT A LINE NUMBER, and that is the whole reason this is cheap
	// enough to maintain per command: recording the offset is two stores, where
	// recording the line would be a scan for a number almost no script ever reads.
	// The scan happens when the number is ASKED FOR, which is when a script writes
	// `$LINENO` or when the shell has already failed.
	//
	// The tree travels with the offset because a tree carries the SOURCE it was
	// parsed from, and the shell runs trees from more than one: a function body
	// belongs to the tree its definition was parsed from, and it may be called
	// from inside an `eval` whose own source is a different string entirely.
	// Keeping only an offset would measure a script offset against an eval string.
	void set_command_origin(const syntax::tree& t, uint32_t offset) noexcept {
		// The mapper is re-seated only when the SOURCE changes, so its memo survives
		// a whole script. Comparing the data pointer rather than the text: two
		// sources may hold equal bytes and still be different buffers.
		if (_lines.source().data() != t.source().data())
			_lines = syntax::source_map{t.source()};
		_origin = &t;
		_origin_offset = offset;
	}

	// Save and restore around a nested source - an `eval`, a dot script, a trap
	// body. Returning the old value rather than keeping a stack: the nesting IS the
	// C++ call stack, and a second stack would be a second thing to get wrong.
	struct command_origin {
		const syntax::tree* tree = nullptr;
		uint32_t offset = 0;
		std::string_view file;
	};
	[[nodiscard]] command_origin command_origin_now() const noexcept {
		return {_origin, _origin_offset, _origin_file};
	}
	void restore_command_origin(const command_origin& saved) noexcept {
		if (saved.tree != nullptr)
			set_command_origin(*saved.tree, saved.offset);
		else
			_origin = nullptr;
		_origin_file = saved.file;
	}

	// WHICH FILE A DIAGNOSTIC NAMES while a DOT SCRIPT is running.
	//
	// `$0` everywhere else, per #61 - but a dot script's lines are not `$0`'s
	// lines, and naming `$0` with a line number counted in another file is the one
	// way this format can lie. bash and zsh both name the dot script; dash names
	// `$0` AND appends the script's path, which says the same thing at greater
	// length.
	//
	// A view, not a string: the pathname lives in run_dot_script's own frame, which
	// outlives the run, and the guard that restores the origin restores this with
	// it. Empty means `$0`, which is what an `eval` and a trap body want - neither
	// is a file.
	void set_origin_file(std::string_view file) noexcept { _origin_file = file; }
	[[nodiscard]] std::string_view origin_file() const noexcept {
		return _origin_file.empty() ? std::string_view{_script_name} : _origin_file;
	}

	// Where the running command sits in the text the user typed, as 1-based line
	// and column. An offset inside an ALIAS BODY has no such place, so it resolves
	// to the invocation site and the aliases crossed come back in `site`.
	//
	// Line zero means the shell is not running a command - startup, or a diagnostic
	// about the command line itself. A caller printing a position must test it.
	[[nodiscard]] syntax::source_position where(syntax::invocation_site* site = nullptr) const noexcept {
		if (_origin == nullptr)
			return {0, 0};
		const syntax::invocation_site at = _origin->invocation_of(_origin_offset);
		if (site != nullptr)
			*site = at;
		return _lines.at(at.offset);
	}

	// POSIX: `shift n` discards the first n, and is an error if n exceeds $#.
	[[nodiscard]] bool shift_positional(size_t n);

	[[nodiscard]] int process_id() const noexcept { return _pid; }

	// The path to this shell's own executable, resolved once at startup.
	//
	// POSIX requires the ENOEXEC fallback: when a file is found and readable but
	// execve rejects it - no shebang, no magic number - the shell runs it as a
	// SHELL SCRIPT rather than reporting a format error. Doing that needs a path to
	// re-exec, and argv[0] is not one when the shell was found on PATH.
	[[nodiscard]] std::string_view own_path() const noexcept { return _own_path; }

	// --- getopts ---------------------------------------------------------------
	//
	// POSIX puts getopts' argument index in the OPTIND *variable*, so that a caller
	// can restart the parse with `OPTIND=1`. The position WITHIN a word - the `b` of
	// `-ab`, parsed by the second of two calls - has no such home, and OPTIND cannot
	// express it. It lives here.
	//
	// ANY assignment to OPTIND clears it, which is what makes the documented reset
	// work. Comparing OPTIND against a shadow copy would NOT: OPTIND names the word
	// still being parsed, so it is 1 all the way through `-abc`, and `OPTIND=1`
	// mid-word writes the value that is already there. dash splits the state the
	// same way and clears the offset from its own assignment hook (var.c
	// getoptsreset), for the same reason.
	[[nodiscard]] size_t getopts_offset() const noexcept { return _getopts_offset; }

	// getopts' own write of OPTIND. The one path that does NOT clear the offset,
	// since getopts is reporting the position it just reached rather than asking to
	// start over. dash spells this `setvarsafe("OPTIND", s, VNOFUNC)`.
	//
	// False when OPTIND is readonly, which POSIX XBD 8.1 lets getopts fail over
	// rather than ignore - and dash does fail, so the offset is left alone too.
	[[nodiscard]] bool set_optind(size_t index, size_t offset);

	// --- options --------------------------------------------------------------

	// Held here rather than as globals so a subshell can copy them.
	//
	// HONOURED: -a allexport, -C noclobber, -e errexit, -f noglob, -n noexec,
	// -u nounset, -v verbose, -x xtrace, -o pipefail.
	//
	// RECORDED BUT INERT, still accepted because POSIX requires the shell to take
	// them on its own command line and in `set` - rejecting them is what kept lesh
	// from reading a single byte of the yash signal suite, which invokes the testee
	// as `sh +i +m`:
	//   -b notify, -m monitor  job control, out of scope per ADR-0001
	//   -o vi, -o ignoreeof    the line editor's business (Phase 4)
	//   -h hashondef           command-path hashing; lesh keeps no hash table
	//   -o nolog               history, which needs the line editor too
	// Each is listed by `set -o` and reported by `$-` all the same: a shell that
	// accepts an option must say what it holds, and lying about the value would be
	// worse than admitting the effect is missing.
	struct options {
		bool all_export = false;      // -a
		bool notify = false;          // -b, inert
		bool no_clobber = false;      // -C
		bool exit_on_error = false;   // -e
		bool no_glob = false;         // -f
		bool hash_all = false;        // -h, inert
		bool monitor = false;         // -m, inert
		bool no_exec = false;         // -n
		bool error_on_unset = false;  // -u
		bool verbose = false;         // -v
		bool trace = false;           // -x
		bool ignore_eof = false;      // -o ignoreeof, inert
		bool no_log = false;          // -o nolog, inert
		bool pipefail = false;        // -o pipefail
		bool vi = false;              // -o vi, inert
	};

	// One row per POSIX shell option: the letter, the `-o` spelling, and where the
	// value lives.
	//
	// ONE TABLE. `set`, command-line parsing, `set -o` listing and `$-` all read
	// this, so an option cannot be added to one and forgotten in the others - which
	// is how `sh -m` and `set -m` would come to disagree about which letters exist.
	struct option_descriptor {
		char letter;            // '\0' when POSIX gives the option no letter
		std::string_view name;  // empty when POSIX gives it no `-o` spelling
		bool options::*field;
	};
	static constexpr size_t kOptionCount = 15;
	[[nodiscard]] static const std::array<option_descriptor, kOptionCount>&
		option_table() noexcept;

	// Applies one option letter or `-o` name. Returns false when the option is not
	// one POSIX names, which is an error rather than something to shrug at.
	//
	// Shared by the `set` builtin and by command-line parsing, because POSIX gives
	// them the same option set and two tables would drift apart.
	[[nodiscard]] static bool apply_option_letter(options& o, char letter, bool enable);
	[[nodiscard]] static bool apply_option_name(options& o, std::string_view name,
	                                            bool enable);
	[[nodiscard]] options& opts() noexcept { return _options; }
	[[nodiscard]] const options& opts() const noexcept { return _options; }

	// --- aliases --------------------------------------------------------------
	//
	// The state OWNS its alias text, per ADR-0007. Legacy's aliases were the last
	// blocker to a zero-leak gate: it strdup()s the text, points parse trees into
	// it, and can therefore never free it - the "leak" LeakSanitizer reports there
	// cannot be fixed without a use-after-free. Storing std::string here means the
	// text outlives every use and is released in the destructor.

	void set_alias(std::string_view name, std::string_view value);
	// False when there was no such alias, which POSIX makes an error for `unalias`.
	bool unset_alias(std::string_view name);
	// `unalias -a`.
	void clear_aliases() noexcept { _aliases.clear(); }
	[[nodiscard]] bool lookup_alias(std::string_view name, std::string_view& value) const override;
	[[nodiscard]] bool has_aliases() const noexcept { return !_aliases.empty(); }

	// Every alias, sorted by name, for the listing form of `alias`. Sorted for the
	// same reason variables() is: the map is unordered, POSIX leaves the order
	// unspecified, and a stable order is what makes the listing diffable - which is
	// exactly what alias-p.tst's `alias >save_1; unalias -a; eval alias $(cat
	// save_1); alias >save_2; diff save_1 save_2` round trip asks of it. dash prints
	// in hash order, so this is a deliberate divergence in a place POSIX left open.
	struct alias_row {
		std::string_view name;
		std::string_view value;
	};
	[[nodiscard]] std::vector<alias_row> aliases() const;

	// --- functions --------------------------------------------------------------
	//
	// Shell functions, by name. Here rather than in the executor because that is
	// where the glossary puts them, and because the executor is an INTERFACE: a
	// function table living inside one back end vanishes when the back end is
	// replaced, while functions are shell state in exactly the way variables and
	// aliases are (#106).
	//
	// A body is A NODE IN THE TREE its definition was parsed from, so the state
	// that holds the function has to hold the tree too - see retain_tree. That
	// works for one invocation - `-c 'f() {...}; f'` is a single parse - and is
	// the ONLY case that works today: a function defined inside an `eval` or a dot
	// script points into a tree that dies with the call. Persisting a function
	// past the input it was defined in, which an interactive shell needs, requires
	// copying the body into storage the function owns; that is ADR-0007 work and
	// lands with the line editor. Recorded rather than pretended.
	struct function_definition {
		const syntax::tree* tree = nullptr;
		syntax::node_index body = syntax::no_node;
	};

	// A redefinition replaces the previous body, which is what POSIX requires and
	// what makes reloading an rc file work.
	void define_function(std::string_view name, const syntax::tree& t,
	                     syntax::node_index body);
	// nullptr when there is no such function. The result is a POINTER INTO the
	// table, so a caller that runs the body must take a copy first: the body may
	// redefine the function, and an insertion rehashes.
	[[nodiscard]] const function_definition* lookup_function(std::string_view name) const;
	[[nodiscard]] bool has_function(std::string_view name) const {
		return lookup_function(name) != nullptr;
	}
	// POSIX: `unset -f` on a name that is not a function is not an error, so there
	// is nothing to report and nothing to return.
	void unset_function(std::string_view name);

	// Every function's name, sorted, for the completer's enumeration read
	// (#139, spec 6.9). Sorted for the same reason `aliases()` and `variables()`
	// are: the map is unordered and a stable order is what makes a listing
	// reproducible.
	//
	// NAMES ONLY, and deliberately: the bodies are trees this state keeps alive
	// and the caller is on the other side of a thread boundary, so what crosses
	// is a copy of the names and nothing that points back in here.
	[[nodiscard]] std::vector<std::string_view> function_names() const;

	// Keeps a parsed tree alive for as long as this state, and hands back the copy
	// it now owns. The read loop calls it for every command it parses, because the
	// command may define a function whose body is a node in that tree - and the
	// next command may call it (ADR-0007: state owns its values).
	//
	// A DEQUE, not a vector: a function_definition points INTO these trees, and a
	// vector reallocating would move them out from under it. Reading one command at
	// a time is what makes this necessary - a whole-input parse had exactly one
	// tree and the question never came up.
	//
	// The trees' own nodes live in the CALLER's buffer_pool, which must therefore
	// outlive this object. main builds the pool before the state for that reason,
	// and every test fixture declares it first for the same one.
	syntax::tree& retain_tree(syntax::tree t);

	// What a SUBSHELL changes about the state it was forked from, in ONE place.
	//
	// The traps reset to default (except those set to ignore, which stay ignored),
	// and the trap action's entry status goes away: a subshell is a new execution
	// environment, so an `exit` inside one reports ITS last command rather than the
	// command the enclosing trap interrupted. exit-p.tst's 'default exit status in
	// subshell in signal trap' is precisely that case - `trap '((exit 2); exit);
	// echo $?' INT` prints 2 - and it regressed the moment the entry status was
	// added without this, because the child inherited a value that stopped applying
	// to it. Two facts about one event, so they live in one call.
	void enter_subshell(subshell_role role = subshell_role::detached) {
		_signals.reset_for_subshell();
		_trap_entry_status.reset();
		// A THIRD fact about the same event (#159, scoped by #160): whether this
		// subshell manages the terminal.
		//
		// DETACHED CLEARS, and that is the default because it is the safe answer.
		// The fd is what gates the handoff in `spawn`, so dropping it here is what
		// keeps `nvim &` and `$(nvim)` from taking the terminal away from the editor
		// - #158 decision 3 scopes the handoff to FOREGROUND jobs, and every one of
		// those constructs reaches the same `run_simple_command` a foreground
		// command does, one fork further down. Gating on `interactive()` instead
		// would not do: an async child is still an interactive shell's child, and it
		// stays one all the way to its own exec. A fork site that says nothing about
		// its role therefore gets no terminal, and a NEW one cannot steal it from
		// the line editor by omission.
		//
		// A FOREGROUND `( )` IS THE ONE EXCEPTION, and what it needs is the FD
		// rather than the handoff. A subshell does not exec: the command inside it
		// forks AGAIN, into a group of its own, and only a live fd at that inner
		// fork can make `(nvim .)` the terminal's foreground group. So the subshell
		// keeps the fd and its own foreground children take the terminal the way any
		// foreground command does, giving it back to the subshell's group after each
		// - the subshell is a shell, and it manages the terminal for the jobs it
		// runs exactly as its parent does.
		//
		// A FOREGROUND PIPELINE STAGE IS THE OTHER, for a different reason. The
		// TERMINAL it does not need: `run_pipeline` hands it to the group leader
		// before this call, and one handoff covers every stage because the later ones
		// join that group. The FD it does need, because the fd is also what gates the
		// per-process signal reset in `become_command` - and that reset is owed by
		// every stage that execs, not just the leader. Clearing it here was the
		// review defect in #160's first cut: stages 2..N exec'd with SIGTTOU, SIGTTIN
		// and SIGTSTP still ignored, so `less` at the end of a pipeline could not be
		// suspended and `sleep 30 | sleep 30` under Ctrl-Z stopped only its first
		// stage while the shell blocked on the second.
		//
		// A COMPOUND stage keeps the fd too and that is now safe, where before the
		// split it would not have been: the reset lives on the exec path, a compound
		// stage never execs, so it keeps the SIGTTOU ignore that makes handing the
		// terminal to a nested command and taking it back legal - exactly the
		// property a foreground `( )` relies on.
		//
		// KEEPING IS NOT OBTAINING, which is what makes the exception compose: a
		// `( )` inside `&` or inside `$( )` asks to keep an fd that is already -1,
		// and -1 it stays.
		if (role == subshell_role::detached)
			_tty_fd = -1;
	}

	[[nodiscard]] signal_state& signals() noexcept { return _signals; }
	[[nodiscard]] const signal_state& signals() const noexcept { return _signals; }

	[[nodiscard]] bool interactive() const noexcept { return _interactive; }
	// Told to the signal state as well, because POSIX scopes "a signal ignored on
	// entry cannot be trapped" to a NON-interactive shell and signal_state has to
	// answer that on its own - see signal_state::cannot_be_trapped. Setting one and
	// not the other would silently make an interactive shell untrappable.
	void set_interactive(bool v) noexcept {
		_interactive = v;
		_signals.set_interactive(v);
	}

	// THE TERMINAL THIS SHELL MANAGES (#158 decision 6, #159), or -1 for a shell
	// that manages none.
	//
	// A `dup` of the tty taken once at interactive startup; NON-OWNING here, like
	// the binding console below - the interactive wiring site (src/main.cpp) took
	// it and closes it. Two things follow from it being a saved dup rather than
	// fd 0. It is still a terminal in a child whose stdin has been replaced by a
	// pipe, which is exactly when the handoff runs; and it survives `exec 3>&-`
	// and friends, because it lives above the descriptors shell code can name.
	//
	// -1 IS THE GATE, and it is the only one the runtime asks. A non-interactive
	// shell never sets it, so every tty syscall added by #159 is skipped by
	// construction rather than by an `interactive()` test repeated at each site;
	// and `enter_subshell` clears it for every child but a foreground `( )`, which
	// is what scopes the handoff to foreground jobs. See the note there for why
	// that one child is the exception and why a pipeline stage is not.
	void set_tty_fd(int fd) noexcept { _tty_fd = fd; }
	[[nodiscard]] int tty_fd() const noexcept { return _tty_fd; }

	// The keymap registry `bind` reaches, or null when there is no line editor
	// (#118, #134). NON-OWNING and per-shell: the interactive wiring site owns
	// the editing context and lends this view of it for the life of the session,
	// so ADR-0007's "everything has an owner that frees it" is answered on the
	// other side of the boundary. It lived at file scope in builtins.cpp until
	// the loop was wired up, which is what the note there said would happen.
	//
	// `binding_console` is declared in runtime/builtins.h and only pointed at
	// here, so this header stays free of it.
	void set_binding_console(binding_console* console) noexcept {
		_binding_console = console;
	}
	[[nodiscard]] binding_console* console() const noexcept { return _binding_console; }

	// The prompt registry a configuration builtin will reach, or null when there
	// is no line editor (#157, §6.10). Installed and taken away by the same owner
	// on the same terms as the binding console above - non-owning, per-shell,
	// alive for exactly as long as the session that lent it, and null in every
	// non-interactive shell.
	//
	// NOT THE PROMPT PROVIDER, which is leshper's own `prompt_source` and reads
	// `$PS1`. That one answers "what bytes is the prompt"; this one answers "what
	// is the prompt MADE OF", and §6.10 has the second superseding the first
	// rather than either growing into the other.
	void set_prompt_console(prompt_console* console) noexcept {
		_prompt_console = console;
	}
	[[nodiscard]] prompt_console* prompts() const noexcept { return _prompt_console; }

private:
	struct variable {
		std::string value;
		bool exported = false;
		bool readonly = false;
		// `readonly x` or `export x` on an unset x creates the entry to hold the
		// flag without creating the variable. lookup() reports it as absent, and
		// the environment block leaves it out - otherwise `export x` would send an
		// empty x to every child, which dash does not (#71).
		bool assigned = true;
	};

	std::unordered_map<std::string, variable, lesh::transparent_string_hash, std::equal_to<>> _vars;
	std::string _ifs_default = " \t\n";
	// `~user` lookups, cached. getpwnam returns a pointer into storage the NEXT
	// call may reuse, and the expander is handed a string_view that has to outlive
	// the call, so the answer is copied. Cached because a word can hold several
	// (`PATH=~a/bin:~a/sbin`) and because a failed lookup is worth not repeating.
	// `mutable`: this is a cache behind a const query, not state the shell has.
	mutable std::unordered_map<std::string, std::string, lesh::transparent_string_hash,
	                           std::equal_to<>> _user_homes;
	std::unordered_map<std::string, std::string, lesh::transparent_string_hash, std::equal_to<>> _aliases;
	std::unordered_map<std::string, function_definition, lesh::transparent_string_hash,
	                   std::equal_to<>> _functions;
	// The trees the function bodies point into. See retain_tree for why it is a
	// deque and why the pool has to outlive this object.
	std::deque<syntax::tree> _retained_trees;
	std::vector<std::string> _positional;
	// $0 for a state nobody invoked - a unit test's, chiefly. A real shell always
	// overrides it: parse_invocation names $0 from an operand or from argv[0], so
	// this literal is never what a running shell reports. It WAS, for every
	// invocation that named no command_file, which is issue #43.
	std::string _script_name = "lesh";
	// Where the running command begins. See set_command_origin.
	const syntax::tree* _origin = nullptr;
	uint32_t _origin_offset = 0;
	std::string_view _origin_file;
	syntax::source_map _lines{{}};
	// `$LINENO`'s digits. A member because lookup() hands back a view and has
	// nowhere else to put them; 12 bytes holds every uint32_t and its NUL. Mutable
	// because writing them changes nothing an observer can see - the same offset
	// gives the same digits - which is what keeps lookup() const.
	mutable std::array<char, 12> _lineno_digits{};
	int _pid = 0;
	std::string _own_path;
	signal_state _signals;
	int _last_status = 0;
	std::optional<int> _trap_entry_status;
	size_t _getopts_offset = 0;
	options _options;
	bool _interactive = false;
	// Non-owning; see set_tty_fd. -1 in every non-interactive shell and in every
	// detached subshell, which is what makes the terminal handoff opt-in.
	int _tty_fd = -1;
	// Non-owning; see set_binding_console. Null in every non-interactive shell,
	// which is what makes `bind` say "no line editor" rather than pretend.
	binding_console* _binding_console = nullptr;
	// Non-owning; see set_prompt_console. Null in every non-interactive shell,
	// which is what a prompt-configuration builtin will answer "no line editor"
	// from, exactly as `bind` does today.
	prompt_console* _prompt_console = nullptr;
	// Backing store for option_flags(). Rebuilt on demand and mutable because
	// parameter_source::option_flags() is const, for the same reason
	// environment_block() owns its strings: the returned view must outlive the call.
	mutable std::string _option_flags;

	// Backing store for environment_block(). Rebuilt on demand; owned here so the
	// strings outlive the char* array handed to execve.
	std::vector<std::string> _env_strings;
	std::vector<char*> _env_pointers;
};

} // namespace lesh::runtime
