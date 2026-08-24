#include "runtime/shell_state.h"

#include <algorithm>
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

	// POSIX: OPTIND is initialised to 1 when the shell is invoked - which is why
	// this OVERRIDES an inherited value rather than importing it, and why it is
	// unexported: `OPTIND=7 sh -c 'echo $OPTIND'` prints 1 in dash too, and
	// getopts-p.tst asserts that a child shell sees its own 1 rather than the
	// parent's index. OPTARG is deliberately NOT initialised; POSIX says nothing
	// about it, and dash passes an inherited one through.
	_vars.insert_or_assign("OPTIND", variable{"1", false});

	// The rest of the set POSIX XCU 2.5.3 makes the SHELL's job to set, enumerated
	// once rather than discovered one conformance file at a time (#42). Every one
	// of these OVERRIDES an inherited value, because POSIX says the shell sets it
	// at invocation - `IFS=X dash -c 'printf "[%s]" "$IFS"'` prints the default,
	// which fsplit-p.tst asserts as 'default IFS (overriding environment
	// variable)'. None is exported, matching dash.
	//
	// IFS was the one that mattered: splitting read shell_state::ifs(), which falls
	// back to the default, so the MECHANISM was right while `echo "[$IFS]"` printed
	// nothing and `${IFS+set}` said unset. The variable and the mechanism disagreed.
	_vars.insert_or_assign("IFS", variable{" \t\n", false});
	// PS1, PS2 and PS4 have POSIX-specified defaults. PS4 is the one with a test:
	// option-p.tst's '$PS4' assigns it and expects `set -x` to use the assigned
	// value, and trace_command already reads the variable - it only lacked a
	// default to be overridden.
	_vars.insert_or_assign("PS1", variable{"$ ", false});
	_vars.insert_or_assign("PS2", variable{"> ", false});
	_vars.insert_or_assign("PS4", variable{"+ ", false});

	// PWD is deliberately NOT here. It is inherited from the environment and
	// rewritten by `cd`, which owns the logical-versus-physical question POSIX
	// attaches to it (#46); seeding it here would put a second author on one
	// variable. dash refreshes it from getcwd at startup and lesh does not - a
	// divergence visible only as `env -u PWD lesh -c 'echo ${PWD-unset}'`.
	//
	// LINENO is deliberately NOT here. It is not a constant to seed but a variable
	// the shell must keep current as it reads, and a token's offset can land in an
	// alias text region rather than in the input (#47), so the mapping from offset
	// to line number is entangled with alias substitution. Seeding it with 1 would
	// be a stub that succeeds: `echo $LINENO` on line 9 would print 1 and look
	// implemented. lineno-p.tst stays 0/3 and says why.
}

shell_state::~shell_state() = default;

void shell_state::set_alias(std::string_view name, std::string_view value) {
	if (const auto it = _aliases.find(name); it != _aliases.end()) {
		it->second = value;
		return;
	}
	_aliases.emplace(std::string(name), std::string(value));
}

bool shell_state::unset_alias(std::string_view name) {
	const auto it = _aliases.find(name);
	if (it == _aliases.end())
		return false;
	_aliases.erase(it);
	return true;
}

std::vector<shell_state::alias_row> shell_state::aliases() const {
	std::vector<alias_row> rows;
	rows.reserve(_aliases.size());
	for (const auto& [name, value] : _aliases)
		rows.push_back({name, value});
	std::sort(rows.begin(), rows.end(),
	          [](const alias_row& a, const alias_row& b) { return a.name < b.name; });
	return rows;
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
	// An entry that holds nothing but a readonly flag - `readonly x` on an unset x -
	// is ABSENT to every reader: `${x-unset}` says unset in dash too, and reporting
	// it as an empty string would make `set -u` accept it.
	if (it == _vars.end() || !it->second.assigned)
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

bool shell_state::set(std::string_view name, int64_t value) {
	// Arithmetic's port, so the diagnostic is the unprefixed one: dash reports
	// `x: is read only` for `$((x=1))` exactly as it does for `x=1`.
	if (!set(name, std::to_string(value))) {
		report_readonly({}, name);
		return false;
	}
	return true;
}

bool shell_state::assign_parameter(std::string_view name, std::string_view value) {
	// `${x=default}`. Reported here rather than in the expander, because the
	// expander must not know what a variable table is - and dash words this one
	// like a plain assignment too.
	if (!set(name, value)) {
		report_readonly({}, name);
		return false;
	}
	return true;
}

bool shell_state::set(std::string_view name, std::string_view value) {
	// `set -a`: every variable CREATED OR MODIFIED by an assignment is marked for
	// export. Applied here rather than at each assignment site so `x=1`, a `for`
	// loop's variable, `read`, and `${x=default}` all obey it - which is what dash
	// does, verified with `dash -a -c 'for i in 1; do sh -c "echo \${i-unset}"; done'`.
	const bool exported = _options.all_export;
	const auto it = _vars.find(name);
	if (it != _vars.end()) {
		// POSIX: a readonly variable cannot be assigned to. Refused here, in the
		// ONE place every assignment funnels through, so `x=1`, a `for` variable,
		// `read`, `getopts`, `${x=1}` and `$((x=1))` cannot each forget it.
		if (it->second.readonly)
			return false;
		// Assigning OPTIND restarts getopts: the within-word position it kept beside
		// the index is discarded, so `OPTIND=1; getopts abc o` re-reads `-abc` from
		// its first letter instead of resuming at the letter it had reached. Every
		// assignment funnels through here, so the hook cannot be bypassed by `export
		// OPTIND=1`, `${OPTIND=1}`, or a `read OPTIND` - and it comes after the
		// readonly test, because an assignment that was REFUSED must not restart the
		// parse as a side effect of failing.
		if (name == "OPTIND")
			_getopts_offset = 0;
		it->second.value = value;
		it->second.exported = it->second.exported || exported;
		it->second.assigned = true;
		return true;
	}
	if (name == "OPTIND")
		_getopts_offset = 0;
	_vars.emplace(std::string(name), variable{std::string(value), exported, false, true});
	return true;
}

bool shell_state::set_exported(std::string_view name, std::string_view value) {
	if (!set(name, value))
		return false;
	_vars.find(name)->second.exported = true;
	return true;
}

void shell_state::mark_exported(std::string_view name) {
	const auto it = _vars.find(name);
	if (it != _vars.end()) {
		it->second.exported = true;
		return;
	}
	// `export x` on an unset x exports the NAME: dash's `export -p` lists it and
	// a child sees x set to the empty string.
	_vars.emplace(std::string(name), variable{std::string{}, true, false, true});
}

void shell_state::mark_readonly(std::string_view name) {
	const auto it = _vars.find(name);
	if (it != _vars.end()) {
		it->second.readonly = true;
		return;
	}
	// Readonly and UNSET: the entry exists only to hold the flag, so lookup() and
	// the environment block both skip it until something assigns.
	_vars.emplace(std::string(name), variable{std::string{}, false, true, false});
}

bool shell_state::is_readonly(std::string_view name) const {
	const auto it = _vars.find(name);
	return it != _vars.end() && it->second.readonly;
}

void shell_state::report_readonly(std::string_view who, std::string_view name) {
	if (who.empty()) {
		std::fprintf(stderr, "lesh: %.*s: is read only\n",
		             static_cast<int>(name.size()), name.data());
		return;
	}
	std::fprintf(stderr, "lesh: %.*s: %.*s: is read only\n",
	             static_cast<int>(who.size()), who.data(),
	             static_cast<int>(name.size()), name.data());
}

std::vector<shell_state::variable_row> shell_state::variables() const {
	std::vector<variable_row> rows;
	rows.reserve(_vars.size());
	for (const auto& [name, var] : _vars)
		rows.push_back({name, var.value, var.assigned, var.exported, var.readonly});
	std::sort(rows.begin(), rows.end(),
	          [](const variable_row& a, const variable_row& b) { return a.name < b.name; });
	return rows;
}

bool shell_state::unset(std::string_view name) {
	// POSIX: unsetting a readonly variable is an error. Checked before the OPTIND
	// hook, so a refused `unset OPTIND` does not restart getopts as a side effect
	// of failing.
	if (const auto it = _vars.find(name); it != _vars.end()) {
		if (it->second.readonly)
			return false;
		if (name == "OPTIND")
			_getopts_offset = 0;  // same restart as an assignment; see set()
		_vars.erase(it);
		return true;
	}
	if (name == "OPTIND")
		_getopts_offset = 0;
	return true;
}

bool shell_state::set_optind(size_t index, size_t offset) {
	// set() clears the offset unconditionally, so the offset getopts wants to keep
	// is written back AFTER the index. Ordering, not redundancy.
	if (!set("OPTIND", std::to_string(index)))
		return false;
	_getopts_offset = offset;
	return true;
}

bool shell_state::is_exported(std::string_view name) const {
	const auto it = _vars.find(name);
	return it != _vars.end() && it->second.exported;
}

// The one table of POSIX shell options.
//
// Ordered as POSIX lists them in `set`: the lettered options by letter, then the
// three that have a `-o` spelling and no letter at all. That order is what `$-`
// and `set -o` print in, and it has to be STABLE - set-p.tst's round-trip diffs
// one `set -o` listing against another.
//
// `h` is the one lettered option POSIX gives no `-o` name; dash rejects the letter
// outright and fails both hashondef cases in option-p.tst, so accepting and
// reporting it is a deliberate divergence in lesh's favour.
const std::array<shell_state::option_descriptor, shell_state::kOptionCount>&
shell_state::option_table() noexcept {
	static constexpr std::array<option_descriptor, kOptionCount> table = {{
		{'a', "allexport", &options::all_export},
		{'b', "notify",    &options::notify},
		{'C', "noclobber", &options::no_clobber},
		{'e', "errexit",   &options::exit_on_error},
		{'f', "noglob",    &options::no_glob},
		{'h', "",          &options::hash_all},
		{'m', "monitor",   &options::monitor},
		{'n', "noexec",    &options::no_exec},
		{'u', "nounset",   &options::error_on_unset},
		{'v', "verbose",   &options::verbose},
		{'x', "xtrace",    &options::trace},
		{'\0', "ignoreeof", &options::ignore_eof},
		{'\0', "nolog",     &options::no_log},
		{'\0', "pipefail",  &options::pipefail},
		{'\0', "vi",        &options::vi},
	}};
	return table;
}

bool shell_state::apply_option_letter(options& o, char letter, bool enable) {
	for (const auto& row : option_table()) {
		if (row.letter != '\0' && row.letter == letter) {
			o.*row.field = enable;
			return true;
		}
	}
	return false;
}

bool shell_state::apply_option_name(options& o, std::string_view name, bool enable) {
	for (const auto& row : option_table()) {
		if (!row.name.empty() && row.name == name) {
			o.*row.field = enable;
			return true;
		}
	}
	return false;
}

// `$-`: the letters of the options currently on.
//
// POSIX leaves the order unspecified, so the table's order is used - stable rather
// than arbitrary, because option-p.tst and set-p.tst both `grep` this string and a
// reordering between two calls would be a real bug.
//
// `i` is not in the table: interactive is not a `set` option, but POSIX lists it
// among the flags `$-` reports and dash prints it, so it is appended here.
std::string_view shell_state::option_flags() const {
	_option_flags.clear();
	for (const auto& row : option_table())
		if (row.letter != '\0' && _options.*row.field)
			_option_flags.push_back(row.letter);
	if (_interactive)
		_option_flags.push_back('i');
	return _option_flags;
}

char** shell_state::environment_block() {
	_env_strings.clear();
	_env_pointers.clear();
	for (const auto& [name, var] : _vars) {
		if (!var.exported || !var.assigned)
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
