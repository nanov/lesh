#include "leshper/abi.h"
#include "ui/completion.h"
#include "ui/editor_host.h"
#include "leshper/pager.h"
#include "leshper/registry.h"
#include "ui/shell_knowledge.h"
#include "leshper/state.h"
#include "runtime/shell_state.h"
#include "substrate/arena.h"
#include "ui/shell_state_knowledge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lesh::leshper;
using namespace lesh::ui;

// The v1 completer (#139, #137, spec §6.9): three sources, synchronously, on the
// loop thread.
//
// FOUR LAYERS, each tested against the one below rather than a mock of it:
//
//   `classify_token` - C-6's lexer, asked what is under the cursor. A pure
//   function of (buffer, cursor), so a case is one line and there are many.
//
//   `shell_completer` - the trio, over a FAKE `shell_knowledge` (what a shell
//   would answer) and a FAKE `directory_reader` (what a filesystem would). Since
//   #151 the completer holds the knowledge DIRECTLY and calls `enumerate` on the
//   loop thread, so the fake is the same shape the wiring site's adapter is and
//   there is no round trip left to stand in for. Faking the
//   filesystem is what makes the path rules testable without a temporary
//   directory per case; the real `posix_directory_reader` gets its own test
//   against a real directory, because that is the only place `d_type` and
//   `stat` can disagree.
//
//   `shell_state_knowledge::enumerate` - the ui layer's adapter, over a REAL
//   `shell_state` with real aliases, functions and a real `PATH`. It is the one
//   join between the completer and the shell and faking it here would leave it
//   unchecked.
//
//   The `complete` ACTION and the ABI verbs under it, dispatched by name through
//   `loop_harness` exactly as the loop dispatches them. Nothing here calls into
//   builtin_actions.cpp directly, because there is no other entry point.
//
// WHAT IS NOT HERE: the pager (#138), the keymap binding for Tab (#141), and any
// concurrency. There is no enumeration round trip left to cover (#151 deleted
// the slot); the rule that makes the direct read legal is asserted where it can
// fail, in ui_command_kind_tests.cpp.

namespace {

// A `shell_knowledge` that is a map, which is what a test has. It counts its
// calls, because "one read per Tab per domain" is a claim about how often this
// gets asked.
//
// `classify` and `path` are the other half of the interface and no completer
// asks them; they answer honestly rather than aborting, so that this fake is a
// whole `shell_knowledge` and not a `name_source` wearing its name.
class fake_names final : public shell_knowledge {
public:
	void define(name_domain which, std::vector<std::string> names) {
		_tables.insert_or_assign(static_cast<int>(which), std::move(names));
	}

	[[nodiscard]] command_kind classify(std::string_view) const override {
		return command_kind::unknown;
	}

	[[nodiscard]] bool path(std::string_view&) const override { return false; }

	void enumerate(name_domain which, std::vector<std::string>& into) const override {
		asked.push_back(which);
		const auto found = _tables.find(static_cast<int>(which));
		if (found == _tables.end())
			return;   // a shell with an empty table is not a shell with none
		into.insert(into.end(), found->second.begin(), found->second.end());
	}

	mutable std::vector<name_domain> asked;

private:
	std::map<int, std::vector<std::string>> _tables;
};

// A `directory_reader` that is a map from path to entries.
class fake_directories final : public directory_reader {
public:
	void file(std::string where, std::string name, bool executable = false) {
		entry one;
		one.name = std::move(name);
		one.executable = executable;
		_tree[std::move(where)].push_back(std::move(one));
	}
	void directory(std::string where, std::string name) {
		entry one;
		one.name = std::move(name);
		one.directory = true;
		_tree[std::move(where)].push_back(std::move(one));
	}
	void symlink(std::string where, std::string name) {
		entry one;
		one.name = std::move(name);
		one.symlink = true;
		_tree[std::move(where)].push_back(std::move(one));
	}

	bool read(std::string_view path, std::vector<entry>& into) const override {
		visited.emplace_back(path);
		const auto found = _tree.find(std::string{path});
		if (found == _tree.end())
			return false;
		into.insert(into.end(), found->second.begin(), found->second.end());
		return true;
	}

	mutable std::vector<std::string> visited;

private:
	std::map<std::string, std::vector<entry>> _tree;
};

struct complete_fixture {
	fake_names names;
	fake_directories directories;
	// "No shell attached" is a NULL KNOWLEDGE since #151, where it used to be a
	// `name_source` that answered false. Same meaning, one fewer way to say it.
	bool attached = true;

	[[nodiscard]] completion_result run(std::string_view buffer, std::size_t cursor) {
		const shell_completer completer{attached ? &names : nullptr, directories};
		completion_result result;
		completer.complete(completion_query{buffer, cursor}, result);
		return result;
	}

	[[nodiscard]] completion_result run(std::string_view buffer) {
		return run(buffer, buffer.size());
	}

	[[nodiscard]] static std::vector<std::string> texts(const completion_result& result) {
		std::vector<std::string> out;
		out.reserve(result.candidates.size());
		for (const candidate& one : result.candidates)
			out.push_back(one.text);
		return out;
	}
};

// The action-side fixture: a registry with the built-ins, a completer wired onto
// it, and the harness the loop dispatches through.
struct action_fixture {
	fake_names names;
	fake_directories directories;
	registry reg;
	loop_harness loop{reg};
	shell_completer completer{&names, directories};
	// THE ONE DOOR (#168 Phase B). `reg.completion` was a `completer*`; the
	// completer is behind `leshper::host` now, with the shell's tables, and
	// `lesh_complete` raises a `want_completion` the host carries out where it
	// stands.
	editor_host host{&names, &completer};
	// "No completer wired up", for the tests that need it: a host with a null
	// completer, which is what a leshper embedded with no completion sources has.
	editor_host completerless{&names};

	action_fixture() {
		register_builtin_actions(reg);
		register_pager_actions(reg);
		reg.host = &host;
	}

	[[nodiscard]] static lesh::leshper::state line(std::string_view text) {
		lesh::leshper::state s;
		if (!text.empty())
			s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), text);
		s.cursor = s.buffer.end_position();
		s.gen.bump();
		return s;
	}

	// One Tab press, dispatched by name.
	action_result tab(lesh::leshper::state& s) {
		return loop.invoke(s, "complete_word", invocation{});
	}
};

std::filesystem::path repository_root() {
	std::filesystem::path here{__FILE__};
	return here.parent_path().parent_path().parent_path();   // tests/unit/<this>
}

} // namespace

// ---------------------------------------------------------------------------
// C-6: the token under the cursor
// ---------------------------------------------------------------------------

TEST(UiCompleteToken, AnEmptyLineIsCommandPosition) {
	const token_under_cursor found = classify_token("", 0);
	EXPECT_EQ(found.source, completion_source::command);
	EXPECT_EQ(found.from, 0u);
}

TEST(UiCompleteToken, TheFirstWordIsACommandName) {
	const token_under_cursor found = classify_token("gre", 3);
	EXPECT_EQ(found.source, completion_source::command);
	EXPECT_EQ(found.from, 0u);
}

TEST(UiCompleteToken, TheSecondWordIsAPath) {
	const token_under_cursor found = classify_token("grep fo", 7);
	EXPECT_EQ(found.source, completion_source::path);
	EXPECT_EQ(found.from, 5u);
	EXPECT_EQ(found.directory, "");
}

TEST(UiCompleteToken, AFreshWordAfterABlankIsStillAnArgument) {
	const token_under_cursor found = classify_token("grep ", 5);
	EXPECT_EQ(found.source, completion_source::path);
	EXPECT_EQ(found.from, 5u);
}

TEST(UiCompleteToken, EveryCommandOpenerRestoresCommandPosition) {
	for (const std::string_view line : {"ls |", "ls ||", "ls &&", "ls;", "ls &", "ls\n",
	                                    "(ls;", "ls;;"}) {
		const token_under_cursor found = classify_token(line, line.size());
		EXPECT_EQ(found.source, completion_source::command) << line;
		EXPECT_EQ(found.from, line.size()) << line;
	}
}

TEST(UiCompleteToken, AWordRightAfterAPipeIsACommandName) {
	const token_under_cursor found = classify_token("ls | gre", 8);
	EXPECT_EQ(found.source, completion_source::command);
	EXPECT_EQ(found.from, 5u);
}

// POSIX 2.10.2: an assignment PRECEDES command position, it does not end it.
TEST(UiCompleteToken, AnAssignmentDoesNotEndCommandPosition) {
	const token_under_cursor found = classify_token("FOO=bar gre", 11);
	EXPECT_EQ(found.source, completion_source::command);
	EXPECT_EQ(found.from, 8u);
}

TEST(UiCompleteToken, AWordThatIsAnAssignmentIsNotItselfACommandName) {
	// The cursor is inside `FOO=ba`, which is not a name to look up in a table.
	const token_under_cursor found = classify_token("FOO=ba", 6);
	EXPECT_EQ(found.source, completion_source::path);
}

TEST(UiCompleteToken, ARedirectionTargetIsAPathAndDoesNotEndCommandPosition) {
	const token_under_cursor target = classify_token("> ou", 4);
	EXPECT_EQ(target.source, completion_source::path);
	// The command name after the redirection is still a command name.
	const token_under_cursor after = classify_token("> out gre", 9);
	EXPECT_EQ(after.source, completion_source::command);
}

// POSIX 2.9.1.1: a command name containing a slash is a pathname.
TEST(UiCompleteToken, ACommandNameWithASlashIsAPath) {
	const token_under_cursor found = classify_token("./bi", 4);
	EXPECT_EQ(found.source, completion_source::path);
	EXPECT_EQ(found.directory, "./");
	EXPECT_EQ(found.from, 2u);
}

TEST(UiCompleteToken, TheDirectoryPartStopsAtTheLastSlash) {
	const token_under_cursor found = classify_token("cat /usr/loc", 12);
	EXPECT_EQ(found.source, completion_source::path);
	EXPECT_EQ(found.directory, "/usr/");
	EXPECT_EQ(found.from, 9u);
}

TEST(UiCompleteToken, ATildeIsKeptOutOfTheReplacedRange) {
	const token_under_cursor found = classify_token("cat ~/Doc", 9);
	EXPECT_EQ(found.source, completion_source::path);
	EXPECT_EQ(found.directory, "~/");
	EXPECT_EQ(found.from, 6u);
}

TEST(UiCompleteToken, ADollarTailIsAVariable) {
	const token_under_cursor found = classify_token("echo $HO", 8);
	EXPECT_EQ(found.source, completion_source::variable);
	EXPECT_EQ(found.from, 6u);   // past the `$`, so the `$` stays in the buffer
}

TEST(UiCompleteToken, ABracedDollarTailIsAVariable) {
	const token_under_cursor found = classify_token("echo ${HO", 9);
	EXPECT_EQ(found.source, completion_source::variable);
	EXPECT_EQ(found.from, 7u);
}

TEST(UiCompleteToken, ABareDollarOffersEveryVariable) {
	const token_under_cursor found = classify_token("echo $", 6);
	EXPECT_EQ(found.source, completion_source::variable);
	EXPECT_EQ(found.from, 6u);
}

TEST(UiCompleteToken, ADollarInsideAWordStillCompletesTheVariable) {
	const token_under_cursor found = classify_token("echo abc$HO", 11);
	EXPECT_EQ(found.source, completion_source::variable);
	EXPECT_EQ(found.from, 9u);
}

// The no-expander rule, made visible: a prefix whose `$` is not a plain
// parameter cannot be completed, because completing it would mean knowing what
// it expands to.
TEST(UiCompleteToken, APrefixWithAnExpansionThatIsNotANameCompletesNothing) {
	for (const std::string_view line : {"cat $(ec", "cat ${#fo", "cat $?x"}) {
		const token_under_cursor found = classify_token(line, line.size());
		EXPECT_EQ(found.source, completion_source::none) << line;
	}
}

TEST(UiCompleteToken, InsideACommentThereIsNothingToComplete) {
	const token_under_cursor found = classify_token("ls # not a com", 14);
	EXPECT_EQ(found.source, completion_source::none);
}

TEST(UiCompleteToken, AnOperatorUnderTheCursorCompletesNothing) {
	// The cursor is INSIDE the `&&`, between its two bytes.
	const token_under_cursor found = classify_token("ls && grep", 4);
	EXPECT_EQ(found.source, completion_source::none);
}

TEST(UiCompleteToken, ACursorInsideAWordCompletesWhatIsBehindIt) {
	const token_under_cursor found = classify_token("grep foobar", 8);
	EXPECT_EQ(found.source, completion_source::path);
	EXPECT_EQ(found.from, 5u);
}

TEST(UiCompleteToken, ACursorPastTheEndIsClamped) {
	const token_under_cursor found = classify_token("gre", 99);
	EXPECT_EQ(found.source, completion_source::command);
	EXPECT_EQ(found.from, 0u);
}

// ---------------------------------------------------------------------------
// Quoting
// ---------------------------------------------------------------------------

TEST(UiCompleteQuoting, OnlyBytesTheShellRereadsDifferentlyNeedIt) {
	EXPECT_FALSE(needs_quoting("plain.txt"));
	EXPECT_FALSE(needs_quoting(""));
	EXPECT_TRUE(needs_quoting("my file"));
	EXPECT_TRUE(needs_quoting("a$b"));
	EXPECT_TRUE(needs_quoting("a*b"));
	// Conditional: special only where a word begins.
	EXPECT_TRUE(needs_quoting("~home"));
	EXPECT_FALSE(needs_quoting("a~b"));
	EXPECT_TRUE(needs_quoting("#hash"));
	EXPECT_FALSE(needs_quoting("a#b"));
}

TEST(UiCompleteQuoting, QuotingIsABackslashPerSpecialByte) {
	std::string into;
	quote_into("my file.txt", into);
	EXPECT_EQ(into, "my\\ file.txt");
	into.clear();
	quote_into("~x", into);
	EXPECT_EQ(into, "\\~x");
}

TEST(UiCompleteQuoting, UnquotingUndoesBackslashesAndBothQuoteForms) {
	const auto unquoted = [](std::string_view text) {
		std::string out;
		unquote_into(text, out);
		return out;
	};
	EXPECT_EQ(unquoted("my\\ fi"), "my fi");
	EXPECT_EQ(unquoted("'my fi"), "my fi");           // unterminated: still typing
	EXPECT_EQ(unquoted("'my fi'le"), "my file");
	EXPECT_EQ(unquoted("\"a\\\"b"), "a\"b");
	EXPECT_EQ(unquoted("plain"), "plain");
}

// ---------------------------------------------------------------------------
// The trio: command names
// ---------------------------------------------------------------------------

TEST(UiCompleteCommands, TheThreeTablesAndThePathWalkAllContribute) {
	complete_fixture fixture;
	fixture.names.define(name_domain::builtin, {"cd", "command"});
	fixture.names.define(name_domain::function, {"codebase"});
	fixture.names.define(name_domain::alias, {"co"});
	fixture.names.define(name_domain::path_directory, {"/bin"});
	fixture.directories.file("/bin", "cowsay", true);

	const completion_result result = fixture.run("c");
	EXPECT_EQ(complete_fixture::texts(result),
	          (std::vector<std::string>{"cd", "co", "codebase", "command", "cowsay"}));
}

TEST(UiCompleteCommands, EachSourceKeepsItsMarker) {
	complete_fixture fixture;
	fixture.names.define(name_domain::builtin, {"xb"});
	fixture.names.define(name_domain::function, {"xf"});
	fixture.names.define(name_domain::alias, {"xa"});
	fixture.names.define(name_domain::path_directory, {"/bin"});
	fixture.directories.file("/bin", "xp", true);

	const completion_result result = fixture.run("x");
	ASSERT_EQ(result.candidates.size(), 4u);
	// Sorted by text: xa, xb, xf, xp.
	EXPECT_EQ(result.candidates[0].kind, pager_kind::word);
	EXPECT_EQ(result.candidates[1].kind, pager_kind::word);
	EXPECT_EQ(result.candidates[2].kind, pager_kind::word);
	EXPECT_EQ(result.candidates[3].kind, pager_kind::executable);
}

TEST(UiCompleteCommands, ANonExecutableOnPathIsNotACommand) {
	complete_fixture fixture;
	fixture.names.define(name_domain::path_directory, {"/bin"});
	fixture.directories.file("/bin", "readme", false);
	fixture.directories.file("/bin", "runner", true);
	EXPECT_EQ(complete_fixture::texts(fixture.run("r")),
	          (std::vector<std::string>{"runner"}));
}

TEST(UiCompleteCommands, ADirectoryOnPathIsNotACommand) {
	complete_fixture fixture;
	fixture.names.define(name_domain::path_directory, {"/bin"});
	fixture.directories.directory("/bin", "subdir");
	EXPECT_TRUE(fixture.run("s").candidates.empty());
}

TEST(UiCompleteCommands, ARepeatedPathElementIsWalkedOnce) {
	complete_fixture fixture;
	fixture.names.define(name_domain::path_directory, {"/bin", "/bin", "/usr/bin"});
	fixture.directories.file("/bin", "zz", true);
	(void)fixture.run("z");
	EXPECT_EQ(fixture.directories.visited,
	          (std::vector<std::string>{"/bin", "/usr/bin"}));
}

TEST(UiCompleteCommands, OneNameFromTwoSourcesIsOneCandidate) {
	complete_fixture fixture;
	fixture.names.define(name_domain::builtin, {"cd"});
	fixture.names.define(name_domain::function, {"cd"});
	EXPECT_EQ(complete_fixture::texts(fixture.run("cd")), (std::vector<std::string>{"cd"}));
}

TEST(UiCompleteCommands, EachDomainIsAskedExactlyOncePerTab) {
	complete_fixture fixture;
	fixture.names.define(name_domain::builtin, {"cd"});
	(void)fixture.run("c");
	EXPECT_EQ(fixture.names.asked,
	          (std::vector<name_domain>{name_domain::builtin, name_domain::function,
	                                    name_domain::alias, name_domain::path_directory}));
}

TEST(UiCompleteCommands, WithNoShellAttachedThereAreNoCommandNames) {
	complete_fixture fixture;
	fixture.names.define(name_domain::builtin, {"cd"});
	fixture.attached = false;
	EXPECT_TRUE(fixture.run("c").candidates.empty());
}

// ---------------------------------------------------------------------------
// The trio: variables
// ---------------------------------------------------------------------------

TEST(UiCompleteVariables, ADollarPrefixCompletesNamesAndKeepsTheDollar) {
	complete_fixture fixture;
	fixture.names.define(name_domain::variable, {"HOME", "HOSTNAME", "PATH"});
	const completion_result result = fixture.run("echo $HO");
	EXPECT_EQ(complete_fixture::texts(result),
	          (std::vector<std::string>{"HOME", "HOSTNAME"}));
	EXPECT_EQ(result.replace_from, 6u);
	EXPECT_EQ(result.replace_to, 8u);
	EXPECT_EQ(result.candidates[0].kind, pager_kind::word);
}

TEST(UiCompleteVariables, OnlyTheVariableDomainIsAsked) {
	complete_fixture fixture;
	fixture.names.define(name_domain::variable, {"HOME"});
	(void)fixture.run("echo $H");
	EXPECT_EQ(fixture.names.asked, (std::vector<name_domain>{name_domain::variable}));
}

// ---------------------------------------------------------------------------
// The trio: paths
// ---------------------------------------------------------------------------

TEST(UiCompletePaths, TheCurrentDirectoryIsWalkedForABareLeaf) {
	complete_fixture fixture;
	fixture.directories.file("", "notes.txt");
	fixture.directories.directory("", "node_modules");
	EXPECT_EQ(complete_fixture::texts(fixture.run("cat no")),
	          (std::vector<std::string>{"node_modules", "notes.txt"}));
}

TEST(UiCompletePaths, ADirectoryGetsASlashAndStaysOpen) {
	complete_fixture fixture;
	fixture.directories.directory("", "build");
	const completion_result result = fixture.run("cat bu");
	ASSERT_EQ(result.candidates.size(), 1u);
	// BARE TEXT, and the kind carries the `/`: pager.h's one-spelling rule.
	EXPECT_EQ(result.candidates[0].text, "build");
	EXPECT_EQ(result.candidates[0].kind, pager_kind::directory);
	EXPECT_EQ(pager_trailer(result.candidates[0].kind), "/");
}

TEST(UiCompletePaths, AFileClosesWithASpace) {
	complete_fixture fixture;
	fixture.directories.file("", "README");
	const completion_result result = fixture.run("cat RE");
	ASSERT_EQ(result.candidates.size(), 1u);
	EXPECT_EQ(result.candidates[0].text, "README");
	EXPECT_EQ(result.candidates[0].kind, pager_kind::word);
	EXPECT_EQ(pager_trailer(result.candidates[0].kind), " ");
}

TEST(UiCompletePaths, ASymlinkGetsItsOwnMarker) {
	complete_fixture fixture;
	fixture.directories.symlink("", "link");
	const completion_result result = fixture.run("cat li");
	ASSERT_EQ(result.candidates.size(), 1u);
	EXPECT_EQ(result.candidates[0].kind, pager_kind::symlink);
}

TEST(UiCompletePaths, ADotfileIsOfferedOnlyWhenTheLeafAsksForOne) {
	complete_fixture fixture;
	fixture.directories.file("", ".hidden");
	fixture.directories.file("", "visible");
	EXPECT_EQ(complete_fixture::texts(fixture.run("cat ")),
	          (std::vector<std::string>{"visible"}));
	EXPECT_EQ(complete_fixture::texts(fixture.run("cat .")),
	          (std::vector<std::string>{".hidden"}));
}

TEST(UiCompletePaths, TheDirectoryPrefixIsWalkedAndNotReplaced) {
	complete_fixture fixture;
	fixture.directories.file("/usr/", "share");
	const completion_result result = fixture.run("cat /usr/sh");
	EXPECT_EQ(fixture.directories.visited, (std::vector<std::string>{"/usr/"}));
	EXPECT_EQ(result.replace_from, 9u);
	EXPECT_EQ(result.replace_to, 11u);
	EXPECT_EQ(complete_fixture::texts(result), (std::vector<std::string>{"share"}));
}

TEST(UiCompletePaths, AnUnreadableDirectoryIsNoCandidatesAndNotAnError) {
	complete_fixture fixture;
	const completion_result result = fixture.run("cat /nowhere/x");
	EXPECT_TRUE(result.candidates.empty());
}

// §6.9: `~` expanded for the LISTING, kept in the BUFFER.
TEST(UiCompletePaths, ATildeIsExpandedForTheWalkAndKeptInTheBuffer) {
	complete_fixture fixture;
	const char* home = ::getenv("HOME");
	ASSERT_NE(home, nullptr);
	fixture.directories.file(std::string{home} + "/", "Documents");
	const completion_result result = fixture.run("cat ~/Doc");
	EXPECT_EQ(fixture.directories.visited, (std::vector<std::string>{std::string{home} + "/"}));
	// The replacement range starts past the `~/`, so the tilde is never rewritten.
	EXPECT_EQ(result.replace_from, 6u);
	EXPECT_EQ(complete_fixture::texts(result), (std::vector<std::string>{"Documents"}));
}

TEST(UiCompletePaths, ATildeUserIsNotExpandedAndOffersNothing) {
	complete_fixture fixture;
	fixture.directories.file("", "x");
	const completion_result result = fixture.run("cat ~someone/x");
	EXPECT_TRUE(result.candidates.empty());
	EXPECT_TRUE(fixture.directories.visited.empty());
}

TEST(UiCompletePaths, WhatTheUserEscapedIsMatchedUnescapedAndQuotedBack) {
	complete_fixture fixture;
	fixture.directories.file("", "my file.txt");
	const completion_result result = fixture.run("cat my\\ fi");
	ASSERT_EQ(result.candidates.size(), 1u);
	// ONE SPELLING: what the pager shows is what it inserts, so the candidate IS
	// the quoted form. The quoting covers the replaced component only - `cat `
	// is outside the range and is never touched.
	EXPECT_EQ(result.candidates[0].text, "my\\ file.txt");
	EXPECT_EQ(result.replace_from, 4u);
	EXPECT_EQ(result.replace_to, 10u);
}

// ---------------------------------------------------------------------------
// F-30 is NOT here
//
// `decide_pager` owns it (#138), and the completer must not have a second
// opinion. What is asserted here is only that the completer hands over a set
// the pager can decide about - and the decision itself is asserted through the
// action, below, where the real `lesh_pager_commit` makes it.
// ---------------------------------------------------------------------------

TEST(UiCompleteSet, NothingMatchedIsAnEmptySetAndNotAnError) {
	complete_fixture fixture;
	EXPECT_TRUE(fixture.run("cat zz").candidates.empty());
}

TEST(UiCompleteSet, TheSetIsSortedAndDeduplicated) {
	complete_fixture fixture;
	fixture.directories.file("", "report");
	fixture.directories.file("", "readme");
	fixture.names.define(name_domain::builtin, {"readme"});
	const completion_result result = fixture.run("cat re");
	EXPECT_EQ(complete_fixture::texts(result),
	          (std::vector<std::string>{"readme", "report"}));
}

// ---------------------------------------------------------------------------
// The wiring-site adapter, over a real shell_state
// ---------------------------------------------------------------------------

TEST(UiCompleteKnowledge, TheThreeTablesEnumerateThroughTheAdapter) {
	lesh::runtime::shell_state state;
	state.set_alias("ll", "ls -l");
	const shell_state_knowledge knowledge{state};

	std::vector<std::string> builtins;
	knowledge.enumerate(name_domain::builtin, builtins);
	EXPECT_NE(std::find(builtins.begin(), builtins.end(), "cd"), builtins.end());
	EXPECT_NE(std::find(builtins.begin(), builtins.end(), "export"), builtins.end());

	std::vector<std::string> aliases;
	knowledge.enumerate(name_domain::alias, aliases);
	EXPECT_EQ(aliases, (std::vector<std::string>{"ll"}));
}

TEST(UiCompleteKnowledge, FunctionsEnumerateSortedAndByNameOnly) {
	lesh::buffer_pool pool{1 << 16};
	lesh::runtime::shell_state state;
	const lesh::syntax::tree parsed =
		lesh::syntax::parse(pool, "zeta() { :; }\nalpha() { :; }\n", nullptr);
	// Whatever the parser produced, define the two names directly - the shape of
	// the tree is #106's business and not this door's.
	state.define_function("zeta", parsed, parsed.root());
	state.define_function("alpha", parsed, parsed.root());

	const shell_state_knowledge knowledge{state};
	std::vector<std::string> functions;
	knowledge.enumerate(name_domain::function, functions);
	EXPECT_EQ(functions, (std::vector<std::string>{"alpha", "zeta"}));
}

TEST(UiCompleteKnowledge, VariablesEnumerateByNameAndIncludeMarkedOnes) {
	lesh::runtime::shell_state state;
	EXPECT_TRUE(state.set("ZZ_ASSIGNED", "1"));
	state.mark_exported("ZZ_MARKED");
	const shell_state_knowledge knowledge{state};

	std::vector<std::string> names;
	knowledge.enumerate(name_domain::variable, names);
	EXPECT_NE(std::find(names.begin(), names.end(), "ZZ_ASSIGNED"), names.end());
	EXPECT_NE(std::find(names.begin(), names.end(), "ZZ_MARKED"), names.end());
	// Names only: no value ever crosses.
	for (const std::string& one : names)
		EXPECT_EQ(one.find('='), std::string::npos);
}

TEST(UiCompleteKnowledge, ThePathIsSplitByTheShellAndTheWalkIsNot) {
	lesh::runtime::shell_state state;
	EXPECT_TRUE(state.set("PATH", "/bin::/usr/bin"));
	const shell_state_knowledge knowledge{state};
	std::vector<std::string> directories;
	knowledge.enumerate(name_domain::path_directory, directories);
	// POSIX 2.6: an empty element means the current directory, applied once,
	// here.
	EXPECT_EQ(directories, (std::vector<std::string>{"/bin", ".", "/usr/bin"}));
}

TEST(UiCompleteKnowledge, AnUnsetPathIsNoDirectoriesAndAnEmptyOneIsDot) {
	lesh::runtime::shell_state state;
	EXPECT_TRUE(state.unset("PATH"));
	const shell_state_knowledge unset{state};
	std::vector<std::string> none;
	unset.enumerate(name_domain::path_directory, none);
	EXPECT_TRUE(none.empty());

	EXPECT_TRUE(state.set("PATH", ""));
	std::vector<std::string> empty;
	unset.enumerate(name_domain::path_directory, empty);
	EXPECT_EQ(empty, (std::vector<std::string>{"."}));
}

TEST(UiCompleteKnowledge, TheDefaultKnowledgeEnumeratesNothing) {
	const environment_knowledge nothing;
	std::vector<std::string> names;
	nothing.enumerate(name_domain::builtin, names);
	nothing.enumerate(name_domain::variable, names);
	EXPECT_TRUE(names.empty());
}

// ---------------------------------------------------------------------------
// The real directory reader
// ---------------------------------------------------------------------------

TEST(UiCompleteReader, ARealDirectoryIsReadWithItsKinds) {
	const std::filesystem::path root = repository_root();
	std::vector<directory_reader::entry> entries;
	ASSERT_TRUE(posix_directory_reader().read(root.string(), entries));

	const auto find = [&](std::string_view name) -> const directory_reader::entry* {
		for (const directory_reader::entry& one : entries)
			if (one.name == name)
				return &one;
		return nullptr;
	};
	const directory_reader::entry* src = find("src");
	ASSERT_NE(src, nullptr);
	EXPECT_TRUE(src->directory);
	const directory_reader::entry* readme = find("CMakeLists.txt");
	ASSERT_NE(readme, nullptr);
	EXPECT_FALSE(readme->directory);
	// `.` and `..` are never candidates.
	EXPECT_EQ(find("."), nullptr);
	EXPECT_EQ(find(".."), nullptr);
}

TEST(UiCompleteReader, ADirectoryThatIsNotThereIsFalseAndNotACrash) {
	std::vector<directory_reader::entry> entries;
	EXPECT_FALSE(posix_directory_reader().read("/no/such/place/at/all", entries));
	EXPECT_TRUE(entries.empty());
}

// ---------------------------------------------------------------------------
// The ABI verbs and the `complete_word` action
//
// Driven through `loop_harness::invoke` by NAME, which is how the loop
// dispatches. What these assert is the join between the completer and #138's
// pager: the action asks, feeds `lesh_pager_open`/`_add`, and commits - and
// `lesh_pager_commit` is what decides F-30, so a test that asserts the buffer
// after Tab is asserting the two halves agreeing.
// ---------------------------------------------------------------------------

TEST(UiCompleteAction, TheActionIsRegisteredUnderItsName) {
	registry reg;
	register_builtin_actions(reg);
	std::int32_t exists = 0;
	EXPECT_EQ(lesh_action_exists(&reg, "complete_word", &exists), LESH_OK);
	EXPECT_EQ(exists, 1);
}

TEST(UiCompleteAction, ASingleCandidateIsInsertedWithItsKindsTrailer) {
	action_fixture fixture;
	fixture.directories.file("", "README");
	lesh::leshper::state s = action_fixture::line("cat RE");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat README ");
	EXPECT_EQ(s.cursor.byte_offset(), 11u);
	EXPECT_FALSE(s.pager.open);
}

// §6.9: "directories complete with `/` and stay open". The `/` is the pager's,
// from the kind; staying open is the completer's - it re-runs, and what it
// finds the second time is the directory's contents.
TEST(UiCompleteAction, ADirectoryGetsItsSlashAndTheCompletionStaysOpen) {
	action_fixture fixture;
	fixture.directories.directory("", "build");
	fixture.directories.file("build/", "one");
	fixture.directories.file("build/", "two");
	lesh::leshper::state s = action_fixture::line("cat bu");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat build/");
	// The re-run happened: the pager is showing what is inside.
	EXPECT_TRUE(s.pager.open);
	ASSERT_EQ(s.pager.candidates.size(), 2u);
	EXPECT_EQ(s.pager.candidates[0].text, "one");
}

TEST(UiCompleteAction, ADirectoryWithOneEntryCompletesItAndStops) {
	action_fixture fixture;
	fixture.directories.directory("", "build");
	fixture.directories.file("build/", "only");
	lesh::leshper::state s = action_fixture::line("cat bu");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat build/only ");
	EXPECT_FALSE(s.pager.open);
}

// F-30, made by `lesh_pager_commit` and not by anything in this ticket.
TEST(UiCompleteAction, TheSharedPrefixIsInsertedWithoutOpeningThePager) {
	action_fixture fixture;
	fixture.directories.file("", "report-a");
	fixture.directories.file("", "report-b");
	lesh::leshper::state s = action_fixture::line("cat re");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat report-");
	EXPECT_FALSE(s.pager.open);
}

TEST(UiCompleteAction, AnAmbiguousSetOpensThePagerOverTheTokensSpan) {
	action_fixture fixture;
	fixture.directories.file("", "report");
	fixture.directories.file("", "readme");
	lesh::leshper::state s = action_fixture::line("cat re");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat re");
	EXPECT_TRUE(s.pager.open);
	ASSERT_EQ(s.pager.candidates.size(), 2u);
	EXPECT_EQ(s.pager.candidates[0].text, "readme");
	EXPECT_EQ(s.pager.replace_from.byte_offset(), 4u);
	EXPECT_EQ(s.pager.replace_to.byte_offset(), 6u);
}

// A-12: the one buffer write is the pager's staged insertion, so a completion is
// one undo entry and one generation bump - and undoing it puts back exactly what
// was typed.
TEST(UiCompleteAction, AnInsertionIsOneUndoEntryAndOneGeneration) {
	action_fixture fixture;
	fixture.directories.file("", "README");
	lesh::leshper::state s = action_fixture::line("cat RE");
	const std::uint64_t before = s.gen.value();
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.gen.value(), before + 1);
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat RE");
}

TEST(UiCompleteAction, AnUnmatchedPrefixChangesNothingAndOpensNothing) {
	action_fixture fixture;
	fixture.directories.file("", "README");
	lesh::leshper::state s = action_fixture::line("cat zz");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat zz");
	EXPECT_FALSE(s.pager.open);
}

TEST(UiCompleteAction, WithNoCompleterWiredUpTabIsAnOrdinaryNothing) {
	action_fixture fixture;
	fixture.reg.host = &fixture.completerless;
	lesh::leshper::state s = action_fixture::line("cat re");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat re");
	EXPECT_FALSE(s.pager.open);
}

TEST(UiCompleteAction, AQuotedCandidateReachesTheBufferEscaped) {
	action_fixture fixture;
	fixture.directories.file("", "my file.txt");
	lesh::leshper::state s = action_fixture::line("cat my");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cat my\\ file.txt ");
}

TEST(UiCompleteAction, ACommandNameCompletesInCommandPosition) {
	action_fixture fixture;
	fixture.names.define(name_domain::builtin, {"readonly"});
	lesh::leshper::state s = action_fixture::line("reado");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "readonly ");
}

TEST(UiCompleteAction, AVariableCompletesAndTheDollarStays) {
	action_fixture fixture;
	fixture.names.define(name_domain::variable, {"HOME"});
	lesh::leshper::state s = action_fixture::line("echo $HO");
	EXPECT_EQ(fixture.tab(s).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "echo $HOME ");
}

namespace {

// The probe an ABI-level assertion needs: an action is the only place an editor
// handle exists (ADR-0008), so the verbs are exercised from inside one.
struct verb_probe {
	std::int32_t complete_status = LESH_OK;
	std::size_t count = 0;
	std::int32_t range_status = LESH_OK;
	std::size_t from = 0;
	std::size_t to = 0;
	std::int32_t candidate_status = LESH_OK;
	std::string first;
	std::uint32_t kind = 99;
	std::int32_t past_the_end_status = LESH_OK;
	// Set to ask before completing, which is how "not yet run" is observed.
	bool ask_before_completing = false;
	std::int32_t early_range_status = LESH_OK;
	std::int32_t early_candidate_status = LESH_OK;
};

std::int32_t probe_action(lesh_editor* editor, const lesh_invocation*, void* self) {
	auto& probe = *static_cast<verb_probe*>(self);
	char buffer[256];
	std::size_t length = 0;
	if (probe.ask_before_completing) {
		std::size_t ignored = 0;
		probe.early_range_status = lesh_completion_range(editor, &ignored, &ignored);
		probe.early_candidate_status =
			lesh_completion_candidate(editor, 0, buffer, sizeof(buffer), &length, nullptr);
	}
	probe.complete_status = lesh_complete(editor, &probe.count);
	probe.range_status = lesh_completion_range(editor, &probe.from, &probe.to);
	probe.candidate_status = lesh_completion_candidate(editor, 0, buffer, sizeof(buffer),
	                                                   &length, &probe.kind);
	if (probe.candidate_status == LESH_OK)
		probe.first.assign(buffer, length);
	probe.past_the_end_status =
		lesh_completion_candidate(editor, probe.count, buffer, sizeof(buffer), &length,
		                          nullptr);
	return LESH_OK;
}

} // namespace

TEST(UiCompleteAbi, TheVerbsAnswerTheCompletersOwnResult) {
	action_fixture fixture;
	fixture.directories.directory("", "build");
	verb_probe probe;
	ASSERT_EQ(lesh_action_register(&fixture.reg, "probe", probe_action, &probe), LESH_OK);
	lesh::leshper::state s = action_fixture::line("cat bu");
	EXPECT_EQ(fixture.loop.invoke(s, "probe", invocation{}).status, LESH_OK);

	EXPECT_EQ(probe.complete_status, LESH_OK);
	EXPECT_EQ(probe.count, 1u);
	EXPECT_EQ(probe.range_status, LESH_OK);
	EXPECT_EQ(probe.from, 4u);
	EXPECT_EQ(probe.to, 6u);
	EXPECT_EQ(probe.candidate_status, LESH_OK);
	// BARE, and the kind is the one `lesh_pager_add` takes.
	EXPECT_EQ(probe.first, "build");
	EXPECT_EQ(probe.kind, LESH_PAGER_DIRECTORY);
	EXPECT_EQ(probe.past_the_end_status, LESH_ERR_NOTFOUND);
}

TEST(UiCompleteAbi, BeforeCompleteHasRunTheReadersSayNotFound) {
	action_fixture fixture;
	verb_probe probe;
	probe.ask_before_completing = true;
	ASSERT_EQ(lesh_action_register(&fixture.reg, "probe", probe_action, &probe), LESH_OK);
	lesh::leshper::state s = action_fixture::line("cat bu");
	EXPECT_EQ(fixture.loop.invoke(s, "probe", invocation{}).status, LESH_OK);
	EXPECT_EQ(probe.early_range_status, LESH_ERR_NOTFOUND);
	EXPECT_EQ(probe.early_candidate_status, LESH_ERR_NOTFOUND);
}

TEST(UiCompleteAbi, WithNoCompleterCompleteSaysNotFound) {
	action_fixture fixture;
	fixture.reg.host = &fixture.completerless;
	verb_probe probe;
	ASSERT_EQ(lesh_action_register(&fixture.reg, "probe", probe_action, &probe), LESH_OK);
	lesh::leshper::state s = action_fixture::line("cat bu");
	EXPECT_EQ(fixture.loop.invoke(s, "probe", invocation{}).status, LESH_OK);
	EXPECT_EQ(probe.complete_status, LESH_ERR_NOTFOUND);
	EXPECT_EQ(probe.count, 0u);
}

TEST(UiCompleteAbi, ARerunDiscardsThePreviousSet) {
	action_fixture fixture;
	fixture.directories.directory("", "build");
	verb_probe probe;
	ASSERT_EQ(lesh_action_register(&fixture.reg, "probe", probe_action, &probe), LESH_OK);
	lesh::leshper::state s = action_fixture::line("cat bu");
	EXPECT_EQ(fixture.loop.invoke(s, "probe", invocation{}).status, LESH_OK);
	// A second dispatch over a line that matches nothing: the previous call's
	// candidates must not survive into it.
	lesh::leshper::state other = action_fixture::line("cat zz");
	EXPECT_EQ(fixture.loop.invoke(other, "probe", invocation{}).status, LESH_OK);
	EXPECT_EQ(probe.count, 0u);
	EXPECT_EQ(probe.candidate_status, LESH_ERR_NOTFOUND);
}

// ---------------------------------------------------------------------------
// The rule the link graph already enforces, said out loud
// ---------------------------------------------------------------------------

TEST(UiCompleteIncludeDiscipline, TheCompleterIncludesNothingFromTheExpander) {
	// THIS TEST IS THE WHOLE GUARD NOW, and #168 Phase B is why it had to be
	// said out loud. While the completer was `src/leshper/complete.cpp` the rule
	// enforced itself: `lesh_leshper` does not link `lesh_runtime`, so an
	// `#include "runtime/expander.h"` would not have LINKED. It is
	// `src/ui/completion.cpp` now, in the one library that links both halves, and
	// the link can no longer say no. §6.9's rule did not change - `$VAR` in a
	// completion prefix is never expanded in v1 - so what changed is that the
	// only thing keeping it is this loop over the include lines.
	const std::filesystem::path source =
		repository_root() / "src" / "ui" / "completion.cpp";
	ASSERT_TRUE(std::filesystem::exists(source))
		<< "the guard could not find completion.cpp from __FILE__ (" << __FILE__ << ")";
	std::ifstream in{source};
	ASSERT_TRUE(in.is_open());

	std::vector<std::string> offences;
	std::string line;
	while (std::getline(in, line)) {
		if (line.rfind("#include", 0) != 0)
			continue;
		if (line.find("runtime/") != std::string::npos)
			offences.push_back(line);
	}
	EXPECT_TRUE(offences.empty())
		<< "completion.cpp must include nothing from src/runtime/ - the expander "
		   "lives there, and §6.9 keeps the completer expander-free. The link graph "
		   "stopped enforcing it when the file moved into lesh_ui (#168 Phase B), so "
		   "this assertion is the only thing left that does. Found: "
		<< (offences.empty() ? std::string{} : offences.front());
}
