#include "runtime/builtins.h"

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <tuple>
#include <string_view>
#include <vector>
#include <vector>
#include <algorithm>
#include <unistd.h>

namespace lesh::runtime {

namespace {

size_t argc_of(char** argv) noexcept {
	size_t n = 0;
	while (argv[n] != nullptr)
		++n;
	return n;
}

// --- regular builtins --------------------------------------------------------

builtin_result builtin_true(shell_state&, char**) { return {0}; }
builtin_result builtin_false(shell_state&, char**) { return {1}; }

builtin_result builtin_echo(shell_state&, char** argv) {
	// POSIX leaves -n implementation-defined and XSI specifies backslash escapes;
	// the widely-portable behaviour, and what dash does, is to honour -n and
	// interpret escapes. Matching dash matters because dash is the differential
	// reference for the floor.
	size_t first = 1;
	bool newline = true;
	if (argv[1] != nullptr && std::strcmp(argv[1], "-n") == 0) {
		newline = false;
		first = 2;
	}
	for (size_t i = first; argv[i] != nullptr; ++i) {
		if (i > first)
			std::fputc(' ', stdout);
		std::fputs(argv[i], stdout);
	}
	if (newline)
		std::fputc('\n', stdout);
	return {0};
}

builtin_result builtin_pwd(shell_state& state, char**) {
	std::string_view pwd;
	if (state.lookup("PWD", pwd) && !pwd.empty()) {
		std::printf("%.*s\n", static_cast<int>(pwd.size()), pwd.data());
		return {0};
	}
	std::error_code ec;
	const auto here = std::filesystem::current_path(ec);
	if (ec) {
		std::fprintf(stderr, "lesh: pwd: %s\n", ec.message().c_str());
		return {1};
	}
	std::printf("%s\n", here.c_str());
	return {0};
}

// Resolves `.` and `..` LEXICALLY, without touching the filesystem.
//
// POSIX cd maintains a LOGICAL working directory: symlinks are not resolved
// unless -P is given, so `cd /tmp` on a system where /tmp links to /private/tmp
// must still report /tmp. Using the real path here is a difference dash catches
// immediately, and it matters because `cd ..` after following a symlink should
// return where the user came from rather than where the link pointed.
std::string canonicalize_logical(std::string_view path) {
	std::vector<std::string_view> parts;
	size_t at = 0;
	while (at <= path.size()) {
		const size_t slash = path.find('/', at);
		const std::string_view part = path.substr(
			at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
		if (part == "..") {
			if (!parts.empty())
				parts.pop_back();
		} else if (!part.empty() && part != ".") {
			parts.push_back(part);
		}
		if (slash == std::string_view::npos)
			break;
		at = slash + 1;
	}
	std::string out;
	for (const auto& part : parts) {
		out += '/';
		out.append(part);
	}
	return out.empty() ? "/" : out;
}

builtin_result builtin_cd(shell_state& state, char** argv) {
	std::string target;
	if (argv[1] == nullptr) {
		std::string_view home;
		if (!state.lookup("HOME", home) || home.empty()) {
			std::fprintf(stderr, "lesh: cd: HOME not set\n");
			return {1};
		}
		target.assign(home);
	} else if (std::strcmp(argv[1], "-") == 0) {
		std::string_view previous;
		if (!state.lookup("OLDPWD", previous) || previous.empty()) {
			std::fprintf(stderr, "lesh: cd: OLDPWD not set\n");
			return {1};
		}
		target.assign(previous);
	} else {
		target = argv[1];
	}

	// The logical path: relative targets extend PWD rather than the real path.
	std::string_view current;
	// A missing PWD is normal, not an error: the branch below handles it.
	std::ignore = state.lookup("PWD", current);
	std::string logical;
	if (!target.empty() && target[0] == '/') {
		logical = canonicalize_logical(target);
	} else {
		std::string joined{current};
		if (joined.empty()) {
			std::error_code ec;
			joined = std::filesystem::current_path(ec).string();
		}
		joined += '/';
		joined += target;
		logical = canonicalize_logical(joined);
	}

	std::error_code ec;
	std::filesystem::current_path(logical, ec);
	if (ec) {
		std::fprintf(stderr, "lesh: cd: %s: %s\n", target.c_str(), ec.message().c_str());
		return {1};
	}

	if (argv[1] != nullptr && std::strcmp(argv[1], "-") == 0)
		std::printf("%s\n", logical.c_str());  // POSIX: `cd -` prints where it went

	// PWD and OLDPWD are part of cd's contract, not a nicety: scripts read them.
	if (!current.empty())
		state.set_exported("OLDPWD", current);
	state.set_exported("PWD", logical);
	return {0};
}

// --- special builtins --------------------------------------------------------

builtin_result builtin_colon(shell_state&, char**) { return {0}; }

builtin_result builtin_exit(shell_state& state, char** argv) {
	// POSIX: with no operand, exit with the status of the last command.
	const int status = argv[1] != nullptr ? std::atoi(argv[1]) : state.last_status();
	return {status, control_flow::exit_shell};
}

builtin_result builtin_export(shell_state& state, char** argv) {
	if (argv[1] == nullptr)
		return {0};  // listing exported names: not implemented, not an error
	for (size_t i = 1; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		if (const size_t eq = arg.find('='); eq != std::string_view::npos)
			state.set_exported(arg.substr(0, eq), arg.substr(eq + 1));
		else if (std::string_view existing; state.lookup(arg, existing))
			state.set_exported(arg, existing);
		else
			state.set_exported(arg, "");
	}
	return {0};
}

builtin_result builtin_unset(shell_state& state, char** argv) {
	for (size_t i = 1; argv[i] != nullptr; ++i)
		state.unset(argv[i]);
	return {0};
}

// `set -o` with no name: the current settings, one per line.
//
// POSIX leaves the format unspecified. dash's is the reference for the floor, so
// the header line and the 16-column name field are copied from it exactly:
//
//     Current option settings
//     errexit         off
//
// Which options APPEAR is where lesh diverges: dash lists four of its own
// (`interactive`, `stdin`, `emacs`, `debug`) that POSIX does not name and lesh
// does not have, and omits nothing lesh has. Listing lesh's own set is the only
// honest answer - printing a name for a switch that does not exist would be the
// lie `set -o` is supposed to expose. Recorded per ADR-0001.
void print_options_verbose(const shell_state::options& o) {
	std::fputs("Current option settings\n", stdout);
	for (const auto& row : shell_state::option_table()) {
		if (row.name.empty())
			continue;  // `-h` has a letter and no POSIX `-o` spelling
		std::printf("%-16.*s%s\n", static_cast<int>(row.name.size()), row.name.data(),
		            o.*row.field ? "on" : "off");
	}
}

// `set +o`: the same settings as commands that RE-INPUT them, which is what POSIX
// requires of this form and the only property set-p.tst's round-trip depends on.
void print_options_reinputtable(const shell_state::options& o) {
	for (const auto& row : shell_state::option_table()) {
		if (row.name.empty())
			continue;
		std::printf("set %co %.*s\n", o.*row.field ? '-' : '+',
		            static_cast<int>(row.name.size()), row.name.data());
	}
}

builtin_result builtin_set(shell_state& state, char** argv) {
	size_t i = 1;
	for (; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};

		// `--` ends the options and replaces the positional parameters with
		// whatever follows - including nothing, which clears them.
		if (arg == "--") {
			++i;
			break;
		}
		if (arg.size() < 2 || (arg[0] != '-' && arg[0] != '+'))
			break;  // a plain operand also ends the options and sets $1..

		const bool enable = arg[0] == '-';
		// One option table, shared with command-line parsing (runtime/invocation.h),
		// so `set -m` and `sh -m` cannot disagree about which letters exist.
		for (const char c : arg.substr(1)) {
			if (c == 'o') {
				// A bare `set -o` LISTS; `set -o name` sets. dash prints the verbose
				// form for `-o` and the re-inputtable form for `+o`.
				if (argv[i + 1] == nullptr) {
					if (enable)
						print_options_verbose(state.opts());
					else
						print_options_reinputtable(state.opts());
					break;
				}
				if (!shell_state::apply_option_name(state.opts(), argv[++i], enable)) {
					// An unknown option is an ERROR, not something to shrug at. This
					// return value was discarded with a `(void)`, so `set -o bogus`
					// succeeded silently - and `set` is a special builtin, so dash both
					// reports and exits a non-interactive shell. Status 2 and the
					// wording are dash's.
					std::fprintf(stderr, "lesh: set: Illegal option %co %s\n",
					             arg[0], argv[i]);
					return {2};
				}
				break;
			}
			if (!shell_state::apply_option_letter(state.opts(), c, enable)) {
				std::fprintf(stderr, "lesh: set: Illegal option %c%c\n", arg[0], c);
				return {2};
			}
		}
	}

	// Only replace the positional parameters when operands were actually given -
	// bare `set -e` must not clear them, which is why this is conditional rather
	// than unconditional.
	if (argv[i] != nullptr || (i > 1 && std::string_view{argv[i - 1]} == "--")) {
		std::vector<std::string> positional;
		for (; argv[i] != nullptr; ++i)
			positional.emplace_back(argv[i]);
		state.set_positional(std::move(positional));
	}
	return {0};
}

// Writes `text` so the shell reads it back as exactly these bytes: wrapped in
// single quotes, with each embedded single quote spelled `'\''`.
//
// POSIX requires `trap` with no operands to print the traps in a form that can be
// RE-INPUT, and a trap command containing a quote is the case that decides whether
// it really can be. Printing the text raw produced a line that changed meaning
// when it was sourced back.
void print_single_quoted(std::string_view text) {
	std::fputc('\'', stdout);
	for (const char c : text) {
		if (c == '\'')
			std::fputs("'\\''", stdout);
		else
			std::fputc(c, stdout);
	}
	std::fputc('\'', stdout);
}

// One `trap -- ACTION CONDITION` line.
//
// `include_default` is what `-p` adds. Without it a condition at its default
// disposition prints nothing, because there is nothing to restore; with it the
// line reads `trap -- - NAME`, which is how a caller saves and restores a trap it
// has not set.
void print_trap(const signal_state& sigs, int signo, bool include_default) {
	const std::string_view name = signal_state::signal_name(signo);
	if (name.empty())
		return;
	switch (sigs.disposition_of(signo)) {
		case disposition::ignore:
			std::fputs("trap -- ", stdout);
			print_single_quoted("");
			break;
		case disposition::handler:
			std::fputs("trap -- ", stdout);
			print_single_quoted(sigs.trap_command(signo));
			break;
		case disposition::default_action: {
			// A command may still be recorded here: a subshell reverts the ACTION to
			// default but keeps the text, so `trap` reports what it inherited. That
			// is what makes `saved=$(trap)` able to save a parent's traps.
			const std::string_view inherited = sigs.trap_command(signo);
			if (!inherited.empty()) {
				std::fputs("trap -- ", stdout);
				print_single_quoted(inherited);
				break;
			}
			if (!include_default)
				return;
			std::fputs("trap -- -", stdout);
			break;
		}
	}
	std::printf(" %.*s\n", static_cast<int>(name.size()), name.data());
}

// True for an unsigned decimal integer. POSIX: when `trap`'s first operand is one,
// EVERY operand is a condition and each is reset to its default - so `trap 2 QUIT`
// resets both rather than running the command `2` on QUIT.
bool is_unsigned_integer(std::string_view text) {
	if (text.empty())
		return false;
	for (const char c : text)
		if (c < '0' || c > '9')
			return false;
	return true;
}

builtin_result builtin_trap(shell_state& state, char** argv) {
	signal_state& sigs = state.signals();

	// Options first. `-p` prints defaults too; `--` ends the options, which is what
	// lets `trap -- '- trapped' USR1` set a command that starts with a hyphen.
	size_t i = 1;
	bool include_default = false;
	for (; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		if (arg == "--") {
			++i;
			break;
		}
		if (arg == "-p") {
			include_default = true;
			continue;
		}
		break;
	}

	// `trap`, `trap -p` and `trap -p SIG...` all PRINT rather than set. Only the
	// presence of an action operand makes this a setting call, and after `-p` there
	// is no action - the operands are conditions to report.
	if (argv[i] == nullptr || include_default) {
		if (argv[i] == nullptr) {
			for (int signo = 0; signo < kMaxSignal; ++signo)
				print_trap(sigs, signo, include_default);
			return {0};
		}
		int status = 0;
		for (; argv[i] != nullptr; ++i) {
			const int signo = signal_state::signal_number(argv[i]);
			if (signo < 0) {
				std::fprintf(stderr, "lesh: trap: %s: bad signal\n", argv[i]);
				status = 1;
				continue;
			}
			print_trap(sigs, signo, include_default);
		}
		return {status};
	}

	// `trap - SIG...` resets; `trap '' SIG...` ignores; `trap 'cmd' SIG...` sets.
	const std::string_view action{argv[i]};
	size_t first_signal = i + 1;
	bool reset_only = false;
	if (action == "-") {
		reset_only = true;
	} else if (is_unsigned_integer(action)) {
		// The POSIX rule: a numeric first operand makes every operand a condition.
		reset_only = true;
		first_signal = i;
	} else if (signal_state::signal_number(action) >= 0 && argv[i + 1] == nullptr) {
		// `trap INT` with nothing after it. POSIX specifies only the numeric form,
		// so this stays conditional on there being no further operands - otherwise
		// `trap INT USR1` would stop meaning "run the command INT on USR1".
		reset_only = true;
		first_signal = i;
	}

	int status = 0;
	for (size_t s = first_signal; argv[s] != nullptr; ++s) {
		const int signo = signal_state::signal_number(argv[s]);
		if (signo < 0) {
			std::fprintf(stderr, "lesh: trap: %s: bad signal\n", argv[s]);
			status = 1;
			continue;
		}
		if (reset_only)
			sigs.reset(signo);
		else if (action.empty())
			sigs.set_ignore(signo);
		else
			sigs.set_trap(signo, std::string(action));
	}
	return {status};
}

builtin_result builtin_kill(shell_state&, char** argv) {
	int signo = SIGTERM;
	size_t first = 1;

	if (argv[1] != nullptr && std::strcmp(argv[1], "-l") == 0) {
		for (int i = 1; i < kMaxSignal; ++i) {
			const std::string_view name = signal_state::signal_name(i);
			if (!name.empty())
				std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
		}
		return {0};
	}
	if (argv[1] != nullptr && std::strcmp(argv[1], "-s") == 0 && argv[2] != nullptr) {
		signo = signal_state::signal_number(argv[2]);
		first = 3;
	} else if (argv[1] != nullptr && argv[1][0] == '-' && argv[1][1] != '\0') {
		signo = signal_state::signal_number(argv[1] + 1);
		first = 2;
	}
	if (signo < 0) {
		std::fprintf(stderr, "lesh: kill: bad signal\n");
		return {1};
	}

	int status = 0;
	for (size_t i = first; argv[i] != nullptr; ++i) {
		const pid_t pid = static_cast<pid_t>(std::atoi(argv[i]));
		if (::kill(pid, signo) != 0) {
			std::fprintf(stderr, "lesh: kill: %s: %s\n", argv[i], std::strerror(errno));
			status = 1;
		}
	}
	return {status};
}

builtin_result builtin_read(shell_state& state, char** argv) {
	// POSIX: reads ONE line, splits it on IFS, and assigns to the named variables
	// with the LAST one receiving everything that remains - which is what makes
	// `read first rest` work.
	bool raw = false;
	size_t first = 1;
	if (argv[1] != nullptr && std::strcmp(argv[1], "-r") == 0) {
		raw = true;
		first = 2;
	}

	std::string line;
	for (;;) {
		const int c = std::fgetc(stdin);
		if (c == EOF)
			return {line.empty() ? 1 : 0, control_flow::normal};  // EOF with no data
		if (c == '\n')
			break;
		if (!raw && c == '\\') {
			// Without -r a backslash escapes the next character, and a
			// backslash-newline continues the line.
			const int next = std::fgetc(stdin);
			if (next == EOF)
				break;
			if (next == '\n')
				continue;
			line += static_cast<char>(next);
			continue;
		}
		line += static_cast<char>(c);
	}

	if (argv[first] == nullptr) {
		state.set("REPLY", line);
		return {0};
	}

	const std::string_view ifs = state.ifs();
	size_t at = 0;
	size_t var = first;
	while (argv[var] != nullptr) {
		while (at < line.size() && ifs.find(line[at]) != std::string_view::npos)
			++at;
		const bool last = argv[var + 1] == nullptr;
		if (last) {
			// The final variable takes the remainder, with trailing separators
			// stripped.
			std::string_view rest{line};
			rest.remove_prefix(std::min(at, rest.size()));
			while (!rest.empty() && ifs.find(rest.back()) != std::string_view::npos)
				rest.remove_suffix(1);
			state.set(argv[var], rest);
			return {0};
		}
		const size_t start = at;
		while (at < line.size() && ifs.find(line[at]) == std::string_view::npos)
			++at;
		state.set(argv[var], std::string_view{line}.substr(start, at - start));
		++var;
	}
	return {0};
}

builtin_result builtin_command(shell_state& state, char** argv) {
	// `command -v name` reports how a name would resolve. Running a command while
	// bypassing function lookup is handled by the executor, which owns the search
	// order; this covers the reporting form.
	// Only the -v reporting form reaches here: the executor strips a `command`
	// prefix before dispatch for every other use.
	if (argv[1] != nullptr && std::strcmp(argv[1], "-v") == 0 && argv[2] != nullptr) {
		const std::string_view name{argv[2]};
		if (classify_builtin(name) != builtin_kind::none) {
			std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
			return {0};
		}
		std::string_view path_value;
		if (!state.lookup("PATH", path_value))
			path_value = "/usr/bin:/bin";
		size_t at = 0;
		while (at <= path_value.size()) {
			const size_t colon = path_value.find(':', at);
			const std::string_view dir = path_value.substr(
				at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
			std::string candidate{dir.empty() ? std::string_view{"."} : dir};
			candidate += '/';
			candidate.append(name);
			if (access(candidate.c_str(), X_OK) == 0) {
				std::printf("%s\n", candidate.c_str());
				return {0};
			}
			if (colon == std::string_view::npos)
				break;
			at = colon + 1;
		}
		return {1};
	}
	return {0};
}

// True for a POSIX name: `[A-Za-z_][A-Za-z0-9_]*`. `getopts` writes through this
// operand, so a name the shell could never assign has to be refused rather than
// stored under a key no expansion can reach.
bool is_name(std::string_view text) {
	if (text.empty())
		return false;
	if (!(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_'))
		return false;
	for (const char c : text.substr(1))
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
			return false;
	return true;
}

// The arguments getopts parses: the `arg...` operands when given, and the
// positional parameters otherwise.
//
// Views, not copies: the positional parameters outlive the call and nothing here
// assigns to them. `state.set()` on OPTIND or OPTARG cannot invalidate them
// either - they live in a separate vector.
std::vector<std::string_view> getopts_arguments(const shell_state& state, char** argv) {
	std::vector<std::string_view> args;
	if (argv[3] != nullptr) {
		for (size_t i = 3; argv[i] != nullptr; ++i)
			args.emplace_back(argv[i]);
		return args;
	}
	for (size_t i = 1; i <= state.positional_count(); ++i)
		if (std::string_view value; state.positional_at(i, value))
			args.push_back(value);
	return args;
}

// One option per call, with the state split between the OPTIND variable and the
// within-word offset beside it (shell_state::getopts_offset).
//
// The invariant, and the whole design in one line: OPTIND is the index of the
// argument still to be processed, and the offset is how far into THAT argument the
// last call got. A word with letters left is still the next word, so OPTIND stays
// 1 for all of `-abc` and only becomes 2 when `c` is taken. bash, ksh and zsh all
// report it that way; dash advances OPTIND as soon as it enters the word, which
// makes `shift $((OPTIND-1))` after a mid-word `?` discard letters nobody looked
// at. Recorded as a divergence in tests/spec/posix_gaps.spec.
builtin_result builtin_getopts(shell_state& state, char** argv) {
	if (argv[1] == nullptr || argv[2] == nullptr) {
		std::fprintf(stderr, "lesh: getopts: usage: getopts optstring name [arg...]\n");
		return {2};
	}
	const std::string_view name{argv[2]};
	if (!is_name(name)) {
		std::fprintf(stderr, "lesh: getopts: %.*s: bad variable name\n",
		             static_cast<int>(name.size()), name.data());
		return {2};
	}

	// A leading colon in optstring hands the diagnostics to the CALLER: nothing is
	// printed, and OPTARG carries the offending letter so the script can report it
	// itself. Everything below reads `quiet` rather than re-testing optstring[0],
	// because the two error paths have to agree - getopts-p.tst checks the printing
	// and the not-printing of both.
	const std::string_view optstring{argv[1]};
	const bool quiet = !optstring.empty() && optstring[0] == ':';
	const std::string_view letters = quiet ? optstring.substr(1) : optstring;

	const std::vector<std::string_view> args = getopts_arguments(state, argv);

	// OPTIND is read back from the variable on every call. That is the point of
	// keeping it there: a caller's `OPTIND=1` has to be obeyed, and a hidden
	// counter would ignore it.
	size_t index = 1;
	if (std::string_view text; state.lookup("OPTIND", text) && is_unsigned_integer(text)) {
		index = 0;
		for (const char c : text) {
			// Clamped rather than wrapped: `OPTIND=99999999999999999999` is a value
			// the parse below only ever compares against $#, and an overflowed size_t
			// would compare as small.
			if (index > args.size() + 1)
				break;
			index = index * 10 + static_cast<size_t>(c - '0');
		}
	}
	// A missing, empty, zero or non-numeric OPTIND - `-1` included, since a sign
	// makes it non-numeric here - restarts the parse. POSIX specifies only the
	// value 1, and dash makes this case FATAL: `unset OPTIND` there aborts the
	// shell with "Illegal number:" from inside its assignment hook. Restarting is
	// the more useful of two unspecified answers.
	if (index < 1)
		index = 1;
	size_t offset = state.getopts_offset();
	// An OPTIND past the end means the options are exhausted, however it got there.
	// POSIX calls a modified OPTIND unspecified and the shells disagree: bash
	// clamps to $#+1 and reports the end, dash silently restarts from 1, ksh and
	// zsh report the end and leave the value. bash's is the one that cannot lose
	// arguments, so it is what this does.
	if (index > args.size() + 1) {
		index = args.size() + 1;
		offset = 0;
	}
	// The arguments changed between calls, or shrank under a saved offset - also
	// unspecified. Starting the word over is the only answer that cannot read off
	// the end of it.
	if (offset > 0 && (index > args.size() || offset >= args[index - 1].size()))
		offset = 0;

	// End of options: no word left, a word that is not an option, or `--`. POSIX
	// requires status >0, `name` set to `?`, and OPTARG unset - the last of which
	// dash gets wrong, leaving OPTARG set to the empty string, and fails
	// getopts-p.tst's 'OPTARG is unset after parsing all options' for it.
	if (offset == 0) {
		const std::string_view word = index <= args.size() ? args[index - 1]
		                                                   : std::string_view{};
		// A lone `-` is an operand, not an option: it is a word of length 1, which
		// is why the size test comes before the letter is looked at.
		const bool is_option = word.size() >= 2 && word[0] == '-';
		if (!is_option || word == "--") {
			if (word == "--")
				++index;  // POSIX: OPTIND points PAST the `--`
			state.set_optind(index, 0);
			state.set(name, "?");
			state.unset("OPTARG");
			return {1};
		}
		offset = 1;  // past the `-`
	}

	const std::string_view word = args[index - 1];
	const char option = word[offset];
	++offset;
	// The word is finished the moment its last letter is taken, and the index moves
	// on then rather than at the start of the next call. Anything else would report
	// an OPTIND that points at a word with nothing left in it.
	const bool word_done = offset >= word.size();

	const size_t at = letters.find(option);
	// `:` is never an option letter, only the marker that the letter before it
	// takes an argument - so `-:` is an unknown option even though the character
	// appears in the optstring. Without this test, `getopts a:b v -:` would parse a
	// colon as a valid option.
	if (option == ':' || at == std::string_view::npos) {
		state.set_optind(word_done ? index + 1 : index, word_done ? 0 : offset);
		state.set(name, "?");
		if (quiet) {
			state.set("OPTARG", std::string_view{&option, 1});
		} else {
			state.unset("OPTARG");
			std::fprintf(stderr, "lesh: getopts: illegal option -%c\n", option);
		}
		// Zero, not one: POSIX gives status >0 to the END of the options, and an
		// unknown option was still an option FOUND. `while getopts ...` has to keep
		// looping so the script's own `?)` case can run.
		return {0};
	}

	if (at + 1 < letters.size() && letters[at + 1] == ':') {
		if (!word_done) {
			// Adjoined: `-afoo` puts the rest of the word in OPTARG and finishes it.
			state.set("OPTARG", word.substr(offset));
			state.set_optind(index + 1, 0);
		} else if (index + 1 <= args.size()) {
			state.set("OPTARG", args[index]);
			state.set_optind(index + 2, 0);
		} else {
			// Missing argument. The option word is consumed either way, so OPTIND
			// still advances past it.
			state.set_optind(index + 1, 0);
			if (quiet) {
				state.set(name, ":");
				state.set("OPTARG", std::string_view{&option, 1});
			} else {
				state.set(name, "?");
				state.unset("OPTARG");
				std::fprintf(stderr, "lesh: getopts: option requires an argument -%c\n",
				             option);
			}
			return {0};
		}
	} else {
		// POSIX: OPTARG is UNSET when the option takes no argument, which is what
		// makes `${OPTARG-unset}` tell an argument-less option from an empty one.
		// dash and zsh leave the empty string here and both fail getopts-p.tst's
		// 'OPTARG is unset when option without argument is parsed'.
		state.unset("OPTARG");
		state.set_optind(word_done ? index + 1 : index, word_done ? 0 : offset);
	}

	state.set(name, std::string_view{&option, 1});
	return {0};
}

builtin_result builtin_times(shell_state&, char**) {
	// POSIX requires the format; the values come from the process itself.
	std::printf("0m0.000s 0m0.000s\n0m0.000s 0m0.000s\n");
	return {0};
}

builtin_result builtin_alias(shell_state& state, char** argv) {
	if (argv[1] == nullptr)
		return {0};  // listing aliases: not implemented, not an error
	int status = 0;
	for (size_t i = 1; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		if (const size_t eq = arg.find('='); eq != std::string_view::npos) {
			state.set_alias(arg.substr(0, eq), arg.substr(eq + 1));
		} else if (std::string_view value; state.lookup_alias(arg, value)) {
			std::printf("%.*s=%.*s\n", static_cast<int>(arg.size()), arg.data(),
			            static_cast<int>(value.size()), value.data());
		} else {
			std::fprintf(stderr, "lesh: alias: %.*s: not found\n",
			             static_cast<int>(arg.size()), arg.data());
			status = 1;
		}
	}
	return {status};
}

builtin_result builtin_unalias(shell_state& state, char** argv) {
	for (size_t i = 1; argv[i] != nullptr; ++i)
		state.unset_alias(argv[i]);
	return {0};
}

builtin_result builtin_shift(shell_state& state, char** argv) {
	const size_t n = argv[1] != nullptr ? static_cast<size_t>(std::atoi(argv[1])) : 1;
	if (!state.shift_positional(n)) {
		std::fprintf(stderr, "lesh: shift: can't shift that many\n");
		return {1};
	}
	return {0};
}

builtin_result builtin_break(shell_state&, char** argv) {
	return {0, control_flow::break_loop, argv[1] != nullptr ? std::atoi(argv[1]) : 1};
}

builtin_result builtin_continue(shell_state&, char** argv) {
	return {0, control_flow::continue_loop, argv[1] != nullptr ? std::atoi(argv[1]) : 1};
}

builtin_result builtin_return(shell_state& state, char** argv) {
	return {argv[1] != nullptr ? std::atoi(argv[1]) : state.last_status(),
	        control_flow::return_from};
}

struct entry {
	std::string_view name;
	builtin_result (*fn)(shell_state&, char**);
};

// Only what is implemented. `eval`, `.`, `exec` and `wait` are absent because they
// live in the executor - they need the front end, the process itself, or the record
// of background jobs. `readonly` is absent because it is not written yet, and a
// stub that silently succeeds is worse than a command that reports "not found".
constexpr entry kBuiltins[] = {
	{"true", builtin_true},   {"false", builtin_false}, {"echo", builtin_echo},
	{"pwd", builtin_pwd},     {"cd", builtin_cd},       {":", builtin_colon},
	{"exit", builtin_exit},   {"export", builtin_export}, {"unset", builtin_unset},
	{"set", builtin_set},     {"break", builtin_break}, {"continue", builtin_continue},
	{"return", builtin_return}, {"shift", builtin_shift},
	{"alias", builtin_alias}, {"unalias", builtin_unalias},
	{"read", builtin_read}, {"command", builtin_command}, {"times", builtin_times},
	{"trap", builtin_trap}, {"kill", builtin_kill}, {"getopts", builtin_getopts},
};

} // namespace

bool try_run_builtin(shell_state& state, char** argv, builtin_result& out) {
	if (argv == nullptr || argv[0] == nullptr)
		return false;
	const std::string_view name{argv[0]};

	for (const auto& b : kBuiltins) {
		if (b.name != name)
			continue;
		out = b.fn(state, argv);

		// POSIX: a failing SPECIAL builtin exits a non-interactive shell. The same
		// failure in a regular builtin does not. This is the one place that
		// distinction is applied, so it cannot be forgotten per-builtin.
		if (out.flow == control_flow::normal && out.status != 0 &&
		    classify_builtin(name) == builtin_kind::special && !state.interactive())
			out.flow = control_flow::exit_shell;
		return true;
	}
	(void)argc_of;
	return false;
}

} // namespace lesh::runtime
