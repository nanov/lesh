#include "leshper/vi.h"

#include "leshper/abi.h"
#include "leshper/editor.h"
#include "leshper/event.h"
#include "leshper/keymap.h"
#include "leshper/kill_store.h"
#include "leshper/registry.h"
#include "leshper/state.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

// The vi repertoire (#99, #119), in its own file.
//
// Wave 3's rule, kept: one test file per lane. What lives here is everything
// #119 owns - the kill store, the motions and their counts, the operators, the
// text objects, the mode entries, and `.`.
//
// Every test drives the editor through `step()` with real key events, because
// that is the only entrance (A-9) and because the count and operator-pending
// machinery lives in dispatch: a test that called the actions directly would be
// testing the half that is not the interesting half.

using namespace lesh::leshper;

namespace {

effects press(state& s, char32_t codepoint, key_modifiers modifiers = {}) {
	return step(s, key_event::of(codepoint, modifiers));
}

void type(state& s, std::string_view text) {
	for (const char byte : text)
		press(s, static_cast<char32_t>(static_cast<unsigned char>(byte)));
}

std::string text_of(const state& s) { return std::string(s.buffer.text()); }
std::size_t cursor_of(const state& s) { return s.cursor.byte_offset(); }
std::string mode_of(const state& s) { return std::string(s.keymaps.mode()); }

// A state holding `text`, cursor at `at`, in command mode - which is where every
// test below starts, because that is where the repertoire lives.
state vi_state(std::string_view text, std::size_t at = 0) {
	state s;
	apply_edit(s, s.cursor, s.cursor, text);
	s.undo.break_coalescing();
	s.cursor = position::from_byte_offset(at);
	s.keymaps.set_mode(keymap_registry::vi_command);
	return s;
}

std::string killed(const state& s) {
	const auto* entry = s.kills.get(kill_store::unnamed);
	return entry == nullptr ? std::string{"<nothing>"} : entry->text;
}

bool kill_is_linewise(const state& s) {
	const auto* entry = s.kills.get(kill_store::unnamed);
	return entry != nullptr && (entry->flags & kill_linewise) != 0;
}

constexpr char32_t escape_key = 0x1B;

} // namespace

// ---------------------------------------------------------------------------
// The kill store (#99 answer 3): one keyed table, unnamed key as default.
// ---------------------------------------------------------------------------

TEST(LeshperKillStore, TheUnnamedKeyIsTheDefaultAndAPutReplaces) {
	kill_store store;
	EXPECT_TRUE(store.empty());
	EXPECT_EQ(store.get(kill_store::unnamed), nullptr);

	store.put(kill_store::unnamed, "first", kill_charwise);
	ASSERT_NE(store.get(kill_store::unnamed), nullptr);
	EXPECT_EQ(store.get(kill_store::unnamed)->text, "first");

	// Replaces rather than accumulates: v1 has one entry per key, and a ring is
	// a later shape over the same table.
	store.put(kill_store::unnamed, "second", kill_linewise);
	EXPECT_EQ(store.size(), 1u);
	EXPECT_EQ(store.get(kill_store::unnamed)->text, "second");
	EXPECT_EQ(store.get(kill_store::unnamed)->flags, kill_linewise);
}

TEST(LeshperKillStore, KeysAreSeparateEntriesSoNamedRegistersAreAdditive) {
	// The whole reason the store is keyed when v1 has one key: `"a` arrives as a
	// KEY, not as a second store and not as a second read path in `p`.
	kill_store store;
	store.put(kill_store::unnamed, "unnamed", kill_charwise);
	store.put("a", "named", kill_charwise);
	EXPECT_EQ(store.size(), 2u);
	EXPECT_EQ(store.get(kill_store::unnamed)->text, "unnamed");
	EXPECT_EQ(store.get("a")->text, "named");
	EXPECT_EQ(store.get("z"), nullptr);
}

TEST(LeshperKillStore, ShapeTravelsWithTheTextBecausePutHasToKnowIt) {
	kill_store store;
	store.put(kill_store::unnamed, "one line\n", kill_linewise);
	EXPECT_TRUE((store.get(kill_store::unnamed)->flags & kill_linewise) != 0);
}

TEST(LeshperKillStore, EmacsAndViReadTheSameTable) {
	// #99 answer 3, as a test rather than as a paragraph: `C-w` in emacs mode
	// fills the store, and vi's `p` in command mode puts back what it left.
	state s;
	type(s, "echo hello");
	press(s, 0x17);   // C-w, backward-kill-word
	EXPECT_EQ(text_of(s), "echo ");
	EXPECT_EQ(killed(s), "hello");

	s.keymaps.set_mode(keymap_registry::vi_command);
	s.cursor = position::from_byte_offset(4);
	press(s, U'p');
	EXPECT_EQ(text_of(s), "echo hello");
}

TEST(LeshperKillStore, TheEmacsYankReadsWhatViKilled) {
	state s = vi_state("echo hi", 5);
	press(s, U'd');
	press(s, U'w');
	EXPECT_EQ(text_of(s), "echo ");
	EXPECT_EQ(killed(s), "hi");

	// The same store, from the other paradigm's key - and emacs's cursor rule,
	// which is not vi's: point lands AFTER what was yanked.
	s.keymaps.set_mode(keymap_stack::default_mode);
	s.cursor = s.buffer.end_position();
	press(s, 0x19);   // C-y
	EXPECT_EQ(text_of(s), "echo hi");
	EXPECT_EQ(cursor_of(s), 7u);
}

// ---------------------------------------------------------------------------
// Motions (spec §6.5's set), and counts that multiply them.
// ---------------------------------------------------------------------------

TEST(LeshperViMotion, TheCharacterMotionsMoveByOneAndByACount) {
	state s = vi_state("abcdef", 0);
	press(s, U'l');
	EXPECT_EQ(cursor_of(s), 1u);
	type(s, "3l");
	EXPECT_EQ(cursor_of(s), 4u);
	type(s, "2h");
	EXPECT_EQ(cursor_of(s), 2u);
	// A motion that runs out of buffer stops rather than failing, which is what
	// vi does and what keeps `100h` a way to reach column zero.
	type(s, "100h");
	EXPECT_EQ(cursor_of(s), 0u);
}

TEST(LeshperViMotion, WordMotionsAreClassAwareAndBlankSeparatedInTwoFamilies) {
	state s = vi_state("git commit -m'x'", 0);
	press(s, U'w');
	EXPECT_EQ(cursor_of(s), 4u);   // commit
	press(s, U'w');
	EXPECT_EQ(cursor_of(s), 11u);  // -
	press(s, U'w');
	EXPECT_EQ(cursor_of(s), 12u);  // m, because `-` is its own punctuation word

	// `W` is blank-separated, so the whole `-m'x'` blob is one word.
	state big = vi_state("git commit -m'x'", 0);
	press(big, U'W');
	EXPECT_EQ(cursor_of(big), 4u);
	press(big, U'W');
	EXPECT_EQ(cursor_of(big), 11u);
	press(big, U'W');
	EXPECT_EQ(cursor_of(big), 16u);
}

TEST(LeshperViMotion, EndOfWordLandsOnTheLastCharacterAndBackwardLandsOnTheFirst) {
	state s = vi_state("echo hello", 0);
	press(s, U'e');
	EXPECT_EQ(cursor_of(s), 3u);   // the `o` of echo
	press(s, U'e');
	EXPECT_EQ(cursor_of(s), 9u);   // the `o` of hello
	press(s, U'b');
	EXPECT_EQ(cursor_of(s), 5u);
	press(s, U'b');
	EXPECT_EQ(cursor_of(s), 0u);
}

TEST(LeshperViMotion, ZeroIsTheLineStartUntilACountMakesItADigit) {
	// vi's one genuinely ambiguous key, and the state that makes it ambiguous is
	// the state that resolves it.
	state s = vi_state("abcdefghijkl", 5);
	press(s, U'0');
	EXPECT_EQ(cursor_of(s), 0u);
	// `10l` - the `0` here is the second digit of ten, not a motion.
	type(s, "10l");
	EXPECT_EQ(cursor_of(s), 10u);
}

TEST(LeshperViMotion, CaretIsTheFirstNonBlankAndDollarIsTheLineEnd) {
	state s = vi_state("   indented", 8);
	press(s, U'^');
	EXPECT_EQ(cursor_of(s), 3u);
	press(s, U'$');
	EXPECT_EQ(cursor_of(s), 11u);
	press(s, U'0');
	EXPECT_EQ(cursor_of(s), 0u);
}

TEST(LeshperViMotion, TheBufferIsTwoDimensionalSoJAndKAreReal) {
	// F-2's 2D buffer is what makes `j k` mean anything at a prompt, and #99
	// names it as the reason they are in the repertoire at all.
	state s = vi_state("one\ntwo\nthree", 1);
	press(s, U'j');
	EXPECT_EQ(cursor_of(s), 5u);   // same column, second line
	press(s, U'j');
	EXPECT_EQ(cursor_of(s), 9u);
	press(s, U'k');
	EXPECT_EQ(cursor_of(s), 5u);
	// The column clamps to a shorter line rather than running past its end.
	state ragged = vi_state("longer line\nab", 8);
	press(ragged, U'j');
	EXPECT_EQ(cursor_of(ragged), 14u);
}

TEST(LeshperViMotion, TheFindFamilyTakesItsTargetFromTheNextKey) {
	state s = vi_state("echo hello world", 0);
	type(s, "fo");
	EXPECT_EQ(cursor_of(s), 3u);    // the `o` of echo
	type(s, "fo");
	EXPECT_EQ(cursor_of(s), 9u);    // the `o` of hello
	type(s, "Fh");
	EXPECT_EQ(cursor_of(s), 5u);    // the `h` of hello
	type(s, "tw");
	EXPECT_EQ(cursor_of(s), 10u);   // just before the `w`
	type(s, "Te");
	EXPECT_EQ(cursor_of(s), 7u);    // just after the `e` of hello
}

TEST(LeshperViMotion, SemicolonRepeatsTheFindAndCommaReversesIt) {
	state s = vi_state("a.b.c.d", 0);
	type(s, "f.");
	EXPECT_EQ(cursor_of(s), 1u);
	press(s, U';');
	EXPECT_EQ(cursor_of(s), 3u);
	press(s, U';');
	EXPECT_EQ(cursor_of(s), 5u);
	press(s, U',');
	EXPECT_EQ(cursor_of(s), 3u);
}

TEST(LeshperViMotion, AFindThatIsNotThereDoesNothingAndIsNotAnError) {
	state s = vi_state("echo hi", 0);
	type(s, "fz");
	EXPECT_EQ(cursor_of(s), 0u);
	EXPECT_EQ(mode_of(s), "vi_command");
	// And the one-shot keymap it pushed is gone again.
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
}

TEST(LeshperViMotion, AFindStaysOnItsOwnLine) {
	// vi's find is line-local, and the 2D buffer makes that a real distinction.
	state s = vi_state("echo\nzed", 0);
	type(s, "fz");
	EXPECT_EQ(cursor_of(s), 0u);
}

// ---------------------------------------------------------------------------
// Operators, the doubled forms, and counts through them (#99 answer 1).
// ---------------------------------------------------------------------------

TEST(LeshperViOperator, DeleteTakesAMotionAndFeedsTheStore) {
	state s = vi_state("echo hello world", 5);
	type(s, "dw");
	EXPECT_EQ(text_of(s), "echo world");
	EXPECT_EQ(cursor_of(s), 5u);
	EXPECT_EQ(killed(s), "hello ");
	// One undo entry, because one action's writes commit as one (A-12).
	press(s, U'u');
	EXPECT_EQ(text_of(s), "echo hello world");
}

TEST(LeshperViOperator, ACountBetweenTheVerbAndTheMotionMultipliesTheMotion) {
	// `d2w` - #99's own example, and the reason the count lives in dispatch
	// rather than in the operator.
	state s = vi_state("one two three four", 0);
	type(s, "d2w");
	EXPECT_EQ(text_of(s), "three four");
	EXPECT_EQ(killed(s), "one two ");
}

TEST(LeshperViOperator, ACountBeforeTheVerbReachesTheObjectToo) {
	// `3dd` - the count arrives at `d`, and `d` hands it forward to the object
	// that will use it, because the object is the next dispatch.
	state s = vi_state("one\ntwo\nthree\nfour", 0);
	type(s, "3dd");
	EXPECT_EQ(text_of(s), "four");
	EXPECT_TRUE(kill_is_linewise(s));
	EXPECT_EQ(killed(s), "one\ntwo\nthree\n");
}

TEST(LeshperViOperator, TheDoubledFormIsALineAndMustMatchItsVerb) {
	state s = vi_state("echo hi", 3);
	type(s, "dd");
	EXPECT_EQ(text_of(s), "");
	EXPECT_TRUE(kill_is_linewise(s));

	// `dc` is not `dd`: the doubled form refuses a mismatched pair and the
	// operator gives up cleanly rather than deleting a line by accident.
	state mixed = vi_state("echo hi", 3);
	type(mixed, "dc");
	EXPECT_EQ(text_of(mixed), "echo hi");
	EXPECT_EQ(mixed.keymaps.layers.size(), 1u);
	EXPECT_TRUE(mixed.keymaps.pending_operator.empty());
}

TEST(LeshperViOperator, ChangeIsDeleteThatLandsInInsertMode) {
	state s = vi_state("echo hello", 5);
	type(s, "cw");
	EXPECT_EQ(text_of(s), "echo ");
	EXPECT_EQ(mode_of(s), "vi_insert");
	type(s, "bye");
	EXPECT_EQ(text_of(s), "echo bye");
	press(s, escape_key);
	EXPECT_EQ(mode_of(s), "vi_command");
}

TEST(LeshperViOperator, CcKeepsTheLineWhereDdTakesItAway) {
	// The only difference is one newline, and it is the one an operator has to
	// know about because the LINE OBJECT told it the span was a line.
	state s = vi_state("one\ntwo", 5);
	type(s, "cc");
	EXPECT_EQ(text_of(s), "one\n");
	EXPECT_EQ(mode_of(s), "vi_insert");

	state deleted = vi_state("one\ntwo", 5);
	type(deleted, "dd");
	EXPECT_EQ(text_of(deleted), "one\n");
	EXPECT_EQ(mode_of(deleted), "vi_command");
}

TEST(LeshperViOperator, YankLeavesTheBufferAloneAndFillsTheStore) {
	state s = vi_state("echo hello", 5);
	type(s, "yw");
	EXPECT_EQ(text_of(s), "echo hello");
	EXPECT_EQ(killed(s), "hello");
	EXPECT_FALSE(s.selection_active());
}

TEST(LeshperViOperator, EscapeAbandonsAPendingOperator) {
	state s = vi_state("echo hi", 0);
	press(s, U'd');
	EXPECT_EQ(s.keymaps.pending_operator, "vi_delete");
	EXPECT_EQ(s.keymaps.layers.size(), 2u);
	press(s, escape_key);
	EXPECT_TRUE(s.keymaps.pending_operator.empty());
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
	EXPECT_EQ(text_of(s), "echo hi");
}

TEST(LeshperViOperator, OperatorPendingIsOpaqueSoAnIrrelevantKeyDoesNotLeakThrough) {
	// The reason the map is opaque rather than falling through to vi_command:
	// `dp` would otherwise PASTE with a delete still pending, and `di` would
	// enter insert mode.
	state s = vi_state("echo hi", 0);
	s.kills.put(kill_store::unnamed, "XX", kill_charwise);
	type(s, "dp");
	EXPECT_EQ(text_of(s), "echo hi");
	EXPECT_EQ(mode_of(s), "vi_command");
}

TEST(LeshperViOperator, AnInclusiveMotionIncludesWhatItLandsOn) {
	// `de` takes the whole word; `dw` takes the word and the blank after it.
	// vi's inclusiveness is the mode's projection (spec §6.3), applied where the
	// mode knows an operator is waiting.
	state s = vi_state("echo hello world", 5);
	type(s, "de");
	EXPECT_EQ(text_of(s), "echo  world");
	EXPECT_EQ(killed(s), "hello");
}

TEST(LeshperViOperator, DeleteToAFoundCharacterIncludesIt) {
	state s = vi_state("echo hello", 0);
	type(s, "dfl");
	EXPECT_EQ(text_of(s), "lo");
}

// ---------------------------------------------------------------------------
// Text objects (#99 answer 2): one action that sets the selection to a range.
// ---------------------------------------------------------------------------

TEST(LeshperViObject, InnerWordTakesTheRunUnderTheCursor) {
	state s = vi_state("echo hello world", 7);
	type(s, "diw");
	EXPECT_EQ(text_of(s), "echo  world");
	EXPECT_EQ(killed(s), "hello");
}

TEST(LeshperViObject, AWordTakesTheTrailingBlankToo) {
	state s = vi_state("echo hello world", 7);
	type(s, "daw");
	EXPECT_EQ(text_of(s), "echo world");
	EXPECT_EQ(killed(s), "hello ");
}

TEST(LeshperViObject, AWordFallsBackToTheLeadingBlankWhenThereIsNoTrailingOne) {
	state s = vi_state("echo hello", 7);
	type(s, "daw");
	EXPECT_EQ(text_of(s), "echo");
}

TEST(LeshperViObject, TheBlankSeparatedWordObjectTakesTheWholeBlob) {
	state s = vi_state("run -m'x' now", 5);
	type(s, "diW");
	EXPECT_EQ(text_of(s), "run  now");

	state small = vi_state("run -m'x' now", 5);
	type(small, "diw");
	EXPECT_EQ(text_of(small), "run -'x' now");   // just the `m`
}

TEST(LeshperViObject, InnerWordOnWhitespaceSelectsTheWhitespace) {
	// A run of blanks is a run like any other, which is what makes this need no
	// special case at all.
	state s = vi_state("a    b", 2);
	type(s, "diw");
	EXPECT_EQ(text_of(s), "ab");
}

TEST(LeshperViObject, TheBracketHelperFindsTheEnclosingPair) {
	state s = vi_state("echo $(ls -l) done", 9);
	type(s, "di(");
	EXPECT_EQ(text_of(s), "echo $() done");
	EXPECT_EQ(killed(s), "ls -l");

	state around = vi_state("echo $(ls -l) done", 9);
	type(around, "da(");
	EXPECT_EQ(text_of(around), "echo $ done");
}

TEST(LeshperViObject, TheHelperCountsNestingWhenTheDelimitersDiffer) {
	state s = vi_state("a(b(c)d)e", 4);
	type(s, "di(");
	EXPECT_EQ(text_of(s), "a(b()d)e");

	state outer = vi_state("a(b(c)d)e", 2);
	type(outer, "di(");
	EXPECT_EQ(text_of(outer), "a()e");
}

TEST(LeshperViObject, TheCursorOnADelimiterIsInsideItsOwnPair) {
	state s = vi_state("echo (word)", 5);
	type(s, "di(");
	EXPECT_EQ(text_of(s), "echo ()");

	state closer = vi_state("echo (word)", 10);
	type(closer, "di(");
	EXPECT_EQ(text_of(closer), "echo ()");
}

TEST(LeshperViObject, AQuoteIsPairedAlongTheLineBecauseItDoesNotNest) {
	state s = vi_state("echo 'one' and 'two'", 7);
	type(s, "di'");
	EXPECT_EQ(text_of(s), "echo '' and 'two'");

	state second = vi_state("echo 'one' and 'two'", 16);
	type(second, "di\"");   // the wrong delimiter finds nothing and changes nothing
	EXPECT_EQ(text_of(second), "echo 'one' and 'two'");
	type(second, "di'");
	EXPECT_EQ(text_of(second), "echo 'one' and ''");
}

TEST(LeshperViObject, AnObjectThatMatchesNothingAbandonsTheOperator) {
	state s = vi_state("echo hi", 2);
	type(s, "di(");
	EXPECT_EQ(text_of(s), "echo hi");
	EXPECT_TRUE(s.keymaps.pending_operator.empty());
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
	// And the editor still takes the next command.
	press(s, U'x');
	EXPECT_EQ(text_of(s), "eco hi");
}

TEST(LeshperViObject, ChangeOverAnObjectWorksBecauseTheObjectIsJustARange) {
	state s = vi_state("echo $(ls) done", 8);
	type(s, "ci(");
	EXPECT_EQ(text_of(s), "echo $() done");
	EXPECT_EQ(mode_of(s), "vi_insert");
	type(s, "pwd");
	EXPECT_EQ(text_of(s), "echo $(pwd) done");
}

TEST(LeshperViObject, TheSameObjectWorksInVisualModeWithNoExtraMachinery) {
	// `viw` and `diw` are the same action reached from two stacks, which is what
	// "an object sets the selection" bought.
	state s = vi_state("echo hello", 7);
	type(s, "viw");
	ASSERT_TRUE(s.selection_active());
	const auto region = s.selection_range();
	ASSERT_TRUE(region.has_value());
	EXPECT_EQ(region->from.byte_offset(), 5u);
	EXPECT_EQ(region->to.byte_offset(), 10u);
}

// ---------------------------------------------------------------------------
// The single-key edits: x s r ~ D C Y p P.
// ---------------------------------------------------------------------------

TEST(LeshperViEdit, XDeletesUnderTheCursorAndKillsWhatItTook) {
	state s = vi_state("abcdef", 1);
	press(s, U'x');
	EXPECT_EQ(text_of(s), "acdef");
	EXPECT_EQ(killed(s), "b");
	type(s, "3x");
	EXPECT_EQ(text_of(s), "af");
	EXPECT_EQ(killed(s), "cde");
}

TEST(LeshperViEdit, XDoesNotEatTheNewline) {
	state s = vi_state("ab\ncd", 1);
	type(s, "5x");
	EXPECT_EQ(text_of(s), "a\ncd");
}

TEST(LeshperViEdit, SSubstitutesAndLandsInInsert) {
	state s = vi_state("abc", 0);
	press(s, U's');
	EXPECT_EQ(text_of(s), "bc");
	EXPECT_EQ(mode_of(s), "vi_insert");
	type(s, "X");
	EXPECT_EQ(text_of(s), "Xbc");
}

TEST(LeshperViEdit, RReplacesTheNextKeyAndStaysInCommandMode) {
	state s = vi_state("abc", 1);
	type(s, "rZ");
	EXPECT_EQ(text_of(s), "aZc");
	EXPECT_EQ(mode_of(s), "vi_command");
	EXPECT_EQ(cursor_of(s), 1u);
	// A count replaces that many, and refuses when there are not that many.
	state few = vi_state("abc", 1);
	type(few, "5rZ");
	EXPECT_EQ(text_of(few), "abc");
	state many = vi_state("abcdef", 1);
	type(many, "3rZ");
	EXPECT_EQ(text_of(many), "aZZZef");
}

TEST(LeshperViEdit, TildeTogglesCaseAndAdvances) {
	state s = vi_state("abc", 0);
	press(s, U'~');
	EXPECT_EQ(text_of(s), "Abc");
	type(s, "2~");
	EXPECT_EQ(text_of(s), "ABC");
}

TEST(LeshperViEdit, CapitalDAndCTakeTheRestOfTheLine) {
	state s = vi_state("echo hello", 4);
	press(s, U'D');
	EXPECT_EQ(text_of(s), "echo");
	EXPECT_EQ(killed(s), " hello");

	state changed = vi_state("echo hello", 4);
	press(changed, U'C');
	EXPECT_EQ(text_of(changed), "echo");
	EXPECT_EQ(mode_of(changed), "vi_insert");
}

TEST(LeshperViEdit, CapitalYIsYyAndIsLinewise) {
	// vi's own inconsistency - `Y` is not `y$` - kept because muscle memory is
	// what a repertoire is for.
	state s = vi_state("echo hi", 4);
	press(s, U'Y');
	EXPECT_EQ(text_of(s), "echo hi");
	EXPECT_TRUE(kill_is_linewise(s));
	EXPECT_EQ(killed(s), "echo hi\n");
}

TEST(LeshperViEdit, PutAfterAndPutBeforeAreCharwiseWhenTheKillWas) {
	state s = vi_state("abc", 0);
	press(s, U'x');            // kills "a", buffer "bc"
	press(s, U'p');
	EXPECT_EQ(text_of(s), "bac");
	press(s, U'P');
	EXPECT_EQ(text_of(s), "baac");
	// A count puts it that many times.
	state many = vi_state("bc", 0);
	many.kills.put(kill_store::unnamed, "z", kill_charwise);
	type(many, "3p");
	EXPECT_EQ(text_of(many), "bzzzc");
}

TEST(LeshperViEdit, ALinewisePutGoesOnItsOwnLine) {
	state s = vi_state("one\ntwo\nthree", 0);
	type(s, "dd");
	EXPECT_EQ(text_of(s), "two\nthree");
	press(s, U'p');
	EXPECT_EQ(text_of(s), "two\none\nthree");

	state before = vi_state("two\nthree", 0);
	before.kills.put(kill_store::unnamed, "one\n", kill_linewise);
	press(before, U'P');
	EXPECT_EQ(text_of(before), "one\ntwo\nthree");
}

TEST(LeshperViEdit, PuttingWithNothingKilledDoesNothingAndIsNotAnError) {
	state s = vi_state("abc", 0);
	press(s, U'p');
	EXPECT_EQ(text_of(s), "abc");
	EXPECT_EQ(s.undo.step_count(), 1u);   // the seeding edit, and no other
}

// ---------------------------------------------------------------------------
// Mode entries (#118's delegated decision, exercised through the keymap).
// ---------------------------------------------------------------------------

TEST(LeshperViMode, TheEntriesReachInsertModeFromEveryDirection) {
	state s = vi_state("echo", 2);
	press(s, U'i');
	EXPECT_EQ(mode_of(s), "vi_insert");
	type(s, "X");
	EXPECT_EQ(text_of(s), "ecXho");

	state appended = vi_state("echo", 2);
	press(appended, U'a');
	type(appended, "X");
	EXPECT_EQ(text_of(appended), "echXo");

	state at_end = vi_state("echo", 1);
	press(at_end, U'A');
	type(at_end, "!");
	EXPECT_EQ(text_of(at_end), "echo!");

	state at_start = vi_state("   echo", 6);
	press(at_start, U'I');
	type(at_start, "!");
	EXPECT_EQ(text_of(at_start), "   !echo");
}

TEST(LeshperViMode, OpenMakesANewLineAboveOrBelow) {
	state s = vi_state("one", 1);
	press(s, U'o');
	EXPECT_EQ(mode_of(s), "vi_insert");
	type(s, "two");
	EXPECT_EQ(text_of(s), "one\ntwo");

	state above = vi_state("one", 1);
	press(above, U'O');
	type(above, "zero");
	EXPECT_EQ(text_of(above), "zero\none");
}

TEST(LeshperViMode, EscapeLeavesInsertModeAndStepsBackOntoTheLastCharacter) {
	state s = vi_state("", 0);
	press(s, U'i');
	type(s, "abc");
	EXPECT_EQ(cursor_of(s), 3u);
	press(s, escape_key);
	EXPECT_EQ(mode_of(s), "vi_command");
	EXPECT_EQ(cursor_of(s), 2u);
}

TEST(LeshperViMode, CommandModeSwallowsAnUnboundPrintable) {
	// The opaque flag, still doing what #118 gave it to do now that the map is
	// full: an unbound `z` types nothing.
	state s = vi_state("abc", 0);
	press(s, U'z');
	EXPECT_EQ(text_of(s), "abc");
	press(s, U'q');
	EXPECT_EQ(text_of(s), "abc");
}

TEST(LeshperViMode, VisualModeIsAPushThatSetsTheSelection) {
	state s = vi_state("echo hello", 0);
	press(s, U'v');
	EXPECT_EQ(s.keymaps.layers.size(), 2u);
	EXPECT_EQ(indicator_of(context_of(s).keymaps(), s.keymaps), "VISUAL");
	EXPECT_TRUE(s.selection_active());
	type(s, "lll");
	const auto region = s.selection_range();
	ASSERT_TRUE(region.has_value());
	EXPECT_EQ(region->from.byte_offset(), 0u);
	EXPECT_EQ(region->to.byte_offset(), 3u);
	press(s, escape_key);
	EXPECT_FALSE(s.selection_active());
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
}

TEST(LeshperViMode, VisualOSwapsTheAnchorAndTheHead) {
	state s = vi_state("abcdef", 2);
	press(s, U'v');
	type(s, "ll");
	EXPECT_EQ(cursor_of(s), 4u);
	EXPECT_EQ(s.selection_anchor().byte_offset(), 2u);
	press(s, U'o');
	EXPECT_EQ(cursor_of(s), 2u);
	EXPECT_EQ(s.selection_anchor().byte_offset(), 4u);
	// And the derived region is the same either way.
	const auto region = s.selection_range();
	ASSERT_TRUE(region.has_value());
	EXPECT_EQ(region->from.byte_offset(), 2u);
	EXPECT_EQ(region->to.byte_offset(), 4u);
}

TEST(LeshperViMode, AVisualVerbActsOnTheRegionInclusively) {
	state s = vi_state("echo hello", 5);
	press(s, U'v');
	type(s, "ll");
	press(s, U'd');
	EXPECT_EQ(text_of(s), "echo lo");
	EXPECT_EQ(killed(s), "hel");
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
	EXPECT_FALSE(s.selection_active());
}

TEST(LeshperViMode, AVisualYankLeavesTheBufferAndPopsTheMode) {
	state s = vi_state("echo hello", 5);
	press(s, U'v');
	type(s, "ll");
	press(s, U'y');
	EXPECT_EQ(text_of(s), "echo hello");
	EXPECT_EQ(killed(s), "hel");
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
}

TEST(LeshperViMode, TheModeEntriesAreOrdinaryActionsAUserCanRebind) {
	// The whole argument for putting mode entry on the ABI (#118's delegated
	// decision): `i` is a NAME, and a name is a thing a binding can move.
	state s = vi_state("abc", 0);
	editing_context& context = context_of(s);
	keymap* command = context.keymaps().find(keymap_registry::vi_command);
	ASSERT_NE(command, nullptr);
	std::string key;
	ASSERT_TRUE(parse_key_notation("z", key));
	command->bind(key, "vi_insert_mode");
	press(s, U'z');
	EXPECT_EQ(mode_of(s), "vi_insert");
}

// ---------------------------------------------------------------------------
// `.` (#99 answer 4): minimal, and honest about its boundary.
// ---------------------------------------------------------------------------

TEST(LeshperViRepeat, ARepeatReplaysANonInsertingChange) {
	state s = vi_state("one two three four", 0);
	type(s, "dw");
	EXPECT_EQ(text_of(s), "two three four");
	press(s, U'.');
	EXPECT_EQ(text_of(s), "three four");
	press(s, U'.');
	EXPECT_EQ(text_of(s), "four");
}

TEST(LeshperViRepeat, ARepeatCarriesTheCountBecauseTheCountIsInTheKeys) {
	state s = vi_state("a b c d e f", 0);
	type(s, "d2w");
	EXPECT_EQ(text_of(s), "c d e f");
	press(s, U'.');
	EXPECT_EQ(text_of(s), "e f");
}

TEST(LeshperViRepeat, SingleKeyChangesRepeatToo) {
	state s = vi_state("abcdef", 0);
	press(s, U'x');
	press(s, U'.');
	press(s, U'.');
	EXPECT_EQ(text_of(s), "def");
}

TEST(LeshperViRepeat, ATextObjectChangeRepeats) {
	state s = vi_state("aa bb cc", 0);
	type(s, "diw");
	EXPECT_EQ(text_of(s), " bb cc");
	// The cursor is on the space; `.` deletes the space's own run.
	press(s, U'.');
	EXPECT_EQ(text_of(s), "bb cc");
}

TEST(LeshperViRepeat, ADoubledLineFormRepeats) {
	state s = vi_state("one\ntwo\nthree", 0);
	type(s, "dd");
	EXPECT_EQ(text_of(s), "two\nthree");
	press(s, U'.');
	EXPECT_EQ(text_of(s), "three");
}

TEST(LeshperViRepeat, AnInsertCarryingChangeMakesItANoOp) {
	// THE DOCUMENTED BOUNDARY. `ciw` changed the buffer AND left the mode, and
	// replaying its keys would delete and then sit in insert mode waiting for
	// text nobody is about to type. `.` says so by doing nothing.
	state s = vi_state("aa bb", 0);
	type(s, "ciwXX");
	EXPECT_EQ(text_of(s), "XX bb");
	press(s, escape_key);
	EXPECT_EQ(mode_of(s), "vi_command");
	const std::string before = text_of(s);
	press(s, U'.');
	EXPECT_EQ(text_of(s), before);
}

TEST(LeshperViRepeat, TypingInInsertModeIsNotWhatTheDotRepeats) {
	// Each typed character IS a buffer change, and each is recorded - as having
	// been made in vi_insert. `.` pressed in vi_command declines them, which is
	// what keeps the letters of a word out of the repeat's way.
	state s = vi_state("", 0);
	press(s, U'i');
	type(s, "hi");
	press(s, escape_key);
	press(s, U'.');
	EXPECT_EQ(text_of(s), "hi");
}

TEST(LeshperViRepeat, WithNothingChangedYetItIsANoOp) {
	state s = vi_state("abc", 0);
	// The seeding edit was not dispatched, so nothing is recorded.
	press(s, U'.');
	EXPECT_EQ(text_of(s), "abc");
}

TEST(LeshperViRepeat, TheRecordIsTheKeysAndItSurvivesIntervningMotions) {
	state s = vi_state("aa bb cc", 0);
	press(s, U'x');
	EXPECT_EQ(text_of(s), "a bb cc");
	type(s, "www");   // motions change nothing and must not clear the record
	press(s, U'0');
	press(s, U'.');
	EXPECT_EQ(text_of(s), " bb cc");
}

// ---------------------------------------------------------------------------
// The dispatch machinery the repertoire rides on.
// ---------------------------------------------------------------------------

TEST(LeshperViDispatch, ThePendingCountIsClearedBeforeTheActionSeesIt) {
	// The rule that makes a digit accumulate rather than re-read itself: dispatch
	// hands the count over and clears it, so an action that sets one is always
	// setting the NEXT dispatch's.
	state s = vi_state("abcdefghijklmnopqrstuvwxyz", 0);
	press(s, U'2');
	EXPECT_TRUE(s.keymaps.has_pending_count);
	EXPECT_EQ(s.keymaps.pending_count, 2);
	press(s, U'3');
	EXPECT_EQ(s.keymaps.pending_count, 23);
	press(s, U'l');
	EXPECT_FALSE(s.keymaps.has_pending_count);
	EXPECT_EQ(cursor_of(s), 23u);
}

TEST(LeshperViDispatch, EscapeInCommandModeThrowsAwayAHalfTypedCount) {
	state s = vi_state("abcdef", 0);
	press(s, U'3');
	press(s, escape_key);
	EXPECT_FALSE(s.keymaps.has_pending_count);
	press(s, U'l');
	EXPECT_EQ(cursor_of(s), 1u);
}

TEST(LeshperViDispatch, TheOperatorSlotIsSetAndConsumedByDispatchAndNotByTheVerb) {
	state s = vi_state("echo hi", 0);
	press(s, U'd');
	EXPECT_EQ(s.keymaps.pending_operator, "vi_delete");
	EXPECT_EQ(s.keymaps.layers.back(), "vi_operator_pending");
	press(s, U'w');
	EXPECT_TRUE(s.keymaps.pending_operator.empty());
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
}

TEST(LeshperViDispatch, ASwitchOfModeDropsEverythingPushedAboveIt) {
	state s = vi_state("echo hi", 0);
	press(s, U'v');
	EXPECT_EQ(s.keymaps.layers.size(), 2u);
	press(s, U'c');   // visual change: pops, edits, and swaps the base
	EXPECT_EQ(mode_of(s), "vi_insert");
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
}

// ---------------------------------------------------------------------------
// The geometry the ABI grew for this (#119), through an editor handle.
// ---------------------------------------------------------------------------

namespace {

// An action registered only to ask the ABI a geometry question and record the
// answer, which is how a test reaches a copy-out accessor at all.
struct probe {
	std::size_t from = 0;
	std::size_t to = 0;
	std::int32_t status = LESH_OK;
	std::uint32_t open = '(';
	std::uint32_t close = ')';
	lesh_span span = LESH_SPAN_WORD;
	bool ask_pair = true;
};

std::int32_t run_probe(lesh_editor* editor, const lesh_invocation*, void* userdata) {
	auto& answer = *static_cast<probe*>(userdata);
	std::size_t at = 0;
	lesh_cursor_get(editor, &at);
	answer.status = answer.ask_pair
		? lesh_match_pair(editor, at, answer.open, answer.close, &answer.from, &answer.to)
		: lesh_span_at(editor, at, answer.span, &answer.from, &answer.to);
	return LESH_OK;
}

probe ask(state& s, bool pair, char open = '(', char close = ')',
          lesh_span span = LESH_SPAN_WORD) {
	probe answer;
	answer.ask_pair = pair;
	answer.open = static_cast<std::uint32_t>(open);
	answer.close = static_cast<std::uint32_t>(close);
	answer.span = span;
	editing_context& context = context_of(s);
	lesh_action_register(&context.actions(), "test_probe", run_probe, &answer);
	context.loop().invoke(s, "test_probe", invocation{});
	return answer;
}

} // namespace

TEST(LeshperViGeometry, MatchPairIsOneHelperNeitherParadigmOwns) {
	state s = vi_state("cmd $(inner) tail", 8);
	const probe answer = ask(s, true, '(', ')');
	EXPECT_EQ(answer.status, LESH_OK);
	EXPECT_EQ(answer.from, 5u);
	EXPECT_EQ(answer.to, 12u);
}

TEST(LeshperViGeometry, MatchPairAnswersNotFoundOutsideAPair) {
	state s = vi_state("cmd (inner) tail", 14);
	EXPECT_EQ(ask(s, true, '(', ')').status, LESH_ERR_NOTFOUND);
}

TEST(LeshperViGeometry, MatchPairRefusesANonAsciiDelimiter) {
	state s = vi_state("cmd", 0);
	const probe answer = ask(s, true, '(', ')');
	EXPECT_NE(answer.status, LESH_ERR_INVAL);   // the ASCII case is fine
	// And the non-ASCII case is refused rather than guessed at.
	probe rejected;
	editing_context& context = context_of(s);
	lesh_action_register(&context.actions(), "test_probe_wide",
	                     [](lesh_editor* editor, const lesh_invocation*, void* u) {
		                     auto& out = *static_cast<probe*>(u);
		                     out.status = lesh_match_pair(editor, 0, 0x201C, 0x201D,
		                                                  &out.from, &out.to);
		                     return LESH_OK;
	                     },
	                     &rejected);
	context.loop().invoke(s, "test_probe_wide", invocation{});
	EXPECT_EQ(rejected.status, LESH_ERR_INVAL);
}

TEST(LeshperViGeometry, SpanAtIsTheRunUnderThePosition) {
	state s = vi_state("run -m'x' now", 5);
	const probe word = ask(s, false, '(', ')', LESH_SPAN_WORD);
	EXPECT_EQ(word.status, LESH_OK);
	EXPECT_EQ(word.from, 5u);
	EXPECT_EQ(word.to, 6u);   // just the `m`

	const probe blob = ask(s, false, '(', ')', LESH_SPAN_BLANK_WORD);
	EXPECT_EQ(blob.from, 4u);
	EXPECT_EQ(blob.to, 9u);
}

TEST(LeshperViGeometry, TheNewMotionsAreAppendedAndTheOldNumbersAreUnchanged) {
	// The ABI's growth rule, as a compile-time fact: an enumerator that moved
	// would repaint every binding compiled against the old header.
	static_assert(LESH_MOTION_PREV_CLUSTER == 0);
	static_assert(LESH_MOTION_BUFFER_END == 7);
	static_assert(LESH_MOTION_LINE_FIRST_NONBLANK == 8);
	static_assert(LESH_MOTION_BLANK_WORD_END_NEXT == 16);
	SUCCEED();
}

// ---------------------------------------------------------------------------
// The mode capabilities the ABI grew (#118's delegated decision, #119's answer).
// ---------------------------------------------------------------------------

namespace {

struct mode_probe {
	std::int32_t push_status = LESH_OK;
	std::int32_t pop_status = LESH_OK;
	std::int32_t pop_at_base_status = LESH_OK;
	std::string seen;
};

std::int32_t run_mode_probe(lesh_editor* editor, const lesh_invocation*, void* userdata) {
	auto& answer = *static_cast<mode_probe*>(userdata);
	char name[64];
	std::size_t length = 0;
	if (lesh_mode_get(editor, name, sizeof(name), &length) == LESH_OK)
		answer.seen.assign(name, length);
	answer.push_status = lesh_keymap_push(editor, "vi_visual");
	answer.pop_status = lesh_keymap_pop(editor);
	answer.pop_at_base_status = lesh_keymap_pop(editor);
	return LESH_OK;
}

} // namespace

TEST(LeshperViAbi, TheModeIsReadableAndTheStackIsPushableFromABinding) {
	state s = vi_state("abc", 0);
	mode_probe answer;
	editing_context& context = context_of(s);
	lesh_action_register(&context.actions(), "test_mode_probe", run_mode_probe, &answer);
	context.loop().invoke(s, "test_mode_probe", invocation{});

	EXPECT_EQ(answer.seen, "vi_command");
	EXPECT_EQ(answer.push_status, LESH_OK);
	EXPECT_EQ(answer.pop_status, LESH_OK);
	// Popping the BASE is refused: a mode is not something one can pop out of,
	// only something one swaps. An action that thinks it popped one has lost
	// track of its own pushes, and hearing so is the point.
	EXPECT_EQ(answer.pop_at_base_status, LESH_ERR_REFUSED);
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
}

TEST(LeshperViAbi, TheModeEntriesAreOrdinaryRegistrationsWithOriginalsToDelegateTo) {
	// ADR-0008's wrapper hatch, which only exists because these went through the
	// ABI: `.vi_insert_mode` is the unshadowable original a user's wrapper
	// delegates to.
	state s = vi_state("abc", 0);
	editing_context& context = context_of(s);
	std::int32_t exists = 0;
	EXPECT_EQ(lesh_action_exists(&context.actions(), "vi_insert_mode", &exists), LESH_OK);
	EXPECT_EQ(exists, 1);
	EXPECT_EQ(lesh_action_exists(&context.actions(), ".vi_insert_mode", &exists), LESH_OK);
	EXPECT_EQ(exists, 1);
	EXPECT_EQ(lesh_action_exists(&context.actions(), ".vi_repeat", &exists), LESH_OK);
	EXPECT_EQ(exists, 1);
}

TEST(LeshperViAbi, TheKillStoreCrossesTheAbiUnderAKeyAndAShape) {
	state s = vi_state("abc", 0);
	editing_context& context = context_of(s);
	struct kill_probe {
		std::int32_t set_status = LESH_OK;
		std::int32_t missing_status = LESH_OK;
		std::int32_t bad_flags_status = LESH_OK;
		std::string read;
		std::uint32_t flags = 0;
	} answer;
	lesh_action_register(
		&context.actions(), "test_kill_probe",
		[](lesh_editor* editor, const lesh_invocation*, void* u) {
			auto& out = *static_cast<kill_probe*>(u);
			out.set_status = lesh_kill_set(editor, nullptr, "text", 4, LESH_KILL_LINEWISE);
			out.bad_flags_status = lesh_kill_set(editor, nullptr, "x", 1, 0xF0u);
			char buffer[16];
			std::size_t length = 0;
			lesh_kill_get(editor, nullptr, buffer, sizeof(buffer), &length, &out.flags);
			out.read.assign(buffer, length);
			out.missing_status =
				lesh_kill_get(editor, "a", buffer, sizeof(buffer), &length, nullptr);
			return LESH_OK;
		},
		&answer);
	context.loop().invoke(s, "test_kill_probe", invocation{});

	EXPECT_EQ(answer.set_status, LESH_OK);
	EXPECT_EQ(answer.read, "text");
	EXPECT_EQ(answer.flags, LESH_KILL_LINEWISE);
	// An undefined flag bit is refused rather than stored, so the mask can grow
	// additively without an old binding's stray bit meaning something new.
	EXPECT_EQ(answer.bad_flags_status, LESH_ERR_INVAL);
	// A key nobody has written is NOTFOUND, which is what `"ap` will read before
	// named registers exist and is not an error.
	EXPECT_EQ(answer.missing_status, LESH_ERR_NOTFOUND);
}

TEST(LeshperViDispatch, TheChangeAccumulatorDoesNotGrowAcrossFinishedCommands) {
	// The steady-state property behind "no new allocation on the keystroke path":
	// a sequence that ends without changing anything is FORGOTTEN, so a long
	// session of motions leaves the record's accumulator empty rather than
	// holding every key ever pressed.
	state s = vi_state("one two three four five", 0);
	for (int i = 0; i < 200; ++i)
		type(s, "wb");
	EXPECT_TRUE(s.repeat.in_progress.empty());
	EXPECT_TRUE(s.repeat.started_in.empty());
	// And a change records exactly its own keys, not the two hundred before it.
	type(s, "dw");
	EXPECT_EQ(s.repeat.keys, "dw");
}
