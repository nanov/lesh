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

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

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

	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36m~/src\x1b[0m$ ");
	EXPECT_EQ(which.output(surface_id::continuation), "> ");

	which.clear(surface_id::left);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "");

	which.use_default(surface_id::left);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36m~/src\x1b[0m$ ");

	prompt::state failed = quiet();
	failed.status = 2;
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36m~/src\x1b[0m\x1b[31m [2]\x1b[0m$ ");
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
