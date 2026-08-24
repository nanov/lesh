#include "runtime/builtins.h"

#include "runtime/executor.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

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

// `cd`: -L and -P, CDPATH, and the failure status. See issue #46.
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
	std::string root;   // the PHYSICAL pathname of the scratch tree

	void SetUp() override {
		_origin = std::filesystem::current_path();
		std::string pattern = ::testing::TempDir() + "lesh_cd_XXXXXX";
		ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
		// canonical(), because ::testing::TempDir() is itself reached through a
		// symlink on macOS (/var -> /private/var). Comparing a -P result against the
		// name the directory was CREATED under would then fail for a reason that has
		// nothing to do with cd.
		root = std::filesystem::canonical(pattern).string();
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
		// in getcwd rather than here.
		std::filesystem::current_path(_origin);
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
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
