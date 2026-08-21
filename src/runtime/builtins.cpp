#include "runtime/builtins.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
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
#include <sys/stat.h>
#include <unistd.h>

namespace lesh::runtime {

namespace {

size_t argc_of(char** argv) noexcept {
	size_t n = 0;
	while (argv[n] != nullptr)
		++n;
	return n;
}

// Both are defined further down, beside the builtin that first needed them: the
// quoting grew with `trap`'s re-inputtable listing and the name test with
// `getopts`. `export` and `readonly` need both, and they come first in the file.
void print_single_quoted(std::string_view text);
bool is_name(std::string_view text);

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
	// A readonly PWD makes cd FAIL after the directory has already changed, which
	// is what dash does too - the chdir is not undone, but the shell says that the
	// variable no longer describes where it is.
	int status = 0;
	if (!current.empty() && !state.set_exported("OLDPWD", current)) {
		shell_state::report_readonly("cd", "OLDPWD");
		status = 2;
	}
	if (!state.set_exported("PWD", logical)) {
		shell_state::report_readonly("cd", "PWD");
		status = 2;
	}
	return {status};
}

// --- test and [ --------------------------------------------------------------
//
// POSIX gives `test` a table of primaries AND, before it, explicit rules for
// argument counts of 0 through 4. The COUNT RULES COME FIRST, and that ordering is
// the whole difficulty of this utility: `test "$x"` with x set to `-n` is a
// one-argument test of a non-empty string, not a `-n` missing its operand, and an
// implementation that matches operators before counting arguments gets it wrong.
// Everything below is written in that order for that reason.

// What went wrong, so the diagnostic can name the operand. dash's wording, since
// dash is the differential reference for the floor.
struct test_failure {
	const char* message = nullptr;   // nullptr while nothing has failed
	std::string_view token;          // the operand to name, when there is one
};

bool is_test_unary(std::string_view op) {
	// POSIX's unary primaries, plus `-G`, `-O` and `-k`, which dash also accepts.
	// `-N` is deliberately absent: dash rejects it, and accepting it here would
	// make `test -N f` differ from the reference for no standard's sake.
	static constexpr std::string_view ops[] = {
		"-b", "-c", "-d", "-e", "-f", "-g", "-h", "-k", "-L", "-n",
		"-O", "-G", "-p", "-r", "-S", "-s", "-t", "-u", "-w", "-x", "-z",
	};
	for (const auto& candidate : ops)
		if (candidate == op)
			return true;
	return false;
}

bool is_test_binary(std::string_view op) {
	// `<` and `>` are not POSIX primaries; dash has them, and a shell script that
	// reaches the builtin with them would otherwise get "unexpected operator" where
	// the reference answers.
	static constexpr std::string_view ops[] = {
		"=", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
		"-nt", "-ot", "-ef",
	};
	for (const auto& candidate : ops)
		if (candidate == op)
			return true;
	return false;
}

// dash's `getn`: optional sign, decimal digits, blanks either side, and nothing
// else. Overflow is an error rather than a wrap, because `test 99999999999999999999
// -eq 1` must report `Illegal number` rather than compare a truncated value.
bool test_integer(std::string_view text, int64_t& out) {
	size_t at = 0;
	while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n'))
		++at;
	bool negative = false;
	if (at < text.size() && (text[at] == '-' || text[at] == '+'))
		negative = text[at++] == '-';
	size_t digits = 0;
	uint64_t magnitude = 0;
	for (; at < text.size() && text[at] >= '0' && text[at] <= '9'; ++at, ++digits) {
		if (magnitude > (UINT64_MAX - 9) / 10)
			return false;
		magnitude = magnitude * 10 + static_cast<uint64_t>(text[at] - '0');
	}
	if (digits == 0)
		return false;
	while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n'))
		++at;
	if (at != text.size())
		return false;
	const uint64_t limit = negative ? uint64_t{1} << 63
	                                : static_cast<uint64_t>(INT64_MAX);
	if (magnitude > limit)
		return false;
	out = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
	return true;
}

bool test_unary(std::string_view op, const char* operand, test_failure& fail) {
	const std::string_view text{operand};
	if (op == "-n")
		return !text.empty();
	if (op == "-z")
		return text.empty();
	if (op == "-t") {
		int64_t fd = 0;
		if (!test_integer(text, fd)) {
			fail = {"Illegal number", text};
			return false;
		}
		return isatty(static_cast<int>(fd)) == 1;
	}
	// -h and -L ask about the LINK, so they must not follow it; everything else
	// follows, which is why there are two stat calls rather than one.
	struct stat info {};
	if (op == "-h" || op == "-L")
		return lstat(operand, &info) == 0 && S_ISLNK(info.st_mode);
	if (op == "-r")
		return access(operand, R_OK) == 0;
	if (op == "-w")
		return access(operand, W_OK) == 0;
	if (op == "-x")
		return access(operand, X_OK) == 0;
	if (stat(operand, &info) != 0)
		return false;
	if (op == "-e")
		return true;
	if (op == "-b")
		return S_ISBLK(info.st_mode);
	if (op == "-c")
		return S_ISCHR(info.st_mode);
	if (op == "-d")
		return S_ISDIR(info.st_mode);
	if (op == "-f")
		return S_ISREG(info.st_mode);
	if (op == "-p")
		return S_ISFIFO(info.st_mode);
	if (op == "-S")
		return S_ISSOCK(info.st_mode);
	if (op == "-s")
		return info.st_size > 0;
	if (op == "-g")
		return (info.st_mode & S_ISGID) != 0;
	if (op == "-u")
		return (info.st_mode & S_ISUID) != 0;
	if (op == "-k")
		return (info.st_mode & S_ISVTX) != 0;
	if (op == "-O")
		return info.st_uid == geteuid();
	if (op == "-G")
		return info.st_gid == getegid();
	fail = {"unexpected operator", op};
	return false;
}

bool test_binary(const char* left, std::string_view op, const char* right,
                 test_failure& fail) {
	const std::string_view a{left};
	const std::string_view b{right};
	if (op == "=")
		return a == b;
	if (op == "!=")
		return a != b;
	if (op == "<")
		return a < b;
	if (op == ">")
		return a > b;
	if (op == "-nt" || op == "-ot" || op == "-ef") {
		struct stat first {};
		struct stat second {};
		if (stat(left, &first) != 0 || stat(right, &second) != 0)
			return false;
		if (op == "-ef")
			return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
		if (op == "-nt")
			return first.st_mtime > second.st_mtime;
		return first.st_mtime < second.st_mtime;
	}
	int64_t x = 0;
	int64_t y = 0;
	if (!test_integer(a, x)) {
		fail = {"Illegal number", a};
		return false;
	}
	if (!test_integer(b, y)) {
		fail = {"Illegal number", b};
		return false;
	}
	if (op == "-eq") return x == y;
	if (op == "-ne") return x != y;
	if (op == "-lt") return x < y;
	if (op == "-le") return x <= y;
	if (op == "-gt") return x > y;
	return x >= y;  // -ge, the only one left
}

// The POSIX count rules for one and two arguments, used by the three- and
// four-argument rules too - which is why they are functions rather than inline
// branches: `test ! ! ""` is the two-argument rule applied inside the three.
bool test_one(const char* arg) { return std::string_view{arg}.size() != 0; }

bool test_two(char** args, test_failure& fail) {
	const std::string_view op{args[0]};
	if (op == "!")
		return !test_one(args[1]);
	if (is_test_unary(op))
		return test_unary(op, args[1], fail);
	fail = {"unexpected operator", op};
	return false;
}

// The expression grammar, for more than four arguments and for the counts POSIX
// leaves unspecified. Only reached after the count rules have had their say.
//
//   expression := and ( -o and )*
//   and        := negation ( -a negation )*
//   negation   := `!` negation | primary
//   primary    := `(` expression `)` | unary operand | operand binary operand
//                | operand
struct test_parser {
	char** args = nullptr;
	size_t count = 0;
	size_t at = 0;
	test_failure fail;

	[[nodiscard]] std::string_view peek() const {
		return at < count ? std::string_view{args[at]} : std::string_view{};
	}
	[[nodiscard]] bool done() const { return at >= count; }

	bool parse_expression() {
		bool value = parse_and();
		while (!fail.message && peek() == "-o") {
			++at;
			// Not short-circuited: `test x -o BADOP y` must still report the operator,
			// and POSIX gives `test` no lazy evaluation to hide an error behind.
			const bool right = parse_and();
			value = value || right;
		}
		return value;
	}

	bool parse_and() {
		bool value = parse_negation();
		while (!fail.message && peek() == "-a") {
			++at;
			const bool right = parse_negation();
			value = value && right;
		}
		return value;
	}

	bool parse_negation() {
		if (peek() == "!") {
			++at;
			return !parse_negation();
		}
		return parse_primary();
	}

	bool parse_primary() {
		if (done()) {
			fail = {"argument expected", {}};
			return false;
		}
		if (peek() == "(") {
			++at;
			const bool value = parse_expression();
			if (!fail.message && peek() != ")") {
				fail = {"closing paren expected", {}};
				return false;
			}
			++at;
			return value;
		}
		// A unary primary only when an operand follows it. Without that test, a
		// trailing `-n` would read past the end of the arguments.
		if (is_test_unary(peek()) && at + 1 < count) {
			const std::string_view op = peek();
			at += 2;
			return test_unary(op, args[at - 1], fail);
		}
		char* const left = args[at++];
		if (!done() && is_test_binary(peek()) && at + 1 < count) {
			const std::string_view op = peek();
			at += 2;
			return test_binary(left, op, args[at - 1], fail);
		}
		// A bare operand is true when it is not the empty string. An operator here
		// is a syntax error rather than a string, which is what tells
		// `test 1 = 1 = 1` from `test = = =`.
		if (!done() && (is_test_binary(peek()) || is_test_unary(peek()))) {
			fail = {"unexpected operator", std::string_view{left}};
			return false;
		}
		return test_one(left);
	}
};

// The three-argument rule, which the four-argument rule applies inside itself:
// `test ! a = b` is `!` in front of the three-argument case, so this has to be one
// function rather than a branch.
bool test_three(char** args, test_failure& fail) {
	if (is_test_binary(args[1]))
		return test_binary(args[0], args[1], args[2], fail);
	if (std::string_view{args[0]} == "!")
		return !test_two(args + 1, fail);
	if (std::string_view{args[0]} == "(" && std::string_view{args[2]} == ")")
		return test_one(args[1]);
	// POSIX calls the rest of this case unspecified. The expression grammar is the
	// answer that agrees with dash on `test -a -a -a` and reports `test x bogus y`
	// rather than guessing.
	test_parser parser{args, 3, 0, {}};
	const bool value = parser.parse_expression();
	fail = parser.fail;
	if (!fail.message && !parser.done())
		fail = {"unexpected operator", parser.peek()};
	return value;
}

// `test`, and `[` with its closing bracket already removed.
//
// Status is 0 for true, 1 for false, and 2 for an ERROR - which is the difference
// this builtin exists to make: an unimplemented `test` returned 0, so
// `test 1 = 2` was indistinguishable from success (#35).
builtin_result run_test(std::string_view invoked_as, char** args, size_t count) {
	test_failure fail;
	bool value = false;
	switch (count) {
		// POSIX: with no arguments, `test` is FALSE. Not an error.
		case 0: value = false; break;
		case 1: value = test_one(args[0]); break;
		case 2: value = test_two(args, fail); break;
		case 3: value = test_three(args, fail); break;
		case 4:
			if (std::string_view{args[0]} == "!") {
				value = !test_three(args + 1, fail);
			} else if (std::string_view{args[0]} == "(" &&
			           std::string_view{args[3]} == ")") {
				value = test_two(args + 1, fail);
			} else {
				test_parser parser{args, count, 0, {}};
				value = parser.parse_expression();
				fail = parser.fail;
				if (!fail.message && !parser.done())
					fail = {"unexpected operator", parser.peek()};
			}
			break;
		default: {
			test_parser parser{args, count, 0, {}};
			value = parser.parse_expression();
			fail = parser.fail;
			// Arguments left over are an error, not something to ignore: without this
			// `test 1 = 1 junk` would report the truth of its first three operands.
			if (!fail.message && !parser.done())
				fail = {"unexpected operator", parser.peek()};
			break;
		}
	}

	if (fail.message != nullptr) {
		if (fail.token.empty())
			std::fprintf(stderr, "lesh: %.*s: %s\n",
			             static_cast<int>(invoked_as.size()), invoked_as.data(),
			             fail.message);
		else
			std::fprintf(stderr, "lesh: %.*s: %.*s: %s\n",
			             static_cast<int>(invoked_as.size()), invoked_as.data(),
			             static_cast<int>(fail.token.size()), fail.token.data(),
			             fail.message);
		return {2};
	}
	return {value ? 0 : 1};
}

builtin_result builtin_test(shell_state&, char** argv) {
	const std::string_view name{argv[0]};
	size_t count = argc_of(argv) - 1;
	// `[` is the same utility and must require its closing bracket, which is the
	// only difference between the two spellings. Anything after the `]` is an error
	// too, so the check is on the LAST argument rather than a search.
	if (name == "[") {
		if (count == 0 || std::string_view{argv[count]} != "]") {
			std::fprintf(stderr, "lesh: [: missing ]\n");
			return {2};
		}
		--count;
	}
	return run_test(name, argv + 1, count);
}

// --- special builtins --------------------------------------------------------

builtin_result builtin_colon(shell_state&, char**) { return {0}; }

builtin_result builtin_exit(shell_state& state, char** argv) {
	// POSIX: with no operand, exit with the status of the last command.
	const int status = argv[1] != nullptr ? std::atoi(argv[1]) : state.last_status();
	return {status, control_flow::exit_shell};
}

// One `export NAME='VALUE'` or `readonly NAME='VALUE'` line, quoted so the shell
// reads it back as exactly these bytes.
//
// POSIX requires the no-operand form of both builtins to print in a form that can
// be RE-INPUT, which is what export-p.tst's `e="$(export -p)"; eval "$e"` round
// trip checks. A name that is readonly but UNSET prints bare - `readonly x` - and
// printing `x=''` for it would create the variable on re-input.
void print_declaration(std::string_view keyword, const shell_state::variable_row& row) {
	std::printf("%.*s %.*s", static_cast<int>(keyword.size()), keyword.data(),
	            static_cast<int>(row.name.size()), row.name.data());
	if (row.assigned) {
		std::fputc('=', stdout);
		print_single_quoted(row.value);
	}
	std::fputc('\n', stdout);
}

// `export` and `readonly` differ in one flag and one predicate, so they share a
// body: writing them twice is how the two would come to disagree about `--`, about
// a bad name, or about what a refused assignment costs.
builtin_result run_declaration(shell_state& state, char** argv, bool make_readonly) {
	const std::string_view keyword{argv[0]};
	size_t i = 1;
	bool print_only = false;
	for (; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		// `--` ends the options: readonly-p.tst's 'separator preceding operand' is
		// `readonly -- a=foo`, which without this assigned to a name of `--`.
		if (arg == "--") {
			++i;
			break;
		}
		if (arg == "-p") {
			print_only = true;
			continue;
		}
		if (arg.size() >= 2 && arg[0] == '-') {
			std::fprintf(stderr, "lesh: %.*s: Illegal option %.*s\n",
			             static_cast<int>(keyword.size()), keyword.data(),
			             static_cast<int>(arg.size()), arg.data());
			return {2};
		}
		break;
	}

	// `-p` PRINTS and ignores any operands, which is dash's behaviour: `readonly -p
	// x` there neither lists x nor makes it readonly.
	if (print_only || argv[i] == nullptr) {
		for (const auto& row : state.variables())
			if (make_readonly ? row.readonly : row.exported)
				print_declaration(keyword, row);
		return {0};
	}

	for (; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		const size_t eq = arg.find('=');
		const std::string_view name = arg.substr(0, eq);
		if (!is_name(name)) {
			std::fprintf(stderr, "lesh: %.*s: %.*s: bad variable name\n",
			             static_cast<int>(keyword.size()), keyword.data(),
			             static_cast<int>(name.size()), name.data());
			return {2};
		}
		if (eq != std::string_view::npos) {
			const bool ok = make_readonly ? state.set(name, arg.substr(eq + 1))
			                              : state.set_exported(name, arg.substr(eq + 1));
			if (!ok) {
				// Both are SPECIAL builtins, so this status exits a non-interactive
				// shell - the dispatch applies that, not this. readonly-p.tst and
				// export-p.tst each have a case that requires exactly it.
				shell_state::report_readonly(keyword, name);
				return {2};
			}
		} else if (!make_readonly) {
			// `export name` is not an assignment, so it is allowed on a readonly
			// variable and must not create a value for an unset one.
			state.mark_exported(name);
		}
		if (make_readonly)
			state.mark_readonly(name);
	}
	return {0};
}

builtin_result builtin_export(shell_state& state, char** argv) {
	return run_declaration(state, argv, /*make_readonly=*/false);
}

builtin_result builtin_readonly(shell_state& state, char** argv) {
	return run_declaration(state, argv, /*make_readonly=*/true);
}

builtin_result builtin_unset(shell_state& state, char** argv) {
	size_t i = 1;
	for (; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		if (arg == "--") {
			++i;
			break;
		}
		// `-v` is the default and selects variables. `-f` never reaches here: the
		// executor intercepts that form, because removing a function means reaching
		// the function table, which lives there.
		if (arg == "-v")
			continue;
		if (arg.size() >= 2 && arg[0] == '-') {
			std::fprintf(stderr, "lesh: unset: Illegal option %.*s\n",
			             static_cast<int>(arg.size()), arg.data());
			return {2};
		}
		break;
	}
	int status = 0;
	for (; argv[i] != nullptr; ++i) {
		// POSIX: unsetting a readonly variable is an error, and `unset` is a special
		// builtin, so it takes a non-interactive shell down with it. Two cases in
		// unset-p.tst check that, and both were passing silently.
		if (!state.unset(argv[i])) {
			shell_state::report_readonly("unset", argv[i]);
			status = 2;
		}
	}
	return {status};
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
	// A signal ignored on entry to a non-interactive shell prints NOTHING, with or
	// without -p: there is no trap to report and nothing a caller could restore,
	// since the condition cannot be changed. `trap -- - INT` would be worse than
	// silence, because it names the default action for a signal that is ignored.
	//
	// This is a recorded divergence (ADR-0001). dash and zsh accept the trap
	// command, list it, and then never run it; bash lists nothing and so does this.
	// A listing that names an action the shell will not take is the same failure as
	// a builtin that succeeds without doing anything.
	if (sigs.cannot_be_trapped(signo))
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

// `kill -l EXIT_STATUS`: POSIX's inverse of `kill -s`, and a gap here is not
// local. The suite's own run-test.sh renders every signalled exit with
// `kill -l "$actual_exit_status"`, and kill1-p.tst asserts both readings of the
// one operand - `kill -l 9` is KILL, and `kill -l $?` after a child died of
// SIGKILL is also KILL.
//
// The two readings never collide, because NSIG is 32 on macOS and 65 on glibc and
// a shell reports a signalled child as 128+n: at or below 128 the operand is a
// signal number, above it the 128 comes back off. `kill -l 128` is therefore an
// error, not EXIT - signal 0 is the null signal and has no name. dash agrees on
// every one of those.
builtin_result kill_list_one(const char* operand) {
	std::string_view text{operand};
	if (!is_unsigned_integer(text)) {
		std::fprintf(stderr, "lesh: kill: %s: not a signal number or exit status\n",
		             operand);
		return {1};
	}
	int value = 0;
	for (const char c : text) {
		value = value * 10 + (c - '0');
		if (value > 1024)
			break;  // no signal number is anywhere near this; stop before overflow
	}
	const int signo = value > 128 ? value - 128 : value;
	const std::string_view name = signal_state::signal_name(signo);
	if (signo <= 0 || name.empty()) {
		std::fprintf(stderr, "lesh: kill: %s: not a signal number or exit status\n",
		             operand);
		return {1};
	}
	std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
	return {0};
}

builtin_result builtin_kill(shell_state&, char** argv) {
	int signo = SIGTERM;
	size_t first = 1;

	if (argv[1] != nullptr && std::strcmp(argv[1], "-l") == 0) {
		if (argv[2] != nullptr)
			return kill_list_one(argv[2]);  // dash reads only the first operand too
		// `0` leads the list because the null signal is a legal `kill -s` operand
		// and has no name of its own; printing `EXIT` there would be a category
		// error - EXIT is a `trap` condition, not something you can send. dash
		// prints exactly this, one name per line, in numeric order.
		std::puts("0");
		for (int i = 1; i < kMaxSignal; ++i) {
			const std::string_view name = signal_state::signal_name(i);
			if (!name.empty())
				std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
		}
		return {0};
	}
	const char* signal_operand = nullptr;
	if (argv[1] != nullptr && std::strcmp(argv[1], "-s") == 0 && argv[2] != nullptr) {
		signal_operand = argv[2];
		first = 3;
	} else if (argv[1] != nullptr && argv[1][0] == '-' && argv[1][1] != '\0') {
		signal_operand = argv[1] + 1;
		first = 2;
	}
	if (signal_operand != nullptr)
		signo = signal_state::signal_number(signal_operand);
	if (signo < 0) {
		// Naming the operand matters: the whole of issue #38 presented itself as
		// this message with nothing in it to say WHICH signal the shell had never
		// heard of.
		std::fprintf(stderr, "lesh: kill: %s: bad signal\n", signal_operand);
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

// --- read --------------------------------------------------------------------

// One byte at a time, from FD 0, never through stdio. Two bugs in one line.
//
// `read` used to call std::fgetc(stdin). `main` slurps a script with
// read_all(std::cin), which drains fd 0 to EOF and latches the FILE*'s EOF
// indicator; a here-document then dup2()s a fresh pipe onto fd 0, but fgetc kept
// reporting EOF and kept its own stale buffer position. Every `read` in a script
// fed on STANDARD INPUT therefore failed with nothing assigned, while the same
// script run as a FILE worked - which is what scored read-p.tst 1/32 against
// dash's 31/32, since the yash harness always pipes the script in.
//
// And a buffered FILE* over-reads: POSIX requires `read` to consume no more of
// its input than the line it needs, which read-p.tst's "read does not read more
// than needed" asserts by running `cat` straight after it. A block-buffered
// stream would have swallowed that line too.
bool read_byte(char& c) {
	for (;;) {
		const ssize_t n = ::read(STDIN_FILENO, &c, 1);
		if (n == 1)
			return true;
		if (n == 0 || errno != EINTR)
			return false;  // handlers carry SA_RESTART, so EINTR here is rare
	}
}

// One line of input, as `read` needs it: the delimiter removed, backslashes
// resolved, and a parallel flag per character saying whether it came from an
// escape.
//
// The flags are not decoration. A backslash prevents field splitting, so
// `IFS=' -' read a b` on `A\ A B` must yield the fields `A A` and `B`: splitting
// has to know that this space arrived as `\ ` and is not a delimiter. Resolving
// the escapes first and splitting afterwards loses exactly that.
struct input_line {
	std::string text;
	std::string escaped;  // '\1' where text[i] came from a backslash escape
	bool at_eof = false;  // input ended before the delimiter was seen
};

input_line read_input_line(char delimiter, bool raw) {
	input_line line;
	for (;;) {
		char c = '\0';
		if (!read_byte(c)) {
			line.at_eof = true;
			break;
		}
		if (c == delimiter)
			break;
		if (!raw && c == '\\') {
			char next = '\0';
			if (!read_byte(next)) {
				// "orphan backslash is ignored": a trailing backslash at end of input
				// contributes nothing rather than being kept as a literal.
				line.at_eof = true;
				break;
			}
			if (next == '\n')
				continue;  // line continuation: the pair vanishes from the line
			line.text += next;
			line.escaped += '\1';
			continue;
		}
		line.text += c;
		line.escaped += '\0';
	}
	return line;
}

bool is_ifs_at(const input_line& line, std::string_view ifs, size_t i) {
	return line.escaped[i] == '\0' && ifs.find(line.text[i]) != std::string_view::npos;
}

// "IFS white space" is the subset of IFS that is space, tab or newline. POSIX
// treats it differently from the rest of IFS at both ends of the line and around
// a delimiter, which is the whole reason read-p.tst has twelve field-splitting
// cases rather than one.
bool is_ifs_space_at(const input_line& line, std::string_view ifs, size_t i) {
	const char c = line.text[i];
	return is_ifs_at(line, ifs, i) && (c == ' ' || c == '\t' || c == '\n');
}

struct line_field {
	size_t start = 0;
	size_t end = 0;
};

std::vector<line_field> split_line(const input_line& line, std::string_view ifs) {
	std::vector<line_field> fields;
	const size_t n = line.text.size();
	size_t at = 0;
	while (at < n && is_ifs_space_at(line, ifs, at))
		++at;  // leading IFS white space is not a delimiter
	while (at < n) {
		const size_t start = at;
		while (at < n && !is_ifs_at(line, ifs, at))
			++at;
		fields.push_back({start, at});
		// ONE delimiter is IFS white space, then at most one non-white-space IFS
		// character, then more IFS white space. A delimiter that runs to the end of
		// the line yields NO trailing empty field: `IFS=' -' read a b c` on
		// `A-B-C - ` gives exactly three fields, which is read-p.tst's "exact number
		// of fields with non-whitespace IFS". A non-white-space IFS character at the
		// START does yield an empty field, which is why only one of the two ends is
		// special-cased.
		while (at < n && is_ifs_space_at(line, ifs, at))
			++at;
		if (at < n && is_ifs_at(line, ifs, at)) {
			++at;
			while (at < n && is_ifs_space_at(line, ifs, at))
				++at;
		}
	}
	return fields;
}

builtin_result builtin_read(shell_state& state, char** argv) {
	// POSIX: reads ONE line, splits it on IFS, and assigns to the named variables
	// with the LAST one receiving everything that remains - which is what makes
	// `read first rest` work.
	bool raw = false;
	// `-d` is POSIX Issue 8 and the reason read-p.tst scores 32 rather than 31 on a
	// shell that has it: dash predates the addition and fails that one case. The
	// divergence is deliberate and recorded, per ADR-0001.
	char delimiter = '\n';
	size_t first = 1;
	for (; argv[first] != nullptr && argv[first][0] == '-' && argv[first][1] != '\0';
	     ++first) {
		if (std::strcmp(argv[first], "--") == 0) {
			++first;
			break;
		}
		const char* opt = argv[first] + 1;
		while (*opt != '\0') {
			if (*opt == 'r') {
				raw = true;
				++opt;
				continue;
			}
			if (*opt != 'd') {
				std::fprintf(stderr, "lesh: read: illegal option -%c\n", *opt);
				return {2};
			}
			// The delimiter is the rest of the word (`-d:`) or the next one (`-d :`).
			// An empty one means NUL, as it does in bash.
			++opt;
			if (*opt == '\0') {
				if (argv[first + 1] == nullptr) {
					std::fprintf(stderr, "lesh: read: -d: missing delimiter\n");
					return {2};
				}
				++first;
				opt = argv[first];
			}
			delimiter = *opt;
			break;
		}
	}

	const input_line line = read_input_line(delimiter, raw);

	// A readonly target makes `read` FAIL rather than discard the line quietly.
	// dash reports `read: v: is read only` and returns 2 and, being a regular
	// builtin, carries on.
	const auto assign = [&state](std::string_view name, std::string_view value) {
		if (state.set(name, value))
			return true;
		shell_state::report_readonly("read", name);
		return false;
	};

	// EOF reached before the delimiter fails, but the variables are still assigned:
	// `read a </dev/null` leaves `a` set to the empty string with status 1, and
	// `printf 'foo bar' | read a b` assigns both. dash agrees, and read-p.tst
	// asserts both ("EOF fails read", "variables are assigned even if EOF is
	// reached without newline"). Returning early on EOF is what left every variable
	// UNSET.
	const int status = line.at_eof ? 1 : 0;

	// No operands: bash, ksh and zsh assign REPLY; dash makes it an error. Kept
	// because no conformance assertion covers it and `while read; do` is common.
	if (argv[first] == nullptr)
		return {assign("REPLY", line.text) ? status : 2};

	const std::string_view ifs = state.ifs();
	const std::vector<line_field> fields = split_line(line, ifs);
	const size_t names = argc_of(argv) - first;

	for (size_t v = 0; v < names; ++v) {
		std::string_view value;
		if (v + 1 == names && fields.size() > names) {
			// The last variable takes the raw REMAINDER of the line, delimiters and
			// all, with trailing IFS white space removed - which is why a joined value
			// keeps a trailing `-` but not a trailing space. Trailing white space that
			// was escaped stays: it was quoted, so it is not a delimiter.
			size_t end = line.text.size();
			while (end > fields[v].start && is_ifs_space_at(line, ifs, end - 1))
				--end;
			value = std::string_view{line.text}.substr(fields[v].start, end - fields[v].start);
		} else if (v < fields.size()) {
			value = std::string_view{line.text}.substr(fields[v].start,
			                                           fields[v].end - fields[v].start);
		}
		// A name the shell could never assign is refused rather than stored under a
		// key no expansion can reach. Checked HERE rather than before the read,
		// because dash consumes the line first and then fails on the operand:
		// `printf 'A\nB\n' | { read 1bad; read x; }` leaves x as B, and the earlier
		// names are assigned before the bad one stops it.
		if (!is_name(argv[first + v])) {
			std::fprintf(stderr, "lesh: read: %s: bad variable name\n", argv[first + v]);
			return {2};
		}
		// Fewer fields than variables: the surplus variables are assigned the empty
		// string rather than left alone.
		if (!assign(argv[first + v], value))
			return {2};
	}
	return {status};
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
// `refused` is set when a write was rejected because the variable is readonly.
// getopts writes THREE variables - OPTIND, OPTARG and the name operand - and POSIX
// XBD 8.1 lets the builtin fail rather than ignore the readonlyness. dash fails,
// and getopts-p.tst's 'readonly OPTIND' and 'readonly OPTARG' each require a
// diagnostic and a non-zero status from one of the two builtins involved.
int getopts_step(shell_state& state, char** argv, bool& refused) {
	const auto write = [&state, &refused](std::string_view name, std::string_view value) {
		if (!state.set(name, value)) {
			shell_state::report_readonly("getopts", name);
			refused = true;
		}
	};
	const auto write_optind = [&state, &refused](size_t index, size_t offset) {
		if (!state.set_optind(index, offset)) {
			shell_state::report_readonly("getopts", "OPTIND");
			refused = true;
		}
	};
	const auto erase = [&state, &refused](std::string_view name) {
		if (!state.unset(name)) {
			shell_state::report_readonly("getopts", name);
			refused = true;
		}
	};

	if (argv[1] == nullptr || argv[2] == nullptr) {
		std::fprintf(stderr, "lesh: getopts: usage: getopts optstring name [arg...]\n");
		return 2;
	}
	const std::string_view name{argv[2]};
	if (!is_name(name)) {
		std::fprintf(stderr, "lesh: getopts: %.*s: bad variable name\n",
		             static_cast<int>(name.size()), name.data());
		return 2;
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
			write_optind(index, 0);
			write(name, "?");
			erase("OPTARG");
			return 1;
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
		write_optind(word_done ? index + 1 : index, word_done ? 0 : offset);
		write(name, "?");
		if (quiet) {
			write("OPTARG", std::string_view{&option, 1});
		} else {
			erase("OPTARG");
			std::fprintf(stderr, "lesh: getopts: illegal option -%c\n", option);
		}
		// Zero, not one: POSIX gives status >0 to the END of the options, and an
		// unknown option was still an option FOUND. `while getopts ...` has to keep
		// looping so the script's own `?)` case can run.
		return 0;
	}

	if (at + 1 < letters.size() && letters[at + 1] == ':') {
		if (!word_done) {
			// Adjoined: `-afoo` puts the rest of the word in OPTARG and finishes it.
			write("OPTARG", word.substr(offset));
			write_optind(index + 1, 0);
		} else if (index + 1 <= args.size()) {
			write("OPTARG", args[index]);
			write_optind(index + 2, 0);
		} else {
			// Missing argument. The option word is consumed either way, so OPTIND
			// still advances past it.
			write_optind(index + 1, 0);
			if (quiet) {
				write(name, ":");
				write("OPTARG", std::string_view{&option, 1});
			} else {
				write(name, "?");
				erase("OPTARG");
				std::fprintf(stderr, "lesh: getopts: option requires an argument -%c\n",
				             option);
			}
			return 0;
		}
	} else {
		// POSIX: OPTARG is UNSET when the option takes no argument, which is what
		// makes `${OPTARG-unset}` tell an argument-less option from an empty one.
		// dash and zsh leave the empty string here and both fail getopts-p.tst's
		// 'OPTARG is unset when option without argument is parsed'.
		erase("OPTARG");
		write_optind(word_done ? index + 1 : index, word_done ? 0 : offset);
	}

	write(name, std::string_view{&option, 1});
	return 0;
}

builtin_result builtin_getopts(shell_state& state, char** argv) {
	bool refused = false;
	const int status = getopts_step(state, argv, refused);
	// A refused write is getopts' OWN failure: reporting an option it could not
	// record would leave `while getopts ...` looping on a variable that never
	// changes.
	return {refused ? 2 : status};
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

// The handler table. Names, kinds and the executor's share of the work all live
// in kBuiltinRegistry (runtime/builtins.h); this table carries only the functions,
// and the static_assert below is what keeps the two from drifting.
struct entry {
	std::string_view name;
	builtin_result (*fn)(shell_state&, char**);
};

constexpr entry kBuiltins[] = {
	{"true", builtin_true},   {"false", builtin_false}, {"echo", builtin_echo},
	{"pwd", builtin_pwd},     {"cd", builtin_cd},       {":", builtin_colon},
	{"exit", builtin_exit},   {"export", builtin_export}, {"unset", builtin_unset},
	{"set", builtin_set},     {"break", builtin_break}, {"continue", builtin_continue},
	{"return", builtin_return}, {"shift", builtin_shift},
	{"alias", builtin_alias}, {"unalias", builtin_unalias},
	{"read", builtin_read}, {"command", builtin_command}, {"times", builtin_times},
	{"trap", builtin_trap}, {"kill", builtin_kill}, {"getopts", builtin_getopts},
	{"readonly", builtin_readonly},
	{"test", builtin_test}, {"[", builtin_test},
};

constexpr bool has_handler(std::string_view name) {
	for (const auto& b : kBuiltins)
		if (b.name == name)
			return true;
	return false;
}

constexpr bool in_registry(std::string_view name) {
	for (const auto& d : kBuiltinRegistry)
		if (d.name == name)
			return true;
	return false;
}

// THE GUARD, and the deliverable of #35.
//
// Before this, classification lived in shell_state.cpp over two name lists and the
// handlers lived here, with nothing relating them. `test` and `readonly` were in
// the first and not the second, so the command search stopped at "this is a
// builtin" and never reached PATH, the dispatch returned false, and the executor
// discarded it: `test 1 = 2; echo $?` printed 0 and `readonly OPTIND` did nothing
// at all. A unit test diffing the tables would have caught it the day `test` was
// classified; a static_assert cannot even be skipped by not running the tests.
//
// The check is both directions, because each direction is a different bug: a
// registry name with no handler is a command that silently succeeds, and a handler
// with no registry entry is a builtin the search order never reaches.
constexpr bool registry_agrees_with_handlers() {
	for (const auto& d : kBuiltinRegistry)
		if ((d.home == builtin_home::table) != has_handler(d.name))
			return false;
	for (const auto& b : kBuiltins)
		if (!in_registry(b.name))
			return false;
	return true;
}

static_assert(registry_agrees_with_handlers(),
              "kBuiltinRegistry and kBuiltins disagree: every registry entry with "
              "builtin_home::table needs a handler here, every handler needs a "
              "registry entry, and a name the executor implements must be marked "
              "builtin_home::executor. See issue #35.");

} // namespace

builtin_kind classify_builtin(std::string_view name) noexcept {
	for (const auto& d : kBuiltinRegistry)
		if (d.name == name)
			return d.kind;
	return builtin_kind::none;
}

bool builtin_has_handler(std::string_view name) noexcept {
	return has_handler(name);
}

builtin_home builtin_home_of(std::string_view name) noexcept {
	for (const auto& d : kBuiltinRegistry)
		if (d.name == name)
			return d.home;
	return builtin_home::table;
}

bool unset_selects_functions(char** argv) noexcept {
	if (argv == nullptr || argv[0] == nullptr)
		return false;
	for (size_t i = 1; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		if (arg == "--" || arg.size() < 2 || arg[0] != '-')
			return false;
		if (arg.find('f') != std::string_view::npos)
			return true;
	}
	return false;
}

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
	return false;
}

} // namespace lesh::runtime
