#include "leshper/abi.h"
#include "leshper/blit.h"
#include "leshper/decode.h"
#include "leshper/editor.h"
#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/keymap.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "leshper/undo.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
	// F-13: nothing unnamed, nothing unrebindable. The name is what #110's
	// registry keys on and what #118's keymaps hold - there is no enum between
	// them any more, so the name a key resolves to IS the registered name.
	state s;
	editing_context& context = context_of(s);
	const keymap* emacs = context.keymaps().find(keymap_registry::emacs);
	ASSERT_NE(emacs, nullptr);

	const std::string* left = emacs->action_for(encode_key(key_event::of(named_key::left)));
	ASSERT_NE(left, nullptr);
	EXPECT_EQ(*left, "backward_char");

	int32_t exists = 0;
	EXPECT_EQ(lesh_action_exists(&context.actions(), left->c_str(), &exists), LESH_OK);
	EXPECT_EQ(exists, 1);

	// A printable is bound by nothing and typed by the floor, which is the one
	// binding that is a rule rather than a row.
	EXPECT_EQ(emacs->action_for(encode_key(key_event::of(U'x'))), nullptr);
	EXPECT_TRUE(is_self_inserting(key_event::of(U'x')));
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
	ASSERT_TRUE(s.redo_one());
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

// ---------------------------------------------------------------------------
// Input decoding: bytes to events (F-5, F-6, N-4, #111).
//
// The same rule as everything above, one level lower: no terminal, no clock, no
// syscall. A test hands the decoder bytes and instants and asserts on the events
// that come back, which is the whole reason the class takes both as arguments.
// ---------------------------------------------------------------------------

namespace {

using namespace std::chrono_literals;

std::string_view spelled(named_key key) noexcept {
	switch (key) {
	case named_key::backspace:
		return "backspace";
	case named_key::left:
		return "left";
	case named_key::right:
		return "right";
	case named_key::home:
		return "home";
	case named_key::end:
		return "end";
	case named_key::up:
		return "up";
	case named_key::down:
		return "down";
	case named_key::page_up:
		return "page-up";
	case named_key::page_down:
		return "page-down";
	case named_key::insert:
		return "insert";
	case named_key::delete_forward:
		return "delete";
	case named_key::f1:
		return "f1";
	case named_key::f2:
		return "f2";
	case named_key::f3:
		return "f3";
	case named_key::f4:
		return "f4";
	case named_key::f5:
		return "f5";
	case named_key::f6:
		return "f6";
	case named_key::f7:
		return "f7";
	case named_key::f8:
		return "f8";
	case named_key::f9:
		return "f9";
	case named_key::f10:
		return "f10";
	case named_key::f11:
		return "f11";
	case named_key::f12:
		return "f12";
	}
	return "?";
}

std::string hex_of(char32_t value) {
	static constexpr char digits[] = "0123456789ABCDEF";
	const unsigned raw = static_cast<unsigned>(value);
	std::string text;
	for (int shift = 20; shift >= 0; shift -= 4) {
		const unsigned nibble = (raw >> shift) & 0xF;
		if (!text.empty() || nibble != 0 || shift <= 12)
			text.push_back(digits[nibble]);
	}
	return text;
}

// One readable line per event, so a failing sequence assertion says what came
// out rather than dumping a variant's bytes.
std::string describe(const event& one) {
	if (const auto* key = std::get_if<key_event>(&one)) {
		std::string text;
		if (key->modifiers.ctrl)
			text += "C-";
		if (key->modifiers.alt)
			text += "M-";
		if (key->modifiers.shift)
			text += "S-";
		if (key->named)
			return text + std::string(spelled(key->key));
		return text + "U+" + hex_of(key->codepoint);
	}
	if (const auto* pasted = std::get_if<paste_event>(&one))
		return "paste[" + pasted->text + "]";
	return "other";
}

std::vector<std::string> described(const std::vector<event>& all) {
	std::vector<std::string> lines;
	lines.reserve(all.size());
	for (const event& one : all)
		lines.push_back(describe(one));
	return lines;
}

// The event loop's side of the contract, in three lines: feed what was read,
// let time pass, let the timeout resolve what it resolves.
struct driver {
	input_decoder decoder;
	std::vector<event> events;
	input_instant now{};

	void feed(std::string_view bytes) { decoder.feed(bytes, now, events); }

	// One byte per read, which is what a slow pty actually does.
	void feed_byte_at_a_time(std::string_view bytes) {
		for (size_t at = 0; at < bytes.size(); ++at)
			feed(bytes.substr(at, 1));
	}

	void wait(std::chrono::milliseconds elapsed) {
		now += elapsed;
		decoder.expire(now, events);
	}

	[[nodiscard]] std::vector<std::string> seen() const { return described(events); }
};

using lines = std::vector<std::string>;

} // namespace

// ---------------------------------------------------------------------------
// Incremental UTF-8 (F-5) and malformed bytes (N-4).
// ---------------------------------------------------------------------------

TEST(LeshperDecodeUtf8, AsciiIsOneKeyPerByte) {
	driver d;
	d.feed("ls -l");
	EXPECT_EQ(d.seen(), (lines{"U+006C", "U+0073", "U+0020", "U+002D", "U+006C"}));
	EXPECT_FALSE(d.decoder.holding());
	EXPECT_FALSE(d.decoder.deadline().has_value());
}

TEST(LeshperDecodeUtf8, ControlCharactersArriveAsCodepoints) {
	// event.h's rule, asserted at its producer: Ctrl-W is U+0017 and DEL is
	// U+007F, not a second spelling of a named key.
	driver d;
	d.feed("\x17\x7F\x01");
	EXPECT_EQ(d.seen(), (lines{"U+0017", "U+007F", "U+0001"}));
}

TEST(LeshperDecodeUtf8, ACodepointSplitAcrossReadsDecodesOnceAndCorrectly) {
	// The headline of F-5. U+4E2D arrives one byte per read; nothing is emitted
	// until the last byte, and then exactly one key.
	driver d;
	d.feed("\xE4");
	EXPECT_TRUE(d.events.empty());
	EXPECT_TRUE(d.decoder.holding());
	// A partial character waits on more bytes, never on the clock.
	EXPECT_FALSE(d.decoder.deadline().has_value());

	d.feed("\xB8");
	EXPECT_TRUE(d.events.empty());

	d.feed("\xAD");
	EXPECT_EQ(d.seen(), (lines{"U+4E2D"}));
	EXPECT_FALSE(d.decoder.holding());
}

TEST(LeshperDecodeUtf8, AFourByteCodepointSurvivesEverySplit) {
	// U+1F600, split at each of the three interior points.
	static constexpr std::string_view grinning = "\xF0\x9F\x98\x80";
	for (size_t at = 1; at < grinning.size(); ++at) {
		driver d;
		d.feed(grinning.substr(0, at));
		EXPECT_TRUE(d.events.empty()) << "split after " << at;
		d.feed(grinning.substr(at));
		EXPECT_EQ(d.seen(), (lines{"U+1F600"})) << "split after " << at;
	}
}

TEST(LeshperDecodeUtf8, MalformedBytesDegradeOnePerMaximalSubpart) {
	// N-4, by the Unicode-recommended rule: a truncated three-byte sequence
	// followed by an ASCII letter is ONE replacement and then the letter, not
	// two replacements, and not one that swallowed the letter.
	driver d;
	d.feed("\xE4\xB8" "a");
	EXPECT_EQ(d.seen(), (lines{"U+FFFD", "U+0061"}));
}

TEST(LeshperDecodeUtf8, AStrayContinuationByteIsOneReplacement) {
	driver d;
	d.feed("a\x80" "b");
	EXPECT_EQ(d.seen(), (lines{"U+0061", "U+FFFD", "U+0062"}));
}

TEST(LeshperDecodeUtf8, OverlongsSurrogatesAndOutOfRangeAreAllRejected) {
	// Each of these decodes to something real if the continuation bytes are
	// trusted, which is exactly why they are rejected at the lead and second
	// bytes: 0xC0 0x80 is an overlong NUL, 0xED 0xA0 0x80 is U+D800, and
	// 0xF4 0x90 0x80 0x80 is past U+10FFFF.
	driver overlong;
	overlong.feed("\xC0\x80");
	EXPECT_EQ(overlong.seen(), (lines{"U+FFFD", "U+FFFD"}));

	driver surrogate;
	surrogate.feed("\xED\xA0\x80");
	EXPECT_EQ(surrogate.seen(), (lines{"U+FFFD", "U+FFFD", "U+FFFD"}));

	driver too_high;
	too_high.feed("\xF4\x90\x80\x80");
	EXPECT_EQ(too_high.seen(), (lines{"U+FFFD", "U+FFFD", "U+FFFD", "U+FFFD"}));

	// And the boundary just below it still decodes.
	driver highest;
	highest.feed("\xF4\x8F\xBF\xBF");
	EXPECT_EQ(highest.seen(), (lines{"U+10FFFF"}));
}

TEST(LeshperDecodeUtf8, WellFormedLeavesValidTextAlone) {
	const std::string valid = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";
	EXPECT_EQ(well_formed(valid), valid);
	EXPECT_EQ(well_formed("a\xC3(b"), "a\xEF\xBF\xBD(b");
	// A sequence truncated at the very end has nothing more coming.
	EXPECT_EQ(well_formed("a\xE4\xB8"), "a\xEF\xBF\xBD");
}

// ---------------------------------------------------------------------------
// ESC-prefix disambiguation and the #97 floor's sequences (F-5).
// ---------------------------------------------------------------------------

TEST(LeshperDecodeEscape, BareEscapeResolvesOnlyWhenTheTimeoutExpires) {
	driver d;
	d.feed("\x1B");
	EXPECT_TRUE(d.events.empty());
	ASSERT_TRUE(d.decoder.deadline().has_value());

	d.wait(default_escape_timeout - 1ms);
	EXPECT_TRUE(d.events.empty()) << "resolved early: a sequence still in flight would be lost";

	d.wait(1ms);
	EXPECT_EQ(d.seen(), (lines{"U+001B"}));
	EXPECT_FALSE(d.decoder.deadline().has_value());
	EXPECT_FALSE(d.decoder.holding());
}

TEST(LeshperDecodeEscape, AnArrowArrivingBeforeTheTimeoutIsAnArrow) {
	driver d;
	d.feed("\x1B");
	d.wait(default_escape_timeout - 1ms);
	d.feed("[A");
	EXPECT_EQ(d.seen(), (lines{"up"}));
	EXPECT_FALSE(d.decoder.deadline().has_value());

	// And the timeout that would have fired finds nothing left to resolve.
	d.wait(10ms);
	EXPECT_EQ(d.seen(), (lines{"up"}));
}

TEST(LeshperDecodeEscape, ATimedOutPrefixKeepsItsTail) {
	// `ESC [` with no final byte: the ESC was the Escape key and the `[` was
	// typed. Dropping the tail would lose a keystroke.
	driver d;
	d.feed("\x1B[");
	d.wait(default_escape_timeout);
	EXPECT_EQ(d.seen(), (lines{"U+001B", "U+005B"}));
	EXPECT_FALSE(d.decoder.holding());
}

TEST(LeshperDecodeEscape, TheDeadlineIsAnchoredToTheFirstEscapeByte) {
	// Bytes trickling in one at a time must not push the deadline out forever.
	driver d;
	d.feed("\x1B");
	const input_instant first = *d.decoder.deadline();
	d.now += 10ms;
	d.feed("[");
	ASSERT_TRUE(d.decoder.deadline().has_value());
	EXPECT_EQ(*d.decoder.deadline(), first);
}

TEST(LeshperDecodeEscape, EscapeThenACharacterIsAlt) {
	driver d;
	d.feed("\x1B" "a");
	EXPECT_EQ(d.seen(), (lines{"M-U+0061"}));
	EXPECT_FALSE(d.decoder.deadline().has_value());
}

TEST(LeshperDecodeEscape, AltCarriesAMultiByteCharacterSplitAcrossReads) {
	driver d;
	d.feed("\x1B\xE4\xB8");
	EXPECT_TRUE(d.events.empty());
	d.feed("\xAD");
	EXPECT_EQ(d.seen(), (lines{"M-U+4E2D"}));
}

TEST(LeshperDecodeEscape, EscapeEscapeIsOneEscapeAndAFreshStart) {
	driver d;
	d.feed("\x1B\x1B[A");
	EXPECT_EQ(d.seen(), (lines{"U+001B", "up"}));
}

TEST(LeshperDecodeEscape, ArrowsAndHomeEndInBothCsiAndSs3) {
	// Which spelling a terminal sends depends on its keypad mode, and #97's
	// assume-first rule means both are hardcoded rather than looked up.
	driver csi;
	csi.feed("\x1B[A\x1B[B\x1B[C\x1B[D\x1B[H\x1B[F");
	EXPECT_EQ(csi.seen(), (lines{"up", "down", "right", "left", "home", "end"}));

	driver ss3;
	ss3.feed("\x1BOA\x1BOB\x1BOC\x1BOD\x1BOH\x1BOF");
	EXPECT_EQ(ss3.seen(), (lines{"up", "down", "right", "left", "home", "end"}));

	driver tilde;
	tilde.feed("\x1B[1~\x1B[4~\x1B[2~\x1B[3~\x1B[5~\x1B[6~");
	EXPECT_EQ(tilde.seen(),
	          (lines{"home", "end", "insert", "delete", "page-up", "page-down"}));
}

TEST(LeshperDecodeEscape, FunctionKeysAcrossTheFloorsThreeSpellings) {
	driver d;
	d.feed("\x1BOP\x1BOQ\x1BOR\x1BOS");
	EXPECT_EQ(d.seen(), (lines{"f1", "f2", "f3", "f4"}));

	driver numbered;
	numbered.feed("\x1B[15~\x1B[17~\x1B[21~\x1B[23~\x1B[24~");
	EXPECT_EQ(numbered.seen(), (lines{"f5", "f6", "f10", "f11", "f12"}));

	driver lettered;
	lettered.feed("\x1B[P\x1B[1;1Q");
	EXPECT_EQ(lettered.seen(), (lines{"f1", "f2"}));
}

TEST(LeshperDecodeEscape, ModifiersComeFromTheSecondCsiParameter) {
	driver d;
	d.feed("\x1B[1;5C\x1B[1;2A\x1B[1;3D\x1B[1;6B\x1B[3;5~");
	EXPECT_EQ(d.seen(),
	          (lines{"C-right", "S-up", "M-left", "C-S-down", "C-delete"}));
}

TEST(LeshperDecodeEscape, ASequenceSplitAtEveryPointDecodesTheSameWay) {
	static constexpr std::string_view sequence = "\x1B[1;5C";
	for (size_t at = 1; at < sequence.size(); ++at) {
		driver d;
		d.feed(sequence.substr(0, at));
		EXPECT_TRUE(d.events.empty()) << "split after " << at;
		d.feed(sequence.substr(at));
		EXPECT_EQ(d.seen(), (lines{"C-right"})) << "split after " << at;
	}
}

TEST(LeshperDecodeEscape, SequencesAboveTheFloorAreConsumedNeverTyped) {
	// A mouse report, a private-mode reply, an unknown final byte. Each is
	// swallowed whole; none of it reaches the line as text. The `x` proves the
	// decoder resynchronised rather than dropping everything after.
	driver d;
	d.feed("\x1B[<0;12;34M" "x");
	EXPECT_EQ(d.seen(), (lines{"U+0078"}));

	driver unknown;
	unknown.feed("\x1B[99;99W" "y");
	EXPECT_EQ(unknown.seen(), (lines{"U+0079"}));

	driver unknown_ss3;
	unknown_ss3.feed("\x1BOZ" "z");
	EXPECT_EQ(unknown_ss3.seen(), (lines{"U+007A"}));
}

TEST(LeshperDecodeEscape, AnAbortedSequenceReleasesTheByteThatAbortedIt) {
	// A Ctrl-C that raced a half-sent sequence must still reach the editor.
	driver d;
	d.feed("\x1B[12\x03");
	EXPECT_EQ(d.seen(), (lines{"U+0003"}));
	EXPECT_FALSE(d.decoder.holding());
}

// ---------------------------------------------------------------------------
// Bracketed paste (F-6).
// ---------------------------------------------------------------------------

TEST(LeshperDecodePaste, AWholePasteIsExactlyOneEvent) {
	driver d;
	d.feed("\x1B[200~hello world\x1B[201~");
	EXPECT_EQ(d.seen(), (lines{"paste[hello world]"}));
	EXPECT_EQ(d.events.size(), 1u) << "F-6: one mutation, one undo step, one redraw";
	EXPECT_FALSE(d.decoder.holding());
}

TEST(LeshperDecodePaste, ThePayloadIsTextEvenWhenItLooksLikeKeys) {
	// The whole point of bracketed paste: a newline in a paste is a newline in
	// the buffer, not Enter, and an escape sequence in a paste is characters.
	driver d;
	d.feed("\x1B[200~one\ntwo\x1B[A\x03\x1B[201~");
	EXPECT_EQ(d.events.size(), 1u);
	const auto* pasted = std::get_if<paste_event>(&d.events.front());
	ASSERT_NE(pasted, nullptr);
	EXPECT_EQ(pasted->text, "one\ntwo\x1B[A\x03");
}

TEST(LeshperDecodePaste, APasteSplitAcrossReadsIsStillOneEvent) {
	driver d;
	d.feed("\x1B[200~ab");
	EXPECT_TRUE(d.events.empty());
	d.feed("cd\x1B[");
	EXPECT_TRUE(d.events.empty()) << "the terminator's first bytes must be held back";
	d.feed("201~");
	EXPECT_EQ(d.seen(), (lines{"paste[abcd]"}));
}

TEST(LeshperDecodePaste, APasteArrivingOneByteAtATimeIsStillOneEvent) {
	driver d;
	// Every boundary in the payload AND in both markers falls between reads.
	d.feed_byte_at_a_time("\x1B[200~abc\x1B[201~");
	EXPECT_EQ(d.seen(), (lines{"paste[abc]"}));
}

TEST(LeshperDecodePaste, APasteInFlightNeverTimesOut) {
	// F-6's payload may take many reads; a deadline would cut it in half.
	driver d;
	d.feed("\x1B[200~half a paste");
	EXPECT_FALSE(d.decoder.deadline().has_value());
	EXPECT_TRUE(d.decoder.holding());

	d.wait(10 * default_escape_timeout);
	EXPECT_TRUE(d.events.empty());

	d.feed(" and the rest\x1B[201~");
	EXPECT_EQ(d.seen(), (lines{"paste[half a paste and the rest]"}));
}

TEST(LeshperDecodePaste, MalformedBytesInAPasteDegradeTheSameWayTypedOnesDo) {
	driver d;
	d.feed("\x1B[200~a\xC3(b\x1B[201~");
	EXPECT_EQ(d.seen(), (lines{"paste[a\xEF\xBF\xBD(b]"}));
}

TEST(LeshperDecodePaste, AnEmptyPasteIsStillOneEventAndAClosingMarkerAloneIsNone) {
	driver empty;
	empty.feed("\x1B[200~\x1B[201~");
	EXPECT_EQ(empty.seen(), (lines{"paste[]"}));

	driver stray;
	stray.feed("\x1B[201~" "x");
	EXPECT_EQ(stray.seen(), (lines{"U+0078"}));
}

TEST(LeshperDecodePaste, ResetDropsAPasteInFlight) {
	// #98 hands the terminal to a child and takes it back; bytes in flight
	// belonged to the child.
	driver d;
	d.feed("\x1B[200~abandoned");
	d.decoder.reset();
	EXPECT_FALSE(d.decoder.holding());
	d.feed("x\x1B[201~");
	EXPECT_EQ(d.seen(), (lines{"U+0078"}));
}

// ---------------------------------------------------------------------------
// Replayability (N-3): the property the whole shape was chosen for.
// ---------------------------------------------------------------------------

namespace {

// One session's worth of bytes: typing, a multi-byte character, arrows in both
// spellings, a modified key, a paste with an escape sequence inside it, a
// malformed byte, and a kill-word. Ends on a complete key, so nothing is left
// waiting on the clock.
constexpr std::string_view recorded_bytes =
    "ls \xE4\xB8\xAD\x1B[D\x1B[1;5C\x1BOP\x1B[200~pasted \x1B[A text\x1B[201~\x80\x17";

} // namespace

TEST(LeshperDecodeReplay, TheSameBytesYieldTheSameEvents) {
	driver first;
	first.feed(recorded_bytes);
	driver second;
	second.feed(recorded_bytes);
	EXPECT_EQ(first.seen(), second.seen());
	EXPECT_FALSE(first.events.empty());
}

TEST(LeshperDecodeReplay, ChunkingDoesNotChangeTheEvents) {
	// Where the read boundaries fell is not part of the input. A recorded byte
	// log replayed in one gulp must decode to what the live session decoded
	// across however many read()s it took.
	driver whole;
	whole.feed(recorded_bytes);

	driver dribbled;
	dribbled.feed_byte_at_a_time(recorded_bytes);

	driver halved;
	halved.feed(recorded_bytes.substr(0, recorded_bytes.size() / 2));
	halved.feed(recorded_bytes.substr(recorded_bytes.size() / 2));

	EXPECT_EQ(dribbled.seen(), whole.seen());
	EXPECT_EQ(halved.seen(), whole.seen());
	EXPECT_FALSE(whole.decoder.holding());
}

TEST(LeshperDecodeReplay, DecodedKeysDriveTheEditorWithNoTerminalAnywhere) {
	// The seam #111 exists to close: bytes in one end, buffer text out the
	// other, and not a file descriptor in the process. Paste is absent here
	// because leshper_paste_tests.cpp (#121) drives it end to end; this asserts
	// only what the decoder promises.
	driver d;
	d.feed("echo \xE4\xB8\xAD\x1B[D" "x");

	state s;
	for (const event& one : d.events)
		step(s, one);

	EXPECT_EQ(std::string(s.buffer.text()), "echo x\xE4\xB8\xAD");
}


// ---------------------------------------------------------------------------
// The surface, the blitter and the diff (#112, F-37, N-3).
//
// The split is the ticket's, and it is the point of the design: SURFACE tests
// assert on cell grids and never look at a byte, BLITTER tests assert on the
// byte stream and never build a grid by hand. Nothing below opens a terminal;
// the blitter returns bytes and #98's loop is what writes them.
// ---------------------------------------------------------------------------

namespace {

// The glyphs of one row, concatenated. Continuation columns contribute
// nothing - the cluster to their left already spoke for them.
std::string glyphs_of(const cluster_pool& pool, const surface& painted, uint16_t row) {
	std::string out;
	for (uint16_t column = 0; column < painted.columns(); ++column) {
		const cell& one = painted.at(row, column);
		if (!one.glyph.is_continuation())
			out.append(pool.cluster_of(one.glyph));
	}
	return out;
}

// Named so a test reads as text rather than as escaped bytes.
constexpr std::string_view CJK_MIDDLE = "\xE4\xB8\xAD";  // U+4E2D, two columns
constexpr std::string_view CJK_SUN = "\xE6\x97\xA5";     // U+65E5, two columns
constexpr std::string_view E_ACUTE = "e\xCC\x81";        // e + U+0301, one cluster
constexpr std::string_view COMBINING_ACUTE = "\xCC\x81"; // U+0301 with no base
constexpr std::string_view FAMILY =
	"\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA6";      // woman ZWJ boy
constexpr std::string_view AMBIGUOUS = "\xC2\xA1";       // U+00A1, UAX #11 Ambiguous

} // namespace

// ---------------------------------------------------------------------------
// The cell (#97 decision 4).
// ---------------------------------------------------------------------------

TEST(LeshperCell, IsTheSmallPodTheDifferCanMemcmp) {
	// #97's constraint, checked at runtime as well as by the static_asserts in
	// surface.h: two cells built independently from the same inputs have the
	// same sixteen bytes. If a padding hole ever appears, memcmp starts
	// comparing uninitialised memory and every row looks changed.
	cluster_pool pool;
	const style pen{color::of_rgb(1, 2, 3), color::of_index(4), attribute::bold};

	surface first{2, 1};
	surface second{2, 1};
	first.write(pool, 0, 0, "x", pen);
	second.write(pool, 0, 0, "x", pen);

	EXPECT_EQ(std::memcmp(&first.at(0, 0), &second.at(0, 0), sizeof(cell)), 0);
	EXPECT_TRUE(first.at(0, 0) == second.at(0, 0));
	EXPECT_EQ(sizeof(cell), 16u);
}

TEST(LeshperCell, StoresWhatTheThemeAuthoredNotWhatTheTerminalCanShow) {
	// The other half of #97 decision 4: the cell is always truecolor-valued and
	// the surface never quantizes. A theme is authored once, and the 256-colour
	// answer is computed at emit time by the one piece of code that owns it.
	cluster_pool pool;
	surface painted{2, 1};
	painted.write(pool, 0, 0, "x",
	              style{color::of_rgb(200, 100, 50), color::of_default(), attribute::none});

	const color kept = painted.at(0, 0).pen.fg;
	EXPECT_EQ(kept.kind, color_kind::truecolor);
	EXPECT_EQ(kept.r, 200);
	EXPECT_EQ(kept.g, 100);
	EXPECT_EQ(kept.b, 50);
}

// ---------------------------------------------------------------------------
// The cluster pool.
// ---------------------------------------------------------------------------

TEST(LeshperClusterPool, GivesEqualClustersEqualIdsAcrossSurfaces) {
	// The property the whole differ rests on. Two surfaces interned against one
	// pool compare cell for cell; that is why the pool is a separate object a
	// caller holds rather than a member of `surface`.
	cluster_pool pool;
	EXPECT_EQ(pool.intern(FAMILY), pool.intern(FAMILY));
	EXPECT_NE(pool.intern(FAMILY), pool.intern(CJK_MIDDLE));
	EXPECT_EQ(pool.cluster_of(pool.intern(FAMILY)), FAMILY);
}

TEST(LeshperClusterPool, SeedsTheTwoIdsTheCellFixesAtCompileTime) {
	cluster_pool pool;
	EXPECT_EQ(pool.intern(""), grapheme_ref::continuation());
	EXPECT_EQ(pool.intern(" "), grapheme_ref::blank());
	EXPECT_EQ(pool.intern("a"), grapheme_ref::of_ascii('a'));
	EXPECT_TRUE(pool.cluster_of(grapheme_ref::continuation()).empty());
	EXPECT_EQ(pool.cluster_of(grapheme_ref::blank()), " ");
	// A default cell is a blank, which is what makes a cleared surface a
	// constant rather than a pass over the pool.
	EXPECT_EQ(cell{}.glyph, grapheme_ref::blank());
	EXPECT_EQ(blank_cell.width, 1);
}

TEST(LeshperClusterPool, InterningIsIdempotentSoTheTableDoesNotGrow) {
	cluster_pool pool;
	const size_t seeded = pool.size();
	const grapheme_ref first = pool.intern(CJK_MIDDLE);
	const size_t after_first = pool.size();
	EXPECT_EQ(after_first, seeded + 1);

	EXPECT_EQ(pool.intern(CJK_MIDDLE), first);
	EXPECT_EQ(pool.intern("z"), grapheme_ref::of_ascii('z'));
	EXPECT_EQ(pool.size(), after_first);  // ASCII never allocates an entry
}

TEST(LeshperClusterPool, StaysCorrectWhenItGrowsPastItsSeededEntries) {
	// The reason the storage is a deque. The map is keyed by a VIEW of the
	// stored bytes, and a cluster short enough for the small-string buffer keeps
	// its bytes inside the stored object - so storage that relocated its
	// elements would leave every key in the map dangling, and this test running
	// under ASan is what says so.
	cluster_pool pool;
	std::vector<std::string> clusters;
	std::vector<grapheme_ref> refs;
	for (int i = 0; i < 512; ++i) {
		clusters.push_back("x" + std::to_string(i));
		refs.push_back(pool.intern(clusters.back()));
	}

	for (size_t i = 0; i < clusters.size(); ++i) {
		EXPECT_EQ(pool.intern(clusters[i]), refs[i]);
		EXPECT_EQ(pool.cluster_of(refs[i]), clusters[i]);
	}
	EXPECT_EQ(pool.size(), 129u + clusters.size());
}

// ---------------------------------------------------------------------------
// The surface: cell grids, never bytes (N-3).
// ---------------------------------------------------------------------------

TEST(LeshperSurface, StartsBlankAndResizeClears) {
	cluster_pool pool;
	surface painted{4, 2};
	EXPECT_EQ(painted.columns(), 4);
	EXPECT_EQ(painted.rows(), 2);
	EXPECT_EQ(glyphs_of(pool, painted, 0), "    ");
	EXPECT_TRUE(painted.at(1, 3) == blank_cell);

	painted.write(pool, 0, 0, "hi", style{});
	painted.resize(3, 1);
	EXPECT_EQ(painted.columns(), 3);
	EXPECT_EQ(glyphs_of(pool, painted, 0), "   ");

	painted.write(pool, 0, 0, "hi", style{});
	painted.clear();
	EXPECT_EQ(glyphs_of(pool, painted, 0), "   ");
}

TEST(LeshperSurface, LaysAsciiOneCellPerColumnAndCarriesTheStyle) {
	cluster_pool pool;
	surface painted{6, 1};
	const style pen{color::of_index(4), color::of_rgb(9, 9, 9),
	                attribute::bold | attribute::italic};

	EXPECT_EQ(painted.write(pool, 0, 1, "abc", pen), 4);
	EXPECT_EQ(glyphs_of(pool, painted, 0), " abc  ");
	for (uint16_t column = 1; column <= 3; ++column) {
		EXPECT_EQ(painted.at(0, column).width, 1);
		EXPECT_TRUE(painted.at(0, column).pen == pen);
	}
	EXPECT_TRUE(painted.at(0, 0) == blank_cell);
	EXPECT_TRUE(painted.at(0, 4) == blank_cell);
}

TEST(LeshperSurface, AWideClusterOwnsTwoColumnsAndTheSecondIsAContinuation) {
	cluster_pool pool;
	surface painted{5, 1};
	EXPECT_EQ(painted.write(pool, 0, 0, CJK_MIDDLE, style{}), 2);

	EXPECT_EQ(pool.cluster_of(painted.at(0, 0).glyph), CJK_MIDDLE);
	EXPECT_EQ(painted.at(0, 0).width, 2);
	EXPECT_TRUE(painted.at(0, 1).glyph.is_continuation());
	EXPECT_EQ(painted.at(0, 1).width, 0);
	// The column index IS the screen column - the invariant the blitter and
	// every test below depend on.
	EXPECT_TRUE(painted.at(0, 2) == blank_cell);
}

TEST(LeshperSurface, ACombiningMarkStaysInsideItsBasesCell) {
	// N-4: a combining mark is part of its base's cluster, so it is part of its
	// base's CELL. Summing per-codepoint widths would have made this two.
	cluster_pool pool;
	surface painted{4, 1};
	EXPECT_EQ(painted.write(pool, 0, 0, E_ACUTE, style{}), 1);
	EXPECT_EQ(pool.cluster_of(painted.at(0, 0).glyph), E_ACUTE);
	EXPECT_EQ(painted.at(0, 0).width, 1);
	EXPECT_TRUE(painted.at(0, 1) == blank_cell);
}

TEST(LeshperSurface, AZwjSequenceIsOneClusterInTwoColumns) {
	// The half of the problem #108 says no surveyed library solves: summing the
	// codepoints of woman-ZWJ-boy gives four columns where a terminal draws two,
	// and the cursor ends up two cells from where the user can see it.
	cluster_pool pool;
	surface painted{6, 1};
	EXPECT_EQ(painted.write(pool, 0, 0, FAMILY, style{}), 2);
	EXPECT_EQ(pool.cluster_of(painted.at(0, 0).glyph), FAMILY);
	EXPECT_EQ(painted.at(0, 0).width, 2);
	EXPECT_TRUE(painted.at(0, 1).glyph.is_continuation());
}

TEST(LeshperSurface, WidthComesFromThePolicySeamAndNotFromAHardcodedAnswer) {
	// #108 decision 3, and all this ticket owes it: the answer is not hardcoded
	// anywhere unreachable. Pass a policy that says UAX #11 Ambiguous is two
	// columns - what a CJK locale's terminal shows - and the grid changes with
	// nothing in the renderer changing.
	cluster_pool pool;
	surface narrow{4, 1};
	surface wide{4, 1};

	lesh::grapheme::width_policy cjk;
	cjk.ambiguous = 2;

	EXPECT_EQ(narrow.write(pool, 0, 0, AMBIGUOUS, style{}), 1);
	EXPECT_EQ(wide.write(pool, 0, 0, AMBIGUOUS, style{}, cjk), 2);
	EXPECT_EQ(narrow.at(0, 0).width, 1);
	EXPECT_EQ(wide.at(0, 0).width, 2);
	EXPECT_TRUE(wide.at(0, 1).glyph.is_continuation());
}

TEST(LeshperSurface, ClipsAtTheRightEdgeAndNeverWraps) {
	// Where a line breaks is layout (F-37) and reflow is F-38. This ring leaves
	// the seam and does not build it, so text that runs off the edge stops.
	cluster_pool pool;
	surface painted{3, 2};
	EXPECT_EQ(painted.write(pool, 0, 0, "abcd", style{}), 3);
	EXPECT_EQ(glyphs_of(pool, painted, 0), "abc");
	EXPECT_EQ(glyphs_of(pool, painted, 1), "   ");  // nothing wrapped onto row 1
}

TEST(LeshperSurface, AWideClusterThatWouldStraddleTheEdgeIsNotWritten) {
	cluster_pool pool;
	surface painted{2, 1};
	EXPECT_EQ(painted.write(pool, 0, 0, std::string("a").append(CJK_MIDDLE), style{}), 1);
	EXPECT_EQ(pool.cluster_of(painted.at(0, 0).glyph), "a");
	EXPECT_TRUE(painted.at(0, 1) == blank_cell);
}

TEST(LeshperSurface, ControlCharactersAreNotPainted) {
	// HOW a control is DISPLAYED - `^C`, a tab expanded to the next stop - is an
	// editor decision. What the surface guarantees is that a control byte never
	// reaches the blitter's output, where it would corrupt the stream.
	cluster_pool pool;
	surface painted{6, 1};
	EXPECT_EQ(painted.write(pool, 0, 0, "a\tb\nc", style{}), 3);
	EXPECT_EQ(glyphs_of(pool, painted, 0), "abc   ");
}

TEST(LeshperSurface, AZeroWidthClusterJoinsTheCellBeforeItOrIsDropped) {
	// A mark with no base of its own. The terminal would attach it to whatever
	// precedes it, so the surface does too - otherwise a column index stops
	// being a screen column. With nothing to attach to, it is dropped.
	cluster_pool pool;

	surface joined{4, 1};
	// The tab breaks the cluster, so the acute arrives on its own.
	joined.write(pool, 0, 0, std::string("a\t").append(COMBINING_ACUTE).append("b"), style{});
	EXPECT_EQ(pool.cluster_of(joined.at(0, 0).glyph), std::string("a").append(COMBINING_ACUTE));
	EXPECT_EQ(joined.at(0, 0).width, 1);
	EXPECT_EQ(pool.cluster_of(joined.at(0, 1).glyph), "b");

	surface orphaned{4, 1};
	orphaned.write(pool, 0, 0, std::string(COMBINING_ACUTE).append("ab"), style{});
	EXPECT_EQ(glyphs_of(pool, orphaned, 0), "ab  ");
}

TEST(LeshperSurface, EqualityCoversTheGridTheSizeAndTheCursor) {
	cluster_pool pool;
	surface first{4, 1};
	surface second{4, 1};
	EXPECT_TRUE(first == second);

	first.write(pool, 0, 0, "a", style{});
	EXPECT_FALSE(first == second);
	second.write(pool, 0, 0, "a", style{});
	EXPECT_TRUE(first == second);

	first.write(pool, 0, 0, "a", style{color::of_index(2), color::of_default(), attribute::none});
	EXPECT_FALSE(first == second);  // same glyph, different pen
	second.write(pool, 0, 0, "a", style{color::of_index(2), color::of_default(), attribute::none});

	first.cursor().column = 2;
	EXPECT_FALSE(first == second);
	second.cursor().column = 2;
	EXPECT_TRUE(first == second);

	first.resize(5, 1);
	second.resize(4, 1);
	EXPECT_FALSE(first == second);
}

// ---------------------------------------------------------------------------
// The blitter: bytes, and only here (#97, F-37).
// ---------------------------------------------------------------------------

TEST(LeshperBlitter, NothingChangedIsNoBytes) {
	cluster_pool pool;
	surface before{10, 2};
	before.write(pool, 0, 0, "echo hi", style{});
	const surface after = before;

	EXPECT_EQ(blitter{pool}.update(before, after), "");
}

TEST(LeshperBlitter, OneChangedCellMovesThereAndWritesOnlyIt) {
	cluster_pool pool;
	surface before{5, 1};
	surface after = before;
	after.write(pool, 0, 2, "x", style{});

	// Relative movement, never absolute: the surface is not the screen (F-39
	// scrolls shell output above it), so a row number would be a lie.
	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[2Cx\r");
}

TEST(LeshperBlitter, ARowThatGotShorterIsClearedNotSpacedOut) {
	// fish's trick: ESC[K says in three bytes what a run of spaces says in a
	// screen width. Deleting to end of line must not cost a repaint.
	cluster_pool pool;
	surface before{10, 1};
	before.write(pool, 0, 0, "hello", style{});
	surface after{10, 1};
	after.write(pool, 0, 0, "he", style{});

	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[2C\x1b[K\r");
}

TEST(LeshperBlitter, EmitsOneSgrPerStyleRun) {
	cluster_pool pool;
	surface before{5, 1};
	surface after = before;
	after.write(pool, 0, 0, "ab",
	            style{color::of_index(1), color::of_default(), attribute::bold});

	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[1;38;5;1mab\x1b[0m\r");
}

TEST(LeshperBlitter, TurningAnAttributeOffResetsRatherThanGuessing) {
	// SGR 22 clears bold and dim together, so "dim off, bold still on" is not
	// expressible as one parameter. Reset and restate is the only correct move.
	cluster_pool pool;
	surface before{5, 1};
	surface after = before;
	after.write(pool, 0, 0, "a",
	            style{color::of_default(), color::of_default(), attribute::bold});
	after.write(pool, 0, 1, "b", style{});

	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[1ma\x1b[0mb\r");
}

TEST(LeshperBlitter, EmitsTruecolorWhenTheTerminalHasItAndQuantizesWhenItDoesNot) {
	// #97: truecolor is opportunistic, the 256-colour downmap lives here and
	// only here, and the surface is identical in both cases.
	cluster_pool pool;
	surface before{3, 1};
	surface after = before;
	after.write(pool, 0, 0, "x",
	            style{color::of_rgb(255, 0, 0), color::of_default(), attribute::none});

	terminal_capabilities rich;
	rich.colors = color_depth::truecolor;
	EXPECT_EQ((blitter{pool, rich}.update(before, after)), "\x1b[38;2;255;0;0mx\x1b[0m\r");
	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[38;5;196mx\x1b[0m\r");
}

TEST(LeshperBlitter, AnIndexedColourPassesThroughUntouchedAtEveryDepth) {
	// An indexed colour names a slot in the user's palette, which the user may
	// have redefined. Resolving it to RGB would replace their choice with ours.
	cluster_pool pool;
	surface before{3, 1};
	surface after = before;
	after.write(pool, 0, 0, "x",
	            style{color::of_default(), color::of_index(33), attribute::none});

	terminal_capabilities rich;
	rich.colors = color_depth::truecolor;
	EXPECT_EQ((blitter{pool, rich}.update(before, after)), "\x1b[48;5;33mx\x1b[0m\r");
	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[48;5;33mx\x1b[0m\r");
}

TEST(LeshperBlitter, MonochromeEmitsNoColourAtAll) {
	cluster_pool pool;
	surface before{3, 1};
	surface after = before;
	after.write(pool, 0, 0, "x",
	            style{color::of_rgb(255, 0, 0), color::of_index(4), attribute::none});

	terminal_capabilities plain;
	plain.colors = color_depth::monochrome;
	EXPECT_EQ((blitter{pool, plain}.update(before, after)), "x\r");
}

TEST(LeshperBlitter, UndercurlIsOpportunisticAndDegradesToAnUnderline) {
	cluster_pool pool;
	surface before{3, 1};
	surface after = before;
	after.write(pool, 0, 0, "x",
	            style{color::of_default(), color::of_default(), attribute::undercurl});

	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[4mx\x1b[0m\r");

	terminal_capabilities rich;
	rich.undercurl = true;
	EXPECT_EQ((blitter{pool, rich}.update(before, after)), "\x1b[4:3mx\x1b[0m\r");
}

TEST(LeshperBlitter, SkipsTheContinuationColumnOfAWideCluster) {
	// The wide glyph moved the terminal's cursor across both columns already;
	// emitting anything for the second would overwrite the right half.
	cluster_pool pool;
	surface before{5, 1};
	before.write(pool, 0, 0, CJK_MIDDLE, style{});
	surface after{5, 1};
	after.write(pool, 0, 0, CJK_MIDDLE,
	            style{color::of_default(), color::of_default(), attribute::bold});

	EXPECT_EQ(blitter{pool}.update(before, after),
	          std::string("\x1b[1m").append(CJK_MIDDLE).append("\x1b[0m\r"));
}

TEST(LeshperBlitter, ChangingAWideClusterReemitsItWhole) {
	cluster_pool pool;
	surface before{6, 1};
	before.write(pool, 0, 0, std::string("a").append(CJK_MIDDLE), style{});
	surface after{6, 1};
	after.write(pool, 0, 0, std::string("a").append(CJK_SUN), style{});

	EXPECT_EQ(blitter{pool}.update(before, after),
	          std::string("\x1b[1C").append(CJK_SUN).append("\r"));
}

TEST(LeshperBlitter, ACarriageReturnFollowsAGlyphInTheLastColumn) {
	// A glyph written into the last column leaves the terminal in pending wrap,
	// and whether the cursor has already moved to the next row is not knowable
	// from here. `\r` is the one sequence that settles it.
	cluster_pool pool;
	surface before{2, 2};
	surface after = before;
	after.write(pool, 0, 0, "ab", style{});
	after.write(pool, 1, 0, "c", style{});
	after.cursor() = cursor_placement{1, 1, true};

	EXPECT_EQ(blitter{pool}.update(before, after), "ab\r\x1b[1Bc");
}

TEST(LeshperBlitter, LeavesTheCursorWhereTheSurfaceSays) {
	cluster_pool pool;
	surface before{5, 3};
	surface after = before;
	after.cursor() = cursor_placement{1, 3, true};

	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[1B\x1b[3C");
}

TEST(LeshperBlitter, ChangesCursorVisibilityOnlyWhenItChanges) {
	cluster_pool pool;
	surface before{5, 1};
	surface after = before;
	EXPECT_EQ(blitter{pool}.update(before, after), "");

	after.cursor().visible = false;
	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[?25l");
	EXPECT_EQ(blitter{pool}.update(after, before), "\x1b[?25h");
}

TEST(LeshperBlitter, ADifferentSizeIsAFullRepaint) {
	// F-37: full repaint only on resize or explicit request. What a resize does
	// to CONTENT is F-38's reflow; diffing two grids of different shapes would
	// be inventing half of that answer here.
	cluster_pool pool;
	surface before{5, 1};
	before.write(pool, 0, 0, "hello", style{});
	surface after{3, 1};

	EXPECT_EQ(blitter{pool}.update(before, after), "\x1b[K");
}

TEST(LeshperBlitter, PaintWritesEveryRowAndClearsTheRest) {
	cluster_pool pool;
	surface painted{4, 2};
	painted.write(pool, 0, 0, "hi", style{});

	EXPECT_EQ(blitter{pool}.paint(painted), "hi\x1b[K\r\x1b[1B\x1b[K\x1b[1A");
}

TEST(LeshperBlitter, TheIntoFormAppendsSoTheLoopCanReuseOneBuffer) {
	// N-2: the hot path keeps one buffer. The returning forms are the
	// convenience, not the primitive.
	cluster_pool pool;
	surface before{5, 1};
	surface after = before;
	after.write(pool, 0, 0, "x", style{});

	std::string out = "keep:";
	blitter{pool}.update_into(before, after, out);
	EXPECT_EQ(out, "keep:x\r");
}

TEST(LeshperBlitter, NeverEmitsAControlByteOtherThanEscapeOrCarriageReturn) {
	cluster_pool pool;
	surface painted{8, 2};
	painted.write(pool, 0, 0, std::string("a\tb\n").append(CJK_MIDDLE), style{});
	painted.write(pool, 1, 0, FAMILY,
	              style{color::of_rgb(1, 2, 3), color::of_index(9),
	                    attribute::bold | attribute::undercurl});

	for (const char byte : blitter{pool}.paint(painted)) {
		const auto value = static_cast<unsigned char>(byte);
		if (value < 0x20)
			EXPECT_TRUE(value == 0x1B || value == '\r') << "control byte " << int(value);
	}
}

// ---------------------------------------------------------------------------
// Quantization, and the capability reads that decide whether it runs.
// ---------------------------------------------------------------------------

TEST(LeshperBlitter, QuantizesIntoTheCubeAndTheGreyRampButNeverThePalette) {
	EXPECT_EQ(quantize_to_256(255, 0, 0), 196);     // 16 + 36*5
	EXPECT_EQ(quantize_to_256(255, 255, 255), 231); // the cube's white corner
	EXPECT_EQ(quantize_to_256(0, 0, 0), 16);        // never 0: that slot is the user's
	EXPECT_EQ(quantize_to_256(95, 135, 175), 67);   // an exact cube point
	EXPECT_EQ(quantize_to_256(128, 128, 128), 244); // the ramp beats the cube on greys

	// Indices 0..15 are the user's palette and are never a target.
	for (int red = 0; red <= 255; red += 17)
		for (int green = 0; green <= 255; green += 17)
			for (int blue = 0; blue <= 255; blue += 17)
				EXPECT_GE(quantize_to_256(static_cast<uint8_t>(red),
				                          static_cast<uint8_t>(green),
				                          static_cast<uint8_t>(blue)),
				          16);
}

TEST(LeshperBlitter, ReadsCapabilitiesFromTheEnvironmentAndNeverAsksTheTerminal) {
	// #97 decision 2: assume first, trivial env reads only, no startup query.
	using caps = terminal_capabilities;
	EXPECT_EQ(caps::from_env("xterm-256color", nullptr, nullptr).colors,
	          color_depth::indexed_256);
	EXPECT_EQ(caps::from_env("xterm-256color", "truecolor", nullptr).colors,
	          color_depth::truecolor);
	EXPECT_EQ(caps::from_env("xterm-256color", "24bit", nullptr).colors,
	          color_depth::truecolor);
	EXPECT_EQ(caps::from_env("dumb", "truecolor", nullptr).colors, color_depth::monochrome);
	EXPECT_EQ(caps::from_env(nullptr, "truecolor", nullptr).colors, color_depth::monochrome);
	EXPECT_EQ(caps::from_env("xterm-256color", "truecolor", "1").colors,
	          color_depth::monochrome);
	// Nothing announces undercurl and #97 forbids asking, so it stays off.
	EXPECT_FALSE(caps::from_env("xterm-256color", "truecolor", nullptr).undercurl);
	EXPECT_EQ(caps::floor().colors, color_depth::indexed_256);
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
	// It used to be two implementations - editor.cpp's switch over an `action`
	// enum and builtin_actions.cpp over the ABI - and this test existed to catch
	// them drifting. #118 deleted the switch, so what it asserts now is the
	// stronger statement: a key pressed through the keymap stack reaches the same
	// registered action a binding would invoke by name, over a whole session and
	// over the whole state rather than the fields somebody remembered to check.
	// It is no longer a drift alarm; it is the equivalence itself.
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

	// The change record (#119) is DISPATCH's, and this test's ABI side does not
	// dispatch - it calls the action directly, which is the whole shape of the
	// comparison. So the one field a direct invocation cannot produce is aligned
	// before the whole-state compare, rather than dropped from state equality
	// where N-3's replay needs it.
	through_abi.repeat = through_keymap.repeat;
	EXPECT_TRUE(through_abi == through_keymap);
	EXPECT_FALSE(through_abi.buffer.empty());   // and the script did something

	// Redo has no default key, so it is the one built-in the keymap path cannot
	// reach. Through the ABI it is an ordinary action like the other eight.
	const action_result redone = fixture.run(through_abi, "redo");
	EXPECT_EQ(redone.status, LESH_OK);
	EXPECT_FALSE(through_abi == through_keymap);
}

// RETIRED with #118: `MotionIsGraphemeWiseWhereTheEnumPathIsStillScalarWise`.
//
// It pinned a deliberate disagreement - the ABI moved by grapheme cluster
// through #108's segmenter while editor.cpp's enum switch stepped scalar values
// through `text_buffer::next_position` - and its whole value was that neither
// side could change without somebody noticing. #118 deleted the enum switch:
// dispatch resolves a name and invokes the registered action, so the ABI path
// IS the keymap path and there is no second answer left to disagree with. Motion
// is grapheme-cluster-wise everywhere, which is what F-3 asked for.
//
// What replaced it is `LeshperKeymapDispatch.MotionOverACombiningMarkMovesByThe
// WholeCluster` in leshper_keymap_tests.cpp: the same combining-acute input,
// asserted through `step` rather than through the fixture, because `step` is now
// the path that used to be wrong. `text_buffer`'s own scalar stepping is still
// tested where it belongs, in `LeshperBuffer`, and is no longer what a cursor
// key runs.

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
	EXPECT_TRUE(s.undo_one());
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

TEST(LeshperAbiHandles, SelectionAccessorsAnswerFromTheModel) {
	// #96 has landed, so these three are backed rather than stubbed: the getter
	// reports the derived region and the setter takes one. The suites that
	// exercise the model itself are in leshper_selection_tests.cpp; this one
	// stays here to hold the ABI's own shape - the entry points did not have to
	// grow for the model to arrive.
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
	EXPECT_EQ(active, 0);  // a fresh state has an anchor and no live region
	EXPECT_EQ(set_status, LESH_OK);
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
	(void)loop.react(s, LESH_EVENT_CURSOR_MOVED);
	EXPECT_EQ(buffer_runs, 0);
	EXPECT_EQ(cursor_runs, 1);
	(void)loop.react(s, LESH_EVENT_BUFFER_CHANGED | LESH_EVENT_CURSOR_MOVED);
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
	EXPECT_FALSE(apply_batch(s, batches[0]));
	EXPECT_TRUE(s.marks.layers().empty());

	// Recomputed against the buffer as it now is, the same reactor applies.
	batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_TRUE(apply_batch(s, batches[0]));
	ASSERT_EQ(s.marks.layers().size(), 1u);
	EXPECT_EQ(s.marks.layers()[0].spans.size(), 1u);
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
		EXPECT_TRUE(apply_batch(s, one));
	ASSERT_EQ(s.marks.layers().size(), 2u);

	// A new batch from one reactor replaces that reactor's and touches nobody
	// else's, which is the whole of what a namespace has to do here. Both halves
	// of a batch are namespaced the same way (#144), so the proposal count is
	// asserted beside the span and text counts rather than separately.
	for (reactor_batch& one : loop.react(s, LESH_EVENT_BUFFER_CHANGED))
		EXPECT_TRUE(apply_batch(s, one));
	EXPECT_EQ(s.marks.layers().size(), 2u);

	size_t proposals = 0;
	size_t texts = 0;
	size_t spans = 0;
	for (const decorations::layer& one : s.marks.layers()) {
		texts += one.texts.size();
		spans += one.spans.size();
	}
	for (const applied_proposals::layer& one : s.proposals.layers())
		proposals += one.items.size();
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
