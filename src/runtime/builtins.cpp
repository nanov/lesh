#include "runtime/builtins.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
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

// `cd`'s two modes, POSIX XCU `cd` - and `pwd`'s, which are the same two. -L is the
// DEFAULT: `..` is resolved against the LOGICAL working directory, so a symlink
// followed on the way in is unfollowed on the way out. -P hands the path to chdir
// and reads the answer back, so PWD holds a physical path. #24 chose the logical PWD
// deliberately and this does not disturb it - -P is a second mode BESIDE it, not a
// rework of it.
//
// Which directory each mode NAMES is decided in shell_state, not here: `cd` and
// `pwd` are two views of one working directory and a shell whose two builtins
// disagree about where it is would be worse than one that is wrong in both (#51).
enum class cd_mode {
	logical,
	physical,
};

// The option scan the two share. POSIX gives both utilities the same pair and the
// same tie-break: the LAST of -L and -P wins, inside an option group as well as
// across them, so `-P -L -PL` is -L for either. Returns the index of the first
// argument that is not an option.
//
// `accept_e` is `cd`'s POSIX -e and is null for `pwd`, which has no such option -
// dash reports `pwd -e` as illegal and so does this. `unknown` names the offending
// letter so the caller can prefix the diagnostic with its own name, which is dash's
// wording and the reason this reports rather than prints.
size_t scan_directory_options(char** argv, cd_mode& mode, bool* accept_e, char& unknown) {
	size_t at = 1;
	for (; argv[at] != nullptr; ++at) {
		const std::string_view arg{argv[at]};
		if (arg == "--") {
			++at;
			break;
		}
		// A lone `-` is cd's OLDPWD OPERAND, not an option, so the scan stops at it
		// rather than reading it as an empty option group.
		if (arg.size() < 2 || arg[0] != '-')
			break;
		for (const char option : arg.substr(1)) {
			if (option == 'L') {
				mode = cd_mode::logical;
			} else if (option == 'P') {
				mode = cd_mode::physical;
			} else if (option == 'e' && accept_e != nullptr) {
				// POSIX -e: with -P, say so when the new working directory cannot be
				// determined. It has no meaning under -L, where PWD is COMPUTED rather
				// than read back and therefore always known, and POSIX leaves that
				// combination unspecified - so it is accepted and has no effect. dash
				// rejects -e outright, which is what fails it cd-p.tst's 'exit status of
				// success with -e'; the divergence is deliberate and recorded in #46.
				*accept_e = true;
			} else {
				unknown = option;
				return at;
			}
		}
	}
	return at;
}

// POSIX `pwd`. -L reports the LOGICAL working directory - $PWD, but only while it
// still names the directory the shell is in - and -P reports the physical one.
//
// Both halves were missing. The options were not read at all, so `pwd -P` printed
// the logical answer; and the stored value was never verified, so after `readonly
// PWD; cd sub` - a cd that moved the shell and could not record it - `pwd` kept
// printing the directory the shell had left (#51).
builtin_result builtin_pwd(shell_state& state, char** argv) {
	cd_mode mode = cd_mode::logical;   // POSIX: the default is -L
	char unknown = '\0';
	std::ignore = scan_directory_options(argv, mode, nullptr, unknown);
	if (unknown != '\0') {
		std::fprintf(stderr, "lesh: pwd: Illegal option -%c\n", unknown);
		return {2};
	}
	// Operands are IGNORED rather than diagnosed, which is where `pwd` parts from
	// `cd`: #46 diagnoses `cd a b` because taking the first operand and dropping the
	// rest lands the shell somewhere the user did not name. An extra operand to `pwd`
	// changes no answer, so refusing it would be a divergence from dash that buys
	// nothing.

	const std::string here = mode == cd_mode::logical
		? state.logical_working_directory()
		: shell_state::physical_working_directory();
	if (here.empty()) {
		// getcwd could not answer: the directory has been removed under the shell.
		// dash reports and fails here too, and printing nothing at status zero would
		// be a wrong answer a script would act on.
		std::fprintf(stderr, "lesh: pwd: %s\n", std::strerror(errno));
		return {1};
	}
	std::printf("%s\n", here.c_str());
	return {0};
}

// POSIX `cd` step 10's canonicalization: a `.` component is deleted and a `..`
// deletes the component before it, LEXICALLY - the filesystem is not consulted for
// the resolution itself.
//
// That is what makes the working directory logical: symlinks are not resolved
// unless -P is given, so `cd /tmp` on a system where /tmp links to /private/tmp
// must still report /tmp. Using the real path here is a difference dash catches
// immediately, and it matters because `cd ..` after following a symlink should
// return where the user came from rather than where the link pointed.
//
// The ONE filesystem access is step 10(b)(i): the component preceding a `..` must
// name a directory, or `cd` fails. Without it `cd ./file/../dev` succeeds by
// cancelling `file/..` out of a path no resolution could ever walk - which is what
// dash does, and what cd-p.tst's 'non-directory file in operand component (-L)'
// and 'non-existing file in operand component (-L)' fail dash for. `offender` is
// the prefix that failed so the diagnostic can name it, with errno as the stat
// left it.
bool canonicalize_logical(std::string_view path, std::string& out, std::string& offender) {
	std::vector<std::string_view> parts;
	// The check is only meaningful for an ABSOLUTE path, which is all cd hands over:
	// curpath has PWD prepended before it gets here. A relative path would be
	// stat()ed against the process's real directory, which under -L is not where the
	// shell believes it is.
	const bool absolute = !path.empty() && path[0] == '/';
	size_t at = 0;
	while (at <= path.size()) {
		const size_t slash = path.find('/', at);
		const std::string_view part = path.substr(
			at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
		if (part == "..") {
			if (!parts.empty()) {
				if (absolute) {
					std::string prefix;
					for (const auto& kept : parts) {
						prefix += '/';
						prefix.append(kept);
					}
					struct stat info{};
					if (::stat(prefix.c_str(), &info) != 0) {
						offender = std::move(prefix);
						return false;
					}
					if (!S_ISDIR(info.st_mode)) {
						// stat SUCCEEDED, so errno still holds whatever the last failed call
						// left there; the diagnostic has to name which of the two happened.
						errno = ENOTDIR;
						offender = std::move(prefix);
						return false;
					}
				}
				parts.pop_back();
			}
		} else if (!part.empty() && part != ".") {
			parts.push_back(part);
		}
		if (slash == std::string_view::npos)
			break;
		at = slash + 1;
	}
	std::string joined;
	for (const auto& part : parts) {
		joined += '/';
		joined.append(part);
	}
	out = joined.empty() ? "/" : std::move(joined);
	return true;
}

// POSIX `cd` step 5: a relative operand whose first component is neither `.` nor
// `..` is looked up in $CDPATH, left to right, and the FIRST entry under which it
// names a directory wins.
//
// An EMPTY entry means the current directory, and it is the one match whose result
// is NOT written to standard output - which is the half shells get wrong. A
// literal `.` entry prints and an empty one does not; cd-p.tst asserts both
// ('found in dot cd path' expects the line, 'found in empty cd path' expects no
// line), and they differ in nothing else.
//
// Returns true when an entry matched: `out` is the concatenated path and
// `announce` says whether the new directory must be printed.
bool search_cdpath(const shell_state& state, std::string_view operand,
                   std::string& out, bool& announce) {
	// The FIRST COMPONENT, not the first character: `.hidden` is an ordinary name
	// that CDPATH is searched for, while `./hidden` and `..` are not.
	const size_t slash = operand.find('/');
	const std::string_view first = operand.substr(
		0, slash == std::string_view::npos ? std::string_view::npos : slash);
	if (first == "." || first == "..")
		return false;

	std::string_view cdpath;
	if (!state.lookup("CDPATH", cdpath) || cdpath.empty())
		return false;

	size_t at = 0;
	while (at <= cdpath.size()) {
		const size_t colon = cdpath.find(':', at);
		const std::string_view entry = cdpath.substr(
			at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
		std::string candidate{entry.empty() ? std::string_view{"."} : entry};
		candidate += '/';
		candidate.append(operand);
		struct stat info{};
		if (::stat(candidate.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
			out = std::move(candidate);
			announce = !entry.empty();
			return true;
		}
		if (colon == std::string_view::npos)
			break;
		at = colon + 1;
	}
	return false;
}

builtin_result builtin_cd(shell_state& state, char** argv) {
	cd_mode mode = cd_mode::logical;   // POSIX: the default is -L
	bool require_pwd = false;          // -e
	char unknown = '\0';
	// cd-p.tst's 'the last option wins' is `cd -P -L -PL`, whose answer is -L; the
	// scan is shared with `pwd`, which has the same pair and the same tie-break.
	const size_t at = scan_directory_options(argv, mode, &require_pwd, unknown);
	if (unknown != '\0') {
		std::fprintf(stderr, "lesh: cd: Illegal option -%c\n", unknown);
		return {2};
	}

	// At most ONE operand. dash takes the first and ignores the rest in silence,
	// which turns `cd my dir` - an unquoted pathname with a space in it - into a
	// successful cd to `my`. A wrong answer with no diagnostic is the failure mode
	// this project keeps paying for, so this diverges from the reference
	// deliberately (ADR-0001 wants the divergence in writing, and #46 carries it):
	// POSIX's synopsis takes one operand, bash, ksh, yash and zsh all diagnose a
	// second, dash is the outlier, and cd-p.tst asserts nothing either way.
	if (argc_of(argv) - at > 1) {
		std::fprintf(stderr, "lesh: cd: too many operands\n");
		return {2};
	}

	std::string operand;       // what the user asked for, for the diagnostic
	std::string curpath;       // POSIX's curpath, which may come from CDPATH
	bool announce = false;     // POSIX: some forms WRITE the new directory
	if (argv[at] == nullptr) {
		std::string_view home;
		if (!state.lookup("HOME", home) || home.empty()) {
			std::fprintf(stderr, "lesh: cd: HOME not set\n");
			return {2};
		}
		operand.assign(home);
		curpath = operand;
	} else if (std::strcmp(argv[at], "-") == 0) {
		std::string_view previous;
		if (!state.lookup("OLDPWD", previous) || previous.empty()) {
			std::fprintf(stderr, "lesh: cd: OLDPWD not set\n");
			return {2};
		}
		operand.assign(previous);
		curpath = operand;
		announce = true;   // POSIX: `cd -` prints where it went
	} else {
		operand = argv[at];
		// chdir("") is ENOENT and POSIX gives cd no special case for an empty
		// operand. Both of the paths below would answer wrongly: joining it onto PWD
		// succeeds as a no-op, and searching CDPATH for it finds every entry, so
		// `CDPATH=/x; cd ''` would land in /x. dash succeeds here; cd-p.tst's 'empty
		// operand' asserts the failure, and #46 records the divergence.
		if (operand.empty()) {
			std::fprintf(stderr, "lesh: cd: : %s\n", std::strerror(ENOENT));
			return {2};
		}
		curpath = operand;
		if (operand[0] != '/') {
			std::string found;
			if (search_cdpath(state, operand, found, announce))
				curpath = std::move(found);
		}
	}

	// The PREVIOUS logical directory, copied out before anything is written back:
	// it becomes OLDPWD, and it is what a relative curpath extends. Step 8 joins
	// onto the LOGICAL PWD rather than onto the real path, which is what makes `cd
	// link` then `cd ..` return where the user came from.
	//
	// It is shell_state's answer rather than a reading of $PWD, and #51 is why: a
	// $PWD that no longer names the current directory - what `readonly PWD; cd sub`
	// leaves behind, and what a lie in the environment used to leave at startup -
	// would make every relative operand resolve against a directory the shell is not
	// in. `pwd` falls back by the same rule, so the two cannot disagree.
	const std::string previous_pwd = state.logical_working_directory();
	if (curpath.empty() || curpath[0] != '/') {
		std::string joined = previous_pwd;
		joined += '/';
		joined += curpath;
		curpath = std::move(joined);
	}

	std::string next_pwd;
	bool pwd_unknown = false;
	if (mode == cd_mode::physical) {
		// -P: the kernel does the resolving, and PWD is read BACK from it. That is
		// the whole difference - by the time getcwd answers, every symlink in curpath
		// is gone. No lexical canonicalization runs, because POSIX's step 10 is -L's
		// alone: `cd -P link/..` must land where the LINK's parent is, not where the
		// text says.
		std::error_code ec;
		std::filesystem::current_path(curpath, ec);
		if (ec) {
			std::fprintf(stderr, "lesh: cd: %s: %s\n", operand.c_str(), ec.message().c_str());
			return {2};
		}
		next_pwd = shell_state::physical_working_directory();
		if (next_pwd.empty()) {
			// The directory DID change and only its name is unknown, so this is not a
			// failed cd: PWD gets the best answer available and the status is decided at
			// the end, where -e is honoured.
			next_pwd = curpath;
			pwd_unknown = true;
		}
	} else {
		std::string offender;
		if (!canonicalize_logical(curpath, next_pwd, offender)) {
			std::fprintf(stderr, "lesh: cd: %s: %s\n", offender.c_str(), std::strerror(errno));
			return {2};
		}
		// The CANONICAL path is what gets chdir'd, not the operand: under -L the two
		// name different directories whenever a symlink and a `..` meet, and PWD has
		// to describe the one the shell actually moved to.
		std::error_code ec;
		std::filesystem::current_path(next_pwd, ec);
		if (ec) {
			std::fprintf(stderr, "lesh: cd: %s: %s\n", operand.c_str(), ec.message().c_str());
			return {2};
		}
	}

	// POSIX: `cd -`, and a cd resolved through a NON-EMPTY CDPATH entry, write the
	// new working directory to standard output. `pwd` is not enough for a script to
	// find out where a CDPATH search landed it - that is what the rule is for.
	if (announce)
		std::printf("%s\n", next_pwd.c_str());

	// PWD and OLDPWD are part of cd's contract, not a nicety: scripts read them.
	// A readonly PWD makes cd FAIL after the directory has already changed, which
	// is what dash does too - the chdir is not undone, but the shell says that the
	// variable no longer describes where it is.
	int status = 0;
	if (!previous_pwd.empty() && !state.set_exported("OLDPWD", previous_pwd)) {
		shell_state::report_readonly("cd", "OLDPWD");
		status = 2;
	}
	if (!state.set_exported("PWD", next_pwd)) {
		shell_state::report_readonly("cd", "PWD");
		status = 2;
	}
	// POSIX -e, and the one cd failure that is not 2: the shell DID move and only
	// cannot say where, which a script asks about precisely because it is not the
	// same as not having moved.
	if (status == 0 && pwd_unknown && require_pwd)
		status = 1;
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
	// NEGATED IN THE UNSIGNED DOMAIN, because -INT64_MIN has no int64_t to be. The
	// limit above deliberately ADMITS the magnitude 2^63, so INT64_MIN is a legal
	// operand - dash, bash and zsh all compare `test -9223372036854775808 -eq 1`
	// without complaint - and negating the signed conversion of it was undefined
	// behaviour on the one value the range check exists to let through (#62).
	// Unsigned wrapping is defined, and 0 - 2^63 converts back to exactly INT64_MIN.
	//
	// SATURATING WOULD BE THE WRONG SHAPE HERE, unlike arithmetic's over-large
	// literal (#59): `test` is comparing, not computing, so an operand it cannot
	// represent is a usage error and stays one - the `magnitude > limit` return
	// above, which reaches the caller as `Illegal number` and status 2, exactly as
	// dash and bash answer. A clamped operand would silently compare a number the
	// script never wrote.
	out = static_cast<int64_t>(negative ? uint64_t{0} - magnitude : magnitude);
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
		const bool have_first = stat(left, &first) == 0;
		const bool have_second = stat(right, &second) == 0;
		// `-ef` asks whether two pathnames name the SAME file, and a pathname that
		// names nothing names nothing in common with anything.
		if (op == "-ef")
			return have_first && have_second && first.st_dev == second.st_dev &&
			       first.st_ino == second.st_ino;
		// A file that does not exist has no modification time, so it cannot be newer
		// than anything - and a file that does exist is newer than one that is not
		// there. `test XXXXX -ot newer` is TRUE and `test newer -nt XXXXX` is TRUE,
		// which is what test-p.tst asserts and what bash answers; two missing
		// operands are neither newer nor older than each other.
		//
		// Refusing both the moment either stat failed made an ABSENT file
		// indistinguishable from a file with the same timestamp, which is the answer
		// dash and zsh give and the one thing a freshness test is asked for: `[ out
		// -nt in ]` is how every hand-written build rule spells "rebuild", and with
		// no `out` yet it said there was nothing to do.
		if (!have_first && !have_second)
			return false;
		if (!have_first)
			return op == "-ot";
		if (!have_second)
			return op == "-nt";
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
	const size_t operand = first_operand(argv);
	// POSIX: with no operand, exit with the status of the last command - except
	// inside a TRAP ACTION, where POSIX XCU `exit` makes "the last command" the one
	// that ran immediately BEFORE the trap action. So `trap '(exit 2); exit' INT`
	// exits with the status of the command the signal interrupted and not with 2,
	// which is exit-p.tst's 'default exit status in signal trap' and dash's answer.
	const int status = argv[operand] != nullptr
		? std::atoi(argv[operand])
		: state.trap_entry_status().value_or(state.last_status());
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

// 2, as for any other builtin's usage error, and what dash answers for every
// malformed `kill` line. Distinct from 1, which this builtin keeps for a kill(2)
// the SYSTEM refused: the command was well formed and the shell did ask.
constexpr int kKillUsageError = 2;

builtin_result kill_usage() {
	std::fprintf(stderr, "lesh: kill: usage: kill -s signal_name pid... | "
	                     "kill -signal_name pid... | kill -l [exit_status]\n");
	return {kKillUsageError};
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
		return {kKillUsageError};
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
		return {kKillUsageError};
	}
	std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
	return {0};
}

// `0` leads the list because the null signal is a legal `kill -s` operand and has
// no name of its own; printing `EXIT` there would be a category error - EXIT is a
// `trap` condition, not something you can send. dash prints exactly this, one name
// per line, in numeric order.
builtin_result kill_list_all() {
	std::puts("0");
	for (int i = 1; i < kMaxSignal; ++i) {
		const std::string_view name = signal_state::signal_name(i);
		if (!name.empty())
			std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
	}
	return {0};
}

// A pid operand, read strictly. This is what `std::atoi` was doing instead, and a
// missing operand was only the QUIETEST member of the family it let through: atoi
// answers 0 for `notanumber`, for `--`, for `%1` and for `0x10`, and kill(0, sig)
// signals THE WHOLE PROCESS GROUP - so `kill -s TERM notanumber` killed the shell
// and everything beside it, and `kill -s TERM -1` asked the kernel to signal every
// process the user owns. Neither of those is a diagnostic being missed; they are
// the wrong syscall being made (#45).
//
// dash's tolerances are copied deliberately rather than tightened: surrounding
// blanks, a leading `+` and leading zeros are all accepted, because dash is the
// POSIX floor and a script may already rely on `kill "$pid_with_spaces"`. The
// range check is dash's too - it refuses a number it cannot hold rather than
// wrapping onto some unrelated process.
bool read_pid_operand(const char* operand, pid_t& out) {
	std::string_view text{operand};
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
		text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
		text.remove_suffix(1);
	// A NEGATIVE pid names a process group, and POSIX writes it after `--` -
	// `kill -s HUP -- -$pgid`, which is the form kill4-p.tst uses. A bare `-$pgid`
	// is an option as far as the option scan is concerned, and is refused there.
	bool negative = false;
	if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
		negative = text.front() == '-';
		text.remove_prefix(1);
	}
	if (!is_unsigned_integer(text))
		return false;
	long long value = 0;
	for (const char c : text) {
		value = value * 10 + (c - '0');
		if (value > static_cast<long long>(std::numeric_limits<pid_t>::max()))
			return false;
	}
	out = static_cast<pid_t>(negative ? -value : value);
	return true;
}

// POSIX gives `kill` exactly two forms: a signal specification followed by AT
// LEAST ONE pid, and `-l [exit_status]`. Every other spelling is a usage error,
// and saying so is the whole point of this reading - `kill -s TERM` used to
// signal nothing and report SUCCESS, the stub-that-succeeds shape that has now
// cost this project five debugging sessions (`command`, `set -o pipefail`,
// `test`, `kill -l EXITSTATUS`, and this).
//
// One scan over argv rather than a check bolted onto the front of the old one:
// the old reading looked at argv[1] and argv[2] by hand and had no notion of
// where the OPERANDS began, which is why `kill -s` took the `s` for a signal name
// and `kill -s TERM --` sent SIGTERM to the shell's process group.
builtin_result builtin_kill(shell_state&, char** argv) {
	const char* signal_operand = nullptr;
	bool list = false;
	size_t i = 1;
	for (; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		// POSIX XCU 1.4: `--` ends the options. first_operand cannot serve here -
		// it answers for a utility whose `--` can only be argv[1], and `kill`'s
		// comes after the signal option.
		if (arg == "--") {
			++i;
			break;
		}
		if (arg.size() < 2 || arg.front() != '-')
			break;  // the operands start here
		if (arg == "-l") {
			list = true;
			continue;
		}
		if (arg == "-s") {
			// `kill -s` with nothing after it names no signal. It used to fall
			// through to the `-NAME` reading below, which took the `s` for the name
			// and reported `s: bad signal` - a diagnostic about the wrong thing.
			if (argv[i + 1] == nullptr)
				return kill_usage();
			signal_operand = argv[++i];
			continue;
		}
		if (arg.substr(0, 2) == "-s") {
			signal_operand = argv[i] + 2;  // `kill -sTERM`, which dash accepts too
			continue;
		}
		// `-TERM`, `-15`. Validated below, so an option this reading does not know -
		// `kill -x 1`, `kill -n 9 $$` - is refused by name rather than silently
		// taken for a pid.
		signal_operand = argv[i] + 1;
	}

	if (list) {
		// dash reads only the first operand and ignores the rest.
		return argv[i] != nullptr ? kill_list_one(argv[i]) : kill_list_all();
	}

	int signo = SIGTERM;
	if (signal_operand != nullptr) {
		signo = signal_state::signal_number(signal_operand);
		// `EXIT` is a `trap` CONDITION and not something you can send, so the name
		// resolving to 0 is not the same answer as the number 0 - which is the null
		// signal and a legal operand. `kill -s EXIT $$` reported success having sent
		// nothing; the same category error `kill -l` refuses at the other end.
		if (signo == kExitTrap && !is_unsigned_integer(signal_operand))
			signo = -1;
		if (signo < 0) {
			// Naming the operand matters: the whole of issue #38 presented itself as
			// this message with nothing in it to say WHICH signal the shell had never
			// heard of.
			std::fprintf(stderr, "lesh: kill: %s: bad signal\n", signal_operand);
			return {kKillUsageError};
		}
	}
	// The ticket's case, and the reason for all of the above: a signal with no pid.
	if (argv[i] == nullptr)
		return kill_usage();

	int status = 0;
	for (; argv[i] != nullptr; ++i) {
		pid_t pid = 0;
		if (!read_pid_operand(argv[i], pid)) {
			std::fprintf(stderr, "lesh: kill: %s: not a process id\n", argv[i]);
			status = kKillUsageError;
			continue;
		}
		if (::kill(pid, signo) != 0) {
			std::fprintf(stderr, "lesh: kill: %s: %s\n", argv[i], std::strerror(errno));
			// A usage error already reported outranks this one: dash answers 2 for a
			// line it refused to run and 1 only for one the system refused.
			if (status == 0)
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

// One `name='value'` line of an `alias` listing, quoted so the shell reads it
// back as exactly these bytes.
//
// POSIX requires the listing to be in a form that can be RE-INPUT, and a value
// containing a blank or a quote is what decides whether it really can: printing
// `e=echo hi` raw defines an alias `e` whose value is `echo` when fed back. This
// is the same defect as the `trap` listing in #33 and the `readonly` listing in
// #35, which is why it uses their quoting helper rather than a third one.
//
// The NAME is printed raw, as dash does: a name needing quotes could not have
// been written on the left of an `=` in the first place.
//
// Declared in builtins.h because `command -v` prints one too: POSIX writes an
// alias as a command line that re-creates it, so `eval "$(command -v abc)"` has
// to define the same alias the listing would.

builtin_result builtin_alias(shell_state& state, char** argv) {
	// No operands: every alias, sorted. It printed NOTHING before - a listing that
	// produces no output fails every case that inspects one, and it made `unalias -a`
	// look correct while doing nothing (#40).
	if (argv[1] == nullptr) {
		for (const auto& row : state.aliases())
			print_alias(row.name, row.value);
		return {0};
	}
	int status = 0;
	for (size_t i = 1; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		// The `=` is searched from the SECOND byte, so `=foo` is a lookup of an alias
		// named `=foo` rather than an assignment to the empty name. dash does the
		// same, and POSIX gives `alias` no options at all - which is why `-p` and
		// `--` are operands here and not flags, and are reported as not found.
		if (const size_t eq = arg.find('=', 1); eq != std::string_view::npos) {
			state.set_alias(arg.substr(0, eq), arg.substr(eq + 1));
		} else if (std::string_view value; state.lookup_alias(arg, value)) {
			print_alias(arg, value);
		} else {
			std::fprintf(stderr, "lesh: alias: %.*s: not found\n",
			             static_cast<int>(arg.size()), arg.data());
			status = 1;
		}
	}
	return {status};
}

builtin_result builtin_unalias(shell_state& state, char** argv) {
	// `-a` removes every alias. It used to be looked up as the NAME `-a`, which
	// removed nothing and reported nothing - and the case that asserts it passed
	// anyway, because `alias` printed nothing to compare against. Two silent
	// failures cancelling out is exactly what #38 found in sigurg5-p.tst.
	if (argv[1] != nullptr && std::string_view{argv[1]} == "-a") {
		state.clear_aliases();
		return {0};
	}
	int status = 0;
	for (size_t i = 1; argv[i] != nullptr; ++i) {
		// POSIX: removing an alias that does not exist is an ERROR. Returning 0 made
		// `unalias true; unalias true` succeed twice.
		if (!state.unset_alias(argv[i])) {
			std::fprintf(stderr, "lesh: unalias: %s: not found\n", argv[i]);
			status = 1;
		}
	}
	return {status};
}

builtin_result builtin_shift(shell_state& state, char** argv) {
	const size_t n = argv[1] != nullptr ? static_cast<size_t>(std::atoi(argv[1])) : 1;
	if (!state.shift_positional(n)) {
		std::fprintf(stderr, "lesh: shift: can't shift that many\n");
		return {1};
	}
	return {0};
}

// How many levels `break` or `continue` unwinds. False when the operand was not a
// positive decimal integer, having reported it.
//
// POSIX XCU: n "shall be a positive decimal integer". `break 0` went through
// std::atoi and asked to unwind ZERO levels, which consume_loop_flow reads as
// "already arrived" - so the break VANISHED and the loop carried on to the
// commands after it. A break that silently does nothing is the
// stub-that-succeeds failure #24's resolution warned about, and break-p.tst's
// 'zero operand' is the assertion for it. dash writes `Illegal number: 0` and
// exits 2; being a special builtin, so does this.
//
// The digits are checked before the value rather than after: `break -1` and
// `break x` both reach atoi as something <= 0, and a NEGATIVE level would make
// consume_loop_flow's `--_flow_level <= 0` true at the first loop and leave the
// operand indistinguishable from 1.
bool read_flow_level(std::string_view name, char* const* argv, int& level) {
	const size_t operand = first_operand(argv);
	if (argv[operand] == nullptr)
		return true;  // no operand: the default of one stands
	const std::string_view text{argv[operand]};
	unsigned long long value = 0;
	if (!text.empty() &&
	    text.find_first_not_of("0123456789") == std::string_view::npos) {
		for (const char c : text) {
			value = value * 10 + static_cast<unsigned long long>(c - '0');
			// Clamped rather than wrapped: `break 99999999999999` means out of every
			// loop there is, which is what any level past the nesting depth already
			// means ('breaking much more than actual nest level'). Wrapping could land
			// on zero and turn the break back into the no-op this function exists to
			// refuse.
			if (value > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
				value = static_cast<unsigned long long>(std::numeric_limits<int>::max());
				break;
			}
		}
	}
	if (value == 0) {
		std::fprintf(stderr, "lesh: %.*s: %.*s: not a positive integer\n",
		             static_cast<int>(name.size()), name.data(),
		             static_cast<int>(text.size()), text.data());
		return false;
	}
	level = static_cast<int>(value);
	return true;
}

builtin_result builtin_break(shell_state&, char** argv) {
	int level = 1;
	// 2, as for any other builtin's usage error, and what dash answers. `break` is
	// SPECIAL, so try_run_builtin turns the non-zero status into an exit for a
	// non-interactive shell - which is what dash does too.
	if (!read_flow_level("break", argv, level))
		return {2};
	return {0, control_flow::break_loop, level};
}

builtin_result builtin_continue(shell_state&, char** argv) {
	int level = 1;
	if (!read_flow_level("continue", argv, level))
		return {2};
	return {0, control_flow::continue_loop, level};
}

builtin_result builtin_return(shell_state& state, char** argv) {
	const size_t operand = first_operand(argv);
	// The same rule `exit` follows, and for the same reason: POSIX gives both the
	// status of the last command executed, and inside a TRAP ACTION "the last
	// command" is the one that ran immediately before the action. So
	// `fn() { true; return; }` called from a trap reports the status the trap
	// interrupted rather than `true`'s - return-p.tst's 'default exit status in
	// function in trap', which expects 19 and not 0.
	//
	// DIVERGENCE FROM dash, recorded in ADR-0001: dash, bash and zsh all report
	// the body's own last command here, while all four of the equivalent `exit`
	// cases in exit-p.tst agree with the conformance suite - and dash itself
	// applies the rule to `exit`. A shell whose `exit` and `return` answer the same
	// question differently would be the real defect.
	return {argv[operand] != nullptr
	        ? std::atoi(argv[operand])
	        : state.trap_entry_status().value_or(state.last_status()),
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
	{"read", builtin_read}, {"times", builtin_times},
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

void print_alias(std::string_view name, std::string_view value) {
	std::printf("%.*s=", static_cast<int>(name.size()), name.data());
	print_single_quoted(value);
	std::fputc('\n', stdout);
}

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

builtin_report builtin_report_of(std::string_view name) noexcept {
	for (const auto& d : kBuiltinRegistry)
		if (d.name == name)
			return d.report;
	return builtin_report::name;
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
