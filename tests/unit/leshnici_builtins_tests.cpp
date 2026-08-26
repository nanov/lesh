#include "leshnici/builtins.h"

#include "runtime/builtins.h"
#include "runtime/executor.h"
#include "runtime/invocation.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <deque>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// leshnici's extension builtins, and the gate they sit behind (#165).
//
// TWO THINGS ARE UNDER TEST HERE AND THEY FAIL DIFFERENTLY. The SEAM - a second
// builtin table, borrowed, consulted after the core one and only when
// `set -o leshnici` is on - is a property of `runtime/`, and getting it wrong
// changes what a POSIX script does: a name that shadows `/bin/ls` in a shell
// that never asked for it is a conformance regression wearing a feature's
// clothes. The four UTILITIES are ordinary code whose bugs are ordinary. The
// first half is why the option cases come first and why they assert the
// fall-through to PATH with a marker rather than by eye.
class LeshniciBuiltinsTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	// Kept alive for the same reason BuiltinRegistryTest keeps them: a tree's
	// spans are views into its source, and shell state holds the tree.
	std::deque<std::string> sources;
	shell_state state;
	lesh::testing::temp_path scratch;

	// THREE DIRECTORIES, NOT ONE. `ls` lists a directory, so a test that puts its
	// fixtures and the capture file in the same place is a test that asserts the
	// capture file's name - which is a self-fulfilling listing. `data/` holds what
	// is listed, `bin/` holds the marker `ls` that PATH finds, and the capture
	// file stays in the root where nothing looks.
	std::string data;
	std::string bin;

	void SetUp() override {
		lesh::leshnici::install_builtins(state);
		data = scratch.file("data");
		bin = scratch.file("bin");
		ASSERT_EQ(::mkdir(data.c_str(), 0755), 0);
		ASSERT_EQ(::mkdir(bin.c_str(), 0755), 0);
	}

	[[nodiscard]] std::string data_file(std::string_view name) const {
		return data + "/" + std::string{name};
	}

	int run(std::string_view src) {
		const std::string& source = sources.emplace_back(src);
		tree_walking_executor ex{pool, state};
		return ex.run(state.retain_tree(parse(pool, source)));
	}

	std::string capture(std::string_view src) {
		const std::string path = scratch.file("leshnici_capture.txt");
		std::remove(path.c_str());
		std::string wrapped{"{ "};
		wrapped.append(src);
		wrapped += "; } > ";
		wrapped += path;
		(void)run(wrapped);
		std::ifstream in{path};
		std::ostringstream text;
		text << in.rdbuf();
		std::remove(path.c_str());
		return text.str();
	}

	void write_file(std::string_view name, std::string_view contents) {
		std::ofstream out{data_file(name), std::ios::binary};
		out << contents;
	}

	// A `ls` on PATH that says which one ran. The only way to tell the builtin
	// from the external command apart without reading `/bin/ls`'s mind.
	void install_marker_ls() {
		const std::string path = bin + "/ls";
		{
			std::ofstream out{path};
			out << "#!/bin/sh\necho EXTERNAL-LS\n";
		}
		::chmod(path.c_str(), 0755);
		ASSERT_TRUE(state.set("PATH", bin));
	}
};

} // namespace

// --- the option, and what it gates -------------------------------------------

TEST_F(LeshniciBuiltinsTest, TheTableIsInstalledButInertUntilTheOptionIsOn) {
	// INSTALLED IN BOTH MODES is main.cpp's decision: a table that was handed over
	// only when interactive could not be turned on by `set -o leshnici` in a
	// script, and the option would gate nothing.
	EXPECT_FALSE(state.extension_builtins().empty());
	EXPECT_FALSE(state.opts().leshnici);
	EXPECT_FALSE(state.extension_builtins_enabled());
	EXPECT_EQ(classify_builtin(state, "ls"), builtin_kind::none);
	EXPECT_FALSE(builtin_has_handler(state, "ls"));

	state.opts().leshnici = true;
	EXPECT_TRUE(state.extension_builtins_enabled());
	// REGULAR, NEVER SPECIAL: POSIX 2.14's set is closed and a failing `ls` must
	// not end a non-interactive shell that turned the option on.
	EXPECT_EQ(classify_builtin(state, "ls"), builtin_kind::regular);
	EXPECT_TRUE(builtin_has_handler(state, "ls"));
	// The state-free form answers for the PROGRAM and must not have moved: it is
	// what the registry tests and #35's static_assert are about.
	EXPECT_EQ(classify_builtin("ls"), builtin_kind::none);
	EXPECT_FALSE(builtin_has_handler("ls"));
}

TEST_F(LeshniciBuiltinsTest, EveryExtensionNameIsVisibleExactlyWhenTheOptionIs) {
	for (const extension_builtin& one : state.extension_builtins()) {
		state.opts().leshnici = false;
		EXPECT_EQ(classify_builtin(state, one.name), builtin_kind::none) << one.name;
		state.opts().leshnici = true;
		EXPECT_EQ(classify_builtin(state, one.name), builtin_kind::regular) << one.name;
		EXPECT_TRUE(builtin_has_handler(state, one.name)) << one.name;
	}
}

TEST_F(LeshniciBuiltinsTest, WithTheOptionOffTheNameFallsThroughToPath) {
	install_marker_ls();
	EXPECT_EQ(capture("ls"), "EXTERNAL-LS\n")
		<< "a script that never asked for the extension set must reach PATH, or "
		<< "every conformance case that runs `ls` would be measuring a different "
		<< "utility";
	// `command -v` has to agree with what would RUN, or the two answers are #35
	// again in a new table.
	const std::string described = capture("command -v ls");
	EXPECT_NE(described.find("/ls\n"), std::string::npos) << described;

	state.opts().leshnici = true;
	EXPECT_EQ(capture("command -v ls"), "ls\n")
		<< "a regular built-in the shell really implements is written by name";
	EXPECT_EQ(capture("ls " + bin), "ls\n")
		<< "and the builtin runs instead of the marker script, listing the "
		<< "directory rather than announcing itself";
}

TEST_F(LeshniciBuiltinsTest, TheOptionIsListedAndTogglesLikeAnyOther) {
	const std::string listing = capture("set -o");
	EXPECT_NE(listing.find("leshnici        off\n"), std::string::npos) << listing;
	EXPECT_EQ(run("set -o leshnici"), 0);
	EXPECT_TRUE(state.opts().leshnici);
	EXPECT_EQ(run("set +o leshnici"), 0);
	EXPECT_FALSE(state.opts().leshnici);
}

TEST(LeshniciBuiltinsInvocation, TheCommandLineNamesTheOptionOrLeavesItToMain) {
	// The tri-state main() reads. Unset means "nobody said", which is the only
	// answer that lets `lesh -o leshnici script.sh` survive the interactive
	// default written over it a line later.
	char* plain[] = {const_cast<char*>("lesh"), const_cast<char*>("-c"),
	                 const_cast<char*>(":"), nullptr};
	EXPECT_FALSE(parse_invocation(3, plain).leshnici.has_value());

	char* on[] = {const_cast<char*>("lesh"), const_cast<char*>("-o"),
	              const_cast<char*>("leshnici"), const_cast<char*>("-c"),
	              const_cast<char*>(":"), nullptr};
	const invocation enabled = parse_invocation(5, on);
	ASSERT_TRUE(enabled.leshnici.has_value());
	EXPECT_TRUE(*enabled.leshnici);
	EXPECT_TRUE(enabled.options.leshnici);

	char* off[] = {const_cast<char*>("lesh"), const_cast<char*>("+o"),
	               const_cast<char*>("leshnici"), const_cast<char*>("-c"),
	               const_cast<char*>(":"), nullptr};
	const invocation disabled = parse_invocation(5, off);
	ASSERT_TRUE(disabled.leshnici.has_value());
	EXPECT_FALSE(*disabled.leshnici);

	// ANOTHER `-o` MUST NOT LOOK LIKE THIS ONE. The two-seed probe that answers
	// `-i` copies the whole option struct across on every `-o` occurrence, which
	// is why this tri-state is recorded directly instead of derived from it.
	char* other[] = {const_cast<char*>("lesh"), const_cast<char*>("-o"),
	                 const_cast<char*>("xtrace"), const_cast<char*>("-c"),
	                 const_cast<char*>(":"), nullptr};
	EXPECT_FALSE(parse_invocation(5, other).leshnici.has_value());

	// And the rule main() applies to it, written down where it can be checked.
	EXPECT_TRUE(parse_invocation(3, plain).leshnici.value_or(/*interactive=*/true));
	EXPECT_FALSE(parse_invocation(3, plain).leshnici.value_or(/*interactive=*/false));
	EXPECT_TRUE(parse_invocation(5, on).leshnici.value_or(/*interactive=*/false));
}

TEST_F(LeshniciBuiltinsTest, ACollisionWithACoreBuiltinIsRefusedWhole) {
	// CORE WINS, and the refusal takes the whole table with it: a partially
	// installed set is a shell whose behaviour depends on which row was wrong.
	static constexpr std::array<extension_builtin, 2> collides = {{
		{"ls", &lesh::leshnici::builtin_ls},
		{"cd", &lesh::leshnici::builtin_ls},
	}};
	shell_state fresh;
	EXPECT_FALSE(fresh.set_extension_builtins(
		std::span<const extension_builtin>{collides}));
	EXPECT_TRUE(fresh.extension_builtins().empty());
	fresh.opts().leshnici = true;
	EXPECT_EQ(classify_builtin(fresh, "ls"), builtin_kind::none);
	// `cd` is still exactly what it was.
	EXPECT_EQ(classify_builtin(fresh, "cd"), builtin_kind::regular);
	EXPECT_TRUE(builtin_has_handler("cd"));
}

TEST_F(LeshniciBuiltinsTest, AnEmptyTableLeavesTheShellUnchanged) {
	shell_state fresh;
	EXPECT_TRUE(fresh.set_extension_builtins({}));
	fresh.opts().leshnici = true;
	// The option alone is not a table: `enabled` asks both questions.
	EXPECT_FALSE(fresh.extension_builtins_enabled());
	EXPECT_EQ(classify_builtin(fresh, "ls"), builtin_kind::none);
}

// --- the utilities -----------------------------------------------------------

namespace {

class LeshniciUtilitiesTest : public LeshniciBuiltinsTest {
protected:
	void SetUp() override {
		LeshniciBuiltinsTest::SetUp();
		state.opts().leshnici = true;
	}
};

} // namespace

TEST_F(LeshniciUtilitiesTest, LsListsOneNamePerLineIntoAPipeOrAFile) {
	write_file("b.txt", "");
	write_file("a.txt", "");
	write_file(".hidden", "");
	// Sorted by bytes, and the dotfile left out. Output is a file here, so the
	// one-name-per-line form is the one POSIX asks for.
	EXPECT_EQ(capture("ls " + data), "a.txt\nb.txt\n");
	EXPECT_EQ(capture("ls -1 " + data), "a.txt\nb.txt\n");
}

TEST_F(LeshniciUtilitiesTest, LsMinusAIncludesTheDotFiles) {
	write_file(".hidden", "");
	write_file("plain", "");
	const std::string listing = capture("ls -a " + data);
	EXPECT_NE(listing.find(".hidden\n"), std::string::npos) << listing;
	EXPECT_NE(listing.find("plain\n"), std::string::npos) << listing;
	// `.` and `..` are entries of the directory and `-a` means all of them.
	EXPECT_NE(listing.find("..\n"), std::string::npos) << listing;
}

TEST_F(LeshniciUtilitiesTest, LsMinusLWritesAModeStringAndTheName) {
	write_file("sized", "0123456789");
	const std::string listing = capture("ls -l " + data);
	// One row, ending in the name, beginning with a regular file's mode.
	EXPECT_EQ(listing.compare(0, 1, "-"), 0) << listing;
	EXPECT_NE(listing.find(" 10 "), std::string::npos)
		<< "the size column: " << listing;
	EXPECT_NE(listing.find("sized\n"), std::string::npos) << listing;
}

TEST_F(LeshniciUtilitiesTest, LsNamesEachDirectoryOnlyWhenMoreThanOneWasAsked) {
	write_file("one", "");
	const std::string sub = data_file("sub");
	::mkdir(sub.c_str(), 0755);
	{
		std::ofstream out{sub + "/two"};
		out << "";
	}
	EXPECT_EQ(capture("ls " + sub), "two\n") << "one operand gets bytes, no header";
	const std::string both = capture("ls " + data + " " + sub);
	EXPECT_NE(both.find(data + ":\n"), std::string::npos) << both;
	EXPECT_NE(both.find(sub + ":\n"), std::string::npos) << both;
}

TEST_F(LeshniciUtilitiesTest, LsReportsAnOperandItCannotReachAndCarriesOn) {
	write_file("here", "");
	// 1, not 2: the command line was the right shape and the filesystem was not
	// what the script assumed. coreutils draws the same line.
	EXPECT_EQ(run("ls " + data_file("nosuch") + " 2>/dev/null"), 1);
	const std::string listing =
		capture("ls " + data_file("nosuch") + " " + data + " 2>/dev/null");
	EXPECT_NE(listing.find("here\n"), std::string::npos)
		<< "the operand after the bad one is still listed: " << listing;
}

TEST_F(LeshniciUtilitiesTest, LsRefusesALetterItDoesNotHave) {
	// 2, and the shell survives: an extension builtin is regular, so a usage
	// error in one never ends a non-interactive shell however POSIX 2.8.1 reads.
	EXPECT_EQ(run("ls -Z 2>/dev/null"), 2);
	EXPECT_EQ(capture("ls -Z 2>/dev/null; echo reached"), "reached\n");
}

TEST_F(LeshniciUtilitiesTest, CatConcatenatesTwoFiles) {
	write_file("one", "first\n");
	write_file("two", "second\n");
	EXPECT_EQ(capture("cat " + data_file("one") + " " + data_file("two")),
	          "first\nsecond\n");
}

TEST_F(LeshniciUtilitiesTest, CatReadsStandardInputForNoOperandAndForAHyphen) {
	EXPECT_EQ(capture("echo piped | cat"), "piped\n");
	EXPECT_EQ(capture("echo piped | cat -"), "piped\n");
}

TEST_F(LeshniciUtilitiesTest, CatKeepsGoingPastAFileItCannotOpen) {
	write_file("real", "kept\n");
	EXPECT_EQ(capture("cat " + data_file("gone") + " " + data_file("real") +
	                  " 2>/dev/null"),
	          "kept\n");
	EXPECT_EQ(run("cat " + data_file("gone") + " 2>/dev/null"), 1);
}

TEST_F(LeshniciUtilitiesTest, CatCarriesBytesThatAreNotText) {
	// No line is ever materialised, so a NUL and a very long line are not special.
	// A `std::getline` implementation would truncate at the first of these.
	std::string payload;
	payload.push_back('a');
	payload.push_back('\0');
	payload.push_back('b');
	payload.append(200 * 1024, 'x');  // three times the 64 KiB buffer
	payload.push_back('\n');
	write_file("binary", payload);
	EXPECT_EQ(capture("cat " + data_file("binary")), payload);
}

TEST_F(LeshniciUtilitiesTest, HeadTakesTenLinesByDefaultAndCountWithMinusN) {
	std::string twelve;
	for (int i = 1; i <= 12; ++i)
		twelve += std::to_string(i) + "\n";
	write_file("twelve", twelve);
	EXPECT_EQ(capture("head " + data_file("twelve")),
	          "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
	EXPECT_EQ(capture("head -n 2 " + data_file("twelve")), "1\n2\n");
	EXPECT_EQ(capture("head -n2 " + data_file("twelve")), "1\n2\n")
		<< "POSIX guideline 7: attached and separate are both the argument";
	EXPECT_EQ(capture("head -n 0 " + data_file("twelve")), "");
	EXPECT_EQ(capture("head -n 99 " + data_file("twelve")), twelve);
}

TEST_F(LeshniciUtilitiesTest, HeadReadsAPipeAndAFileTheSameWay) {
	EXPECT_EQ(capture("echo one | head -n 1"), "one\n");
	// A last line with no newline is still a line.
	write_file("unterminated", "a\nb");
	EXPECT_EQ(capture("head -n 5 " + data_file("unterminated")), "a\nb");
}

TEST_F(LeshniciUtilitiesTest, HeadHeadsEachFileOnlyWhenMoreThanOneWasAsked) {
	write_file("one", "1\n");
	write_file("two", "2\n");
	EXPECT_EQ(capture("head -n 1 " + data_file("one")), "1\n");
	const std::string both = capture("head -n 1 " + data_file("one") + " " +
	                                 data_file("two"));
	EXPECT_EQ(both, "==> " + data_file("one") + " <==\n1\n" +
	                "\n==> " + data_file("two") + " <==\n2\n");
}

TEST_F(LeshniciUtilitiesTest, HeadRefusesACountItWillNotImplement) {
	EXPECT_EQ(run("head -n abc 2>/dev/null"), 2);
	EXPECT_EQ(run("head -n -3 2>/dev/null"), 2)
		<< "coreutils reads a negative count as `all but the last N`, which is a "
		<< "second algorithm; refusing beats guessing";
	EXPECT_EQ(run("head -n 2>/dev/null"), 2);
}

TEST_F(LeshniciUtilitiesTest, TailTakesTheLastTenLinesOfASeekableFile) {
	std::string twelve;
	for (int i = 1; i <= 12; ++i)
		twelve += std::to_string(i) + "\n";
	write_file("twelve", twelve);
	EXPECT_EQ(capture("tail " + data_file("twelve")),
	          "3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n");
	EXPECT_EQ(capture("tail -n 2 " + data_file("twelve")), "11\n12\n");
	EXPECT_EQ(capture("tail -n 1 " + data_file("twelve")), "12\n")
		<< "the trailing newline TERMINATES the last line rather than starting one; "
		<< "getting it wrong prints an empty line here";
	EXPECT_EQ(capture("tail -n 0 " + data_file("twelve")), "");
	EXPECT_EQ(capture("tail -n 99 " + data_file("twelve")), twelve);
}

TEST_F(LeshniciUtilitiesTest, TailCrossesTheBackwardScansBlockBoundary) {
	// The backward scan reads 64 KiB at a time. A file bigger than one block, whose
	// answer is in the last of them, is what makes the multi-block loop run at all;
	// a file bigger than one block whose answer is NOT in the last block is what
	// makes the offset arithmetic matter.
	std::string big;
	const int lines = 40000;  // roughly 270 KiB of digits and newlines
	for (int i = 0; i < lines; ++i)
		big += std::to_string(i) + "\n";
	write_file("big", big);
	EXPECT_EQ(capture("tail -n 2 " + data_file("big")), "39998\n39999\n");
	EXPECT_EQ(capture("tail -n 1 " + data_file("big")), "39999\n");
	// The whole file back, which only happens if the scan stops at offset zero
	// rather than running off the front of it.
	EXPECT_EQ(capture("tail -n " + std::to_string(lines + 10) + " " +
	                  data_file("big")),
	          big);
}

TEST_F(LeshniciUtilitiesTest, TailOverAPipeUsesTheRingAndAgreesWithTheFile) {
	std::string six;
	for (int i = 1; i <= 6; ++i)
		six += std::to_string(i) + "\n";
	write_file("six", six);
	// The SAME answer by two different algorithms: a pipe cannot seek, so this
	// goes through the ring, and a disagreement between the two is the bug this
	// pair exists to catch.
	EXPECT_EQ(capture("tail -n 2 " + data_file("six")), "5\n6\n");
	EXPECT_EQ(capture("cat " + data_file("six") + " | tail -n 2"), "5\n6\n");
	EXPECT_EQ(capture("cat " + data_file("six") + " | tail -n 99"), six);
	EXPECT_EQ(capture("cat " + data_file("six") + " | tail -n 0"), "");
}

TEST_F(LeshniciUtilitiesTest, TailKeepsALastLineWithNoNewline) {
	write_file("unterminated", "a\nb\nc");
	EXPECT_EQ(capture("tail -n 2 " + data_file("unterminated")), "b\nc");
	EXPECT_EQ(capture("cat " + data_file("unterminated") + " | tail -n 2"), "b\nc");
	EXPECT_EQ(capture("tail -n 1 " + data_file("unterminated")), "c");
}

TEST_F(LeshniciUtilitiesTest, TailHeadsEachFileOnlyWhenMoreThanOneWasAsked) {
	write_file("one", "1\n");
	write_file("two", "2\n");
	EXPECT_EQ(capture("tail -n 1 " + data_file("one")), "1\n");
	const std::string both = capture("tail -n 1 " + data_file("one") + " " +
	                                 data_file("two"));
	EXPECT_EQ(both, "==> " + data_file("one") + " <==\n1\n" +
	                "\n==> " + data_file("two") + " <==\n2\n");
}

TEST_F(LeshniciUtilitiesTest, TailReportsAFileItCannotOpen) {
	EXPECT_EQ(run("tail " + data_file("gone") + " 2>/dev/null"), 1);
	EXPECT_EQ(run("tail -n -1 2>/dev/null"), 2);
}

TEST_F(LeshniciUtilitiesTest, AnExtensionBuiltinInAPipelineStageStillRuns) {
	// A stage runs in its own process and reaches try_run_builtin through a
	// DIFFERENT branch of the executor than a simple command does. The branch
	// asserts that a classified name always has a handler, which is only true if
	// its classify and its dispatch read the same gate.
	write_file("three", "1\n2\n3\n");
	EXPECT_EQ(capture("cat " + data_file("three") + " | head -n 1"), "1\n");
	EXPECT_EQ(capture("head -n 2 " + data_file("three") + " | tail -n 1"), "2\n");
}

TEST_F(LeshniciUtilitiesTest, RedirectionsApplyToAnExtensionBuiltinAndAreUndone) {
	write_file("src", "x\n");
	const std::string out = data_file("out");
	EXPECT_EQ(run("cat " + data_file("src") + " > " + out), 0);
	std::ifstream in{out};
	std::ostringstream text;
	text << in.rdbuf();
	EXPECT_EQ(text.str(), "x\n");
	// The shell's own stdout is back: a builtin runs in THIS process, so a
	// redirection that leaked would take the rest of the session with it.
	EXPECT_EQ(capture("echo after"), "after\n");
}
