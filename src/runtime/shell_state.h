#pragma once

#include "runtime/expander.h"
#include "runtime/signals.h"
#include "syntax/parser.h"
#include "substrate/traits.h"

#include <array>
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

	// Marks a name exported WITHOUT writing a value, which `export name` needs:
	// exporting a readonly variable is not an assignment and dash allows it, so
	// routing it through set() would refuse a command POSIX permits.
	void mark_exported(std::string_view name);

	// --- readonly -------------------------------------------------------------
	//
	// POSIX: a readonly variable cannot be assigned to and cannot be unset. The
	// flag lives on the variable rather than in a separate set, so a name can be
	// readonly while UNSET - `readonly x` then `x=1` fails, and `${x-unset}`
	// still says unset, which is what dash does and what readonly-p.tst checks.

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
		bool assigned = false;   // false for a name that is readonly but unset
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
	void enter_subshell() {
		_signals.reset_for_subshell();
		_trap_entry_status.reset();
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

private:
	struct variable {
		std::string value;
		bool exported = false;
		bool readonly = false;
		// `readonly x` on an unset x creates the entry to hold the flag without
		// creating the variable. lookup() reports it as absent, and the
		// environment block leaves it out, or `sh -c 'readonly x' ` would export
		// an empty x to every child.
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
	std::vector<std::string> _positional;
	std::string _script_name = "lesh";
	int _pid = 0;
	std::string _own_path;
	signal_state _signals;
	int _last_status = 0;
	std::optional<int> _trap_entry_status;
	size_t _getopts_offset = 0;
	options _options;
	bool _interactive = false;
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
