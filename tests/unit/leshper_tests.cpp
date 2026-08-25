#include "leshper/editor.h"
#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/state.h"
#include "leshper/undo.h"

#include <gtest/gtest.h>

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
