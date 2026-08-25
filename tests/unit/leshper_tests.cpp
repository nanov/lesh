#include "leshper/abi.h"
#include "leshper/editor.h"
#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "leshper/undo.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace lesh::leshper;

// The harness IS the deliverable (A-2, N-3).
//
// Every test below builds a state, feeds it events, and asserts on the state
// and the effects that came back. Nothing here opens a terminal, forks, reads a
// file or waits on a thread - which is the property the module was shaped
// around, not a convenience of the tests. If a future change makes any of this
// need a PTY, the change is wrong.

namespace {

// Helpers named for what a user does, so a test reads as a session.
effects type(state& s, std::string_view text) {
	effects all;
	for (const char byte : text) {
		effects one = step(s, key_event::of(static_cast<char32_t>(static_cast<unsigned char>(byte))));
		all.insert(all.end(), one.begin(), one.end());
	}
	return all;
}

effects press(state& s, named_key key) { return step(s, key_event::of(key)); }
effects press(state& s, char32_t codepoint) { return step(s, key_event::of(codepoint)); }

template <typename Alternative>
size_t count_of(const effects& produced) {
	size_t found = 0;
	for (const effect& one : produced)
		if (std::holds_alternative<Alternative>(one))
			++found;
	return found;
}

std::string text_of(const state& s) { return std::string(s.buffer.text()); }
size_t cursor_of(const state& s) { return s.cursor.byte_offset(); }

constexpr char32_t backspace_key = 0x7F;
constexpr char32_t kill_word_key = 0x17;  // Ctrl-W
constexpr char32_t undo_key = 0x1F;       // Ctrl-_

} // namespace

// ---------------------------------------------------------------------------
// The buffer and its positions.
// ---------------------------------------------------------------------------

TEST(LeshperBuffer, StepsOverAMultiByteCharacterInOneMove) {
	// Not F-3 yet - #88 owns grapheme clusters - but the cursor must already
	// refuse to land in the middle of a character, or every edit built on top is
	// splitting UTF-8 in half.
	text_buffer buffer;
	// a, é (two bytes), 中 (three), b.
	buffer.replace(buffer.begin_position(), buffer.begin_position(),
	               "a\xC3\xA9" "\xE4\xB8\xAD" "b");

	position at = buffer.begin_position();
	std::vector<size_t> offsets{at.byte_offset()};
	while (at != buffer.end_position()) {
		at = buffer.next_position(at);
		offsets.push_back(at.byte_offset());
		EXPECT_TRUE(buffer.is_boundary(at));
	}
	EXPECT_EQ(offsets, (std::vector<size_t>{0, 1, 3, 6, 7}));

	while (at != buffer.begin_position()) {
		at = buffer.previous_position(at);
		EXPECT_TRUE(buffer.is_boundary(at));
	}
}

TEST(LeshperBuffer, ReplaceAnswersWhereTheReplacementEnds) {
	text_buffer buffer;
	buffer.replace(buffer.begin_position(), buffer.begin_position(), "echo hi");
	const position at = buffer.replace(position::from_byte_offset(5),
	                                   position::from_byte_offset(7), "there");
	EXPECT_EQ(buffer.text(), "echo there");
	EXPECT_EQ(at.byte_offset(), 10u);
}

// ---------------------------------------------------------------------------
// Typing, moving, deleting - the first editing slice.
// ---------------------------------------------------------------------------

TEST(LeshperEditor, TypingBuildsTheBuffer) {
	state s;
	type(s, "echo hi");
	EXPECT_EQ(text_of(s), "echo hi");
	EXPECT_EQ(cursor_of(s), 7u);
}

TEST(LeshperEditor, TypingInsertsAtTheCursorRatherThanAtTheEnd) {
	state s;
	type(s, "echo");
	press(s, named_key::home);
	type(s, "# ");
	EXPECT_EQ(text_of(s), "# echo");
	EXPECT_EQ(cursor_of(s), 2u);
}

TEST(LeshperEditor, CursorMovesLeftRightHomeAndEnd) {
	state s;
	type(s, "abc");
	press(s, named_key::left);
	EXPECT_EQ(cursor_of(s), 2u);
	press(s, named_key::left);
	press(s, named_key::left);
	press(s, named_key::left); // already at the start: stays there
	EXPECT_EQ(cursor_of(s), 0u);
	press(s, named_key::right);
	EXPECT_EQ(cursor_of(s), 1u);
	press(s, named_key::end);
	EXPECT_EQ(cursor_of(s), 3u);
	press(s, named_key::right); // already at the end: stays there
	EXPECT_EQ(cursor_of(s), 3u);
	press(s, named_key::home);
	EXPECT_EQ(cursor_of(s), 0u);
	EXPECT_EQ(text_of(s), "abc");
}

TEST(LeshperEditor, AMoveThatCannotHappenAsksForNothing) {
	state s;
	type(s, "ab");
	press(s, named_key::home);
	const effects produced = press(s, named_key::left);
	EXPECT_TRUE(produced.empty());
	EXPECT_EQ(cursor_of(s), 0u);
}

TEST(LeshperEditor, HomeAndEndActOnTheLineNotTheBuffer) {
	// F-2: the buffer is a 2D text object, so these are line operations rather
	// than buffer operations.
	//
	// The buffer is seeded rather than typed, because no key produces a newline
	// yet and that is deliberate: F-35 makes Enter a decision the parser takes
	// part in (complete → accept, incomplete → newline and keep editing), and
	// binding it to self-insert to make this test shorter would answer that
	// question wrongly and quietly. The other producers - history recall of a
	// multiline entry (F-34), a bracketed paste (F-6) - are later tickets too.
	state s;
	s.cursor = s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(),
	                            "first\nsecond");
	press(s, named_key::home);
	EXPECT_EQ(cursor_of(s), 6u);
	press(s, named_key::end);
	EXPECT_EQ(cursor_of(s), 12u);
}

TEST(LeshperEditor, BackspaceDeletesOneCharacterIncludingMultiByteOnes) {
	state s;
	type(s, "abc");
	press(s, backspace_key);
	EXPECT_EQ(text_of(s), "ab");
	EXPECT_EQ(cursor_of(s), 2u);

	step(s, key_event::of(static_cast<char32_t>(0xE9))); // é, two bytes
	EXPECT_EQ(text_of(s), "ab\xC3\xA9");
	press(s, named_key::backspace);
	EXPECT_EQ(text_of(s), "ab");
	EXPECT_EQ(cursor_of(s), 2u);
}

TEST(LeshperEditor, BackspaceAtTheStartOfTheBufferDoesNothingAtAll) {
	state s;
	const effects produced = press(s, backspace_key);
	EXPECT_TRUE(produced.empty());
	EXPECT_EQ(text_of(s), "");
	EXPECT_EQ(s.gen.value(), 0u);
}

TEST(LeshperEditor, DeleteBackwardWordEatsTrailingBlanksAndOneWord) {
	state s;
	type(s, "echo one two   ");
	press(s, kill_word_key);
	EXPECT_EQ(text_of(s), "echo one ");
	press(s, kill_word_key);
	EXPECT_EQ(text_of(s), "echo ");
	press(s, kill_word_key);
	EXPECT_EQ(text_of(s), "");
	const effects produced = press(s, kill_word_key);
	EXPECT_TRUE(produced.empty()); // nothing left to delete: no edit, no redraw
}

TEST(LeshperEditor, EveryBuiltInActionHasAName) {
	// F-13: nothing unnamed, nothing unrebindable. The name is what #93's
	// registry will key on.
	EXPECT_STREQ(name_of(action::self_insert), "self-insert");
	EXPECT_STREQ(name_of(action::delete_backward_word), "delete-backward-word");
	EXPECT_STREQ(name_of(binding_for(key_event::of(named_key::left))), "backward-char");
	EXPECT_STREQ(name_of(binding_for(key_event::of(U'x'))), "self-insert");
}

TEST(LeshperEditor, AnUnboundKeyChangesNothingAndEmitsNothing) {
	state s;
	type(s, "hi");
	const state before = s;
	const effects produced = press(s, static_cast<char32_t>(0x03)); // Ctrl-C: #93 binds it
	EXPECT_TRUE(produced.empty());
	EXPECT_TRUE(s == before);
}

// ---------------------------------------------------------------------------
// The generation counter (A-10, N-4).
// ---------------------------------------------------------------------------

TEST(LeshperGeneration, BumpsExactlyOncePerMutatingAction) {
	state s;
	EXPECT_EQ(s.gen.value(), 0u);
	type(s, "abc");
	EXPECT_EQ(s.gen.value(), 3u); // three insertions, three bumps

	press(s, backspace_key);
	EXPECT_EQ(s.gen.value(), 4u);
	press(s, kill_word_key);
	EXPECT_EQ(s.gen.value(), 5u);
	press(s, undo_key);
	EXPECT_EQ(s.gen.value(), 6u);
}

TEST(LeshperGeneration, NonMutatingActionsLeaveItAloneAndChangeNoText) {
	state s;
	type(s, "abc");
	const uint64_t after_typing = s.gen.value();
	const std::string typed = text_of(s);

	press(s, named_key::left);
	press(s, named_key::home);
	press(s, named_key::end);
	press(s, named_key::right);

	EXPECT_EQ(s.gen.value(), after_typing);
	EXPECT_EQ(text_of(s), typed);
}

TEST(LeshperGeneration, AMutationAsksTheReactorsToRecomputeAgainstTheNewOne) {
	// The A-10 loop's first half: the action edited, the generation bumped, and
	// the request that goes out carries it.
	state s;
	const effects produced = press(s, U'x');
	ASSERT_EQ(count_of<worker_request>(produced), 1u);
	EXPECT_EQ(count_of<render_request>(produced), 1u);
	for (const effect& one : produced)
		if (const auto* request = std::get_if<worker_request>(&one))
			EXPECT_EQ(request->computed_against, s.gen);
}

TEST(LeshperGeneration, CursorMovementAsksForARedrawButNotForRecomputation) {
	state s;
	type(s, "abc");
	const effects produced = press(s, named_key::left);
	EXPECT_EQ(count_of<render_request>(produced), 1u);
	EXPECT_EQ(count_of<worker_request>(produced), 0u);
}

// ---------------------------------------------------------------------------
// Undo (F-1, F-4).
// ---------------------------------------------------------------------------

TEST(LeshperUndo, ARunOfPlainTypingIsOneStep) {
	state s;
	type(s, "abc");
	EXPECT_EQ(s.undo.step_count(), 1u);
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "");
}

TEST(LeshperUndo, RestoresTextAndCursorTogether) {
	// F-4 names both, and the cursor is the half that gets forgotten: undoing a
	// deletion in the middle of a line must put the cursor back where the text
	// came from, not at the end.
	state s;
	type(s, "echo there");
	press(s, named_key::home);
	press(s, named_key::right);
	press(s, named_key::right);
	press(s, named_key::right);
	press(s, named_key::right); // after "echo"
	const size_t cursor_before = cursor_of(s);
	press(s, kill_word_key);
	ASSERT_EQ(text_of(s), " there");
	ASSERT_EQ(cursor_of(s), 0u);

	press(s, undo_key);
	EXPECT_EQ(text_of(s), "echo there");
	EXPECT_EQ(cursor_of(s), cursor_before);
}

TEST(LeshperUndo, ACursorMoveBreaksTheTypingRun) {
	state s;
	type(s, "ab");
	press(s, named_key::left);
	press(s, named_key::right);
	type(s, "cd");
	EXPECT_EQ(s.undo.step_count(), 2u);
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "ab");
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "");
}

TEST(LeshperUndo, DeletionsDoNotCoalesceIntoOneStep) {
	// Deliberate, and fish's behaviour: single-character insertions coalesce and
	// nothing else. Undo gives a word back one piece at a time.
	state s;
	type(s, "abc");
	press(s, backspace_key);
	press(s, backspace_key);
	ASSERT_EQ(text_of(s), "a");
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "ab");
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "abc");
}

TEST(LeshperUndo, RedoPutsBackWhatUndoTookAway) {
	state s;
	type(s, "abc");
	press(s, undo_key);
	ASSERT_EQ(text_of(s), "");
	ASSERT_TRUE(s.undo.redo(s.buffer, s.cursor));
	EXPECT_EQ(text_of(s), "abc");
	EXPECT_EQ(cursor_of(s), 3u);
}

TEST(LeshperUndo, ANewEditAfterAnUndoDiscardsTheRedoTail) {
	state s;
	type(s, "abc");
	press(s, undo_key);
	type(s, "x");
	EXPECT_FALSE(s.undo.can_redo());
	EXPECT_EQ(text_of(s), "x");
}

TEST(LeshperUndo, AnEditGroupUndoesAsOneStep) {
	// The shape F-6 (bracketed paste is one undo step) and #92's atomic
	// write-back both need. Exercised directly, because no action opens a group
	// yet - the mechanism lands before its first caller on purpose.
	state s;
	type(s, "echo ");
	s.undo.begin_group();
	type(s, "one");
	press(s, backspace_key);
	type(s, "x");
	s.undo.end_group();
	ASSERT_EQ(text_of(s), "echo onx");

	const size_t steps = s.undo.step_count();
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "echo ");
	EXPECT_EQ(s.undo.step_count(), steps - 1);
}

TEST(LeshperUndo, EditGroupsNest) {
	// F-15 lets a user action call another; both open a group, and only the
	// outermost close commits the step.
	state s;
	s.undo.begin_group();
	type(s, "a");
	s.undo.begin_group();
	type(s, "b");
	s.undo.end_group();
	EXPECT_TRUE(s.undo.group_open());
	type(s, "c");
	s.undo.end_group();
	ASSERT_EQ(text_of(s), "abc");
	EXPECT_EQ(s.undo.step_count(), 1u);
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "");
}

TEST(LeshperUndo, UndoingWithNothingToUndoIsANoOp) {
	state s;
	const state before = s;
	const effects produced = press(s, undo_key);
	EXPECT_TRUE(produced.empty());
	EXPECT_TRUE(s == before);
}

// ---------------------------------------------------------------------------
// Events: the only way in (A-9).
// ---------------------------------------------------------------------------

TEST(LeshperEvents, InjectedInputIsReadBackAsThoughTyped) {
	// F-7, zle's `zle -U`.
	state s;
	type(s, "ec");
	step(s, injected_input{"ho hi"});
	EXPECT_EQ(text_of(s), "echo hi");
	EXPECT_TRUE(s.pending.empty());
}

TEST(LeshperEvents, InjectedInputGoesThroughTheKeymapRatherThanAroundIt) {
	// A control character in injected text invokes its binding, exactly as it
	// would if the user had pressed the key. Splicing text into the buffer
	// instead would be a route the user's own bindings never see (A-12).
	state s;
	type(s, "echo one");
	step(s, injected_input{"\x17two"});
	EXPECT_EQ(text_of(s), "echo two");
}

TEST(LeshperEvents, InjectedMultiByteTextSurvivesTheRoundTrip) {
	state s;
	step(s, injected_input{"caf\xC3\xA9 \xE4\xB8\xAD"});
	EXPECT_EQ(text_of(s), "caf\xC3\xA9 \xE4\xB8\xAD");
}

TEST(LeshperEvents, AStaleWorkerResultIsDropped) {
	// N-4, and the reason generation exists. The result was computed against a
	// buffer the user has since changed; it is dropped unlooked-at, and the
	// editor emits nothing at all.
	state s;
	type(s, "ec");
	const generation stale = s.gen;
	type(s, "ho");
	ASSERT_FALSE(stale == s.gen);

	const state before = s;
	const effects produced = step(s, worker_result{stale});
	EXPECT_TRUE(produced.empty());
	EXPECT_TRUE(s == before);
}

TEST(LeshperEvents, ACurrentWorkerResultIsAccepted) {
	state s;
	type(s, "echo");
	const effects produced = step(s, worker_result{s.gen});
	EXPECT_EQ(count_of<render_request>(produced), 1u);
}

TEST(LeshperEvents, ResizeIsAnEventAndAFullRepaint) {
	// A-3: SIGWINCH reaches the editor as an ordinary event, never as a handler
	// poking editor state. F-37 keeps full repaints for resize.
	state s;
	const effects produced = step(s, resize_event{80, 24});
	EXPECT_EQ(s.columns, 80);
	EXPECT_EQ(s.rows, 24);
	EXPECT_EQ(count_of<render_request>(produced), 1u);
}

TEST(LeshperEvents, AJobNoticeRepaintsAndLosesNothing) {
	// F-39: the notice prints above the prompt and the edit line survives
	// underneath it.
	state s;
	type(s, "echo hi");
	const std::string typed = text_of(s);
	const uint64_t generation_before = s.gen.value();

	const effects produced = step(s, job_notice{4242, 0});
	EXPECT_EQ(count_of<render_request>(produced), 1u);
	EXPECT_EQ(text_of(s), typed);
	EXPECT_EQ(s.gen.value(), generation_before);
}

TEST(LeshperEvents, ASignalEntersButIsBoundToNothingYet) {
	// The entrance is what A-9 asks for. #98 settled what SIGINT does; the
	// binding that expresses it is keymap data, and keymaps are #93's.
	state s;
	type(s, "echo");
	const state before = s;
	const effects produced = step(s, signal_event{2});
	EXPECT_TRUE(produced.empty());
	EXPECT_TRUE(s == before);
}

TEST(LeshperEffects, TheLoopCarriesLaneTwoOfThePortOutAsAnEffect) {
	// #92's shape, asserted so it cannot drift: only lane 2 - an external, spawned
	// exec-only - leaves the process, so only lane 2 is an effect. Nothing emits
	// one yet; the provider that will is #94's.
	const effect one = spawn_request{{"git", "status"}, generation{}};
	ASSERT_TRUE(std::holds_alternative<spawn_request>(one));
	EXPECT_EQ(std::get<spawn_request>(one).argv.size(), 2u);
}

// ---------------------------------------------------------------------------
// Deterministic replay (N-3).
// ---------------------------------------------------------------------------

TEST(LeshperReplay, TheSameEventSequenceReplaysToTheSameState) {
	// N-3's seed: a recorded event sequence - worker-result timing included -
	// reproduces an identical state. This is why `state` has an equality
	// operator over every field rather than the fields a test remembered.
	const std::vector<event> recorded{
		key_event::of(U'e'),      key_event::of(U'c'),
		key_event::of(U'h'),      key_event::of(U'o'),
		key_event::of(U' '),      resize_event{100, 30},
		key_event::of(U'o'),      key_event::of(U'n'),
		key_event::of(U'e'),      worker_result{generation{}}, // stale by now
		key_event::of(0x17),      injected_input{"two"},
		key_event::of(0x7F),      key_event::of(U'o'),
		key_event::of(0x01),      key_event::of(0x05),
		key_event::of(0x1F),      signal_event{28},
		job_notice{7, 0},
	};

	const auto play = [&recorded] {
		state s;
		size_t effect_count = 0;
		for (const event& one : recorded)
			effect_count += step(s, one).size();
		return std::pair{s, effect_count};
	};

	const auto [first, first_effects] = play();
	const auto [second, second_effects] = play();

	EXPECT_TRUE(first == second);
	EXPECT_EQ(first_effects, second_effects);
	// And the replay actually did something, so equality is not vacuous.
	EXPECT_FALSE(first.buffer.empty());
	EXPECT_GT(first.gen.value(), 0u);
}

TEST(LeshperReplay, StateEqualityNoticesEachFieldItCompares) {
	state a;
	state b;
	EXPECT_TRUE(a == b);

	type(a, "x");
	EXPECT_FALSE(a == b);
	type(b, "x");
	EXPECT_TRUE(a == b);

	press(a, named_key::home);
	EXPECT_FALSE(a == b); // cursor
	press(b, named_key::home);

	a.columns = 80;
	EXPECT_FALSE(a == b); // terminal size
	b.columns = 80;

	a.pending.injected = "x";
	EXPECT_FALSE(a == b); // pending input
	b.pending.injected = "x";
	EXPECT_TRUE(a == b);
}

// ===========================================================================
// The action/reactor ABI (#93, ADR-0008, architecture spec 6.1).
//
// Everything below is the LeshperAbi* suites. They exercise the C surface the
// way a binding will - through abi.h, against handles the loop minted - because
// the property under test is not "the editor edits" but "the only way to make
// it edit is this one".
// ===========================================================================

namespace {

// A registry with the built-ins in it, and a loop to run them, in one line per
// test. The registry owns everything it hands out and frees it here (ADR-0007).
struct abi_fixture {
	registry actions;
	loop_harness loop{actions};

	abi_fixture() { register_builtin_actions(actions); }

	action_result run(state& s, std::string_view name) {
		return loop.invoke(s, name, invocation{});
	}

	action_result type_one(state& s, std::string_view bytes) {
		return loop.invoke(s, "self_insert",
		                   invocation{std::string(bytes)});
	}

	action_result type_all(state& s, std::string_view text) {
		action_result last;
		for (const char byte : text)
			last = type_one(s, std::string_view(&byte, 1));
		return last;
	}
};

// Reads the whole buffer back through the ABI, the way a binding must: ask how
// long it is, then ask for the bytes. No accessor lends a pointer, so there is
// no shorter way, and that is the point rather than an inconvenience.
std::string abi_buffer(lesh_editor* editor) {
	size_t length = 0;
	EXPECT_EQ(lesh_buffer_length(editor, &length), LESH_OK);
	std::string out(length, '\0');
	size_t written = 0;
	EXPECT_EQ(lesh_buffer_get(editor, out.data(), out.size(), &written), LESH_OK);
	return out;
}

std::string token_buffer(const lesh_request* request) {
	size_t length = 0;
	EXPECT_EQ(lesh_request_buffer_length(request, &length), LESH_OK);
	std::string out(length, '\0');
	size_t written = 0;
	EXPECT_EQ(lesh_request_buffer(request, out.data(), out.size(), &written), LESH_OK);
	return out;
}

int32_t nothing_action(lesh_editor*, const lesh_invocation*, void*) { return LESH_OK; }
int32_t nothing_reactor(lesh_request*, void*) { return LESH_OK; }

int32_t count_calls(lesh_editor*, const lesh_invocation*, void* userdata) {
	++*static_cast<int*>(userdata);
	return LESH_OK;
}

} // namespace

// ---------------------------------------------------------------------------
// The registries.
// ---------------------------------------------------------------------------

TEST(LeshperAbiRegistry, NamesAreSnakeCaseAndNothingElse) {
	registry reg;
	EXPECT_EQ(lesh_action_register(&reg, "smart_accept", nothing_action, nullptr), LESH_OK);
	EXPECT_EQ(lesh_action_register(&reg, "a", nothing_action, nullptr), LESH_OK);
	EXPECT_EQ(lesh_action_register(&reg, "accept_line_2", nothing_action, nullptr), LESH_OK);

	// #93 fixed snake_case. Admitting the kebab spelling as well would make
	// `accept-line` and `accept_line` two actions that look like one.
	EXPECT_EQ(lesh_action_register(&reg, "accept-line", nothing_action, nullptr), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_action_register(&reg, "AcceptLine", nothing_action, nullptr), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_action_register(&reg, "2fast", nothing_action, nullptr), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_action_register(&reg, "", nothing_action, nullptr), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_action_register(&reg, "with space", nothing_action, nullptr), LESH_ERR_INVAL);
	EXPECT_EQ(lesh_action_register(&reg, "ok", nullptr, nullptr), LESH_ERR_INVAL);
}

TEST(LeshperAbiRegistry, RegistrationReplacesSoResourcingAnRcFileIsIdempotent) {
	// #101: an rc file read twice must leave one action, not two and not an
	// error. Replacement is what makes that free.
	registry reg;
	int first = 0;
	int second = 0;
	EXPECT_EQ(lesh_action_register(&reg, "greet", count_calls, &first), LESH_OK);
	EXPECT_EQ(lesh_action_register(&reg, "greet", count_calls, &second), LESH_OK);

	state s;
	loop_harness loop(reg);
	EXPECT_EQ(loop.invoke(s, "greet", invocation{}).status, LESH_OK);
	EXPECT_EQ(first, 0);   // the first registration is gone, not shadowed
	EXPECT_EQ(second, 1);
}

TEST(LeshperAbiRegistry, TheDotOriginalIsMintedOnceAndCannotBeShadowed) {
	registry reg;
	int original_calls = 0;
	int wrapper_calls = 0;
	ASSERT_EQ(lesh_action_register(&reg, "accept_line", count_calls, &original_calls), LESH_OK);
	ASSERT_EQ(lesh_action_register(&reg, "accept_line", count_calls, &wrapper_calls), LESH_OK);

	state s;
	loop_harness loop(reg);
	loop.invoke(s, "accept_line", invocation{});
	EXPECT_EQ(wrapper_calls, 1);
	EXPECT_EQ(original_calls, 0);

	// `.accept_line` still reaches the first definition. That is what lets a
	// wrapper delegate, and what keeps F-18's recovery path off a name the user
	// has replaced.
	loop.invoke(s, ".accept_line", invocation{});
	EXPECT_EQ(original_calls, 1);
	EXPECT_EQ(wrapper_calls, 1);

	// An original you can overwrite is not one.
	EXPECT_EQ(lesh_action_register(&reg, ".accept_line", nothing_action, nullptr),
	          LESH_ERR_REFUSED);
	EXPECT_EQ(lesh_action_register(&reg, ".brand_new", nothing_action, nullptr),
	          LESH_ERR_REFUSED);
}

TEST(LeshperAbiRegistry, AWrapperDelegatingToTheOriginalIsOneUndoEntry) {
	// F-15 through the ABI, and #92's atomicity across it: the callee stages
	// into the caller's staging area, so two edits in two actions commit as one
	// replacement, one undo entry and one generation bump.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "shout",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          size_t at = 0;
		          lesh_cursor_get(editor, &at);
		          return lesh_buffer_replace(editor, at, at, "!", 1);
	          }, nullptr), LESH_OK);
	ASSERT_EQ(lesh_action_register(&reg, "shout",
	          [](lesh_editor* editor, const lesh_invocation* how, void*) -> int32_t {
		          const int32_t status = lesh_action_invoke(editor, ".shout", how);
		          if (status != LESH_OK)
			          return status;
		          size_t at = 0;
		          lesh_cursor_get(editor, &at);
		          return lesh_buffer_replace(editor, at, at, "!", 1);
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	const action_result out = loop.invoke(s, "shout", invocation{});
	EXPECT_EQ(out.status, LESH_OK);
	EXPECT_EQ(std::string(s.buffer.text()), "!!");
	EXPECT_EQ(s.undo.step_count(), 1u);
	EXPECT_EQ(s.gen.value(), 1u);
}

TEST(LeshperAbiRegistry, TheRecursionCeilingIsAnErrorAndNotAStackOverflow) {
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "spin",
	          [](lesh_editor* editor, const lesh_invocation* how, void*) -> int32_t {
		          return lesh_action_invoke(editor, "spin", how);
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	EXPECT_EQ(loop.invoke(s, "spin", invocation{}).status,
	          LESH_ERR_RECURSION);
}

TEST(LeshperAbiRegistry, DispatchThroughAnUnregisteredNameIsAMissAndNotACrash) {
	abi_fixture fixture;
	state s;
	const action_result out = fixture.run(s, "no_such_action");
	EXPECT_EQ(out.status, LESH_ERR_NOTFOUND);
	EXPECT_TRUE(out.produced.empty());
	EXPECT_TRUE(s.buffer.empty());
}

TEST(LeshperAbiRegistry, ReactorRegistrationTakesAMaskAndRejectsBitsItDoesNotKnow) {
	registry reg;
	EXPECT_EQ(lesh_reactor_register(&reg, "todo_marker", LESH_EVENT_BUFFER_CHANGED,
	                                nothing_reactor, nullptr), LESH_OK);
	EXPECT_EQ(lesh_reactor_register(&reg, "todo_marker",
	                                LESH_EVENT_BUFFER_CHANGED | LESH_EVENT_CURSOR_MOVED,
	                                nothing_reactor, nullptr), LESH_OK);
	// Subscribing to nothing is a mistake, not a subscription.
	EXPECT_EQ(lesh_reactor_register(&reg, "quiet", 0, nothing_reactor, nullptr),
	          LESH_ERR_INVAL);
	// The mask is reserved for additive growth, so an unknown bit is refused
	// rather than ignored: a reactor built against a newer header must not
	// silently receive fewer events than it asked for.
	EXPECT_EQ(lesh_reactor_register(&reg, "future", 0x80u, nothing_reactor, nullptr),
	          LESH_ERR_INVAL);

	int32_t exists = 0;
	EXPECT_EQ(lesh_reactor_exists(&reg, "todo_marker", &exists), LESH_OK);
	EXPECT_EQ(exists, 1);
	EXPECT_EQ(lesh_reactor_exists(&reg, "quiet", &exists), LESH_OK);
	EXPECT_EQ(exists, 0);
}

// ---------------------------------------------------------------------------
// The built-ins, through the ABI and by no other route.
// ---------------------------------------------------------------------------

TEST(LeshperAbiBuiltins, EveryEnumBehaviourHasAnAbiRegistration) {
	// The claim ADR-0008 makes: built-ins are the registry's first clients,
	// registered through the identical signatures a binding would use. If one of
	// #107's behaviours is missing here, something is still reachable only
	// natively - which is the side door the whole design is against.
	abi_fixture fixture;
	const char* every[] = {"self_insert",       "delete_backward_char",
	                       "delete_backward_word", "backward_char",
	                       "forward_char",      "beginning_of_line",
	                       "end_of_line",       "undo",
	                       "redo"};
	for (const char* name : every) {
		int32_t exists = 0;
		EXPECT_EQ(lesh_action_exists(&fixture.actions, name, &exists), LESH_OK) << name;
		EXPECT_EQ(exists, 1) << name;
		// And its unshadowable original came with it.
		EXPECT_EQ(lesh_action_exists(&fixture.actions, ("." + std::string(name)).c_str(),
		                             &exists), LESH_OK);
		EXPECT_EQ(exists, 1) << name;
	}
	// action::none is the absence of a binding, not a behaviour, so it has no
	// registration - which is why the enum has ten members and the registry nine
	// actions.
	int32_t exists = 1;
	EXPECT_EQ(lesh_action_exists(&fixture.actions, "none", &exists), LESH_OK);
	EXPECT_EQ(exists, 0);
}

TEST(LeshperAbiEquivalence, TheAbiPathAndTheKeymapPathAgreeOnEveryBuiltIn) {
	// Two implementations of these behaviours exist right now: editor.cpp's
	// switch over the enum, and builtin_actions.cpp over the ABI. The keymap
	// stack that deletes the first is the rest of #93's work. Until then this is
	// what keeps them from drifting, and it asserts on the whole state rather
	// than on the fields somebody remembered to check.
	struct move {
		const char* abi_name;
		const char* keys;   // the bytes self_insert inserts; null for the rest
		event replay;       // the same thing, as the key a user would press
	};
	const move script[] = {
		{"self_insert", "e", key_event::of(U'e')},
		{"self_insert", "c", key_event::of(U'c')},
		{"self_insert", "h", key_event::of(U'h')},
		{"self_insert", "o", key_event::of(U'o')},
		{"self_insert", " ", key_event::of(U' ')},
		{"self_insert", "h", key_event::of(U'h')},
		{"self_insert", "i", key_event::of(U'i')},
		{"backward_char", nullptr, key_event::of(named_key::left)},
		{"backward_char", nullptr, key_event::of(named_key::left)},
		{"beginning_of_line", nullptr, key_event::of(named_key::home)},
		{"backward_char", nullptr, key_event::of(named_key::left)},
		{"end_of_line", nullptr, key_event::of(named_key::end)},
		{"delete_backward_char", nullptr, key_event::of(backspace_key)},
		{"delete_backward_word", nullptr, key_event::of(kill_word_key)},
		{"forward_char", nullptr, key_event::of(named_key::right)},
		{"self_insert", "x", key_event::of(U'x')},
		{"undo", nullptr, key_event::of(undo_key)},
		{"undo", nullptr, key_event::of(undo_key)},
	};

	abi_fixture fixture;
	state through_abi;
	state through_keymap;

	for (const move& one : script) {
		const action_result abi =
			one.keys != nullptr ? fixture.type_one(through_abi, one.keys)
			                    : fixture.run(through_abi, one.abi_name);
		EXPECT_EQ(abi.status, LESH_OK) << one.abi_name;

		const effects keymap = step(through_keymap, one.replay);

		EXPECT_EQ(std::string(through_abi.buffer.text()),
		          std::string(through_keymap.buffer.text())) << one.abi_name;
		EXPECT_EQ(through_abi.cursor.byte_offset(), through_keymap.cursor.byte_offset())
			<< one.abi_name;
		EXPECT_EQ(through_abi.gen.value(), through_keymap.gen.value()) << one.abi_name;
		EXPECT_EQ(through_abi.undo.step_count(), through_keymap.undo.step_count())
			<< one.abi_name;
		EXPECT_EQ(abi.produced.size(), keymap.size()) << one.abi_name;
	}

	EXPECT_TRUE(through_abi == through_keymap);
	EXPECT_FALSE(through_abi.buffer.empty());   // and the script did something

	// Redo has no default key, so it is the one built-in the keymap path cannot
	// reach. Through the ABI it is an ordinary action like the other eight.
	const action_result redone = fixture.run(through_abi, "redo");
	EXPECT_EQ(redone.status, LESH_OK);
	EXPECT_FALSE(through_abi == through_keymap);
}

TEST(LeshperAbiBuiltins, MotionIsGraphemeWiseWhereTheEnumPathIsStillScalarWise) {
	// F-3 through #108, and the one place the two paths deliberately DISAGREE.
	// "e" plus a combining acute is one cluster of three bytes; the enum path's
	// text_buffer still steps scalar values and stops between them, which #107
	// wrote down as a placeholder. The ABI asks the editor, and the editor asks
	// the segmenter.
	abi_fixture fixture;
	state s;
	fixture.type_all(s, "x");
	fixture.type_one(s, "e");
	fixture.type_one(s, "\xCC\x81");   // U+0301 COMBINING ACUTE ACCENT
	ASSERT_EQ(std::string(s.buffer.text()), "xe\xCC\x81");
	ASSERT_EQ(s.cursor.byte_offset(), 4u);

	fixture.run(s, "backward_char");
	EXPECT_EQ(s.cursor.byte_offset(), 1u);   // over the whole cluster, not into it

	fixture.run(s, "forward_char");
	EXPECT_EQ(s.cursor.byte_offset(), 4u);

	// The enum path, for contrast, stops between the "e" and its accent.
	state scalar_wise;
	type(scalar_wise, "x");
	step(scalar_wise, key_event::of(U'e'));
	step(scalar_wise, key_event::of(static_cast<char32_t>(0x0301)));
	ASSERT_EQ(std::string(scalar_wise.buffer.text()), "xe\xCC\x81");
	press(scalar_wise, named_key::left);
	EXPECT_EQ(scalar_wise.cursor.byte_offset(), 2u);

	// And through the ABI one backspace takes the whole cluster.
	fixture.run(s, "delete_backward_char");
	EXPECT_EQ(std::string(s.buffer.text()), "x");
}

// ---------------------------------------------------------------------------
// Staging and the atomic commit.
// ---------------------------------------------------------------------------

TEST(LeshperAbiStaging, SixWritesInOneActionAreOneUndoEntryAndOneGenerationBump) {
	// #92 decision 3, A-12: writes stage and the loop commits atomically on
	// return. An action that builds its answer in pieces must not leave the user
	// six undo steps to walk back, and must not make the reactors recompute six
	// times over.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "spell_it_out",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          for (const char* piece : {"h", "e", "l", "l", "o", "!"}) {
			          size_t at = 0;
			          lesh_cursor_get(editor, &at);
			          const int32_t status = lesh_buffer_replace(editor, at, at, piece, 1);
			          if (status != LESH_OK)
				          return status;
		          }
		          return LESH_OK;
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	const action_result out = loop.invoke(s, "spell_it_out",
	                                      invocation{});
	EXPECT_EQ(out.status, LESH_OK);
	EXPECT_EQ(std::string(s.buffer.text()), "hello!");
	EXPECT_EQ(s.gen.value(), 1u);
	EXPECT_EQ(s.undo.step_count(), 1u);
	EXPECT_EQ(count_of<worker_request>(out.produced), 1u);

	// And it undoes as one.
	position cursor = s.cursor;
	EXPECT_TRUE(s.undo.undo(s.buffer, cursor));
	EXPECT_EQ(std::string(s.buffer.text()), "");
}

TEST(LeshperAbiStaging, AnActionThatChangesItsMindChangesNothing) {
	// The other half of staging: the buffer is not touched until the action
	// returns, so writing and then unwriting leaves no undo entry, no generation
	// bump and no redraw. On the enum path there is no way to express this at
	// all - the mutation has already happened.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "second_thoughts",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          lesh_buffer_replace(editor, 0, 0, "oops", 4);
		          return lesh_buffer_replace(editor, 0, 4, nullptr, 0);
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	const action_result out = loop.invoke(s, "second_thoughts",
	                                      invocation{});
	EXPECT_EQ(out.status, LESH_OK);
	EXPECT_TRUE(s.buffer.empty());
	EXPECT_EQ(s.gen.value(), 0u);
	EXPECT_EQ(s.undo.step_count(), 0u);
	EXPECT_TRUE(out.produced.empty());
}

TEST(LeshperAbiStaging, AnActionSeesItsOwnWritesBeforeAnyoneElseDoes) {
	registry reg;
	static std::string seen_inside;
	static std::string seen_outside;
	seen_inside.clear();
	seen_outside = "unset";

	state s;
	ASSERT_EQ(lesh_action_register(&reg, "look",
	          [](lesh_editor* editor, const lesh_invocation*, void* userdata) -> int32_t {
		          lesh_buffer_replace(editor, 0, 0, "staged", 6);
		          seen_inside = abi_buffer(editor);
		          seen_outside = std::string(static_cast<state*>(userdata)->buffer.text());
		          return LESH_OK;
	          }, &s), LESH_OK);

	loop_harness loop(reg);
	loop.invoke(s, "look", invocation{});
	EXPECT_EQ(seen_inside, "staged");
	EXPECT_EQ(seen_outside, "");   // the real buffer was untouched while it ran
	EXPECT_EQ(std::string(s.buffer.text()), "staged");
}

TEST(LeshperAbiStaging, PositionsClampAndWriteRangesSnapToClusterBoundaries) {
	// #93: positions are byte offsets that clamp and snap, so no binding needs
	// grapheme geometry and no binding can leave half a cluster behind. The
	// offsets here are the kind a binding really produces - a byte index a user
	// typed into a variable, or one a regex handed back.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "vandalise",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          // [1, 2) is the middle of a three-byte cluster at both ends.
		          return lesh_buffer_replace(editor, 1, 2, "-", 1);
	          }, nullptr), LESH_OK);
	ASSERT_EQ(lesh_action_register(&reg, "way_past_the_end",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_buffer_replace(editor, 9000, 9001, "!", 1);
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "e\xCC\x81z");
	s.cursor = s.buffer.end_position();

	loop.invoke(s, "vandalise", invocation{});
	// The whole cluster went, and what is left is well formed - not an "e" with
	// a stray combining mark, and not half a UTF-8 sequence.
	EXPECT_EQ(std::string(s.buffer.text()), "-z");
	EXPECT_EQ(s.cursor.byte_offset(), 1u);

	loop.invoke(s, "way_past_the_end", invocation{});
	EXPECT_EQ(std::string(s.buffer.text()), "-z!");
}

TEST(LeshperAbiStaging, TheCursorClampsAndSnapsAtCommit) {
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "aim_badly",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_cursor_set(editor, 2);   // inside "e" + combining acute
	          }, nullptr), LESH_OK);
	ASSERT_EQ(lesh_action_register(&reg, "aim_into_orbit",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_cursor_set(editor, 9000);
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "e\xCC\x81z");

	loop.invoke(s, "aim_badly", invocation{});
	EXPECT_EQ(s.cursor.byte_offset(), 0u);   // back to where the cluster starts
	EXPECT_EQ(s.gen.value(), 0u);            // a cursor move is not a mutation

	loop.invoke(s, "aim_into_orbit", invocation{});
	EXPECT_EQ(s.cursor.byte_offset(), 4u);   // the end of the buffer, not past it
}

TEST(LeshperAbiStaging, SettingTheWholeBufferLeavesTheCursorWhereTheUserPutIt) {
	// `$BUFFER=...` in the lesh binding (F-14), with zle's semantics: replacing
	// the text does not move `$CURSOR`, it only clamps it.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "rewrite",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_buffer_set(editor, "ab", 2);
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "abcdef");
	s.cursor = position::from_byte_offset(1);
	loop.invoke(s, "rewrite", invocation{});
	EXPECT_EQ(std::string(s.buffer.text()), "ab");
	EXPECT_EQ(s.cursor.byte_offset(), 1u);
	EXPECT_EQ(s.undo.step_count(), 1u);
	EXPECT_EQ(s.gen.value(), 1u);
}

TEST(LeshperAbiStaging, ARunOfSelfInsertsThroughTheAbiStillCoalesces) {
	// F-4 survives the trip through the ABI. The loop has no action names to go
	// by, so it decides from what the commit was: a single plain insertion
	// continues the run, and anything else ends it.
	abi_fixture fixture;
	state s;
	fixture.type_all(s, "abc");
	EXPECT_EQ(s.undo.step_count(), 1u);
	fixture.run(s, "backward_char");
	fixture.type_all(s, "d");
	EXPECT_EQ(s.undo.step_count(), 2u);
}

// ---------------------------------------------------------------------------
// Capabilities: outcomes, history, injected input.
// ---------------------------------------------------------------------------

TEST(LeshperAbiOutcomes, LoopOutcomesAreRequestedAndNotReturned) {
	// ADR-0008: the return value is status only, so a new outcome is a new
	// function rather than a signature change. The action goes on running after
	// asking, and the loop reads the request once the writes have committed.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "smart_accept",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          size_t length = 0;
		          lesh_buffer_length(editor, &length);
		          if (length == 0)
			          return LESH_OK;   // nothing to accept, so nothing requested
		          lesh_accept_line(editor);
		          size_t at = 0;
		          lesh_cursor_get(editor, &at);
		          return lesh_buffer_replace(editor, at, at, " # accepted", 11);
	          }, nullptr), LESH_OK);

	state empty;
	loop_harness loop(reg);
	action_result out = loop.invoke(empty, "smart_accept",
	                                invocation{});
	EXPECT_EQ(out.status, LESH_OK);
	EXPECT_EQ(out.outcome, loop_outcome::none);

	state typed;
	typed.buffer.replace(typed.buffer.begin_position(), typed.buffer.begin_position(), "ls");
	typed.cursor = typed.buffer.end_position();
	out = loop.invoke(typed, "smart_accept", invocation{});
	EXPECT_EQ(out.status, LESH_OK);
	EXPECT_EQ(out.outcome, loop_outcome::accept_line);
	// The write it made after asking still landed: asking is not returning.
	EXPECT_EQ(std::string(typed.buffer.text()), "ls # accepted");
}

TEST(LeshperAbiOutcomes, EachOutcomeIsItsOwnFunctionAndTheLastAskWins) {
	registry reg;
	int which = 0;
	ASSERT_EQ(lesh_action_register(&reg, "ask",
	          [](lesh_editor* editor, const lesh_invocation*, void* userdata) -> int32_t {
		          switch (*static_cast<int*>(userdata)) {
		          case 0: return lesh_cancel_line(editor);
		          case 1: return lesh_exit(editor, 7);
		          case 2: return lesh_recursive_edit(editor);
		          default: break;
		          }
		          lesh_cancel_line(editor);
		          return lesh_exit(editor, 3);   // two asks in one action
	          }, &which), LESH_OK);

	state s;
	loop_harness loop(reg);
	const invocation how{};

	EXPECT_EQ(loop.invoke(s, "ask", how).outcome, loop_outcome::cancel_line);
	which = 1;
	action_result out = loop.invoke(s, "ask", how);
	EXPECT_EQ(out.outcome, loop_outcome::exit);
	EXPECT_EQ(out.exit_status, 7);
	which = 2;
	EXPECT_EQ(loop.invoke(s, "ask", how).outcome, loop_outcome::recursive_edit);
	which = 3;
	out = loop.invoke(s, "ask", how);
	EXPECT_EQ(out.outcome, loop_outcome::exit);
	EXPECT_EQ(out.exit_status, 3);
}

TEST(LeshperAbiOutcomes, PositiveStatusPassesThroughAsDomainStatus) {
	// One int32_t space: negatives are the ABI's, positives are the binding's. A
	// user action's exit status crosses without the ABI knowing that shells have
	// exit statuses.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "fails_the_shell_way",
	          [](lesh_editor*, const lesh_invocation*, void*) -> int32_t { return 130; },
	          nullptr), LESH_OK);
	state s;
	loop_harness loop(reg);
	EXPECT_EQ(loop.invoke(s, "fails_the_shell_way",
	                      invocation{}).status, 130);
}

TEST(LeshperAbiOutcomes, HistoryMovementRefusesToShareAnActionWithAnEdit) {
	// Undo restores the buffer to a state the staged edits were never applied
	// to, so the two together are not asking for anything the history can mean.
	// Refused, loudly, rather than resolved by a coin flip.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "edit_then_undo",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          lesh_buffer_replace(editor, 0, 0, "x", 1);
		          return lesh_undo(editor);
	          }, nullptr), LESH_OK);

	abi_fixture fixture;
	state s;
	fixture.type_all(s, "hi");
	loop_harness loop(reg);
	EXPECT_EQ(loop.invoke(s, "edit_then_undo",
	                      invocation{}).status,
	          LESH_ERR_REFUSED);
}

TEST(LeshperAbiOutcomes, UndoingWithNothingToUndoIsNotAnError) {
	abi_fixture fixture;
	state s;
	const action_result out = fixture.run(s, "undo");
	EXPECT_EQ(out.status, LESH_OK);
	EXPECT_EQ(s.gen.value(), 0u);
	EXPECT_TRUE(out.produced.empty());
}

TEST(LeshperAbiOutcomes, PushedInputLandsInPendingAndNotInTheBuffer) {
	// F-7, zle's `zle -U`: pushed bytes are read back as though typed, through
	// the keymap. Splicing them into the buffer would be quicker and would put
	// leshper on a route the user's own bindings never see (A-12).
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "suggest",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_push_input(editor, "ls\x01", 3);
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	loop.invoke(s, "suggest", invocation{});
	EXPECT_TRUE(s.buffer.empty());
	EXPECT_EQ(s.pending.injected, std::string("ls\x01", 3));
}

// ---------------------------------------------------------------------------
// Handles, and the copy-out contract.
// ---------------------------------------------------------------------------

TEST(LeshperAbiHandles, TheHandleGoesDeadWhenTheCallReturns) {
	// ADR-0008: handles are valid only for the call that received them, and
	// stashing one is undefined behaviour asserted in debug builds. What this
	// checks is the assertion's predicate - dying is the enforcement, not the
	// evidence.
	abi_fixture fixture;
	state s;
	EXPECT_FALSE(handle_is_live(fixture.loop.handle()));
	fixture.type_all(s, "hi");
	EXPECT_FALSE(handle_is_live(fixture.loop.handle()));

	static const lesh_editor* stashed = nullptr;
	static bool live_during_the_call = false;
	stashed = nullptr;
	live_during_the_call = false;
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "stash",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          stashed = editor;
		          live_during_the_call = handle_is_live(editor);
		          return LESH_OK;
	          }, nullptr), LESH_OK);
	loop_harness loop(reg);
	loop.invoke(s, "stash", invocation{});
	EXPECT_TRUE(live_during_the_call);
	EXPECT_FALSE(handle_is_live(stashed));
}

TEST(LeshperAbiHandles, ReadingCopiesOutAndSaysHowMuchWouldNotFit) {
	registry reg;
	static int32_t zero_capacity = LESH_OK;
	static int32_t too_small = LESH_OK;
	static size_t needed = 0;
	static std::string slice;
	ASSERT_EQ(lesh_action_register(&reg, "measure",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          zero_capacity = lesh_buffer_get(editor, nullptr, 0, &needed);
		          char cramped[2] = {0, 0};
		          size_t length = 0;
		          too_small = lesh_buffer_get(editor, cramped, sizeof(cramped), &length);
		          char room[8] = {};
		          size_t slice_length = 0;
		          lesh_buffer_read(editor, 2, 5, room, sizeof(room), &slice_length);
		          slice.assign(room, slice_length);
		          return LESH_OK;
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "abcdef");
	loop.invoke(s, "measure", invocation{});
	// Asking with no room is how a caller learns what to allocate, not a failure
	// to answer: the length comes back either way.
	EXPECT_EQ(zero_capacity, LESH_ERR_TOOSMALL);
	EXPECT_EQ(needed, 6u);
	EXPECT_EQ(too_small, LESH_ERR_TOOSMALL);
	EXPECT_EQ(slice, "cde");
}

TEST(LeshperAbiHandles, SelectionAccessorsAreAnHonestPlaceholderUntilTheModelExists) {
	// #96 owns the selection model and has not decided it. The getter answers
	// truthfully - there is no selection - and the setter refuses. The entry
	// points exist so that #96 fills bodies in rather than growing the ABI, and
	// so a binding written against this header today keeps compiling.
	registry reg;
	static int32_t get_status = LESH_OK;
	static int32_t set_status = LESH_OK;
	static int32_t active = 1;
	active = 1;
	ASSERT_EQ(lesh_action_register(&reg, "select",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          size_t from = 0;
		          size_t to = 0;
		          get_status = lesh_selection_get(editor, &from, &to, &active);
		          set_status = lesh_selection_set(editor, 0, 1);
		          return LESH_OK;
	          }, nullptr), LESH_OK);

	state s;
	loop_harness loop(reg);
	loop.invoke(s, "select", invocation{});
	EXPECT_EQ(get_status, LESH_OK);
	EXPECT_EQ(active, 0);
	EXPECT_EQ(set_status, LESH_ERR_REFUSED);
}

// ---------------------------------------------------------------------------
// Reactors: the token is the only mint, and the loop is the only applier.
// ---------------------------------------------------------------------------

namespace {

// The C trampoline half of the Lua sketch on #93, written in C++ against the
// same surface: find "TODO", emit a span over it in an interned semantic style.
int32_t todo_marker(lesh_request* request, void* userdata) {
	const std::string text = token_buffer(request);
	const size_t at = text.find("TODO");
	if (at == std::string::npos)
		return LESH_OK;
	return lesh_emit_span(request, at, at + 4, *static_cast<uint32_t*>(userdata));
}

int32_t count_reactor_calls(lesh_request*, void* userdata) {
	++*static_cast<int*>(userdata);
	return LESH_OK;
}

} // namespace

TEST(LeshperAbiReactors, AReactorSeesTheSnapshotAndEmitsThroughTheToken) {
	registry reg;
	uint32_t todo_style = LESH_STYLE_NONE;
	ASSERT_EQ(lesh_style_intern(&reg, "comment.todo", &todo_style), LESH_OK);
	EXPECT_NE(todo_style, LESH_STYLE_NONE);
	ASSERT_EQ(lesh_reactor_register(&reg, "todo_marker", LESH_EVENT_BUFFER_CHANGED,
	                                todo_marker, &todo_style), LESH_OK);

	state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "# TODO: later");
	s.gen.bump();

	loop_harness loop(reg);
	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_EQ(batches[0].reactor, "todo_marker");
	EXPECT_EQ(batches[0].status, LESH_OK);
	ASSERT_EQ(batches[0].spans.size(), 1u);
	EXPECT_EQ(batches[0].spans[0].start, 2u);
	EXPECT_EQ(batches[0].spans[0].end, 6u);
	EXPECT_EQ(batches[0].spans[0].style_id, todo_style);
	EXPECT_TRUE(batches[0].computed_against == s.gen);
}

TEST(LeshperAbiReactors, OnlyTheReactorsSubscribedToTheEventRun) {
	registry reg;
	int buffer_runs = 0;
	int cursor_runs = 0;
	ASSERT_EQ(lesh_reactor_register(&reg, "on_buffer", LESH_EVENT_BUFFER_CHANGED,
	                                count_reactor_calls, &buffer_runs), LESH_OK);
	ASSERT_EQ(lesh_reactor_register(&reg, "on_cursor", LESH_EVENT_CURSOR_MOVED,
	                                count_reactor_calls, &cursor_runs), LESH_OK);

	state s;
	loop_harness loop(reg);
	loop.react(s, LESH_EVENT_CURSOR_MOVED);
	EXPECT_EQ(buffer_runs, 0);
	EXPECT_EQ(cursor_runs, 1);
	loop.react(s, LESH_EVENT_BUFFER_CHANGED | LESH_EVENT_CURSOR_MOVED);
	EXPECT_EQ(buffer_runs, 1);
	EXPECT_EQ(cursor_runs, 2);
}

TEST(LeshperAbiReactors, AStaleBatchIsNotAppliedBecauseThereIsNowhereToApplyIt) {
	// N-4, in the shape ADR-0008 gave it: staleness is not checked in the
	// reactor, it is UNEXPRESSIBLE. There is no apply function in abi.h, so the
	// only route a batch has is through the loop, and the loop refuses one whose
	// generation has moved on.
	registry reg;
	uint32_t style = LESH_STYLE_NONE;
	ASSERT_EQ(lesh_style_intern(&reg, "comment.todo", &style), LESH_OK);
	ASSERT_EQ(lesh_reactor_register(&reg, "todo_marker", LESH_EVENT_BUFFER_CHANGED,
	                                todo_marker, &style), LESH_OK);

	abi_fixture fixture;
	state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "TODO");
	s.cursor = s.buffer.end_position();
	s.gen.bump();

	loop_harness loop(reg);
	std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);

	// The user typed while the worker was thinking.
	fixture.type_all(s, "!");
	EXPECT_FALSE(loop.apply(s, batches[0]));
	EXPECT_TRUE(loop.applied().empty());

	// Recomputed against the buffer as it now is, the same reactor applies.
	batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_TRUE(loop.apply(s, batches[0]));
	ASSERT_EQ(loop.applied().size(), 1u);
	EXPECT_EQ(loop.applied()[0].spans.size(), 1u);
}

TEST(LeshperAbiReactors, TheSupersededPollIsCooperativeAndTheAnswerIsDroppedAnyway) {
	registry reg;
	state s;
	loop_harness loop(reg);
	static int32_t polled_before = -1;
	static int32_t polled_after = -1;
	polled_before = -1;
	polled_after = -1;
	ASSERT_EQ(lesh_reactor_register(&reg, "slow_thinker", LESH_EVENT_BUFFER_CHANGED,
	          [](lesh_request* request, void* userdata) -> int32_t {
		          lesh_request_superseded(request, &polled_before);
		          static_cast<loop_harness*>(userdata)->supersede();
		          lesh_request_superseded(request, &polled_after);
		          return polled_after != 0 ? LESH_ERR_SUPERSEDED : LESH_OK;
	          }, &loop), LESH_OK);

	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_EQ(polled_before, 0);
	EXPECT_EQ(polled_after, 1);
	// Giving up early is a courtesy to the worker, not a correctness mechanism:
	// a reactor that never polled would have had its answer dropped just the
	// same.
	EXPECT_EQ(batches[0].status, LESH_ERR_SUPERSEDED);
}

TEST(LeshperAbiReactors, TheEmittingReactorIsTheDecorationNamespace) {
	registry reg;
	uint32_t style = LESH_STYLE_NONE;
	ASSERT_EQ(lesh_style_intern(&reg, "comment.todo", &style), LESH_OK);
	ASSERT_EQ(lesh_reactor_register(&reg, "todo_marker", LESH_EVENT_BUFFER_CHANGED,
	                                todo_marker, &style), LESH_OK);
	ASSERT_EQ(lesh_reactor_register(&reg, "autosuggest", LESH_EVENT_BUFFER_CHANGED,
	          [](lesh_request* request, void*) -> int32_t {
		          size_t at = 0;
		          lesh_request_cursor(request, &at);
		          lesh_emit_virtual_text(request, at, " --help", 7);
		          return lesh_propose(request, LESH_PROPOSAL_AUTOSUGGESTION, "TODO --help", 11);
	          }, nullptr), LESH_OK);

	state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "TODO");
	s.cursor = s.buffer.end_position();
	loop_harness loop(reg);

	for (reactor_batch& one : loop.react(s, LESH_EVENT_BUFFER_CHANGED))
		EXPECT_TRUE(loop.apply(s, std::move(one)));
	ASSERT_EQ(loop.applied().size(), 2u);

	// A new batch from one reactor replaces that reactor's and touches nobody
	// else's, which is the whole of what a namespace has to do here.
	for (reactor_batch& one : loop.react(s, LESH_EVENT_BUFFER_CHANGED))
		EXPECT_TRUE(loop.apply(s, std::move(one)));
	EXPECT_EQ(loop.applied().size(), 2u);

	size_t proposals = 0;
	size_t texts = 0;
	size_t spans = 0;
	for (const reactor_batch& one : loop.applied()) {
		proposals += one.proposals.size();
		texts += one.texts.size();
		spans += one.spans.size();
	}
	EXPECT_EQ(proposals, 1u);
	EXPECT_EQ(texts, 1u);
	EXPECT_EQ(spans, 1u);
}

TEST(LeshperAbiReactors, StylesAreInternedSemanticIdsAndInterningIsIdempotent) {
	// F-21: a span carries "comment.todo" and the theme decides what that looks
	// like at render. The reactor never names a colour.
	registry reg;
	uint32_t first = LESH_STYLE_NONE;
	uint32_t again = LESH_STYLE_NONE;
	uint32_t other = LESH_STYLE_NONE;
	EXPECT_EQ(lesh_style_intern(&reg, "comment.todo", &first), LESH_OK);
	EXPECT_EQ(lesh_style_intern(&reg, "comment.todo", &again), LESH_OK);
	EXPECT_EQ(lesh_style_intern(&reg, "command.unknown", &other), LESH_OK);
	EXPECT_EQ(first, again);
	EXPECT_NE(first, other);
	EXPECT_NE(first, LESH_STYLE_NONE);

	char name[32] = {};
	size_t length = 0;
	EXPECT_EQ(lesh_style_name(&reg, first, name, sizeof(name), &length), LESH_OK);
	EXPECT_EQ(std::string(name, length), "comment.todo");
	EXPECT_EQ(lesh_style_name(&reg, 9999, name, sizeof(name), &length), LESH_ERR_NOTFOUND);
	EXPECT_EQ(lesh_style_name(&reg, LESH_STYLE_NONE, name, sizeof(name), &length),
	          LESH_ERR_NOTFOUND);
}

TEST(LeshperAbiReactors, EmittedPositionsClampIntoTheSnapshot) {
	registry reg;
	ASSERT_EQ(lesh_reactor_register(&reg, "wild", LESH_EVENT_BUFFER_CHANGED,
	          [](lesh_request* request, void*) -> int32_t {
		          lesh_emit_span(request, 9000, 9001, LESH_STYLE_NONE);
		          return lesh_emit_span(request, 3, 1, LESH_STYLE_NONE);
	          }, nullptr), LESH_OK);

	state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "abcd");
	loop_harness loop(reg);
	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	ASSERT_EQ(batches[0].spans.size(), 2u);
	EXPECT_EQ(batches[0].spans[0].start, 4u);
	EXPECT_EQ(batches[0].spans[0].end, 4u);
	EXPECT_EQ(batches[0].spans[1].start, 1u);
	EXPECT_EQ(batches[0].spans[1].end, 3u);
}
