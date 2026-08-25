#include "runtime/builtins.h"

#include "runtime/executor.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include "interactive_signal_guard.h"
#include "temp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace lesh::runtime;
using namespace lesh::syntax;

namespace {

// `readonly`, and the assignments it then refuses. See issue #35.
//
// The builtin was classified and unimplemented, so `readonly OPTIND` returned 0
// having done nothing: no flag, no listing, and every later assignment allowed.
// The tests assert the REFUSAL at each of the places a variable can be written -
// a plain assignment, a prefix, a `for` variable, `read`, `getopts`, `${x=v}` and
// `$((x=1))` - because one central check that some writer bypasses is the same bug
// in a different place.
class ReadonlyTest : public ::testing::Test {
protected:
	lesh::buffer_pool pool{1024 * 64};
	shell_state state;
	lesh::testing::temp_path scratch;

	int run(std::string_view src) {
		const tree t = parse(pool, src);
		tree_walking_executor ex{pool, state};
		return ex.run(t);
	}
	int quietly(std::string_view src) {
		std::string wrapped{src};
		wrapped += " 2>/dev/null";
		return run(wrapped);
	}
	std::string capture(std::string_view src) { return capture_fd("1", src); }
	std::string capture_stderr(std::string_view src) { return capture_fd("2", src); }

	std::string capture_fd(std::string_view fd, std::string_view src) {
		const std::string path = scratch.file("readonly_capture.txt");
		std::remove(path.c_str());
		std::string wrapped{"{ "};
		wrapped.append(src);
		wrapped += "; } ";
		wrapped.append(fd);
		wrapped += "> ";
		wrapped += path;
		(void)run(wrapped);
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
};

} // namespace

TEST_F(ReadonlyTest, MarksTheVariableAndRefusesLaterAssignment) {
	EXPECT_EQ(run("readonly a=1"), 0);
	EXPECT_TRUE(state.is_readonly("a"));
	EXPECT_EQ(value_of("a"), "1");
	EXPECT_NE(quietly("a=2"), 0);
	EXPECT_EQ(value_of("a"), "1") << "the value must be untouched by a refused write";
}

TEST_F(ReadonlyTest, AssignmentToAReadonlyVariableIsFatalToANonInteractiveShell) {
	// POSIX 2.8.1 lists a variable assignment error among the errors that exit a
	// non-interactive shell, and dash agrees: `readonly a=1; a=2; echo not reached`
	// prints the diagnostic and nothing else, with status 2. Verified against dash
	// rather than assumed, because the opposite reading - report and carry on - is
	// what a regular builtin's failure does.
	EXPECT_EQ(capture("readonly a=1; a=2; echo not reached"), "");
	EXPECT_NE(capture_stderr("readonly a=1; a=2"), "");
	// The same write from every other direction.
	EXPECT_EQ(capture("readonly b=1; b=2 echo prefix; echo not reached"), "");
	EXPECT_EQ(capture("readonly c; for c in 1 2; do echo $c; done; echo not reached"), "");
	EXPECT_EQ(capture("readonly d; : ${d=1}; echo not reached"), "");
	EXPECT_EQ(capture("readonly e; echo $((e=1)); echo not reached"), "");
}

TEST_F(ReadonlyTest, AnInteractiveShellReportsAndCarriesOn) {
	// The other half of the same POSIX sentence, and the reason the fatality lives
	// in one place rather than in each writer.
	//
	// The guard restores the interactive SIGNAL defaults set_interactive installs
	// (#52), which are nothing to do with this test and everything to do with the
	// next one.
	const lesh::testing::interactive_disposition_guard dispositions;
	state.set_interactive(true);
	EXPECT_EQ(capture("readonly a=1; a=2 2>/dev/null; echo reached"), "reached\n");
}

TEST_F(ReadonlyTest, ReadonlyWithNoOperandsListsInReInputtableForm) {
	// POSIX requires a form the shell can read back. dash prints `readonly r='1'`,
	// and the quoting is what makes a value containing a quote survive the round
	// trip - the property that distinguishes a listing from a report.
	//
	// One shell per listing, because the flag PERSISTS: marking a name readonly
	// twice is itself an error, so a second `readonly r=1` would exit before it
	// printed anything.
	EXPECT_EQ(capture("readonly"), "") << "nothing readonly, nothing printed";
	EXPECT_EQ(capture("readonly r=1; readonly"), "readonly r='1'\n");
	EXPECT_EQ(capture("readonly -p"), "readonly r='1'\n") << "-p prints the same";
	// A name that is readonly but UNSET prints bare: `readonly u=''` on re-input
	// would create the variable the listing says does not exist. Sorted by name,
	// which is why u follows r.
	EXPECT_EQ(capture("readonly u; readonly"), "readonly r='1'\nreadonly u\n");
	EXPECT_EQ(value_of("u"), "<unset>");
	EXPECT_EQ(capture("readonly q=\"a'b\"; readonly -p"),
	          "readonly q='a'\\''b'\nreadonly r='1'\nreadonly u\n");
}

TEST_F(ReadonlyTest, OperandFormsAndBadNames) {
	// `--` ends the options: readonly-p.tst's 'separator preceding operand' is
	// `readonly -- a=foo`, which without it assigned to a variable called `--`.
	EXPECT_EQ(capture("readonly -- sep=foo; echo $sep"), "foo\n");
	EXPECT_EQ(capture("b=B; readonly a=A b c=C; echo $a $b $c"), "A B C\n");
	EXPECT_TRUE(state.is_readonly("b")) << "a name with no `=` keeps its value";
	// A name the shell could never assign is refused rather than stored under a key
	// no expansion can reach - and `readonly` is special, so it is fatal.
	EXPECT_EQ(quietly("readonly 1bad"), 2);
	EXPECT_EQ(quietly("readonly ''"), 2);
	EXPECT_EQ(quietly("readonly -Z x"), 2) << "an unknown option is an error";
	// Re-marking is fine; re-ASSIGNING is not, whatever the value.
	EXPECT_EQ(capture("readonly again=1; readonly again; echo reached"), "reached\n");
	EXPECT_EQ(quietly("readonly again=1"), 2) << "the same value is still a write";
}

TEST_F(ReadonlyTest, UnsetRefusesAReadonlyVariable) {
	// POSIX: unsetting a readonly variable is an error, and `unset` is a special
	// builtin - so it takes a non-interactive shell down with it. Both of
	// unset-p.tst's cases for this were passing against a shell that did nothing.
	EXPECT_EQ(capture("readonly a=; unset a; echo not reached"), "");
	EXPECT_EQ(capture("readonly a=; unset -v a; echo not reached"), "");
	EXPECT_NE(capture_stderr("readonly a=; unset a"), "");
	EXPECT_EQ(capture("v=1; unset -v v; echo \"[${v-unset}]\""), "[unset]\n");
}

TEST_F(ReadonlyTest, ExportInteractsWithoutAssigning) {
	// `export name` is not an assignment, so POSIX and dash both allow it on a
	// readonly variable. `export name=value` is one, and is refused.
	EXPECT_EQ(capture("readonly a=1; export a; echo reached"), "reached\n");
	EXPECT_TRUE(state.is_exported("a"));
	EXPECT_EQ(capture("readonly b=1; export b=2; echo not reached"), "");
	EXPECT_EQ(value_of("b"), "1");
	// export's own listing, which POSIX requires to be re-inputtable for the same
	// reason readonly's is. Grepped rather than compared whole: the environment this
	// process inherited is in there too.
	EXPECT_NE(capture("export ex=1; export -p").find("export ex='1'\n"),
	          std::string::npos);
}

TEST_F(ReadonlyTest, WritersReportWithTheirOwnName) {
	// dash prefixes the diagnostic with the builtin that refused - `unset: a: is
	// read only` - and leaves a plain assignment unprefixed. The prefix is how a
	// script's author finds which line refused, so it is asserted rather than left
	// to whichever writer happened to be written last.
	EXPECT_NE(capture_stderr("readonly u=; unset u").find("unset: u:"),
	          std::string::npos);
	EXPECT_NE(capture_stderr("readonly e=1; export e=2").find("export: e:"),
	          std::string::npos);
	EXPECT_NE(capture_stderr("readonly r=1; readonly r=2").find("readonly: r:"),
	          std::string::npos);
	EXPECT_NE(capture_stderr("readonly v; echo hi | { read v; }").find("read: v:"),
	          std::string::npos);
	EXPECT_NE(capture_stderr("readonly OPTIND; getopts a o -a").find("getopts: OPTIND:"),
	          std::string::npos);
}

TEST_F(ReadonlyTest, GetoptsFailsRatherThanIgnoreAReadonlyVariable) {
	// POSIX XBD 8.1 allows three answers when getopts' variables are readonly: the
	// readonly builtin fails, getopts fails, or getopts succeeds ignoring the
	// readonlyness. getopts-p.tst accepts any of them but not silence, and dash
	// takes the middle one, so lesh does too.
	EXPECT_EQ(quietly("readonly OPTIND; getopts a o -a"), 2);
	EXPECT_EQ(quietly("readonly OPTARG; getopts a: o -a foo"), 2);
	EXPECT_EQ(quietly("readonly o; getopts a o -a"), 2);
}

TEST_F(ReadonlyTest, AReadonlyNameIsAbsentUntilItIsAssigned) {
	// `readonly x` creates the entry that holds the flag, and that entry must not
	// look like a variable: dash reports `${x-unset}` as unset, and `set -u` has to
	// treat it as unset too.
	EXPECT_EQ(capture("readonly x; echo ${x-unset}"), "unset\n");
	EXPECT_EQ(capture("readonly x; set -u; echo ${x-unset}"), "unset\n");
	EXPECT_FALSE(state.is_exported("x"));
}

// `export`, and the same question `readonly` answers. See issue #71.
//
// The two builtins ask whether MARKING a name creates it, and answered it two
// ways: `readonly x` recorded the flag on an entry that lookup() reports as
// absent, while `export x` fabricated an assignment of the empty string. So
// `${x-unset}` and `${x:-empty}` stopped being distinguishable after a bare
// `export x`, and a child saw `x=` where dash exports nothing. POSIX marks a name
// for export "whether or not it is set": the attribute belongs to the NAME. Both
// now use the one `assigned` flag on the variable, which is where #24 put
// readonly's, so the answer cannot drift apart again.

TEST_F(ReadonlyTest, ExportMarksANameWithoutCreatingTheVariable) {
	EXPECT_EQ(run("export m"), 0);
	EXPECT_TRUE(state.is_exported("m")) << "the NAME is marked";
	EXPECT_EQ(value_of("m"), "<unset>") << "and no variable was created for it";
	// The distinction the empty value destroyed: `-` tests for set, `:-` for
	// set-and-non-empty, and dash tells them apart here.
	EXPECT_EQ(capture("export m; echo \"[${m-unset}][${m:-empty}]\""), "[unset][empty]\n");
	// `set -u` has to see it as unset too, for the same reason it does a readonly
	// name that was never assigned.
	EXPECT_EQ(capture("export m; set -u; echo ${m-unset}"), "unset\n");
}

TEST_F(ReadonlyTest, AMarkedButUnsetNameIsInNoChildsEnvironment) {
	// Measured against dash first: `dash -c 'export A; env'` prints no A line and
	// `sh -c 'echo ${A-unset}'` under it says unset. An `A=` in the block would be
	// a variable the child can see and the parent cannot.
	EXPECT_EQ(run("export gone"), 0);
	bool found = false;
	for (char** env = state.environment_block(); *env != nullptr; ++env)
		if (std::string_view{*env}.starts_with("gone="))
			found = true;
	EXPECT_FALSE(found) << "a marked name with no value must not reach a child";

	// And the mark is still there to catch the value when one arrives.
	EXPECT_EQ(run("gone=1"), 0);
	found = false;
	for (char** env = state.environment_block(); *env != nullptr; ++env)
		if (std::string_view{*env} == "gone=1")
			found = true;
	EXPECT_TRUE(found) << "assigning after export must export the value";
}

TEST_F(ReadonlyTest, ExportListsAMarkedButUnsetNameBare) {
	// POSIX requires the listing to be RE-INPUTTABLE, which is the defect #40 and
	// #38 both hit: `export m=''` names a variable that does not exist, so reading
	// the listing back would CREATE it. dash prints `export m`, and so does this.
	// Grepped rather than compared whole - the inherited environment is in there.
	const std::string listed = capture("export m; export -p");
	EXPECT_NE(listed.find("export m\n"), std::string::npos);
	EXPECT_EQ(listed.find("export m="), std::string::npos);
	// The round trip itself, which is what the requirement is FOR.
	EXPECT_EQ(capture("export m; e='export m'; unset m; eval \"$e\"; echo \"[${m-unset}]\""),
	          "[unset]\n");
}

TEST_F(ReadonlyTest, ExportOfASplitOperandMarksEveryNameItYields) {
	// How this surfaced (#55, #71): `export $n` with n='a b' field-splits into two
	// operands, and each was being ASSIGNED the empty string rather than marked.
	EXPECT_EQ(capture("n='p q'; export $n; echo \"[${p-unset}][${q-unset}]\""),
	          "[unset][unset]\n");
	EXPECT_TRUE(state.is_exported("p"));
	EXPECT_TRUE(state.is_exported("q"));
}

TEST_F(ReadonlyTest, ExportWithAValueStillAssignsAndStillExports) {
	// The half that was always right, asserted so the fix above cannot take it out.
	EXPECT_EQ(run("export v=1"), 0);
	EXPECT_EQ(value_of("v"), "1");
	EXPECT_TRUE(state.is_exported("v"));
	EXPECT_NE(capture("export v=1; export -p").find("export v='1'\n"), std::string::npos);
}

TEST_F(ReadonlyTest, MarkingForExportIsNotAnAssignmentToAReadonlyName) {
	// `export r` on a readonly-and-unset r is legal - marking is not writing - and
	// it must not smuggle in the empty value that would make `r=1` impossible for a
	// reason the author never asked for.
	EXPECT_EQ(capture("readonly r; export r; echo \"[${r-unset}]\""), "[unset]\n");
	EXPECT_TRUE(state.is_exported("r"));
	EXPECT_TRUE(state.is_readonly("r"));
	// #24's enforcement is untouched: the name is readonly while unset.
	EXPECT_EQ(capture("readonly r; export r; r=1; echo not reached"), "");
}
