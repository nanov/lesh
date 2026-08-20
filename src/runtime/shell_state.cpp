#include "runtime/shell_state.h"

#include <array>
#include <cstdlib>
#include <unistd.h>

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

shell_state::shell_state() {
	_pid = static_cast<int>(getpid());
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
