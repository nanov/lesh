// The prompt engine's runtime half (#157, spec §6.10).
//
// WHAT IS NOT HERE, AND WHY. The composer's omission rules, the default table's
// exact bytes and the SGR round trip are asserted by the COMPILER, in prompt.h's
// `selftest` namespace - a failure there is a build failure, which is the right
// outcome for "the prompt stopped omitting". What is left for a running test is
// everything a constant expression cannot see: how many times a module was
// INVOKED, which is the whole of recalculation-by-cause, and everything that
// crosses the C ABI.

#include "leshper/abi.h"
#include "leshper/prompt.h"
#include "leshper/registry.h"
#include "leshper/git_head.h"
// The runtime's half of the seam (#157): `prompt_console` is declared there and
// installed on `shell_state`, and this file is one of the places both halves are
// linked - the same standing `leshper_keymap_tests.cpp` has for `bind`.
#include "runtime/builtins.h"
#include "runtime/shell_state.h"
#include "temp_path.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

namespace {

using lesh::leshper::prompt::element_status;
using lesh::leshper::prompt::engine;
using lesh::leshper::prompt::surface_id;
namespace prompt = lesh::leshper::prompt;

// One element, run on its own. What the module tests want and nothing more.
struct rendered {
	std::string bytes;
	element_status status = element_status::omitted;
	std::uint64_t wake = 0;
};

rendered run_element(prompt::element_fn fn, const prompt::state& facts,
                     const void* data = nullptr) {
	prompt::sink out;
	const element_status answered = prompt::status_of(fn(facts, out, data));
	return rendered{std::string{out.bytes()}, answered, out.wake()};
}

// A session in `~/src`, no repo, last command succeeded.
prompt::state quiet() {
	prompt::state facts;
	facts.pwd = "/home/u/src";
	facts.home = "/home/u";
	return facts;
}

bool fake_getvar(const void*, std::string_view name, std::string_view& out) {
	if (name == "USER") {
		out = "dana";
		return true;
	}
	// A variable whose NAME contains a colon, which is a name only `\:` can
	// spell in a template - the escape's whole point, and unreachable without it.
	if (name == "A:B") {
		out = "escaped";
		return true;
	}
	if (name == "EMPTY") {
		out = std::string_view{};
		return true;
	}
	return false;
}

// A module that counts its invocations and says so, so that "was this
// re-invoked" is answerable from the bytes as well as from the counter.
struct counter {
	int calls = 0;
	std::string label;
	std::uint64_t wake = 0;
	bool constant = false;   // when set, the bytes do not change between calls
};

int counting_module(const prompt::state&, prompt::sink& out, const void* data) {
	auto* which = static_cast<counter*>(prompt::userdata_of(data));
	++which->calls;
	out.append(which->label);
	if (!which->constant)
		out.append(std::to_string(which->calls));
	if (which->wake != 0)
		out.wake_in(which->wake);
	return prompt::code(element_status::ready);
}

// What a module registered across the C ABI saw, and what it did with the four
// verbs it has.
struct abi_probe {
	int calls = 0;
	std::string arg;
	std::uint64_t tick = 0;
	std::int32_t last_status = 0;
	std::string variable;
	std::int32_t variable_result = LESH_OK;
};

std::int32_t abi_module(lesh_prompt_context* context, void* userdata) {
	auto* probe = static_cast<abi_probe*>(userdata);
	++probe->calls;

	char buffer[64];
	std::size_t length = 0;
	if (lesh_prompt_arg(context, buffer, sizeof buffer, &length) == LESH_OK)
		probe->arg.assign(buffer, length);

	lesh_prompt_tick(context, &probe->tick);
	lesh_prompt_last_status(context, &probe->last_status);

	probe->variable_result =
		lesh_prompt_variable(context, "USER", buffer, sizeof buffer, &length);
	if (probe->variable_result == LESH_OK)
		probe->variable.assign(buffer, length);

	lesh_prompt_wake_in(context, 7);

	const std::string bytes = "<" + probe->arg + ">";
	lesh_prompt_write(context, bytes.data(), bytes.size());
	return LESH_PROMPT_READY;
}

// ---------------------------------------------------------------------------
// The built-in modules
// ---------------------------------------------------------------------------

TEST(LeshperPromptModules, PathContractsHomeByComponent) {
	EXPECT_EQ(run_element(&prompt::module_path, quiet()).bytes, "~/src");

	prompt::state facts = quiet();
	facts.pwd = "/home/u";
	EXPECT_EQ(run_element(&prompt::module_path, facts).bytes, "~");

	// A sibling whose name merely starts with the home directory's is not under
	// it. The cheap prefix test would have rendered `~name/x`.
	facts.pwd = "/home/username/x";
	EXPECT_EQ(run_element(&prompt::module_path, facts).bytes, "/home/username/x");

	facts.home = std::string_view{};
	facts.pwd = "/home/u/src";
	EXPECT_EQ(run_element(&prompt::module_path, facts).bytes, "/home/u/src");
}

TEST(LeshperPromptModules, PathOmitsWithoutAPwd) {
	prompt::state facts;
	EXPECT_EQ(run_element(&prompt::module_path, facts).status, element_status::omitted);
	EXPECT_TRUE(run_element(&prompt::module_path, facts).bytes.empty());
}

TEST(LeshperPromptModules, StatusOmitsOnSuccess) {
	EXPECT_EQ(run_element(&prompt::module_status, quiet()).status, element_status::omitted);

	prompt::state facts = quiet();
	facts.status = 127;
	const rendered got = run_element(&prompt::module_status, facts);
	EXPECT_EQ(got.status, element_status::ready);
	EXPECT_EQ(got.bytes, "127");

	facts.status = -6;
	EXPECT_EQ(run_element(&prompt::module_status, facts).bytes, "-6");
}

TEST(LeshperPromptModules, JobsOmitsWhenThereAreNone) {
	EXPECT_EQ(run_element(&prompt::module_jobs, quiet()).status, element_status::omitted);

	prompt::state facts = quiet();
	facts.jobs = 12;
	EXPECT_EQ(run_element(&prompt::module_jobs, facts).bytes, "12");
}

TEST(LeshperPromptModules, ModeIsWhateverTheKeymapDeclared) {
	EXPECT_EQ(run_element(&prompt::module_mode, quiet()).status, element_status::omitted);

	prompt::state facts = quiet();
	facts.mode = "NORMAL";
	EXPECT_EQ(run_element(&prompt::module_mode, facts).bytes, "NORMAL");
}

TEST(LeshperPromptModules, TimeIsPaddedAndAsksForTheNextSecond) {
	prompt::state facts = quiet();
	facts.hours = 9;
	facts.minutes = 5;
	facts.seconds = 3;
	facts.tick = 37;

	const rendered got = run_element(&prompt::module_time, facts);
	EXPECT_EQ(got.status, element_status::ready);
	EXPECT_EQ(got.bytes, "09:05:03");

	// The next second on the 10 ms grid, DERIVED from the tick and stored
	// nowhere - which is what makes a parked clock re-arm from the fire rather
	// than catch up.
	EXPECT_EQ(got.wake, 63u);

	facts.tick = 100;
	EXPECT_EQ(run_element(&prompt::module_time, facts).wake, 100u);

	facts.tick = 199;
	EXPECT_EQ(run_element(&prompt::module_time, facts).wake, 1u);
}

TEST(LeshperPromptModules, DurationHasAFloorAndThreeFormats) {
	prompt::state facts = quiet();

	facts.duration_ms = 1999;
	EXPECT_EQ(run_element(&prompt::module_duration, facts).status, element_status::omitted);

	facts.duration_ms = 2000;
	EXPECT_EQ(run_element(&prompt::module_duration, facts).bytes, "2s");

	facts.duration_ms = 59'999;
	EXPECT_EQ(run_element(&prompt::module_duration, facts).bytes, "59s");

	facts.duration_ms = 90'000;
	EXPECT_EQ(run_element(&prompt::module_duration, facts).bytes, "1m30s");

	facts.duration_ms = 3'600'000;
	EXPECT_EQ(run_element(&prompt::module_duration, facts).bytes, "1h0m0s");

	facts.duration_ms = 7'384'000;
	EXPECT_EQ(run_element(&prompt::module_duration, facts).bytes, "2h3m4s");
}

TEST(LeshperPromptModules, EnvReadsItsBoundArgument) {
	prompt::state facts = quiet();
	facts.getvar = &fake_getvar;

	prompt::binding user{std::string_view{"USER"}, nullptr};
	EXPECT_EQ(run_element(&prompt::module_env, facts, &user).bytes, "dana");

	// Set but empty omits: `{env:HOST}@` should vanish rather than render a bare
	// `@`.
	prompt::binding empty{std::string_view{"EMPTY"}, nullptr};
	EXPECT_EQ(run_element(&prompt::module_env, facts, &empty).status, element_status::omitted);

	prompt::binding missing{std::string_view{"NOPE"}, nullptr};
	EXPECT_EQ(run_element(&prompt::module_env, facts, &missing).status, element_status::omitted);

	// No lookup wired up is an omission, not an error.
	facts.getvar = nullptr;
	EXPECT_EQ(run_element(&prompt::module_env, facts, &user).status, element_status::omitted);

	// An unargued placement has no variable to read.
	facts.getvar = &fake_getvar;
	EXPECT_EQ(run_element(&prompt::module_env, facts, nullptr).status, element_status::omitted);
}

TEST(LeshperPromptModules, GitOmitsBeforeTouchingTheFilesystem) {
	prompt::state facts = quiet();
	EXPECT_FALSE(facts.fs_allowed);
	EXPECT_EQ(run_element(&prompt::module_git, facts).status, element_status::omitted);

	// Allowed, but with nowhere to look.
	facts.fs_allowed = true;
	facts.pwd = std::string_view{};
	EXPECT_EQ(run_element(&prompt::module_git, facts).status, element_status::omitted);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST(LeshperPromptEngine, RegistersTheBuiltInsAndListsThemSorted) {
	engine which;

	std::vector<std::string> names;
	which.module_names(names);
	const std::vector<std::string> expected{"duration", "env", "git",  "jobs",
	                                        "mode",     "path", "status", "time"};
	EXPECT_EQ(names, expected);

	EXPECT_TRUE(which.module_exists("git"));
	EXPECT_FALSE(which.module_exists("weather"));
}

TEST(LeshperPromptEngine, RegistrationReplacesAndValidatesTheName) {
	engine which;
	counter first;
	counter second;
	first.label = "first";
	second.label = "second";

	EXPECT_EQ(which.register_module("probe", &counting_module, &first), LESH_OK);
	EXPECT_EQ(which.register_module("probe", &counting_module, &second), LESH_OK);

	EXPECT_EQ(which.register_module("Probe", &counting_module, &first), LESH_ERR_INVAL);
	EXPECT_EQ(which.register_module("two-words", &counting_module, &first), LESH_ERR_INVAL);
	EXPECT_EQ(which.register_module("", &counting_module, &first), LESH_ERR_INVAL);
	EXPECT_EQ(which.register_module("probe", nullptr, &first), LESH_ERR_INVAL);

	which.clear(surface_id::left);
	which.add_module(surface_id::left, "probe", "");
	which.render_full(quiet());

	// The second registration is the one that runs: re-sourcing an rc file is
	// idempotent (#101), not cumulative.
	EXPECT_EQ(first.calls, 0);
	EXPECT_EQ(second.calls, 1);
}

TEST(LeshperPromptEngine, DefaultAndClearRoundTrip) {
	engine which;

	// THE SHIPPED PROMPT, ALL OF IT (owner's ruling on #157): the working
	// directory with `$HOME` contracted, and an arrow. No colour, no branch, no
	// status - the quiet default a user has not yet decided against.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "~/src> ");
	EXPECT_EQ(which.output(surface_id::continuation), "> ");

	which.clear(surface_id::left);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "");

	which.use_default(surface_id::left);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "~/src> ");

	// The path is a real module and follows the facts: a different `$PWD` is a
	// different prompt, which is the whole of what the default promises.
	prompt::state moved = quiet();
	moved.pwd = "/etc";
	which.render_full(moved);
	EXPECT_EQ(which.output(surface_id::left), "/etc> ");

	// AND A FAILURE CHANGES NOTHING. The default carries no `status` seg since the
	// ruling; that the segment machinery still omits and still takes its affixes
	// with it is asserted on the seg itself - `prompt.h`'s selftests 1, 2c and 5,
	// and `AGroupVanishesWithItsModule` below over the ABI.
	prompt::state failed = quiet();
	failed.status = 2;
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), "~/src> ");
}

TEST(LeshperPromptEngine, AddModuleAnswersFalseForAnUnknownName) {
	engine which;
	which.clear(surface_id::left);
	EXPECT_FALSE(which.add_module(surface_id::left, "weather", ""));
	EXPECT_TRUE(which.add_module(surface_id::left, "path", ""));
}

TEST(LeshperPromptEngine, GroupsDoNotNestAndCloseNeedsAnOpen) {
	engine which;
	EXPECT_FALSE(which.close_group(surface_id::left));
	EXPECT_TRUE(which.open_group(surface_id::left));
	EXPECT_FALSE(which.open_group(surface_id::left));
	EXPECT_TRUE(which.close_group(surface_id::left));
	EXPECT_FALSE(which.close_group(surface_id::left));
}

// ---------------------------------------------------------------------------
// The memo
// ---------------------------------------------------------------------------

TEST(LeshperPromptEngine, OneRenderComputesAModuleOncePerArgument) {
	counter probe;
	probe.label = "x";
	probe.constant = true;

	engine which;
	which.register_module("probe", &counting_module, &probe);
	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	which.add_module(surface_id::left, "probe", "same");
	which.add_literal(surface_id::left, "/");
	which.add_module(surface_id::left, "probe", "same");

	which.render_full(quiet());

	// Two placements, one computation - §6.10's per-prompt `(module, arg)` memo,
	// which is what makes free placement free.
	EXPECT_EQ(probe.calls, 1);
	EXPECT_EQ(which.output(surface_id::left), "x/x");

	// A different argument is a different question and is asked.
	probe.calls = 0;
	which.add_module(surface_id::left, "probe", "other");
	which.render_full(quiet());
	EXPECT_EQ(probe.calls, 2);

	// And the memo does not survive the render: a second prompt asks again.
	probe.calls = 0;
	which.render_full(quiet());
	EXPECT_EQ(probe.calls, 2);
}

// ---------------------------------------------------------------------------
// The tick wheel - recalculation by cause
// ---------------------------------------------------------------------------

TEST(LeshperPromptEngine, ATickReInvokesOnlyWhatIsDue) {
	counter early;
	counter late;
	counter still;
	early.label = "a";
	early.wake = 2;
	late.label = "b";
	late.wake = 5;
	still.label = "c";
	still.constant = true;   // no wake at all

	engine which;
	which.register_module("early", &counting_module, &early);
	which.register_module("late", &counting_module, &late);
	which.register_module("still", &counting_module, &still);

	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	which.add_module(surface_id::left, "early", "");
	which.add_module(surface_id::left, "late", "");
	which.add_module(surface_id::left, "still", "");

	prompt::state facts = quiet();
	facts.tick = 0;
	which.render_full(facts);
	EXPECT_EQ(which.output(surface_id::left), "a1b1c");
	EXPECT_EQ(early.calls, 1);
	EXPECT_EQ(late.calls, 1);
	EXPECT_EQ(still.calls, 1);

	// The deadline list's minimum, absolute. `still` armed nothing and therefore
	// contributes nothing to it.
	EXPECT_EQ(which.next_wake(), 2u);

	// A tick before anything is due invokes nothing and owes no write.
	facts.tick = 1;
	EXPECT_FALSE(which.render_tick(facts));
	EXPECT_EQ(early.calls, 1);
	EXPECT_EQ(late.calls, 1);
	EXPECT_EQ(still.calls, 1);

	// THE GATE. At tick 2 exactly one element is due; the other two are spliced
	// from their slots without being asked. A tick that re-invoked an element
	// with no deadline is the defect this test exists for.
	facts.tick = 2;
	EXPECT_TRUE(which.render_tick(facts));
	EXPECT_EQ(early.calls, 2);
	EXPECT_EQ(late.calls, 1);
	EXPECT_EQ(still.calls, 1);
	EXPECT_EQ(which.output(surface_id::left), "a2b1c");

	// Re-armed FROM THE FIRE rather than from the original schedule: `early`'s
	// next deadline is two ticks after the tick it ran on, so a parked stretch
	// causes no catch-up burst (§6.10).
	EXPECT_EQ(which.next_wake(), 4u);

	facts.tick = 5;
	EXPECT_TRUE(which.render_tick(facts));
	EXPECT_EQ(early.calls, 3);
	EXPECT_EQ(late.calls, 2);
	EXPECT_EQ(still.calls, 1);
	EXPECT_EQ(which.output(surface_id::left), "a3b2c");
}

TEST(LeshperPromptEngine, ATickThatChangesNothingOwesNoWrite) {
	counter steady;
	steady.label = "tick";
	steady.constant = true;
	steady.wake = 3;

	engine which;
	which.register_module("steady", &counting_module, &steady);
	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	which.add_module(surface_id::left, "steady", "");

	prompt::state facts = quiet();
	which.render_full(facts);
	EXPECT_EQ(which.output(surface_id::left), "tick");
	EXPECT_EQ(steady.calls, 1);

	facts.tick = 3;
	// Re-invoked - it asked to be - and the bytes came back identical, so the
	// answer is that nothing needs blitting.
	EXPECT_FALSE(which.render_tick(facts));
	EXPECT_EQ(steady.calls, 2);
	EXPECT_EQ(which.output(surface_id::left), "tick");
}

TEST(LeshperPromptEngine, AStaticPromptArmsNoTimer) {
	engine which;
	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	which.add_module(surface_id::left, "path", "");
	which.add_literal(surface_id::left, "$ ");

	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "~/src$ ");

	// An empty deadline list is no timer at all, which is what makes a static
	// prompt cost zero idle wakeups (§6.10).
	EXPECT_EQ(which.next_wake(), 0u);

	prompt::state facts = quiet();
	facts.tick = 1'000;
	EXPECT_FALSE(which.render_tick(facts));
}

TEST(LeshperPromptEngine, TheTimeModuleDrivesTheWheel) {
	engine which;
	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	which.add_module(surface_id::left, "time", "");

	prompt::state facts = quiet();
	facts.tick = 37;
	facts.hours = 1;
	facts.minutes = 2;
	facts.seconds = 3;
	which.render_full(facts);
	EXPECT_EQ(which.output(surface_id::left), "01:02:03");
	EXPECT_EQ(which.next_wake(), 100u);

	facts.tick = 100;
	facts.seconds = 4;
	EXPECT_TRUE(which.render_tick(facts));
	EXPECT_EQ(which.output(surface_id::left), "01:02:04");
	EXPECT_EQ(which.next_wake(), 200u);
}

// ---------------------------------------------------------------------------
// The C ABI
// ---------------------------------------------------------------------------

TEST(LeshperPromptAbi, EveryVerbNeedsAnEngineOnTheRegistry) {
	lesh_registry bare;
	ASSERT_EQ(bare.prompt_engine, nullptr);

	std::int32_t exists = 0;
	EXPECT_EQ(lesh_prompt_module_register(&bare, "x", &abi_module, nullptr), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_module_exists(&bare, "x", &exists), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_clear(&bare, LESH_PROMPT_LEFT), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_use_default(&bare, LESH_PROMPT_LEFT), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_add_module(&bare, LESH_PROMPT_LEFT, "path", nullptr),
	          LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_add_literal(&bare, LESH_PROMPT_LEFT, "$", 1), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_group_open(&bare, LESH_PROMPT_LEFT), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_group_close(&bare, LESH_PROMPT_LEFT), LESH_ERR_NOTFOUND);

	// A null registry is a malformed argument, which is a different answer.
	EXPECT_EQ(lesh_prompt_clear(nullptr, LESH_PROMPT_LEFT), LESH_ERR_INVAL);
}

TEST(LeshperPromptAbi, RejectsBadSurfacesAndBadNames) {
	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;

	EXPECT_EQ(lesh_prompt_clear(&registry, 7u), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_add_literal(&registry, LESH_PROMPT_LEFT, nullptr, 3), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, nullptr, nullptr),
	          LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "weather", nullptr),
	          LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_module_register(&registry, "Bad-Name", &abi_module, nullptr),
	          LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_module_register(&registry, "ok_name", nullptr, nullptr),
	          LESH_ERR_INVAL);
}

TEST(LeshperPromptAbi, ARegisteredModuleWritesReadsItsArgAndAsksForAWake) {
	abi_probe probe;

	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;

	ASSERT_EQ(lesh_prompt_module_register(&registry, "probe", &abi_module, &probe), LESH_OK);

	std::int32_t exists = 0;
	ASSERT_EQ(lesh_prompt_module_exists(&registry, "probe", &exists), LESH_OK);
	EXPECT_EQ(exists, 1);
	ASSERT_EQ(lesh_prompt_module_exists(&registry, "weather", &exists), LESH_OK);
	EXPECT_EQ(exists, 0);

	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_CONTINUATION), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "probe", "hello"), LESH_OK);

	prompt::state facts = quiet();
	facts.tick = 42;
	facts.status = 3;
	facts.getvar = &fake_getvar;
	which.render_full(facts);

	EXPECT_EQ(probe.calls, 1);
	EXPECT_EQ(probe.arg, "hello");
	EXPECT_EQ(probe.tick, 42u);
	EXPECT_EQ(probe.last_status, 3);
	EXPECT_EQ(probe.variable_result, LESH_OK);
	EXPECT_EQ(probe.variable, "dana");
	EXPECT_EQ(which.output(surface_id::left), "<hello>");

	// The wake it asked for, turned into an absolute deadline at the render that
	// heard it.
	EXPECT_EQ(which.next_wake(), 49u);
}

TEST(LeshperPromptAbi, ARegisteredModuleIsReplacedNotStacked) {
	abi_probe first;
	abi_probe second;

	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;

	ASSERT_EQ(lesh_prompt_module_register(&registry, "probe", &abi_module, &first), LESH_OK);
	ASSERT_EQ(lesh_prompt_module_register(&registry, "probe", &abi_module, &second), LESH_OK);

	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_CONTINUATION), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "probe", "x"), LESH_OK);
	which.render_full(quiet());

	EXPECT_EQ(first.calls, 0);
	EXPECT_EQ(second.calls, 1);
}

TEST(LeshperPromptAbi, AGroupVanishesWithItsModule) {
	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;

	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_CONTINUATION), LESH_OK);

	ASSERT_EQ(lesh_prompt_group_open(&registry, LESH_PROMPT_LEFT), LESH_OK);
	// A second open while one is open is refused rather than nested: groups do
	// not nest in v1 across this surface.
	EXPECT_EQ(lesh_prompt_group_open(&registry, LESH_PROMPT_LEFT), LESH_ERR_REFUSED);
	ASSERT_EQ(lesh_prompt_add_literal(&registry, LESH_PROMPT_LEFT, " [", 2), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "status", nullptr), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_literal(&registry, LESH_PROMPT_LEFT, "]", 1), LESH_OK);
	ASSERT_EQ(lesh_prompt_group_close(&registry, LESH_PROMPT_LEFT), LESH_OK);
	EXPECT_EQ(lesh_prompt_group_close(&registry, LESH_PROMPT_LEFT), LESH_ERR_REFUSED);

	ASSERT_EQ(lesh_prompt_add_literal(&registry, LESH_PROMPT_LEFT, "$ ", 2), LESH_OK);

	// The module omitted, so the brackets went with it - and the top-level
	// literal did not, because binding is explicit grouping.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "$ ");

	prompt::state failed = quiet();
	failed.status = 130;
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), " [130]$ ");
}

TEST(LeshperPromptAbi, AGroupsDecorationsDoNotRunWhenTheVoteFails) {
	counter inner;
	inner.label = "!";

	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;

	// A module that never says anything, so the group can never be shown.
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_CONTINUATION), LESH_OK);
	ASSERT_EQ(lesh_prompt_group_open(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_literal(&registry, LESH_PROMPT_LEFT, "on ", 3), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "git", nullptr), LESH_OK);
	ASSERT_EQ(lesh_prompt_group_close(&registry, LESH_PROMPT_LEFT), LESH_OK);

	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "");

	// The same shape with a module that DOES say something: the literal runs
	// now, and in declared order before the module's bytes.
	ASSERT_EQ(which.register_module("shout", &counting_module, &inner), LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_group_open(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_literal(&registry, LESH_PROMPT_LEFT, "on ", 3), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "shout", nullptr), LESH_OK);
	ASSERT_EQ(lesh_prompt_group_close(&registry, LESH_PROMPT_LEFT), LESH_OK);

	which.render_full(quiet());
	EXPECT_EQ(inner.calls, 1);
	EXPECT_EQ(which.output(surface_id::left), "on !1");
}

// A null context is a STASHED-HANDLE bug, and abi.h says so: it is undefined
// behaviour, debug-asserted, and the assertion aborts. There is no test for it
// here for the same reason there is none for a stashed `lesh_editor` - proving
// it would need a death test, and the behaviour being proved is "the process
// stops", which the assertion already guarantees at the call that did it.

// A module that asks for its argument with a buffer too small for it, so the
// copy-out convention is exercised the way a real binding exercises it: ask the
// length, then ask again.
std::int32_t measuring_module(lesh_prompt_context* context, void* userdata) {
	auto* probe = static_cast<abi_probe*>(userdata);
	++probe->calls;

	std::size_t needed = 0;
	const std::int32_t first = lesh_prompt_arg(context, nullptr, 0, &needed);
	char small[2];
	const std::int32_t second = lesh_prompt_arg(context, small, sizeof small, &needed);

	probe->variable_result = (first == LESH_ERR_TOOSMALL && second == LESH_ERR_TOOSMALL)
		? LESH_OK
		: LESH_ERR_INVAL;
	probe->arg = std::to_string(needed);

	std::string bytes;
	bytes.resize(needed);
	if (lesh_prompt_arg(context, bytes.data(), bytes.size(), &needed) == LESH_OK)
		lesh_prompt_write(context, bytes.data(), bytes.size());
	return LESH_PROMPT_READY;
}

TEST(LeshperPromptAbi, ArgFollowsTheCopyOutConvention) {
	abi_probe probe;

	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;

	ASSERT_EQ(lesh_prompt_module_register(&registry, "measure", &measuring_module, &probe),
	          LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_CONTINUATION), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "measure", "abcdef"), LESH_OK);

	which.render_full(quiet());
	EXPECT_EQ(probe.calls, 1);
	EXPECT_EQ(probe.variable_result, LESH_OK);
	EXPECT_EQ(probe.arg, "6");
	EXPECT_EQ(which.output(surface_id::left), "abcdef");
}

// A module that fails. Its status is not an error channel out of the render -
// the element is simply omitted and the prompt still draws.
std::int32_t failing_module(lesh_prompt_context* context, void*) {
	lesh_prompt_write(context, "residue", 7);
	return LESH_ERR_INVAL;
}

TEST(LeshperPromptAbi, ANegativeStatusReadsAsOmitted) {
	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;

	ASSERT_EQ(lesh_prompt_module_register(&registry, "broken", &failing_module, nullptr),
	          LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_CONTINUATION), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "broken", nullptr), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_literal(&registry, LESH_PROMPT_LEFT, "$ ", 2), LESH_OK);

	which.render_full(quiet());

	// Omitted means its bytes never reach the surface, whatever it wrote before
	// giving up.
	EXPECT_EQ(which.output(surface_id::left), "$ ");
}

} // namespace

// ---------------------------------------------------------------------------
// --- the git HEAD reader (#157) ---
// ---------------------------------------------------------------------------
//
// §6.10's budgeted `git` module reads `.git/HEAD` by hand and falls back to
// `git` itself when it meets a layout it does not speak. Both halves are tested
// against the REAL filesystem, built by hand, because there is nothing here
// worth faking: the whole content of the fast path is which file is opened next
// and what its bytes mean, and a fake filesystem would be a restatement of this
// file's own assumptions rather than a check on them.
//
// The layouts are built rather than produced by running `git`. Running git
// would make the tests depend on the version installed - `git worktree`'s
// `commondir` and `pack-refs`'s header line have both changed spelling - and
// would test that git agrees with itself. Written by hand, each case says
// exactly which bytes the reader is being held to.
//
// The FALLBACK is exercised with a stub `git`, not the real one: the point
// under test is the spawn's plumbing - argv, the pipe, the exit status, the
// deadline and the kill - and a stub is the only way to drive the paths that
// matter (a command that prints nothing, and one that never returns). One
// stub answers BOTH argv forms, because the fallback is two commands and the
// detached path only exists past the first one's empty output.

namespace {

using lesh::leshper::prompt::git_head;
using lesh::leshper::prompt::git_probe_options;
using lesh::leshper::prompt::read_git_head;

void write_text(const std::string& path, std::string_view text) {
	std::filesystem::create_directories(std::filesystem::path{path}.parent_path());
	std::ofstream out{path, std::ios::binary | std::ios::trunc};
	ASSERT_TRUE(out.good()) << path;
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	out.close();
	ASSERT_TRUE(out.good()) << path;
}

void make_dirs(const std::string& path) {
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
	ASSERT_FALSE(ec) << path << ": " << ec.message();
}

// A `git` that is two lines of `sh`, answering both forms the fallback sends.
//
// `branch_line` empty means `branch --show-current` prints nothing and exits 0,
// which is how real git says "detached". Every invocation appends its whole
// argv to `args_log`, so a test can assert that `-C <dir>` actually went where
// it was meant to - the fallback's correctness rests on that one flag.
void write_git_stub(const std::string& path, std::string_view branch_line,
                    std::string_view sha_line, int sha_status,
                    const std::string& args_log) {
	std::string script = "#!/bin/sh\n";
	script += "printf '%s\\n' \"$@\" >> '" + args_log + "'\n";
	script += "for a in \"$@\"; do\n";
	script += "\tif [ \"$a\" = \"--show-current\" ]; then\n";
	if (!branch_line.empty())
		script += "\t\tprintf '%s\\n' '" + std::string{branch_line} + "'\n";
	script += "\t\texit 0\n";
	script += "\tfi\n";
	script += "\tif [ \"$a\" = \"rev-parse\" ]; then\n";
	if (!sha_line.empty())
		script += "\t\tprintf '%s\\n' '" + std::string{sha_line} + "'\n";
	script += "\t\texit " + std::to_string(sha_status) + "\n";
	script += "\tfi\n";
	script += "done\n";
	script += "exit 1\n";
	write_text(path, script);
	ASSERT_EQ(::chmod(path.c_str(), 0755), 0) << path;
}

std::string read_text(const std::string& path) {
	std::ifstream in{path, std::ios::binary};
	return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Forty hex digits with `seed` repeated, so a test's expected short sha is
// readable at the call site instead of being a slice of a magic constant.
std::string sha40(char seed) { return std::string(40, seed); }

// No TearDown: `temp_path`'s destructor removes the tree, which reaches even a
// case that died on an ASSERT partway through building a layout.
class LeshperPromptGit : public ::testing::Test {
protected:
	[[nodiscard]] std::string at(std::string_view relative) const {
		return _tmp.file(relative);
	}

	// A plain repository at `<tmp>/<name>` with the given HEAD contents.
	[[nodiscard]] std::string make_repo(std::string_view name, std::string_view head) {
		const std::string root = at(name);
		make_dirs(root + "/.git/refs/heads");
		write_text(root + "/.git/HEAD", head);
		return root;
	}

	// EVERY CASE THAT ACTUALLY SPAWNS OVERRIDES THE 50 ms DEFAULT, and the
	// reason is the test process rather than the shell. Under ASan/UBSan/LSan a
	// `posix_spawn` of `/bin/sh` costs 40-60 ms here - the sanitized binary's
	// loader work, then a shell script on top of it - so the shipped budget
	// would SIGKILL the stub mid-flight and every fallback assertion would then
	// pass or fail by the machine's load rather than by the code. That is not a
	// claim about the default being wrong: it is measured against real `git` in
	// a release binary, and this constant is the same budget scaled to an
	// instrumented one. The deadline itself is under test in its own two cases.
	static constexpr std::uint32_t kSpawnBudgetMs = 5000;

	lesh::testing::temp_path _tmp;
};

// --- the fast path: symbolic HEAD -----------------------------------------

TEST_F(LeshperPromptGit, SymbolicHeadResolvesThroughLooseRef) {
	const std::string repo = make_repo("plain", "ref: refs/heads/main\n");
	write_text(repo + "/.git/refs/heads/main", sha40('a') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "main");
	EXPECT_EQ(head.short_sha, "aaaaaaa");
}

TEST_F(LeshperPromptGit, BranchNameKeepsSlashesBelowRefsHeads) {
	const std::string repo = make_repo("slashy", "ref: refs/heads/feature/deep/name\n");
	write_text(repo + "/.git/refs/heads/feature/deep/name", sha40('b') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "feature/deep/name");
	EXPECT_EQ(head.short_sha, "bbbbbbb");
}

// Mid-rebase, HEAD names a ref outside `refs/heads/`. Reported whole, because
// shortening it would print something that looks like an ordinary branch.
TEST_F(LeshperPromptGit, RefnameOutsideRefsHeadsIsReportedWhole) {
	const std::string repo = make_repo("rebasing", "ref: refs/rebase-merge/onto\n");
	write_text(repo + "/.git/refs/rebase-merge/onto", sha40('c') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "refs/rebase-merge/onto");
	EXPECT_EQ(head.short_sha, "ccccccc");
}

// --- the fast path: packed refs -------------------------------------------

TEST_F(LeshperPromptGit, PackedRefsMatchExactlyAndSkipHeaderAndPeeledLines) {
	const std::string repo = make_repo("packed", "ref: refs/heads/main\n");
	// No loose ref at all - this is a freshly `pack-refs`'d repository. The
	// neighbours are the point: a prefix match answers `main2`, a suffix match
	// answers `origin/main`, and a line-kind mistake answers the peeled sha.
	write_text(repo + "/.git/packed-refs",
		"# pack-refs with: peeled fully-peeled sorted \n"
		+ sha40('1') + " refs/heads/main2\n"
		+ sha40('2') + " refs/heads/mainline\n"
		+ sha40('3') + " refs/tags/v1.0\n"
		"^" + sha40('4') + "\n"
		+ sha40('5') + " refs/heads/main\n"
		+ sha40('6') + " refs/remotes/origin/main\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "main");
	EXPECT_EQ(head.short_sha, "5555555");
}

// The loose file is git's own resolution order, and it matters after a branch
// moves: `pack-refs` leaves the packed line behind and a new loose file wins.
TEST_F(LeshperPromptGit, LooseRefWinsOverAStalePackedLine) {
	const std::string repo = make_repo("both", "ref: refs/heads/main\n");
	write_text(repo + "/.git/refs/heads/main", sha40('e') + "\n");
	write_text(repo + "/.git/packed-refs", sha40('f') + " refs/heads/main\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.short_sha, "eeeeeee");
}

// A `packed-refs` with no trailing newline on its last line is still a file
// with that ref in it.
TEST_F(LeshperPromptGit, PackedRefsFinalLineNeedsNoNewline) {
	const std::string repo = make_repo("nonl", "ref: refs/heads/last\n");
	write_text(repo + "/.git/packed-refs",
		"# pack-refs with: peeled\n" + sha40('7') + " refs/heads/last");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "last");
	EXPECT_EQ(head.short_sha, "7777777");
}

// --- the fast path: unborn and detached ------------------------------------

TEST_F(LeshperPromptGit, UnbornBranchIsFoundWithNoSha) {
	// `git init` and nothing else: HEAD names a branch, and no ref exists.
	const std::string repo = make_repo("fresh", "ref: refs/heads/main\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "main");
	EXPECT_TRUE(head.short_sha.empty());
}

TEST_F(LeshperPromptGit, DetachedHeadIsTheObjectNameItself) {
	const std::string repo = make_repo("detached", sha40('d') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_TRUE(head.detached);
	EXPECT_TRUE(head.branch.empty());
	EXPECT_EQ(head.short_sha, "ddddddd");
}

TEST_F(LeshperPromptGit, DetachedHeadAcceptsASha256ObjectName) {
	const std::string repo = make_repo("sha256", std::string(64, '9') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_TRUE(head.detached);
	EXPECT_EQ(head.short_sha, "9999999");
}

// --- discovery -------------------------------------------------------------

TEST_F(LeshperPromptGit, DiscoveryClimbsOutOfANestedDirectory) {
	const std::string repo = make_repo("nested", "ref: refs/heads/trunk\n");
	write_text(repo + "/.git/refs/heads/trunk", sha40('8') + "\n");
	const std::string deep = repo + "/src/leshper/prompt";
	make_dirs(deep);

	const git_head head = read_git_head(deep);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "trunk");
	EXPECT_EQ(head.short_sha, "8888888");
}

TEST_F(LeshperPromptGit, TrailingSlashesOnTheDirectoryDoNotMatter) {
	const std::string repo = make_repo("slashes", "ref: refs/heads/main\n");

	const git_head head = read_git_head(repo + "///");
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "main");
}

// The walk runs to the filesystem root and finds nothing. Not a hand-off to
// the fallback: there is nothing here for `git` to tell us either.
TEST_F(LeshperPromptGit, OutsideAnyRepositoryTheAnswerIsNotFound) {
	const std::string plain = at("no_repo_here/deeper");
	make_dirs(plain);

	const git_head head = read_git_head(plain);
	EXPECT_FALSE(head.found);
	EXPECT_TRUE(head.branch.empty());
	EXPECT_TRUE(head.short_sha.empty());
}

// --- gitfile indirection ---------------------------------------------------

TEST_F(LeshperPromptGit, GitfileWithARelativeGitdirIsFollowed) {
	// The submodule shape: the working tree's `.git` is a file pointing into
	// the superproject's `.git/modules/`.
	const std::string root = at("super");
	make_dirs(root + "/.git/modules/sub/refs/heads");
	write_text(root + "/.git/modules/sub/HEAD", "ref: refs/heads/feature\n");
	write_text(root + "/.git/modules/sub/refs/heads/feature", sha40('a') + "\n");
	make_dirs(root + "/sub");
	// Relative to the directory holding the `.git` FILE, not to the cwd.
	write_text(root + "/sub/.git", "gitdir: ../.git/modules/sub\n");

	const git_head head = read_git_head(root + "/sub");
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "feature");
	EXPECT_EQ(head.short_sha, "aaaaaaa");
}

TEST_F(LeshperPromptGit, GitfileWithAnAbsoluteGitdirIsFollowed) {
	const std::string root = at("super_abs");
	const std::string gitdir = root + "/.git/modules/sub";
	make_dirs(gitdir + "/refs/heads");
	write_text(gitdir + "/HEAD", "ref: refs/heads/other\n");
	write_text(gitdir + "/refs/heads/other", sha40('b') + "\n");
	make_dirs(root + "/sub");
	write_text(root + "/sub/.git", "gitdir: " + gitdir + "\n");

	const git_head head = read_git_head(root + "/sub");
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "other");
	EXPECT_EQ(head.short_sha, "bbbbbbb");
}

// A `.git` file that is not the one format a `.git` file has. Unrecognized, so
// with the spawn declined the answer is not-found rather than a guess.
TEST_F(LeshperPromptGit, MalformedGitfileIsUnrecognizedRatherThanGuessed) {
	const std::string root = at("bad_gitfile");
	make_dirs(root);
	write_text(root + "/.git", "this is not a gitfile\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(root, options).found);
}

// --- the linked-worktree shape ---------------------------------------------

TEST_F(LeshperPromptGit, LinkedWorktreeReadsHeadLocallyAndRefsFromTheCommonDir) {
	const std::string main_root = at("wt_main");
	const std::string common = main_root + "/.git";
	make_dirs(common + "/refs/heads");
	write_text(common + "/HEAD", "ref: refs/heads/main\n");
	// The refs live once, in the common dir, and they are packed there.
	write_text(common + "/packed-refs",
		"# pack-refs with: peeled fully-peeled sorted \n"
		+ sha40('1') + " refs/heads/main\n"
		+ sha40('2') + " refs/heads/side\n");

	const std::string private_dir = common + "/worktrees/side";
	make_dirs(private_dir);
	write_text(private_dir + "/HEAD", "ref: refs/heads/side\n");
	write_text(private_dir + "/commondir", "../..\n");

	const std::string linked = at("wt_side");
	make_dirs(linked);
	write_text(linked + "/.git", "gitdir: " + private_dir + "\n");

	// HEAD from the PRIVATE dir - the linked worktree is on `side` - and the
	// sha from the COMMON dir's packed-refs, where the ref actually is.
	const git_head side = read_git_head(linked);
	EXPECT_TRUE(side.found);
	EXPECT_FALSE(side.detached);
	EXPECT_EQ(side.branch, "side");
	EXPECT_EQ(side.short_sha, "2222222");

	// The main worktree is unmoved by any of it, which is the other half of
	// "HEAD is local": a commondir read backwards would report `main` above.
	const git_head main_head = read_git_head(main_root);
	EXPECT_TRUE(main_head.found);
	EXPECT_EQ(main_head.branch, "main");
	EXPECT_EQ(main_head.short_sha, "1111111");
}

// --- layouts this reader refuses to guess at -------------------------------

TEST_F(LeshperPromptGit, ReftableWithoutSpawnIsNotFound) {
	const std::string repo = make_repo("reftable_repo", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	// A ref that would resolve if the reader ignored the reftable dir. It must
	// not be reported: with a reftable store the loose file is not the truth.
	write_text(repo + "/.git/refs/heads/main", sha40('a') + "\n");

	git_probe_options options;
	options.allow_spawn = false;
	const git_head head = read_git_head(repo, options);
	EXPECT_FALSE(head.found);
	EXPECT_TRUE(head.branch.empty());
}

TEST_F(LeshperPromptGit, UnreadableHeadWithoutSpawnIsNotFound) {
	const std::string repo = make_repo("garbage_head", "who knows what this is\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// Short hex is not an object name. `1234567` in HEAD is a file we do not
// understand, and reading it as a detached head would be a plausible lie.
TEST_F(LeshperPromptGit, ShortHexInHeadIsNotADetachedHead) {
	const std::string repo = make_repo("shorthex", "1234567\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// --- the spawn fallback ----------------------------------------------------

TEST_F(LeshperPromptGit, FallbackAsksGitAndReportsItsBranch) {
	const std::string repo = make_repo("fallback_branch", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string stub = at("git_stub_branch.sh");
	const std::string log = at("git_stub_branch.args");
	write_git_stub(stub, "from-the-stub", "abc1234", 0, log);

	git_probe_options options;
	options.git_command = stub.c_str();
	options.budget_ms = kSpawnBudgetMs;
	const git_head head = read_git_head(repo, options);

	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "from-the-stub");
	EXPECT_TRUE(head.short_sha.empty());

	// The argv is load-bearing: without `-C <directory>` the stub - and the
	// real git - would answer about whatever directory the shell happens to be
	// in, which at prompt time is a different repository often enough.
	const std::string args = read_text(log);
	EXPECT_EQ(args, "-C\n" + repo + "\nbranch\n--show-current\n");
}

TEST_F(LeshperPromptGit, FallbackTurnsEmptyOutputIntoASecondQuestion) {
	const std::string repo = make_repo("fallback_detached", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string stub = at("git_stub_detached.sh");
	const std::string log = at("git_stub_detached.args");
	// Nothing from `branch --show-current`, which is how git says detached.
	write_git_stub(stub, "", "beefca7", 0, log);

	git_probe_options options;
	options.git_command = stub.c_str();
	options.budget_ms = kSpawnBudgetMs;
	const git_head head = read_git_head(repo, options);

	EXPECT_TRUE(head.found);
	EXPECT_TRUE(head.detached);
	EXPECT_TRUE(head.branch.empty());
	EXPECT_EQ(head.short_sha, "beefca7");

	// Both commands, in order, both with the directory.
	const std::string args = read_text(log);
	EXPECT_EQ(args, "-C\n" + repo + "\nbranch\n--show-current\n"
	                "-C\n" + repo + "\nrev-parse\n--short\nHEAD\n");
}

// Half an answer is worse than none: `found + detached` with no sha renders a
// segment's literals around nothing.
TEST_F(LeshperPromptGit, FallbackWithoutTheShaIsNotFoundRatherThanHalfDetached) {
	const std::string repo = make_repo("fallback_half", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string stub = at("git_stub_half.sh");
	const std::string log = at("git_stub_half.args");
	write_git_stub(stub, "", "", 128, log);

	git_probe_options options;
	options.git_command = stub.c_str();
	options.budget_ms = kSpawnBudgetMs;
	const git_head head = read_git_head(repo, options);
	EXPECT_FALSE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_TRUE(head.short_sha.empty());
}

TEST_F(LeshperPromptGit, AMissingGitCommandIsNotFoundAndNotACrash) {
	const std::string repo = make_repo("no_git", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string absent = at("definitely_not_here/git");

	git_probe_options options;
	options.git_command = absent.c_str();
	options.budget_ms = kSpawnBudgetMs;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// --- the budget ------------------------------------------------------------

TEST_F(LeshperPromptGit, ZeroBudgetAnswersWithoutTouchingTheFilesystem) {
	const std::string repo = make_repo("budget_zero", "ref: refs/heads/main\n");
	write_text(repo + "/.git/refs/heads/main", sha40('a') + "\n");

	git_probe_options options;
	options.budget_ms = 0;

	const auto started = std::chrono::steady_clock::now();
	const git_head head = read_git_head(repo, options);
	const auto elapsed = std::chrono::steady_clock::now() - started;

	// A repository that resolves perfectly well with any budget at all.
	EXPECT_TRUE(read_git_head(repo).found);
	EXPECT_FALSE(head.found);
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 200);
}

// The one that would hang a prompt. The stub sleeps far past the budget; the
// probe must give up, SIGKILL the child, reap it, and return. The sanitized
// build is half the assertion here - an unreaped child's pipe and buffers show
// up as a leak - and the elapsed bound is the other half.
TEST_F(LeshperPromptGit, AHungFallbackIsKilledAndTheProbeReturns) {
	const std::string repo = make_repo("budget_hang", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string stub = at("git_stub_hang.sh");
	write_text(stub, "#!/bin/sh\nsleep 5\necho too-late\n");
	ASSERT_EQ(::chmod(stub.c_str(), 0755), 0);

	git_probe_options options;
	options.git_command = stub.c_str();
	// Comfortably past the spawn cost above, nowhere near the stub's five
	// seconds: what is being timed is the deadline, not the loader.
	options.budget_ms = 500;

	const auto started = std::chrono::steady_clock::now();
	const git_head head = read_git_head(repo, options);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();

	EXPECT_FALSE(head.found);
	// Generous: the bound that matters is "well under the stub's five seconds",
	// and a loaded machine under three sanitizers is not a stopwatch.
	EXPECT_LT(elapsed, 1500) << "the probe waited " << elapsed << "ms";
}

// ---------------------------------------------------------------------------
// --- git through the composer, and the gate it guards (#157) ---
// ---------------------------------------------------------------------------
//
// The reader has its own cases above; these two are about what the COMPOSER
// does with it, which is a different question and the one §6.10's performance
// floor rests on.

TEST_F(LeshperPromptGit, APlacedGitSegRendersTheBranchItIsStandingIn) {
	const std::string repo = make_repo("composed", "ref: refs/heads/topic\n");
	write_text(repo + "/.git/refs/heads/topic", sha40('d') + "\n");

	// A `git` seg, PLACED, against a real repository: the group votes ready, brings
	// its literal with it, and the branch appears.
	//
	// PLACED RATHER THAN SHIPPED, since the owner's ruling on #157 took `git` out
	// of the default table - the default is a path and an arrow now, and a user who
	// wants a branch asks for one. What is asserted is unchanged and so is its
	// strength: the same reader, the same real `.git`, the same composer, and the
	// group here is the runtime form of the `seg` the table used to hold, which
	// means this now exercises the ABI's two-phase group as well.
	engine which;
	which.clear(surface_id::left);
	ASSERT_TRUE(which.open_group(surface_id::left));
	which.add_literal(surface_id::left, " on ");
	ASSERT_TRUE(which.add_module(surface_id::left, "git", ""));
	ASSERT_TRUE(which.close_group(surface_id::left));

	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.home = std::string_view{};
	// The one fact that lets a budgeted module touch the disk at all.
	facts.fs_allowed = true;
	which.render_full(facts);

	const std::string_view left = which.output(surface_id::left);
	EXPECT_NE(left.find(" on topic"), std::string_view::npos) << left;

	// And the same placement outside a repository says nothing at all about git -
	// the seg's literal vanishing with the module, which the compile-time selftests
	// check against synthetic facts and this checks against a real filesystem.
	const std::string bare = at("not_a_repo");
	make_dirs(bare);
	prompt::state elsewhere = facts;
	elsewhere.pwd = bare;
	which.render_full(elsewhere);
	EXPECT_EQ(which.output(surface_id::left).find(" on "), std::string_view::npos)
		<< which.output(surface_id::left);
}

// THE GATE #157 NAMES, and the one that would be expensive to get wrong: a
// spinner ticking beside `git` must not drag `git` along with it. §6.10 is
// explicit - "a tick that animates a spinner re-invokes the spinner alone and
// `git`'s slot is memcpy'd" - and the cost of the defect is a `.git/HEAD` read,
// possibly on an NFS mount, ten times a second.
//
// The assertion is made against the FILESYSTEM rather than against a call
// counter, because a counter would only prove the engine did not call the
// function this test registered. Moving the branch on disk under the running
// prompt proves the bytes came from the slot: had the tick re-read anything, it
// would have read `two`.
TEST_F(LeshperPromptGit, ATickSplicesGitsSlotAndOnlyANewPromptRereadsIt) {
	const std::string repo = make_repo("ticking", "ref: refs/heads/one\n");
	write_text(repo + "/.git/refs/heads/one", sha40('e') + "\n");
	write_text(repo + "/.git/refs/heads/two", sha40('f') + "\n");

	counter spinner;
	spinner.label = "|";
	spinner.wake = 3;

	engine which;
	which.register_module("spinner", &counting_module, &spinner);
	which.clear(surface_id::left);
	which.add_module(surface_id::left, "git", "");
	which.add_literal(surface_id::left, " ");
	which.add_module(surface_id::left, "spinner", "");

	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.fs_allowed = true;
	facts.tick = 0;
	which.render_full(facts);
	EXPECT_EQ(which.output(surface_id::left), "one |1");
	EXPECT_EQ(which.next_wake(), 3u);

	// The branch moves, under a prompt that is already on screen.
	write_text(repo + "/.git/HEAD", "ref: refs/heads/two\n");

	facts.tick = 3;
	EXPECT_TRUE(which.render_tick(facts));
	// The spinner advanced; `git` did not. Its slot was spliced whole.
	EXPECT_EQ(which.output(surface_id::left), "one |2");

	// A NEW PROMPT is the cause that re-reads it - the only one that does
	// (§6.10's three reasons, and `git` answers exactly one of them).
	facts.tick = 4;
	which.render_full(facts);
	EXPECT_EQ(which.output(surface_id::left), "two |3");
}

// ---------------------------------------------------------------------------
// --- the runtime seam (#157, §6.10) ---
// ---------------------------------------------------------------------------
//
// WHERE `timer_interval_ms`'S COVERAGE IS: in prompt.h, as `static_assert`s
// beside the function. It is a pure integer expression with four cases and the
// compiler checks all four in every translation unit that includes the header,
// which is a stronger statement than a test that runs on this binary only - and
// the same discipline the composer's omission rules already follow.
//
// WHERE `leshper_prompt_console`'S IS NOT: it stays anonymous in read.cpp,
// unreachable from here, and that is `binding_console`'s precedent exactly -
// that adapter is tested through `bind`, and this one through `prompt`, whose
// own tests drive a fake console rather than reaching into read.cpp
// (tests/unit/prompt_builtin_tests.cpp). Contorting the design to make an
// untestable-by-necessity
// class reachable would be paying for the follow-up ticket's test in this
// ticket's code. What IS asserted here is the seam's own contract: the interface
// is implementable over the real engine, the six verbs reach it, the two
// surfaces map, and the install point round-trips - which is everything the
// builtin will stand on when it is written.

// The same twenty lines read.cpp's adapter has, over a real engine. It exists to
// prove they are enough, which is what `leshper_keymap_tests.cpp`'s copy did for
// `bind` before there was a loop to hold the real one.
class test_prompt_console final : public lesh::runtime::prompt_console {
public:
	explicit test_prompt_console(engine& which) noexcept : _engine(&which) {}

	void module_names(std::vector<std::string>& into) const override {
		_engine->module_names(into);
	}

	outcome clear(surface which) override {
		_engine->clear(surface_of(which));
		return outcome::ok;
	}

	outcome use_default(surface which) override {
		_engine->use_default(surface_of(which));
		return outcome::ok;
	}

	outcome add_module(surface which, std::string_view name, std::string_view arg) override {
		return _engine->add_module(surface_of(which), name, arg) ? outcome::ok
		                                                         : outcome::no_such_module;
	}

	outcome add_literal(surface which, std::string_view bytes) override {
		_engine->add_literal(surface_of(which), bytes);
		return outcome::ok;
	}

	outcome open_group(surface which) override {
		return _engine->open_group(surface_of(which)) ? outcome::ok : outcome::unbalanced_group;
	}

	outcome close_group(surface which) override {
		return _engine->close_group(surface_of(which)) ? outcome::ok : outcome::unbalanced_group;
	}

	// The same two bodies read.cpp's adapter has, now that there is a language to
	// read a template in: one parse-and-swap, and the source string it remembered.
	outcome set(surface which, std::string_view template_text, std::string& error_out) override {
		return _engine->set_template(surface_of(which), template_text, error_out)
			? outcome::ok
			: outcome::bad_template;
	}

	void text(surface which, std::string& out) const override {
		out.assign(_engine->template_text(surface_of(which)));
	}

private:
	[[nodiscard]] static surface_id surface_of(surface which) noexcept {
		return which == surface::continuation ? surface_id::continuation : surface_id::left;
	}

	engine* _engine;
};

using console_surface = lesh::runtime::prompt_console::surface;
using console_outcome = lesh::runtime::prompt_console::outcome;

TEST(LeshperPromptWiring, TheConsoleRoundTripsOnTheShellState) {
	lesh::runtime::shell_state shell;
	// A non-interactive shell has no prompt engine and says so rather than
	// pretending - `bind`'s "no line editor", one seam over.
	EXPECT_EQ(shell.prompts(), nullptr);

	engine which;
	test_prompt_console console{which};
	shell.set_prompt_console(&console);
	ASSERT_EQ(shell.prompts(), &console);

	// And the owner takes the view away as it takes the object (ADR-0007), which
	// is what `session::~session` does one line below its `bind` counterpart.
	shell.set_prompt_console(nullptr);
	EXPECT_EQ(shell.prompts(), nullptr);
}

TEST(LeshperPromptWiring, TheConsoleVerbsReachTheEngineAndBothSurfaces) {
	engine which;
	test_prompt_console console{which};
	lesh::runtime::prompt_console& seam = console;

	std::vector<std::string> names;
	seam.module_names(names);
	EXPECT_NE(std::find(names.begin(), names.end(), "git"), names.end());
	EXPECT_NE(std::find(names.begin(), names.end(), "path"), names.end());

	// The two surfaces are configured apart, and what this asserts is the
	// translation between the runtime's `surface` and leshper's `surface_id`:
	// bytes put on one must not appear on the other.
	EXPECT_EQ(seam.clear(console_surface::left), console_outcome::ok);
	EXPECT_EQ(seam.clear(console_surface::continuation), console_outcome::ok);
	EXPECT_EQ(seam.add_literal(console_surface::left, "L"), console_outcome::ok);
	EXPECT_EQ(seam.add_literal(console_surface::continuation, "C"), console_outcome::ok);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "L");
	EXPECT_EQ(which.output(surface_id::continuation), "C");

	// The one miss a placement has, and it is a miss rather than a fault.
	EXPECT_EQ(seam.add_module(console_surface::left, "path", ""), console_outcome::ok);
	EXPECT_EQ(seam.add_module(console_surface::left, "no_such_thing", ""),
	          console_outcome::no_such_module);

	// Groups do not nest in v1, and neither half of the imbalance is silent.
	EXPECT_EQ(seam.open_group(console_surface::left), console_outcome::ok);
	EXPECT_EQ(seam.open_group(console_surface::left), console_outcome::unbalanced_group);
	EXPECT_EQ(seam.close_group(console_surface::left), console_outcome::ok);
	EXPECT_EQ(seam.close_group(console_surface::left), console_outcome::unbalanced_group);

	// And the shipped table comes back, which is how a user undoes all of it.
	EXPECT_EQ(seam.use_default(console_surface::left), console_outcome::ok);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left).find('L'), std::string_view::npos);
}

// ---------------------------------------------------------------------------
// --- the precedence rule's one bit (#157, §6.10) ---
// ---------------------------------------------------------------------------

// `configured()` is what lets `$PS1` stay the default prompt while the native
// engine supersedes it the moment anybody configures one. The wiring asks it
// once per prompt (`session::refresh_prompt`), so what it answers on a shell
// nobody has touched is the whole of whether today's prompts still work.
TEST(LeshperPromptEngine, ConfiguredIsFalseUntilAVerbRunsAndIsNeverRegained) {
	{
		engine fresh;
		EXPECT_FALSE(fresh.configured());
		// A RENDER IS NOT A CONFIGURATION. The engine arrives holding the default
		// table - it has to, or `render_full` on a fresh one would answer nothing -
		// and rendering it does not make it the user's choice.
		fresh.render_full(quiet());
		EXPECT_FALSE(fresh.configured());
		EXPECT_FALSE(fresh.output(surface_id::left).empty());
	}
	{
		engine one;
		one.clear(surface_id::left);
		EXPECT_TRUE(one.configured());
	}
	{
		engine one;
		// The verb that puts the shipped table back is STILL a configuration: a
		// user asking for the native default is not asking for `$PS1`.
		one.use_default(surface_id::left);
		EXPECT_TRUE(one.configured());
	}
	{
		engine one;
		one.add_literal(surface_id::left, "x");
		EXPECT_TRUE(one.configured());
	}
	{
		engine one;
		EXPECT_TRUE(one.add_module(surface_id::left, "path", ""));
		EXPECT_TRUE(one.configured());
	}
	{
		engine one;
		EXPECT_TRUE(one.open_group(surface_id::left));
		EXPECT_TRUE(one.configured());
		EXPECT_TRUE(one.close_group(surface_id::left));
		EXPECT_TRUE(one.configured());
	}
	{
		// A VERB THAT FAILED CONFIGURED NOTHING. An `add_module` for a name nobody
		// registered changed no placement, so the prompt is still the one the user
		// had - and that is `$PS1`.
		engine one;
		EXPECT_FALSE(one.add_module(surface_id::left, "nobody_registered_this", ""));
		EXPECT_FALSE(one.configured());
		EXPECT_FALSE(one.close_group(surface_id::left));
		EXPECT_FALSE(one.configured());
	}
	{
		// NEVER REGAINED. There is no un-configure, on any path.
		engine one;
		one.add_literal(surface_id::left, "x");
		one.clear(surface_id::left);
		one.use_default(surface_id::left);
		one.clear(surface_id::continuation);
		EXPECT_TRUE(one.configured());
	}
	{
		// A REFUSED TEMPLATE IS NOT A CONFIGURATION EITHER, and it is the one verb
		// where the distinction is visible to a user: a typo in an rc file must not
		// take `$PS1` away and leave nothing in its place.
		engine one;
		std::string error;
		EXPECT_FALSE(one.set_template(surface_id::left, "{gti}", error));
		EXPECT_FALSE(one.configured());
		EXPECT_TRUE(one.set_template(surface_id::left, "{git}", error));
		EXPECT_TRUE(one.configured());
	}
}

// ---------------------------------------------------------------------------
// --- the template language (#157, §6.10) ---
// ---------------------------------------------------------------------------
//
// WHAT IS NOT HERE, AND WHY: the grammar. Every structural rule, every refusal
// and every byte offset is asserted by the COMPILER, in prompt.h's selftests,
// through the same `scan_template` these tests drive - one walk at two
// evaluation times, and the cheaper one is checked at build time. What is left
// for a running test is what a constant expression cannot see: the NODES the
// walk builds and the bytes they render, the wording of the sentences, the
// atomicity of the swap, and everything that crosses the ABI.

// A template set on the left surface and rendered. The whole omission table is
// checked through this.
std::string set_and_render(engine& which, std::string_view text, const prompt::state& facts) {
	std::string error;
	EXPECT_TRUE(which.set_template(surface_id::left, text, error)) << text << ": " << error;
	which.render_full(facts);
	return std::string{which.output(surface_id::left)};
}

// The facts a template test wants: a variable table, and a failed last command
// so `status` has something to say.
prompt::state loud() {
	prompt::state facts = quiet();
	facts.getvar = &fake_getvar;
	facts.status = 2;
	return facts;
}

TEST(LeshperPromptTemplate, EveryRowOfTheOmissionTableRenders) {
	engine which;
	const prompt::state facts = loud();

	// prmt's table, row for row: an empty slot is the default, and a trailing
	// colon is legal.
	EXPECT_EQ(set_and_render(which, "{path}", facts), "~/src");
	EXPECT_EQ(set_and_render(which, "{path:}", facts), "~/src");
	EXPECT_EQ(set_and_render(which, "{path:cyan}", facts), "\x1b[36m~/src\x1b[0m");
	EXPECT_EQ(set_and_render(which, "{env::USER}", facts), "dana");
	EXPECT_EQ(set_and_render(which, "{status:red::[:]}", facts), "\x1b[31m[2]\x1b[0m");
	EXPECT_EQ(set_and_render(which, "{status::::!}", facts), "2!");
	EXPECT_EQ(set_and_render(which, "{status:magenta::on :}", facts), "\x1b[35mon 2\x1b[0m");
	EXPECT_EQ(set_and_render(which, "{path}> ", facts), "~/src> ");

	// AND THE AFFIXES VANISH WITH THE MODULE, which is the whole reason they are
	// slots rather than adjacent literal runs: the same template, one fact
	// different, and the brackets are gone with the number.
	EXPECT_EQ(set_and_render(which, "{status:red::[:]}", quiet()), "");
	EXPECT_EQ(set_and_render(which, "{status::::!}", quiet()), "");

	// A free literal run is unconditional and vanishes with nothing - binding is
	// explicit grouping, never adjacency (§6.10).
	EXPECT_EQ(set_and_render(which, "[{status}]", quiet()), "[]");
}

TEST(LeshperPromptTemplate, TheStandaloneLiteralCarriesItsTextInTheAffixSlots) {
	engine which;

	EXPECT_EQ(set_and_render(which, "{literal:blue::your mother}", quiet()),
	          "\x1b[34myour mother\x1b[0m");
	// Both affixes, in order, with nothing invented between them.
	EXPECT_EQ(set_and_render(which, "{literal:blue::hi :there}", quiet()),
	          "\x1b[34mhi there\x1b[0m");
	// Unstyled, and then the same bytes written bare: one literal node either way.
	EXPECT_EQ(set_and_render(which, "{literal:::plain}", quiet()), "plain");
	EXPECT_EQ(set_and_render(which, "plain", quiet()), "plain");

	std::string error;
	// THE TYPE SLOT IS NOT WHERE THE TEXT GOES. Slot 3 is always the type and
	// `literal` has none, so the spelling that looks like it ought to work is the
	// refusal that matters most.
	EXPECT_FALSE(which.set_template(surface_id::left, "{literal::x}", error));
	EXPECT_EQ(error, "literal takes no type at byte 10");
	EXPECT_FALSE(which.set_template(surface_id::left, "{literal:blue:x:hi}", error));
	EXPECT_EQ(error, "literal takes no type at byte 14");

	// And a literal with no text at all is not an empty literal, it is a mistake.
	EXPECT_FALSE(which.set_template(surface_id::left, "{literal}", error));
	EXPECT_EQ(error, "literal needs text at byte 1");
	EXPECT_FALSE(which.set_template(surface_id::left, "{literal:blue}", error));
	EXPECT_EQ(error, "literal needs text at byte 1");
}

TEST(LeshperPromptTemplate, AStyledLiteralIsGrammarAndDoesNotVote) {
	engine which;

	// `git` says nothing here (`fs_allowed` is false) and the group has no other
	// module, so the whole group goes and the styled literal with it. Had the
	// literal voted, this would paint `on ` in a session that is in no repository
	// at all.
	EXPECT_EQ(set_and_render(which, "({literal:dim::on} {git})", quiet()), "");

	// The same span at top level, where nothing votes on anything: it paints, and
	// it puts the pen back at its own end rather than leaking it into what
	// follows.
	EXPECT_EQ(set_and_render(which, "{literal:dim::on} x", quiet()), "\x1b[2mon\x1b[0m x");
}

TEST(LeshperPromptTemplate, TheEscapesReachTheBytes) {
	engine which;
	const prompt::state facts = loud();

	// `\:` inside a type slot is the escape's whole purpose: a variable whose name
	// contains a colon is unreachable without it.
	EXPECT_EQ(set_and_render(which, "{env::A\\:B}", facts), "escaped");
	EXPECT_EQ(set_and_render(which, "\\{path\\}", facts), "{path}");
	EXPECT_EQ(set_and_render(which, "\\(not a group\\)", facts), "(not a group)");
	EXPECT_EQ(set_and_render(which, "a\\nb\\tc", facts), "a\nb\tc");
	EXPECT_EQ(set_and_render(which, "c:\\\\d", facts), "c:\\d");
	EXPECT_EQ(set_and_render(which, "{literal:::\\(hi\\)}", facts), "(hi)");

	// A bare `}` means nothing outside a placement and needs no escape; a `)` is
	// structural everywhere and does.
	EXPECT_EQ(set_and_render(which, "}x", facts), "}x");
}

TEST(LeshperPromptTemplate, EveryRefusalIsOneSentenceWithTheByteItPointsAt) {
	engine which;
	std::string error;

	const auto refuses = [&](std::string_view text, std::string_view said) {
		error.clear();
		EXPECT_FALSE(which.set_template(surface_id::left, text, error)) << text;
		EXPECT_EQ(error, said) << text;
	};

	refuses("{path}{gti}", "unknown module 'gti' at byte 7");
	refuses("{path", "unclosed '{' at byte 0");
	refuses("( x", "unclosed '(' at byte 0");
	refuses("{path} x)", "unbalanced ')' at byte 8");
	refuses("{path:blod}", "bad style 'blod' at byte 6");
	refuses("{path:cyan.blod}", "bad style 'blod' at byte 11");
	refuses("{a:b:c:d:e:f}", "too many fields at byte 10");
	refuses("{env}", "env needs a variable name at byte 1");
	refuses("{path::short}", "path takes no argument at byte 7");
	refuses("{}", "a placement needs a module name at byte 1");
	refuses("a\\qb", "unknown escape '\\q' at byte 1");
}

TEST(LeshperPromptTemplate, ARefusedTemplateLeavesEverythingExactlyAsItWas) {
	engine which;
	std::string error;

	ASSERT_TRUE(which.set_template(surface_id::left, "{path:cyan}> ", error));
	which.render_full(loud());
	const std::string before{which.output(surface_id::left)};
	const std::string remembered{which.template_text(surface_id::left)};
	ASSERT_FALSE(before.empty());

	// A template that gets a long way in before it fails - a placement, a group,
	// a literal - so that a builder mutating the surface as it walked would leave
	// visible wreckage rather than none.
	EXPECT_FALSE(which.set_template(surface_id::left, "{path}( on {gti}) {status}", error));
	EXPECT_EQ(error, "unknown module 'gti' at byte 12");

	which.render_full(loud());
	EXPECT_EQ(which.output(surface_id::left), before);
	EXPECT_EQ(which.template_text(surface_id::left), remembered);
}

TEST(LeshperPromptTemplate, TheDefaultTableAndItsTemplateAgree) {
	// THE SHIPPED PROMPT, TWICE OVER: the `constexpr` table an untouched engine
	// holds, and the string `use_default` remembers for it. That the two render
	// the same bytes is what makes `prompt` printing `{path}> ` on a fresh shell a
	// true statement rather than a plausible one.
	engine table_side;
	engine template_side;

	std::string error;
	ASSERT_TRUE(template_side.set_template(surface_id::left, prompt::kDefaultLeftTemplate, error));
	ASSERT_TRUE(template_side.set_template(surface_id::continuation,
	                                       prompt::kDefaultContinuationTemplate, error));

	prompt::state elsewhere = quiet();
	elsewhere.pwd = "/etc";
	elsewhere.status = 130;

	for (const prompt::state& facts : {quiet(), elsewhere}) {
		table_side.render_full(facts);
		template_side.render_full(facts);
		EXPECT_EQ(table_side.output(surface_id::left), template_side.output(surface_id::left));
		EXPECT_EQ(table_side.output(surface_id::continuation),
		          template_side.output(surface_id::continuation));
	}

	EXPECT_EQ(table_side.template_text(surface_id::left), "{path}> ");
	EXPECT_EQ(table_side.template_text(surface_id::continuation), "> ");
}

TEST(LeshperPromptTemplate, TheRememberedTextIsTheSourceOrNothingAtAll) {
	engine which;

	EXPECT_EQ(which.template_text(surface_id::left), "{path}> ");
	EXPECT_EQ(which.template_text(surface_id::continuation), "> ");

	which.clear(surface_id::left);
	EXPECT_EQ(which.template_text(surface_id::left), "");

	std::string error;
	ASSERT_TRUE(which.set_template(surface_id::left, "{path:cyan}$ ", error));
	EXPECT_EQ(which.template_text(surface_id::left), "{path:cyan}$ ");

	// AN ASSEMBLY VERB HAS NO TEMPLATE STRING, so it says so rather than leaving
	// a stale one behind that no longer describes the prompt.
	which.add_literal(surface_id::left, "!");
	EXPECT_EQ(which.template_text(surface_id::left), "");

	ASSERT_TRUE(which.set_template(surface_id::left, "{path}> ", error));
	EXPECT_TRUE(which.add_module(surface_id::left, "status", ""));
	EXPECT_EQ(which.template_text(surface_id::left), "");

	which.use_default(surface_id::left);
	EXPECT_EQ(which.template_text(surface_id::left), "{path}> ");
}

TEST(LeshperPromptTemplate, TheValidatorsBuiltInTableIsTheRegistrys) {
	// The `constexpr` validator resolves names against a table in the header; the
	// engine resolves them against the registry its constructor filled. Two lists
	// that must not drift, and this is the only place they can be compared.
	engine which;
	std::vector<std::string> names;
	which.module_names(names);

	ASSERT_EQ(names.size(), std::size(prompt::kBuiltinModules));
	for (std::size_t i = 0; i < names.size(); ++i)
		EXPECT_EQ(names[i], prompt::kBuiltinModules[i].name) << i;
}

TEST(LeshperPromptTemplate, AModuleTheAbiRegisteredOwnsItsOwnArgument) {
	abi_probe probe;
	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;
	ASSERT_EQ(lesh_prompt_module_register(&registry, "probe", &abi_module, &probe), LESH_OK);

	// The grammar has nothing to say about a type slot it did not define: the
	// module parses its own argument and the parser's job is to hand it over.
	EXPECT_EQ(set_and_render(which, "{probe::whatever}", quiet()), "<whatever>");
	EXPECT_EQ(probe.arg, "whatever");

	// A REPLACED BUILT-IN IS THE SAME CASE. `path` takes no argument only while
	// `path` is the built-in; a user who registered their own owns its grammar,
	// which is why the rule is keyed on what the name resolves to.
	counter mine;
	mine.label = "p";
	mine.constant = true;
	std::string error;
	EXPECT_FALSE(which.set_template(surface_id::left, "{path::short}", error));
	ASSERT_EQ(which.register_module("path", &counting_module, &mine), LESH_OK);
	EXPECT_TRUE(which.set_template(surface_id::left, "{path::short}", error)) << error;

	// And before any registration at all the same template is a refusal.
	engine fresh;
	EXPECT_FALSE(fresh.set_template(surface_id::left, "{probe::whatever}", error));
	EXPECT_EQ(error, "unknown module 'probe' at byte 1");
}

TEST(LeshperPromptEngine, AddStyleReachesTheGroupsResetFlag) {
	engine which;
	which.clear(surface_id::left);
	which.clear(surface_id::continuation);

	ASSERT_TRUE(which.open_group(surface_id::left));
	EXPECT_TRUE(which.add_style(surface_id::left, "red.bold"));
	which.add_literal(surface_id::left, "[");
	ASSERT_TRUE(which.add_module(surface_id::left, "status", ""));
	which.add_literal(surface_id::left, "]");
	ASSERT_TRUE(which.close_group(surface_id::left));
	which.add_literal(surface_id::left, "$");

	// THE FLAG THIS VERB FINALLY SETS: the group ends with a reset, so the `$`
	// after it is not red.
	prompt::state failed = quiet();
	failed.status = 7;
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), "\x1b[1;31m[7]\x1b[0m$");

	// And the style vanishes with the module, like any other decoration.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "$");

	// A spec that will not parse places nothing.
	EXPECT_FALSE(which.add_style(surface_id::left, "blod"));
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "$");
}

TEST(LeshperPromptAbi, TheTemplateVerbsTravelAndTheMessageFollowsTheCopyOutConvention) {
	engine which;
	lesh_registry registry;
	registry.prompt_engine = &which;

	constexpr std::string_view kTemplate = "{path}> ";
	std::size_t length = 7;
	ASSERT_EQ(lesh_prompt_set(&registry, LESH_PROMPT_LEFT, kTemplate.data(), kTemplate.size(),
	                          nullptr, 0, &length),
	          LESH_OK);
	// Zero, and written even though there was no message: that is how a caller
	// sizing the message first learns there is none.
	EXPECT_EQ(length, 0u);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "~/src> ");

	char buffer[64];
	std::size_t out_length = 0;
	ASSERT_EQ(lesh_prompt_text(&registry, LESH_PROMPT_LEFT, buffer, sizeof buffer, &out_length),
	          LESH_OK);
	EXPECT_EQ(std::string(buffer, out_length), kTemplate);

	// THE REFUSAL, ASKED FOR TWICE: the length first with no buffer at all, then
	// the sentence with room for it.
	constexpr std::string_view kBad = "{gti}";
	std::size_t needed = 0;
	EXPECT_EQ(lesh_prompt_set(&registry, LESH_PROMPT_LEFT, kBad.data(), kBad.size(), nullptr, 0,
	                          &needed),
	          LESH_ERR_TOOSMALL);
	ASSERT_GT(needed, 0u);
	std::string message;
	message.resize(needed);
	EXPECT_EQ(lesh_prompt_set(&registry, LESH_PROMPT_LEFT, kBad.data(), kBad.size(),
	                          message.data(), message.size(), &needed),
	          1);
	EXPECT_EQ(message, "unknown module 'gti' at byte 1");

	// And the prompt that was standing is still standing, twice refused.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "~/src> ");
	ASSERT_EQ(lesh_prompt_text(&registry, LESH_PROMPT_LEFT, buffer, sizeof buffer, &out_length),
	          LESH_OK);
	EXPECT_EQ(std::string(buffer, out_length), kTemplate);

	// The style verb, and its own domain status for a spec that will not parse.
	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_group_open(&registry, LESH_PROMPT_LEFT), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_style(&registry, LESH_PROMPT_LEFT, "cyan", 4), LESH_OK);
	ASSERT_EQ(lesh_prompt_add_module(&registry, LESH_PROMPT_LEFT, "path", nullptr), LESH_OK);
	ASSERT_EQ(lesh_prompt_group_close(&registry, LESH_PROMPT_LEFT), LESH_OK);
	EXPECT_EQ(lesh_prompt_add_style(&registry, LESH_PROMPT_LEFT, "blod", 4), 1);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36m~/src\x1b[0m");

	// A surface assembled verb by verb has no template string.
	ASSERT_EQ(lesh_prompt_text(&registry, LESH_PROMPT_LEFT, buffer, sizeof buffer, &out_length),
	          LESH_OK);
	EXPECT_EQ(out_length, 0u);

	// The argument errors, and the one that is a missing engine rather than a
	// malformed call.
	lesh_registry bare;
	ASSERT_EQ(bare.prompt_engine, nullptr);
	EXPECT_EQ(lesh_prompt_set(&bare, LESH_PROMPT_LEFT, "x", 1, nullptr, 0, &needed),
	          LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_text(&bare, LESH_PROMPT_LEFT, buffer, sizeof buffer, &out_length),
	          LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_add_style(&bare, LESH_PROMPT_LEFT, "cyan", 4), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_set(nullptr, LESH_PROMPT_LEFT, "x", 1, nullptr, 0, &needed),
	          LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_set(&registry, 7u, "x", 1, nullptr, 0, &needed), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_set(&registry, LESH_PROMPT_LEFT, nullptr, 3, nullptr, 0, &needed),
	          LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_set(&registry, LESH_PROMPT_LEFT, "x", 1, nullptr, 0, nullptr),
	          LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_add_style(&registry, 7u, "cyan", 4), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_add_style(&registry, LESH_PROMPT_LEFT, nullptr, 4), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_text(&registry, 7u, buffer, sizeof buffer, &out_length), LESH_ERR_INVAL);
}

// --- groups, against a real repository -------------------------------------

TEST_F(LeshperPromptGit, ATemplatesGroupVanishesWithItsModule) {
	const std::string repo = make_repo("templated", "ref: refs/heads/topic\n");
	write_text(repo + "/.git/refs/heads/topic", sha40('a') + "\n");
	const std::string bare = at("templated_not_a_repo");
	make_dirs(bare);

	engine which;
	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.home = std::string_view{};
	facts.fs_allowed = true;

	prompt::state elsewhere = facts;
	elsewhere.pwd = bare;

	// §6.10's own example, spelled in the language for the first time.
	EXPECT_EQ(set_and_render(which, "{path}( on {git})> ", facts), repo + " on topic> ");
	which.render_full(elsewhere);
	EXPECT_EQ(which.output(surface_id::left), bare + "> ");

	// The same thing said with slots instead of a group, which is what the
	// desugaring means: one styled span, the affixes inside it, the style
	// covering both.
	EXPECT_EQ(set_and_render(which, "{path}{git:magenta:: on :}> ", facts),
	          repo + "\x1b[35m on topic\x1b[0m> ");
	which.render_full(elsewhere);
	EXPECT_EQ(which.output(surface_id::left), bare + "> ");
}

TEST_F(LeshperPromptGit, GroupsNestAndTheInnerOneRendersOnlyIfTheOuterSurvived) {
	const std::string repo = make_repo("nested_template", "ref: refs/heads/deep\n");
	write_text(repo + "/.git/refs/heads/deep", sha40('b') + "\n");
	const std::string bare = at("nested_not_a_repo");
	make_dirs(bare);

	engine which;
	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.home = std::string_view{};
	facts.fs_allowed = true;
	facts.status = 3;

	// NESTING IS THE TEMPLATE LANGUAGE'S - the ABI's verb stream still refuses it,
	// having no way to say which group a close belongs to - and the vote is
	// unchanged by it: only DIRECT module children vote, exactly as a `seg` nested
	// in a `seg` does not vote at compile time. So this outer group lives or dies
	// on `git`, and the inner one runs only if it lived.
	EXPECT_EQ(set_and_render(which, "( on {git} ([{status}]))", facts), " on deep [3]");

	// Outside a repository the whole thing goes, inner group included.
	prompt::state elsewhere = facts;
	elsewhere.pwd = bare;
	which.render_full(elsewhere);
	EXPECT_EQ(which.output(surface_id::left), "");

	// The inner module alone cannot save the outer group, which is the other half
	// of the same rule and the half that would surprise someone: a group is not a
	// module and does not vote for its parent.
	EXPECT_EQ(set_and_render(which, "( on ({status}))", elsewhere), "");
}

TEST_F(LeshperPromptGit, PuttingAColourOnAPlacementDoesNotChangeHowItVotes) {
	// THE TRAP THE MANUAL SMOKE CAUGHT, and the reason a desugared placement is
	// stamped a module rather than a group: `( on {git})` works, so
	// `( on {git:magenta})` has to work, or adding one word to a prompt that was
	// fine would silently empty it - a group with nothing in it that can vote can
	// never be shown, and nothing about the spelling says so.
	const std::string repo = make_repo("coloured", "ref: refs/heads/tinted\n");
	write_text(repo + "/.git/refs/heads/tinted", sha40('c') + "\n");
	const std::string bare = at("coloured_not_a_repo");
	make_dirs(bare);

	engine which;
	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.home = std::string_view{};
	facts.fs_allowed = true;

	EXPECT_EQ(set_and_render(which, "{path}( on {git})> ", facts), repo + " on tinted> ");
	EXPECT_EQ(set_and_render(which, "{path}( on {git:magenta})> ", facts),
	          repo + " on \x1b[35mtinted\x1b[0m> ");
	// With its affixes inside the group as well - a placement carrying a style and
	// two affixes is still one placement to the group around it.
	EXPECT_EQ(set_and_render(which, "{path}({git:magenta:: on :!})> ", facts),
	          repo + "\x1b[35m on tinted!\x1b[0m> ");

	// And all three vanish together outside a repository.
	prompt::state elsewhere = facts;
	elsewhere.pwd = bare;
	EXPECT_EQ(set_and_render(which, "{path}( on {git})> ", elsewhere), bare + "> ");
	EXPECT_EQ(set_and_render(which, "{path}( on {git:magenta})> ", elsewhere), bare + "> ");
	EXPECT_EQ(set_and_render(which, "{path}({git:magenta:: on :!})> ", elsewhere), bare + "> ");
}

} // namespace
