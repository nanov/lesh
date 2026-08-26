#include "leshper/abi.h"
#include "leshper/registry.h"
#include "ui/editor_host.h"
#include "ui/shell_knowledge.h"
#include "leshper/state.h"
#include "runtime/shell_state.h"
#include "substrate/arena.h"
#include "syntax/parser.h"
#include "ui/shell_actor.h"
#include "ui/shell_state_knowledge.h"
#include "ui/workers.h"

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
using namespace lesh::ui;

// What the shell knows, on the request token (#135; #130's verb, ADR-0009's
// thread model) - the HOST's half of it (#168).
//
// TWO LAYERS, and each is tested against the layer below rather than against a
// mock of it:
//
//   `shell_state_knowledge` - the adapter, over a REAL `shell_state` with real
//   aliases, real functions and a real `PATH` variable. It is the only thing
//   standing between the highlighter and the shell, and faking it here would
//   leave the one join nobody had checked.
//
//   `shell_actor` - the thing that puts the knowledge on a token (#151). It was
//   the loop, and the token build on the far side dropped the field, which is
//   why `exit` and `bind` painted red in a real shell while every test passed.
//   `shell_writing_flag` turns ADR-0009's rule into an assertion that can fire,
//   with a death test that fires it.
//
// The layers BELOW these - the ABI verb and the highlighter's classes - are
// `ui_command_kind_tests.cpp`, which drives the ABI verb over a fake host.

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
	editor_host host{&shell};

	verb_fixture() {
		EXPECT_EQ(lesh_reactor_register(&reg, "probe", LESH_EVENT_BUFFER_CHANGED,
		                                probe_reactor, &asked),
		          LESH_OK);
		reg.host = &host;
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
// The adapter, over a real shell_state.
// ---------------------------------------------------------------------------

namespace {

struct adapter_fixture {
	lesh::runtime::shell_state shell;
	shell_state_knowledge knowledge{shell};
};

} // namespace

TEST(UiKnowledge, TheAdapterReadsTheRealAliasTable) {
	adapter_fixture fixture;
	EXPECT_EQ(fixture.knowledge.classify("ll"), command_kind::unknown);
	fixture.shell.set_alias("ll", "ls -l");
	EXPECT_EQ(fixture.knowledge.classify("ll"), command_kind::alias);
	EXPECT_TRUE(fixture.shell.unset_alias("ll"));
	EXPECT_EQ(fixture.knowledge.classify("ll"), command_kind::unknown);
}

TEST(UiKnowledge, TheAdapterReadsTheRealFunctionTable) {
	adapter_fixture fixture;
	lesh::buffer_pool pool{BUFFER_POOL_SIZE};
	const lesh::syntax::tree body = lesh::syntax::parse(pool, "true");
	EXPECT_EQ(fixture.knowledge.classify("deploy"), command_kind::unknown);
	fixture.shell.define_function("deploy", body, 0);
	EXPECT_EQ(fixture.knowledge.classify("deploy"), command_kind::function);
	fixture.shell.unset_function("deploy");
	EXPECT_EQ(fixture.knowledge.classify("deploy"), command_kind::unknown);
}

TEST(UiKnowledge, TheAdapterKnowsTheStaticBuiltinTable) {
	// The one table that is not shell state: builtins.h's registry, the same one
	// the executor's command search reads. A highlighter that disagreed with it
	// about what `cd` is would be C-5's bug class wearing a colour.
	adapter_fixture fixture;
	EXPECT_EQ(fixture.knowledge.classify("cd"), command_kind::builtin);
	EXPECT_EQ(fixture.knowledge.classify(":"), command_kind::builtin);
	EXPECT_EQ(fixture.knowledge.classify("["), command_kind::builtin);
	EXPECT_EQ(fixture.knowledge.classify("definitelynotabuiltin"), command_kind::unknown);
}

TEST(UiKnowledge, TheAdapterReadsTheShellsPathAndNotTheProcessEnvironments) {
	// The half of #124 that `getenv` got wrong: the two agree until the shell
	// assigns to PATH, and the line being typed is exactly where that happens.
	const scoped_env_path environment{"/nonexistent-environment"};
	adapter_fixture fixture;
	std::string_view path;
	ASSERT_TRUE(fixture.shell.set("PATH", "/shell/only"));
	EXPECT_TRUE(fixture.knowledge.path(path));
	EXPECT_EQ(path, "/shell/only");
}

TEST(UiKnowledge, TheAdapterAnswersAnUnsetPathAsUnset) {
	adapter_fixture fixture;
	ASSERT_TRUE(fixture.shell.set("PATH", "/somewhere"));
	ASSERT_TRUE(fixture.shell.unset("PATH"));
	std::string_view path;
	EXPECT_FALSE(fixture.knowledge.path(path));
}

TEST(UiKnowledge, TheAdapterAnswersTheVerbEndToEnd) {
	// Adapter and verb together, over a real state - the join a fake on either
	// side would have left unchecked.
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("mytool"));
	verb_fixture fixture;
	lesh::runtime::shell_state shell;
	shell.set_alias("ll", "ls -l");
	ASSERT_TRUE(shell.set("PATH", scratch.dir()));
	const shell_state_knowledge knowledge{shell};
	editor_host host{&knowledge};
	fixture.reg.host = &host;
	EXPECT_EQ(fixture.kind_of("ll"), LESH_COMMAND_ALIAS);
	EXPECT_EQ(fixture.kind_of("cd"), LESH_COMMAND_BUILTIN);
	EXPECT_EQ(fixture.kind_of("mytool"), LESH_COMMAND_EXTERNAL);
	EXPECT_EQ(fixture.kind_of("nosuchtool"), LESH_COMMAND_UNKNOWN);
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
	EXPECT_EQ(asking.host, nullptr);
	actor.post_highlight("probe", &probe_reactor, &asked, std::move(asking));
	ASSERT_TRUE(actor.serve_one());
	std::vector<shell_message> inbox;
	EXPECT_EQ(actor.replies().drain(inbox), 1u);
	actor.replies().recycle(inbox);
}

} // namespace

TEST(UiKnowledge, TheActorStampsItsShellsTablesOnTheTokenItServes) {
	// #151's defect, at the seam it lived in. The shell-thread reactor's token was
	// built from a snapshot that carried the pointer, and the build copied every
	// field except that one - so `lesh_request_command_kind` saw a null adapter,
	// fell back to `environment_knowledge`, and every name that is ONLY a builtin,
	// a function or an alias resolved unknown. `cd` passed anyway, because macOS
	// ships `/usr/bin/cd`; `exit` is the case with no binary behind it.
	idle_shell nothing;
	fake_knowledge shell;
	shell.define("exit", command_kind::builtin);
	const editor_host host{&shell};
	shell_actor actor{nothing, &host};

	probe asked;
	asked.ask = {"exit"};
	serve_one_highlight(actor, asked);

	ASSERT_EQ(asked.kinds.size(), 1u);
	EXPECT_EQ(asked.kinds.front(), LESH_COMMAND_BUILTIN);
	EXPECT_EQ(shell.asked, (std::vector<std::string>{"exit"}));
}

TEST(UiKnowledge, AnActorWithNoTablesLeavesTheTokenOnTheEnvironmentFallback) {
	// The null the constructor still accepts, and what it means: no shell
	// attached, empty tables, `getenv("PATH")` - a leshper embedded in something
	// that is not this shell.
	lesh::testing::temp_path scratch;
	make_executable(scratch.file("tool"));
	const scoped_env_path path{scratch.dir().c_str()};

	idle_shell nothing;
	// The environment fallback is a HOST now (#168 Phase B): what used to be
	// leshper's own `environment_knowledge` behind the ABI verb is one more thing
	// on the far side of the one door, because the `$PATH` sweep it needs is
	// filesystem knowledge.
	const environment_knowledge environment;
	const editor_host host{&environment};
	shell_actor actor{nothing, &host};

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

TEST(UiKnowledge, TheWritingFlagIsDownExceptInsideTheTwoWriters) {
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
	const editor_host host{&knowledge};
	shell_actor actor{shell, &host, &writing};

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
TEST(UiKnowledgeDeathTest, AReadWhileTheShellIsWritingTripsTheAssertion) {
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
	const editor_host host{&knowledge};
	shell_actor actor{wrong, &host, &writing};

	lesh::leshper::state s;
	actor.post_execute("anything", s.gen);
	EXPECT_DEATH((void)actor.serve_one(), "assertion failed");
}
#endif
