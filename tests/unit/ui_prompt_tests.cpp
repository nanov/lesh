// The prompt engine's runtime half (#157, spec §6.10).
//
// WHAT IS NOT HERE, AND WHY. The composer's omission rules, the default table's
// exact bytes and the SGR round trip are asserted by the COMPILER, in prompt.h's
// `selftest` namespace - a failure there is a build failure, which is the right
// outcome for "the prompt stopped omitting". What is left for a running test is
// everything a constant expression cannot see: how many times a module was
// INVOKED, which is the whole of recalculation-by-cause, and everything that
// crosses the C ABI.

#include "leshnici/leshnici.h"
#include "leshper/abi.h"
#include "leshper/registry.h"
#include "ui/prompt/abi.h"
#include "ui/prompt/prompt.h"
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

using lesh::ui::prompt::element_status;
using lesh::ui::prompt::engine;
using lesh::ui::prompt::surface_id;
namespace prompt = lesh::ui::prompt;

// One module, run on its own - through its OWN type-slot grammar, because that
// is now half of what a module is. `parse` then `render`, which is exactly the
// pair a placement makes at set time and at render time.
struct rendered {
	std::string bytes;
	element_status status = element_status::omitted;
	std::uint64_t wake = 0;
};

rendered run_module(const prompt::module& which, std::string_view type,
                    const prompt::state& facts) {
	prompt::params_blob params;
	prompt::parse_error why;
	EXPECT_TRUE(which.parse(type, params, why)) << which.name() << " refused '" << type << "'";

	prompt::sink out;
	const element_status answered = prompt::status_of(which.render(facts, params, out));
	return rendered{std::string{out.bytes()}, answered, out.wake()};
}

// Whether a module refuses a type slot. The WORDING is asserted through
// `set_template`, where a user would actually see it.
bool refuses_type(const prompt::module& which, std::string_view type) {
	prompt::params_blob params;
	prompt::parse_error why;
	return !which.parse(type, params, why);
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

// A module written the way a module is now written: a `typed_module` over a
// STRUCT of params, with its own type-slot grammar.
//
// IT COUNTS BOTH VERBS SEPARATELY, and that is the point of it. `parses` says
// how many times the type slot was interpreted and `calls` how many times bytes
// were produced - so "parsed once at set time, rendered once per prompt" is a
// pair of numbers rather than a claim. The old test double was a bare function
// pointer plus a `void*`, which could not have been asked either question.
struct probe_params {
	prompt::fixed_text<32> tag{};
};

class test_module final : public prompt::typed_module<probe_params> {
public:
	test_module(std::string named, std::string label)
		: _named(std::move(named)), _label(std::move(label)) {}

	mutable int parses = 0;
	mutable int calls = 0;

	std::uint64_t wake = 0;
	bool constant = false;   // when set, the bytes do not change between calls
	bool silent = false;     // when set, it omits - a module with nothing to say

	[[nodiscard]] std::string_view name() const noexcept override { return _named; }

protected:
	// A GRAMMAR OF ITS OWN: anything up to 32 bytes is a tag, longer is refused.
	// Enough to be a real one, small enough to say so in a sentence.
	bool parse(std::string_view type, probe_params& out, prompt::parse_error& err) const override {
		++parses;
		if (!out.tag.assign(type)) {
			err.what = ": tag is too long";
			return false;
		}
		return true;
	}

	int render(const prompt::state&, const probe_params& params,
	           prompt::sink& out) const override {
		++calls;
		if (wake != 0)
			out.wake_in(wake);
		if (silent)
			return prompt::code(element_status::omitted);
		out.append(_label);
		out.append(params.tag.view());
		if (!constant)
			out.append(std::to_string(calls));
		return prompt::code(element_status::ready);
	}

private:
	std::string _named;
	std::string _label;
};

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

TEST(UiPromptModules, PathContractsHomeByComponent) {
	EXPECT_EQ(run_module(prompt::kModulePath, "", quiet()).bytes, "~/src");

	prompt::state facts = quiet();
	facts.pwd = "/home/u";
	EXPECT_EQ(run_module(prompt::kModulePath, "", facts).bytes, "~");

	// A sibling whose name merely starts with the home directory's is not under
	// it. The cheap prefix test would have rendered `~name/x`.
	facts.pwd = "/home/username/x";
	EXPECT_EQ(run_module(prompt::kModulePath, "", facts).bytes, "/home/username/x");

	facts.home = std::string_view{};
	facts.pwd = "/home/u/src";
	EXPECT_EQ(run_module(prompt::kModulePath, "", facts).bytes, "/home/u/src");
}

TEST(UiPromptModules, PathOmitsWithoutAPwd) {
	prompt::state facts;
	EXPECT_EQ(run_module(prompt::kModulePath, "", facts).status, element_status::omitted);
	EXPECT_TRUE(run_module(prompt::kModulePath, "", facts).bytes.empty());
}

// THE FIVE VARIANTS `path` NOW OWNS (#157's ruling, prmt's spellings). The
// compile-time selftests render each against one set of facts; what is here is
// the two questions a constant expression cannot ask - what a variant does to a
// path with no home in it, and what the module says to a spelling it does not
// know.
TEST(UiPromptModules, PathHasFiveVariantsAndRefusesAnySixth) {
	prompt::state facts = quiet();
	facts.pwd = "/home/u/private/github/lesh";

	EXPECT_EQ(run_module(prompt::kModulePath, "", facts).bytes, "~/private/github/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "relative", facts).bytes, "~/private/github/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "r", facts).bytes, "~/private/github/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "absolute", facts).bytes,
	          "/home/u/private/github/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "a", facts).bytes, "/home/u/private/github/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "f", facts).bytes, "/home/u/private/github/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "short", facts).bytes, "lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "s", facts).bytes, "lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "initials", facts).bytes, "~/p/g/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "i", facts).bytes, "~/p/g/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "unvowel", facts).bytes, "~/prvt/gthb/lesh");
	EXPECT_EQ(run_module(prompt::kModulePath, "u", facts).bytes, "~/prvt/gthb/lesh");

	// OUTSIDE HOME THERE IS NO `~` TO KEEP, and the reduction starts at the root:
	// the leading `/` is a separator, not a component, so nothing eats it.
	facts.pwd = "/usr/local/share/doc";
	facts.home = "/home/u";
	EXPECT_EQ(run_module(prompt::kModulePath, "i", facts).bytes, "/u/l/s/doc");
	EXPECT_EQ(run_module(prompt::kModulePath, "u", facts).bytes, "/sr/lcl/shr/doc");
	EXPECT_EQ(run_module(prompt::kModulePath, "s", facts).bytes, "doc");

	// A COMPONENT THAT WOULD VANISH KEEPS ITS FIRST BYTE. `~//x` would read as a
	// mistake; one letter is the smallest honest answer.
	facts.pwd = "/home/u/aeiou/x";
	EXPECT_EQ(run_module(prompt::kModulePath, "u", facts).bytes, "~/a/x");

	// AT HOME THE SHORT FORM IS `~`, not the name of the directory home happens
	// to live in - which falls out of contracting first and shortening after.
	facts.pwd = "/home/u";
	EXPECT_EQ(run_module(prompt::kModulePath, "s", facts).bytes, "~");

	// And a spelling nobody defined is refused, at set time, by `path`.
	EXPECT_TRUE(refuses_type(prompt::kModulePath, "medum"));
	EXPECT_TRUE(refuses_type(prompt::kModulePath, "SHORT"));
}

TEST(UiPromptModules, StatusOmitsOnSuccess) {
	EXPECT_EQ(run_module(prompt::kModuleStatus, "", quiet()).status, element_status::omitted);

	prompt::state facts = quiet();
	facts.status = 127;
	const rendered got = run_module(prompt::kModuleStatus, "", facts);
	EXPECT_EQ(got.status, element_status::ready);
	EXPECT_EQ(got.bytes, "127");

	facts.status = -6;
	EXPECT_EQ(run_module(prompt::kModuleStatus, "", facts).bytes, "-6");

	// `code` is the default spelled out, and the one word that is not a symbol.
	EXPECT_EQ(run_module(prompt::kModuleStatus, "code", facts).bytes, "-6");
}

// THE SYMBOL FORM: the type slot IS the mark, so there is no vocabulary for a
// typo to fall outside of - only a ceiling on how long a mark may be.
TEST(UiPromptModules, StatusShowsASymbolInsteadOfANumber) {
	prompt::state facts = quiet();
	facts.status = 1;

	EXPECT_EQ(run_module(prompt::kModuleStatus, "✗", facts).bytes, "✗");
	EXPECT_EQ(run_module(prompt::kModuleStatus, "FAIL", facts).bytes, "FAIL");

	// And it still omits on success, which is the whole reason it is one module
	// with two forms rather than two modules.
	facts.status = 0;
	EXPECT_EQ(run_module(prompt::kModuleStatus, "✗", facts).status, element_status::omitted);

	EXPECT_TRUE(refuses_type(prompt::kModuleStatus, std::string(17, 'x')));
	EXPECT_FALSE(refuses_type(prompt::kModuleStatus, std::string(16, 'x')));
}

TEST(UiPromptModules, JobsOmitsWhenThereAreNone) {
	EXPECT_EQ(run_module(prompt::kModuleJobs, "", quiet()).status, element_status::omitted);

	prompt::state facts = quiet();
	facts.jobs = 12;
	EXPECT_EQ(run_module(prompt::kModuleJobs, "", facts).bytes, "12");
}

TEST(UiPromptModules, ModeIsWhateverTheKeymapDeclared) {
	EXPECT_EQ(run_module(prompt::kModuleMode, "", quiet()).status, element_status::omitted);

	prompt::state facts = quiet();
	facts.mode = "NORMAL";
	EXPECT_EQ(run_module(prompt::kModuleMode, "", facts).bytes, "NORMAL");
}

// THE PARAMETERLESS MODULES REFUSE A TYPE SLOT, all three with one wording,
// because a `{jobs::x}` that was quietly ignored is a user who believes they
// asked for something. (leshnici's `git` is the fourth, and says it the same
// way through the same `refuse_any_type` - see leshnici_git_tests.cpp.)
TEST(UiPromptModules, TheParameterlessModulesTakeNoType) {
	const prompt::module* const parameterless[] = {
		&prompt::kModuleJobs,
		&prompt::kModuleMode,
		&prompt::kModuleDuration,
	};
	for (const prompt::module* which : parameterless) {
		EXPECT_FALSE(refuses_type(*which, "")) << which->name();
		EXPECT_TRUE(refuses_type(*which, "x")) << which->name();
	}
}

TEST(UiPromptModules, TimeHasFourFormsAndACadenceForEach) {
	prompt::state facts = quiet();
	facts.hours = 9;
	facts.minutes = 5;
	facts.seconds = 3;
	facts.tick = 37;

	// THE DEFAULT IS HH:MM (prmt's `24h`, owner's ruling on #157), and the form
	// that shows seconds asks for them by name.
	const rendered minutes = run_module(prompt::kModuleTime, "", facts);
	EXPECT_EQ(minutes.status, element_status::ready);
	EXPECT_EQ(minutes.bytes, "09:05");
	EXPECT_EQ(run_module(prompt::kModuleTime, "24h", facts).bytes, "09:05");
	EXPECT_EQ(run_module(prompt::kModuleTime, "24hs", facts).bytes, "09:05:03");

	// Twelve-hour, with no am/pm suffix: midnight is 12, noon is 12, 13 is 1.
	facts.hours = 0;
	EXPECT_EQ(run_module(prompt::kModuleTime, "12h", facts).bytes, "12:05");
	facts.hours = 12;
	EXPECT_EQ(run_module(prompt::kModuleTime, "12hs", facts).bytes, "12:05:03");
	facts.hours = 13;
	EXPECT_EQ(run_module(prompt::kModuleTime, "12h", facts).bytes, "01:05");

	// THE CADENCE FOLLOWS THE FORM, and both are DERIVED from the tick and stored
	// nowhere - which is what makes a parked clock re-arm from the fire rather
	// than catch up. The next second on the 10 ms grid is `100 - tick % 100`; the
	// next minute is that plus the whole seconds left in this one.
	EXPECT_EQ(run_module(prompt::kModuleTime, "24hs", facts).wake, 63u);
	EXPECT_EQ(run_module(prompt::kModuleTime, "24h", facts).wake, 56u * 100 + 63);

	facts.tick = 100;
	EXPECT_EQ(run_module(prompt::kModuleTime, "24hs", facts).wake, 100u);
	facts.tick = 199;
	EXPECT_EQ(run_module(prompt::kModuleTime, "24hs", facts).wake, 1u);

	// The last second of a minute wants the next second, which IS the next minute.
	facts.seconds = 59;
	facts.tick = 100;
	EXPECT_EQ(run_module(prompt::kModuleTime, "24h", facts).wake, 100u);

	EXPECT_TRUE(refuses_type(prompt::kModuleTime, "13h"));
}

TEST(UiPromptModules, DurationHasAFloorAndThreeFormats) {
	prompt::state facts = quiet();

	facts.duration_ms = 1999;
	EXPECT_EQ(run_module(prompt::kModuleDuration, "", facts).status, element_status::omitted);

	facts.duration_ms = 2000;
	EXPECT_EQ(run_module(prompt::kModuleDuration, "", facts).bytes, "2s");

	facts.duration_ms = 59'999;
	EXPECT_EQ(run_module(prompt::kModuleDuration, "", facts).bytes, "59s");

	facts.duration_ms = 90'000;
	EXPECT_EQ(run_module(prompt::kModuleDuration, "", facts).bytes, "1m30s");

	facts.duration_ms = 3'600'000;
	EXPECT_EQ(run_module(prompt::kModuleDuration, "", facts).bytes, "1h0m0s");

	facts.duration_ms = 7'384'000;
	EXPECT_EQ(run_module(prompt::kModuleDuration, "", facts).bytes, "2h3m4s");
}

TEST(UiPromptModules, EnvReadsTheVariableItsTypeSlotNames) {
	prompt::state facts = quiet();
	facts.getvar = &fake_getvar;

	EXPECT_EQ(run_module(prompt::kModuleEnv, "USER", facts).bytes, "dana");

	// Set but empty omits: `{env::HOST}@` should vanish rather than render a bare
	// `@`.
	EXPECT_EQ(run_module(prompt::kModuleEnv, "EMPTY", facts).status, element_status::omitted);
	EXPECT_EQ(run_module(prompt::kModuleEnv, "NOPE", facts).status, element_status::omitted);

	// No lookup wired up is an omission, not an error.
	facts.getvar = nullptr;
	EXPECT_EQ(run_module(prompt::kModuleEnv, "USER", facts).status, element_status::omitted);

	// AND A NAMELESS `env` NEVER GETS AS FAR AS A RENDER. There is no default
	// variable to mean, so the placement is refused where it was written - which
	// is the difference between a typo and a segment that silently disappeared.
	EXPECT_TRUE(refuses_type(prompt::kModuleEnv, ""));
	EXPECT_FALSE(refuses_type(prompt::kModuleEnv, std::string(64, 'V')));
	EXPECT_TRUE(refuses_type(prompt::kModuleEnv, std::string(65, 'V')));
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST(UiPromptEngine, RegistersTheBuiltInsAndListsThemSorted) {
	engine which;

	std::vector<std::string> names;
	which.module_names(names);
	const std::vector<std::string> expected{"duration", "env",    "jobs", "mode",
	                                        "path",     "status", "time"};
	EXPECT_EQ(names, expected);

	EXPECT_TRUE(which.module_exists("path"));
	// SEVEN, AND `git` IS NOT ONE OF THEM (#163). It is leshnici's, and a bare
	// engine has never heard of it - which is what makes the wiring site's
	// `install_prompt_modules` the only way a prompt gets one.
	EXPECT_FALSE(which.module_exists("git"));
	EXPECT_FALSE(which.module_exists("weather"));
}

TEST(UiPromptEngine, RegistrationReplacesAndValidatesTheName) {
	engine which;
	test_module first{"probe", "first"};
	test_module second{"probe", "second"};
	first.constant = true;
	second.constant = true;

	EXPECT_EQ(which.register_module("probe", &first), LESH_OK);
	EXPECT_EQ(which.register_module("probe", &second), LESH_OK);

	EXPECT_EQ(which.register_module("Probe", &first), LESH_ERR_INVAL);
	EXPECT_EQ(which.register_module("two-words", &first), LESH_ERR_INVAL);
	EXPECT_EQ(which.register_module("", &first), LESH_ERR_INVAL);
	EXPECT_EQ(which.register_module("probe", nullptr), LESH_ERR_INVAL);

	which.clear(surface_id::left);
	which.add_module(surface_id::left, "probe", "");
	which.render_full(quiet());

	// The second registration is the one that runs: re-sourcing an rc file is
	// idempotent (#101), not cumulative.
	EXPECT_EQ(first.calls, 0);
	EXPECT_EQ(second.calls, 1);
}

// PARSED ONCE AT SET TIME, RENDERED ONCE PER PROMPT, and the two counters say so
// separately. This is the property the typed-module split exists for: the type
// slot is interpreted where a user can be told about it, and the render path
// never looks at a string again.
TEST(UiPromptEngine, AModulesTypeSlotIsParsedOnceAndNotPerRender) {
	engine which;
	test_module probe{"probe", "p"};
	probe.constant = true;
	ASSERT_EQ(which.register_module("probe", &probe), LESH_OK);

	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	ASSERT_TRUE(which.add_module(surface_id::left, "probe", "tag"));

	EXPECT_EQ(probe.parses, 1);
	EXPECT_EQ(probe.calls, 0);

	which.render_full(quiet());
	which.render_full(quiet());
	which.render_full(quiet());

	// Three prompts, three renders - and still ONE parse.
	EXPECT_EQ(probe.parses, 1);
	EXPECT_EQ(probe.calls, 3);
	EXPECT_EQ(which.output(surface_id::left), "ptag");

	// A REFUSED TYPE SLOT PLACES NOTHING, which is `place`'s half of the same
	// atomicity `set_template` promises: the surface is what it was.
	EXPECT_FALSE(which.add_module(surface_id::left, "probe", std::string(33, 'x')));
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "ptag");
}

TEST(UiPromptEngine, DefaultAndClearRoundTrip) {
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

TEST(UiPromptEngine, AddModuleAnswersFalseForAnUnknownName) {
	engine which;
	which.clear(surface_id::left);
	EXPECT_FALSE(which.add_module(surface_id::left, "weather", ""));
	EXPECT_TRUE(which.add_module(surface_id::left, "path", ""));
}

TEST(UiPromptEngine, GroupsDoNotNestAndCloseNeedsAnOpen) {
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

TEST(UiPromptEngine, OneRenderComputesAModuleOncePerParams) {
	engine which;
	test_module probe{"probe", "x"};
	probe.constant = true;

	which.register_module("probe", &probe);
	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	which.add_module(surface_id::left, "probe", "same");
	which.add_literal(surface_id::left, "/");
	which.add_module(surface_id::left, "probe", "same");

	which.render_full(quiet());

	// Two placements, one computation - §6.10's per-prompt (module, params) memo,
	// which is what makes free placement free. THE KEY IS THE PARAMS BYTES, not a
	// string compare: two placements that parsed to the same value are the same
	// question however they were spelled.
	EXPECT_EQ(probe.calls, 1);
	EXPECT_EQ(which.output(surface_id::left), "xsame/xsame");

	// Different params are a different question and are asked.
	probe.calls = 0;
	which.add_module(surface_id::left, "probe", "other");
	which.render_full(quiet());
	EXPECT_EQ(probe.calls, 2);

	// And the memo does not survive the render: a second prompt asks again.
	probe.calls = 0;
	which.render_full(quiet());
	EXPECT_EQ(probe.calls, 2);
}

// THE MEMO'S KEY IS (MODULE, PARAMS) AND BOTH HALVES MATTER. Two modules that
// parsed identical params are still two questions, because the module pointer
// differs; one module with two different params is two questions, because the
// bytes differ. Neither half alone would be enough, and a memo that got this
// wrong would show one segment's bytes under another segment's name.
TEST(UiPromptEngine, TheMemoKeysOnTheModuleAndItsParamsTogether) {
	engine which;
	test_module first{"one", "A"};
	test_module second{"two", "B"};
	first.constant = true;
	second.constant = true;

	ASSERT_EQ(which.register_module("one", &first), LESH_OK);
	ASSERT_EQ(which.register_module("two", &second), LESH_OK);

	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	// The same params bytes, twice each, over two different modules.
	ASSERT_TRUE(which.add_module(surface_id::left, "one", "t"));
	ASSERT_TRUE(which.add_module(surface_id::left, "two", "t"));
	ASSERT_TRUE(which.add_module(surface_id::left, "one", "t"));
	ASSERT_TRUE(which.add_module(surface_id::left, "two", "t"));

	which.render_full(quiet());
	EXPECT_EQ(first.calls, 1);
	EXPECT_EQ(second.calls, 1);
	EXPECT_EQ(which.output(surface_id::left), "AtBtAtBt");
}

// ---------------------------------------------------------------------------
// The tick wheel - recalculation by cause
// ---------------------------------------------------------------------------

TEST(UiPromptEngine, ATickReInvokesOnlyWhatIsDue) {
	test_module early{"early", "a"};
	test_module late{"late", "b"};
	test_module still{"still", "c"};
	early.wake = 2;
	late.wake = 5;
	still.constant = true;   // no wake at all

	engine which;
	which.register_module("early", &early);
	which.register_module("late", &late);
	which.register_module("still", &still);

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

TEST(UiPromptEngine, ATickThatChangesNothingOwesNoWrite) {
	test_module steady{"steady", "tick"};
	steady.constant = true;
	steady.wake = 3;

	engine which;
	which.register_module("steady", &steady);
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

TEST(UiPromptEngine, AStaticPromptArmsNoTimer) {
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

TEST(UiPromptEngine, TheTimeModuleDrivesTheWheel) {
	engine which;
	which.clear(surface_id::left);
	which.clear(surface_id::continuation);
	// THE SECONDS FORM, ASKED FOR BY NAME. The default shows HH:MM and therefore
	// wakes once a minute (owner's ruling on #157, prmt's `24h`); the wheel is
	// easier to watch a second at a time, and the minute cadence is asserted
	// below on the same engine.
	which.add_module(surface_id::left, "time", "24hs");

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

	// AND THE DEFAULT FORM COSTS SIXTY TIMES FEWER WAKEUPS. A prompt showing
	// HH:MM has no business being woken to redraw bytes that did not move -
	// §6.10's "unchanged output produces no write" would have caught the write,
	// but not the wakeup, and the wakeup is what costs a laptop its battery.
	which.clear(surface_id::left);
	which.add_module(surface_id::left, "time", "");
	which.render_full(facts);
	EXPECT_EQ(which.output(surface_id::left), "01:02");
	// Absolute, like every other deadline: the tick it was computed at, plus the
	// rest of this second, plus the whole seconds left in the minute.
	EXPECT_EQ(which.next_wake(), facts.tick + 100u + (59u - facts.seconds) * 100u);
}

// ---------------------------------------------------------------------------
// The C ABI
// ---------------------------------------------------------------------------

TEST(UiPromptAbi, EveryVerbNeedsAnEngineOnTheRegistry) {
	lesh_registry bare;
	ASSERT_EQ(bare.host_prompt, nullptr);

	std::int32_t exists = 0;
	EXPECT_EQ(lesh_prompt_module_register(&bare, "x", &abi_module, nullptr), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_module_exists(&bare, "x", &exists), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_clear(&bare, LESH_PROMPT_LEFT), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_use_default(&bare, LESH_PROMPT_LEFT), LESH_ERR_NOTFOUND);
	const lesh_prompt_placement one[] = { { .module = "path" } };
	EXPECT_EQ(lesh_prompt_set_placements(&bare, LESH_PROMPT_LEFT, one, 1), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_module_register_with(&bare, "x", &abi_module, nullptr, {}),
	          LESH_ERR_NOTFOUND);

	// A null registry is a malformed argument, which is a different answer.
	EXPECT_EQ(lesh_prompt_clear(nullptr, LESH_PROMPT_LEFT), LESH_ERR_INVAL);
}

TEST(UiPromptAbi, RejectsBadSurfacesAndBadNames) {
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	const lesh_prompt_placement path[] = { { .module = "path" } };
	EXPECT_EQ(lesh_prompt_clear(&registry, 7u), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_set_placements(&registry, 7u, path, 1), LESH_ERR_INVAL);

	const lesh_prompt_placement weather[] = { { .module = "weather" } };
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, weather, 1),
	          LESH_ERR_NOTFOUND);

	EXPECT_EQ(lesh_prompt_module_register(&registry, "Bad-Name", &abi_module, nullptr),
	          LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_module_register(&registry, "ok_name", nullptr, nullptr),
	          LESH_ERR_INVAL);

	// AN ITEM WITH NEITHER `module` NOR `children` (#157, owner's ruling) is a
	// structural error in the tree, LESH_ERR_INVAL - it is not a placement and
	// not a group. NULL and "" answer alike.
	const lesh_prompt_placement neither[] = { {} };
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, neither, 1),
	          LESH_ERR_INVAL);
	const lesh_prompt_placement empty_module[] = { { .module = "" } };
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, empty_module, 1),
	          LESH_ERR_INVAL);

	// AN ITEM WITH BOTH is the other structural error - a caller that meant one
	// and wrote both.
	const lesh_prompt_placement leaf[] = { { .module = "path" } };
	const lesh_prompt_placement both[] = {
		{ .module = "path", .children = leaf, .child_count = 1 },
	};
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, both, 1), LESH_ERR_INVAL);

	// A GROUP WITH NO CHILDREN is the third: a zero `child_count` answers exactly
	// as a NULL `children` does, whether or not the pointer is set.
	const lesh_prompt_placement empty_group[] = { { .children = leaf, .child_count = 0 } };
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, empty_group, 1),
	          LESH_ERR_INVAL);

	// "no module" IS SPELLED "literal", exactly as the template spells it - a
	// literal is a placement, not an omission, and the keyword is how this verb
	// says so. It needs bytes to paint, the same refusal `{literal}` gets from the
	// template parser - and the array verb has ONE status, 1, for every
	// style/type/literal refusal (abi.h says why).
	const lesh_prompt_placement bare_literal[] = { { .module = "literal" } };
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, bare_literal, 1), 1);
	const lesh_prompt_placement painted_literal[] = {
		{ .module = "literal", .options = { .prefix = "x" } },
	};
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, painted_literal, 1),
	          LESH_OK);

	// And it has no type slot, the same refusal `{literal::x}` gets - text alone
	// does not excuse a type.
	const lesh_prompt_placement typed_literal[] = {
		{ .module = "literal", .options = { .type = "x", .prefix = "hi" } },
	};
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, typed_literal, 1), 1);
}

TEST(UiPromptAbi, CountZeroIsTheEmptySurfaceAndItemsGoesUnread) {
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	const lesh_prompt_placement items[] = { { .module = "path" } };
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);
	which.render_full(quiet());
	ASSERT_FALSE(which.output(surface_id::left).empty());

	// `count == 0` clears the surface - `items` unread, so even a dangling or
	// nonsensical pointer is harmless.
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT,
	                                     reinterpret_cast<const lesh_prompt_placement*>(1), 0),
	          LESH_OK);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "");

	// Otherwise a NULL `items` is a malformed call.
	EXPECT_EQ(lesh_prompt_set_placements(&registry, LESH_PROMPT_LEFT, nullptr, 1), LESH_ERR_INVAL);
}

// THE WHOLE-SURFACE VERB, ROUND TRIP. The template's items, as a tree, build
// the identical program - which is what "one builder, two front doors" (#157,
// owner's ruling) buys: the ABI and the string are one configuration language
// with two spellings.
TEST(UiPromptAbi, SetPlacementsSpellsExactlyWhatATemplateDoes) {
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	// `{path:cyan:s}{status:red::[:]}> `.
	const lesh_prompt_placement items[] = {
		{ .module = "path", .options = { .style = "cyan", .type = "s" } },
		{ .module = "status", .options = { .style = "red", .prefix = "[", .postfix = "]" } },
		{ .module = "literal", .options = { .prefix = "> " } },
	};
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);

	prompt::state failed = quiet();
	failed.status = 2;
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36msrc\x1b[0m\x1b[31m[2]\x1b[0m> ");

	// The affixes vanish with the module; the free literal does not.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36msrc\x1b[0m> ");

	// AND THE SAME CONFIGURATION AS A STRING RENDERS THE SAME BYTES. Two
	// spellings, one language - asserted rather than asserted-by-comment.
	engine spelled;
	std::string error;
	ASSERT_TRUE(spelled.set_template(surface_id::left, "{path:cyan:s}{status:red::[:]}> ", error))
		<< error;
	for (const prompt::state& against : {failed, quiet()}) {
		spelled.render_full(against);
		which.render_full(against);
		EXPECT_EQ(spelled.output(surface_id::left), which.output(surface_id::left))
			<< "the tree and the template disagree";
	}

	// ATOMICITY: a bad item anywhere in the tree leaves the previous surface
	// exactly as it was, before and after render identical - the whole point of
	// validating everything before applying anything.
	const lesh_prompt_placement bad_style[] = {
		{ .module = "path" },
		{ .module = "status", .options = { .style = "blod" } },
	};
	EXPECT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, bad_style), 1);
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36msrc\x1b[0m\x1b[31m[2]\x1b[0m> ");

	const lesh_prompt_placement bad_type[] = {
		{ .module = "path", .options = { .type = "medum" } },
	};
	EXPECT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, bad_type), 1);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36msrc\x1b[0m> ");

	// AND THE FIRST OFFENDER IS THE WHOLE ANSWER: an unknown module past a good
	// item still leaves everything standing, unbuilt.
	const lesh_prompt_placement unknown[] = {
		{ .module = "path" },
		{ .module = "weather" },
	};
	EXPECT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, unknown), LESH_ERR_NOTFOUND);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36msrc\x1b[0m> ");
}

// NESTING, WHICH THE OLD VERB STREAM COULD NOT SAY (#157): a group inside a
// group, built directly as a tree rather than refused as a second `open`.
TEST(UiPromptAbi, GroupsNestToAnyDepthAsATree) {
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	// `(a(b{status}))` - the inner group votes with `status`, and the outer
	// group votes because the inner one does. Two levels of `.children`, which
	// the old verb stream had no way to say at all.
	const lesh_prompt_placement inner[] = {
		{ .module = "literal", .options = { .prefix = "b" } },
		{ .module = "status" },
	};
	const lesh_prompt_placement outer_children[] = {
		{ .module = "literal", .options = { .prefix = "a" } },
		{ .children = inner, .child_count = 2 },
	};
	const lesh_prompt_placement items[] = {
		{ .children = outer_children, .child_count = 2 },
	};
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);

	prompt::state failed = quiet();
	failed.status = 5;
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), "ab5");

	// `status` never votes at `$? == 0` (its own render rule), so both groups
	// vanish together.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "");
}

TEST(UiPromptAbi, ARegisteredModuleWritesReadsItsArgAndAsksForAWake) {
	abi_probe probe;

	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	ASSERT_EQ(lesh_prompt_module_register(&registry, "probe", &abi_module, &probe), LESH_OK);

	std::int32_t exists = 0;
	ASSERT_EQ(lesh_prompt_module_exists(&registry, "probe", &exists), LESH_OK);
	EXPECT_EQ(exists, 1);
	ASSERT_EQ(lesh_prompt_module_exists(&registry, "weather", &exists), LESH_OK);
	EXPECT_EQ(exists, 0);

	ASSERT_EQ(lesh_prompt_clear(&registry, LESH_PROMPT_CONTINUATION), LESH_OK);
	const lesh_prompt_placement items[] = {
		{ .module = "probe", .options = { .type = "hello" } },
	};
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);

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

TEST(UiPromptAbi, ARegisteredModuleIsReplacedNotStacked) {
	abi_probe first;
	abi_probe second;

	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	ASSERT_EQ(lesh_prompt_module_register(&registry, "probe", &abi_module, &first), LESH_OK);
	ASSERT_EQ(lesh_prompt_module_register(&registry, "probe", &abi_module, &second), LESH_OK);

	const lesh_prompt_placement items[] = { { .module = "probe", .options = { .type = "x" } } };
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);
	which.render_full(quiet());

	EXPECT_EQ(first.calls, 0);
	EXPECT_EQ(second.calls, 1);
}

// A type-slot validator: accepts one spelling, refuses everything else, and
// names why through the same `error_out`/`capacity`/`length_out` copy-out
// convention every reader in this ABI uses.
std::int32_t only_ok_validate(const char* type, std::size_t length, void*, char* error_out,
                              std::size_t capacity, std::size_t* length_out) {
	if (std::string_view{type == nullptr ? "" : type, length} == "ok")
		return 0;

	static constexpr std::string_view kMessage = "must be 'ok'";
	*length_out = kMessage.size();
	if (error_out != nullptr)
		std::copy_n(kMessage.data(), std::min(capacity, kMessage.size()), error_out);
	return 1;
}

TEST(UiPromptAbi, RegisterWithNoValidatorAcceptsAnyType) {
	abi_probe probe;
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	// `options.validate` left at its zero default is exactly the plain verb: no
	// grammar to check, so any type slot reaches the module as written.
	ASSERT_EQ(lesh_prompt_module_register_with(&registry, "any", &abi_module, &probe, {}),
	          LESH_OK);
	const lesh_prompt_placement items[] = { { .module = "any", .options = { .type = "whatever" } } };
	EXPECT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);
	which.render_full(quiet());
	EXPECT_EQ(probe.arg, "whatever");
}

TEST(UiPromptAbi, RegisterWithAValidatorRefusesAndNamesWhy) {
	abi_probe probe;
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	ASSERT_EQ(lesh_prompt_module_register_with(&registry, "checked", &abi_module, &probe,
	                                           { .validate = &only_ok_validate }),
	          LESH_OK);

	// WHAT THE VALIDATOR ACCEPTS REACHES THE MODULE; what it refuses places
	// nothing at all, and answers the array verb's one refusal status, 1 - the
	// same status a bad style or `literal` misuse gets, because there is no
	// message channel here to tell them apart with.
	const lesh_prompt_placement ok[] = { { .module = "checked", .options = { .type = "ok" } } };
	EXPECT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, ok), LESH_OK);
	const lesh_prompt_placement nope[] = { { .module = "checked", .options = { .type = "nope" } } };
	EXPECT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, nope), 1);

	// AND THE WORDS TRAVEL THROUGH THE TEMPLATE, where there is a message
	// channel - `lesh_prompt_set_placements` has none, by design (abi.h says why).
	std::string error;
	EXPECT_FALSE(which.set_template(surface_id::left, "{checked::nope}", error));
	EXPECT_EQ(error, "checked: must be 'ok' at byte 10");
}

TEST(UiPromptAbi, AGroupVanishesWithItsModule) {
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	const lesh_prompt_placement group[] = {
		{ .module = "literal", .options = { .prefix = " [" } },
		{ .module = "status" },
		{ .module = "literal", .options = { .prefix = "]" } },
	};
	const lesh_prompt_placement items[] = {
		{ .children = group, .child_count = 3 },
		{ .module = "literal", .options = { .prefix = "$ " } },
	};
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);

	// The module omitted, so the brackets went with it - and the top-level
	// literal did not, because binding is explicit grouping.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "$ ");

	prompt::state failed = quiet();
	failed.status = 130;
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), " [130]$ ");
}

TEST(UiPromptAbi, AGroupsDecorationsDoNotRunWhenTheVoteFails) {
	test_module inner{"shout", "!"};

	engine which;
	// `git` is the module that says nothing here, and it is leshnici's - so this
	// engine has to be given it first (#163).
	lesh::leshnici::install_prompt_modules(which);
	lesh_registry registry;
	registry.host_prompt = &which;

	// A module that never says anything, so the group can never be shown.
	const lesh_prompt_placement group_git[] = {
		{ .module = "literal", .options = { .prefix = "on " } },
		{ .module = "git" },
	};
	const lesh_prompt_placement items_git[] = { { .children = group_git, .child_count = 2 } };
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items_git), LESH_OK);

	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "");

	// The same shape with a module that DOES say something: the literal runs
	// now, and in declared order before the module's bytes.
	ASSERT_EQ(which.register_module("shout", &inner), LESH_OK);
	const lesh_prompt_placement group_shout[] = {
		{ .module = "literal", .options = { .prefix = "on " } },
		{ .module = "shout" },
	};
	const lesh_prompt_placement items_shout[] = { { .children = group_shout, .child_count = 2 } };
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items_shout), LESH_OK);

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

TEST(UiPromptAbi, ArgFollowsTheCopyOutConvention) {
	abi_probe probe;

	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	ASSERT_EQ(lesh_prompt_module_register(&registry, "measure", &measuring_module, &probe),
	          LESH_OK);
	const lesh_prompt_placement items[] = {
		{ .module = "measure", .options = { .type = "abcdef" } },
	};
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);

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

TEST(UiPromptAbi, ANegativeStatusReadsAsOmitted) {
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

	ASSERT_EQ(lesh_prompt_module_register(&registry, "broken", &failing_module, nullptr),
	          LESH_OK);
	const lesh_prompt_placement items[] = {
		{ .module = "broken" },
		{ .module = "literal", .options = { .prefix = "$ " } },
	};
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, items), LESH_OK);

	which.render_full(quiet());

	// Omitted means its bytes never reach the surface, whatever it wrote before
	// giving up.
	EXPECT_EQ(which.output(surface_id::left), "$ ");
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
// WHERE `leshper_prompt_console`'S IS NOT: it stays anonymous in
// `ui/session.cpp`, unreachable from here, and that is `binding_console`'s
// precedent exactly - that adapter is tested through `bind`, and this one
// through `prompt`, whose own tests drive a fake console rather than reaching
// into `ui/session.cpp` (tests/unit/prompt_builtin_tests.cpp). Contorting the
// design to make an untestable-by-necessity class reachable would be paying for
// the follow-up ticket's test in this ticket's code. What IS asserted here is
// the seam's own contract: the interface is implementable over the real engine,
// the six verbs reach it, the two surfaces map, and the install point
// round-trips - which is everything the builtin will stand on when it is
// written.

// The same twenty lines `ui/session.cpp`'s adapter has, over a real engine. It
// exists to prove they are enough, which is what `leshper_keymap_tests.cpp`'s
// copy did for `bind` before there was a loop to hold the real one.
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

	// The same two bodies `ui/session.cpp`'s adapter has, now that there is a
	// language to read a template in: one parse-and-swap, and the source string
	// it remembered.
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

TEST(UiPromptWiring, TheConsoleRoundTripsOnTheShellState) {
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

TEST(UiPromptWiring, TheConsoleVerbsReachTheEngineAndBothSurfaces) {
	engine which;
	// With the shipped extension set on it, the way the wiring site builds one -
	// so the list the console hands back covers an installed module as well as a
	// built-in.
	lesh::leshnici::install_prompt_modules(which);
	test_prompt_console console{which};
	lesh::runtime::prompt_console& seam = console;

	std::vector<std::string> names;
	seam.module_names(names);
	EXPECT_NE(std::find(names.begin(), names.end(), "git"), names.end());
	EXPECT_NE(std::find(names.begin(), names.end(), "path"), names.end());

	// The two surfaces are configured apart, and what this asserts is the
	// translation between the runtime's `surface` and leshper's `surface_id`:
	// bytes put on one must not appear on the other.
	std::string error;
	EXPECT_EQ(seam.clear(console_surface::left), console_outcome::ok);
	EXPECT_EQ(seam.clear(console_surface::continuation), console_outcome::ok);
	EXPECT_EQ(seam.set(console_surface::left, "L", error), console_outcome::ok) << error;
	EXPECT_EQ(seam.set(console_surface::continuation, "C", error), console_outcome::ok) << error;
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "L");
	EXPECT_EQ(which.output(surface_id::continuation), "C");

	// The one failure the console has, and it arrives as a SENTENCE: the
	// per-element verbs that once answered a bare `no_such_module` are gone (see
	// the enum in builtins.h), so the refusal names the module and the byte.
	EXPECT_EQ(seam.set(console_surface::left, "{path}", error), console_outcome::ok) << error;
	EXPECT_EQ(seam.set(console_surface::left, "{no_such_thing}", error),
	          console_outcome::bad_template);
	EXPECT_NE(error.find("no_such_thing"), std::string::npos) << error;
	// And the refusal left the previous prompt standing.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "~/src");
	EXPECT_EQ(seam.set(console_surface::left, "L", error), console_outcome::ok) << error;

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
TEST(UiPromptEngine, ConfiguredIsFalseUntilAVerbRunsAndIsNeverRegained) {
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
		lesh::leshnici::install_prompt_modules(one);
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

TEST(UiPromptTemplate, EveryRowOfTheOmissionTableRenders) {
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

TEST(UiPromptTemplate, TheStandaloneLiteralCarriesItsTextInTheAffixSlots) {
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

TEST(UiPromptTemplate, AStyledLiteralIsGrammarAndDoesNotVote) {
	engine which;
	lesh::leshnici::install_prompt_modules(which);

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

TEST(UiPromptTemplate, TheEscapesReachTheBytes) {
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

TEST(UiPromptTemplate, EveryRefusalIsOneSentenceWithTheByteItPointsAt) {
	engine which;
	// `{git::x}` below is a module's own refusal, and the module is leshnici's.
	lesh::leshnici::install_prompt_modules(which);
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
	refuses("{}", "a placement needs a module name at byte 1");
	refuses("a\\qb", "unknown escape '\\q' at byte 1");

	// THE TYPE SLOT'S REFUSALS ARE THE MODULES' OWN, assembled from the module's
	// name and the module's own words. There is no table of message shapes here
	// and there could not be one: a module a binding registered would not be in
	// it, and it gets the same sentence.
	//
	// An empty slot has no byte of its own to point at, so the NAME is what a user
	// has to look at; a slot with bytes in it is pointed at directly and quoted
	// back.
	refuses("{env}", "env needs a variable name at byte 1");
	refuses("{path}{env:cyan}", "env needs a variable name at byte 7");
	refuses("{git::x}", "git takes no argument at byte 6");
	refuses("{path::medum}", "path: unknown variant 'medum' at byte 7");
	refuses("{path:cyan:medum}", "path: unknown variant 'medum' at byte 11");
	refuses("{time::13h}", "time: unknown variant '13h' at byte 7");
	refuses("{status::" + std::string(17, 'x') + "}", "status: symbol is too long at byte 9");

	// AND WHAT IS NO LONGER A REFUSAL, which is the model change seen from the
	// outside: `path` had no type slot at all and now owns five variants, so the
	// spelling that used to be `path takes no argument` is a prompt.
	std::string ok;
	EXPECT_TRUE(which.set_template(surface_id::left, "{path::short}", ok)) << ok;
	EXPECT_TRUE(which.set_template(surface_id::left, "{path:cyan:s}", ok)) << ok;
}

TEST(UiPromptTemplate, ARefusedTemplateLeavesEverythingExactlyAsItWas) {
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

TEST(UiPromptTemplate, TheDefaultTableAndItsTemplateAgree) {
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

TEST(UiPromptTemplate, TheDefaultAlsoAgreesWithItsOwnArrayForm) {
	// A THIRD SPELLING OF THE SAME PROMPT (#157): `{path}> ` as `lesh_prompt_
	// set_placements` items, proving "one builder, two front doors" on the one
	// prompt every fresh shell actually shows.
	engine array_side;
	lesh_registry registry;
	registry.host_prompt = &array_side;

	const lesh_prompt_placement left[] = {
		{ .module = "path" },
		{ .module = "literal", .options = { .prefix = "> " } },
	};
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, left), LESH_OK);
	const lesh_prompt_placement continuation[] = {
		{ .module = "literal", .options = { .prefix = "> " } },
	};
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_CONTINUATION, continuation), LESH_OK);

	engine template_side;
	std::string error;
	ASSERT_TRUE(template_side.set_template(surface_id::left, prompt::kDefaultLeftTemplate, error));
	ASSERT_TRUE(template_side.set_template(surface_id::continuation,
	                                       prompt::kDefaultContinuationTemplate, error));

	prompt::state elsewhere = quiet();
	elsewhere.pwd = "/etc";
	elsewhere.status = 130;

	for (const prompt::state& facts : {quiet(), elsewhere}) {
		array_side.render_full(facts);
		template_side.render_full(facts);
		EXPECT_EQ(array_side.output(surface_id::left), template_side.output(surface_id::left));
		EXPECT_EQ(array_side.output(surface_id::continuation),
		          template_side.output(surface_id::continuation));
	}

	// AN ARRAY-BUILT SURFACE HAS NO TEMPLATE STRING - the same rule the old
	// assembly verbs always followed.
	char buffer[64];
	std::size_t length = 0;
	ASSERT_EQ(lesh_prompt_text(&registry, LESH_PROMPT_LEFT, buffer, sizeof buffer, &length),
	          LESH_OK);
	EXPECT_EQ(length, 0u);
}

TEST(UiPromptTemplate, TheRememberedTextIsTheSourceOrNothingAtAll) {
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

TEST(UiPromptTemplate, TheValidatorsBuiltInTableIsTheRegistrys) {
	// The `constexpr` validator resolves names against a table in the header; the
	// engine resolves them against the registry its constructor filled. Two lists
	// that must not drift, and this is the only place they can be compared.
	//
	// ON A FRESH ENGINE, which is what makes the comparison mean anything since
	// #163: the table is the BUILT-INS, and an engine that has been handed the
	// shipped extension set has more than the built-ins in it.
	engine which;
	std::vector<std::string> names;
	which.module_names(names);

	ASSERT_EQ(names.size(), std::size(prompt::kBuiltinModules));
	for (std::size_t i = 0; i < names.size(); ++i)
		EXPECT_EQ(names[i], prompt::kBuiltinModules[i].name) << i;

	// AND WHAT LESHNICI ADDS IS EXACTLY ONE NAME, `git`. The other half of the
	// same no-drift question: the extension set is a list somebody maintains too,
	// and this is where it is stated rather than assumed.
	lesh::leshnici::install_prompt_modules(which);
	std::vector<std::string> after;
	which.module_names(after);

	std::vector<std::string> added;
	std::set_difference(after.begin(), after.end(), names.begin(), names.end(),
	                    std::back_inserter(added));
	EXPECT_EQ(added, std::vector<std::string>{"git"});
}

TEST(UiPromptTemplate, AModuleTheAbiRegisteredOwnsItsOwnArgument) {
	abi_probe probe;
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;
	ASSERT_EQ(lesh_prompt_module_register(&registry, "probe", &abi_module, &probe), LESH_OK);

	// The grammar has nothing to say about a type slot it did not define: the
	// module parses its own argument and the parser's job is to hand it over.
	EXPECT_EQ(set_and_render(which, "{probe::whatever}", quiet()), "<whatever>");
	EXPECT_EQ(probe.arg, "whatever");

	// A REPLACED BUILT-IN IS THE SAME CASE, and it is the case that says WHOSE a
	// type slot's grammar is. `medum` is not one of `path`'s five variants only
	// while `path` IS the built-in; a user who registered their own owns its
	// grammar as surely as they own its bytes, so the template that was a refusal
	// a moment ago is legal now. Nothing in the engine decides this - the module
	// the name resolves to does.
	test_module mine{"path", "p"};
	mine.constant = true;
	std::string error;
	EXPECT_FALSE(which.set_template(surface_id::left, "{path::medum}", error));
	EXPECT_EQ(error, "path: unknown variant 'medum' at byte 7");
	ASSERT_EQ(which.register_module("path", &mine), LESH_OK);
	EXPECT_TRUE(which.set_template(surface_id::left, "{path::medum}", error)) << error;

	// And before any registration at all the same template is a refusal.
	engine fresh;
	EXPECT_FALSE(fresh.set_template(surface_id::left, "{probe::whatever}", error));
	EXPECT_EQ(error, "unknown module 'probe' at byte 1");
}

// A STYLE IS A FIELD OF A PLACEMENT, NOT A THING PLACED BESIDE ONE, and this is
// what that bought. The verb that used to exist - `add_style`, a decoration in
// the stream that painted from there on and left an enclosing group owing a reset
// at its end - is gone, and with it the whole question of who owes the reset: the
// placement that opened a pen closes it, always, so the `$` after this group
// cannot be red no matter what is around it.
//
// The bytes are the ones the old spelling produced, unchanged.
TEST(UiPromptEngine, APlacementsPenIsScopedToThatPlacement) {
	engine which;
	which.clear(surface_id::left);
	which.clear(surface_id::continuation);

	ASSERT_TRUE(which.open_group(surface_id::left));
	ASSERT_EQ(which.place(surface_id::left, "status", "red.bold", "", "[", "]"),
	          prompt::place_result::ok);
	ASSERT_TRUE(which.close_group(surface_id::left));
	which.add_literal(surface_id::left, "$");

	prompt::state failed = quiet();
	failed.status = 7;
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), "\x1b[1;31m[7]\x1b[0m$");

	// And the pen vanishes with the module, exactly as the brackets do - because
	// it is the same record and answers with them.
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "$");

	// A spec that will not parse places nothing at all.
	EXPECT_EQ(which.place(surface_id::left, "status", "blod", "", "", ""),
	          prompt::place_result::bad_style);
	which.render_full(failed);
	EXPECT_EQ(which.output(surface_id::left), "\x1b[1;31m[7]\x1b[0m$");
}

TEST(UiPromptAbi, TheTemplateVerbsTravelAndTheMessageFollowsTheCopyOutConvention) {
	engine which;
	lesh_registry registry;
	registry.host_prompt = &which;

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

	// A styled placement inside a group, built as a tree, and the domain status
	// for a spec that will not parse.
	const lesh_prompt_placement cyan_group[] = {
		{ .module = "path", .options = { .style = "cyan" } },
	};
	const lesh_prompt_placement cyan_items[] = { { .children = cyan_group, .child_count = 1 } };
	ASSERT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, cyan_items), LESH_OK);
	const lesh_prompt_placement blod[] = { { .module = "path", .options = { .style = "blod" } } };
	EXPECT_EQ(prompt::set_placements(&registry, LESH_PROMPT_LEFT, blod), 1);
	which.render_full(quiet());
	EXPECT_EQ(which.output(surface_id::left), "\x1b[36m~/src\x1b[0m");

	// A surface built from a tree has no template string.
	ASSERT_EQ(lesh_prompt_text(&registry, LESH_PROMPT_LEFT, buffer, sizeof buffer, &out_length),
	          LESH_OK);
	EXPECT_EQ(out_length, 0u);

	// The argument errors, and the one that is a missing engine rather than a
	// malformed call.
	lesh_registry bare;
	ASSERT_EQ(bare.host_prompt, nullptr);
	EXPECT_EQ(lesh_prompt_set(&bare, LESH_PROMPT_LEFT, "x", 1, nullptr, 0, &needed),
	          LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_text(&bare, LESH_PROMPT_LEFT, buffer, sizeof buffer, &out_length),
	          LESH_ERR_NOTFOUND);
	EXPECT_EQ(prompt::set_placements(&bare, LESH_PROMPT_LEFT, cyan_items), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_prompt_set(nullptr, LESH_PROMPT_LEFT, "x", 1, nullptr, 0, &needed),
	          LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_set(&registry, 7u, "x", 1, nullptr, 0, &needed), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_set(&registry, LESH_PROMPT_LEFT, nullptr, 3, nullptr, 0, &needed),
	          LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_set(&registry, LESH_PROMPT_LEFT, "x", 1, nullptr, 0, nullptr),
	          LESH_ERR_INVAL);
	EXPECT_EQ(prompt::set_placements(&registry, 7u, cyan_items), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_set_placements(nullptr, LESH_PROMPT_LEFT, cyan_items, 1), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_prompt_text(&registry, 7u, buffer, sizeof buffer, &out_length), LESH_ERR_INVAL);
}

} // namespace
