#include "leshper/abi.h"
#include "leshper/registry.h"
#include "ui/editor_host.h"
#include "ui/shell_knowledge.h"
#include "leshper/state.h"
#include "runtime/shell_state.h"
#include "substrate/arena.h"
#include "syntax/parser.h"
#include "ui/loop.h"
#include "ui/shell_side.h"
#include "ui/shell_state_knowledge.h"
#include "ui/reactor_call.h"

#include "temp_path.h"
#include "ui_fakes.h"

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
using lesh::testing::fake_knowledge;
using lesh::testing::scoped_env_path;

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
//   `event_loop` - the thing that puts the knowledge on a token. It was the
//   loop, then `shell_actor` (#151), and it is the loop again (#201) - the
//   difference being that the pointer now arrives WITH THE SHELL at
//   `attach_shell` instead of being copied across a thread, which is the copy
//   that dropped the field and made `exit` and `bind` paint red in a real shell
//   while every test passed. `shell_writing_flag` turns ADR-0009's rule into an
//   assertion that can fire, with a death test that fires it.
//
// The layers BELOW these - the ABI verb and the highlighter's classes - are
// `ui_command_kind_tests.cpp`, which drives the ABI verb over a fake host.

namespace {

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
// The loop's stamp (#151, #201).
// ---------------------------------------------------------------------------

namespace {

// A `shell_side` with nothing behind it. These tests are about the token the
// loop MINTS, not about anything it runs.
class idle_shell final : public shell_side {
public:
	std::int32_t execute(std::string_view) override { return 0; }
	std::int32_t port_call(std::string_view) override { return 0; }
};

// A shell that breaks ADR-0009 deliberately: it reads the tables from inside
// `execute`, which is the one moment the rule forbids. Nothing in the tree does
// this; it exists so that the tripwire has something to trip on. It is no less
// forbidden for being on one thread - what the rule is about is a table being
// read mid-rewrite, not which thread is doing the reading.
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

// A loop over a pipe with the shell attached, its shell-state reactor named
// `probe`, and one keystroke put through it.
//
// THE SEAM UNDER TEST IS THE MINT (#201). `shell_actor::serve_one` was what this
// used to drive; the reactor runs inside `notify_reactors` now, so the way to
// reach it is a buffer change - which is also the only way a real session ever
// reaches it.
void run_one_shell_reactor(shell_side& shell, const lesh::leshper::host* host, probe& asked,
                           shell_writing_flag* writing = nullptr) {
	lesh::testing::fake_tty tty;
	registry reg;
	ASSERT_EQ(lesh_reactor_register(&reg, "probe", LESH_EVENT_BUFFER_CHANGED,
	                                &probe_reactor, &asked),
	          LESH_OK);

	loop_options options;
	options.manage_terminal = false;
	options.shell_thread_reactor = "probe";
	event_loop loop{tty.fds(), options};
	loop.attach_registry(reg);
	loop.attach_shell(shell, host, writing);
	// WHAT `snapshot_of` LEAVES NULL AND THE LOOP FILLS IN. The pointer is not
	// something the loop knows about the shell; it arrived with the shell, which
	// is why the assertion is about where it came from.
	EXPECT_EQ(snapshot_of(lesh::leshper::state{}, LESH_EVENT_BUFFER_CHANGED).host, nullptr);
	EXPECT_EQ(loop.shell_host(), host);

	loop.enter_read();
	tty.type("x");
	loop.turn(50);
}

} // namespace

TEST(UiKnowledge, TheLoopStampsItsShellsTablesOnTheTokenItMints) {
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

	probe asked;
	asked.ask = {"exit"};
	run_one_shell_reactor(nothing, &host, asked);

	ASSERT_EQ(asked.kinds.size(), 1u);
	EXPECT_EQ(asked.kinds.front(), LESH_COMMAND_BUILTIN);
	EXPECT_EQ(shell.asked, (std::vector<std::string>{"exit"}));
}

TEST(UiKnowledge, AShellWithNoTablesLeavesTheTokenOnTheEnvironmentFallback) {
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

	probe asked;
	asked.ask = {"tool", "nosuchtool"};
	run_one_shell_reactor(nothing, &host, asked);

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

	// THE LOOP RAISES IT NOW (#201), because the loop is what makes the two calls.
	// It was `shell_actor::serve_execute` and `serve_port_call`; the scope moved
	// with the call site and the flag did not move at all.
	lesh::testing::fake_tty tty;
	loop_options options;
	options.manage_terminal = false;
	event_loop loop{tty.fds(), options};
	loop.attach_shell(shell, &host, &writing);
	loop.enter_read();

	EXPECT_FALSE(writing.writing());
	// An empty line, which is what a cancel is - the shortest way to reach
	// `execute` with no keystroke in the way.
	loop.finish_cancelled_line();
	EXPECT_TRUE(up_during_execute);
	EXPECT_FALSE(writing.writing()) << "the scope puts it down again";

	(void)loop.call_port("anything");
	EXPECT_TRUE(up_during_port_call);
	EXPECT_FALSE(writing.writing());

	// And a read is legal now, which is the state every reader in the tree runs
	// in: anywhere in a turn that is not inside one of the two calls.
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

	lesh::testing::fake_tty tty;
	loop_options options;
	options.manage_terminal = false;
	event_loop loop{tty.fds(), options};
	loop.attach_shell(wrong, &host, &writing);
	loop.enter_read();

	EXPECT_DEATH(loop.finish_cancelled_line(), "assertion failed");
}
#endif
