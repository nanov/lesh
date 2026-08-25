#include "leshper/abi.h"
#include "leshper/editor.h"
#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "leshper/text.h"
#include "leshper/undo.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using namespace lesh::leshper;

// The selection primitive (#96, spec §6.3): an anchor, a flag, and the cursor
// as the head.
//
// Its own file rather than another thousand lines in leshper_tests.cpp, per the
// per-lane split the map now asks for. What it holds is one model asserted three
// ways: the type and its derived region, what an edit does to it (the marker
// rules), and what the ABI can see of it. Nothing here knows about a mode, a
// shape or a renderer - linewise, blockwise and inclusive-of-last are the
// keymap's projections and they do not exist yet.

namespace {

// Puts text in a buffer without going through a keystroke, so a test can set up
// a selection over known offsets and assert on the region rather than on the
// path that produced it.
void fill(state& s, std::string_view text) {
	s.buffer.replace(s.buffer.begin_position(), s.buffer.end_position(), text);
}

position at(size_t offset) { return position::from_byte_offset(offset); }

std::string text_of(const state& s) { return std::string(s.buffer.text()); }

// The region as a pair of offsets, or "no region". Every assertion below reads
// through this, because the point of a DERIVED region is that a test asks the
// same question a renderer would.
std::optional<std::pair<size_t, size_t>> range_of(const state& s) {
	const std::optional<region> found = s.selection_range();
	if (!found)
		return std::nullopt;
	return std::pair<size_t, size_t>{found->from.byte_offset(), found->to.byte_offset()};
}

constexpr char32_t undo_key = 0x1F;  // Ctrl-_

} // namespace

// ---------------------------------------------------------------------------
// The type: one pair, and the region falls out of it.
// ---------------------------------------------------------------------------

TEST(LeshperSelection, AFreshStateHasAnAnchorAndNoRegion) {
	// The distinction the model is built on: there is ALWAYS an anchor, and the
	// flag is the separate question of whether it means anything. An optional
	// position would have collapsed the two and cost emacs its mark.
	state s;
	EXPECT_FALSE(s.selection_active());
	EXPECT_EQ(s.selection_anchor(), s.buffer.begin_position());
	EXPECT_FALSE(range_of(s).has_value());
}

TEST(LeshperSelection, TheRegionIsDerivedFromTheAnchorAndTheCursor) {
	state s;
	fill(s, "hello world");
	s.cursor = at(5);
	s.set_anchor(at(2));
	// Half-open and sorted: the anchor is at 2 and the head at 5, so the region
	// is [2, 5) - "llo" - and the character at 5 is NOT in it.
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{2, 5}));

	// The cursor is the head, so moving it moves the region. Nothing else was
	// written; this is the whole of what a helix motion needs.
	s.cursor = at(9);
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{2, 9}));
}

TEST(LeshperSelection, TheRegionSortsWhenTheHeadIsBehindTheAnchor) {
	// A backward selection is a real thing and not an error: the pair carries
	// direction, the derived region carries extent, and vi's `o` swaps the one
	// without disturbing the other.
	state s;
	fill(s, "hello world");
	s.cursor = at(8);
	s.set_anchor(at(8));
	s.cursor = at(3);
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{3, 8}));
	EXPECT_EQ(s.selection_anchor(), at(8));
}

TEST(LeshperSelection, AnEmptyRegionIsActiveAndNotAbsent) {
	// `anchor == cursor` with the flag set is an empty selection, which renders
	// as nothing and is still live. A model that reported it as "no selection"
	// would make helix's always-a-selection invariant unrepresentable.
	state s;
	fill(s, "abcdef");
	s.cursor = at(3);
	s.set_anchor(at(3));
	ASSERT_TRUE(range_of(s).has_value());
	EXPECT_TRUE(s.selection_range()->empty());
	EXPECT_TRUE(s.selection_active());
}

TEST(LeshperSelection, DroppingTheRegionKeepsTheAnchor) {
	// Emacs's mark outliving `deactivate-mark`, and the reason the model is a
	// flag beside a position rather than an optional one. Nothing in leshper
	// exercises the return trip yet; the primitive is what #96 decided.
	state s;
	fill(s, "abcdef");
	s.cursor = at(1);
	s.set_anchor(at(4));
	s.drop_selection();
	EXPECT_FALSE(s.selection_active());
	EXPECT_EQ(s.selection_anchor(), at(4));
	EXPECT_FALSE(range_of(s).has_value());
}

TEST(LeshperSelection, SwappingTheAnchorAndTheCursorLeavesTheRegionAlone) {
	// vi visual's `o`: the head and the tail trade places, and the extent does
	// not move. That it is one method and not a mode's arithmetic is the point.
	state s;
	fill(s, "hello world");
	s.cursor = at(2);
	s.set_anchor(at(2));
	s.cursor = at(7);

	const auto before = range_of(s);
	s.swap_anchor_and_cursor();
	EXPECT_EQ(range_of(s), before);
	EXPECT_EQ(s.selection_anchor(), at(7));
	EXPECT_EQ(s.cursor, at(2));
}

TEST(LeshperSelection, SelectionParticipatesInStateEquality) {
	// N-3's replay comparison is one operator over every field. A selection that
	// equality did not see would let a replayed run diverge silently, which is
	// exactly the class of bug the whole-state comparison exists to catch.
	state a;
	state b;
	fill(a, "abc");
	fill(b, "abc");
	EXPECT_TRUE(a == b);

	a.set_anchor(at(1));
	EXPECT_FALSE(a == b);
	b.set_anchor(at(1));
	EXPECT_TRUE(a == b);

	a.drop_selection();
	EXPECT_FALSE(a == b);  // same anchor, different flag
	b.drop_selection();
	EXPECT_TRUE(a == b);
}

// ---------------------------------------------------------------------------
// The marker rules: what every edit does to the anchor (#96 decision 4).
// ---------------------------------------------------------------------------

TEST(LeshperSelectionMarkerRules, AnEditAfterTheAnchorLeavesItAlone) {
	state s;
	fill(s, "hello world");
	s.cursor = at(2);
	s.set_anchor(at(2));
	apply_edit(s, at(6), at(11), "there");
	EXPECT_EQ(s.selection_anchor(), at(2));
	EXPECT_TRUE(s.selection_active());
}

TEST(LeshperSelectionMarkerRules, AnEditBeforeTheAnchorShiftsItByTheDelta) {
	state s;
	fill(s, "hello world");
	s.cursor = at(11);
	s.set_anchor(at(6));

	// "hello" (5 bytes) becomes "hi" (2): everything after moves back three.
	apply_edit(s, at(0), at(5), "hi");
	EXPECT_EQ(text_of(s), "hi world");
	EXPECT_EQ(s.selection_anchor(), at(3));

	// And the other direction: a longer replacement pushes it forward.
	apply_edit(s, at(0), at(2), "hello");
	EXPECT_EQ(text_of(s), "hello world");
	EXPECT_EQ(s.selection_anchor(), at(6));
}

TEST(LeshperSelectionMarkerRules, AnAnchorInsideTheReplacedSpanClampsToTheEditStart) {
	state s;
	fill(s, "hello world");
	s.cursor = at(0);
	s.set_anchor(at(8));  // inside "world"

	apply_edit(s, at(6), at(11), "x");
	EXPECT_EQ(text_of(s), "hello x");
	EXPECT_EQ(s.selection_anchor(), at(6));
	// The flag survives an edit that ate the region: a collapsed selection
	// renders as nothing and is still live (#96 decision 4).
	EXPECT_TRUE(s.selection_active());
}

TEST(LeshperSelectionMarkerRules, AnEditEndingExactlyAtTheAnchorShiftsIt) {
	// The half-open boundary case on the near side: the replaced span is
	// [3, 6) and the anchor is at 6, so the edit is entirely before it.
	state s;
	fill(s, "abcdefghi");
	s.cursor = at(0);
	s.set_anchor(at(6));
	apply_edit(s, at(3), at(6), "");
	EXPECT_EQ(text_of(s), "abcghi");
	EXPECT_EQ(s.selection_anchor(), at(3));
}

TEST(LeshperSelectionMarkerRules, InsertingAtTheAnchorLeavesItPutSoTheRegionGrows) {
	// The gravity question, and the one case the three rules do not settle on
	// their own: at a pure insertion `begin == end == anchor`. The anchor stays,
	// which is emacs's default marker insertion type, and it is what makes typing
	// into an empty active region grow it rather than leave it empty forever.
	state s;
	fill(s, "ab");
	s.cursor = at(1);
	s.set_anchor(at(1));
	ASSERT_TRUE(s.selection_range()->empty());

	apply_edit(s, at(1), at(1), "XY");
	EXPECT_EQ(text_of(s), "aXYb");
	EXPECT_EQ(s.selection_anchor(), at(1));
	EXPECT_EQ(s.cursor, at(3));
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{1, 3}));
}

TEST(LeshperSelectionMarkerRules, AnInactiveAnchorStillTracksTheBuffer) {
	// The mark is not frozen when the region goes away. An anchor that stopped
	// following edits while inactive would point into text that no longer exists
	// the moment the region came back.
	state s;
	fill(s, "hello world");
	s.cursor = at(0);
	s.set_anchor(at(6));
	s.drop_selection();

	apply_edit(s, at(0), at(5), "hi");
	EXPECT_EQ(s.selection_anchor(), at(3));
	EXPECT_FALSE(s.selection_active());
}

TEST(LeshperSelectionMarkerRules, TypingCarriesTheRegionThroughTheKeymapPath) {
	// The same rules, reached the way a user reaches them: through step() and the
	// placeholder keymap, with no test poking apply_edit directly.
	state s;
	fill(s, "world");
	s.cursor = at(0);
	s.set_anchor(at(0));
	s.cursor = at(5);
	ASSERT_EQ(range_of(s), (std::pair<size_t, size_t>{0, 5}));

	// The cursor is at the end, so typing is an insertion after the anchor and
	// before the head: the region grows with it.
	step(s, key_event::of(U'!'));
	EXPECT_EQ(text_of(s), "world!");
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{0, 6}));
}

TEST(LeshperSelectionMarkerRules, TheAnchorStaysOnABoundaryThroughMultiByteEdits) {
	// F-3's invariant on the anchor, held positively rather than by a death test:
	// the debug assertion in state.h fires on a violation, and this walks every
	// boundary of a multi-byte buffer and shows there is none to fire on. The
	// repo's own precedent is asserting the PREDICATE (registry.h's
	// handle_is_live) rather than making dying the only evidence.
	//
	// a, é (two bytes), 中 (three), b.
	const std::string_view mixed = "a\xC3\xA9" "\xE4\xB8\xAD" "b";

	for (position anchor = at(0);; ) {
		state s;
		fill(s, mixed);
		s.cursor = s.buffer.end_position();
		s.set_anchor(anchor);

		// An edit that replaces one multi-byte cluster with another of a different
		// length, entirely at the front, so every anchor past it has to shift.
		apply_edit(s, at(1), at(3), "\xE4\xB8\xAD");
		EXPECT_TRUE(s.buffer.is_boundary(s.selection_anchor()))
			<< "anchor left a cluster in halves after an edit";

		ASSERT_TRUE(s.undo_one());
		EXPECT_EQ(s.selection_anchor(), anchor);

		if (anchor == s.buffer.end_position())
			break;
		anchor = s.buffer.next_position(anchor);
	}
}

// ---------------------------------------------------------------------------
// Undo carries the selection (F-4, #96 decision 4).
// ---------------------------------------------------------------------------

TEST(LeshperSelectionUndo, UndoRestoresTheRegionAlongsideTheCursor) {
	state s;
	fill(s, "hello world");
	s.cursor = at(0);
	s.set_anchor(at(8));

	apply_edit(s, at(6), at(11), "x");
	ASSERT_EQ(s.selection_anchor(), at(6));

	ASSERT_TRUE(s.undo_one());
	EXPECT_EQ(text_of(s), "hello world");
	EXPECT_EQ(s.selection_anchor(), at(8));
	EXPECT_TRUE(s.selection_active());

	ASSERT_TRUE(s.redo_one());
	EXPECT_EQ(text_of(s), "hello x");
	EXPECT_EQ(s.selection_anchor(), at(6));
}

TEST(LeshperSelectionUndo, UndoRestoresTheFlagAndNotOnlyTheAnchor) {
	// The half a test that only checked positions would miss: an edit made while
	// the region was live comes back live, and one made while it was not does
	// not come back live.
	state s;
	fill(s, "abcdef");
	s.cursor = at(6);
	s.drop_selection();
	apply_edit(s, at(6), at(6), "g");

	s.set_anchor(at(2));
	ASSERT_TRUE(s.selection_active());

	ASSERT_TRUE(s.undo_one());
	EXPECT_EQ(text_of(s), "abcdef");
	EXPECT_FALSE(s.selection_active());
}

TEST(LeshperSelectionUndo, ACoalescedTypingRunUndoesToWhereItBegan) {
	// Typing collapses into one step (F-4), and the merged record has to carry
	// the selection from the START of the run - the state the user watched it
	// begin from - not from whichever keystroke happened to land last.
	state s;
	s.set_anchor(at(0));
	ASSERT_TRUE(s.selection_active());

	for (const char byte : std::string_view{"abc"})
		step(s, key_event::of(static_cast<char32_t>(byte)));
	ASSERT_EQ(text_of(s), "abc");
	ASSERT_EQ(s.undo.step_count(), 1u);
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{0, 3}));

	step(s, key_event::of(undo_key));
	EXPECT_EQ(text_of(s), "");
	EXPECT_EQ(s.selection_anchor(), at(0));
	EXPECT_TRUE(s.selection_active());
}

// ---------------------------------------------------------------------------
// The ABI, backed for real (#110's stubs, filled).
// ---------------------------------------------------------------------------

namespace {

// Where an action's reads land, so a test can assert on what the action saw
// rather than only on what the state ended up as. Statics because the C
// function pointers an action registers through carry no captures.
struct seen {
	int32_t status = LESH_OK;
	size_t from = 0;
	size_t to = 0;
	int32_t active = -1;
};

seen observed;

} // namespace

TEST(LeshperSelectionAbi, AnActionSeesTheDerivedRegion) {
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "look",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          observed.status =
			          lesh_selection_get(editor, &observed.from, &observed.to, &observed.active);
		          return LESH_OK;
	          }, nullptr), LESH_OK);

	state s;
	fill(s, "hello world");
	s.cursor = at(9);
	s.set_anchor(at(3));

	loop_harness loop(reg);
	observed = seen{};
	ASSERT_EQ(loop.invoke(s, "look", invocation{}).status, LESH_OK);
	EXPECT_EQ(observed.status, LESH_OK);
	EXPECT_EQ(observed.from, 3u);
	EXPECT_EQ(observed.to, 9u);
	EXPECT_EQ(observed.active, 1);
}

TEST(LeshperSelectionAbi, SettingTheRegionPutsTheHeadOnItsEnd) {
	// The head IS the cursor, so there is nowhere else for `end` to go. A setter
	// that left the cursor behind would leave the state describing a different
	// region than the one it was handed.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "select_middle",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_selection_set(editor, 2, 7);
	          }, nullptr), LESH_OK);

	state s;
	fill(s, "hello world");
	loop_harness loop(reg);
	ASSERT_EQ(loop.invoke(s, "select_middle", invocation{}).status, LESH_OK);
	EXPECT_TRUE(s.selection_active());
	EXPECT_EQ(s.selection_anchor(), at(2));
	EXPECT_EQ(s.cursor, at(7));
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{2, 7}));
}

TEST(LeshperSelectionAbi, AReversedPairIsADirectionAndNotAnError) {
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "select_backward",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_selection_set(editor, 7, 2);
	          }, nullptr), LESH_OK);

	state s;
	fill(s, "hello world");
	loop_harness loop(reg);
	ASSERT_EQ(loop.invoke(s, "select_backward", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.selection_anchor(), at(7));
	EXPECT_EQ(s.cursor, at(2));
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{2, 7}));
}

TEST(LeshperSelectionAbi, OffsetsClampAndSnapToClusterBoundaries) {
	// #93's clamp-and-snap, now applied to the selection: a binding hands in
	// byte offsets it got from a regex match or a variable a user typed, and no
	// such offset may leave a cluster in halves. Snapped BACK on both ends,
	// because a selection endpoint is a cursor-like position that rests on a
	// cluster - unlike a replacement range, which must swallow whole ones.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "select_ragged",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_selection_set(editor, 2, 900);
	          }, nullptr), LESH_OK);

	state s;
	// a, é (two bytes at 1..3), 中 (three at 3..6), b.
	fill(s, "a\xC3\xA9" "\xE4\xB8\xAD" "b");
	loop_harness loop(reg);
	ASSERT_EQ(loop.invoke(s, "select_ragged", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.selection_anchor(), at(1));  // 2 is inside é, snapped back to 1
	EXPECT_EQ(s.cursor, at(7));              // 900 clamped to the end
}

TEST(LeshperSelectionAbi, ClearingLeavesTheAnchorWhereItWas) {
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "deselect",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_selection_clear(editor);
	          }, nullptr), LESH_OK);

	state s;
	fill(s, "hello world");
	s.cursor = at(1);
	s.set_anchor(at(4));

	loop_harness loop(reg);
	ASSERT_EQ(loop.invoke(s, "deselect", invocation{}).status, LESH_OK);
	EXPECT_FALSE(s.selection_active());
	EXPECT_EQ(s.selection_anchor(), at(4));
}

TEST(LeshperSelectionAbi, AnActionThatEditsWithoutTouchingTheSelectionGetsTheMarkerRules) {
	// The staging seam, from the other side: the action wrote only the buffer, so
	// the anchor is carried across by apply_edit at commit and NOT overwritten by
	// a staged copy the action never meant as a selection.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "shorten",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          return lesh_buffer_replace(editor, 0, 5, "hi", 2);
	          }, nullptr), LESH_OK);

	state s;
	fill(s, "hello world");
	s.cursor = at(11);
	s.set_anchor(at(6));

	loop_harness loop(reg);
	ASSERT_EQ(loop.invoke(s, "shorten", invocation{}).status, LESH_OK);
	EXPECT_EQ(text_of(s), "hi world");
	EXPECT_EQ(s.selection_anchor(), at(3));
	EXPECT_TRUE(s.selection_active());
}

TEST(LeshperSelectionAbi, AnActionThatSetsTheSelectionAfterEditingMeansTheStagedOffsets) {
	// And the case the staging exists for: the action edited and THEN set a
	// region, so its offsets are against the text it left behind. Committing the
	// staged anchor as-is is the only reading under which the action got what it
	// asked for; running the marker rules over it a second time would move it.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "edit_then_select",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          const int32_t status = lesh_buffer_replace(editor, 0, 5, "hi", 2);
		          if (status != LESH_OK)
			          return status;
		          return lesh_selection_set(editor, 3, 8);  // "world" in "hi world"
	          }, nullptr), LESH_OK);

	state s;
	fill(s, "hello world");
	loop_harness loop(reg);
	ASSERT_EQ(loop.invoke(s, "edit_then_select", invocation{}).status, LESH_OK);
	EXPECT_EQ(text_of(s), "hi world");
	EXPECT_EQ(range_of(s), (std::pair<size_t, size_t>{3, 8}));
}

TEST(LeshperSelectionAbi, AReactorsTokenCarriesTheRegionInItsSnapshot) {
	// The token carries buffer, cursor, selection and generation and nothing
	// else. Until now the selection third of that was a zero.
	registry reg;
	ASSERT_EQ(lesh_reactor_register(&reg, "watcher", LESH_EVENT_SELECTION_CHANGED,
	          [](lesh_request* request, void*) -> int32_t {
		          observed.status =
			          lesh_request_selection(request, &observed.from, &observed.to,
			                                 &observed.active);
		          return LESH_OK;
	          }, nullptr), LESH_OK);

	state s;
	fill(s, "hello world");
	s.cursor = at(2);
	s.set_anchor(at(9));

	loop_harness loop(reg);
	observed = seen{};
	const auto batches = loop.react(s, LESH_EVENT_SELECTION_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_EQ(observed.status, LESH_OK);
	EXPECT_EQ(observed.from, 2u);  // sorted, even though the head is behind
	EXPECT_EQ(observed.to, 9u);
	EXPECT_EQ(observed.active, 1);
}

TEST(LeshperSelectionAbi, UndoThroughTheAbiResyncsTheStagedSelection) {
	// An action that undoes and then reads must see the selection that came back,
	// not the one the call started with.
	registry reg;
	ASSERT_EQ(lesh_action_register(&reg, "undo_and_look",
	          [](lesh_editor* editor, const lesh_invocation*, void*) -> int32_t {
		          const int32_t status = lesh_undo(editor);
		          if (status != LESH_OK)
			          return status;
		          observed.status =
			          lesh_selection_get(editor, &observed.from, &observed.to, &observed.active);
		          return LESH_OK;
	          }, nullptr), LESH_OK);

	state s;
	fill(s, "hello world");
	s.cursor = at(0);
	s.set_anchor(at(8));
	apply_edit(s, at(6), at(11), "x");
	ASSERT_EQ(s.selection_anchor(), at(6));

	loop_harness loop(reg);
	observed = seen{};
	ASSERT_EQ(loop.invoke(s, "undo_and_look", invocation{}).status, LESH_OK);
	EXPECT_EQ(text_of(s), "hello world");
	EXPECT_EQ(s.selection_anchor(), at(8));
	EXPECT_EQ(observed.from, 0u);  // the head is at 0, the anchor back at 8
	EXPECT_EQ(observed.to, 8u);
	EXPECT_EQ(observed.active, 1);
}
