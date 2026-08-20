#include "runtime/shell_state.h"

#include <array>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace lesh::runtime {

namespace {

// POSIX XCU 2.14. The list is closed and the membership matters: a failure in one
// of these exits a non-interactive shell, and assignments preceding one persist.
constexpr std::array<std::string_view, 15> kSpecialBuiltins = {
	"break", ":", "continue", ".", "eval", "exec", "exit", "export",
	"readonly", "return", "set", "shift", "times", "trap", "unset",
};

constexpr std::array<std::string_view, 11> kRegularBuiltins = {
	"cd", "echo", "false", "pwd", "true", "test", "alias", "unalias",
	"read", "command", "kill",
};

} // namespace

builtin_kind classify_builtin(std::string_view name) noexcept {
	for (const auto& s : kSpecialBuiltins)
		if (s == name)
			return builtin_kind::special;
	for (const auto& r : kRegularBuiltins)
		if (r == name)
			return builtin_kind::regular;
	return builtin_kind::none;
}

namespace {

// This process's own executable path. There is no portable call for it: macOS has
// _NSGetExecutablePath, Linux has /proc/self/exe. Both are one line, and the
// alternative - trusting argv[0] - is wrong the moment the shell is found on PATH.
std::string resolve_own_path() {
#if defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);  // asks for the required length
	std::string buffer(size, '\0');
	if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
		return {};
	buffer.resize(std::strlen(buffer.c_str()));
	return buffer;
#else
	std::string buffer(4096, '\0');
	const ssize_t got = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
	if (got <= 0)
		return {};
	buffer.resize(static_cast<size_t>(got));
	return buffer;
#endif
}

} // namespace

shell_state::shell_state() {
	_pid = static_cast<int>(getpid());
	_own_path = resolve_own_path();
	// Seed from the process environment. Copying rather than borrowing is the
	// ADR-0007 decision: these strings are owned here and released in the
	// destructor, so the leak gate expects zero rather than "zero except envp".
	for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
		const std::string_view entry{*e};
		const size_t eq = entry.find('=');
		if (eq == std::string_view::npos)
			continue;
		_vars.emplace(std::string(entry.substr(0, eq)),
		              variable{std::string(entry.substr(eq + 1)), true});
	}

	// PPID is a variable POSIX requires the shell to set at startup, not a special
	// parameter - it is assignable and it is NOT exported, which is what dash does
	// too. Set after the environment is copied so an inherited PPID cannot win.
	//
	// It was missing, and the yash signal tests use `kill -s SIG $PPID` from a child
	// shell for every case whose sender is another process: ninety of the 180 cases
	// per file could not send the signal at all.
	_vars.insert_or_assign("PPID", variable{std::to_string(getppid()), false});
}

shell_state::~shell_state() = default;

void shell_state::set_alias(std::string_view name, std::string_view value) {
	if (const auto it = _aliases.find(name); it != _aliases.end()) {
		it->second = value;
		return;
	}
	_aliases.emplace(std::string(name), std::string(value));
}

void shell_state::unset_alias(std::string_view name) {
	if (const auto it = _aliases.find(name); it != _aliases.end())
		_aliases.erase(it);
}

bool shell_state::lookup_alias(std::string_view name, std::string_view& value) const {
	const auto it = _aliases.find(name);
	if (it == _aliases.end())
		return false;
	value = it->second;
	return true;
}

bool shell_state::positional_at(size_t index, std::string_view& out) const {
	if (index == 0 || index > _positional.size())
		return false;
	out = _positional[index - 1];
	return true;
}

bool shell_state::shift_positional(size_t n) {
	if (n > _positional.size())
		return false;
	_positional.erase(_positional.begin(), _positional.begin() + static_cast<long>(n));
	return true;
}

bool shell_state::lookup(std::string_view name, std::string_view& value) const {
	const auto it = _vars.find(name);
	if (it == _vars.end())
		return false;
	value = it->second.value;
	return true;
}

std::string_view shell_state::home_directory() const {
	std::string_view home;
	if (lookup("HOME", home))
		return home;
	return "/";
}

std::string_view shell_state::ifs() const {
	std::string_view value;
	// POSIX: when IFS is unset, splitting behaves as if it were space-tab-newline.
	// When it is set but empty, no splitting happens at all - which is why this
	// returns the value rather than falling back on emptiness.
	if (lookup("IFS", value))
		return value;
	return _ifs_default;
}

int64_t shell_state::get(std::string_view name) const {
	std::string_view text;
	if (!lookup(name, text))
		return 0;  // unset is zero, not an error
	int64_t value = 0;
	bool negative = false;
	size_t at = 0;
	if (at < text.size() && (text[at] == '-' || text[at] == '+')) {
		negative = text[at] == '-';
		++at;
	}
	// A non-numeric value is zero too: POSIX leaves it unspecified, and dash
	// treats it as zero rather than failing.
	for (; at < text.size(); ++at) {
		if (text[at] < '0' || text[at] > '9')
			return 0;
		value = value * 10 + (text[at] - '0');
	}
	return negative ? -value : value;
}

void shell_state::set(std::string_view name, int64_t value) {
	set(name, std::to_string(value));
}

void shell_state::set(std::string_view name, std::string_view value) {
	const auto it = _vars.find(name);
	if (it != _vars.end()) {
		it->second.value = value;
		return;
	}
	_vars.emplace(std::string(name), variable{std::string(value), false});
}

void shell_state::set_exported(std::string_view name, std::string_view value) {
	set(name, value);
	_vars.find(name)->second.exported = true;
}

void shell_state::unset(std::string_view name) {
	if (const auto it = _vars.find(name); it != _vars.end())
		_vars.erase(it);
}

bool shell_state::is_exported(std::string_view name) const {
	const auto it = _vars.find(name);
	return it != _vars.end() && it->second.exported;
}

// The one table of POSIX shell options. `set` and the command line both route
// through here so they cannot disagree about which letters exist.
bool shell_state::apply_option_letter(options& o, char letter, bool enable) {
	switch (letter) {
		case 'a': o.all_export = enable; return true;
		case 'b': o.notify = enable; return true;
		case 'C': o.no_clobber = enable; return true;
		case 'e': o.exit_on_error = enable; return true;
		case 'f': o.no_glob = enable; return true;
		case 'h': o.hash_all = enable; return true;
		case 'm': o.monitor = enable; return true;
		case 'n': o.no_exec = enable; return true;
		case 'u': o.error_on_unset = enable; return true;
		case 'v': o.verbose = enable; return true;
		case 'x': o.trace = enable; return true;
		default: return false;
	}
}

// The `-o name` spellings, which POSIX lists alongside the letters. `ignoreeof`,
// `nolog` and `vi` have no letter at all, which is why this is a second table
// rather than a lookup from name to letter.
bool shell_state::apply_option_name(options& o, std::string_view name, bool enable) {
	if (name == "allexport") { o.all_export = enable; return true; }
	if (name == "errexit")   { o.exit_on_error = enable; return true; }
	if (name == "ignoreeof") { o.ignore_eof = enable; return true; }
	if (name == "monitor")   { o.monitor = enable; return true; }
	if (name == "noclobber") { o.no_clobber = enable; return true; }
	if (name == "noexec")    { o.no_exec = enable; return true; }
	if (name == "noglob")    { o.no_glob = enable; return true; }
	if (name == "nolog")     { o.no_log = enable; return true; }
	if (name == "notify")    { o.notify = enable; return true; }
	if (name == "nounset")   { o.error_on_unset = enable; return true; }
	if (name == "verbose")   { o.verbose = enable; return true; }
	if (name == "vi")        { o.vi = enable; return true; }
	if (name == "xtrace")    { o.trace = enable; return true; }
	return false;
}

char** shell_state::environment_block() {
	_env_strings.clear();
	_env_pointers.clear();
	for (const auto& [name, var] : _vars) {
		if (!var.exported)
			continue;
		_env_strings.emplace_back(name + "=" + var.value);
	}
	// Pointers are taken only after every string is in place: _env_strings can
	// reallocate while it is being filled, which would dangle every pointer taken
	// during the loop. This is the same class of bug as the arena_array leak.
	_env_pointers.reserve(_env_strings.size() + 1);
	for (auto& s : _env_strings)
		_env_pointers.push_back(s.data());
	_env_pointers.push_back(nullptr);
	return _env_pointers.data();
}

} // namespace lesh::runtime
