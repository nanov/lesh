#pragma once

#include "runtime/expander.h"
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

class shell_state final : public parameter_source, public arithmetic_variables {
public:
	shell_state();
	~shell_state() override;

	shell_state(const shell_state&) = delete;
	shell_state& operator=(const shell_state&) = delete;

	// --- parameter_source -----------------------------------------------------

	[[nodiscard]] bool lookup(std::string_view name, std::string_view& value) const override;
	[[nodiscard]] std::string_view home_directory() const override;
	[[nodiscard]] std::string_view ifs() const override;

	// --- arithmetic_variables -------------------------------------------------

	[[nodiscard]] int64_t get(std::string_view name) const override;
	void set(std::string_view name, int64_t value) override;

	// --- variables ------------------------------------------------------------

	// Copies the value in. The state owns it from here, which is what makes the
	// zero-leak gate reachable - the alternative is what legacy does.
	void set(std::string_view name, std::string_view value);
	void set_exported(std::string_view name, std::string_view value);
	void unset(std::string_view name);
	[[nodiscard]] bool is_exported(std::string_view name) const;

	// Builds the envp array for a child process. The returned pointers are owned
	// by this object and stay valid until the next call.
	[[nodiscard]] char** environment_block();

	// --- special parameters ---------------------------------------------------

	[[nodiscard]] int last_status() const noexcept { return _last_status; }
	void set_last_status(int status) noexcept { _last_status = status; }

	// --- options --------------------------------------------------------------

	// set -e: exit on a command failing. set -u: unset parameter is an error.
	// set -x: trace. Held here rather than as globals so a subshell can copy them.
	struct options {
		bool exit_on_error = false;
		bool error_on_unset = false;
		bool trace = false;
	};
	[[nodiscard]] options& opts() noexcept { return _options; }
	[[nodiscard]] const options& opts() const noexcept { return _options; }

	[[nodiscard]] bool interactive() const noexcept { return _interactive; }
	void set_interactive(bool v) noexcept { _interactive = v; }

private:
	struct variable {
		std::string value;
		bool exported = false;
	};

	std::unordered_map<std::string, variable, lesh::transparent_string_hash, std::equal_to<>> _vars;
	std::string _ifs_default = " \t\n";
	int _last_status = 0;
	options _options;
	bool _interactive = false;

	// Backing store for environment_block(). Rebuilt on demand; owned here so the
	// strings outlive the char* array handed to execve.
	std::vector<std::string> _env_strings;
	std::vector<char*> _env_pointers;
};

} // namespace lesh::runtime
