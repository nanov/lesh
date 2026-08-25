#include "leshper/decode.h"
#include "leshper/editor.h"
#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/state.h"
#include "leshper/undo.h"

#include <gtest/gtest.h>

#include <chrono>
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
	// other, and not a file descriptor in the process. Paste is deliberately
	// absent - the editor's handling of paste_event is the keymap ticket's
	// (#93/#96), and this asserts only what the decoder promises.
	driver d;
	d.feed("echo \xE4\xB8\xAD\x1B[D" "x");

	state s;
	for (const event& one : d.events)
		step(s, one);

	EXPECT_EQ(std::string(s.buffer.text()), "echo x\xE4\xB8\xAD");
}
