#include "leshper/pager.h"

#include "leshper/abi.h"
#include "leshper/editor.h"
#include "leshper/event.h"
#include "leshper/keymap.h"
#include "leshper/layout.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "substrate/grapheme.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pager (#138, F-28 to F-30, spec §6.9), in its own file.
//
// Wave 3's rule, kept: one test file per lane.
//
// TWO ENTRANCES AND NO THIRD. The picture is asserted as a CELL GRID (N-3) by
// calling `render_pager`, which is the surface A-6 says the pager owns. Every
// BEHAVIOUR is driven through `step()` with real key events or through an action
// dispatched by NAME - never by poking `pager_state`, because the whole claim of
// this ticket is that the pager needs no special dispatch and a test that
// reached past dispatch would not be testing that.
//
// The candidates are FED, always. Generation is #139's, and a test that walked a
// real `$PATH` would pass differently on every machine.

using namespace lesh::leshper;

namespace {

// --- Driving keys -----------------------------------------------------------

effects press(state& s, char32_t codepoint, key_modifiers modifiers = {}) {
	return step(s, key_event::of(codepoint, modifiers));
}
effects press(state& s, named_key key, key_modifiers modifiers = {}) {
	return step(s, key_event::of(key, modifiers));
}
void type(state& s, std::string_view text) {
	for (const char byte : text)
		press(s, static_cast<char32_t>(static_cast<unsigned char>(byte)));
}

std::string text_of(const state& s) { return std::string(s.buffer.text()); }
std::size_t cursor_of(const state& s) { return s.cursor.byte_offset(); }

constexpr char32_t tab_key = U'\t';
constexpr char32_t escape_key = 0x1B;
constexpr char32_t enter_key = U'\r';
constexpr char32_t delete_key = 0x7F;

// --- Feeding candidates, exactly as #139 will --------------------------------
//
// The seam, spelled out: open a span, add candidates, commit. Nothing else in
// leshper knows how to fill a pager, and this action - registered through
// `lesh_action_register` like any other - is what a completer is.
struct feed {
	std::vector<std::pair<std::string, std::uint32_t>> candidates;
	std::size_t from = 0;
	std::size_t to = 0;
	std::uint32_t outcome = LESH_PAGER_NOTHING;
	std::int32_t last_add = LESH_OK;
};

int32_t feed_action(lesh_editor* editor, const lesh_invocation*, void* self) {
	feed& me = *static_cast<feed*>(self);
	const int32_t opened = lesh_pager_open(editor, me.from, me.to);
	if (opened != LESH_OK)
		return opened;
	for (const auto& one : me.candidates)
		me.last_add = lesh_pager_add(editor, one.first.data(), one.first.size(), one.second);
	return lesh_pager_commit(editor, &me.outcome);
}

// Registers the feeder and runs it, answering what commit decided. The state's
// OWN context, so the pager keymap that commit pushes is the one a subsequent
// keypress dispatches through.
std::uint32_t fill(state& s, feed& what) {
	registry& reg = context_of(s).actions();
	EXPECT_EQ(lesh_action_register(&reg, "test_fill_pager", &feed_action, &what), LESH_OK);
	context_of(s).loop().invoke(s, "test_fill_pager", invocation{});
	return what.outcome;
}

// A pager over a set of plain words, opened on `[from, to)` of an empty-ish
// buffer. The common shape of most tests below.
feed words(std::vector<std::string> names, std::uint32_t kind = LESH_PAGER_WORD) {
	feed what;
	for (std::string& one : names)
		what.candidates.emplace_back(std::move(one), kind);
	return what;
}

state sized_state(std::uint16_t columns = 40, std::uint16_t rows = 10) {
	state s;
	step(s, resize_event{columns, rows});
	return s;
}

// --- Reading the picture ----------------------------------------------------

std::string glyphs_of(const cluster_pool& pool, const surface& painted, std::uint16_t row) {
	std::string out;
	for (std::uint16_t column = 0; column < painted.columns(); ++column) {
		const cell& one = painted.at(row, column);
		if (!one.glyph.is_continuation())
			out.append(pool.cluster_of(one.glyph));
	}
	return out;
}

// An expected row: `head`, then blanks out to the picture's width. Computed
// rather than written out, so an expectation is never a run of spaces nobody
// can count by eye - the same helper the layout suite uses, for the same reason.
std::string padded(std::string_view head, const surface& painted) {
	std::string out{head};
	for (int filled = lesh::grapheme::string_width(head); filled < painted.columns(); ++filled)
		out.push_back(' ');
	return out;
}

std::vector<std::string> rows_of(const cluster_pool& pool, const surface& painted) {
	std::vector<std::string> out;
	for (std::uint16_t row = 0; row < painted.rows(); ++row)
		out.push_back(glyphs_of(pool, painted, row));
	return out;
}

// The pager's own surface for a state, at the state's own size (A-6).
surface picture(cluster_pool& pool, const state& s) {
	const pager_grid grid =
		measure_pager(s.pager, s.columns, pager_row_budget(s.rows));
	return render_pager(pool, s.pager, grid);
}

pager_grid grid_of(const state& s) {
	return measure_pager(s.pager, s.columns, pager_row_budget(s.rows));
}

std::string selected_text(const state& s) {
	const pager_candidate* one = pager_selected(s.pager);
	return one == nullptr ? std::string{"<none>"} : one->text;
}

} // namespace

// ---------------------------------------------------------------------------
// F-30: the decision that happens before a pager opens.
// ---------------------------------------------------------------------------

TEST(LeshperPagerCommonPrefix, AUniqueCandidateInsertsItselfAndNothingOpens) {
	state s = sized_state();
	feed what = words({"grep"});
	EXPECT_EQ(fill(s, what), LESH_PAGER_INSERTED);
	// The whole candidate plus its kind's trailer: a word is followed by a
	// space, because what comes next is an argument.
	EXPECT_EQ(text_of(s), "grep ");
	EXPECT_FALSE(s.pager.open);
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
}

TEST(LeshperPagerCommonPrefix, AnUnambiguousPrefixInsertsWithoutOpeningThePager) {
	state s = sized_state();
	type(s, "gi");
	feed what = words({"github", "gitlab", "gitea"});
	what.from = 0;
	what.to = 2;
	EXPECT_EQ(fill(s, what), LESH_PAGER_INSERTED);
	// `git` is what they agree on, and it is more than `gi` - so it goes in and
	// no list appears. No trailer: the candidates disagree about what follows.
	EXPECT_EQ(text_of(s), "git");
	EXPECT_FALSE(s.pager.open);
}

TEST(LeshperPagerCommonPrefix, AnAmbiguousSetWithNothingLeftToInsertOpens) {
	state s = sized_state();
	type(s, "git");
	feed what = words({"github", "gitlab"});
	what.from = 0;
	what.to = 3;
	EXPECT_EQ(fill(s, what), LESH_PAGER_OPENED);
	EXPECT_EQ(text_of(s), "git") << "opening must not edit the line";
	EXPECT_TRUE(s.pager.open);
	// The second Tab lists, which is the same rule twice: by now the prefix IS
	// the agreement.
	EXPECT_EQ(s.keymaps.layers.back(), "pager");
}

TEST(LeshperPagerCommonPrefix, NoCandidatesOpensNothingAndChangesNothing) {
	state s = sized_state();
	type(s, "zzz");
	const generation before = s.gen;
	feed what = words({});
	what.to = 3;
	EXPECT_EQ(fill(s, what), LESH_PAGER_NOTHING);
	EXPECT_EQ(text_of(s), "zzz");
	EXPECT_TRUE(s.gen == before);
	EXPECT_FALSE(s.pager.open);
}

TEST(LeshperPagerCommonPrefix, APrefixThatDoesNotExtendWhatIsTypedOpensInstead) {
	// A history search's candidates are whole lines and share a prefix that has
	// nothing to do with the query. Inserting it would replace the line with a
	// stranger's beginning, so the rule is "extends", not "is longer".
	state s = sized_state();
	type(s, "log");
	feed what = words({"git log --oneline", "git log -p"}, LESH_PAGER_PLAIN);
	what.from = 0;
	what.to = 3;
	EXPECT_EQ(fill(s, what), LESH_PAGER_OPENED);
	EXPECT_EQ(text_of(s), "log");
}

TEST(LeshperPagerCommonPrefix, TheAgreementStopsOnAClusterBoundary) {
	// Two names agreeing on the first two bytes of a three-byte character agree
	// on nothing anybody can insert (F-3).
	state s = sized_state();
	feed what = words({"\xE2\x82\xAC" "a", "\xE2\x82\xAC" "b"});  // €a and €b
	EXPECT_EQ(fill(s, what), LESH_PAGER_INSERTED);
	EXPECT_EQ(text_of(s), "\xE2\x82\xAC") << "the euro sign, whole or not at all";
}

TEST(LeshperPagerCommonPrefix, TheCommonPrefixIsTakenOverTheFilteredListOnly) {
	pager_state pager;
	pager.candidates = {{"alpha", pager_kind::word},
	                    {"alps", pager_kind::word},
	                    {"beta", pager_kind::word}};
	pager.filter = "al";
	pager_refilter(pager);
	EXPECT_EQ(pager_common_prefix(pager), "alp");
}

// ---------------------------------------------------------------------------
// The keymap: #117's opaque-with-a-default, which is the whole of dispatch.
// ---------------------------------------------------------------------------

TEST(LeshperPagerKeymap, ThePagerMapIsOpaqueAndRoutesUnboundPrintablesToItsFilter) {
	state s;
	const keymap* map = context_of(s).keymaps().find(keymap_registry::pager);
	ASSERT_NE(map, nullptr);
	EXPECT_TRUE(map->opaque) << "an open pager must take the self_insert floor with it";
	EXPECT_EQ(map->default_action, "pager_filter_key");
}

TEST(LeshperPagerKeymap, ThePagerDeclaresNoIndicator) {
	// F-40's indicator reads the topmost keymap declaring one. A pager is
	// something you can see; claiming the indicator would blank VISUAL while a
	// pager was open over visual mode.
	state s;
	const keymap* map = context_of(s).keymaps().find(keymap_registry::pager);
	ASSERT_NE(map, nullptr);
	EXPECT_TRUE(map->indicator.empty());

	s.keymaps.set_mode(keymap_registry::vi_command);
	s.keymaps.push(keymap_registry::vi_visual);
	s.keymaps.push(keymap_registry::pager);
	EXPECT_EQ(indicator_of(context_of(s).keymaps(), s.keymaps), "VISUAL");
}

TEST(LeshperPagerKeymap, OpeningPushesTheLayerAndClosingPopsIt) {
	state s = sized_state();
	feed what = words({"alpha", "beta"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	ASSERT_EQ(s.keymaps.layers.size(), 2u);
	EXPECT_EQ(s.keymaps.layers.back(), "pager");

	press(s, escape_key);
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
	EXPECT_FALSE(s.pager.open);
	EXPECT_TRUE(s.pager.candidates.empty());
}

TEST(LeshperPagerKeymap, ThePagerActionsAreAllRegistered) {
	state s;
	registry& reg = context_of(s).actions();
	for (const char* name : {"pager_next", "pager_previous", "pager_next_row",
	                         "pager_previous_row", "pager_accept", "pager_close",
	                         "pager_filter_key", "pager_filter_backspace",
	                         "pager_show_completions", "pager_show_history_matches",
	                         "pager_show_suggestions"}) {
		int32_t exists = 0;
		EXPECT_EQ(lesh_action_exists(&reg, name, &exists), LESH_OK) << name;
		EXPECT_EQ(exists, 1) << name;
	}
}

// ---------------------------------------------------------------------------
// Cycling.
// ---------------------------------------------------------------------------

TEST(LeshperPagerSelection, TabCyclesForwardAndWrapsAtTheEnd) {
	state s = sized_state();
	feed what = words({"alpha", "beta", "gamma"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	EXPECT_EQ(selected_text(s), "alpha");
	press(s, tab_key);
	EXPECT_EQ(selected_text(s), "beta");
	press(s, tab_key);
	EXPECT_EQ(selected_text(s), "gamma");
	press(s, tab_key);
	EXPECT_EQ(selected_text(s), "alpha") << "Tab is a cycle, not a walk that stops";
}

TEST(LeshperPagerSelection, LeftWrapsBackwardsFromTheFirst) {
	state s = sized_state();
	feed what = words({"alpha", "beta", "gamma"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	press(s, named_key::left);
	EXPECT_EQ(selected_text(s), "gamma");
}

TEST(LeshperPagerSelection, DownMovesByAWholeGridRow) {
	// Six short candidates in a 40-column terminal fit on one row; narrow it and
	// Down becomes a real step. The row width is the GRID's, which is why the
	// axis is a question for the editor rather than arithmetic in a binding.
	state s = sized_state(20, 10);
	feed what = words({"aa", "bb", "cc", "dd", "ee", "ff"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	const pager_grid grid = grid_of(s);
	ASSERT_EQ(grid.columns, 5) << "four columns of `xx` plus gutters in twenty";
	EXPECT_EQ(selected_text(s), "aa");
	press(s, named_key::down);
	EXPECT_EQ(selected_text(s), "ff") << "one row down from index 0 is index 5";
}

TEST(LeshperPagerSelection, MovingWithNoPagerIsHarmless) {
	// A user who bound `pager_next` outside the pager gets nothing, not a beep
	// and not an error.
	state s = sized_state();
	const action_result ran =
		context_of(s).loop().invoke(s, "pager_next", invocation{});
	EXPECT_EQ(ran.status, LESH_OK);
}

// ---------------------------------------------------------------------------
// F-29: filtering while typing.
// ---------------------------------------------------------------------------

TEST(LeshperPagerFilter, TypingNarrowsTheListIncrementally) {
	state s = sized_state();
	feed what = words({"alpha", "alps", "beta"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	EXPECT_EQ(s.pager.matching.size(), 3u);

	press(s, U'a');
	EXPECT_EQ(s.pager.filter, "a");
	EXPECT_EQ(s.pager.matching.size(), 3u) << "beta contains an `a` too";
	press(s, U'l');
	EXPECT_EQ(s.pager.matching.size(), 2u);
	press(s, U'p');
	press(s, U'h');
	EXPECT_EQ(s.pager.matching.size(), 1u);
	EXPECT_EQ(selected_text(s), "alpha");
}

TEST(LeshperPagerFilter, TheFilterNeverReachesTheBuffer) {
	// The opaque flag takes the `self_insert` floor with it, which is the whole
	// of why a printable typed over a pager filters instead of typing.
	state s = sized_state();
	type(s, "ls ");
	const generation before = s.gen;
	feed what = words({"alpha", "beta"});
	what.from = 3;
	what.to = 3;
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	press(s, U'a');
	press(s, U'l');
	EXPECT_EQ(text_of(s), "ls ") << "filtering is not editing";
	EXPECT_TRUE(s.gen == before);
	EXPECT_EQ(s.pager.filter, "al");
}

TEST(LeshperPagerFilter, BackspaceShortensTheFilterAndThenClosesThePager) {
	state s = sized_state();
	feed what = words({"alpha", "beta"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	press(s, U'a');
	press(s, U'l');
	ASSERT_EQ(s.pager.filter, "al");
	press(s, delete_key);
	EXPECT_EQ(s.pager.filter, "a");
	EXPECT_TRUE(s.pager.open);
	press(s, delete_key);
	EXPECT_TRUE(s.pager.filter.empty());
	EXPECT_TRUE(s.pager.open);
	// Nothing left to shorten: backing out of the filter backs out of the pager.
	press(s, delete_key);
	EXPECT_FALSE(s.pager.open);
	EXPECT_EQ(s.keymaps.layers.size(), 1u);
}

TEST(LeshperPagerFilter, BackspaceRemovesAWholeCluster) {
	pager_state pager;
	pager.filter = "a\xE2\x82\xAC";  // a€
	EXPECT_TRUE(pager_filter_pop(pager));
	EXPECT_EQ(pager.filter, "a");
	EXPECT_TRUE(pager_filter_pop(pager));
	EXPECT_TRUE(pager.filter.empty());
	EXPECT_FALSE(pager_filter_pop(pager));
}

TEST(LeshperPagerFilter, AFilterThatMatchesNothingLeavesNothingSelected) {
	state s = sized_state();
	feed what = words({"alpha", "beta"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	press(s, U'z');
	EXPECT_TRUE(s.pager.matching.empty());
	EXPECT_EQ(selected_text(s), "<none>");
	// Accepting nothing is a no-op, not a crash and not an empty insertion.
	press(s, enter_key);
	EXPECT_EQ(text_of(s), "");
}

TEST(LeshperPagerFilter, FilteringKeepsPointingAtTheSelectedCandidateWhereItCan) {
	state s = sized_state();
	feed what = words({"alpha", "beta", "gamma"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	press(s, tab_key);
	ASSERT_EQ(selected_text(s), "beta");
	press(s, U'a');   // all three still match
	EXPECT_EQ(selected_text(s), "beta");
}

// ---------------------------------------------------------------------------
// A-12: accepting.
// ---------------------------------------------------------------------------

TEST(LeshperPagerAccept, EnterReplacesTheSpanTheClientOpenedOn) {
	state s = sized_state();
	type(s, "cat REA");
	feed what = words({"README", "REALLY"});
	what.from = 4;
	what.to = 7;
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	press(s, enter_key);
	EXPECT_EQ(text_of(s), "cat README ");
	EXPECT_EQ(cursor_of(s), 11u);
	EXPECT_FALSE(s.pager.open);
}

TEST(LeshperPagerAccept, ADirectoryTakesASlashAndAWordTakesASpace) {
	state s = sized_state();
	feed dirs;
	dirs.candidates = {{"src", LESH_PAGER_DIRECTORY}, {"tmp", LESH_PAGER_DIRECTORY}};
	ASSERT_EQ(fill(s, dirs), LESH_PAGER_OPENED);
	press(s, enter_key);
	EXPECT_EQ(text_of(s), "src/");

	state other = sized_state();
	feed files;
	files.candidates = {{"main.c", LESH_PAGER_WORD}, {"other.c", LESH_PAGER_WORD}};
	ASSERT_EQ(fill(other, files), LESH_PAGER_OPENED);
	press(other, enter_key);
	EXPECT_EQ(text_of(other), "main.c ");
}

TEST(LeshperPagerAccept, APlainCandidateInsertsBareBecauseAHistoryLineIsWhole) {
	state s = sized_state();
	type(s, "git");
	feed what = words({"git log --oneline", "grep -r x ."}, LESH_PAGER_PLAIN);
	what.from = 0;
	what.to = 3;
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	press(s, enter_key);
	EXPECT_EQ(text_of(s), "git log --oneline");
}

TEST(LeshperPagerAccept, AnAcceptIsOneUndoEntryAndOneGeneration) {
	state s = sized_state();
	type(s, "cat RE");
	s.undo.break_coalescing();
	const std::uint64_t before = s.gen.value();
	feed what = words({"README", "RELEASE"});
	what.from = 4;
	what.to = 6;
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	press(s, tab_key);
	press(s, enter_key);
	ASSERT_EQ(text_of(s), "cat RELEASE ");
	EXPECT_EQ(s.gen.value(), before + 1) << "one mutation, through apply_edit";
	EXPECT_TRUE(s.undo_one());
	EXPECT_EQ(text_of(s), "cat RE") << "one undo step takes the whole insertion";
}

TEST(LeshperPagerAccept, F30sInsertionIsAlsoOneUndoStep) {
	state s = sized_state();
	type(s, "gi");
	s.undo.break_coalescing();
	feed what = words({"github", "gitlab"});
	what.from = 0;
	what.to = 2;
	ASSERT_EQ(fill(s, what), LESH_PAGER_INSERTED);
	ASSERT_EQ(text_of(s), "git");
	EXPECT_TRUE(s.undo_one());
	EXPECT_EQ(text_of(s), "gi");
}

// ---------------------------------------------------------------------------
// The picture (N-3: a cell grid, never a byte stream).
// ---------------------------------------------------------------------------

TEST(LeshperPagerRender, ColumnsAreSizedToTheLongestCandidate) {
	cluster_pool pool;
	state s = sized_state(40, 10);
	feed what = words({"a", "bb", "ccccc", "d"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);

	const pager_grid grid = grid_of(s);
	EXPECT_EQ(grid.entry_width, 5) << "the longest candidate sets the column";
	EXPECT_EQ(grid.column_width, 7) << "plus a two-column gutter";
	EXPECT_EQ(grid.columns, 4) << "four candidates never need a fifth column";
	EXPECT_EQ(grid.rows, 1);

	const surface page = picture(pool, s);
	const std::vector<std::string> rows = rows_of(pool, page);
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_EQ(rows[0], padded("a      bb     ccccc  d", page));
}

TEST(LeshperPagerRender, TheGridFillsAcrossThenDown) {
	cluster_pool pool;
	state s = sized_state(12, 10);
	feed what = words({"aa", "bb", "cc", "dd", "ee"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	const pager_grid grid = grid_of(s);
	ASSERT_EQ(grid.columns, 3) << "three columns of four in twelve";
	ASSERT_EQ(grid.rows, 2);

	const std::vector<std::string> rows = rows_of(pool, picture(pool, s));
	ASSERT_EQ(rows.size(), 2u);
	EXPECT_EQ(rows[0], "aa  bb  cc");
	EXPECT_EQ(rows[1], "dd  ee    ");
}

TEST(LeshperPagerRender, EachKindDrawsItsLsMarkerAndNothingElse) {
	cluster_pool pool;
	state s = sized_state(60, 10);
	feed what;
	what.candidates = {{"dir", LESH_PAGER_DIRECTORY},
	                   {"run", LESH_PAGER_EXECUTABLE},
	                   {"lnk", LESH_PAGER_SYMLINK},
	                   {"txt", LESH_PAGER_WORD},
	                   {"raw", LESH_PAGER_PLAIN}};
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	const surface page = picture(pool, s);
	const std::vector<std::string> rows = rows_of(pool, page);
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_EQ(rows[0], padded("dir/  run*  lnk@  txt   raw", page));
}

TEST(LeshperPagerRender, TheSelectedEntryCarriesItsPenAcrossTheWholeColumn) {
	cluster_pool pool;
	state s = sized_state(20, 10);
	feed what = words({"aa", "bbbb"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);

	const surface page = picture(pool, s);
	const style selected = pager_pens{}.selected;
	// The bar is the COLUMN's width, not the candidate's: a ragged highlight is
	// what painting only the letters would give.
	for (std::uint16_t column = 0; column < 4; ++column)
		EXPECT_EQ(page.at(0, column).pen, selected) << "column " << column;
	EXPECT_EQ(page.at(0, 4).pen, style{}) << "the gutter is not part of the bar";
	EXPECT_EQ(page.at(0, 6).pen, style{}) << "the unselected entry keeps the plain pen";
}

TEST(LeshperPagerRender, ThePictureIsTheColumnsItDrewAndNotTheTerminal) {
	// A-6: the surface is the PAGER's, so its width is its own content's. What
	// centres or pads it is the compositor's business, and layout.cpp
	// deliberately does neither.
	cluster_pool pool;
	state s = sized_state(40, 10);
	feed what = words({"aa", "bb"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	const surface page = picture(pool, s);
	EXPECT_EQ(page.columns(), 6) << "two entries of two, one gutter between them";
	EXPECT_EQ(page.rows(), 1);
}

TEST(LeshperPagerRender, ACandidateWiderThanTheScreenGetsOneColumnAndIsClipped) {
	cluster_pool pool;
	state s = sized_state(6, 10);
	feed what = words({"enormous", "aa"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	const pager_grid grid = grid_of(s);
	EXPECT_EQ(grid.columns, 1);
	EXPECT_EQ(grid.entry_width, 6);
	const std::vector<std::string> rows = rows_of(pool, picture(pool, s));
	ASSERT_EQ(rows.size(), 2u);
	EXPECT_EQ(rows[0], "enormo");
}

TEST(LeshperPagerRender, AWideClusterIsNeverSplitAcrossAColumnEdge) {
	cluster_pool pool;
	state s = sized_state(40, 10);
	feed what = words({"\xE6\xBC\xA2\xE5\xAD\x97", "ab"});   // 漢字, four columns
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	const pager_grid grid = grid_of(s);
	EXPECT_EQ(grid.entry_width, 4) << "two double-width clusters";
	const surface page = picture(pool, s);
	EXPECT_TRUE(page.at(0, 1).glyph.is_continuation());
	EXPECT_TRUE(page.at(0, 3).glyph.is_continuation());
}

// ---------------------------------------------------------------------------
// Scrolling.
// ---------------------------------------------------------------------------

TEST(LeshperPagerScroll, ThePagerNeverTakesTheLastRowOfTheTerminal) {
	EXPECT_EQ(pager_row_budget(0), 0);
	EXPECT_EQ(pager_row_budget(1), 0) << "there is nowhere to put a pager";
	EXPECT_EQ(pager_row_budget(2), 1);
	EXPECT_EQ(pager_row_budget(24), 12);
}

TEST(LeshperPagerScroll, AListTallerThanTheBudgetShowsAWindow) {
	state s = sized_state(4, 6);   // one column of `xx`, three rows of budget
	feed what = words({"a1", "b2", "c3", "d4", "e5", "f6"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	const pager_grid grid = grid_of(s);
	EXPECT_EQ(grid.columns, 1);
	EXPECT_EQ(grid.rows, 6);
	EXPECT_EQ(grid.visible_rows, 3);
	EXPECT_TRUE(grid.scrolled());
	EXPECT_EQ(grid.first_row, 0);
}

TEST(LeshperPagerScroll, TheWindowFollowsTheSelectionByTheLeastItCan) {
	cluster_pool pool;
	state s = sized_state(4, 6);
	feed what = words({"a1", "b2", "c3", "d4", "e5", "f6"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);

	for (int i = 0; i < 3; ++i)
		press(s, tab_key);
	ASSERT_EQ(selected_text(s), "d4");
	EXPECT_EQ(grid_of(s).first_row, 1) << "one row, not a jump to the middle";
	const std::vector<std::string> rows = rows_of(pool, picture(pool, s));
	ASSERT_EQ(rows.size(), 3u);
	EXPECT_EQ(rows[0], "b2");
	EXPECT_EQ(rows[2], "d4");
}

TEST(LeshperPagerScroll, AResizeThatShrinksTheWindowCannotHideTheSelection) {
	// The offset is a HINT the renderer clamps and overrides - #123's argument,
	// kept - so a resize needs no reflow and no stored geometry to fix up.
	state s = sized_state(4, 20);
	feed what = words({"a1", "b2", "c3", "d4", "e5", "f6", "g7", "h8"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	for (int i = 0; i < 7; ++i)
		press(s, tab_key);
	ASSERT_EQ(selected_text(s), "h8");

	step(s, resize_event{4, 4});
	const pager_grid grid = grid_of(s);
	ASSERT_EQ(grid.visible_rows, 2);
	EXPECT_EQ(grid.first_row, 6) << "the last two rows, so the selection is on screen";
}

// ---------------------------------------------------------------------------
// The layout composites it (#123's invariants must survive).
// ---------------------------------------------------------------------------

TEST(LeshperPagerLayout, AClosedPagerChangesNothingAboutTheLayout) {
	cluster_pool pool;
	state s = sized_state(20, 6);
	type(s, "echo");
	const layout made = lay_out(pool, input_for(s, "$ "));
	EXPECT_EQ(made.pager_rows, 0);
	EXPECT_EQ(made.screen.rows(), 1);
	EXPECT_EQ(made.edit_rows(), 1);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), "$ echo              ");
}

TEST(LeshperPagerLayout, ThePagerIsCompositedBelowTheEditLine) {
	cluster_pool pool;
	state s = sized_state(12, 6);
	type(s, "e");
	feed what = words({"aa", "bb", "cc", "dd", "ee"});
	what.from = 1;
	what.to = 1;
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);

	const layout made = lay_out(pool, input_for(s, "$ "));
	EXPECT_EQ(made.pager_rows, 2);
	EXPECT_EQ(made.edit_rows(), 1);
	ASSERT_EQ(made.screen.rows(), 3);
	const std::vector<std::string> rows = rows_of(pool, made.screen);
	EXPECT_EQ(rows[0], "$ e         ");
	EXPECT_EQ(rows[1], "aa  bb  cc  ");
	EXPECT_EQ(rows[2], "dd  ee      ");
	// The cursor stays on the edit line, in screen coordinates the blitter can
	// reach.
	EXPECT_EQ(made.screen.cursor().row, 0);
	EXPECT_EQ(made.screen.cursor().column, 3);
}

TEST(LeshperPagerLayout, ThePagersRowsComeOffTheEditViewRatherThanPushingItAway) {
	// A long buffer and a pager together: the pager takes its rows first and the
	// edit view windows into what is left, so the edit line is never pushed off
	// the top by a list of candidates.
	cluster_pool pool;
	state s = sized_state(4, 6);
	type(s, "aaaabbbbccccdddd");   // four rows of content at width four
	feed what = words({"x1", "y2", "z3"});
	what.from = 16;
	what.to = 16;
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);

	const layout made = lay_out(pool, input_for(s, ""));
	EXPECT_EQ(made.content_rows, 5) << "sixteen bytes at four columns, plus the cursor row";
	EXPECT_EQ(made.pager_rows, 3);
	EXPECT_EQ(made.edit_rows(), 3);
	EXPECT_EQ(made.screen.rows(), 6);
	EXPECT_TRUE(made.scrolled());
	// The cursor is inside the edit view, which is the invariant lay_out asserts.
	EXPECT_LT(made.screen.cursor().row, made.edit_rows());
}

TEST(LeshperPagerLayout, TwoCallsWithEqualInputsStillProduceEqualLayouts) {
	cluster_pool pool;
	state s = sized_state(20, 8);
	feed what = words({"alpha", "beta"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	const layout_input in = input_for(s, "$ ");
	EXPECT_TRUE(lay_out(pool, in) == lay_out(pool, in));
}

// ---------------------------------------------------------------------------
// One pager, three clients (#137 decision 3).
// ---------------------------------------------------------------------------

namespace {

// A reactor that proposes a fixed list under a fixed kind - the shape a history
// searcher and an autosuggester both have from the pager's side.
struct proposer {
	std::uint32_t kind = LESH_PROPOSAL_HISTORY_MATCH;
	std::vector<std::string> entries;
};

int32_t propose_all(lesh_request* request, void* self) {
	const proposer& me = *static_cast<proposer*>(self);
	for (const std::string& one : me.entries)
		lesh_propose(request, me.kind, one.data(), one.size());
	return LESH_OK;
}

// One registry and one harness for dispatch; what is APPLIED lives in the state
// (#144), which is where `lesh_proposal_read` reads it from.
struct proposal_fixture {
	registry reg;
	loop_harness loop{reg};
	proposer source;

	proposal_fixture() {
		register_builtin_actions(reg);
		register_pager_actions(reg);
		lesh_reactor_register(&reg, "test_proposer", LESH_EVENT_BUFFER_CHANGED,
		                      &propose_all, &source);
	}

	void show(state& s) {
		for (reactor_batch& one : loop.react(s, LESH_EVENT_BUFFER_CHANGED))
			apply_batch(s, one);
	}
};

} // namespace

TEST(LeshperPagerClients, HistoryMatchesFillTheSameSurfaceThroughTheSameAction) {
	proposal_fixture fixture;
	state s = sized_state();
	apply_edit(s, s.cursor, s.cursor, "git");
	fixture.source.kind = LESH_PROPOSAL_HISTORY_MATCH;
	fixture.source.entries = {"git log --oneline", "grep -r x ."};
	fixture.show(s);

	fixture.loop.invoke(s, "pager_show_history_matches", invocation{});
	ASSERT_TRUE(s.pager.open) << "two entries that share no extension of `git` open a list";
	EXPECT_EQ(s.pager.matching.size(), 2u);
	EXPECT_EQ(s.pager.candidates[0].kind, pager_kind::plain);
	// A history entry replaces the whole line, which is the span this client
	// opened on.
	EXPECT_EQ(s.pager.replace_from.byte_offset(), 0u);
	EXPECT_EQ(s.pager.replace_to.byte_offset(), 3u);

	fixture.loop.invoke(s, "pager_accept", invocation{});
	EXPECT_EQ(text_of(s), "git log --oneline");
}

TEST(LeshperPagerClients, SuggestionsFillItToo) {
	proposal_fixture fixture;
	state s = sized_state();
	apply_edit(s, s.cursor, s.cursor, "gi");
	fixture.source.kind = LESH_PROPOSAL_AUTOSUGGESTION;
	fixture.source.entries = {"t status"};
	fixture.show(s);

	// One candidate is unambiguous, so F-30 inserts it and no list ever appears -
	// which is what accepting a suggestion has always looked like.
	fixture.loop.invoke(s, "pager_show_suggestions", invocation{});
	EXPECT_FALSE(s.pager.open);
	EXPECT_EQ(text_of(s), "git status");
}

TEST(LeshperPagerClients, NothingProposedIsNotAnError) {
	proposal_fixture fixture;
	state s = sized_state();
	const action_result ran =
		fixture.loop.invoke(s, "pager_show_completions", invocation{});
	EXPECT_EQ(ran.status, LESH_OK);
	EXPECT_FALSE(s.pager.open);
}

// ---------------------------------------------------------------------------
// The ABI's own edges.
// ---------------------------------------------------------------------------

TEST(LeshperPagerAbi, AnUnknownCandidateKindIsRefused) {
	state s = sized_state();
	feed what;
	what.candidates = {{"alpha", 99u}};
	EXPECT_EQ(fill(s, what), LESH_PAGER_NOTHING);
	EXPECT_EQ(what.last_add, LESH_ERR_INVAL);
}

TEST(LeshperPagerAbi, TheRangeAndStatusReadBackWhatTheClientOpenedOn) {
	state s = sized_state();
	type(s, "cat RE");
	feed what = words({"README", "RELEASE"});
	what.from = 4;
	what.to = 6;
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);

	struct probe {
		int32_t open = -1;
		std::size_t count = 0;
		std::size_t selected = 0;
		std::size_t from = 0;
		std::size_t to = 0;
		std::string text;
		std::uint32_t kind = 0;
	} seen;

	registry& reg = context_of(s).actions();
	lesh_action_register(
		&reg, "test_probe_pager",
		[](lesh_editor* editor, const lesh_invocation*, void* self) -> int32_t {
			probe& me = *static_cast<probe*>(self);
			lesh_pager_status(editor, &me.open, &me.count, &me.selected);
			lesh_pager_range(editor, &me.from, &me.to);
			char out[64] = {};
			std::size_t length = 0;
			lesh_pager_selected(editor, out, sizeof(out), &length, &me.kind);
			me.text.assign(out, length);
			return LESH_OK;
		},
		&seen);
	context_of(s).loop().invoke(s, "test_probe_pager", invocation{});

	EXPECT_EQ(seen.open, 1);
	EXPECT_EQ(seen.count, 2u);
	EXPECT_EQ(seen.selected, 0u);
	EXPECT_EQ(seen.from, 4u);
	EXPECT_EQ(seen.to, 6u);
	EXPECT_EQ(seen.text, "README");
	EXPECT_EQ(seen.kind, LESH_PAGER_WORD);
}

// ---------------------------------------------------------------------------
// N-3: what the replay compare sees.
// ---------------------------------------------------------------------------

TEST(LeshperPagerReplay, TwoStatesThatWereFedAndDrivenAlikeCompareEqual) {
	state one = sized_state();
	state two = sized_state();
	feed first = words({"alpha", "alps", "beta"});
	feed second = words({"alpha", "alps", "beta"});
	ASSERT_EQ(fill(one, first), LESH_PAGER_OPENED);
	ASSERT_EQ(fill(two, second), LESH_PAGER_OPENED);
	press(one, U'a');
	press(two, U'a');
	press(one, tab_key);
	press(two, tab_key);
	EXPECT_TRUE(one == two);
}

TEST(LeshperPagerReplay, AKeyThatMovedTheSelectionIsVisibleToTheCompare) {
	state one = sized_state();
	state two = sized_state();
	feed first = words({"alpha", "beta"});
	feed second = words({"alpha", "beta"});
	ASSERT_EQ(fill(one, first), LESH_PAGER_OPENED);
	ASSERT_EQ(fill(two, second), LESH_PAGER_OPENED);
	ASSERT_TRUE(one == two);
	press(one, tab_key);
	EXPECT_FALSE(one == two) << "a selection the keys moved must not be invisible to N-3";
}

TEST(LeshperPagerReplay, AClosedPagerCompareEqualToOneThatNeverOpened) {
	// `clear()` puts every field back, not only the list: two states that are
	// both between commands must compare equal, which is the same rule
	// `change_replay::abandon` follows.
	state one = sized_state();
	state two = sized_state();
	feed what = words({"alpha", "beta"});
	ASSERT_EQ(fill(one, what), LESH_PAGER_OPENED);
	press(one, U'a');
	press(one, tab_key);
	press(one, escape_key);
	EXPECT_TRUE(one == two);
}

// ---------------------------------------------------------------------------
// #214: the pager is the third thing on screen an action can move, and every
// move must ask for a repaint. Before the fix the menu opened invisibly - no
// generation bump, no cursor move, so `invoke`'s rule emitted nothing and Tab
// read as a hang until the next unrelated repaint.
// ---------------------------------------------------------------------------

bool asks_for_repaint(const effects& out) {
	for (const effect& one : out)
		if (std::holds_alternative<render_request>(one))
			return true;
	return false;
}

TEST(LeshperPagerRepaint, OpeningThePagerAsksForARepaint) {
	state s = sized_state();
	feed what = words({"alpha", "beta"});
	registry& reg = context_of(s).actions();
	ASSERT_EQ(lesh_action_register(&reg, "test_fill_pager_repaint", &feed_action, &what),
	          LESH_OK);
	const action_result ran =
		context_of(s).loop().invoke(s, "test_fill_pager_repaint", invocation{});
	ASSERT_EQ(what.outcome, static_cast<std::uint32_t>(LESH_PAGER_OPENED));
	EXPECT_TRUE(asks_for_repaint(ran.produced))
		<< "an invisible menu is a hang to the person waiting for it";
}

TEST(LeshperPagerRepaint, CyclingTheSelectionAsksForARepaint) {
	state s = sized_state();
	feed what = words({"aa", "bb", "cc"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	EXPECT_TRUE(asks_for_repaint(press(s, tab_key)))
		<< "Tab moved the selection; the screen must follow";
}

TEST(LeshperPagerRepaint, ClosingThePagerAsksForARepaint) {
	state s = sized_state();
	feed what = words({"aa", "bb"});
	ASSERT_EQ(fill(s, what), LESH_PAGER_OPENED);
	EXPECT_TRUE(asks_for_repaint(press(s, escape_key)))
		<< "a dismissed menu must leave the screen, not linger";
}

TEST(LeshperPagerRepaint, ANoOpPagerActionStillAsksForNothing) {
	// The counterweight: the fix compares pager identity, it does not emit
	// unconditionally. `pager_next` with no pager open changes nothing and must
	// keep asking for nothing.
	state s = sized_state();
	const action_result ran =
		context_of(s).loop().invoke(s, "pager_next", invocation{});
	EXPECT_EQ(ran.status, LESH_OK);
	EXPECT_FALSE(asks_for_repaint(ran.produced));
}
