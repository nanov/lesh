#include "runtime/builtins.h"

#include "runtime/executor.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// `cd`: -L and -P, CDPATH, and the failure status. See issue #46. `pwd` and the
// startup value of PWD joined them in #51, because -L and -P mean one thing across
// the two builtins and the shell decides it once.
//
// The shape of the scratch tree is cd-p.tst's, because the interesting cases all
// need a SYMLINK to a directory: -L and -P differ nowhere else, and `cd link` then
// `cd ..` is the observable difference between a logical working directory and a
// physical one.
//
// The tree is built here rather than borrowed from /tmp on purpose. /tmp on macOS
// is a symlink to /private/tmp, which makes `cd -P /tmp` a two-line demonstration
// of -P - and a test that would pass for a different reason, or not at all, on a
// system where /tmp is a real directory.
class CdTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	shell_state state;
	lesh::testing::temp_path scratch;
	std::string root;   // the PHYSICAL pathname of the scratch tree

	void SetUp() override {
		_origin = std::filesystem::current_path();
		// scratch.dir() is already canonical (see temp_path.h) - comparing a -P
		// result against the name the directory was CREATED under, before that
		// resolution, would fail for a reason that has nothing to do with cd.
		root = scratch.dir();
		std::filesystem::create_directories(root + "/real/inner");
		std::filesystem::create_directory_symlink(root + "/real", root + "/link");
		std::ofstream{root + "/file"} << "not a directory\n";
		std::filesystem::create_directories(root + "/first/wanted");
		std::filesystem::create_directories(root + "/second/wanted");
		std::filesystem::create_directories(root + "/here");
		// The shell starts where the tree is, and BELIEVES it is there: PWD is what
		// cd extends a relative operand onto, and a stale one would make every
		// logical answer below wrong in the same direction.
		std::filesystem::current_path(root);
		ASSERT_TRUE(state.set_exported("PWD", root));
	}

	void TearDown() override {
		// The process's working directory is global, and a test that left it inside a
		// deleted temporary directory would make every LATER test in the binary fail
		// in getcwd rather than here. Restoring it BEFORE the fixture (and so
		// `scratch`) is destroyed keeps that ordering; `scratch`'s destructor then
		// removes the tree, tolerant of a test that already removed pieces of it.
		std::filesystem::current_path(_origin);
	}

	int run(std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}

	// Runs `src` with its standard output in a file and returns what it wrote. A
	// redirection rather than freopen, for the reason getopts_tests gives: the
	// shell's own stdio is part of what is measured.
	std::string capture(std::string_view src) {
		const std::string path = root + "/.capture";
		std::remove(path.c_str());
		std::string wrapped{"{ "};
		wrapped.append(src);
		wrapped += "; } > ";
		wrapped += path;
		std::ignore = run(wrapped);
		std::ifstream in{path};
		std::ostringstream text;
		text << in.rdbuf();
		std::remove(path.c_str());
		return text.str();
	}

	[[nodiscard]] std::string value_of(std::string_view name) const {
		std::string_view text;
		return state.lookup(name, text) ? std::string{text} : std::string{"<unset>"};
	}

private:
	std::filesystem::path _origin;
};

} // namespace

TEST_F(CdTest, TheDefaultIsLogicalAndDotDotIsLexical) {
	// #24's answer, kept: PWD follows the path the user NAMED, so the symlink is
	// unfollowed on the way back out. `cd link` then `cd ..` returning to `real`'s
	// parent by luck is not the same thing - here `real` and `link` share a parent,
	// so the test uses `inner` to tell a lexical `..` from a physical one.
	EXPECT_EQ(run("cd link/inner"), 0);
	EXPECT_EQ(value_of("PWD"), root + "/link/inner");
	EXPECT_EQ(run("cd .."), 0);
	EXPECT_EQ(value_of("PWD"), root + "/link");
	EXPECT_EQ(capture("pwd"), root + "/link\n");
}

TEST_F(CdTest, PhysicalModeResolvesTheSymlinkAndReadsPwdBack) {
	// -P is new behaviour BESIDE the logical PWD, not a replacement: the kernel
	// resolves the path and PWD is whatever getcwd then reports.
	EXPECT_EQ(run("cd -P link/inner"), 0);
	EXPECT_EQ(value_of("PWD"), root + "/real/inner");
	// And `..` under -P is the real parent, which is why the mode exists.
	EXPECT_EQ(run("cd -P .."), 0);
	EXPECT_EQ(value_of("PWD"), root + "/real");
}

TEST_F(CdTest, PhysicalOldPwdIsResolvedToo) {
	// cd-p.tst's 'symbolic links are resolved (in old PWD, -P)': a -P cd from a
	// LOGICAL directory resolves the symlinks the previous cd left in PWD, because
	// curpath is built from PWD and then handed to the kernel whole.
	EXPECT_EQ(run("cd -L link"), 0);
	EXPECT_EQ(run("cd -P ./../real/inner"), 0);
	EXPECT_EQ(value_of("PWD"), root + "/real/inner");
}

TEST_F(CdTest, TheLastOfLAndPWins) {
	// POSIX says the last of the two, and cd-p.tst asserts it across groups: `-PL`
	// is two options in one argument, so the scan cannot stop at the first letter.
	EXPECT_EQ(run("cd -P -L -PL link"), 0);
	EXPECT_EQ(value_of("PWD"), root + "/link") << "-PL ends in L, so logical";
	EXPECT_EQ(run("cd -L -P -LP ."), 0);
	EXPECT_EQ(value_of("PWD"), root + "/real") << "-LP ends in P, so physical";
}

TEST_F(CdTest, ADotDotComponentRequiresTheThingBeforeItToBeADirectory) {
	// POSIX cd step 10(b)(i), and the reason the -L canonicalization touches the
	// filesystem at all. dash cancels `file/..` lexically and succeeds, which is
	// what fails it cd-p.tst's 'non-directory file in operand component (-L)': the
	// path it moved to is one no resolution could have walked.
	EXPECT_EQ(run("cd -L ./file/../real 2>/dev/null"), 2);
	EXPECT_EQ(value_of("PWD"), root) << "a failed cd moves nothing";
	EXPECT_EQ(run("cd -L ./_no_such_file_/../real 2>/dev/null"), 2);
	// A symlink TO a directory satisfies it, because the check follows the link -
	// which is what makes `cd link/../real` work while `cd file/../real` does not.
	EXPECT_EQ(run("cd -L link/../real"), 0);
}

TEST_F(CdTest, CdpathIsSearchedLeftToRight) {
	// POSIX step 5. `wanted` exists under both entries, so the FIRST one must win;
	// a search that took the last would pass a test using only one entry.
	EXPECT_EQ(run("CDPATH=" + root + "/first:" + root + "/second; cd wanted >/dev/null"), 0);
	EXPECT_EQ(value_of("PWD"), root + "/first/wanted");
}

TEST_F(CdTest, ACdpathMatchIsPrintedAndAnEmptyEntryIsNot) {
	// The rule shells get wrong, and the one cd-p.tst separates into two cases: a
	// match under a NON-EMPTY entry is written to standard output, because `pwd`
	// afterwards is not enough for a script to find out where it landed. An empty
	// entry means the current directory and prints nothing - and a literal `.`
	// entry, naming the same directory, DOES print.
	EXPECT_EQ(capture("CDPATH=" + root + "/first; cd wanted"), root + "/first/wanted\n");
	EXPECT_EQ(capture("cd " + root + "; CDPATH=:" + root + "/first; cd here"), "");
	EXPECT_EQ(value_of("PWD"), root + "/here");
	EXPECT_EQ(capture("cd " + root + "; CDPATH=.:" + root + "/first; cd here"),
	          root + "/here\n");
}

TEST_F(CdTest, CdpathIsNotSearchedForAnAbsoluteOrDotPrefixedOperand) {
	// POSIX step 4 and step 5: an absolute operand, and one whose FIRST COMPONENT is
	// dot or dot-dot, skip the search. The component, not the character - `.hidden`
	// is an ordinary name.
	EXPECT_EQ(capture("CDPATH=" + root + "/first; cd " + root + "/here"), "");
	EXPECT_EQ(value_of("PWD"), root + "/here");
	EXPECT_EQ(capture("cd " + root + "; CDPATH=" + root + "/first; cd ./here"), "");
	EXPECT_EQ(value_of("PWD"), root + "/here");
}

TEST_F(CdTest, TheCdpathEntrysTrailingSlashDoesNotSurviveCanonicalization) {
	// CDPATH=/ concatenates to `//dev`, and dash cds there and reports it, which is
	// what fails it cd-p.tst's 'cd path ending with slash (-L)'. The canonicalization
	// collapses the empty component, so the answer is the one directory the user
	// meant.
	EXPECT_EQ(capture("CDPATH=" + root + "/; cd real"), root + "/real\n");
	EXPECT_EQ(value_of("PWD"), root + "/real");
}

TEST_F(CdTest, FailureIsStatusTwoAndSparesTheShell) {
	// dash's status, and cd-p.tst's: 'exit status of change error with -e' asserts
	// `[ $? -gt 1 ]`, so 1 is not an acceptable answer for a cd that did not happen.
	// And `cd` is a REGULAR builtin - the failure must not exit a non-interactive
	// shell, which is what the second half measures.
	EXPECT_EQ(classify_builtin("cd"), builtin_kind::regular);
	EXPECT_EQ(run("cd _no_such_path_ 2>/dev/null"), 2);
	EXPECT_EQ(capture("cd _no_such_path_ 2>/dev/null; echo reached"), "reached\n");
	EXPECT_NE(capture("cd _no_such_path_ 2>&1"), "") << "and it says so";
}

TEST_F(CdTest, UsageErrorsAreLoudRatherThanSilentSuccess) {
	// Two deliberate divergences from dash, both recorded in #46. dash ignores every
	// operand after the first, so `cd my dir` - an unquoted pathname with a space -
	// succeeds into `my`; and dash accepts an empty operand as a no-op, where
	// chdir("") is ENOENT and cd-p.tst asserts the failure.
	EXPECT_EQ(run("cd real link 2>/dev/null"), 2);
	EXPECT_EQ(value_of("PWD"), root) << "a rejected call moves nothing";
	EXPECT_EQ(run("cd '' 2>/dev/null"), 2);
	EXPECT_EQ(run("cd -x 2>/dev/null"), 2);
	EXPECT_NE(capture("cd -x 2>&1"), "");
}

TEST_F(CdTest, TheHyphenOperandIsOldPwdAndIsPrinted) {
	// `-` is an OPERAND, not an option group: the scan has to stop at it. Whatever
	// else changes about the option parsing, `cd -` must keep working - it is one of
	// the four behaviours #46 lists as the regression checklist.
	EXPECT_EQ(run("cd real"), 0);
	EXPECT_EQ(value_of("OLDPWD"), root);
	EXPECT_EQ(capture("cd -"), root + "\n");
	EXPECT_EQ(value_of("PWD"), root);
	EXPECT_EQ(value_of("OLDPWD"), root + "/real");
	// And with an explicit mode in front of it, which is how cd-p.tst asks.
	EXPECT_EQ(capture("cd -P -"), root + "/real\n");
}

TEST_F(CdTest, NoOperandMeansHomeAndDoesNotPrint) {
	EXPECT_EQ(capture("HOME=" + root + "/real; cd"), "");
	EXPECT_EQ(value_of("PWD"), root + "/real");
	EXPECT_EQ(run("unset HOME; cd 2>/dev/null"), 2);
}

TEST_F(CdTest, DashEIsAcceptedWithPhysical) {
	// POSIX's -e reports a working directory that cannot be determined AFTER a
	// successful chdir, which is not a state a test can produce reliably - cd-p.tst
	// says so in a comment where the case would be. What is asserted is that the
	// option is ACCEPTED, because dash rejects it and a script written to POSIX
	// would die on the illegal-option status.
	EXPECT_EQ(run("cd -P -e ."), 0);
	EXPECT_EQ(run("cd -P -e _no_such_path_ 2>/dev/null"), 2)
		<< "and -e does not turn a failed cd into 1";
}

TEST_F(CdTest, ReadonlyPwdFailsAfterTheDirectoryHasChanged) {
	// POSIX XBD 8.1 allows three answers here; this is dash's - the chdir is not
	// undone, and cd reports that PWD no longer describes where the shell is.
	EXPECT_EQ(run("readonly PWD; cd real 2>/dev/null"), 2);
	EXPECT_EQ(value_of("PWD"), root) << "the refused assignment left PWD alone";
	EXPECT_EQ(std::filesystem::current_path(), root + "/real") << "the chdir stands";
}

// --- `pwd`, and the logical/physical rule it shares with `cd` (#51) ----------
//
// These live beside `cd`'s because they are the same question: `pwd -L` reports the
// working directory `cd -L` maintains, and a shell whose two builtins disagree about
// which directory it is in is worse than one that is wrong in both. The scratch tree
// is already the one the question needs - a symlink is the only place -L and -P
// differ.

TEST_F(CdTest, PwdReportsTheLogicalDirectoryAndDashPTheRealOne) {
	EXPECT_EQ(run("cd link/inner"), 0);
	EXPECT_EQ(capture("pwd"), root + "/link/inner\n") << "the default is -L";
	EXPECT_EQ(capture("pwd -L"), root + "/link/inner\n");
	EXPECT_EQ(capture("pwd -P"), root + "/real/inner\n");
}

TEST_F(CdTest, TheLastOfPwdsLAndPWins) {
	// The same rule `cd` has, because it is the same option pair - a group as well
	// as a sequence, so the scan cannot stop at the first letter.
	EXPECT_EQ(run("cd link"), 0);
	EXPECT_EQ(capture("pwd -P -L"), root + "/link\n");
	EXPECT_EQ(capture("pwd -L -P"), root + "/real\n");
	EXPECT_EQ(capture("pwd -LP"), root + "/real\n");
}

TEST_F(CdTest, PwdFallsBackToTheRealDirectoryWhenPwdHasGoneStale) {
	// `readonly PWD` then a cd is the one state that produces a stale PWD without
	// the script having written a wrong one: the directory changed and the variable
	// could not follow. POSIX `pwd -L` falls back when PWD does not name the current
	// directory, and printing the stored value there is a wrong answer a user acts on.
	EXPECT_EQ(run("readonly PWD; cd real 2>/dev/null"), 2);
	EXPECT_EQ(value_of("PWD"), root) << "the refused assignment left PWD alone";
	EXPECT_EQ(capture("pwd"), root + "/real\n");
	EXPECT_EQ(capture("pwd -P"), root + "/real\n");
}

TEST_F(CdTest, AStalePwdDoesNotSteerALaterRelativeCd) {
	// The other half of the same fallback, and the reason it is one rule rather than
	// two: `cd` extends a relative operand onto the logical directory, so believing
	// a stale PWD would resolve `inner` against a directory the shell has left.
	EXPECT_EQ(run("readonly PWD; cd real 2>/dev/null"), 2);
	EXPECT_EQ(run("cd inner 2>/dev/null"), 2) << "readonly PWD still refuses the write";
	EXPECT_EQ(std::filesystem::current_path(), root + "/real/inner")
		<< "but the cd resolved against where the shell actually is";
}

TEST_F(CdTest, PwdRejectsAnIllegalOptionAndIgnoresOperands) {
	EXPECT_EQ(run("pwd -x 2>/dev/null"), 2);
	EXPECT_NE(capture("pwd -x 2>&1"), "") << "and it says so";
	// Operands are IGNORED rather than diagnosed, which is dash's answer and where
	// `pwd` differs from `cd`: #46 diagnoses `cd a b` because taking the first
	// operand silently lands the shell somewhere the user did not name. An extra
	// operand to `pwd` changes no answer, so refusing it would be a divergence that
	// buys nothing.
	EXPECT_EQ(capture("pwd extra"), root + "\n");
}

// --- what a shell BELIEVES about where it is when it starts (#51) ------------

namespace {

// POSIX 2.5.3 makes setting PWD the shell's job at initialization, so what is under
// test is what a CONSTRUCTOR decides: the environment and the working directory have
// to be arranged before shell_state exists, which is why the state is built inside
// each test rather than held as a fixture member.
//
// The process environment is global to the test binary. A leaked PWD would arrive at
// the next test as an inherited value and make it pass or fail for a reason it does
// not name, so it is saved and restored around every case.
class StartupPwdTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	lesh::testing::temp_path scratch;
	std::string root;   // the PHYSICAL pathname of the scratch tree

	void SetUp() override {
		_origin = std::filesystem::current_path();
		if (const char* inherited = std::getenv("PWD")) {
			_had_pwd = true;
			_saved_pwd = inherited;
		}
		root = scratch.dir();
		std::filesystem::create_directories(root + "/real/inner");
		std::filesystem::create_directory_symlink(root + "/real", root + "/link");
		std::filesystem::create_directories(root + "/sub");
		std::filesystem::current_path(root);
	}

	void TearDown() override {
		// Before `scratch` is destroyed, for the reason CdTest gives: a working
		// directory left inside a deleted tree fails every later test in getcwd.
		std::filesystem::current_path(_origin);
		if (_had_pwd)
			::setenv("PWD", _saved_pwd.c_str(), 1);
		else
			::unsetenv("PWD");
	}

	// What a shell started here would believe about where it is.
	static std::string believed(const shell_state& state) {
		std::string_view text;
		return state.lookup("PWD", text) ? std::string{text} : std::string{"<unset>"};
	}

	int run(shell_state& state, std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}

private:
	std::filesystem::path _origin;
	bool _had_pwd = false;
	std::string _saved_pwd;
};

} // namespace

TEST_F(StartupPwdTest, AnAbsentPwdIsSetFromTheRealDirectory) {
	// `env -u PWD lesh -c 'echo "[$PWD]"'` printed `[]`: the shell set nothing and a
	// script that read $PWD got an empty string rather than a pathname.
	::unsetenv("PWD");
	shell_state state;
	EXPECT_EQ(believed(state), root);
	EXPECT_TRUE(state.is_exported("PWD")) << "a child shell inherits it, as in dash";
}

TEST_F(StartupPwdTest, AnInheritedPwdNamingSomewhereElseIsReplaced) {
	// The bug this ticket is named for: `PWD=/etc lesh -c pwd` answered /etc.
	::setenv("PWD", "/etc", 1);
	shell_state state;
	EXPECT_EQ(believed(state), root);
}

TEST_F(StartupPwdTest, ARelativeInheritedPwdIsReplaced) {
	// POSIX describes PWD as an ABSOLUTE pathname, so a relative one names nowhere
	// the shell could join a relative operand onto.
	::setenv("PWD", "relative", 1);
	shell_state state;
	EXPECT_EQ(believed(state), root);
}

TEST_F(StartupPwdTest, AnInheritedPwdReachedThroughASymlinkIsKept) {
	// The case the check exists FOR, and the one a string comparison would break.
	// `link` and `real` are one directory; a logical PWD that names it through the
	// symlink is exactly the value #46's -L maintains, so startup must not undo it.
	std::filesystem::current_path(root + "/real");
	::setenv("PWD", (root + "/link").c_str(), 1);
	shell_state state;
	EXPECT_EQ(believed(state), root + "/link")
		<< "device and inode say these name one directory; the text does not";
}

TEST_F(StartupPwdTest, AnInheritedPwdWithADotComponentIsReplaced) {
	// A deliberate divergence, recorded in #51: dash, bash and ksh keep these and
	// then print a pathname with a `.` or `..` in it for a directory whose real name
	// they know. POSIX 2.5.3 says PWD holds an absolute pathname containing no
	// component that is dot or dot-dot; zsh replaces them as lesh does.
	::setenv("PWD", (root + "/.").c_str(), 1);
	shell_state dotted;
	EXPECT_EQ(believed(dotted), root);

	const std::string leaf = std::filesystem::path{root}.filename().string();
	::setenv("PWD", (root + "/../" + leaf).c_str(), 1);
	shell_state dotdotted;
	EXPECT_EQ(believed(dotdotted), root);
}

TEST_F(StartupPwdTest, AWrongInheritedPwdDoesNotSteerARelativeCd) {
	// Why the startup value is worth fixing rather than only reporting: a lie in the
	// environment used to reach `cd` as the directory to join a relative operand
	// onto, so `cd sub` looked for /etc/sub.
	::setenv("PWD", "/etc", 1);
	shell_state state;
	EXPECT_EQ(run(state, "cd sub"), 0);
	std::string_view where;
	ASSERT_TRUE(state.lookup("PWD", where));
	EXPECT_EQ(std::string{where}, root + "/sub");
}
