#include "runtime/builtins.h"

#include <cerrno>
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
		for (const char c : arg.substr(1)) {
			switch (c) {
				case 'e': state.opts().exit_on_error = enable; break;
				case 'u': state.opts().error_on_unset = enable; break;
				case 'x': state.opts().trace = enable; break;
				case 'f': state.opts().no_glob = enable; break;
				default: break;
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

// Only what is implemented. `eval`, `.`, `exec`, `trap`, `times` and `readonly`
// are deliberately absent: each needs machinery from another ticket, and a stub
// that silently succeeds is worse than a command that reports "not found".
constexpr entry kBuiltins[] = {
	{"true", builtin_true},   {"false", builtin_false}, {"echo", builtin_echo},
	{"pwd", builtin_pwd},     {"cd", builtin_cd},       {":", builtin_colon},
	{"exit", builtin_exit},   {"export", builtin_export}, {"unset", builtin_unset},
	{"set", builtin_set},     {"break", builtin_break}, {"continue", builtin_continue},
	{"return", builtin_return}, {"shift", builtin_shift},
	{"alias", builtin_alias}, {"unalias", builtin_unalias},
	{"read", builtin_read}, {"command", builtin_command}, {"times", builtin_times},
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
