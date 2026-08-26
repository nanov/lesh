#include "runtime/builtins.h"

#include "runtime/diagnostic.h"
#include "runtime/option_word.h"
#include "substrate/args.h"
#include "substrate/numeric.h"

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
#include <utility>
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
	//
	// `echo` GETS NO SPEC (#148), and the reasoning was re-read against the corpus
	// rather than carried over. POSIX gives `echo` no OPTIONS section at all, so a
	// word in option position that is not `-n` is an ARGUMENT and has to be
	// printed. A table cannot say that: a spec with an `n` row refuses `-Z`, and a
	// spec with no rows refuses it too. Measured at 634e4c8:
	//
	//     lesh   echo -Z    -Z      dash   echo -Z    -Z
	//     bash   echo -Z    -Z      zsh    echo -Z    -Z
	//
	// zsh spells the same exemption as a flag on its table row (BINF_SKIPINVALID:
	// "a word containing any unknown char is an operand"), which this parser does
	// not have and should not grow for one utility. Six lines of special case are
	// less code than the row that would replace them, and `--` stays absent here
	// for the same reason - `echo --` prints `--`, as it does in dash.
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

// The options the two share, as two rows of a table rather than a scan (#148).
// POSIX gives both utilities the same pair and the same tie-break: the LAST of
// -L and -P wins, inside an option group as well as across them, so `-P -L -PL`
// is -L for either. Binding both letters to ONE field is what makes that true -
// the second write simply overwrites the first, with no group bookkeeping to get
// wrong inside a cluster, which is what cd-p.tst:354 and :365 assert.
//
// -e IS CD'S ALONE, so `pwd` declares two rows where `cd` declares three and
// `pwd -e` is an illegal option - which is dash's behaviour and this tree's.
// POSIX -e: with -P, say so when the new working directory cannot be determined.
// It has no meaning under -L, where PWD is COMPUTED rather than read back and
// therefore always known, and POSIX leaves that combination unspecified - so it
// is accepted and has no effect. dash rejects -e outright, which is what fails it
// cd-p.tst's 'exit status of success with -e'; the divergence is deliberate and
// recorded in #46.
//
// A lone `-` is cd's OLDPWD OPERAND rather than an empty option group, and the
// parser knows it: XBD 12.2 says a word of one character is an operand.
struct directory_opts {
	cd_mode mode = cd_mode::logical;   // POSIX: the default is -L
	bool require_pwd = false;          // -e
};

constexpr auto kCd = args::spec<directory_opts>(
	args::option{'L', args::field<&directory_opts::mode>, cd_mode::logical}
		.help("resolve .. against the logical working directory"),
	args::option{'P', args::field<&directory_opts::mode>, cd_mode::physical}
		.help("resolve .. against the physical working directory"),
	args::option{'e', args::field<&directory_opts::require_pwd>}
		.help("with -P, fail when the new working directory cannot be determined"));

constexpr auto kPwd = args::spec<directory_opts>(
	args::option{'L', args::field<&directory_opts::mode>, cd_mode::logical}
		.help("report the logical working directory"),
	args::option{'P', args::field<&directory_opts::mode>, cd_mode::physical}
		.help("report the physical working directory"));

// POSIX `pwd`. -L reports the LOGICAL working directory - $PWD, but only while it
// still names the directory the shell is in - and -P reports the physical one.
//
// Both halves were missing. The options were not read at all, so `pwd -P` printed
// the logical answer; and the stored value was never verified, so after `readonly
// PWD; cd sub` - a cd that moved the shell and could not record it - `pwd` kept
// printing the directory the shell had left (#51).
builtin_result builtin_pwd(shell_state& state, char** argv) {
	const auto parsed = args::parse(kPwd, argv);
	if (parsed.err)
		return {report_option_error("pwd", parsed.err)};
	const cd_mode mode = parsed.opts.mode;
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
		report("pwd: %s", std::strerror(errno));
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
	// cd-p.tst's 'the last option wins' is `cd -P -L -PL`, whose answer is -L; the
	// table is shared in shape with `pwd`, which has the same pair and the same
	// tie-break, and differs only by the row for -e.
	const auto parsed = args::parse(kCd, argv);
	if (parsed.err)
		return {report_option_error("cd", parsed.err)};
	const cd_mode mode = parsed.opts.mode;
	const bool require_pwd = parsed.opts.require_pwd;
	// The untouched operand tail, as an index into the caller's own argv.
	const size_t at = static_cast<size_t>(parsed.rest - argv);

	// At most ONE operand. dash takes the first and ignores the rest in silence,
	// which turns `cd my dir` - an unquoted pathname with a space in it - into a
	// successful cd to `my`. A wrong answer with no diagnostic is the failure mode
	// this project keeps paying for, so this diverges from the reference
	// deliberately (ADR-0001 wants the divergence in writing, and #46 carries it):
	// POSIX's synopsis takes one operand, bash, ksh, yash and zsh all diagnose a
	// second, dash is the outlier, and cd-p.tst asserts nothing either way.
	if (argc_of(argv) - at > 1) {
		report("cd: too many operands");
		return {2};
	}

	std::string operand;       // what the user asked for, for the diagnostic
	std::string curpath;       // POSIX's curpath, which may come from CDPATH
	bool announce = false;     // POSIX: some forms WRITE the new directory
	if (argv[at] == nullptr) {
		std::string_view home;
		if (!state.lookup("HOME", home) || home.empty()) {
			report("cd: HOME not set");
			return {2};
		}
		operand.assign(home);
		curpath = operand;
	} else if (std::strcmp(argv[at], "-") == 0) {
		std::string_view previous;
		if (!state.lookup("OLDPWD", previous) || previous.empty()) {
			report("cd: OLDPWD not set");
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
			report("cd: : %s", std::strerror(ENOENT));
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
			report("cd: %s: %s", operand.c_str(), ec.message().c_str());
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
			report("cd: %s: %s", offender.c_str(), std::strerror(errno));
			return {2};
		}
		// The CANONICAL path is what gets chdir'd, not the operand: under -L the two
		// name different directories whenever a symlink and a `..` meet, and PWD has
		// to describe the one the shell actually moved to.
		std::error_code ec;
		std::filesystem::current_path(next_pwd, ec);
		if (ec) {
			report("cd: %s: %s", operand.c_str(), ec.message().c_str());
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
// else - all four of which the `test_operand` row of the numeric policy table now
// carries, so the tolerances are stated where every site's are and not buried in
// a loop here (#63). Overflow is an error rather than a wrap, because
// `test 99999999999999999999 -eq 1` must report `Illegal number` rather than
// compare a truncated value.
bool test_integer(std::string_view text, int64_t& out) {
	const numeric_result parsed = parse_integer(text, numeric_site::test_operand);
	if (parsed.status != numeric_parse::ok)
		return false;
	// NEGATED IN THE UNSIGNED DOMAIN, which parse_integer does for every signed
	// site: -INT64_MIN has no int64_t to be, and the `test_operand` row
	// deliberately ADMITS the magnitude 2^63, so INT64_MIN is a legal operand -
	// dash, bash and zsh all compare `test -9223372036854775808 -eq 1` without
	// complaint - where negating the signed conversion of it was undefined
	// behaviour on the one value the range check exists to let through (#62).
	//
	// SATURATING WOULD BE THE WRONG SHAPE HERE, unlike arithmetic's over-large
	// literal (#59): `test` is comparing, not computing, so an operand it cannot
	// represent is a usage error and stays one - the false return above, which
	// reaches the caller as `Illegal number` and status 2, exactly as dash and bash
	// answer. A clamped operand would silently compare a number the script never
	// wrote. parse_integer hands back the clamp anyway; ignoring it is the policy.
	out = parsed.value;
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
			report("%.*s: %s",
			       static_cast<int>(invoked_as.size()), invoked_as.data(),
			       fail.message);
		else
			report("%.*s: %.*s: %s",
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
			report("[: missing ]");
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
	if (argv[operand] == nullptr)
		return {state.trap_entry_status().value_or(state.last_status()),
		        control_flow::exit_shell};

	// THE WORST OF THE FOUR `std::atoi` SITES, because a script's exit status is
	// what its caller branches on: atoi answered 0 for `notanumber`, so
	// `lesh -c 'exit notanumber'` REPORTED SUCCESS, and truncated `3x` to 3. dash
	// refuses both with `Illegal number` at 2 and so does this now (#63).
	//
	// THE MODULO 256 POSIX APPLIES TO A STATUS IS A SEPARATE RULE AND STAYS ONE.
	// `exit 256` is 0 and `exit 300` is 44 because only the low byte of a status
	// survives waitpid, not because 256 is out of range - it is a perfectly
	// representable int, and it is the KERNEL that truncates it. Conflating the two
	// is how `return 99999999999999999999` came to report -1: a number too large to
	// be an int is refused here, and a number that fits is passed through whole.
	const numeric_result parsed = parse_integer(argv[operand], numeric_site::exit_status);
	if (parsed.status != numeric_parse::ok)
		return {report_bad_number("exit", argv[operand], parsed.status)};
	return {static_cast<int>(parsed.value), control_flow::exit_shell};
}

// One `export NAME='VALUE'` or `readonly NAME='VALUE'` line, quoted so the shell
// reads it back as exactly these bytes.
//
// POSIX requires the no-operand form of both builtins to print in a form that can
// be RE-INPUT, which is what export-p.tst's `e="$(export -p)"; eval "$e"` round
// trip checks. A name that is MARKED BUT UNSET prints bare - `readonly x`,
// `export x` - and printing `x=''` for it would create the variable on re-input,
// naming something that does not exist. dash prints both forms bare (#71).
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
// The one option `export` and `readonly` have between them (#148). POSIX gives
// both utilities the same `-p` and nothing else, so they share a table for the
// same reason they share a body: two of them would be two chances to disagree.
//
// CLUSTERING COMES WITH THE TABLE, and it is a fix rather than a side effect.
// The loop this replaces compared the WHOLE WORD with `-p`, so `export -pp` was
// `Illegal option -pp` here while dash, bash and zsh all read it as `-p` given
// twice. Harmless today because there is one letter; a live bug the day there
// are two.
struct declaration_opts {
	bool print_only = false;  // -p
};

constexpr auto kDeclaration = args::spec<declaration_opts>(
	args::option{'p', args::field<&declaration_opts::print_only>}
		.help("write the declarations in a form that can be re-input"));

builtin_result run_declaration(shell_state& state, char** argv, bool make_readonly) {
	const std::string_view keyword{argv[0]};
	// `--` ends the options: readonly-p.tst's 'separator preceding operand' is
	// `readonly -- a=foo`, which without it assigned to a name of `--`. The table
	// answers that once, for every utility, rather than once per loop.
	const auto parsed = args::parse(kDeclaration, argv);
	if (parsed.err)
		return {report_option_error(keyword, parsed.err)};
	const bool print_only = parsed.opts.print_only;
	size_t i = static_cast<size_t>(parsed.rest - argv);

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
			report("%.*s: %.*s: bad variable name",
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
			// `export name` is not an assignment: it is allowed on a readonly
			// variable, and it must not create a value for an unset one. POSIX marks
			// the name "whether or not it is set", the same rule mark_readonly obeys
			// two lines below - which is why both go through a mark_ call and not
			// through set() (#71).
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

// WHICH TABLE `unset` REACHES, as ONE field with two letters writing it - which
// is `cd`'s -L/-P shape, and gives last-one-wins for free: `unset -fv x` unsets a
// variable and `unset -vf x` a function, for the same reason `x = 1; x = 2` is 2.
//
// THIS FIELD IS THE WHOLE OF #150. The tree read `unset`'s command line TWICE by
// two different rules: this builtin's own loop, which rejected any option but
// `-v`, and `unset_selects_functions`, which asked `arg.find('f') != npos` - a
// SUBSTRING SEARCH over the whole word, with no notion of which letters `unset`
// actually has. So `unset -qf f` selected the function table by the rule that had
// never heard of `-q`, and the rule that would have rejected `-q` was never
// consulted, because the first one had already returned. Two readings of one
// command line, free to disagree, and they did: dash, bash and zsh all refuse
// `-q` and lesh silently removed the function at status 0.
//
// There is one reading now and it is this table, so the disagreement has nowhere
// to live. That is the same argument the registry `static_assert` (#35) makes one
// layer up.
enum class unset_target : std::uint8_t {
	variables,  // -v, and the default
	functions,  // -f
};

struct unset_opts {
	unset_target target = unset_target::variables;
};

constexpr auto kUnset = args::spec<unset_opts>(
	args::option{'f', args::field<&unset_opts::target>, unset_target::functions}
		.help("unset functions rather than variables"),
	args::option{'v', args::field<&unset_opts::target>, unset_target::variables}
		.help("unset variables, which is the default"));

builtin_result builtin_unset(shell_state& state, char** argv) {
	const auto parsed = args::parse(kUnset, argv);
	if (parsed.err)
		return {report_option_error("unset", parsed.err)};
	size_t i = static_cast<size_t>(parsed.rest - argv);

	// `unset -f name...` removes FUNCTIONS. It used to live in the executor, because
	// the function table did; #106 moved the table to shell state, and try_run_builtin
	// is handed shell state, so the interception the executor kept for this one form
	// has nothing left to do.
	//
	// POSIX: unsetting a name that is not a function is NOT an error, which is why
	// there is no diagnostic and no status but zero. The operands are deliberately NOT
	// validated as names either - `unset -f 1bad` succeeds silently in dash.
	if (parsed.opts.target == unset_target::functions) {
		for (; argv[i] != nullptr; ++i)
			state.unset_function(argv[i]);
		return {0};
	}

	int status = 0;
	for (; argv[i] != nullptr; ++i) {
		// AN OPERAND THAT IS NOT A NAME, refused before anything is unset (#73).
		//
		// `unset 1bad` reported 0 and said nothing - accepted, and silently
		// achieving nothing, which is the stub-that-succeeds shape this project has
		// now paid for seven times. dash reports and exits 2; bash, zsh and yash all
		// diagnose it too.
		//
		// THE RULE WAS ALREADY WRITTEN TWO FUNCTIONS ABOVE. run_declaration refuses
		// exactly this word for `export` and `readonly`, with is_name and this
		// wording, and `unset` is the third utility whose operand POSIX spells NAME.
		// Sharing the predicate is the whole of the fix; a second spelling of "is
		// this a name" is what #63 was opened to end.
		//
		// The `-f` form never reaches here - it returned above - and it is
		// deliberately NOT validated: `unset -f 1bad` succeeds silently in dash, so
		// a check on every operand of every form would have been a fix past the bug.
		if (!is_name(argv[i])) {
			report("unset: %s: bad variable name", argv[i]);
			return {2};
		}
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

// `set`'s table, and the hardest one in the tree (#148).
//
// THE ROWS BIND THE OPTION STATE ITSELF. Every POSIX shell option is already a
// `bool` field of `shell_state::options`, which is exactly what a `toggle` row
// binds - so `set -x` and `set +x` are one row writing one field, and the deviant
// the research note kept setting aside (`+x` is not an option word anywhere else)
// becomes the most ordinary thing in the table.
//
// A scan struct DERIVES from the option state rather than copying it field by
// field: `-o NAME` needs somewhere to put the name, a spec requires every row to
// bind one struct, and eleven hand-written copies in and eleven out would be
// eleven chances to forget the twelfth. Seeding is then one assignment and
// writing back is one assignment.
//
// `set -h` HAS NO `-o` SPELLING and `pipefail`, `vi`, `nolog` and `ignoreeof`
// have no letter; shell_state::option_table() is where that asymmetry lives, and
// `apply_option_name` is still what reads it. The letters are the rows.
struct set_scan : shell_state::options {
	// `-o NAME` / `+o NAME`. A default-constructed view has a NULL data pointer and
	// `set -o ''` gives an empty one that is not null, so the two stay apart - and
	// `set -o ''` keeps reaching `apply_option_name`, which refuses it.
	std::string_view name{};
};

// `shell_option<&shell_state::options::trace>` - one row's binding.
//
// `&shell_state::options::trace` has type `bool options::*` however it is spelled,
// including through the derived struct ([expr.unary.op]/4), and a spec requires
// every row to bind ONE struct. The base-to-derived conversion is the language's
// own way of saying "the same field, seen from here", and it is a constant
// expression, so the row is still built at compile time.
template <bool shell_state::options::*Member>
inline constexpr auto shell_option = args::field<static_cast<bool set_scan::*>(Member)>;

constexpr auto kSet = args::spec<set_scan>(
	args::option{'a', shell_option<&shell_state::options::all_export>, args::toggle}
		.help("export every name that is assigned"),
	args::option{'b', shell_option<&shell_state::options::notify>, args::toggle}
		.help("report terminated background jobs at once (recorded, inert)"),
	args::option{'C', shell_option<&shell_state::options::no_clobber>, args::toggle}
		.help("refuse to truncate an existing file with >"),
	args::option{'e', shell_option<&shell_state::options::exit_on_error>, args::toggle}
		.help("exit when a command fails"),
	args::option{'f', shell_option<&shell_state::options::no_glob>, args::toggle}
		.help("disable pathname expansion"),
	args::option{'h', shell_option<&shell_state::options::hash_all>, args::toggle}
		.help("hash commands as functions are defined (recorded, inert)"),
	args::option{'m', shell_option<&shell_state::options::monitor>, args::toggle}
		.help("job control (recorded, inert)"),
	args::option{'n', shell_option<&shell_state::options::no_exec>, args::toggle}
		.help("read commands without executing them"),
	args::option{'u', shell_option<&shell_state::options::error_on_unset>, args::toggle}
		.help("treat an unset parameter as an error"),
	args::option{'v', shell_option<&shell_state::options::verbose>, args::toggle}
		.help("echo input lines as they are read"),
	args::option{'x', shell_option<&shell_state::options::trace>, args::toggle}
		.help("trace commands as they are executed"),
	args::option{'o', args::field<&set_scan::name>, args::value("NAME")}
		.plus_sigil()
		.help("set the option NAME names; with no NAME, list the options"));

builtin_result builtin_set(shell_state& state, char** argv) {
	// The scan IS the option state, seeded from it. A word that is refused half way
	// therefore leaves the letters it had already applied applied - `set -x -Z`
	// turns on xtrace and then reports - which is what the loop this replaces did,
	// and what dash does.
	set_scan scan;
	static_cast<shell_state::options&>(scan) = state.opts();

	// ONE OPTION WORD AT A TIME, because `-o NAME` is applied AS IT ARRIVES. A
	// string_view field holds one name and `set -o allexport -o noglob` gives it
	// two - dash applies both, and so does this. The sigil is per occurrence for
	// the same reason: `set -o errexit +o xtrace` turns one on and the other off,
	// and a stored view cannot remember which sigil it arrived under.
	char** cur = argv;
	bool separator = false;
	for (;;) {
		scan.name = {};
		const option_word step = next_option_word(kSet, cur, scan);
		state.opts() = scan;
		if (step.err) {
			if (step.err.kind == args::error_kind::missing_argument &&
			    step.err.letter == 'o') {
				// A BARE `set -o` LISTS. `next_option_word` widens its window only
				// when argv really does hold a next word, so a missing argument to
				// `-o` can only mean there is none - which is exactly the condition
				// the loop tested as `argv[i + 1] == nullptr`. dash prints the verbose
				// form for `-o` and the re-inputtable form for `+o`, and set-p.tst's
				// round trip depends on the second.
				if (cur[1][0] == '-')
					print_options_verbose(state.opts());
				else
					print_options_reinputtable(state.opts());
				cur += 1;
				break;
			}
			return {report_option_error("set", step.err)};
		}
		if (scan.name.data() != nullptr) {
			// One option table, shared with command-line parsing
			// (runtime/invocation.h), so `set -o monitor` and `sh -o monitor` cannot
			// disagree about which names exist.
			//
			// An unknown name is an ERROR, not something to shrug at. The return
			// value was once discarded with a `(void)`, so `set -o bogus` succeeded
			// silently - and `set` is a special builtin, so dash both reports and
			// exits a non-interactive shell. Status 2 and the wording are dash's.
			if (!shell_state::apply_option_name(scan, scan.name, cur[1][0] == '-')) {
				report("set: Illegal option %co %.*s", cur[1][0],
				       static_cast<int>(scan.name.size()), scan.name.data());
				return {2};
			}
			state.opts() = scan;
		}
		cur += step.consumed;
		if (step.done) {
			separator = step.separator;
			break;
		}
	}

	// Only replace the positional parameters when operands were actually given -
	// bare `set -e` must not clear them, which is why this is conditional rather
	// than unconditional. A bare `set --` DOES clear them, which is why the
	// separator is a fact the driver hands back rather than one re-read from argv.
	size_t i = static_cast<size_t>(cur + 1 - argv);
	if (argv[i] != nullptr || separator) {
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

// EVERY non-zero status `trap` reports comes from one place: a condition operand
// that names no signal on this platform. ISSUE #66 is that the shell then treated
// it as a reason to exit.
//
// POSIX XCU 2.8.1 makes a special builtin's failure fatal to a non-interactive
// shell for a UTILITY SYNTAX ERROR, which that table spells out as "option or
// operand error" - a command line that is not the shape the utility accepts. This
// is not one. `INT`, `URG` and `9` are all well-formed conditions; whether this
// platform HAS them is a runtime lookup, the same class of question as `cd` on a
// directory that is not there, and `trap` reports it the way any utility reports
// an operation it could not perform.
//
// The platform is the argument that settles it. SIGURG, SIGINFO and SIGPWR are not
// present everywhere, so a shell that made an unknown name fatal would have made
// 2.8.1's own rule depend on which kernel the script was running under - and it
// ended `trap "" "" || echo reached` at a line whose author had written down that
// the failure was expected. dash, bash, zsh and yash all report and carry on.
//
// Returned on the success path too, where the field is not read: the alternative
// is for each `return` to decide, and the point of putting the answer in one
// function is that `trap` has only one answer to give.
[[nodiscard]] constexpr builtin_result trap_condition_status(int status) noexcept {
	return {status, control_flow::normal, 1, failure_kind::operational};
}

// `trap`'s one option. The OPERAND grammar after it - a lone `-` for reset, a
// numeric first operand making every operand a condition - is untouched and stays
// below; only the option scan moved into the table.
struct trap_opts {
	bool print_defaults = false;  // -p
};

constexpr auto kTrap = args::spec<trap_opts>(
	args::option{'p', args::field<&trap_opts::print_defaults>}
		.help("include conditions left at their default disposition"));

builtin_result builtin_trap(shell_state& state, char** argv) {
	signal_state& sigs = state.signals();

	// Options first. `-p` prints defaults too; `--` ends the options, which is what
	// lets `trap -- '- trapped' USR1` set a command that starts with a hyphen.
	//
	// AN OPTION `trap` DOES NOT HAVE is a usage error (#73), and the table is now
	// what says so: the loop this replaces broke out here, so `-Z` silently became
	// the ACTION STRING and `trap -Z x INT` went on to read `x` as a condition and
	// report `x: bad signal` at status 1 - a diagnostic about the wrong operand,
	// and not fatal. POSIX XCU 2.8.1 makes it a UTILITY SYNTAX ERROR, which in a
	// SPECIAL builtin exits a non-interactive shell.
	//
	// A BARE HYPHEN is `trap`'s reset ACTION and not an option, and every
	// `trap - SIG` in both test suites depends on it - the parser knows a one-
	// character word is an operand, so this no longer needs its own guard.
	const auto parsed = args::parse(kTrap, argv);
	if (parsed.err)
		return {report_option_error("trap", parsed.err)};
	const bool include_default = parsed.opts.print_defaults;
	size_t i = static_cast<size_t>(parsed.rest - argv);

	// `trap`, `trap -p` and `trap -p SIG...` all PRINT rather than set. Only the
	// presence of an action operand makes this a setting call, and after `-p` there
	// is no action - the operands are conditions to report.
	if (argv[i] == nullptr || include_default) {
		if (argv[i] == nullptr) {
			for (int signo = 0; signo < kMaxSignal; ++signo)
				print_trap(sigs, signo, include_default);
			return trap_condition_status(0);
		}
		int status = 0;
		for (; argv[i] != nullptr; ++i) {
			const int signo = signal_state::signal_number(argv[i]);
			if (signo < 0) {
				report("trap: %s: bad signal", argv[i]);
				status = 1;
				continue;
			}
			print_trap(sigs, signo, include_default);
		}
		// The listing form reaches the same lookup as the setting form below and takes
		// the same answer for the same reason; a condition that named no signal must
		// not depend on which spelling of `trap` asked about it.
		return trap_condition_status(status);
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
			report("trap: %s: bad signal", argv[s]);
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
	return trap_condition_status(status);
}

// 2, as for any other builtin's usage error, and what dash answers for every
// malformed `kill` line. Distinct from 1, which this builtin keeps for a kill(2)
// the SYSTEM refused: the command was well formed and the shell did ask.
constexpr int kKillUsageError = 2;

builtin_result kill_usage() {
	report("kill: usage: kill -s signal_name pid... | "
	       "kill -signal_name pid... | kill -l [exit_status]");
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
	// Both ways the parse can fail take the SAME answer here, because both describe
	// an operand that names no signal: `kill -l notanumber` and
	// `kill -l 99999999999999999999` are the one diagnostic the reading below would
	// reach anyway, arrived at without overflowing an int on the way (#63).
	const numeric_result parsed = parse_integer(operand, numeric_site::kill_list_operand);
	if (parsed.status != numeric_parse::ok) {
		report("kill: %s: not a signal number or exit status",
		       operand);
		return {kKillUsageError};
	}
	const int value = static_cast<int>(parsed.value);
	const int signo = value > 128 ? value - 128 : value;
	const std::string_view name = signal_state::signal_name(signo);
	if (signo <= 0 || name.empty()) {
		report("kill: %s: not a signal number or exit status",
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
	// A NEGATIVE pid names a process group, and POSIX writes it after `--` -
	// `kill -s HUP -- -$pgid`, which is the form kill4-p.tst uses. A bare `-$pgid`
	// is an option as far as the option scan is concerned, and is refused there.
	// The surrounding blanks and the leading `+` are dash's tolerances, copied
	// deliberately rather than tightened, and they live in the policy table's
	// `kill_pid_operand` row now that every site's tolerances are stated together.
	const numeric_result parsed = parse_integer(operand, numeric_site::kill_pid_operand);
	if (parsed.status != numeric_parse::ok)
		return false;   // dash refuses a pid it cannot hold rather than wrapping
	out = static_cast<pid_t>(parsed.value);
	return true;
}

// The second seam between a representational range and a platform type. The table
// says a pid is an `int`, which it is everywhere lesh builds; a platform with a
// wider pid_t would merely be refused a pid it could have held, and one with a
// NARROWER pid_t would be handed a value that does not fit - so only that
// direction is an error.
static_assert(std::numeric_limits<pid_t>::max() >=
                  policy_for(numeric_site::kill_pid_operand).high,
              "a pid_t must hold every value the kill_pid_operand range admits");

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
// `kill`'s two real options. `-SIGNAME` and `-9` are NOT among them: they are
// signals standing in option position, which is why yash and zsh both refuse to
// route `kill` through their shared parsers at all (research S4.1, S4.2).
//
// THIS TREE ROUTES IT, and the fall-through is explicit rather than a hole in a
// loop: the table owns `-l`, `-s NAME` in both attachment forms, `--` and the
// operand boundary, and a word the table REFUSES is re-read as the signal. That
// is a per-utility policy on ONE error kind - unknown_option - and `kill` is the
// only utility in the tree that has it. Everything else the table refuses (a
// missing argument to `-s`) is still a usage error.
struct kill_opts {
	bool list = false;            // -l
	std::string_view signal{};    // -s NAME, attached or separate
};

constexpr auto kKill = args::spec<kill_opts>(
	args::option{'l', args::field<&kill_opts::list>}
		.help("write the signal names rather than send one"),
	args::option{'s', args::field<&kill_opts::signal>, args::value("SIGNAL")}
		.help("the signal to send, by name or by number"));

builtin_result builtin_kill(shell_state&, char** argv) {
	const char* signal_operand = nullptr;
	bool list = false;
	// POSIX XCU 1.4: `--` ends the options. `first_operand` cannot serve here - it
	// answers for a utility whose `--` can only be argv[1], and `kill`'s comes
	// after the signal option - which is why this walks the words.
	char** cur = argv;
	for (;;) {
		kill_opts o;
		const option_word step = next_option_word(kKill, cur, o);
		if (step.err) {
			if (step.err.kind == args::error_kind::unknown_option) {
				// `-TERM`, `-15`, and also `kill -x 1` or `kill -n 9 $$`: the word is
				// taken as a signal and VALIDATED below, so an option this tree does
				// not have is refused by name rather than silently taken for a pid.
				signal_operand = cur[1] + 1;
				cur += 1;
				continue;
			}
			// `kill -s` with nothing after it names no signal. It used to fall
			// through to the `-NAME` reading, which took the `s` for the name and
			// reported `s: bad signal` - a diagnostic about the wrong thing.
			return kill_usage();
		}
		if (o.list)
			list = true;
		if (!o.signal.empty())
			signal_operand = o.signal.data();  // a view into argv, nul-terminated
		cur += step.consumed;
		if (step.done)
			break;
	}
	size_t i = static_cast<size_t>(cur + 1 - argv);

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
			report("kill: %s: bad signal", signal_operand);
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
			report("kill: %s: not a process id", argv[i]);
			status = kKillUsageError;
			continue;
		}
		if (::kill(pid, signo) != 0) {
			report("kill: %s: %s", argv[i], std::strerror(errno));
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

// `read`'s options. `-d` is POSIX Issue 8 and the reason read-p.tst scores 32
// rather than 31 on a shell that has it: dash predates the addition and fails
// that one case. The divergence is deliberate and recorded, per ADR-0001.
//
// The delimiter is a VIEW rather than a char, because that is what the table can
// store; `read` takes its first byte. A default of `\n` cannot live in the view -
// an absent `-d` and `-d ''` have to stay distinguishable, and the second means
// NUL - so the view starts unset and the newline is applied where it is read.
struct read_opts {
	bool raw = false;                 // -r
	std::string_view delimiter{"\n"}; // -d
};

constexpr auto kRead = args::spec<read_opts>(
	args::option{'r', args::field<&read_opts::raw>}
		.help("do not treat a backslash as an escape"),
	args::option{'d', args::field<&read_opts::delimiter>, args::value("DELIM")}
		.help("read up to DELIM's first byte rather than to a newline"));

builtin_result builtin_read(shell_state& state, char** argv) {
	// POSIX: reads ONE line, splits it on IFS, and assigns to the named variables
	// with the LAST one receiving everything that remains - which is what makes
	// `read first rest` work.
	const auto parsed = args::parse(kRead, argv);
	if (parsed.err)
		return {report_option_error("read", parsed.err)};
	const bool raw = parsed.opts.raw;
	// The delimiter is the rest of the word (`-d:`) or the next one (`-d :`) - the
	// table takes both, as XBD 12.2 Guideline 7 requires and as this loop already
	// did. An EMPTY one means NUL, as it does in bash.
	const char delimiter = parsed.opts.delimiter.empty() ? '\0' : parsed.opts.delimiter.front();
	size_t first = static_cast<size_t>(parsed.rest - argv);

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
			report("read: %s: bad variable name", argv[first + v]);
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
		report("getopts: usage: getopts optstring name [arg...]");
		return 2;
	}
	const std::string_view name{argv[2]};
	if (!is_name(name)) {
		report("getopts: %.*s: bad variable name",
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
	if (std::string_view text; state.lookup("OPTIND", text)) {
		// CLAMPED RATHER THAN WRAPPED, which is this site's policy and not the
		// mechanism's: `OPTIND=99999999999999999999` is a value the parse below only
		// ever compares against $#, and an overflowed size_t would compare as SMALL -
		// so a clamp at the top of the range lands exactly where the "options are
		// exhausted" branch below wants it. A non-numeric OPTIND reads as 0 and is
		// restarted at 1 by the check underneath, which is where that answer already
		// lived.
		const numeric_result parsed = parse_integer(text, numeric_site::optind);
		index = parsed.status == numeric_parse::not_a_number
		        ? 0 : static_cast<size_t>(parsed.value);
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
			report("getopts: illegal option -%c", option);
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
				report("getopts: option requires an argument -%c",
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
	// `--` as the FIRST argument is discarded. POSIX XCU 1.4 requires it of a utility
	// with no options that takes operands - "Standard utilities that do not accept
	// options, but that do accept operands, shall recognize '--' as a first argument
	// to be discarded" - and `alias` is exactly that shape: OPTIONS is "None." and
	// the operands are `alias-name[=string]`.
	//
	// dash and bash both report `--` as an alias that is not found, so this is a
	// DELIBERATE DIVERGENCE from the reference shell rather than a match for it; zsh
	// and yash conform. It is what `alias >f; unalias -a; eval alias -- $(cat f)`
	// needs, which is how alias-p.tst:93 asserts the listing is re-inputtable - a
	// name beginning with `-` would otherwise be read as an option by a shell that
	// has one.
	const size_t first = first_operand(argv);

	// No operands: every alias, sorted. It printed NOTHING before - a listing that
	// produces no output fails every case that inspects one, and it made `unalias -a`
	// look correct while doing nothing (#40).
	if (argv[first] == nullptr) {
		for (const auto& row : state.aliases())
			print_alias(row.name, row.value);
		return {0};
	}
	int status = 0;
	for (size_t i = first; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		// The `=` is searched from the SECOND byte, so `=foo` is a lookup of an alias
		// named `=foo` rather than an assignment to the empty name. dash does the
		// same, and POSIX gives `alias` no options at all - which is why `-p` is an
		// operand here and not a flag, and is reported as not found.
		if (const size_t eq = arg.find('=', 1); eq != std::string_view::npos) {
			state.set_alias(arg.substr(0, eq), arg.substr(eq + 1));
		} else if (std::string_view value; state.lookup_alias(arg, value)) {
			print_alias(arg, value);
		} else {
			report("alias: %.*s: not found",
			       static_cast<int>(arg.size()), arg.data());
			status = 1;
		}
	}
	return {status};
}

// --- `bind` ----------------------------------------------------------------
//
// zsh's `bindkey`, spelled the way #117 decided: `-l` lists keymaps, `-N`
// creates one (optionally as a copy), `-m` selects which keymap the operands
// apply to. There is no map-vs-noremap pair, because a binding names an ACTION
// and never a key sequence, so there is nothing for a second form to
// distinguish - the whole of vim's `:map` family collapses to one verb here.
//
// It reaches the keymap registry through `binding_console` and by no other
// route; see the note in builtins.h for why that indirection exists rather than
// a direct call.
builtin_result bind_without_an_editor() {
	// Not a usage error: the command line was right and there was nowhere to
	// apply it. `failure_kind::operational` keeps `bind ... || true` from ending
	// a non-interactive script, which is the shape an rc file guarded for both
	// kinds of shell is written in.
	report("bind: no line editor in this shell");
	return {1, control_flow::normal, 1, failure_kind::operational};
}

int report_bind_outcome(binding_console::outcome what, std::string_view subject) {
	switch (what) {
	case binding_console::outcome::ok:
		return 0;
	case binding_console::outcome::no_such_keymap:
		report("bind: %.*s: no such keymap",
		       static_cast<int>(subject.size()), subject.data());
		return 1;
	case binding_console::outcome::no_such_action:
		report("bind: %.*s: no such action",
		       static_cast<int>(subject.size()), subject.data());
		return 1;
	case binding_console::outcome::bad_notation:
		report("bind: %.*s: not a key sequence",
		       static_cast<int>(subject.size()), subject.data());
		return 1;
	}
	return 1;
}

// `bind`'s table. It is leshper's builtin rather than POSIX's, and the third
// consumer the substrate placement of lesh::args was argued from (#148).
struct bind_opts {
	bool list_keymaps = false;   // -l
	std::string_view keymap{};   // -m KEYMAP
	std::string_view create{};   // -N NAME
};

constexpr auto kBind = args::spec<bind_opts>(
	args::option{'l', args::field<&bind_opts::list_keymaps>}
		.help("write the keymap names"),
	args::option{'m', args::field<&bind_opts::keymap>, args::value("KEYMAP")}
		.help("act on KEYMAP rather than the current one"),
	args::option{'N', args::field<&bind_opts::create>, args::value("NAME")}
		.help("create keymap NAME, optionally copying an existing one"));

builtin_result builtin_bind(shell_state& state, char** argv) {
	binding_console* console = state.console();

	// Options first, POSIX-style, and `--` ends them. Read before the console is
	// consulted so that a malformed command line is a malformed command line in
	// every shell, interactive or not.
	//
	// `bind -memacs` NOW WORKS, and did not before: the loop this replaces compared
	// whole words, so `-m` took the next word and the attached spelling every other
	// option-taking utility in the tree accepts was an unknown option. XBD 12.2
	// Guideline 7 gives a row no say in which spelling it takes, which is half of
	// what the thirteen loops disagreed about.
	const auto parsed = args::parse(kBind, argv);
	if (parsed.err)
		return {report_option_error("bind", parsed.err)};
	const std::string_view keymap = parsed.opts.keymap;
	const bool list_keymaps = parsed.opts.list_keymaps;
	const std::string_view create = parsed.opts.create;
	size_t at = static_cast<size_t>(parsed.rest - argv);

	if (list_keymaps && (!create.empty() || argv[at] != nullptr)) {
		report("bind: -l takes no operands");
		return {2};
	}
	if (console == nullptr)
		return bind_without_an_editor();

	if (list_keymaps) {
		std::vector<std::string> names;
		console->keymap_names(names);
		for (const std::string& one : names)
			std::printf("%s\n", one.c_str());
		return {0};
	}

	if (!create.empty()) {
		// `bind -N new [from]`: the copy operand is optional, and a second one is
		// a mistake rather than something silently ignored.
		const std::string_view from = argv[at] != nullptr ? std::string_view{argv[at]}
		                                                  : std::string_view{};
		if (argv[at] != nullptr && argv[at + 1] != nullptr) {
			report("bind: -N takes at most one keymap to copy");
			return {2};
		}
		const binding_console::outcome what = console->create_keymap(create, from);
		return {report_bind_outcome(what, from)};
	}

	// No operands: list what the keymap holds, the way `alias` with no operands
	// lists every alias - and in the same re-inputtable shape, so that
	// `bind -m emacs > f` and reading `f` back rebuilds the table.
	if (argv[at] == nullptr) {
		std::vector<std::pair<std::string, std::string>> bindings;
		const binding_console::outcome what = console->list_bindings(keymap, bindings);
		if (what != binding_console::outcome::ok)
			return {report_bind_outcome(what, keymap)};
		for (const auto& [keys, action] : bindings)
			std::printf("%s %s\n", keys.c_str(), action.c_str());
		return {0};
	}

	const std::string_view keys{argv[at]};
	if (argv[at + 1] == nullptr) {
		// One operand: a query. Prints nothing for an unbound sequence and
		// answers 1, so `bind '<C-w>' > /dev/null` is a test.
		std::string action;
		const binding_console::outcome what = console->lookup_key(keymap, keys, action);
		if (what != binding_console::outcome::ok)
			return {report_bind_outcome(what, what == binding_console::outcome::no_such_keymap
			                                      ? keymap : keys)};
		if (action.empty())
			return {1};
		std::printf("%.*s %s\n", static_cast<int>(keys.size()), keys.data(), action.c_str());
		return {0};
	}

	if (argv[at + 2] != nullptr) {
		report("bind: too many operands");
		return {2};
	}
	const std::string_view action{argv[at + 1]};
	const binding_console::outcome what = console->bind_key(keymap, keys, action);
	if (what == binding_console::outcome::no_such_keymap)
		return {report_bind_outcome(what, keymap)};
	if (what == binding_console::outcome::no_such_action)
		return {report_bind_outcome(what, action)};
	return {report_bind_outcome(what, keys)};
}

// --- `prompt` --------------------------------------------------------------
//
// `bind`'s shape, one console over (#157, spec §6.10). Where `bind` is the rc
// surface for the keymap registry, this is the rc surface for the prompt engine,
// and it reaches that engine through `prompt_console` and by no other route -
// same link boundary, same reason, and the header's note on `binding_console`
// covers both. This file includes no leshper header, and the link graph is what
// enforces that rather than a reviewer: `lesh_runtime` does not link
// `lesh_leshper`, so a direct call would not build.
//
// THE WHOLE COMMAND LINE, since it is small enough to write down:
//
//     prompt                 write the left prompt's template
//     prompt -c              write the continuation prompt's template
//     prompt TEMPLATE        set the left prompt
//     prompt -c TEMPLATE     set the continuation prompt
//     prompt -l              write the registered module names, sorted
//     prompt -r              put the shipped default back on BOTH prompts
//
// A GETTER AND A SETTER ON THE SAME WORD, which is `bind`'s arrangement too and
// deliberately not a `-p` for the reading form: a prompt has exactly one
// configuration per surface, so the operand's presence already says which of the
// two was meant, and an option to say it again could only ever disagree.
//
// `-r` TAKES NO SURFACE ON PURPOSE. It is the undo, and the thing being undone
// is a session's worth of rc-file configuration across both prompts; a `-r -c`
// that reset only half would leave a shell in a state no rc file ever put it in,
// which is a worse thing to be able to reach than a slightly blunter reset. So
// `-c` with `-r` is a usage error rather than a narrowing, and the same for
// `-l`, which reads a registry that has no surface at all.

builtin_result prompt_without_an_editor() {
	// `bind`'s answer, word for word and status for status. A non-interactive
	// shell has no prompt engine for the same reason it has no keymaps, and an rc
	// file guarded for both kinds of shell must not die here - so this is an
	// OPERATIONAL failure and not a usage error.
	report("prompt: no line editor in this shell");
	return {1, control_flow::normal, 1, failure_kind::operational};
}

// One switch for the console's whole outcome space, `report_bind_outcome`'s
// opposite number.
//
// The switch is TOTAL over an enum of two, and it stays a switch rather than a
// bool for the same reason the builtin registry has a static_assert: the day a
// verb is added to the console with an outcome of its own, the compiler names
// this function rather than letting the new row fall through to a status with
// no diagnostic. (It once had four rows; the two for the per-element assembly
// verbs went with the verbs - see the enum's own note.)
//
// `detail` is the console's own sentence for `bad_template` and is printed
// verbatim: the parser lives across the boundary, so only it can say what was
// wrong with the bytes.
int report_prompt_outcome(prompt_console::outcome what, std::string_view detail) {
	switch (what) {
	case prompt_console::outcome::ok:
		return 0;
	case prompt_console::outcome::bad_template:
		report("prompt: %.*s", static_cast<int>(detail.size()), detail.data());
		return 1;
	}
	return 1;
}

struct prompt_opts {
	bool continuation = false;   // -c
	bool list_modules = false;   // -l
	bool reset = false;          // -r
};

constexpr auto kPrompt = args::spec<prompt_opts>(
	args::option{'c', args::field<&prompt_opts::continuation>}
		.help("act on the continuation prompt rather than the left one"),
	args::option{'l', args::field<&prompt_opts::list_modules>}
		.help("write the names of the modules a template may place"),
	args::option{'r', args::field<&prompt_opts::reset>}
		.help("put the shipped default back on both prompts"));

builtin_result builtin_prompt(shell_state& state, char** argv) {
	// Options first and `--` ends them, so `prompt -- -c` sets a prompt whose
	// template begins with a hyphen. Parsed through lesh::args like every other
	// builtin here (#155), which is also what makes `prompt -cr` mean what a
	// reader of any other utility expects.
	const auto parsed = args::parse(kPrompt, argv);
	if (parsed.err)
		return {report_option_error("prompt", parsed.err)};
	const prompt_opts opts = parsed.opts;
	const size_t at = static_cast<size_t>(parsed.rest - argv);
	const bool has_operand = argv[at] != nullptr;

	// THE COMMAND LINE IS CHECKED BEFORE THE CONSOLE IS ASKED FOR, `bind`'s order
	// and for `bind`'s reason: a malformed command line has to be malformed in
	// every shell, or an rc file would be validated only by the interactive one
	// that runs it and a typo would sit unnoticed in the non-interactive path.
	if (opts.list_modules && opts.reset) {
		report("prompt: -l and -r are separate commands");
		return {2};
	}
	if (opts.list_modules || opts.reset) {
		const char* const verb = opts.list_modules ? "-l" : "-r";
		if (has_operand) {
			report("prompt: %s takes no operands", verb);
			return {2};
		}
		if (opts.continuation) {
			report("prompt: -c has no meaning with %s", verb);
			return {2};
		}
	}
	if (has_operand && argv[at + 1] != nullptr) {
		// ONE operand, and a second is a mistake rather than something ignored: a
		// template is a single word, so `prompt {path} {git}` is an unquoted one and
		// silently setting it to the first half is the worst of the answers.
		report("prompt: too many operands");
		return {2};
	}

	prompt_console* console = state.prompts();
	if (console == nullptr)
		return prompt_without_an_editor();

	if (opts.list_modules) {
		// The console sorts; this only writes. Sorting here as well would be a
		// second ordering to disagree with the ABI's.
		std::vector<std::string> names;
		console->module_names(names);
		for (const std::string& one : names)
			std::printf("%s\n", one.c_str());
		return {0};
	}

	if (opts.reset) {
		// Both surfaces, and the second is attempted even though the first cannot
		// fail today - `use_default` puts back a table that is `constexpr`, so
		// there is nothing for it to refuse. Written as two checked calls because
		// the alternative is a silent half-reset the day that stops being true.
		const prompt_console::outcome left =
			console->use_default(prompt_console::surface::left);
		if (left != prompt_console::outcome::ok)
			return {report_prompt_outcome(left, {})};
		const prompt_console::outcome carried_on =
			console->use_default(prompt_console::surface::continuation);
		return {report_prompt_outcome(carried_on, {})};
	}

	const prompt_console::surface which = opts.continuation
		? prompt_console::surface::continuation
		: prompt_console::surface::left;

	if (!has_operand) {
		// The SOURCE STRING, not a rendering: `prompt > f` and reading `f` back
		// re-inputs the configuration, which is the round trip `bind -m emacs > f`
		// already promises one seam over. An unconfigured surface answers empty and
		// prints an empty line rather than nothing at all, so the output has one
		// line per invocation whatever the state.
		std::string text;
		console->text(which, text);
		std::printf("%.*s\n", static_cast<int>(text.size()), text.data());
		return {0};
	}

	// The template is parsed ONCE, here, and the swap is the console's to make
	// atomic: a refusal leaves the prompt that was already standing. So there is
	// nothing to undo on this side of the failure - only something to report.
	std::string error;
	const prompt_console::outcome what = console->set(which, argv[at], error);
	return {report_prompt_outcome(what, error)};
}

// `unalias -a`, and the one letter POSIX gives it.
//
// `unalias -Z` USED TO BE AN OPERAND - an alias named `-Z`, reported as not found
// at status 1 - because the loop only ever compared argv[1] with `-a`. POSIX gives
// `unalias` an OPTIONS section, so a word in option position that names no option
// is a usage error, and dash, bash and zsh all say so. Measured at 634e4c8:
//
//     lesh   unalias -Z   unalias: -Z: not found         status 1
//     dash   unalias -Z   unalias: Illegal option -Z     status 2
//     bash   unalias -Z   unalias: -Z: invalid option    status 2
//     zsh    unalias -Z   unalias: bad option: -Z        status 1
//
// `alias` is the opposite case and keeps its operand reading: POSIX gives it no
// options at all ("OPTIONS: None."), so `alias -Z` is a lookup of an alias named
// `-Z` - which is dash's answer too, and what alias-p.tst:93's re-input round trip
// needs.
struct unalias_opts {
	bool all = false;  // -a
};

constexpr auto kUnalias = args::spec<unalias_opts>(
	args::option{'a', args::field<&unalias_opts::all>}.help("remove every alias"));

builtin_result builtin_unalias(shell_state& state, char** argv) {
	// `-a` removes every alias. It used to be looked up as the NAME `-a`, which
	// removed nothing and reported nothing - and the case that asserts it passed
	// anyway, because `alias` printed nothing to compare against. Two silent
	// failures cancelling out is exactly what #38 found in sigurg5-p.tst.
	//
	// POSIX puts the options first and `--` after them, so `unalias -a` is an
	// option and `unalias -- -a` removes an alias whose name is `-a`. The table
	// orders the two without a fast path in front of it.
	const auto parsed = args::parse(kUnalias, argv);
	if (parsed.err)
		return {report_option_error("unalias", parsed.err)};
	if (parsed.opts.all) {
		state.clear_aliases();
		return {0};
	}
	int status = 0;
	for (size_t i = static_cast<size_t>(parsed.rest - argv); argv[i] != nullptr; ++i) {
		// POSIX: removing an alias that does not exist is an ERROR. Returning 0 made
		// `unalias true; unalias true` succeed twice.
		if (!state.unset_alias(argv[i])) {
			report("unalias: %s: not found", argv[i]);
			status = 1;
		}
	}
	return {status};
}

builtin_result builtin_shift(shell_state& state, char** argv) {
	// `shift` IS THE SIXTH UTILITY OF THE `first_operand` SHAPE and was reading
	// argv[1] directly, which is two defects in one line. `shift notanumber` went
	// through std::atoi as 0 and shifted NOTHING while reporting success; and
	// `shift -- 2` sent the separator itself to atoi, got 0, and did the same -
	// where POSIX XCU 1.4 discards a leading `--` for any utility that takes
	// operands and no options (#44's precedent, and shift-p.tst's 'separator
	// preceding operand', which bash, zsh and ksh all pass).
	const size_t operand = first_operand(argv);
	size_t count = 1;
	if (argv[operand] != nullptr) {
		// A shift count is a POSITIVE decimal integer, so the `shift_count` row takes
		// no sign: `shift -1` is a malformed operand rather than a negative count,
		// which is dash's reading and bash's and zsh's alike. Refused rather than
		// clamped, because a count is not an index into anything - there is nothing
		// for a saturated one to land on that `can't shift that many` does not
		// already say better.
		const numeric_result parsed =
			parse_integer(argv[operand], numeric_site::shift_count);
		if (parsed.status != numeric_parse::ok)
			return {report_bad_number("shift", argv[operand], parsed.status)};
		count = static_cast<size_t>(parsed.value);
	}
	if (!state.shift_positional(count)) {
		report("shift: can't shift that many");
		// TWO, the same as the operand that would not parse above it (#73).
		//
		// lesh was the only shell of the four surveyed that gave two different
		// answers to "shift refused": 1 here and 2 there. dash answers 2 for both and
		// bash 1 for both, and nothing separated the two cases here - #63 routed them
		// through the one numeric parser, so the split was a leftover rather than a
		// reading of anything.
		//
		// 2 and not 1, on three counts that agree. ADR-0001 makes dash authoritative
		// for the POSIX floor and dash says 2. Both refusals are already FATAL to a
		// non-interactive shell - `failure_kind::usage`, POSIX XCU 2.8.1's utility
		// syntax error row - so this number is the status the SHELL DIES WITH, and it
		// is the one `shift abc`, `set -Z` and `export 1bad=x` already die with.
		// Taking bash's 1 would have meant taking its non-fatality too, or leaving a
		// shell that exits with the status of a command that merely failed.
		return {2};
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
	const numeric_result parsed = parse_integer(text, numeric_site::loop_flow_level);
	// CLAMPED rather than wrapped, and that is this site's policy: `break 99999999999999`
	// means out of every loop there is, which is what any level past the nesting
	// depth already means ('breaking much more than actual nest level'). Wrapping
	// could land on zero and turn the break back into the no-op this function
	// exists to refuse.
	//
	// A NON-NUMBER READS AS ZERO so it meets the same refusal an explicit `break 0`
	// does: both ask to unwind no levels at all, and one message for the two is
	// what the comment above promises. The `loop_flow_level` row takes no sign,
	// which is what keeps `break -1` on this side of the line rather than letting
	// it through as a negative level.
	const int64_t value =
		parsed.status == numeric_parse::not_a_number ? 0 : parsed.value;
	if (value == 0) {
		report("%.*s: %.*s: not a positive integer",
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
	if (argv[operand] == nullptr)
		return {state.trap_entry_status().value_or(state.last_status()),
		        control_flow::return_from};

	// `return 99999999999999999999` REPORTED -1 through std::atoi, which is not a
	// status any shell can produce and not a number atoi is defined on. Refused
	// now, as dash refuses it, at 2 (#63).
	//
	// A status that FITS is passed through unchanged, negative or over 255 alike:
	// `return 300` is 300 and `return -1` is -1 here, which is what dash and zsh
	// both answer. That is the modulo-256 question and not this one - bash and ksh
	// mask both, dash and zsh mask neither, and masking only the negative half
	// would agree with nobody.
	const numeric_result parsed = parse_integer(argv[operand], numeric_site::return_status);
	if (parsed.status != numeric_parse::ok)
		return {report_bad_number("return", argv[operand], parsed.status)};
	return {static_cast<int>(parsed.value), control_flow::return_from};
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
	{"bind", builtin_bind}, {"prompt", builtin_prompt},
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

// The one seam between the shell's `bind` and leshper's keymap registry.
//
// The POINTER lives on `shell_state` since #134 (see set_binding_console there);
// what is left here is the out-of-line destructor every polymorphic base needs a
// home for. It is deliberately NOT an owner: the interactive wiring site owns the
// editing context and lends this view of it, so ADR-0007's "everything has an
// owner that frees it" is answered on the other side of the boundary. The
// registry itself is reached from `state` and is owned there (spec §6.4).
binding_console::~binding_console() = default;

// And the prompt's, on the same terms (#157, §6.10). It stood alone here while
// the seam had no caller; `builtin_prompt` above is that caller now, and the
// arrangement did not have to move to get one - which was the argument for
// installing the console the day the session started rather than the day
// something asked it a question. The ABI verbs still reach the engine through
// the registry rather than through here, and the two doors stay unlayered.
prompt_console::~prompt_console() = default;

int report_bad_number(std::string_view builtin, std::string_view operand,
                      numeric_parse why) {
	// Worded the way lesh's other builtins word an operand they refuse -
	// `lesh: kill: notanumber: not a process id` - rather than copied from dash,
	// which prefixes a line number lesh does not track. The two failures are named
	// apart because they are different mistakes to make: `exit 3x` is a typo in the
	// operand and `exit 99999999999999999999` is a number the shell cannot hold.
	report("%.*s: %.*s: %s",
	       static_cast<int>(builtin.size()), builtin.data(),
	       static_cast<int>(operand.size()), operand.data(),
	       why == numeric_parse::out_of_range ? "number out of range"
	                                          : "not a number");
	return 2;
}

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

// --- the extension table (#165) ---------------------------------------------
//
// Defined HERE rather than in shell_state.cpp for the reason `classify_builtin`
// is: `kBuiltinRegistry` lives in this translation unit's header, and the
// collision check is the whole content of the setter.

bool shell_state::set_extension_builtins(std::span<const extension_builtin> table) {
	// CORE WINS, AT INSTALL TIME AND WHOLE. A table naming a core builtin is
	// refused entirely rather than filtered: a partially installed set is a shell
	// whose behaviour depends on which row was wrong, and the honest answer to a
	// build that shipped `cd` twice is to run without the extension set and say
	// so. Every name is checked so one report names every collision.
	bool collided = false;
	for (const extension_builtin& one : table) {
		for (const builtin_descriptor& core : kBuiltinRegistry) {
			if (core.name != one.name)
				continue;
			report("leshnici: %.*s is already a shell builtin",
			       static_cast<int>(one.name.size()), one.name.data());
			collided = true;
		}
	}
	if (collided)
		return false;
	_extension_builtins = table.data();
	_extension_builtin_count = table.size();
	return true;
}

std::span<const extension_builtin> shell_state::extension_builtins() const noexcept {
	return {_extension_builtins, _extension_builtin_count};
}

namespace {

// The one lookup, so the three questions below and the dispatch cannot disagree
// about what is visible. Null when the option is off, when nothing is installed,
// or when no row matches.
[[nodiscard]] const extension_builtin* find_extension(const shell_state& state,
                                                      std::string_view name) noexcept {
	if (!state.extension_builtins_enabled())
		return nullptr;
	for (const extension_builtin& one : state.extension_builtins())
		if (one.name == name)
			return &one;
	return nullptr;
}

} // namespace

builtin_kind classify_builtin(const shell_state& state, std::string_view name) noexcept {
	// The CORE first, always: `set_extension_builtins` has already refused any
	// name that could reach this line twice, so the order is a statement rather
	// than a tie-break - and it stays correct if the guard is ever weakened.
	if (const builtin_kind core = classify_builtin(name); core != builtin_kind::none)
		return core;
	// Regular, never special. POSIX 2.14's set is closed and a failing `ls` must
	// not end a script that turned the option on.
	return find_extension(state, name) != nullptr ? builtin_kind::regular
	                                              : builtin_kind::none;
}

bool builtin_has_handler(const shell_state& state, std::string_view name) noexcept {
	return has_handler(name) || find_extension(state, name) != nullptr;
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

bool try_run_builtin(shell_state& state, char** argv, builtin_result& out,
                     bool demoted) {
	if (argv == nullptr || argv[0] == nullptr)
		return false;
	const std::string_view name{argv[0]};

	for (const auto& b : kBuiltins) {
		if (b.name != name)
			continue;
		out = b.fn(state, argv);

		// POSIX 2.8.1: a failing SPECIAL builtin exits a non-interactive shell. The
		// same failure in a regular builtin does not. This is the one place that
		// distinction is applied, so it cannot be forgotten per-builtin.
		//
		// THREE CONDITIONS, NOT ONE, and the two that were missing are #66. The rule
		// asks which builtin (special), who is asking (`command` demotes it, as it
		// already demoted the assignment and the redirection failure) and WHICH ERROR
		// (2.8.1 lists the classes, and an operation that simply failed is on none of
		// them - see failure_kind). Reading only the status made every one of a
		// special builtin's failures a syntax error, which is how an unknown signal
		// name came to end a script at a line that had handled it.
		if (out.flow == control_flow::normal && out.status != 0 && !demoted &&
		    out.failure == failure_kind::usage &&
		    classify_builtin(name) == builtin_kind::special && !state.interactive())
			out.flow = control_flow::exit_shell;
		return true;
	}

	// AFTER the core tables and only when the option is on (#165). No special-
	// builtin rule is applied to what comes back: an extension builtin is regular
	// by construction (see classify_builtin's overload), so the block above has
	// nothing to say about it, and `failure_kind` never turns into an exit.
	//
	// `demoted` is not consulted for the same reason. `command ls` demotes
	// properties only a SPECIAL builtin has; a regular one has neither of them,
	// and the executor has already taken the function table out of the question
	// before it got here.
	if (const extension_builtin* found = find_extension(state, name); found != nullptr) {
		out = found->fn(state, argv);
		return true;
	}
	return false;
}

} // namespace lesh::runtime
