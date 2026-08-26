#include "leshper/abi.h"
#include "leshper/registry.h"
#include "leshper/shell_actor.h"
#include "leshper/shell_knowledge.h"
#include "leshper/shell_state_knowledge.h"
#include "leshper/state.h"
#include "leshper/workers.h"
#include "runtime/shell_state.h"
#include "substrate/arena.h"
#include "syntax/parser.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lesh::leshper;

// What the shell knows, on the request token (#135; #130's verb, ADR-0009's
// thread model).
//
// THREE LAYERS, and each is tested against the layer below rather than against a
// mock of it:
//
//   `lesh_request_command_kind` - the ABI verb. Driven through a probe reactor,
//   because a token cannot be minted any other way: the loop mints it, hands it
//   to a reactor, and it dies when the compute returns (ADR-0008). A test that
//   built one by hand would be testing a different object.
//
//   `shell_state_knowledge` - the wiring-site adapter, over a REAL `shell_state`
//   with real aliases, real functions and a real `PATH` variable. It is the only
//   thing standing between the highlighter and the shell, and faking it here
//   would leave the one join nobody had checked.
//
//   The highlighter's classes - `command.alias`, `command.function`,
//   `command.builtin`, `command.path`, `command.unknown` - read back through
//   `lesh_style_name`, so an assertion says what the span MEANS rather than
//   which integer it interned as (F-21; the theme is the other half and is not
//   this ticket's).
//
// There is no copy-on-write test and no race test in this file, and their
// absence is the ticket's content rather than an omission: ADR-0009 gave shell
// state exactly one owner thread, so #130's definitions version was deleted and
// there is no concurrency here to test.
//
// TWO MORE LAYERS ARRIVED WITH #151, at the bottom of the file. `shell_actor`
// is now the thing that puts the knowledge on a token - it was the loop, and the
// token build on the far side dropped the field, which is why `exit` and `bind`
// painted red in a real shell while every test here passed. And
// `shell_writing_flag` turns ADR-0009's rule into an assertion that can fire,
// with a death test that fires it.

namespace {

// A `shell_knowledge` that is a map, which is what a test has.
//
// It counts its calls, because the memo's whole claim - a name repeated on one
// line is walked once - is a claim about how often this gets asked.
class fake_knowledge final : public shell_knowledge {
public:
	void define(std::string name, command_kind kind) {
		_names.insert_or_assign(std::move(name), kind);
	}

	void set_path(std::string value) {
		_path = std::move(value);
		_has_path = true;
	}
	void unset_path() noexcept { _has_path = false; }

	[[nodiscard]] command_kind classify(std::string_view name) const override {
		asked.push_back(std::string{name});
		const auto found = _names.find(name);
		return found == _names.end() ? command_kind::unknown : found->second;
	}

	[[nodiscard]] bool path(std::string_view& out) const override {
		++path_reads;
		if (!_has_path)
			return false;
		out = _path;
		return true;
	}

	// Every name this was asked about, in order.
	mutable std::vector<std::string> asked;
	mutable int path_reads = 0;

private:
	std::map<std::string, command_kind, std::less<>> _names;
	std::string _path;
	bool _has_path = false;
};

// The reactor a test uses to reach the token.
//
// It ignores the buffer: the verb takes bytes the caller supplies, and driving
// it through a list is what lets one request ask the same name three times -
// which is the only way to observe a per-request memo at all.
struct probe {
	std::vector<std::string> ask;
	std::vector<std::uint32_t> kinds;
	std::vector<std::int32_t> statuses;
	// Set to exercise the null-argument refusals, which a well-behaved caller
	// never reaches.
	bool ask_with_null_out = false;
	std::int32_t null_out_status = LESH_OK;
	std::int32_t null_name_status = LESH_OK;
};

std::int32_t probe_reactor(lesh_request* request, void* userdata) {
	probe* self = static_cast<probe*>(userdata);
	self->kinds.clear();
	self->statuses.clear();
	for (const std::string& name : self->ask) {
		// A value no kind uses, so "the call left it alone" is distinguishable
		// from "the call answered unknown".
		std::uint32_t kind = 0xDEADu;
		self->statuses.push_back(
			lesh_request_command_kind(request, name.data(), name.size(), &kind));
		self->kinds.push_back(kind);
	}
	if (self->ask_with_null_out) {
		self->null_out_status = lesh_request_command_kind(request, "ls", 2, nullptr);
		std::uint32_t ignored = 0;
		self->null_name_status = lesh_request_command_kind(request, nullptr, 2, &ignored);
	}
	return LESH_OK;
}

struct verb_fixture {
	registry reg;
	loop_harness loop{reg};
	probe asked;
	fake_knowledge shell;

	verb_fixture() {
		EXPECT_EQ(lesh_reactor_register(&reg, "probe", LESH_EVENT_BUFFER_CHANGED,
		                                probe_reactor, &asked),
		          LESH_OK);
		loop.set_shell_knowledge(&shell);
	}

	// One request, asking about every name in order. Returns what came back.
	std::vector<std::uint32_t> kinds_of(std::vector<std::string> names) {
		asked.ask = std::move(names);
		lesh::leshper::state s;
		s.gen.bump();
		const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
		EXPECT_EQ(batches.size(), 1u);
		return asked.kinds;
	}

	std::uint32_t kind_of(std::string name) {
		const std::vector<std::uint32_t> all = kinds_of({std::move(name)});
		return all.empty() ? 0xDEADu : all.front();
	}
};

// An executable regular file, so that "found on PATH" is a property of the test
// and not of whatever this machine happens to have installed.
void make_executable(const std::string& path) {
	std::FILE* f = std::fopen(path.c_str(), "w");
	ASSERT_NE(f, nullptr);
	std::fputs("#!/bin/sh\n", f);
	std::fclose(f);
	ASSERT_EQ(::chmod(path.c_str(), 0755), 0);
}

void make_plain_file(const std::string& path) {
	std::FILE* f = std::fopen(path.c_str(), "w");
	ASSERT_NE(f, nullptr);
	std::fclose(f);
	ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
}

// The process environment's PATH, restored on the way out. Only the tests for
// the no-shell-attached fallback touch it; every other test here goes through a
// `shell_knowledge` and is indifferent to it.
class scoped_env_path {
public:
	explicit scoped_env_path(const char* value) {
		if (const char* old = ::getenv("PATH")) {
			_had = true;
			_old = old;
		}
		::setenv("PATH", value, 1);
	}
	~scoped_env_path() {
		if (_had)
			::setenv("PATH", _old.c_str(), 1);
		else
			::unsetenv("PATH");
	}

	scoped_env_path(const scoped_env_path&) = delete;
	scoped_env_path& operator=(const scoped_env_path&) = delete;

private:
	bool _had = false;
	std::string _old;
};

} // namespace

// ---------------------------------------------------------------------------
// The verb: resolution order.
// ---------------------------------------------------------------------------

TEST(LeshperKnowledge, AnAliasOutranksEverythingElseOfTheSameName) {
	// POSIX 2.3.1 substitutes the alias in the lexer, before the command search
	// in 2.9.1.1 ever runs, so a name that is all four things is an alias.
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("ll"));
	verb_fixture fixture;
	fixture.shell.define("ll", command_kind::alias);
	fixture.shell.set_path(scratch.dir());
	EXPECT_EQ(fixture.kind_of("ll"), LESH_COMMAND_ALIAS);
}

TEST(LeshperKnowledge, AFunctionOutranksABuiltinAndPath) {
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("deploy"));
	verb_fixture fixture;
	fixture.shell.define("deploy", command_kind::function);
	fixture.shell.set_path(scratch.dir());
	EXPECT_EQ(fixture.kind_of("deploy"), LESH_COMMAND_FUNCTION);
}

TEST(LeshperKnowledge, ABuiltinOutranksPath) {
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("echo"));
	verb_fixture fixture;
	fixture.shell.define("echo", command_kind::builtin);
	fixture.shell.set_path(scratch.dir());
	EXPECT_EQ(fixture.kind_of("echo"), LESH_COMMAND_BUILTIN);
}

TEST(LeshperKnowledge, ANameNoTableHoldsIsLookedUpOnPath) {
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("mytool"));
	verb_fixture fixture;
	fixture.shell.set_path(scratch.dir());
	EXPECT_EQ(fixture.kind_of("mytool"), LESH_COMMAND_EXTERNAL);
	EXPECT_EQ(fixture.kind_of("nosuchtool"), LESH_COMMAND_UNKNOWN);
}

TEST(LeshperKnowledge, TheWalkTakesTheFirstDirectoryThatHasIt) {
	lesh::testing::temp_path first;
	lesh::testing::temp_path second;
	make_plain_file(first.file("tool"));  // present, but not executable
	make_executable(second.file("tool"));
	verb_fixture fixture;
	fixture.shell.set_path(first.dir() + ":" + second.dir());
	// The first directory holds a file of that name that exec would refuse, so
	// the walk must carry on rather than stop at the name.
	EXPECT_EQ(fixture.kind_of("tool"), LESH_COMMAND_EXTERNAL);
}

TEST(LeshperKnowledge, ANonExecutableFileIsNotACommand) {
	lesh::testing::temp_path scratch;
	make_plain_file(scratch.file("notes"));
	verb_fixture fixture;
	fixture.shell.set_path(scratch.dir());
	EXPECT_EQ(fixture.kind_of("notes"), LESH_COMMAND_UNKNOWN);
}

TEST(LeshperKnowledge, ADirectoryIsNotACommand) {
	// access(X_OK) says yes for a directory; S_ISREG is what makes the answer
	// mean "a thing exec would run" (#124's finding, moved with the walk).
	lesh::testing::temp_path scratch;
	ASSERT_EQ(::mkdir(scratch.file("sub").c_str(), 0755), 0);
	verb_fixture fixture;
	fixture.shell.set_path(scratch.dir());
	EXPECT_EQ(fixture.kind_of("sub"), LESH_COMMAND_UNKNOWN);
}

TEST(LeshperKnowledge, AnUnsetPathResolvesNothing) {
	verb_fixture fixture;
	fixture.shell.unset_path();
	EXPECT_EQ(fixture.kind_of("sh"), LESH_COMMAND_UNKNOWN);
}

TEST(LeshperKnowledge, ANameWithASlashGoesStraightToTheFilesystem) {
	// POSIX 2.9.1.1: a command name containing a slash is a pathname, and no
	// table is consulted for it - which is also why the tables are never asked.
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("tool"));
	verb_fixture fixture;
	fixture.shell.define(scratch.file("tool"), command_kind::alias);
	fixture.shell.unset_path();
	EXPECT_EQ(fixture.kind_of(scratch.file("tool")), LESH_COMMAND_EXTERNAL);
	EXPECT_TRUE(fixture.shell.asked.empty());
	EXPECT_EQ(fixture.kind_of(scratch.file("missing")), LESH_COMMAND_UNKNOWN);
}

TEST(LeshperKnowledge, AnAliasIsResolvedOneLevelAndTheBodyIsNeverAsked) {
	// `alias ll='ls -l'` makes `ll` an alias, and the answer stops there. The
	// body is not re-resolved to discover what `ls` is, and is never expanded:
	// #95's rule is that a span names bytes the user typed, and an expanded
	// alias's tokens live in a text region no position in the line can name.
	verb_fixture fixture;
	fixture.shell.define("ll", command_kind::alias);
	fixture.shell.define("ls", command_kind::external);
	EXPECT_EQ(fixture.kind_of("ll"), LESH_COMMAND_ALIAS);
	EXPECT_EQ(fixture.shell.asked, (std::vector<std::string>{"ll"}));
}

// ---------------------------------------------------------------------------
// The verb: boundaries.
// ---------------------------------------------------------------------------

TEST(LeshperKnowledge, AnEmptyNameIsUnknownAndNotAnError) {
	verb_fixture fixture;
	fixture.shell.define("", command_kind::alias);
	EXPECT_EQ(fixture.kind_of(""), LESH_COMMAND_UNKNOWN);
	EXPECT_EQ(fixture.asked.statuses.front(), LESH_OK);
	EXPECT_TRUE(fixture.shell.asked.empty());
}

TEST(LeshperKnowledge, ANulInsideTheNameIsNotTruncated) {
	// The buffer is bytes and may hold a NUL. Truncating at it would hand stat(2)
	// a candidate that names a DIFFERENT file, and answer confidently about it.
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("tool"));
	verb_fixture fixture;
	fixture.shell.set_path(scratch.dir());
	EXPECT_EQ(fixture.kind_of(std::string("tool\0x", 6)), LESH_COMMAND_UNKNOWN);
	EXPECT_EQ(fixture.kind_of("tool"), LESH_COMMAND_EXTERNAL);
}

TEST(LeshperKnowledge, ANullArgumentIsRefusedAndAnswersNothing) {
	verb_fixture fixture;
	fixture.asked.ask_with_null_out = true;
	fixture.kinds_of({});
	EXPECT_EQ(fixture.asked.null_out_status, LESH_ERR_INVAL);
	EXPECT_EQ(fixture.asked.null_name_status, LESH_ERR_INVAL);
}

// ---------------------------------------------------------------------------
// The memo.
// ---------------------------------------------------------------------------

TEST(LeshperKnowledge, ANameRepeatedInOneRequestIsResolvedOnce) {
	// The claim the memo makes, and the only way to see it: one request, the same
	// name three times, one trip to the tables and one $PATH walk.
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("tool"));
	verb_fixture fixture;
	fixture.shell.set_path(scratch.dir());
	const std::vector<std::uint32_t> kinds =
		fixture.kinds_of({"tool", "tool", "tool"});
	EXPECT_EQ(kinds, (std::vector<std::uint32_t>{LESH_COMMAND_EXTERNAL,
	                                             LESH_COMMAND_EXTERNAL,
	                                             LESH_COMMAND_EXTERNAL}));
	EXPECT_EQ(fixture.shell.asked.size(), 1u);
	EXPECT_EQ(fixture.shell.path_reads, 1);
}

TEST(LeshperKnowledge, TheMemoDiesWithItsRequest) {
	// Per request, not per reactor and not per process: the next keystroke's
	// highlight must see an alias defined between the two.
	verb_fixture fixture;
	EXPECT_EQ(fixture.kind_of("g"), LESH_COMMAND_UNKNOWN);
	fixture.shell.define("g", command_kind::alias);
	EXPECT_EQ(fixture.kind_of("g"), LESH_COMMAND_ALIAS);
	EXPECT_EQ(fixture.shell.asked.size(), 2u);
}

TEST(LeshperKnowledge, MoreDistinctNamesThanTheMemoHoldsStillAnswerCorrectly) {
	// The memo is fixed and inline, so it can fill up. Overflow must cost a
	// second walk and never a wrong answer.
	verb_fixture fixture;
	std::vector<std::string> names;
	for (int i = 0; i < 200; ++i) {
		std::string name = "cmd" + std::to_string(i);
		if (i % 2 == 0)
			fixture.shell.define(name, command_kind::function);
		names.push_back(std::move(name));
	}
	const std::vector<std::uint32_t> kinds = fixture.kinds_of(names);
	ASSERT_EQ(kinds.size(), names.size());
	for (std::size_t i = 0; i < kinds.size(); ++i)
		EXPECT_EQ(kinds[i], i % 2 == 0 ? LESH_COMMAND_FUNCTION : LESH_COMMAND_UNKNOWN)
			<< "at " << names[i];
}

TEST(LeshperKnowledge, ANameTooLongToMemoizeIsStillAnsweredCorrectly) {
	const std::string name(command_kind_memo::name_capacity + 8, 'x');
	verb_fixture fixture;
	fixture.shell.define(name, command_kind::function);
	const std::vector<std::uint32_t> kinds = fixture.kinds_of({name, name});
	EXPECT_EQ(kinds, (std::vector<std::uint32_t>{LESH_COMMAND_FUNCTION,
	                                             LESH_COMMAND_FUNCTION}));
	// Asked twice, because it could not be remembered - the fallback is cost, not
	// a wrong answer.
	EXPECT_EQ(fixture.shell.asked.size(), 2u);
}

// ---------------------------------------------------------------------------
// No shell attached.
// ---------------------------------------------------------------------------

TEST(LeshperKnowledge, WithNoShellAttachedThePathIsTheProcessEnvironments) {
	// The documented fallback, and what leshper embedded in something that is not
	// this shell would see: empty tables, `getenv("PATH")`. It is also exactly
	// what the highlighter did before this door existed (#124).
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("tool"));
	const scoped_env_path path{scratch.dir().c_str()};
	verb_fixture fixture;
	fixture.loop.set_shell_knowledge(nullptr);
	EXPECT_EQ(fixture.kind_of("tool"), LESH_COMMAND_EXTERNAL);
	EXPECT_EQ(fixture.kind_of("nosuchtool"), LESH_COMMAND_UNKNOWN);
	EXPECT_TRUE(fixture.shell.asked.empty());
}

TEST(LeshperKnowledge, TheEnvironmentFallbackKnowsNoTables) {
	const environment_knowledge environment;
	EXPECT_EQ(environment.classify("cd"), command_kind::unknown);
	std::string_view path;
	const scoped_env_path set{"/nonexistent"};
	EXPECT_TRUE(environment.path(path));
	EXPECT_EQ(path, "/nonexistent");
}

// ---------------------------------------------------------------------------
// The adapter, over a real shell_state.
// ---------------------------------------------------------------------------

namespace {

struct adapter_fixture {
	lesh::runtime::shell_state shell;
	shell_state_knowledge knowledge{shell};
};

} // namespace

TEST(LeshperKnowledge, TheAdapterReadsTheRealAliasTable) {
	adapter_fixture fixture;
	EXPECT_EQ(fixture.knowledge.classify("ll"), command_kind::unknown);
	fixture.shell.set_alias("ll", "ls -l");
	EXPECT_EQ(fixture.knowledge.classify("ll"), command_kind::alias);
	EXPECT_TRUE(fixture.shell.unset_alias("ll"));
	EXPECT_EQ(fixture.knowledge.classify("ll"), command_kind::unknown);
}

TEST(LeshperKnowledge, TheAdapterReadsTheRealFunctionTable) {
	adapter_fixture fixture;
	lesh::buffer_pool pool{BUFFER_POOL_SIZE};
	const lesh::syntax::tree body = lesh::syntax::parse(pool, "true");
	EXPECT_EQ(fixture.knowledge.classify("deploy"), command_kind::unknown);
	fixture.shell.define_function("deploy", body, 0);
	EXPECT_EQ(fixture.knowledge.classify("deploy"), command_kind::function);
	fixture.shell.unset_function("deploy");
	EXPECT_EQ(fixture.knowledge.classify("deploy"), command_kind::unknown);
}

TEST(LeshperKnowledge, TheAdapterKnowsTheStaticBuiltinTable) {
	// The one table that is not shell state: builtins.h's registry, the same one
	// the executor's command search reads. A highlighter that disagreed with it
	// about what `cd` is would be C-5's bug class wearing a colour.
	adapter_fixture fixture;
	EXPECT_EQ(fixture.knowledge.classify("cd"), command_kind::builtin);
	EXPECT_EQ(fixture.knowledge.classify(":"), command_kind::builtin);
	EXPECT_EQ(fixture.knowledge.classify("["), command_kind::builtin);
	EXPECT_EQ(fixture.knowledge.classify("definitelynotabuiltin"), command_kind::unknown);
}

TEST(LeshperKnowledge, TheAdapterReadsTheShellsPathAndNotTheProcessEnvironments) {
	// The half of #124 that `getenv` got wrong: the two agree until the shell
	// assigns to PATH, and the line being typed is exactly where that happens.
	const scoped_env_path environment{"/nonexistent-environment"};
	adapter_fixture fixture;
	std::string_view path;
	ASSERT_TRUE(fixture.shell.set("PATH", "/shell/only"));
	EXPECT_TRUE(fixture.knowledge.path(path));
	EXPECT_EQ(path, "/shell/only");
}

TEST(LeshperKnowledge, TheAdapterAnswersAnUnsetPathAsUnset) {
	adapter_fixture fixture;
	ASSERT_TRUE(fixture.shell.set("PATH", "/somewhere"));
	ASSERT_TRUE(fixture.shell.unset("PATH"));
	std::string_view path;
	EXPECT_FALSE(fixture.knowledge.path(path));
}

TEST(LeshperKnowledge, TheAdapterAnswersTheVerbEndToEnd) {
	// Adapter and verb together, over a real state - the join a fake on either
	// side would have left unchecked.
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("mytool"));
	verb_fixture fixture;
	lesh::runtime::shell_state shell;
	shell.set_alias("ll", "ls -l");
	ASSERT_TRUE(shell.set("PATH", scratch.dir()));
	const shell_state_knowledge knowledge{shell};
	fixture.loop.set_shell_knowledge(&knowledge);
	EXPECT_EQ(fixture.kind_of("ll"), LESH_COMMAND_ALIAS);
	EXPECT_EQ(fixture.kind_of("cd"), LESH_COMMAND_BUILTIN);
	EXPECT_EQ(fixture.kind_of("mytool"), LESH_COMMAND_EXTERNAL);
	EXPECT_EQ(fixture.kind_of("nosuchtool"), LESH_COMMAND_UNKNOWN);
}

// ---------------------------------------------------------------------------
// The highlighter's classes (F-21).
// ---------------------------------------------------------------------------

namespace {

struct paint_fixture {
	owned_highlighter self;
	registry reg;
	loop_harness loop{reg};
	fake_knowledge shell;

	paint_fixture() {
		register_builtin_reactors(reg, self.get());
		loop.set_shell_knowledge(&shell);
	}

	[[nodiscard]] std::string style_name(std::uint32_t id) {
		char out[64] = {};
		std::size_t length = 0;
		if (lesh_style_name(&reg, id, out, sizeof(out), &length) != LESH_OK)
			return "<none>";
		return std::string(out, length);
	}

	[[nodiscard]] bool has(std::string_view line, std::string_view text,
	                       std::string_view style) {
		lesh::leshper::state s;
		s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(),
		                 std::string(line));
		s.gen.bump();
		const std::vector<reactor_batch> batches =
			loop.react(s, LESH_EVENT_BUFFER_CHANGED);
		EXPECT_EQ(batches.size(), 1u);
		if (batches.empty())
			return false;
		for (const decoration_span& one : batches[0].spans)
			if (line.substr(one.start, one.end - one.start) == text
			    && style_name(one.style_id) == style)
				return true;
		return false;
	}
};

} // namespace

TEST(LeshperKnowledge, TheThreeMissingClassesAreInternedSemanticNames) {
	// #124 landed 13 style ids and recorded these three as blocked on this door.
	// They are names, not colours: the theme maps them at render (F-21).
	paint_fixture fixture;
	for (const char* name : {"command.builtin", "command.function", "command.alias"}) {
		std::uint32_t id = LESH_STYLE_NONE;
		EXPECT_EQ(lesh_style_intern(&fixture.reg, name, &id), LESH_OK) << name;
		EXPECT_NE(id, LESH_STYLE_NONE) << name;
	}
}

TEST(LeshperKnowledge, AnAliasPaintsAsCommandAlias) {
	paint_fixture fixture;
	fixture.shell.define("ll", command_kind::alias);
	EXPECT_TRUE(fixture.has("ll -a", "ll", "command.alias"));
}

TEST(LeshperKnowledge, AFunctionPaintsAsCommandFunction) {
	paint_fixture fixture;
	fixture.shell.define("deploy", command_kind::function);
	EXPECT_TRUE(fixture.has("deploy staging", "deploy", "command.function"));
}

TEST(LeshperKnowledge, ABuiltinPaintsAsCommandBuiltin) {
	paint_fixture fixture;
	fixture.shell.define("cd", command_kind::builtin);
	EXPECT_TRUE(fixture.has("cd /tmp", "cd", "command.builtin"));
}

TEST(LeshperKnowledge, ANameOnTheShellsPathPaintsAsCommandPath) {
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("mytool"));
	paint_fixture fixture;
	fixture.shell.set_path(scratch.dir());
	EXPECT_TRUE(fixture.has("mytool x", "mytool", "command.path"));
}

TEST(LeshperKnowledge, WithAPathThatResolvesNothingACommandNameIsUnknown) {
	// The ticket's own case: `PATH=/nonexistent` turns `ls` unknown, and it is
	// the SHELL's PATH that decides, not the process environment's - so this
	// holds however the machine running the test is set up.
	paint_fixture fixture;
	fixture.shell.set_path("/nonexistent");
	EXPECT_TRUE(fixture.has("ls -l", "ls", "command.unknown"));
}

TEST(LeshperKnowledge, ACommandNameInsideASubstitutionIsClassifiedToo) {
	// #104 parses interiors after the top level, so an interior command name is a
	// node like any other and needs no second code path (#124).
	paint_fixture fixture;
	fixture.shell.define("ll", command_kind::alias);
	EXPECT_TRUE(fixture.has("echo $(ll)", "ll", "command.alias"));
}

TEST(LeshperKnowledge, AWordThatIsNotProvablyLiteralIsStillNotClassified) {
	// The rule #124 set and this ticket does not relax: `$cmd` names a command
	// only after expansion, so it gets no command class at all - not even now
	// that the tables can be reached.
	paint_fixture fixture;
	fixture.shell.define("cmd", command_kind::alias);
	EXPECT_FALSE(fixture.has("$cmd a", "$cmd", "command.alias"));
	EXPECT_FALSE(fixture.has("$cmd a", "$cmd", "command.unknown"));
}

TEST(LeshperKnowledge, TheTablesAreNotConsultedForAWordThatIsNotACommandName) {
	// Only the command-name role asks. An argument that happens to be an alias's
	// name is an argument.
	paint_fixture fixture;
	fixture.shell.define("ll", command_kind::alias);
	fixture.shell.set_path("/nonexistent");
	EXPECT_TRUE(fixture.has("echo ll", "echo", "command.unknown"));
	EXPECT_EQ(fixture.shell.asked, (std::vector<std::string>{"echo"}));
}

// ---------------------------------------------------------------------------
// The actor's stamp (#151).
// ---------------------------------------------------------------------------

namespace {

// A `shell_side` with nothing behind it. These tests are about the token the
// actor MINTS, not about anything it runs.
class idle_shell final : public shell_side {
public:
	std::int32_t execute(std::string_view) override { return 0; }
	std::int32_t port_call(std::string_view) override { return 0; }
};

// A shell that breaks ADR-0009 deliberately: it reads the tables from inside
// `execute`, which is the one moment the rule forbids. Nothing in the tree does
// this; it exists so that the tripwire has something to trip on.
class reading_shell final : public shell_side {
public:
	explicit reading_shell(const shell_knowledge& knowledge) noexcept
		: _knowledge(&knowledge) {}

	std::int32_t execute(std::string_view) override {
		std::string_view ignored;
		(void)_knowledge->path(ignored);
		return 0;
	}

	std::int32_t port_call(std::string_view) override { return 0; }

private:
	const shell_knowledge* _knowledge;
};

// Runs one reactor through the actor's `highlight` slot and answers the probe.
void serve_one_highlight(shell_actor& actor, probe& asked) {
	lesh::leshper::state s;
	s.gen.bump();
	request_snapshot asking = snapshot_of(s, LESH_EVENT_BUFFER_CHANGED);
	// THE LOOP DOES NOT FILL THIS IN, and that is the whole change: what the
	// shell knows is not something the loop knows about the shell.
	EXPECT_EQ(asking.knowledge, nullptr);
	actor.post_highlight("probe", &probe_reactor, &asked, std::move(asking));
	ASSERT_TRUE(actor.serve_one());
	std::vector<shell_message> inbox;
	EXPECT_EQ(actor.replies().drain(inbox), 1u);
	actor.replies().recycle(inbox);
}

} // namespace

TEST(LeshperKnowledge, TheActorStampsItsShellsTablesOnTheTokenItServes) {
	// #151's defect, at the seam it lived in. The shell-thread reactor's token was
	// built from a snapshot that carried the pointer, and the build copied every
	// field except that one - so `lesh_request_command_kind` saw a null adapter,
	// fell back to `environment_knowledge`, and every name that is ONLY a builtin,
	// a function or an alias resolved unknown. `cd` passed anyway, because macOS
	// ships `/usr/bin/cd`; `exit` is the case with no binary behind it.
	idle_shell nothing;
	fake_knowledge shell;
	shell.define("exit", command_kind::builtin);
	shell_actor actor{nothing, &shell};

	probe asked;
	asked.ask = {"exit"};
	serve_one_highlight(actor, asked);

	ASSERT_EQ(asked.kinds.size(), 1u);
	EXPECT_EQ(asked.kinds.front(), LESH_COMMAND_BUILTIN);
	EXPECT_EQ(shell.asked, (std::vector<std::string>{"exit"}));
}

TEST(LeshperKnowledge, AnActorWithNoTablesLeavesTheTokenOnTheEnvironmentFallback) {
	// The null the constructor still accepts, and what it means: no shell
	// attached, empty tables, `getenv("PATH")` - a leshper embedded in something
	// that is not this shell.
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("tool"));
	const scoped_env_path path{scratch.dir().c_str()};

	idle_shell nothing;
	shell_actor actor{nothing, nullptr};

	probe asked;
	asked.ask = {"tool", "nosuchtool"};
	serve_one_highlight(actor, asked);

	ASSERT_EQ(asked.kinds.size(), 2u);
	EXPECT_EQ(asked.kinds[0], LESH_COMMAND_EXTERNAL);
	EXPECT_EQ(asked.kinds[1], LESH_COMMAND_UNKNOWN);
}

// ---------------------------------------------------------------------------
// ADR-0009's rule, as a tripwire (#151).
// ---------------------------------------------------------------------------

TEST(LeshperKnowledge, TheWritingFlagIsDownExceptInsideTheTwoWriters) {
	// The positive half, which is the half that runs on every build: the flag the
	// adapter asserts on is raised around `execute` and `port_call` and around
	// nothing else - not around a highlight, which is a READER and shares the
	// shell thread with them.
	lesh::runtime::shell_state state;
	shell_writing_flag writing;
	const shell_state_knowledge knowledge{state, &writing};

	bool up_during_execute = false;
	bool up_during_port_call = false;
	class watching_shell final : public shell_side {
	public:
		watching_shell(const shell_writing_flag& flag, bool& in_execute, bool& in_port)
			: _flag(&flag), _in_execute(&in_execute), _in_port(&in_port) {}

		std::int32_t execute(std::string_view) override {
			*_in_execute = _flag->writing();
			return 0;
		}

		std::int32_t port_call(std::string_view) override {
			*_in_port = _flag->writing();
			return 0;
		}

	private:
		const shell_writing_flag* _flag;
		bool* _in_execute;
		bool* _in_port;
	};

	watching_shell shell{writing, up_during_execute, up_during_port_call};
	shell_actor actor{shell, &knowledge, &writing};

	lesh::leshper::state s;
	EXPECT_FALSE(writing.writing());
	actor.post_execute("anything", s.gen);
	ASSERT_TRUE(actor.serve_one());
	EXPECT_TRUE(up_during_execute);
	EXPECT_FALSE(writing.writing()) << "the scope puts it down again";

	(void)actor.post_port_call("anything", s.gen);
	ASSERT_TRUE(actor.serve_one());
	EXPECT_TRUE(up_during_port_call);
	EXPECT_FALSE(writing.writing());

	// And a read is legal now, which is the state every reader in the tree runs
	// in: between slots on the shell thread, or on the loop thread while the loop
	// is not blocked on one of the two writers.
	std::string_view ignored;
	(void)knowledge.path(ignored);
}

#ifdef LESH_ENABLE_ASSERTS
TEST(LeshperKnowledgeDeathTest, AReadWhileTheShellIsWritingTripsTheAssertion) {
	// The negative half, and the reason #151 asked for a flag rather than a
	// comment: a reader that reaches the adapter while `execute` is running dies
	// where it did the wrong thing, instead of returning a `string_view` into a
	// table that is being rewritten and being found out later, somewhere else.
	//
	// GUARDED ON THE ASSERT BUILD, because `LESH_ASSERT` compiles out in release
	// and a death test for a statement that is not there would fail honestly.
	::testing::GTEST_FLAG(death_test_style) = "threadsafe";
	lesh::runtime::shell_state state;
	shell_writing_flag writing;
	const shell_state_knowledge knowledge{state, &writing};
	reading_shell wrong{knowledge};
	shell_actor actor{wrong, &knowledge, &writing};

	lesh::leshper::state s;
	actor.post_execute("anything", s.gen);
	EXPECT_DEATH((void)actor.serve_one(), "assertion failed");
}
#endif
