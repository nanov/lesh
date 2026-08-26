#include "runtime/shell_state.h"

#include "runtime/diagnostic.h"
#include "substrate/numeric.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <string>
#include <climits>
#include <pwd.h>
#include <sys/stat.h>
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

	// PWD, which #42 left to #46's author and #51 closed. POSIX 2.5.3 makes setting
	// it the SHELL's job at initialization: from the environment only when the
	// inherited value still names the current directory, and from getcwd otherwise.
	//
	// Doing neither cost twice over. `PWD=/etc lesh -c 'pwd; cd ..; pwd'` answered
	// /etc and then /, because a lie in the environment reached `pwd` AND became the
	// directory every later relative `cd` was joined onto; and `env -u PWD lesh -c
	// 'echo "[$PWD]"'` printed `[]`, where a script reading $PWD expects a pathname.
	//
	// The check is names_current_directory, whose whole point is that it is not a
	// string comparison - an inherited `/tmp` on a system where /tmp is a symlink to
	// /private/tmp is a LOGICAL pathname of this directory and is kept, which is what
	// #46's -L exists to maintain. Exported, as dash exports it, so a child shell
	// inherits the answer rather than recomputing a physical one.
	//
	// When getcwd cannot answer - the directory removed under the shell - nothing is
	// written: there is no better value than the inherited one, and replacing a
	// plausible pathname with an empty string would lose the last thing known.
	if (std::string_view inherited;
	    !lookup("PWD", inherited) || !names_current_directory(inherited)) {
		if (std::string here = physical_working_directory(); !here.empty())
			_vars.insert_or_assign("PWD", variable{std::move(here), true});
	}

	//
	// LINENO is still deliberately NOT here, and now for a reason it can be proud
	// of rather than a deferral. It is not a constant to seed but a QUESTION about
	// where the shell is, answered in lookup() from the offset of the command
	// running - see set_command_origin. Seeding a value would need a write per
	// command, and lineno-p.tst's third assertion says why that write cannot be a
	// per-line counter: a multi-line expansion has to advance the number by the
	// lines it spans, which only a lookup from an offset can do (#76).

	// From here on this shell is where a diagnostic reports itself from (#61).
	bind_diagnostics(this);
}

shell_state::~shell_state() { unbind_diagnostics(this); }

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

std::vector<std::string_view> shell_state::function_names() const {
	std::vector<std::string_view> names;
	names.reserve(_functions.size());
	for (const auto& [name, definition] : _functions)
		names.push_back(name);
	std::sort(names.begin(), names.end());
	return names;
}

bool shell_state::lookup_alias(std::string_view name, std::string_view& value) const {
	const auto it = _aliases.find(name);
	if (it == _aliases.end())
		return false;
	value = it->second;
	return true;
}

void shell_state::define_function(std::string_view name, const syntax::tree& t,
                                  syntax::node_index body) {
	if (const auto it = _functions.find(name); it != _functions.end()) {
		it->second = {&t, body};
		return;
	}
	_functions.emplace(std::string(name), function_definition{&t, body});
}

const shell_state::function_definition*
shell_state::lookup_function(std::string_view name) const {
	const auto it = _functions.find(name);
	return it == _functions.end() ? nullptr : &it->second;
}

void shell_state::unset_function(std::string_view name) {
	if (const auto it = _functions.find(name); it != _functions.end())
		_functions.erase(it);
}

syntax::tree& shell_state::retain_tree(syntax::tree t) {
	_retained_trees.push_back(std::move(t));
	return _retained_trees.back();
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
	if (it != _vars.end() && it->second.assigned) {
		value = it->second.value;
		return true;
	}
	// `LINENO` LAST, so an explicit assignment wins (#76). POSIX says the shell
	// sets it, not that the shell owns it, and dash agrees: `LINENO=99; echo
	// $LINENO` prints 99. Answering it before the map would make the variable
	// writable and the write invisible, which is worse than either answer.
	//
	// Answered HERE rather than seeded at startup because it is not a value, it is
	// a QUESTION about where the shell is - see set_command_origin. Seeding it
	// would need a write per command for a number almost no script reads, and the
	// yash file's third assertion says why a per-line counter cannot be that write.
	//
	// Not in the map also means NOT EXPORTED, which is what dash does: `env` in a
	// dash script lists no LINENO.
	if (name == "LINENO") {
		const uint32_t line = where().line;
		// Zero means no command is running - startup, or the shell's own command
		// line. POSIX leaves LINENO unspecified outside a script or function, and
		// reporting ABSENT there is what lets `${LINENO-x}` say so.
		if (line == 0)
			return false;
		const auto [end, ec] = std::to_chars(_lineno_digits.data(),
		                                     _lineno_digits.data() + _lineno_digits.size(),
		                                     line);
		if (ec != std::errc{})
			return false;
		value = std::string_view{_lineno_digits.data(),
		                         static_cast<size_t>(end - _lineno_digits.data())};
		return true;
	}
	return false;
}

std::string_view shell_state::home_directory() const {
	std::string_view home;
	if (lookup("HOME", home))
		return home;
	return "/";
}

// `~user`. getpwnam is libc, so ADR-0005's one-self-contained-binary rule is not
// at issue: there is no runtime shared-library dependency a static build would not
// already have. A miss is cached as an empty string, so `~nosuchuser` in a loop
// does not hit the password database every iteration.
bool shell_state::home_directory_of(std::string_view user, std::string_view& out) const {
	if (user.empty())
		return false;
	if (const auto it = _user_homes.find(user); it != _user_homes.end()) {
		if (it->second.empty())
			return false;
		out = it->second;
		return true;
	}
	// getpwnam wants a NUL-terminated name, and a string_view is not one.
	const std::string name{user};
	const struct passwd* entry = ::getpwnam(name.c_str());
	const auto [it, _] = _user_homes.emplace(
		name, entry != nullptr && entry->pw_dir != nullptr ? entry->pw_dir : "");
	if (it->second.empty())
		return false;
	out = it->second;
	return true;
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

bool shell_state::names_current_directory(std::string_view path) {
	// Absolute first, because everything after it is meaningless otherwise: `PWD=foo`
	// is a plain assignment any script may make, and a relative value stat'ed below
	// would be resolved against the very directory the question is about.
	if (path.empty() || path.front() != '/')
		return false;

	// No component that is dot or dot-dot. POSIX 2.5.3 describes PWD as an absolute
	// pathname containing neither, and a shell that adopts `/tmp/.` then prints it
	// from `pwd` is naming a directory whose real name it knows. This is a deliberate
	// divergence - dash, bash and ksh keep such a value, zsh replaces it as this does
	// - and it is the one rule here that a stat cannot express: `/tmp/.` and `/tmp`
	// share a device and an inode, so only the text can tell them apart.
	//
	// Dot-dot is the half that can mislead rather than merely look wrong. `cd -L`
	// canonicalizes `..` LEXICALLY against $PWD, so a dot-dot the shell did not put
	// there itself is a claim about the shape of the tree that nothing has checked.
	for (size_t at = 0; at <= path.size();) {
		const size_t slash = path.find('/', at);
		const std::string_view part = path.substr(
			at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
		if (part == "." || part == "..")
			return false;
		if (slash == std::string_view::npos)
			break;
		at = slash + 1;
	}

	// And it must NAME this directory. Device and inode rather than text: a logical
	// pathname legitimately differs from getcwd's physical answer wherever a symlink
	// is involved, which is the whole content of the -L/-P distinction #46 built.
	// `/tmp` and `/private/tmp` are one directory and these two numbers say so.
	struct stat named{};
	struct stat here{};
	const std::string terminated{path};   // stat needs a NUL, string_view has none
	if (::stat(terminated.c_str(), &named) != 0 || ::stat(".", &here) != 0)
		return false;
	return named.st_dev == here.st_dev && named.st_ino == here.st_ino;
}

std::string shell_state::physical_working_directory() {
	// PATH_MAX on the stack rather than getcwd's allocating GNU extension, which is
	// not portable, and rather than a growing loop, which would answer a question
	// nothing here asks: a pathname the kernel cannot fit in PATH_MAX is one no
	// operand could name either.
	char buffer[PATH_MAX];
	if (::getcwd(buffer, sizeof buffer) == nullptr)
		return {};
	return std::string{buffer};
}

std::string shell_state::logical_working_directory() const {
	if (std::string_view pwd; lookup("PWD", pwd) && names_current_directory(pwd))
		return std::string{pwd};
	return physical_working_directory();
}

int64_t shell_state::get(std::string_view name) const {
	std::string_view text;
	if (!lookup(name, text))
		return 0;  // unset is zero, not an error
	// THE SECOND PLACE A LITERAL IS BUILT, and it had the same undefined behaviour
	// arithmetic.cpp's parse_number did (#59): `value * 10 + digit` in an int64_t
	// overflows on any value past INT64_MAX, and `-value` overflows again at the
	// negative limit. It could not even represent INT64_MIN, whose magnitude 2^63
	// does not fit the signed accumulator it was being built in - so
	// `m=$((-9223372036854775807 - 1)); echo $((m))` could not round-trip.
	//
	// BOTH ANSWERS THIS SITE NEEDS COME BACK IN ONE FIELD (#63), and both are
	// deliberate rather than convenient:
	//
	//   - A NON-NUMBER IS ZERO. POSIX leaves it unspecified, and lesh answers 0
	//     rather than the `Illegal number` dash raises - which is what makes
	//     `i=$((i+1))` work without initialising i first. This is the one policy
	//     in the table that would be a defect at any other site, and it is why the
	//     shared parser reports WHICH WAY it failed instead of choosing for its
	//     callers.
	//   - A NUMBER TOO LARGE TO REPRESENT IS CLAMPED, which is what parse_number
	//     does with an over-large literal, so "too large" means one thing whether
	//     the digits were written in the expression or read out of a variable.
	//
	// numeric_result::value is zero for the first and the clamped bound for the
	// second, so neither needs a branch here.
	return parse_integer(text, numeric_site::variable_as_number).value;
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
	// Exported and UNSET, which is the whole of POSIX's "the export attribute is
	// set for the given name whether or not it is set". The entry exists to hold
	// the flag, exactly as mark_readonly's does, and `assigned` false is what keeps
	// it from being a variable: lookup() reports it absent, so `${x-unset}` and
	// `${x:-empty}` stay distinguishable, and environment_block() leaves it out, so
	// a child sees NOTHING rather than `x=`. Measured: `dash -c 'export A; env'`
	// prints no A line, and `sh -c 'echo ${A-unset}'` under it says unset (#71).
	_vars.emplace(std::string(name), variable{std::string{}, true, false, false});
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
		report("%.*s: is read only",
		       static_cast<int>(name.size()), name.data());
		return;
	}
	report("%.*s: %.*s: is read only",
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
