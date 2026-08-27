#include "leshper/blit.h"
#include "leshper/layout.h"
#include "leshper/sgr.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "leshper/text.h"
#include "substrate/measure.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace lesh::leshper;

// Layout as a value (F-37), and reflow as the same value at a new size (F-38).
//
// Every test below asserts a CELL GRID and a cursor cell (N-3). None of them
// asserts a byte stream - that is the blitter's suite, one ring down - and none
// of them opens a terminal. If a change here makes a test need a PTY, the
// change is wrong.

namespace {

// The glyphs of one row, concatenated. Continuation columns contribute nothing:
// the cluster to their left already spoke for them.
std::string glyphs_of(const cluster_pool& pool, const surface& painted, uint16_t row) {
	std::string out;
	for (uint16_t column = 0; column < painted.columns(); ++column) {
		const cell& one = painted.at(row, column);
		if (!one.glyph.is_continuation())
			out.append(pool.cluster_of(one.glyph));
	}
	return out;
}

std::vector<std::string> rows_of(const cluster_pool& pool, const surface& painted) {
	std::vector<std::string> out;
	for (uint16_t row = 0; row < painted.rows(); ++row)
		out.push_back(glyphs_of(pool, painted, row));
	return out;
}

// An expected row: `head`, then blanks out to the width. Computed rather than
// written out, so an expectation is never a run of spaces nobody can count by
// eye.
std::string padded(std::string_view head, int columns) {
	std::string out{head};
	for (int filled = lesh::grapheme::display_width(head); filled < columns; ++filled)
		out.push_back(' ');
	return out;
}

layout_input sized(uint16_t columns, uint16_t rows) {
	layout_input in;
	in.columns = columns;
	in.rows = rows;
	return in;
}

position at(std::size_t offset) { return position::from_byte_offset(offset); }

// Named so a test reads as text rather than as escaped bytes.
constexpr std::string_view CJK_MIDDLE = "\xE4\xB8\xAD";  // U+4E2D, two columns
constexpr std::string_view CJK_SUN = "\xE6\x97\xA5";     // U+65E5, two columns
constexpr std::string_view FAMILY =
	"\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA6";      // woman ZWJ boy, two columns
constexpr std::string_view AMBIGUOUS = "\xC2\xA1";       // U+00A1, UAX #11 Ambiguous
constexpr std::string_view RED = "\x1B[1;31m";           // SGR: an escape, no width
constexpr std::string_view RESET = "\x1B[m";

// Twenty one-character lines, "a\nb\nc..." - line n starts at byte 2n.
std::string twenty_lines() {
	std::string out;
	for (int line = 0; line < 20; ++line) {
		out.push_back(static_cast<char>('a' + line));
		if (line != 19)
			out.push_back('\n');
	}
	return out;
}

} // namespace

// ---------------------------------------------------------------------------
// The prompt and the ordinary line.
// ---------------------------------------------------------------------------

TEST(LeshperLayout, ThePromptAndTheBufferShareTheFirstRow) {
	cluster_pool pool;
	layout_input in = sized(10, 4);
	in.prompt = "$ ";
	in.buffer = "ls";
	in.cursor = at(2);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 1);
	// One row, not the terminal's four: the surface's origin is wherever the
	// terminal's cursor is, and F-39 scrolls shell output above it.
	EXPECT_EQ(made.screen.rows(), 1);
	EXPECT_EQ(made.screen.columns(), 10);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), padded("$ ls", 10));
	EXPECT_EQ(made.cursor_row, 0);
	EXPECT_EQ(made.cursor_column, 4);
	EXPECT_TRUE(made.screen.cursor() == (cursor_placement{0, 4, true}));
	EXPECT_FALSE(made.scrolled());
}

TEST(LeshperLayout, ThePenSeparatesThePromptFromTheText) {
	cluster_pool pool;
	layout_input in = sized(8, 2);
	in.prompt = "$ ";
	in.buffer = "x";
	in.cursor = at(1);
	in.prompt_pen = style{color::of_index(4), color::of_default(), attribute::bold};
	in.text_pen = style{color::of_index(7), color::of_default(), attribute::none};

	const layout made = lay_out(pool, in);
	EXPECT_TRUE(made.screen.at(0, 0).pen == in.prompt_pen);
	EXPECT_TRUE(made.screen.at(0, 2).pen == in.text_pen);
}

TEST(LeshperLayout, AnEscapeInThePromptIsMeasuredAndNeverPainted) {
	// The invariant that carries #114 into this ring: what layout PAINTS for a
	// prompt is exactly what `display_width` MEASURES for it, so a theme author
	// never does width arithmetic and `%{ %}` folklore never exists.
	cluster_pool pool;
	layout_input in = sized(12, 2);
	const std::string prompt = std::string(RED).append("$ ").append(RESET);
	in.prompt = prompt;

	const layout made = lay_out(pool, in);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), padded("$ ", 12));
	EXPECT_EQ(made.cursor_column, lesh::grapheme::display_width(prompt));
	EXPECT_EQ(made.cursor_column, 2);
}

TEST(LeshperLayout, AnEscapeTypedIntoTheBufferIsTextAndNotAnEscape) {
	// Escapes are recognized in PROMPT bytes, which come from a provider (#94).
	// The buffer is what the user typed: the ESC is a control the surface
	// drops, and the bytes after it are the characters they are.
	cluster_pool pool;
	layout_input in = sized(8, 2);
	in.buffer = RED;
	in.cursor = at(RED.size());

	const layout made = lay_out(pool, in);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), padded("[1;31m", 8));
	EXPECT_EQ(made.cursor_column, 6);
}

TEST(LeshperLayout, AMultiLinePromptStartsANewRow) {
	cluster_pool pool;
	layout_input in = sized(6, 4);
	in.prompt = "top\n> ";
	in.buffer = "x";
	in.cursor = at(1);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("top", 6), padded("> x", 6)}));
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 3);
}

TEST(LeshperLayout, APromptLongerThanTheTerminalWrapsLikeAnyOtherText) {
	cluster_pool pool;
	layout_input in = sized(4, 4);
	in.prompt = "abcdef";
	in.buffer = "g";
	in.cursor = at(1);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("abcd", 4), padded("efg", 4)}));
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 3);
}

// ---------------------------------------------------------------------------
// Soft wrap (decision 1).
// ---------------------------------------------------------------------------

TEST(LeshperLayout, ALineLongerThanTheColumnsContinuesAtColumnZero) {
	cluster_pool pool;
	layout_input in = sized(4, 4);
	in.buffer = "abcdef";
	in.cursor = at(6);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("abcd", 4), padded("ef", 4)}));
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 2);
}

TEST(LeshperLayout, ASoftWrappedRowGetsNoSecondaryPrompt) {
	// A wrapped row is not a new logical line, and F-36's secondary prompt is
	// about logical lines. Drawing one here would also mean the buffer's own
	// bytes moved right when the terminal narrowed.
	cluster_pool pool;
	layout_input in = sized(4, 4);
	in.continuation = "> ";
	in.buffer = "abcdef";
	in.cursor = at(6);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("abcd", 4), padded("ef", 4)}));
}

TEST(LeshperLayout, AWideClusterAtTheRightEdgeStartsTheNextRowWhole) {
	// The ticket's rule, and the reason wrapping cannot be "clip at the edge":
	// the blitter has no way to emit half a glyph, so the alternatives were a
	// whole cluster on the next row or a character the user typed disappearing.
	cluster_pool pool;
	layout_input in = sized(4, 4);
	const std::string buffer = std::string("abc").append(CJK_MIDDLE);
	in.buffer = buffer;
	in.cursor = at(buffer.size());

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_TRUE(made.screen.at(0, 3) == blank_cell);  // the column it refused to straddle
	EXPECT_EQ(pool.cluster_of(made.screen.at(1, 0).glyph), CJK_MIDDLE);
	EXPECT_EQ(made.screen.at(1, 0).width, 2);
	EXPECT_TRUE(made.screen.at(1, 1).glyph.is_continuation());
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 2);
}

TEST(LeshperLayout, AZwjSequenceWrapsAsOneImageRatherThanAsItsCodepoints) {
	cluster_pool pool;
	layout_input in = sized(3, 4);
	const std::string buffer = std::string("ab").append(FAMILY).append("c");
	in.buffer = buffer;
	in.cursor = at(2);  // on the family itself

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), padded("ab", 3));
	EXPECT_EQ(pool.cluster_of(made.screen.at(1, 0).glyph), FAMILY);
	EXPECT_EQ(made.screen.at(1, 0).width, 2);
	EXPECT_TRUE(made.screen.at(1, 1).glyph.is_continuation());
	EXPECT_EQ(pool.cluster_of(made.screen.at(1, 2).glyph), "c");
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 0);
}

TEST(LeshperLayout, WidthComesFromThePolicySeamSoWrappingDoesToo) {
	// #108 decision 3 reaching all the way up: a CJK locale's terminal widens
	// UAX #11 Ambiguous, and the line breaks somewhere else with nothing in
	// this file changing.
	cluster_pool pool;
	layout_input narrow = sized(4, 4);
	const std::string three =
		std::string(AMBIGUOUS).append(AMBIGUOUS).append(AMBIGUOUS);
	narrow.buffer = three;
	narrow.cursor = at(three.size());
	layout_input wide = narrow;
	wide.width.ambiguous = 2;

	EXPECT_EQ(lay_out(pool, narrow).content_rows, 1);
	EXPECT_EQ(lay_out(pool, wide).content_rows, 2);
}

TEST(LeshperLayout, AClusterWiderThanTheWholeTerminalStillTerminates) {
	// One column and a two-column cluster: it can never be placed, and the one
	// thing that must not happen is a walk that fails to advance.
	cluster_pool pool;
	layout_input in = sized(1, 3);
	const std::string buffer = std::string(CJK_MIDDLE).append("a");
	in.buffer = buffer;
	in.cursor = at(3);  // on the 'a'

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_TRUE(made.screen.at(0, 0) == blank_cell);  // the surface refuses to straddle
	EXPECT_EQ(pool.cluster_of(made.screen.at(1, 0).glyph), "a");
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 0);
}

// ---------------------------------------------------------------------------
// Multi-line buffers (F-2, decision 2).
// ---------------------------------------------------------------------------

TEST(LeshperLayout, ANewlineStartsALogicalLineHeadedByTheSecondaryPrompt) {
	cluster_pool pool;
	layout_input in = sized(8, 4);
	in.prompt = "$ ";
	in.continuation = "> ";
	in.buffer = "for x\ndo";
	in.cursor = at(8);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("$ for x", 8), padded("> do", 8)}));
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 4);
}

TEST(LeshperLayout, EveryLineIsRenderedIncludingAnEmptyLastOne) {
	// F-2: the buffer is rendered whole. A trailing newline HAS a last, empty
	// line, and the cursor has to be able to sit on it.
	cluster_pool pool;
	layout_input in = sized(8, 6);
	in.continuation = "> ";
	in.buffer = "a\n\nb\n";
	in.cursor = at(5);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 4);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("a", 8), padded("> ", 8), padded("> b", 8),
	                                    padded("> ", 8)}));
	EXPECT_EQ(made.cursor_row, 3);
	EXPECT_EQ(made.cursor_column, 2);
}

TEST(LeshperLayout, CarriageReturnLineFeedIsOneBreakAndNotTwo) {
	// UAX #29's GB3 keeps CR LF together as one cluster, so a byte scan for
	// '\n' would have split a cluster in half.
	cluster_pool pool;
	layout_input in = sized(6, 4);
	in.buffer = "a\r\nb";
	in.cursor = at(4);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("a", 6), padded("b", 6)}));
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 1);
}

TEST(LeshperLayout, ALogicalLineWrapsAndTheNextStillStartsALine) {
	cluster_pool pool;
	layout_input in = sized(4, 6);
	in.continuation = "> ";
	in.buffer = "abcdef\nz";
	in.cursor = at(8);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 3);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("abcd", 4), padded("ef", 4), padded("> z", 4)}));
	EXPECT_EQ(made.cursor_row, 2);
	EXPECT_EQ(made.cursor_column, 3);
}

// ---------------------------------------------------------------------------
// What kind of line a row begins (#189). The blitter reaches a soft row by
// writing through the right edge and a hard row by positioning to it, so that
// the terminal's idea of the frame and this one agree across a resize - and the
// only place the two can be told apart is here, in the walk that made them.
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<bool> hard_rows_of(const surface& screen) {
	std::vector<bool> answer;
	for (std::uint16_t row = 0; row < screen.rows(); ++row)
		answer.push_back(screen.row_starts_hard_line(row));
	return answer;
}

TEST(LeshperLayout, AWrappedRowIsSoftAndARowAfterANewlineIsHard) {
	cluster_pool pool;
	layout_input in = sized(4, 6);
	in.continuation = "> ";
	in.buffer = "abcdef\nz";
	in.cursor = at(8);

	const layout made = lay_out(pool, in);
	// Row zero always hard - it is the top of the frame, and nothing above it
	// wrapped into it. Row one is where `abcdef` ran out of columns. Row two is
	// where the newline is.
	EXPECT_EQ(hard_rows_of(made.screen), (std::vector<bool>{true, false, true}));
}

TEST(LeshperLayout, TheEmptyRowThePhantomColumnPushesTheCursorOntoIsSoft) {
	// It exists only because the row above filled, which is exactly what a soft
	// wrap is - and it matters, because painting it as a hard row is what left
	// the previous frame's top row behind when the window grew.
	cluster_pool pool;
	layout_input in = sized(4, 4);
	in.buffer = "abcd";
	in.cursor = at(4);

	const layout made = lay_out(pool, in);
	ASSERT_EQ(made.screen.rows(), 2);
	EXPECT_EQ(hard_rows_of(made.screen), (std::vector<bool>{true, false}));
}

TEST(LeshperLayout, ANewlineAtExactlyTheRightEdgeStartsAHardRowAndNotASoftOne) {
	// The ambiguity decision 3 accepts, answered for the blitter: the end of a
	// full line and the start of the next are the same cell, and the cursor's
	// own move onto the next row calls it soft before the newline is read. The
	// newline is the later and truer word on it.
	cluster_pool pool;
	layout_input in = sized(4, 4);
	in.continuation = "> ";
	in.buffer = "abcd\nz";
	in.cursor = at(4);

	const layout made = lay_out(pool, in);
	ASSERT_EQ(made.screen.rows(), 2);
	EXPECT_EQ(hard_rows_of(made.screen), (std::vector<bool>{true, true}));
}

TEST(LeshperLayout, TheTopRowOfAWindowedViewIsHardWhateverItContinues) {
	// A scrolled view's first row IS a continuation of content above it, but the
	// frame does not contain that content: the blitter positions the cursor to
	// the frame's top row, and there is no wrap on screen for it to write
	// through. Row zero is hard by construction, and only the rows below it can
	// be reached through a wrap.
	cluster_pool pool;
	layout_input in = sized(4, 2);
	in.buffer = "abcdefghijkl";
	in.cursor = at(12);

	const layout made = lay_out(pool, in);
	ASSERT_TRUE(made.scrolled());
	ASSERT_EQ(made.screen.rows(), 2);
	EXPECT_GT(made.first_visible_row, 0);
	EXPECT_EQ(hard_rows_of(made.screen), (std::vector<bool>{true, false}));
}

// ---------------------------------------------------------------------------
// The cursor (decision 3).
// ---------------------------------------------------------------------------

TEST(LeshperLayout, AnEmptyBufferPutsTheCursorImmediatelyAfterThePrompt) {
	cluster_pool pool;
	layout_input in = sized(10, 4);
	in.prompt = "lesh> ";

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 1);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), padded("lesh> ", 10));
	EXPECT_EQ(made.cursor_row, 0);
	EXPECT_EQ(made.cursor_column, 6);
	EXPECT_TRUE(made.screen.cursor().visible);
}

TEST(LeshperLayout, AnEmptyBufferAndAnEmptyPromptPutsItAtTheOrigin) {
	cluster_pool pool;
	const layout made = lay_out(pool, sized(4, 2));
	EXPECT_EQ(made.content_rows, 1);
	EXPECT_EQ(made.screen.rows(), 1);
	EXPECT_TRUE(made.screen.cursor() == (cursor_placement{0, 0, true}));
}

TEST(LeshperLayout, TheCursorNeverSitsInThePhantomColumn) {
	// A buffer that ends exactly at the right edge. The blitter starts every
	// move with `\r`, which cancels the terminal's pending wrap, so a cursor
	// "in" column four of a four-column terminal is not expressible in the
	// bytes it can emit. It goes to the start of the next row instead, and the
	// content is one row taller because of it.
	cluster_pool pool;
	layout_input in = sized(4, 4);
	in.buffer = "abcd";
	in.cursor = at(4);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_EQ(made.screen.rows(), 2);
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 0);
	EXPECT_EQ(glyphs_of(pool, made.screen, 1), padded("", 4));
}

TEST(LeshperLayout, TheCursorSitsOnTheClusterItsOffsetIsIn) {
	cluster_pool pool;
	layout_input in = sized(8, 2);
	const std::string buffer = std::string(CJK_MIDDLE).append(CJK_SUN);
	in.buffer = buffer;

	in.cursor = at(0);
	EXPECT_EQ(lay_out(pool, in).cursor_column, 0);
	in.cursor = at(3);
	EXPECT_EQ(lay_out(pool, in).cursor_column, 2);
	in.cursor = at(6);
	EXPECT_EQ(lay_out(pool, in).cursor_column, 4);

	// An offset inside a cluster - which #88's boundaries make an error, not a
	// state - lands on that cluster's cell rather than between two.
	in.cursor = at(4);
	EXPECT_EQ(lay_out(pool, in).cursor_column, 2);
}

TEST(LeshperLayout, TheCursorFollowsTheTextOntoAWrappedRow) {
	cluster_pool pool;
	layout_input in = sized(4, 4);
	in.prompt = "$ ";
	in.buffer = "abcd";
	in.cursor = at(3);

	const layout made = lay_out(pool, in);
	// "$ ab" fills row zero; "cd" is on row one, and the cursor is before 'd'.
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("$ ab", 4), padded("cd", 4)}));
	EXPECT_EQ(made.cursor_row, 1);
	EXPECT_EQ(made.cursor_column, 1);
}

// ---------------------------------------------------------------------------
// The view scrolls; the terminal never does (decision 4).
// ---------------------------------------------------------------------------

TEST(LeshperLayout, ContentShorterThanTheTerminalClaimsOnlyTheRowsItUses) {
	cluster_pool pool;
	layout_input in = sized(8, 24);
	in.buffer = "a\nb";
	in.cursor = at(3);

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.screen.rows(), 2);
	EXPECT_EQ(made.content_rows, 2);
	EXPECT_EQ(made.first_visible_row, 0);
	EXPECT_FALSE(made.scrolled());
}

TEST(LeshperLayout, AViewTallerThanTheTerminalIsAWindowCentredOnTheCursor) {
	cluster_pool pool;
	const std::string buffer = twenty_lines();
	layout_input in = sized(4, 5);
	in.buffer = buffer;
	in.cursor = at(20);  // line ten, "k"

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.content_rows, 20);
	EXPECT_EQ(made.screen.rows(), 5);
	EXPECT_TRUE(made.scrolled());
	EXPECT_EQ(made.first_visible_row, 8);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("i", 4), padded("j", 4), padded("k", 4),
	                                    padded("l", 4), padded("m", 4)}));
	EXPECT_EQ(made.cursor_row, 10);
	EXPECT_TRUE(made.screen.cursor() == (cursor_placement{2, 0, true}));
}

TEST(LeshperLayout, TheWindowClampsAtBothEndsOfTheContent) {
	cluster_pool pool;
	const std::string buffer = twenty_lines();
	layout_input in = sized(4, 5);
	in.buffer = buffer;

	in.cursor = at(0);  // the top of the content, not a half-empty window
	const layout top = lay_out(pool, in);
	EXPECT_EQ(top.first_visible_row, 0);
	EXPECT_EQ(rows_of(pool, top.screen),
	          (std::vector<std::string>{padded("a", 4), padded("b", 4), padded("c", 4),
	                                    padded("d", 4), padded("e", 4)}));
	EXPECT_TRUE(top.screen.cursor() == (cursor_placement{0, 0, true}));

	in.cursor = at(buffer.size());  // the last line
	const layout bottom = lay_out(pool, in);
	EXPECT_EQ(bottom.first_visible_row, 15);
	EXPECT_EQ(rows_of(pool, bottom.screen),
	          (std::vector<std::string>{padded("p", 4), padded("q", 4), padded("r", 4),
	                                    padded("s", 4), padded("t", 4)}));
	EXPECT_TRUE(bottom.screen.cursor() == (cursor_placement{4, 1, true}));
}

TEST(LeshperLayout, TheWindowMovesByOneRowPerOneRowCursorMove) {
	// Why centred and not "the last screenful": a window that jumps reads as a
	// redraw rather than as scrolling.
	cluster_pool pool;
	const std::string buffer = twenty_lines();
	layout_input in = sized(4, 5);
	in.buffer = buffer;

	int previous = -1;
	for (int line = 3; line <= 16; ++line) {
		in.cursor = at(static_cast<std::size_t>(2 * line));
		const layout made = lay_out(pool, in);
		if (previous >= 0)
			EXPECT_LE(made.first_visible_row - previous, 1);
		previous = made.first_visible_row;
		// Whatever the window is, the cursor is inside it.
		EXPECT_GE(made.cursor_row, made.first_visible_row);
		EXPECT_LT(made.cursor_row - made.first_visible_row, made.screen.rows());
	}
}

TEST(LeshperLayout, ASizeNobodyHasReportedDrawsNothing) {
	cluster_pool pool;
	layout_input in = sized(0, 0);
	in.prompt = "$ ";
	in.buffer = "ls";

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.screen.columns(), 0);
	EXPECT_EQ(made.screen.rows(), 0);
	EXPECT_EQ(made.content_rows, 0);
	EXPECT_TRUE(made == layout{});
}

// ---------------------------------------------------------------------------
// A value, and reflow (F-37, F-38).
// ---------------------------------------------------------------------------

TEST(LeshperLayout, EqualInputsProduceEqualLayouts) {
	cluster_pool pool;
	layout_input in = sized(6, 4);
	in.prompt = "$ ";
	in.buffer = "abcdefgh";
	in.cursor = at(5);

	EXPECT_TRUE(lay_out(pool, in) == lay_out(pool, in));

	layout_input moved = in;
	moved.cursor = at(6);
	EXPECT_FALSE(lay_out(pool, in) == lay_out(pool, moved));
}

TEST(LeshperLayout, ReflowIsTheSameContentLaidOutAtTheNewSize) {
	cluster_pool pool;
	layout_input in = sized(24, 6);
	in.prompt = "$ ";
	in.buffer = "echo one two three";
	in.cursor = at(18);

	const layout before = lay_out(pool, in);
	EXPECT_EQ(before.content_rows, 1);

	const layout after = reflow(pool, in, resize_event{8, 6});
	EXPECT_EQ(after.screen.columns(), 8);
	EXPECT_EQ(after.content_rows, 3);
	EXPECT_EQ(rows_of(pool, after.screen),
	          (std::vector<std::string>{padded("$ echo o", 8), padded("ne two t", 8),
	                                    padded("hree", 8)}));
	EXPECT_EQ(after.cursor_row, 2);
	EXPECT_EQ(after.cursor_column, 4);

	// The same function at the new size, and nothing carried across it.
	layout_input directly = in;
	directly.columns = 8;
	EXPECT_TRUE(after == lay_out(pool, directly));
}

TEST(LeshperLayout, ReflowMidEditKeepsTheCursorOnTheCharacterItWasOn) {
	// F-38's "including mid-edit": the cursor is a buffer position, so it moves
	// with the text it is in rather than staying at a screen cell.
	cluster_pool pool;
	layout_input in = sized(12, 6);
	in.buffer = "abcdefghij";
	in.cursor = at(6);

	const layout wide = lay_out(pool, in);
	EXPECT_EQ(wide.cursor_row, 0);
	EXPECT_EQ(wide.cursor_column, 6);

	const layout narrow = reflow(pool, in, resize_event{4, 6});
	EXPECT_EQ(narrow.cursor_row, 1);
	EXPECT_EQ(narrow.cursor_column, 2);
	EXPECT_EQ(rows_of(pool, narrow.screen),
	          (std::vector<std::string>{padded("abcd", 4), padded("efgh", 4),
	                                    padded("ij", 4)}));
}

TEST(LeshperLayout, ASizeChangeOwesAPaintAndNotADiff) {
	// #112: `update` refuses two surfaces of different shapes, so F-37's "full
	// repaint only on resize" is a question asked of two values rather than a
	// flag somebody has to remember to set.
	cluster_pool pool;
	layout_input in = sized(24, 6);
	in.prompt = "$ ";
	in.buffer = "echo one two three";
	in.cursor = at(18);

	const layout before = lay_out(pool, in);
	const layout again = lay_out(pool, in);
	EXPECT_TRUE(can_diff(before, again));

	const layout resized = reflow(pool, in, resize_event{8, 6});
	EXPECT_FALSE(can_diff(before, resized));

	// And both paths produce bytes, which is all this suite says about bytes:
	// what they SAY is the blitter's suite.
	const blitter to_bytes{pool};
	EXPECT_TRUE(to_bytes.update(before.screen, again.screen).empty());
	EXPECT_FALSE(to_bytes.paint(resized.screen).empty());
}

TEST(LeshperLayout, TheStateIsWhereTheInputComesFrom) {
	cluster_pool pool;
	state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), "ls -l");
	s.cursor = s.buffer.end_position();
	s.columns = 10;
	s.rows = 4;

	const layout made = lay_out(pool, input_for(s, "$ ", "> "));
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), padded("$ ls -l", 10));
	EXPECT_EQ(made.cursor_column, 7);
	EXPECT_EQ(made.screen.cursor().column, 7);
}

// ---------------------------------------------------------------------------
// A prompt's SGR is a pen (#131).
// ---------------------------------------------------------------------------

namespace {

// The pen a terminal reading `bytes` would hold when it reached the first byte
// that is not part of an escape sequence. Written the way `sgr.h` is meant to
// be driven - every recognized escape fed to it, SGR or not - so the round-trip
// test below reads the blitter's real output rather than a hand-picked slice.
style pen_at_first_glyph(std::string_view bytes) {
	style pen;
	std::size_t i = 0;
	while (i < bytes.size()) {
		if (static_cast<unsigned char>(bytes[i]) != 0x1B)
			break;
		const std::size_t length = lesh::grapheme::measure_detail::escape_length(bytes, i);
		if (length == 0)
			break;
		pen = apply_sgr(bytes.substr(i, length), pen);
		i += length;
	}
	return pen;
}

// Somewhere for a test to start from that is not the default, so "unchanged"
// and "reset" cannot pass for each other.
const style STYLED{color::of_index(5), color::of_rgb(9, 9, 9),
                   attribute::bold | attribute::italic};

} // namespace

TEST(LeshperLayoutSgr, ZeroIsTheTerminalDefaultAndSoIsAnEmptyParameter) {
	// The blitter's `reset_pen` writes exactly these bytes for exactly this
	// style. A reader whose `0` meant "back to the caller's prompt_pen" would
	// not be the inverse of that, it would be a second dialect.
	EXPECT_TRUE(apply_sgr("\x1B[0m", STYLED) == style{});
	EXPECT_TRUE(apply_sgr("\x1B[m", STYLED) == style{});
	EXPECT_TRUE(apply_sgr("\x1B[0;0m", STYLED) == style{});
}

TEST(LeshperLayoutSgr, ThePaletteIsSpelledInFourRangesAndStaysIndexed) {
	// Indexed and NOT resolved to RGB: the user may have redefined slot 2, and
	// picking a green here would be `surface.h`'s quantization mistake made at
	// the other end.
	EXPECT_TRUE(apply_sgr("\x1B[32m", style{}).fg == color::of_index(2));
	EXPECT_TRUE(apply_sgr("\x1B[92m", style{}).fg == color::of_index(10));
	EXPECT_TRUE(apply_sgr("\x1B[41m", style{}).bg == color::of_index(1));
	EXPECT_TRUE(apply_sgr("\x1B[101m", style{}).bg == color::of_index(9));
	EXPECT_TRUE(apply_sgr("\x1B[30m", style{}).fg == color::of_index(0));
	EXPECT_TRUE(apply_sgr("\x1B[47m", style{}).bg == color::of_index(7));

	// 39 and 49 are one channel each, and leave the other alone.
	const style both = apply_sgr("\x1B[32;41m", style{});
	EXPECT_TRUE(apply_sgr("\x1B[39m", both).fg == color::of_default());
	EXPECT_TRUE(apply_sgr("\x1B[39m", both).bg == color::of_index(1));
	EXPECT_TRUE(apply_sgr("\x1B[49m", both).bg == color::of_default());
	EXPECT_TRUE(apply_sgr("\x1B[49m", both).fg == color::of_index(2));
}

TEST(LeshperLayoutSgr, EveryAttributeAndItsOff) {
	const style all = apply_sgr("\x1B[1;2;3;4;7;9m", style{});
	EXPECT_TRUE(all.attrs == (attribute::bold | attribute::dim | attribute::italic
	                          | attribute::underline | attribute::reverse
	                          | attribute::strikethrough));

	// 22 clears bold AND dim, which is not this reader being lossy - it is why
	// the blitter resets and restates rather than turning one attribute off.
	EXPECT_FALSE(has(apply_sgr("\x1B[22m", all).attrs, attribute::bold));
	EXPECT_FALSE(has(apply_sgr("\x1B[22m", all).attrs, attribute::dim));
	EXPECT_TRUE(has(apply_sgr("\x1B[22m", all).attrs, attribute::italic));

	EXPECT_FALSE(has(apply_sgr("\x1B[23m", all).attrs, attribute::italic));
	EXPECT_FALSE(has(apply_sgr("\x1B[24m", all).attrs, attribute::underline));
	EXPECT_FALSE(has(apply_sgr("\x1B[27m", all).attrs, attribute::reverse));
	EXPECT_FALSE(has(apply_sgr("\x1B[29m", all).attrs, attribute::strikethrough));

	// The colours are not attributes and an attribute off does not touch them.
	EXPECT_TRUE(apply_sgr("\x1B[22;23;24;27;29m", STYLED).fg == STYLED.fg);
	EXPECT_TRUE(apply_sgr("\x1B[22;23;24;27;29m", STYLED).attrs == attribute::none);
}

TEST(LeshperLayoutSgr, UndercurlIsTheBlittersOwnColonSpelling) {
	// #97's one opportunistic attribute, and the blitter writes it as `4:3`. A
	// reader that only knew `ESC[4m` would round-trip an undercurl into a plain
	// underline every time the two directions met.
	const style curly = apply_sgr("\x1B[4:3m", style{});
	EXPECT_TRUE(has(curly.attrs, attribute::undercurl));
	EXPECT_FALSE(has(curly.attrs, attribute::underline));

	// One attribute at the terminal, so a plain 4 REPLACES it rather than
	// joining it - holding both would emit `4:3` for a prompt that asked for 4.
	EXPECT_TRUE(apply_sgr("\x1B[4m", curly).attrs == attribute::underline);
	// 24 clears whichever of the two is in force.
	EXPECT_TRUE(apply_sgr("\x1B[24m", curly).attrs == attribute::none);
	EXPECT_TRUE(apply_sgr("\x1B[4:0m", curly).attrs == attribute::none);
	// Double, dotted and dashed have no cell to live in and degrade to a plain
	// underline, which is what the surface can hold.
	EXPECT_TRUE(apply_sgr("\x1B[4:2m", style{}).attrs == attribute::underline);
	EXPECT_TRUE(apply_sgr("\x1B[4:5m", style{}).attrs == attribute::underline);
}

TEST(LeshperLayoutSgr, IndexedAndTruecolorInBothSpellings) {
	EXPECT_TRUE(apply_sgr("\x1B[38;5;196m", style{}).fg == color::of_index(196));
	EXPECT_TRUE(apply_sgr("\x1B[48;5;17m", style{}).bg == color::of_index(17));
	EXPECT_TRUE(apply_sgr("\x1B[38;2;10;20;30m", style{}).fg == color::of_rgb(10, 20, 30));
	EXPECT_TRUE(apply_sgr("\x1B[48;2;0;0;255m", style{}).bg == color::of_rgb(0, 0, 255));

	// The colon form is the same statement; terminals emit both and the
	// blitter's own `4:3` already proves the separator is in the vocabulary.
	EXPECT_TRUE(apply_sgr("\x1B[38:5:196m", style{}).fg == color::of_index(196));
	EXPECT_TRUE(apply_sgr("\x1B[48:2:10:20:30m", style{}).bg == color::of_rgb(10, 20, 30));

	// An indexed colour is not a truecolour that has not been converted yet -
	// they are different statements and both survive the read.
	EXPECT_TRUE(apply_sgr("\x1B[38;5;4m", style{}).fg != apply_sgr("\x1B[38;2;0;0;128m",
	                                                               style{}).fg);
}

TEST(LeshperLayoutSgr, AnUnknownOrMalformedParameterIsDroppedAndTheRestStillParses) {
	// 5 is blink and 53 is overline; neither has a cell to live in. Abandoning
	// the sequence at one would make a prompt's colours depend on whether an
	// unrelated terminal feature appeared earlier in the same ESC[...m.
	const style kept = apply_sgr("\x1B[1;5;53;31m", style{});
	EXPECT_TRUE(has(kept.attrs, attribute::bold));
	EXPECT_TRUE(kept.fg == color::of_index(1));

	// A parameter past what a CSI parameter can hold clamps at the numeric
	// policy's limit and lands in the ignored range, which is where it belongs.
	EXPECT_TRUE(apply_sgr("\x1B[99999999999999999999;31m", style{}).fg
	            == color::of_index(1));

	// An out-of-range or truncated extended colour drops THAT colour and
	// nothing else. The parameters it consumed were the colour's either way.
	EXPECT_TRUE(apply_sgr("\x1B[38;5;999;1m", style{}).fg == color::of_default());
	EXPECT_TRUE(has(apply_sgr("\x1B[38;5;999;1m", style{}).attrs, attribute::bold));
	EXPECT_TRUE(apply_sgr("\x1B[38;5m", STYLED).fg == STYLED.fg);
	EXPECT_TRUE(apply_sgr("\x1B[38;2;1;2m", STYLED).fg == STYLED.fg);
	// A T.416 form the cell cannot hold - transparent, CMY, CMYK.
	EXPECT_TRUE(apply_sgr("\x1B[38;1m", STYLED).fg == STYLED.fg);
}

TEST(LeshperLayoutSgr, EverythingThatIsNotSGRLeavesThePenExactlyAsItWas) {
	// #114 recognizes CSI, OSC and SS3; only one of the three is a pen, and the
	// other two keep the treatment they already had - consumed whole, zero
	// width, no colour.
	EXPECT_TRUE(apply_sgr("\x1B]0;a title\x07", STYLED) == STYLED);
	EXPECT_TRUE(apply_sgr("\x1BOA", STYLED) == STYLED);
	EXPECT_TRUE(apply_sgr("\x1B[2C", STYLED) == STYLED);
	EXPECT_TRUE(apply_sgr("\x1B[K", STYLED) == STYLED);
	// A private-mode string is somebody else's grammar even when it ends in 'm'.
	EXPECT_TRUE(apply_sgr("\x1B[?1m", STYLED) == STYLED);
	EXPECT_TRUE(apply_sgr("\x1B[>4m", STYLED) == STYLED);
	// An intermediate byte says the same thing.
	EXPECT_TRUE(apply_sgr("\x1B[1 m", STYLED) == STYLED);
	// And so does anything that is not an escape at all.
	EXPECT_TRUE(apply_sgr("", STYLED) == STYLED);
	EXPECT_TRUE(apply_sgr("\x1B", STYLED) == STYLED);
	EXPECT_TRUE(apply_sgr("31m", STYLED) == STYLED);
}

TEST(LeshperLayoutSgr, TheBlittersOwnBytesReadBackAsTheStyleThatWroteThem) {
	// The whole argument for this file: it is the blitter's emit vocabulary
	// read backwards, so the two directions have to agree on every style the
	// cell can hold, or one of them is a dialect. Truecolor and undercurl are
	// on so nothing is quantized or downgraded on the way out - what is being
	// checked is the vocabulary, not the capability ladder.
	cluster_pool pool;
	terminal_capabilities caps;
	caps.colors = color_depth::truecolor;
	caps.undercurl = true;
	const blitter to_bytes{pool, caps};

	const std::vector<style> wanted{
		style{},
		style{color::of_index(2), color::of_default(), attribute::none},
		style{color::of_index(9), color::of_index(4), attribute::bold | attribute::italic},
		style{color::of_rgb(1, 2, 3), color::of_rgb(250, 128, 0), attribute::underline},
		style{color::of_default(), color::of_rgb(0, 0, 0), attribute::undercurl},
		style{color::of_index(255), color::of_index(0),
		      attribute::bold | attribute::dim | attribute::italic | attribute::underline
		          | attribute::reverse | attribute::strikethrough},
	};
	for (const style& want : wanted) {
		surface one{4, 1};
		one.write(pool, 0, 0, "x", want);
		EXPECT_TRUE(pen_at_first_glyph(to_bytes.paint(one)) == want);
	}
}

TEST(LeshperLayout, AnSGRInThePromptColoursTheCellsAfterIt) {
	// The ticket's headline: `ESC[32m$ ESC[0m` used to render a plain `$`.
	cluster_pool pool;
	layout_input in = sized(10, 2);
	const std::string prompt = "\x1B[32m$ \x1B[0m";
	in.prompt = prompt;
	in.buffer = "ls";
	in.cursor = at(2);
	in.text_pen = style{color::of_index(7), color::of_default(), attribute::none};

	const layout made = lay_out(pool, in);
	const style green{color::of_index(2), color::of_default(), attribute::none};
	EXPECT_TRUE(made.screen.at(0, 0).pen == green);
	EXPECT_TRUE(made.screen.at(0, 1).pen == green);
	EXPECT_TRUE(made.screen.at(0, 2).pen == in.text_pen);

	// And #114's invariant, which is what the pen had to ride rather than
	// replace: the glyphs and the width are exactly what they were.
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), padded("$ ls", 10));
	EXPECT_EQ(made.cursor_column, lesh::grapheme::display_width(prompt) + 2);
}

TEST(LeshperLayout, ThePenChangesAtTheSGRAndNotBeforeIt) {
	cluster_pool pool;
	layout_input in = sized(12, 2);
	const std::string prompt = "ab\x1B[31mcd\x1B[1;4mef";
	in.prompt = prompt;

	const layout made = lay_out(pool, in);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), padded("abcdef", 12));
	EXPECT_EQ(made.cursor_column, lesh::grapheme::display_width(prompt));

	const style plain{};
	const style red{color::of_index(1), color::of_default(), attribute::none};
	const style loud{color::of_index(1), color::of_default(),
	                 attribute::bold | attribute::underline};
	EXPECT_TRUE(made.screen.at(0, 1).pen == plain);
	EXPECT_TRUE(made.screen.at(0, 2).pen == red);
	EXPECT_TRUE(made.screen.at(0, 3).pen == red);
	EXPECT_TRUE(made.screen.at(0, 4).pen == loud);
	// Cumulative: the second SGR added to the first rather than replacing it.
	EXPECT_TRUE(made.screen.at(0, 5).pen == loud);
}

TEST(LeshperLayout, ThePromptStartsFromThePromptPenAndItsSGRTakesItFromThere) {
	// `prompt_pen` is where the prompt STARTS. An SGR modifies it - a prompt
	// that only sets a colour keeps the caller's bold - and an `ESC[0m` means
	// the terminal's default, because that is what the blitter means by the
	// same bytes.
	cluster_pool pool;
	layout_input in = sized(12, 2);
	in.prompt = "a\x1B[31mb\x1B[0mc";
	in.prompt_pen = style{color::of_index(4), color::of_default(), attribute::bold};

	const layout made = lay_out(pool, in);
	EXPECT_TRUE(made.screen.at(0, 0).pen == in.prompt_pen);
	EXPECT_TRUE(made.screen.at(0, 1).pen
	            == (style{color::of_index(1), color::of_default(), attribute::bold}));
	EXPECT_TRUE(made.screen.at(0, 2).pen == style{});
}

TEST(LeshperLayout, ThePromptsPenNeverBleedsIntoTheBuffer) {
	// A prompt that forgets its reset is a theme author's bug, not the user's
	// text turning red. The buffer's pen is an input and its walk starts from
	// it, so there is no state for a missing `ESC[0m` to leak through.
	cluster_pool pool;
	layout_input in = sized(12, 2);
	in.prompt = "\x1B[31;1m$ ";
	in.buffer = "ls";
	in.cursor = at(2);
	in.text_pen = style{color::of_index(7), color::of_default(), attribute::none};

	const layout made = lay_out(pool, in);
	EXPECT_TRUE(made.screen.at(0, 0).pen
	            == (style{color::of_index(1), color::of_default(), attribute::bold}));
	EXPECT_TRUE(made.screen.at(0, 2).pen == in.text_pen);
	EXPECT_TRUE(made.screen.at(0, 3).pen == in.text_pen);
}

TEST(LeshperLayout, AContinuationPromptsSGRStartsFreshOnEveryLogicalLine) {
	cluster_pool pool;
	layout_input in = sized(10, 4);
	in.prompt = "$ ";
	in.continuation = "\x1B[33m> \x1B[0m";
	in.buffer = "a\nb\nc";
	in.cursor = at(5);
	in.text_pen = style{color::of_index(7), color::of_default(), attribute::none};

	const layout made = lay_out(pool, in);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("$ a", 10), padded("> b", 10),
	                                    padded("> c", 10)}));
	const style yellow{color::of_index(3), color::of_default(), attribute::none};
	// Both continuations are coloured, not just the first: each logical line
	// gets its own walk of the continuation bytes from `prompt_pen`.
	EXPECT_TRUE(made.screen.at(1, 0).pen == yellow);
	EXPECT_TRUE(made.screen.at(2, 0).pen == yellow);
	// And the buffer after it is still the buffer's.
	EXPECT_TRUE(made.screen.at(1, 2).pen == in.text_pen);
	EXPECT_TRUE(made.screen.at(2, 2).pen == in.text_pen);
}

TEST(LeshperLayout, APenCarriesAcrossASoftWrapAndAcrossAPromptsOwnNewline) {
	cluster_pool pool;
	layout_input in = sized(4, 4);
	in.prompt = "\x1B[36mabcdef";

	const layout made = lay_out(pool, in);
	EXPECT_EQ(rows_of(pool, made.screen),
	          (std::vector<std::string>{padded("abcd", 4), padded("ef", 4)}));
	const style cyan{color::of_index(6), color::of_default(), attribute::none};
	EXPECT_TRUE(made.screen.at(0, 0).pen == cyan);
	EXPECT_TRUE(made.screen.at(1, 0).pen == cyan);
	EXPECT_TRUE(made.screen.at(1, 1).pen == cyan);

	// A newline inside the prompt is a row break, not a pen break.
	layout_input lines = sized(6, 4);
	lines.prompt = "\x1B[36mtop\n> ";
	const layout two = lay_out(pool, lines);
	EXPECT_EQ(rows_of(pool, two.screen),
	          (std::vector<std::string>{padded("top", 6), padded("> ", 6)}));
	EXPECT_TRUE(two.screen.at(0, 0).pen == cyan);
	EXPECT_TRUE(two.screen.at(1, 0).pen == cyan);
}

TEST(LeshperLayout, TheWidthInvariantSurvivesEveryEscapeForm) {
	// The one thing #131 was not allowed to break, at every shape #114
	// recognizes plus the malformed ones N-4 says must degrade rather than
	// abort: what layout PAINTS is exactly what `display_width` MEASURES.
	cluster_pool pool;
	const std::string prompts[] = {
		"\x1B[32m$ \x1B[0m",
		"\x1B[1;38;2;255;0;0m" + std::string(CJK_MIDDLE) + "\x1B[m ",
		"\x1B]0;a title\x07\x1B[4:3m$\x1B[24m ",
		"\x1BOA\x1B[2C\x1B[31mx",
		"\x1B[38;5;m$ ",              // truncated extended colour
		"\x1B[38;2;1;2m$ ",           // truncated truecolour
		"\x1B[999999999999m$ ",       // a parameter no site can hold
		"\x1B[$ ",                    // an unterminated CSI: not an escape at all
		"\x1B$ ",                     // a lone ESC: a control, dropped by the surface
		"\x1B]0;never terminated",    // an unterminated OSC
	};
	for (const std::string& prompt : prompts) {
		layout_input in = sized(40, 4);
		in.prompt = prompt;
		const layout made = lay_out(pool, in);
		EXPECT_EQ(made.cursor_row, 0) << prompt;
		EXPECT_EQ(made.cursor_column, lesh::grapheme::display_width(prompt)) << prompt;
	}
}
