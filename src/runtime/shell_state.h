#pragma once

#include "runtime/expander.h"
#include "runtime/signals.h"
#include "syntax/parser.h"
#include "substrate/traits.h"

#include <array>
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
	[[nodiscard]] std::string_view ifs() const override;
	[[nodiscard]] int last_status_value() const override { return _last_status; }
	[[nodiscard]] int process_id_value() const override { return _pid; }
	[[nodiscard]] size_t positional_count() const override { return _positional.size(); }
	[[nodiscard]] bool positional_at(size_t index, std::string_view& out) const override;
	[[nodiscard]] std::string_view script_name_value() const override { return _script_name; }
	[[nodiscard]] std::string_view option_flags() const override;

	// --- arithmetic_variables -------------------------------------------------

	[[nodiscard]] int64_t get(std::string_view name) const override;
	void set(std::string_view name, int64_t value) override;
	[[nodiscard]] bool defined(std::string_view name) const override {
		std::string_view ignored;
		return lookup(name, ignored);
	}

	// --- variables ------------------------------------------------------------

	// Copies the value in. The state owns it from here, which is what makes the
	// zero-leak gate reachable - the alternative is what legacy does.
	void set(std::string_view name, std::string_view value);
	void set_exported(std::string_view name, std::string_view value);
	void assign_parameter(std::string_view name, std::string_view value) override {
		set(name, value);
	}
	void unset(std::string_view name);
	[[nodiscard]] bool is_exported(std::string_view name) const;

	// Builds the envp array for a child process. The returned pointers are owned
	// by this object and stay valid until the next call.
	[[nodiscard]] char** environment_block();

	// --- special and positional parameters ------------------------------------

	[[nodiscard]] int last_status() const noexcept { return _last_status; }
	void set_last_status(int status) noexcept { _last_status = status; }

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
	void set_optind(size_t index, size_t offset);

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
	void unset_alias(std::string_view name);
	[[nodiscard]] bool lookup_alias(std::string_view name, std::string_view& value) const override;
	[[nodiscard]] bool has_aliases() const noexcept { return !_aliases.empty(); }

	[[nodiscard]] signal_state& signals() noexcept { return _signals; }
	[[nodiscard]] const signal_state& signals() const noexcept { return _signals; }

	[[nodiscard]] bool interactive() const noexcept { return _interactive; }
	void set_interactive(bool v) noexcept { _interactive = v; }

private:
	struct variable {
		std::string value;
		bool exported = false;
	};

	std::unordered_map<std::string, variable, lesh::transparent_string_hash, std::equal_to<>> _vars;
	std::string _ifs_default = " \t\n";
	std::unordered_map<std::string, std::string, lesh::transparent_string_hash, std::equal_to<>> _aliases;
	std::vector<std::string> _positional;
	std::string _script_name = "lesh";
	int _pid = 0;
	std::string _own_path;
	signal_state _signals;
	int _last_status = 0;
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
