#include "runtime/executor.h"

#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// Shell options. See issue #31.
//
// Four of the POSIX letters were parsed, recorded, and then read by nothing at
// all: `set -u`, `set -x`, `set -a` and `set -C` were accepted and had no effect,
// and the header claimed two of them were honoured. These tests exist so that
// cannot happen again silently - each one asserts the EFFECT, not the flag.
class OptionsTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	shell_state state;

	int run(std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}

	// Runs `src` with its standard output in a file and returns what it wrote.
	// A redirection rather than freopen, because the shell's own stdio is what is
	// under test and swapping it out from underneath would change the thing measured.
	std::string capture(std::string_view src) {
		const std::string path = ::testing::TempDir() + "lesh_options_capture.txt";
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

	std::string capture_stderr(std::string_view src) {
		const std::string path = ::testing::TempDir() + "lesh_options_stderr.txt";
		std::remove(path.c_str());
		std::string wrapped{"{ "};
		wrapped.append(src);
		wrapped += "; } 2> ";
		wrapped += path;
		(void)run(wrapped);
		std::ifstream in{path};
		std::ostringstream text;
		text << in.rdbuf();
		std::remove(path.c_str());
		return text.str();
	}
};

} // namespace

TEST_F(OptionsTest, EveryTableRowIsReachableByItsLetterAndItsName) {
	// The table is the only place an option is declared. If a row were reachable
	// by letter but not by name - which is what two hand-written switch statements
	// drifting apart looks like - `sh -o noglob` and `sh -f` would disagree.
	for (const auto& row : shell_state::option_table()) {
		shell_state::options o;
		if (row.letter != '\0') {
			ASSERT_TRUE(shell_state::apply_option_letter(o, row.letter, true))
				<< "letter " << row.letter;
			EXPECT_TRUE(o.*row.field) << "letter " << row.letter;
			ASSERT_TRUE(shell_state::apply_option_letter(o, row.letter, false));
			EXPECT_FALSE(o.*row.field);
		}
		if (!row.name.empty()) {
			ASSERT_TRUE(shell_state::apply_option_name(o, row.name, true)) << row.name;
			EXPECT_TRUE(o.*row.field) << row.name;
			ASSERT_TRUE(shell_state::apply_option_name(o, row.name, false));
			EXPECT_FALSE(o.*row.field);
		}
	}
}

TEST_F(OptionsTest, AnOptionPOSIXDoesNotNameIsRejected) {
	shell_state::options o;
	EXPECT_FALSE(shell_state::apply_option_letter(o, 'Z', true));
	EXPECT_FALSE(shell_state::apply_option_letter(o, 'o', true))
		<< "`-o` introduces a name; it is not an option of its own";
	EXPECT_FALSE(shell_state::apply_option_name(o, "bogus", true));
	EXPECT_FALSE(shell_state::apply_option_name(o, "", true));
}

TEST_F(OptionsTest, DollarDashReportsTheLettersThatAreOn) {
	EXPECT_EQ(state.option_flags(), "");
	state.opts().exit_on_error = true;
	state.opts().error_on_unset = true;
	state.opts().trace = true;
	EXPECT_EQ(state.option_flags(), "eux") << "table order, so it is stable per call";
	// `$-` was not a parameter at all: the lexer produced it and the expander had
	// no case for it, so it expanded to nothing and every `$-` case in option-p.tst
	// and set-p.tst failed on a shell that had recorded the options correctly.
	state.opts() = {};
	EXPECT_EQ(capture("set -aef; echo \"[$-]\""), "[aef]\n");
}

TEST_F(OptionsTest, InteractiveIsReportedInDollarDashThoughSetCannotToggleIt) {
	// POSIX lists `i` among the flags `$-` reports and dash prints it, but `set`
	// has no `-i`: interactive is decided at invocation.
	state.set_interactive(true);
	EXPECT_EQ(state.option_flags(), "i");
	shell_state::options o;
	EXPECT_FALSE(shell_state::apply_option_letter(o, 'i', true));
}

TEST_F(OptionsTest, SetMinusOListsEveryNamedOptionWithItsSetting) {
	const std::string listing = capture("set -e; set -o");
	EXPECT_NE(listing.find("Current option settings\n"), std::string::npos);
	EXPECT_NE(listing.find("errexit         on\n"), std::string::npos);
	EXPECT_NE(listing.find("noglob          off\n"), std::string::npos);
	EXPECT_EQ(listing.find("hashondef"), std::string::npos)
		<< "POSIX gives -h no `-o` spelling, so `set -o` has no name to print";
}

TEST_F(OptionsTest, SetPlusOPrintsCommandsThatRestoreTheSettings) {
	// POSIX requires the `+o` form to be RE-INPUTTABLE, and set-p.tst's round trip
	// is the only test of it: save with `set +o`, change everything, read it back,
	// and the listing must match again.
	const std::string saved = capture("set -aeu; set +o");
	EXPECT_NE(saved.find("set -o allexport\n"), std::string::npos);
	EXPECT_NE(saved.find("set +o noglob\n"), std::string::npos);

	EXPECT_EQ(run("set +aeu -f"), 0);
	EXPECT_EQ(run(saved), 0);
	EXPECT_TRUE(state.opts().all_export);
	EXPECT_TRUE(state.opts().exit_on_error);
	EXPECT_TRUE(state.opts().error_on_unset);
	EXPECT_FALSE(state.opts().no_glob) << "the saved listing must turn -f back off";
}

TEST_F(OptionsTest, AnUnknownOptionIsAnErrorThatExitsANonInteractiveShell) {
	// The return value of apply_option_letter was discarded with a `(void)`, so
	// `set -Z` succeeded silently. `set` is a special builtin, so dash reports
	// `Illegal option -Z`, exits 2, and never reaches the next command.
	EXPECT_EQ(run("set -Z; exit 42"), 2);
	EXPECT_EQ(run("set -o bogus; exit 42"), 2);
	EXPECT_EQ(run("set +o bogus; exit 42"), 2);
	EXPECT_EQ(run("set -e; exit 42"), 42) << "a known option still works";
}

TEST_F(OptionsTest, NoExecReadsWithoutRunning) {
	EXPECT_EQ(run("set -n; exit 42"), 0)
		<< "`exit 42` must not run, so the status is the shell's own zero";
	const std::string path = ::testing::TempDir() + "lesh_noexec_marker.txt";
	std::remove(path.c_str());
	state.opts() = {};
	EXPECT_EQ(run("set -n; > " + path), 0);
	std::ifstream created{path};
	EXPECT_FALSE(created.good()) << "a redirection ran under -n";
	std::remove(path.c_str());
}

TEST_F(OptionsTest, NoClobberRefusesToTruncateAnExistingFile) {
	const std::string path = ::testing::TempDir() + "lesh_noclobber.txt";
	std::remove(path.c_str());
	EXPECT_EQ(run("set -C; echo first > " + path), 0)
		<< "-C still creates a file that does not exist";
	EXPECT_EQ(run("echo second > " + path), 2)
		<< "the second `>` must fail rather than truncate";
	EXPECT_EQ(run("echo third >| " + path), 0)
		<< "`>|` is the override, which is why the operator exists";
	std::ifstream in{path};
	std::string line;
	std::getline(in, line);
	EXPECT_EQ(line, "third");
	std::remove(path.c_str());
	// An existing file that is NOT REGULAR is fair game: redir-p.tst checks
	// `echo bar >/dev/null` under -C, and O_EXCL alone would refuse it.
	EXPECT_EQ(run("echo bar > /dev/null"), 0);
}

TEST_F(OptionsTest, AllExportMarksAssignedVariablesForExport) {
	EXPECT_EQ(run("plain=1"), 0);
	EXPECT_FALSE(state.is_exported("plain"));
	EXPECT_EQ(run("set -a; exported=1"), 0);
	EXPECT_TRUE(state.is_exported("exported"));
	// A `for` variable and `${x=default}` are assignments too, which is why -a is
	// applied in shell_state::set rather than at each assignment site.
	EXPECT_EQ(run("for loop in 1; do :; done"), 0);
	EXPECT_TRUE(state.is_exported("loop"));
	EXPECT_EQ(run("echo ${defaulted=x} > /dev/null"), 0);
	EXPECT_TRUE(state.is_exported("defaulted"));
}

TEST_F(OptionsTest, NounsetMakesAnUnsetParameterFatal) {
	// The flag was recorded and read by nothing: `sh -u -c 'echo ${x}'` printed a
	// blank line and reported success.
	EXPECT_EQ(run("set -u; echo ${x}; exit 42"), 2);
	EXPECT_EQ(run("set -u; echo \"${#x}\"; exit 42"), 2);
	EXPECT_EQ(run("set -u; echo \"${x#y}\"; exit 42"), 2);
	// POSIX applies -u inside arithmetic too. dash does not, and fails
	// option-p.tst's 'nounset on: unset variable $((foo))' for it.
	EXPECT_EQ(run("set -u; echo \"$((x))\"; exit 42"), 2);
}

TEST_F(OptionsTest, NounsetLeavesTheDefaultingExpansionsAlone) {
	// `${x-d}`, `${x+d}` and `${x=d}` are the forms whose whole purpose is an
	// unset parameter, so -u must not fire on them.
	EXPECT_EQ(run("set -u; echo ${x-d} ${x+d} ${x=d} > /dev/null; exit 42"), 42);
	EXPECT_EQ(run("set -u; echo \"$@\" $# > /dev/null; exit 42"), 42)
		<< "$@ with no positional parameters is not an unset parameter";
}

TEST_F(OptionsTest, TheQuestionMarkExpansionIsFatalWhateverNounsetSays) {
	// `${x?}` is the caller asking for the shell to stop. The message was printed
	// and the command ran anyway, so `echo "${x?}"` printed a blank line and
	// reported success.
	EXPECT_EQ(run("echo \"${x?}\"; exit 42"), 2);
	EXPECT_EQ(run("empty=; echo \"${empty:?}\"; exit 42"), 2);
	EXPECT_EQ(run("empty=; echo \"${empty?}\" > /dev/null; exit 42"), 42)
		<< "without the colon an empty value is set";
}

TEST_F(OptionsTest, PipefailReportsTheRightmostFailingStage) {
	// POSIX Issue 8 added `pipefail`. Without it a pipeline's status is its LAST
	// command's, which is why `exit 1 | exit 0` succeeds. dash has no pipefail and
	// fails both of pipeline-p.tst's cases for it.
	EXPECT_EQ(run("exit 1 | exit 2 | exit 0"), 0);
	EXPECT_EQ(run("set -o pipefail; exit 1 | exit 2 | exit 0"), 2);
	state.opts() = {};
	EXPECT_EQ(run("set -o pipefail; exit 3 | exit 0 | exit 0"), 3);
	state.opts() = {};
	EXPECT_EQ(run("set -o pipefail; exit 0 | exit 0 | exit 0"), 0);
}

TEST_F(OptionsTest, APipelineThatEnablesPipefailDoesNotAffectItself) {
	// Every stage forks, so `false | set -o pipefail` leaves the shell's own option
	// off - and the pipeline's status is read with the option as it stood, which is
	// pipeline-p.tst's case of the same name. dash fails it by rejecting the name.
	EXPECT_EQ(run("false | set -o pipefail"), 0);
	EXPECT_FALSE(state.opts().pipefail) << "the option escaped the pipeline stage";
}

TEST_F(OptionsTest, XtraceWritesTheExpandedCommandToStandardError) {
	EXPECT_EQ(capture_stderr("set -x; foo=bar; echo $foo > /dev/null"),
	          "+ foo=bar\n+ echo bar\n")
		<< "the trace shows what runs, not the source text";
}

TEST_F(OptionsTest, XtraceExpandsPS4OnEveryLine) {
	// option-p.tst's `$PS4` case sets PS4 to `${foo#X} `, so the prompt is
	// expanded rather than printed literally.
	EXPECT_EQ(capture_stderr("foo=XY; PS4='${foo#X} '; set -x; echo t > /dev/null"),
	          "Y echo t\n");
}
