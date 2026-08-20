#pragma once

#include "runtime/expander.h"
#include "runtime/signals.h"
#include "syntax/parser.h"
#include "substrate/traits.h"

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

	// --- arithmetic_variables -------------------------------------------------

	[[nodiscard]] int64_t get(std::string_view name) const override;
	void set(std::string_view name, int64_t value) override;

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

	// --- options --------------------------------------------------------------

	// set -e: exit on a command failing. set -u: unset parameter is an error.
	// set -x: trace. Held here rather than as globals so a subshell can copy them.
	//
	// The POSIX option letters lesh does not yet honour are still RECORDED, because
	// POSIX requires the shell to accept them on its own command line and in `set`.
	// Rejecting them is what kept lesh from reading a single byte of the yash signal
	// suite, which invokes the testee as `sh +i +m`. Recorded-but-inert is marked as
	// such below so it is a known gap rather than a silent lie; honouring them is
	// issue #31's business.
	struct options {
		bool exit_on_error = false;   // -e
		bool error_on_unset = false;  // -u
		bool trace = false;           // -x
		bool no_glob = false;         // -f
		// Accepted and recorded, not yet honoured:
		bool all_export = false;      // -a
		bool notify = false;          // -b
		bool no_clobber = false;      // -C
		bool hash_all = false;        // -h
		bool monitor = false;         // -m, job control: out of scope per ADR-0001
		bool no_exec = false;         // -n
		bool verbose = false;         // -v
		bool ignore_eof = false;      // -o ignoreeof
		bool no_log = false;          // -o nolog
		bool vi = false;              // -o vi, the line editor's business (Phase 4)
	};

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
	signal_state _signals;
	int _last_status = 0;
	options _options;
	bool _interactive = false;

	// Backing store for environment_block(). Rebuilt on demand; owned here so the
	// strings outlive the char* array handed to execve.
	std::vector<std::string> _env_strings;
	std::vector<char*> _env_pointers;
};

} // namespace lesh::runtime
