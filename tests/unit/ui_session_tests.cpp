#include "leshper/keymap.h"
#include "ui/loop.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "runtime/builtins.h"
#include "runtime/history_store.h"
#include "runtime/shell_state.h"
#include "ui/history/store.h"
#include "ui/session.h"

#include "interactive_signal_guard.h"
#include "temp_path.h"

#include <gtest/gtest.h>

#include <csignal>
#include <string>
#include <vector>

using namespace lesh::leshper;
using namespace lesh::ui;

// THE UI LAYER (#134, moved to `src/ui/` by #164), tested where it can be: the
// four providers, the floor refusal, the outcome latch, and the signal chain.
//
// WHAT IS NOT HERE is the session itself - it spawns a thread, takes the
// process's dispositions and owns a terminal, which is a pty and a fork rather
// than a unit test. That is `ui_pty_tests.cpp`, and it drives the real
// binary's path end to end.

// ===========================================================================
// The syntax layer (#94, sealed; F-35)
// ===========================================================================

TEST(UiSessionSyntax, EnterRunsACompleteLine) {
	const shell_syntax_layer syntax;
	EXPECT_TRUE(syntax.line_is_complete("echo hi"));
	EXPECT_TRUE(syntax.line_is_complete(""));
	EXPECT_TRUE(syntax.line_is_complete("for i in a b; do echo $i; done"));
}

TEST(UiSessionSyntax, EnterContinuesAnIncompleteOne) {
	const shell_syntax_layer syntax;
	// C-2's incompleteness list, which is already load-bearing in parser_tests.
	EXPECT_FALSE(syntax.line_is_complete("echo \"x"));
	EXPECT_FALSE(syntax.line_is_complete("if true; then"));
	EXPECT_FALSE(syntax.line_is_complete("f() { echo x"));
	EXPECT_FALSE(syntax.line_is_complete("case a in"));
}

TEST(UiSessionSyntax, AMalformedLineAcceptsRatherThanTrappingTheUser) {
	// `incomplete()` wins over a defect while more input could come, and a defect
	// with nothing left to come is COMPLETE for F-35: the shell runs it and
	// reports the error. Answering "incomplete" here would hold the user in a
	// continuation prompt no keystroke could escape.
	const shell_syntax_layer syntax;
	EXPECT_TRUE(syntax.line_is_complete("echo ;;"));
}

TEST(UiSessionSyntax, VaredsLayerAcceptsEverything) {
	// F-17: `vared` edits a variable, where a newline is a newline. F-35
	// degenerates rather than being special-cased away, which is #94's own
	// acceptance test for A-5.
	const trivially_complete_syntax syntax;
	EXPECT_TRUE(syntax.line_is_complete("echo \"x"));
	EXPECT_TRUE(syntax.line_is_complete("if true; then"));
}

// ===========================================================================
// The prompt provider (#94: bytes out, no expansion)
// ===========================================================================

TEST(UiSessionPrompt, TheDefaultsArePosixAndComeFromTheShell) {
	const lesh::runtime::shell_state state;
	const shell_prompt_source prompt{state};
	std::string text;
	prompt.left(text);
	EXPECT_EQ(text, "$ ");
	prompt.continuation(text);
	EXPECT_EQ(text, "> ");
}

TEST(UiSessionPrompt, ThePromptIsLiteralBytesAndNothingIsExpanded) {
	// The decision, pinned: expansion is lesh-side and BEHIND this interface
	// (#94), so until it lands the bytes come out exactly as the variable holds
	// them. A half-implemented `\w` here would be a vocabulary the real
	// expansion would then have to keep.
	lesh::runtime::shell_state state;
	ASSERT_TRUE(state.set("PS1", "\\w \\u$ "));
	ASSERT_TRUE(state.set("PS2", "$PWD> "));
	const shell_prompt_source prompt{state};
	std::string text;
	prompt.left(text);
	EXPECT_EQ(text, "\\w \\u$ ");
	prompt.continuation(text);
	EXPECT_EQ(text, "$PWD> ");
}

TEST(UiSessionPrompt, AFixedSourceIsWhatVaredAndTheTestsPass) {
	const fixed_prompt_source prompt{"name: ", "..."};
	std::string text;
	prompt.left(text);
	EXPECT_EQ(text, "name: ");
	prompt.continuation(text);
	EXPECT_EQ(text, "...");
}

// ===========================================================================
// The history adapter (#113 store onto #125's shape)
// ===========================================================================

TEST(UiSessionHistory, TheStoreWalksNewestFirstThroughTheAdapter) {
	const lesh::testing::temp_path scratch;
	lesh::runtime::history_store storage{scratch.file("history")};
	EXPECT_TRUE(storage.append("one"));
	EXPECT_TRUE(storage.append("two"));
	EXPECT_TRUE(storage.append("three"));

	const history_store_source source{storage};
	std::vector<std::string> seen;
	source.for_each_newest_first([&](std::string_view entry) {
		seen.emplace_back(entry);
		return true;
	});
	ASSERT_EQ(seen.size(), 3u);
	EXPECT_EQ(seen[0], "three");
	EXPECT_EQ(seen[1], "two");
	EXPECT_EQ(seen[2], "one");
}

TEST(UiSessionHistory, AWalkThatStopsSeesNothingAfterTheStop) {
	// #125's note made real: the store's callback answers void, so the rest of
	// the walk still happens and this adapter simply stops SHOWING entries. What
	// matters to the searcher's supersede poll is that `fn` is not called again,
	// and that is what this asserts.
	const lesh::testing::temp_path scratch;
	lesh::runtime::history_store storage{scratch.file("history")};
	EXPECT_TRUE(storage.append("one"));
	EXPECT_TRUE(storage.append("two"));
	EXPECT_TRUE(storage.append("three"));

	const history_store_source source{storage};
	std::vector<std::string> seen;
	source.for_each_newest_first([&](std::string_view entry) {
		seen.emplace_back(entry);
		return false;
	});
	ASSERT_EQ(seen.size(), 1u);
	EXPECT_EQ(seen[0], "three");
}

TEST(UiSessionHistory, AMissingFileIsAnEmptyHistoryAndNotAnError) {
	const lesh::testing::temp_path scratch;
	const lesh::runtime::history_store storage{scratch.file("never-written")};
	const history_store_source source{storage};
	std::size_t seen = 0;
	source.for_each_newest_first([&](std::string_view) {
		++seen;
		return true;
	});
	EXPECT_EQ(seen, 0u);
}

// ===========================================================================
// The two-tier history in the bundle (#193, ADR-0010)
//
// The session itself is a pty test (`ui_pty_tests.cpp` drives the real binary
// through `execute` and back out again). What CAN be tested here is the shape
// `main.cpp` assembles and `session::execute` relies on: one object, entered
// twice, once per verb - and the invariant that makes that safe, which is that
// the read side is the same `history_source` the searcher already speaks to.
// ===========================================================================

TEST(UiSessionHistory, TheBundleCarriesOneStoreUnderBothVerbs) {
	lesh::ui::history::store storage;
	provider_bundle providers;
	providers.history = &storage;
	providers.recorder = &storage;

	// What `session::execute` does, in the order it does it: add before the
	// run, resolve after the wait.
	ASSERT_NE(providers.recorder->add("echo hi", "/some/where"),
	          lesh::ui::history::add_status::rejected);
	// And nothing can read it in between.
	std::size_t seen = 0;
	providers.history->for_each_newest_first([&seen](std::string_view) {
		++seen;
		return true;
	});
	EXPECT_EQ(seen, 0u);

	providers.recorder->resolve_pending(0);
	std::vector<std::string> entries;
	providers.history->for_each_newest_first([&entries](std::string_view entry) {
		entries.emplace_back(entry);
		return true;
	});
	EXPECT_EQ(entries, (std::vector<std::string>{"echo hi"}));
}

TEST(UiSessionHistory, ANullRecorderIsAShellThatRemembersNothing) {
	// F-17's `vared`, and a non-interactive shell: `main` builds no store when
	// there is no data directory, and the bundle says so with a null. The read
	// side is then an empty `vector_history_source` rather than a null pointer
	// every call site has to remember to check.
	const vector_history_source empty;
	provider_bundle providers;
	providers.history = &empty;
	EXPECT_EQ(providers.recorder, nullptr);

	std::size_t seen = 0;
	providers.history->for_each_newest_first([&seen](std::string_view) {
		++seen;
		return true;
	});
	EXPECT_EQ(seen, 0u);
}

TEST(UiSessionHistory, TheRecordedDirectoryIsTheShellsLogicalPwd) {
	// `session::execute` reads `$PWD` out of the shell rather than calling
	// `getcwd`, because a directory reached through a symlink is the path the
	// user typed. This is that read, against the same accessor the session uses.
	lesh::runtime::shell_state state;
	ASSERT_TRUE(state.set("PWD", "/logical/path"));
	std::string_view pwd;
	ASSERT_TRUE(state.lookup(std::string_view{"PWD"}, pwd));

	lesh::ui::history::store storage;
	ASSERT_NE(storage.add("echo hi", pwd), lesh::ui::history::add_status::rejected);
	storage.resolve_pending(0);

	std::string recorded;
	storage.for_each_merged_newest_first([&recorded](const lesh::ui::history::merged_entry& one) {
		recorded.assign(reinterpret_cast<const char*>(one.what.cwd.data()),
		                one.what.cwd.size());
		return false;
	});
	EXPECT_EQ(recorded, "/logical/path");
}

// ===========================================================================
// #97 decision 3: the floor
// ===========================================================================

TEST(UiSessionFloor, ATerminalThatSaidItIsNotOneIsBelowTheFloor) {
	EXPECT_FALSE(terminal_meets_floor(nullptr));
	EXPECT_FALSE(terminal_meets_floor(""));
	EXPECT_FALSE(terminal_meets_floor("dumb"));
}

TEST(UiSessionFloor, EverythingElseIsAssumedCapable) {
	// #97 decision 2, "assume first": the trivial environment read and nothing
	// else. Never terminfo, never a startup query - so an unknown name passes.
	EXPECT_TRUE(terminal_meets_floor("xterm-256color"));
	EXPECT_TRUE(terminal_meets_floor("screen"));
	EXPECT_TRUE(terminal_meets_floor("something-nobody-has-heard-of"));
}

TEST(UiSessionFloor, NoColorIsAUsersChoiceAndNotABelowFloorTerminal) {
	// The two questions are different: `from_env` answers what the terminal can
	// DO, and a monochrome xterm is a terminal leshper drives perfectly well.
	// Refusing to start on `NO_COLOR=1` would be refusing a preference.
	EXPECT_TRUE(terminal_meets_floor("xterm-256color"));
	const terminal_capabilities caps =
		terminal_capabilities::from_env("xterm-256color", nullptr, "1");
	EXPECT_EQ(caps.colors, color_depth::monochrome);
}

// ===========================================================================
// `bind` reaches the keymaps through shell_state (#118, #134)
// ===========================================================================

TEST(UiSessionConsole, AShellWithNoEditorHasNoConsole) {
	// The seam moved off file scope, so two shells in one process can no longer
	// be handed each other's keymaps - and a fresh one has nothing installed,
	// which is what makes `bind` in a script say "no line editor".
	const lesh::runtime::shell_state state;
	EXPECT_EQ(state.console(), nullptr);
}

// ===========================================================================
// The outcome effects (#134's latch, re-plumbed by #168)
// ===========================================================================

namespace {

std::int32_t ask_to_accept(lesh_editor* editor, const lesh_invocation*, void*) {
	return lesh_accept_line(editor);
}

std::int32_t ask_to_exit(lesh_editor* editor, const lesh_invocation*, void*) {
	return lesh_exit(editor, 7);
}

std::int32_t ask_for_nothing(lesh_editor*, const lesh_invocation*, void*) { return LESH_OK; }

template <typename Which>
[[nodiscard]] std::size_t count_of(const effects& produced) {
	std::size_t seen = 0;
	for (const effect& one : produced) {
		if (std::holds_alternative<Which>(one))
			++seen;
	}
	return seen;
}

} // namespace

TEST(UiSessionOutcome, WhatAnActionRequestedLeavesAsAnEffect) {
	// #134 parked this on `loop_harness` and the driver pulled it off with
	// `take_outcome`, because the keystroke path runs through `step`, which
	// answers with effects and not results. #168 made it an effect, so the answer
	// leaves down the one channel out of a turn - and a driver that is a separate
	// layer no longer reaches into the editor for it.
	state s;
	editing_context& context = context_of(s);
	ASSERT_EQ(lesh_action_register(&context.actions(), "ask_accept", ask_to_accept, nullptr),
	          LESH_OK);

	const action_result ran = context.loop().invoke(s, "ask_accept", invocation{});
	EXPECT_EQ(ran.outcome, loop_outcome::accept_line);
	EXPECT_EQ(count_of<line_accepted>(ran.produced), 1u);
	// And it is not held anywhere: a second dispatch that asks for nothing
	// produces nothing.
	ASSERT_EQ(lesh_action_register(&context.actions(), "ask_nothing", ask_for_nothing, nullptr),
	          LESH_OK);
	EXPECT_EQ(count_of<line_accepted>(context.loop().invoke(s, "ask_nothing", invocation{}).produced),
	          0u);
}

TEST(UiSessionOutcome, AnExitCarriesItsStatus) {
	state s;
	editing_context& context = context_of(s);
	ASSERT_EQ(lesh_action_register(&context.actions(), "ask_exit", ask_to_exit, nullptr),
	          LESH_OK);
	const action_result ran = context.loop().invoke(s, "ask_exit", invocation{});
	ASSERT_EQ(ran.produced.size(), 1u);
	ASSERT_TRUE(std::holds_alternative<end_of_file>(ran.produced.front()));
	EXPECT_EQ(std::get<end_of_file>(ran.produced.front()).status, 7);
}

TEST(UiSessionOutcome, ABoundKeyCarriesItsOutcomeOutOfStep) {
	// The half that had no answer before the latch existed, and the half the
	// latch existed for: a key bound to `accept_line` goes through `step`, and
	// what comes back is effects.
	state s;
	editing_context& context = context_of(s);
	ASSERT_EQ(lesh_action_register(&context.actions(), "ask_accept", ask_to_accept, nullptr),
	          LESH_OK);
	keymap* map = context.keymaps().find(keymap_registry::emacs);
	ASSERT_NE(map, nullptr);
	std::string encoded;
	ASSERT_TRUE(parse_key_notation("<C-a>", encoded));
	map->bind(encoded, "ask_accept");

	const effects produced = step(s, key_event::of(U'\x01'));
	EXPECT_EQ(count_of<line_accepted>(produced), 1u);
}

// ===========================================================================
// The signal chain (#134's resolution of the ownership question #129 returned)
// ===========================================================================

namespace {

volatile sig_atomic_t g_previous_ran = 0;

void previous_handler(int) { g_previous_ran = 1; }

// The one a `trap` typed mid-session installs, standing in for
// `runtime/signals.cpp`'s `record_signal` - which is the only foreign handler
// that ever appears in a real lesh process.
volatile sig_atomic_t g_newer_ran = 0;

void newer_handler(int) { g_newer_ran = 1; }

} // namespace

TEST(UiSessionSignals, TheHubCallsTheHandlerItReplaced) {
	// THE RESOLUTION, as a test. The shell's `g_pending` is set by a handler
	// `runtime/signals.cpp` installs; while the hub owns the disposition, only
	// chaining keeps that handler running - and it has to keep running not just
	// during editing but during EXECUTION, when the shell thread is busy in a
	// command and drains no slots at all.
	const lesh::testing::saved_disposition saved{SIGUSR1};
	struct sigaction previous{};
	previous.sa_handler = previous_handler;
	sigemptyset(&previous.sa_mask);
	ASSERT_EQ(::sigaction(SIGUSR1, &previous, nullptr), 0);

	signal_hub hub;
	// `deliver` is the handler's whole body, and driving it directly is what
	// keeps this test from touching the binary's real dispositions for a signal
	// the hub actually installs.
	struct sigaction now{};
	ASSERT_EQ(::sigaction(SIGUSR1, nullptr, &now), 0);
	EXPECT_EQ(now.sa_handler, previous_handler);

	g_previous_ran = 0;
	hub.deliver(SIGUSR1);
	// Nothing was SAVED for SIGUSR1 - this hub never installed - so nothing is
	// chained, and the wakeup is still ours.
	EXPECT_EQ(g_previous_ran, 0);
	std::vector<int> pending;
	EXPECT_EQ(hub.drain(pending), 1u);
	ASSERT_EQ(pending.size(), 1u);
	EXPECT_EQ(pending[0], SIGUSR1);
}

TEST(UiSessionSignals, InstallingChainsAndUninstallingPutsItBack) {
	// SIGWINCH, because it is one the hub really takes and because nothing else
	// in this binary cares what happens to it.
	const lesh::testing::saved_disposition saved{SIGWINCH};
	struct sigaction previous{};
	previous.sa_handler = previous_handler;
	sigemptyset(&previous.sa_mask);
	ASSERT_EQ(::sigaction(SIGWINCH, &previous, nullptr), 0);

	{
		signal_hub hub;
		ASSERT_TRUE(hub.install());
		// Ours now, and the shell's is what we saved.
		struct sigaction now{};
		ASSERT_EQ(::sigaction(SIGWINCH, nullptr, &now), 0);
		EXPECT_NE(now.sa_handler, previous_handler);

		g_previous_ran = 0;
		hub.deliver(SIGWINCH);
		EXPECT_EQ(g_previous_ran, 1) << "the saved handler was not chained";
		// And our own work happened too: SIGWINCH is a counter, not a queue.
		EXPECT_EQ(hub.resize_count(), 1u);

		hub.uninstall();
		ASSERT_EQ(::sigaction(SIGWINCH, nullptr, &now), 0);
		EXPECT_EQ(now.sa_handler, previous_handler);
	}
}

TEST(UiSessionSignals, ReassertingTakesItBackAndRetargetsTheChain) {
	// REWRITTEN BY #142, and the rewrite is the point. This test used to steal
	// SIGWINCH with SIG_IGN and assert that `reassert` stomped it and went on
	// chaining to the FIRST-saved handler. Both halves of that are now wrong:
	// an ignore is left standing (rule 3, tested next door), and a foreign
	// handler is taken but the chain is RETARGETED to it (rule 4a) - because
	// chaining to what was there before the user's `trap` existed is precisely
	// how `trap 'cmd' CHLD` was silently dead.
	//
	// The hazard the test exists for is unchanged: the user types
	// `trap ... WINCH` at the prompt, the shell's own `sigaction` replaces ours,
	// and the resize stops reaching the editor. Re-asserting at every read entry
	// takes the disposition back, and now takes the trap's handler with it.
	const lesh::testing::saved_disposition saved{SIGWINCH};
	struct sigaction previous{};
	previous.sa_handler = previous_handler;
	sigemptyset(&previous.sa_mask);
	ASSERT_EQ(::sigaction(SIGWINCH, &previous, nullptr), 0);

	signal_hub hub;
	ASSERT_TRUE(hub.install());

	// The shell, taking it away - what `trap 'cmd' WINCH` does.
	struct sigaction stolen{};
	stolen.sa_handler = newer_handler;
	sigemptyset(&stolen.sa_mask);
	ASSERT_EQ(::sigaction(SIGWINCH, &stolen, nullptr), 0);

	EXPECT_TRUE(hub.reassert());
	struct sigaction now{};
	ASSERT_EQ(::sigaction(SIGWINCH, nullptr, &now), 0);
	EXPECT_NE(now.sa_handler, newer_handler);
	EXPECT_NE(now.sa_handler, previous_handler);

	g_previous_ran = 0;
	g_newer_ran = 0;
	hub.deliver(SIGWINCH);
	EXPECT_EQ(g_newer_ran, 1) << "reassert did not retarget the chain to the newest handler";
	EXPECT_EQ(g_previous_ran, 0) << "reassert chained to the stale entry-time handler";

	// And `_saved` still holds the ENTRY-time disposition, which is the whole
	// reason the two slots are separate.
	hub.uninstall();
	ASSERT_EQ(::sigaction(SIGWINCH, nullptr, &now), 0);
	EXPECT_EQ(now.sa_handler, previous_handler) << "uninstall put back the newest, not the entry";
}

TEST(UiSessionSignals, AHubThatNeverInstalledDoesNotWriteDispositions) {
	// A test's hub is driven by `deliver` alone and must not start taking the
	// binary's dispositions when somebody calls `reassert` on it.
	const lesh::testing::saved_disposition saved{SIGWINCH};
	struct sigaction previous{};
	previous.sa_handler = previous_handler;
	sigemptyset(&previous.sa_mask);
	ASSERT_EQ(::sigaction(SIGWINCH, &previous, nullptr), 0);

	signal_hub hub;
	EXPECT_FALSE(hub.reassert());
	struct sigaction now{};
	ASSERT_EQ(::sigaction(SIGWINCH, nullptr, &now), 0);
	EXPECT_EQ(now.sa_handler, previous_handler);
}
