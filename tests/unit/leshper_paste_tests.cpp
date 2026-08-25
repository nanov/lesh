#include "leshper/decode.h"
#include "leshper/editor.h"
#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/state.h"
#include "leshper/undo.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace lesh::leshper;

// #121: step()'s paste_event branch. F-6's "one buffer mutation, one undo
// step, one generation bump, one redraw" is proven to the event by #111
// (decoder tests); this file proves it to the editor - the payload lands
// through the one apply_edit, and coalescing never folds a paste into a
// typing run on either side.

namespace {

// Mirrors the helpers in leshper_tests.cpp (a separate translation unit, so a
// separate anonymous namespace - #121's file ownership keeps this suite out
// of the shared file rather than out of the shared conventions).
effects type(state& s, std::string_view text) {
	effects all;
	for (const char byte : text) {
		effects one = step(s, key_event::of(static_cast<char32_t>(static_cast<unsigned char>(byte))));
		all.insert(all.end(), one.begin(), one.end());
	}
	return all;
}

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

constexpr char32_t undo_key = 0x1F; // Ctrl-_

} // namespace

// ---------------------------------------------------------------------------
// One paste, one mutation, one undo entry (F-6, F-4).
// ---------------------------------------------------------------------------

TEST(LeshperPaste, OnePasteIsOneUndoEntryThatRestoresTheWholeThingAndTheCursor) {
	state s;
	const generation before_gen = s.gen;

	const effects produced = step(s, paste_event{"pasted text"});

	EXPECT_EQ(text_of(s), "pasted text");
	// The cursor lands after the inserted text (apply_edit's default landing).
	EXPECT_EQ(cursor_of(s), std::string_view{"pasted text"}.size());
	// ONE generation bump, not one per character.
	EXPECT_FALSE(s.gen == before_gen);
	// ONE undo entry for the whole payload.
	EXPECT_EQ(s.undo.step_count(), 1u);
	// A redraw, mirroring what an insert emits.
	EXPECT_GE(count_of<render_request>(produced), 1u);

	// ONE undo restores the whole thing and the cursor together (F-4).
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "");
	EXPECT_EQ(cursor_of(s), 0u);
}

TEST(LeshperPaste, PasteAtACursorInTheMiddleLandsThereAndAfter) {
	state s;
	type(s, "echo ");
	const size_t before = cursor_of(s);

	step(s, paste_event{"hello world"});

	EXPECT_EQ(text_of(s), "echo hello world");
	EXPECT_EQ(cursor_of(s), before + std::string_view{"hello world"}.size());
}

// ---------------------------------------------------------------------------
// Coalescing never folds a paste into a typing run, on either side (F-4, F-6).
// ---------------------------------------------------------------------------

TEST(LeshperPaste, TypingThenPasteThenTypingIsThreeUndoSteps) {
	state s;
	type(s, "ab");
	step(s, paste_event{"XY"});
	type(s, "cd");

	EXPECT_EQ(text_of(s), "abXYcd");
	// Three steps: the "ab" run, the paste (its own step on both sides), and
	// the "cd" run - none of the three folds into a neighbour.
	EXPECT_EQ(s.undo.step_count(), 3u);

	press(s, undo_key);
	EXPECT_EQ(text_of(s), "abXY");
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "ab");
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "");
}

TEST(LeshperPaste, ConsecutivePastesAreSeparateUndoSteps) {
	// Two pastes back to back are each their own step too - the break after
	// the first paste must not leave the history looking like it is mid-typing
	// run for the second.
	state s;
	step(s, paste_event{"one"});
	step(s, paste_event{"two"});

	EXPECT_EQ(text_of(s), "onetwo");
	EXPECT_EQ(s.undo.step_count(), 2u);

	press(s, undo_key);
	EXPECT_EQ(text_of(s), "one");
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "");
}

// ---------------------------------------------------------------------------
// A pasted newline is a newline, not Enter (F-6).
// ---------------------------------------------------------------------------

TEST(LeshperPaste, ANewlineInThePasteIsALiteralNewlineNotEnter) {
	// The event is a mutation, not a key: F-35's accept-or-insert decision for
	// a typed Enter never runs, and there is no Enter action in the placeholder
	// keymap for it to run through even if it tried. A newline byte lands in
	// the buffer exactly like any other character in the payload.
	state s;
	type(s, "echo one");

	step(s, paste_event{"\ntwo"});

	EXPECT_EQ(text_of(s), "echo one\ntwo");
	// Still one mutation for the whole paste, newline included.
	EXPECT_EQ(s.undo.step_count(), 2u);
	press(s, undo_key);
	EXPECT_EQ(text_of(s), "echo one");
}

// ---------------------------------------------------------------------------
// Replayability (N-3): the decoder's bracketed-paste bytes, through step(),
// with no terminal anywhere.
// ---------------------------------------------------------------------------

TEST(LeshperPasteReplay, DecodedBracketedPasteDrivesTheEditorEndToEnd) {
	// `ESC [ 200 ~ ... ESC [ 201 ~`, the #97-floor bracketed-paste markers
	// #111's decoder resolves into one paste_event, fed straight into step()
	// alongside the typed keys around it.
	input_decoder decoder;
	std::vector<event> events;
	input_instant now{};
	decoder.feed("echo \x1B[200~pasted text\x1B[201~ done", now, events);
	ASSERT_FALSE(decoder.holding());

	state s;
	for (const event& one : events)
		step(s, one);

	EXPECT_EQ(text_of(s), "echo pasted text done");
	// Three steps: "echo " typed, the paste, " done" typed - the decoder and
	// the editor agree the paste never joins either neighbour.
	EXPECT_EQ(s.undo.step_count(), 3u);
}

TEST(LeshperPasteReplay, APasteSplitAcrossReadsStillLandsAsOneEditorMutation) {
	// Where the read boundaries fell is not part of the input (N-3): the same
	// paste, fed to the decoder one byte at a time, must still reach the
	// editor as the one paste_event #111 promises.
	input_decoder decoder;
	std::vector<event> events;
	input_instant now{};
	static constexpr std::string_view bytes = "\x1B[200~split across reads\x1B[201~";
	for (const char byte : bytes)
		decoder.feed(std::string_view(&byte, 1), now, events);
	ASSERT_FALSE(decoder.holding());

	state s;
	for (const event& one : events)
		step(s, one);

	EXPECT_EQ(text_of(s), "split across reads");
	EXPECT_EQ(s.undo.step_count(), 1u);
}
