#include "leshper/decoration.h"
#include "leshper/editor.h"
#include "leshper/layout.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "leshper/theme.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace lesh::leshper;

// DECORATIONS, END TO END (#141): what a reactor emitted, normalized, themed,
// and painted onto cells.
//
// Three rings, and the tests are in that order. `decoration.h` decides what the
// spans MEAN once the nesting is resolved; `theme.h` decides what an interned id
// LOOKS like; `layout.cpp` puts it on the grid. None of these tests opens a
// terminal or asserts a byte stream - N-3's rule for the renderer, which the
// layout suite one file over is also held to.

namespace {

// A decorations value with one layer in it, which is what a single reactor
// produces. `apply` takes its vectors by reference and swaps, so they are named
// locals rather than temporaries.
decorations one_layer(std::string_view reactor, std::vector<decoration_span> spans,
                      std::vector<virtual_text> texts = {}) {
	decorations made;
	made.apply(reactor, spans, texts);
	return made;
}

decoration_span span(std::size_t start, std::size_t end, std::uint32_t style) {
	return decoration_span{start, end, style};
}

// A theme that knows two ids, without going near the registry: `bind` is the
// seam a configured theme would arrive at, and using it here is also what says
// the seam is usable.
style_table two_pens(style first, style second) {
	style_table table;
	table.bind(1, first);
	table.bind(2, second);
	return table;
}

const style RED{color::of_rgb(0xD7, 0x5F, 0x5F)};
const style BLUE{color::of_rgb(0x5F, 0x87, 0xD7)};

layout_input sized(std::uint16_t columns, std::uint16_t rows) {
	layout_input in;
	in.columns = columns;
	in.rows = rows;
	return in;
}

position at(std::size_t offset) { return position::from_byte_offset(offset); }

// The pen of every cell on a row, as the style ids a caller would recognize -
// written as the fg colour so a failure prints something legible.
std::vector<color> pens_of(const surface& painted, std::uint16_t row) {
	std::vector<color> out;
	for (std::uint16_t column = 0; column < painted.columns(); ++column)
		out.push_back(painted.at(row, column).pen.fg);
	return out;
}

std::string glyphs_of(const cluster_pool& pool, const surface& painted, std::uint16_t row) {
	std::string out;
	for (std::uint16_t column = 0; column < painted.columns(); ++column) {
		const cell& one = painted.at(row, column);
		if (!one.glyph.is_continuation())
			out.append(pool.cluster_of(one.glyph));
	}
	return out;
}

} // namespace

// ===========================================================================
// The normal form (decoration.h)
// ===========================================================================

TEST(LeshperDecorationNormalize, NestedSpansResolveWithTheInnerOneWinning) {
	// The shape the highlighter actually emits: `paint_segments` emits the
	// double-quoted segment and then recurses INTO it for the `$x` inside, so
	// the two spans nest and the later one has to win over the middle of the
	// earlier one WITHOUT erasing its ends. No sort can answer this - the right
	// answer is three spans, none of which was emitted.
	const decorations marks = one_layer("highlighter", {span(0, 4, 1), span(1, 3, 2)});

	ASSERT_EQ(marks.spans().size(), 3u);
	EXPECT_EQ(marks.spans()[0], span(0, 1, 1));
	EXPECT_EQ(marks.spans()[1], span(1, 3, 2));
	EXPECT_EQ(marks.spans()[2], span(3, 4, 1));
}

TEST(LeshperDecorationNormalize, TheResultIsSortedAndDisjointWhateverTheEmissionOrder) {
	// The renderer walks these with a cursor that only moves forward
	// (layout.cpp's `span_pen`), so "sorted and disjoint" is not tidiness - it is
	// the precondition that makes the walk correct.
	const decorations marks =
		one_layer("r", {span(10, 14, 1), span(0, 4, 2), span(3, 12, 1)});

	std::size_t previous = 0;
	for (const decoration_span& one : marks.spans()) {
		EXPECT_LT(one.start, one.end);
		EXPECT_GE(one.start, previous);
		previous = one.end;
	}
	// [0,3) from the second, then [3,12) from the third and [12,14) from the
	// first - which are the same style abutting, and so come out as one span.
	ASSERT_EQ(marks.spans().size(), 2u);
	EXPECT_EQ(marks.spans()[0], span(0, 3, 2));
	EXPECT_EQ(marks.spans()[1], span(3, 14, 1));
}

TEST(LeshperDecorationNormalize, AbuttingRangesOfOneStyleAreOneSpan) {
	// A seam the renderer cannot see is a seam it should not be given: two
	// adjacent spans of one style would end a run and start another for no
	// reason a cell could show.
	const decorations marks = one_layer("r", {span(0, 3, 1), span(3, 6, 1)});
	ASSERT_EQ(marks.spans().size(), 1u);
	EXPECT_EQ(marks.spans().front(), span(0, 6, 1));
}

TEST(LeshperDecorationNormalize, AnEmptyOrUnstyledSpanOccludesNothing) {
	// Neither is dropped for tidiness. Letting either into the sweep would let it
	// OCCLUDE the span underneath it, which is a visible wrong answer - a
	// highlighted word going plain because a plugin emitted a zero-length
	// annotation on top of it.
	const decorations marks =
		one_layer("r", {span(0, 6, 1), span(2, 2, 2), span(3, 5, 0)});
	ASSERT_EQ(marks.spans().size(), 1u);
	EXPECT_EQ(marks.spans().front(), span(0, 6, 1));
}

TEST(LeshperDecorationNormalize, ALaterLayerPaintsOverAnEarlierOne) {
	// ADR-0008's namespacing does not mean isolation: two reactors may both have
	// something to say about one byte, and the one applied later is the one that
	// said it last.
	decorations marks;
	std::vector<decoration_span> first{span(0, 6, 1)};
	std::vector<virtual_text> none;
	marks.apply("highlighter", first, none);
	std::vector<decoration_span> second{span(2, 4, 2)};
	marks.apply("linter", second, none);

	ASSERT_EQ(marks.spans().size(), 3u);
	EXPECT_EQ(marks.spans()[1], span(2, 4, 2));
}

TEST(LeshperDecorationNormalize, AReactorsNewBatchReplacesItsOwnAndNobodyElses) {
	decorations marks;
	std::vector<virtual_text> none;
	std::vector<decoration_span> highlighter{span(0, 4, 1)};
	marks.apply("highlighter", highlighter, none);
	std::vector<decoration_span> suggester{span(8, 12, 2)};
	marks.apply("suggester", suggester, none);

	std::vector<decoration_span> again{span(0, 2, 2)};
	marks.apply("highlighter", again, none);

	ASSERT_EQ(marks.layers().size(), 2u);
	EXPECT_EQ(marks.layers()[0].reactor, "highlighter");
	EXPECT_EQ(marks.layers()[0].spans.size(), 1u);
	EXPECT_EQ(marks.layers()[0].spans.front(), span(0, 2, 2));
	// The other reactor's word is untouched, which is the whole of "the emitting
	// reactor is the decoration namespace".
	EXPECT_EQ(marks.layers()[1].spans.front(), span(8, 12, 2));
}

TEST(LeshperDecorationNormalize, ApplyHandsTheCallersVectorsBackWithTheirCapacity) {
	// #126's pooling, from the other end. The batch a caller hands us is pooled
	// storage and moving out of it would return an empty vector with no capacity,
	// defeating the pooling on the very path it was built for - so `apply` swaps
	// and the caller gets the previous batch's storage back to refill.
	decorations marks;
	std::vector<decoration_span> spans{span(0, 4, 1)};
	std::vector<virtual_text> texts{virtual_text{4, "abc", 1}};
	marks.apply("r", spans, texts);
	EXPECT_TRUE(spans.empty());

	std::vector<decoration_span> next{span(0, 2, 1)};
	next.reserve(64);
	marks.apply("r", next, texts);
	// What came back is the FIRST batch's vector, contents and all.
	ASSERT_EQ(next.size(), 1u);
	EXPECT_EQ(next.front(), span(0, 4, 1));
}

TEST(LeshperDecorationNormalize, ForgetDropsOneLayerAndClearDropsThemAll) {
	decorations marks;
	std::vector<virtual_text> none;
	std::vector<decoration_span> a{span(0, 4, 1)};
	std::vector<decoration_span> b{span(4, 8, 2)};
	marks.apply("a", a, none);
	marks.apply("b", b, none);

	EXPECT_FALSE(marks.forget("nobody"));
	EXPECT_TRUE(marks.forget("a"));
	ASSERT_EQ(marks.layers().size(), 1u);
	ASSERT_EQ(marks.spans().size(), 1u);
	EXPECT_EQ(marks.spans().front(), span(4, 8, 2));

	marks.clear();
	EXPECT_TRUE(marks.empty());
	EXPECT_TRUE(marks.layers().empty());
}

TEST(LeshperDecorationNormalize, VirtualTextsSortByOffsetAndTiesKeepEmissionOrder) {
	decorations marks;
	std::vector<decoration_span> none;
	std::vector<virtual_text> texts{virtual_text{9, "late", 0}, virtual_text{2, "first", 0},
	                                virtual_text{2, "second", 0}, virtual_text{2, "", 0}};
	marks.apply("r", none, texts);

	ASSERT_EQ(marks.texts().size(), 3u) << "an empty text draws nothing and is dropped";
	EXPECT_EQ(marks.texts()[0].bytes, "first");
	EXPECT_EQ(marks.texts()[1].bytes, "second");
	EXPECT_EQ(marks.texts()[2].bytes, "late");
}

// ===========================================================================
// The theme (theme.h)
// ===========================================================================

// EVERY NAME THE BUILT-IN REACTORS INTERN HAS A DEFAULT PEN is asserted in
// `ui_highlight_tests.cpp` (#168 Phase B), because the reactors that intern them
// are `lesh::ui`'s now. The assertion is the same one and is deliberately not
// restated over a copy of the list here - a copy is exactly what goes stale.

TEST(LeshperDecorationTheme, ThePensAreTruecolorAndNothingHereQuantizes) {
	// #97: the theme authors at full precision and `blit.cpp` owns the downmap,
	// exactly once. A table that pre-quantized would mean a theme is authored
	// once per terminal instead of once.
	for (const theme_entry& one : default_theme) {
		EXPECT_EQ(one.pen.fg.kind, color_kind::truecolor) << one.name;
		EXPECT_EQ(one.pen.bg.kind, color_kind::terminal_default)
			<< one.name << ": the default theme sets no backgrounds";
	}
}

TEST(LeshperDecorationTheme, AnIdNobodyThemedLeavesThePenExactlyAsItWas) {
	// The degradation that keeps a plugin's own vocabulary from turning text
	// black: `over` answers with what the caller was going to paint in.
	registry reg;
	std::uint32_t mine = 0;
	ASSERT_EQ(lesh_style_intern(&reg, "rainbow.sparkle", &mine), LESH_OK);
	style_table table;
	table.sync(reg.styles);

	EXPECT_FALSE(table.knows(mine));
	EXPECT_EQ(table.over(mine, BLUE), BLUE);
	// And LESH_STYLE_NONE is never a style, whatever else is in the table.
	EXPECT_FALSE(table.knows(0));
	EXPECT_EQ(table.over(0, BLUE), BLUE);
	// An id past the end - a batch from a registry that has since been rebuilt -
	// is the same answer rather than a read past the vector.
	EXPECT_EQ(table.over(9999, BLUE), BLUE);
}

TEST(LeshperDecorationTheme, SyncIsIncrementalAndABindingSurvivesIt) {
	registry reg;
	std::uint32_t first = 0;
	ASSERT_EQ(lesh_style_intern(&reg, "keyword", &first), LESH_OK);
	style_table table;
	table.sync(reg.styles);
	const style themed = table.over(first, style{});

	std::uint32_t second = 0;
	ASSERT_EQ(lesh_style_intern(&reg, "comment", &second), LESH_OK);
	table.sync(reg.styles);

	EXPECT_EQ(table.over(first, style{}), themed) << "the second sync re-decided the first id";
	EXPECT_TRUE(table.knows(second));
}

// ===========================================================================
// The paint (layout.cpp)
// ===========================================================================

TEST(LeshperDecorationLayout, ASpanPaintsItsClustersInTheThemesPen) {
	cluster_pool pool;
	const decorations marks = one_layer("highlighter", {span(0, 2, 1)});
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(10, 4);
	in.buffer = "ab cd";
	in.cursor = at(5);
	in.marks = &marks;
	in.theme = &table;

	const layout made = lay_out(pool, in);
	const std::vector<color> pens = pens_of(made.screen, 0);
	EXPECT_EQ(pens[0], RED.fg);
	EXPECT_EQ(pens[1], RED.fg);
	EXPECT_EQ(pens[2], color::of_default()) << "the span ends where it says it ends";
	EXPECT_EQ(glyphs_of(pool, made.screen, 0).substr(0, 5), "ab cd");
}

TEST(LeshperDecorationLayout, TwoSpansPaintTwoRunsAndTheGapBetweenThemIsTheTextPen) {
	cluster_pool pool;
	const decorations marks = one_layer("h", {span(0, 2, 1), span(3, 5, 2)});
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(10, 4);
	in.buffer = "ab cd";
	in.cursor = at(5);
	in.marks = &marks;
	in.theme = &table;

	const std::vector<color> pens = pens_of(lay_out(pool, in).screen, 0);
	EXPECT_EQ(pens[0], RED.fg);
	EXPECT_EQ(pens[2], color::of_default());
	EXPECT_EQ(pens[3], BLUE.fg);
	EXPECT_EQ(pens[4], BLUE.fg);
}

TEST(LeshperDecorationLayout, DecorationsChangeNoGeometryAtAll) {
	// THE WIDTH INVARIANT, on this side of it (#131, layout.h). A pen is not a
	// width: what a span changes is the colour of cells that were going to be
	// painted anyway, so the row count, the cursor and the glyphs must all be
	// bit-for-bit what the undecorated picture was.
	cluster_pool pool;
	const decorations marks =
		one_layer("h", {span(0, 3, 1), span(4, 9, 2), span(11, 13, 1)});
	const style_table table = two_pens(RED, BLUE);

	layout_input plain = sized(8, 6);
	plain.buffer = "echo one two three";
	plain.cursor = at(9);
	layout_input decorated = plain;
	decorated.marks = &marks;
	decorated.theme = &table;

	const layout without = lay_out(pool, plain);
	const layout with = lay_out(pool, decorated);

	EXPECT_EQ(with.content_rows, without.content_rows);
	EXPECT_EQ(with.cursor_row, without.cursor_row);
	EXPECT_EQ(with.cursor_column, without.cursor_column);
	EXPECT_EQ(with.screen.rows(), without.screen.rows());
	for (std::uint16_t row = 0; row < with.screen.rows(); ++row)
		EXPECT_EQ(glyphs_of(pool, with.screen, row), glyphs_of(pool, without.screen, row));
}

TEST(LeshperDecorationLayout, SpansWithNoThemeAtAllPaintTheUndecoratedPicture) {
	// `input_for` produces exactly this before the loop attaches a registry, and
	// it must be a legible line rather than a blank or a black one.
	cluster_pool pool;
	const decorations marks = one_layer("h", {span(0, 4, 1)});

	layout_input in = sized(10, 4);
	in.buffer = "echo";
	in.cursor = at(4);
	layout_input themeless = in;
	themeless.marks = &marks;   // spans, no theme

	EXPECT_TRUE(lay_out(pool, themeless).screen == lay_out(pool, in).screen);
}

TEST(LeshperDecorationLayout, ASpanSurvivesASoftWrapAndSplitsNoCluster) {
	cluster_pool pool;
	// Two-column clusters, and a width that makes the third one straddle the
	// edge: the wrap rule (layout.h decision 1) and the span have to agree.
	const decorations marks = one_layer("h", {span(0, 9, 1)});
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(5, 4);
	in.buffer = "\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD";  // three U+4E2D
	in.cursor = at(9);
	in.marks = &marks;
	in.theme = &table;

	const layout made = lay_out(pool, in);
	ASSERT_EQ(made.content_rows, 2u);
	// Four columns of the first row, then the third cluster whole on the next.
	EXPECT_EQ(made.screen.at(0, 4).glyph.is_continuation(), false);
	EXPECT_EQ(pens_of(made.screen, 0)[0], RED.fg);
	EXPECT_EQ(pens_of(made.screen, 0)[3], RED.fg);
	EXPECT_EQ(pens_of(made.screen, 1)[0], RED.fg) << "the span continues across the wrap";
	EXPECT_EQ(pens_of(made.screen, 1)[1], RED.fg) << "and covers the cluster's second column";
}

TEST(LeshperDecorationLayout, ASpanPastTheEndOfTheBufferPaintsNothing) {
	// The live case for it: a batch computed against a longer line, still applied
	// while the newer one is in flight. It must not reach past the text.
	cluster_pool pool;
	const decorations marks = one_layer("h", {span(20, 40, 1)});
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(10, 4);
	in.buffer = "ab";
	in.cursor = at(2);
	in.marks = &marks;
	in.theme = &table;

	for (const color& pen : pens_of(lay_out(pool, in).screen, 0))
		EXPECT_EQ(pen, color::of_default());
}

// --- Virtual text -----------------------------------------------------------

TEST(LeshperDecorationLayout, VirtualTextAtTheEndOfTheLineIsDrawnAndTheCursorIsNotInIt) {
	// #133's live case, and the one the whole feature is for: the autosuggester
	// emits the rest of a history line at the end of what was typed. The
	// suggestion is on the screen; the cursor sits where the next keystroke will
	// land, which is where the TYPED text ends.
	cluster_pool pool;
	decorations marks;
	std::vector<decoration_span> none;
	std::vector<virtual_text> texts{virtual_text{4, " hello", 2}};
	marks.apply("suggester", none, texts);
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(20, 4);
	in.buffer = "echo";
	in.cursor = at(4);
	in.marks = &marks;
	in.theme = &table;

	const layout made = lay_out(pool, in);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0).substr(0, 10), "echo hello");
	EXPECT_EQ(made.cursor_column, 4u) << "the cursor is after the typed text, not after the suggestion";
	EXPECT_EQ(made.cursor_row, 0u);
	EXPECT_EQ(made.screen.cursor().column, 4u);

	const std::vector<color> pens = pens_of(made.screen, 0);
	EXPECT_EQ(pens[3], color::of_default()) << "the typed text is not the suggestion's colour";
	EXPECT_EQ(pens[4], BLUE.fg);
	EXPECT_EQ(pens[9], BLUE.fg);
}

TEST(LeshperDecorationLayout, VirtualTextInTheMiddleOfTheLinePushesTheTextAfterItAlong) {
	cluster_pool pool;
	decorations marks;
	std::vector<decoration_span> none;
	std::vector<virtual_text> texts{virtual_text{2, "XY", 1}};
	marks.apply("r", none, texts);
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(20, 4);
	in.buffer = "abcd";
	in.cursor = at(4);
	in.marks = &marks;
	in.theme = &table;

	const layout made = lay_out(pool, in);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0).substr(0, 6), "abXYcd");
	EXPECT_EQ(pens_of(made.screen, 0)[2], RED.fg);
	EXPECT_EQ(pens_of(made.screen, 0)[4], color::of_default());
	// The cursor is a BUFFER offset: four typed bytes, two virtual columns
	// before them, so it is at column six.
	EXPECT_EQ(made.cursor_column, 6u);
}

TEST(LeshperDecorationLayout, TheCursorLandsBeforeVirtualTextAtItsOwnOffset) {
	// The rule (layout.h decision 7): an offset is a place in the BUFFER, so the
	// cursor is placed where the buffer's clusters put it and what is virtual
	// there is drawn after. Here the cursor and the virtual text are at the same
	// offset in the MIDDLE of the line, which is the case that distinguishes the
	// rule from "the cursor goes wherever the walk had got to".
	cluster_pool pool;
	decorations marks;
	std::vector<decoration_span> none;
	std::vector<virtual_text> texts{virtual_text{2, "XY", 1}};
	marks.apply("r", none, texts);
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(20, 4);
	in.buffer = "abcd";
	in.cursor = at(2);
	in.marks = &marks;
	in.theme = &table;

	const layout made = lay_out(pool, in);
	EXPECT_EQ(made.cursor_column, 2u);
	EXPECT_TRUE(made.screen.cursor().visible);
}

TEST(LeshperDecorationLayout, VirtualTextWrapsAtTheEdgeAndSplitsNoCluster) {
	cluster_pool pool;
	decorations marks;
	std::vector<decoration_span> none;
	// Two two-column clusters of virtual text, at a width that leaves one column.
	std::vector<virtual_text> texts{
		virtual_text{3, "\xE4\xB8\xAD\xE4\xB8\xAD", 2}};
	marks.apply("r", none, texts);
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(4, 4);
	in.buffer = "abc";
	in.cursor = at(3);
	in.marks = &marks;
	in.theme = &table;

	const layout made = lay_out(pool, in);
	ASSERT_EQ(made.content_rows, 2u);
	// The first virtual cluster does not fit in the one remaining column, so it
	// starts the next row whole - the same rule the buffer's clusters get.
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), "abc ");
	EXPECT_EQ(glyphs_of(pool, made.screen, 1), "\xE4\xB8\xAD\xE4\xB8\xAD");
	EXPECT_EQ(made.cursor_row, 0u);
	EXPECT_EQ(made.cursor_column, 3u);
}

TEST(LeshperDecorationLayout, VirtualTextAtTheEndOfAFullRowPutsTheCursorOnTheNextRow) {
	// Decision 3 and decision 7 meeting: the buffer ends exactly at the right
	// edge, so the cursor cannot be in the phantom column, and the suggestion
	// starts on the row it moved to.
	cluster_pool pool;
	decorations marks;
	std::vector<decoration_span> none;
	std::vector<virtual_text> texts{virtual_text{4, "zz", 2}};
	marks.apply("r", none, texts);
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(4, 4);
	in.buffer = "abcd";
	in.cursor = at(4);
	in.marks = &marks;
	in.theme = &table;

	const layout made = lay_out(pool, in);
	ASSERT_EQ(made.content_rows, 2u);
	EXPECT_EQ(made.cursor_row, 1u);
	EXPECT_EQ(made.cursor_column, 0u);
	EXPECT_EQ(glyphs_of(pool, made.screen, 1).substr(0, 2), "zz");
}

TEST(LeshperDecorationLayout, AVirtualTextPastTheEndOfTheBufferIsNotDrawn) {
	cluster_pool pool;
	decorations marks;
	std::vector<decoration_span> none;
	std::vector<virtual_text> texts{virtual_text{40, "stale", 2}};
	marks.apply("r", none, texts);
	const style_table table = two_pens(RED, BLUE);

	layout_input in = sized(10, 4);
	in.buffer = "ab";
	in.cursor = at(2);
	in.marks = &marks;
	in.theme = &table;

	const layout made = lay_out(pool, in);
	EXPECT_EQ(glyphs_of(pool, made.screen, 0), "ab        ");
	EXPECT_EQ(made.cursor_column, 2u);
}

TEST(LeshperDecorationLayout, VirtualBytesAreBufferBytesAndAnEscapeInThemPaintsNothing) {
	// layout.h decision 7's third rule. A reactor styles what it emits with a
	// style id; a second door made of literal escapes would make a reactor's
	// output measure differently from how it paints, which is exactly the
	// `%{ %}` folklore #114 exists to prevent.
	cluster_pool pool;
	decorations marks;
	std::vector<decoration_span> none;
	std::vector<virtual_text> texts{virtual_text{2, "\x1B[31mQ", 0}};
	marks.apply("r", none, texts);

	layout_input in = sized(10, 4);
	in.buffer = "ab";
	in.cursor = at(2);
	in.marks = &marks;

	const layout made = lay_out(pool, in);
	// Exactly what the same bytes typed into the buffer would do: the ESC is a
	// control the surface drops, and the `[31m` after it are ordinary printable
	// characters that get printed. Not a pen, and not a hole either.
	EXPECT_EQ(glyphs_of(pool, made.screen, 0).substr(0, 7), "ab[31mQ");
	EXPECT_EQ(pens_of(made.screen, 0)[2], color::of_default())
		<< "the escape was a control the surface dropped, not a pen";
}

// ===========================================================================
// The replay compare (state.h)
// ===========================================================================

TEST(LeshperDecorationReplay, TwoStatesDifferingOnlyInTheirDecorationsAreEqual) {
	// The decision, as the test that would fail if somebody put `marks` back into
	// `state::operator==`. N-3 replays a recorded EVENT SEQUENCE and demands an
	// equal state; decorations are what a reactor computed off-thread against
	// $PATH and the filesystem, and whether the answer had landed by the last
	// event is scheduling. Comparing them would make the guarantee a race.
	state first;
	state second;
	apply_edit(first, position{}, position{}, "echo hi");
	apply_edit(second, position{}, position{}, "echo hi");
	ASSERT_TRUE(first == second);

	std::vector<decoration_span> spans{span(0, 4, 1)};
	std::vector<virtual_text> texts{virtual_text{7, " there", 2}};
	first.marks.apply("highlighter", spans, texts);

	EXPECT_TRUE(first == second) << "decorations are derived, not typed input";
	// And they are still comparable in their own right, which is how a test that
	// means to assert on them says so.
	EXPECT_FALSE(first.marks == second.marks);
}

TEST(LeshperDecorationReplay, ACopiedStateCarriesItsDecorationsWithIt) {
	// Out of the compare is not out of the value: a state is copied on every
	// undo boundary and by every test that snapshots one, and decorations that
	// did not travel would be a picture that changed when nothing did.
	state original;
	std::vector<decoration_span> spans{span(0, 4, 1)};
	std::vector<virtual_text> texts;
	original.marks.apply("h", spans, texts);

	const state copied = original;
	ASSERT_EQ(copied.marks.spans().size(), 1u);
	EXPECT_EQ(copied.marks.spans().front(), span(0, 4, 1));
	EXPECT_TRUE(copied.marks == original.marks);
}
