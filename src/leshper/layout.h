#pragma once

#include "leshper/event.h"
#include "leshper/pager.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "leshper/text.h"
#include "leshper/theme.h"
#include "substrate/grapheme.h"

#include <cstdint>
#include <string_view>

namespace lesh::leshper {

// Layout: a pure function from what is being edited to a surface (F-37, F-38).
//
// #112 built the ring below this one and stopped exactly here: `write` clips at
// the right edge and never wraps, and its comment says where a line breaks is
// layout's. This is that. Nothing in this file opens a terminal, holds a
// buffer between calls, or remembers where it scrolled to last time - the
// whole point of F-37's layout-as-value is that a test asserts a cell grid and
// the loop repaints by diffing two values (N-3).
//
// STATELESS IS THE DESIGN, not an accident of a first cut. Everything the
// rendered picture depends on is an argument: the prompt bytes, the buffer, the
// cursor, the decorations, the size. Two calls with equal inputs produce equal
// layouts, and that is what makes F-38 nearly free - a resize is the same
// function at the new width, and there is no carried scroll offset or wrapped
// line cache to reflow, because there is nothing carried at all.
//
// WHAT LAYOUT DECIDES, and the arguments (this ticket's four):
//
//   1. SOFT WRAP. A line longer than the columns continues at column 0 of the
//      next row, and a cluster is never split across the edge: a two-column
//      cluster that would straddle the right edge leaves that column blank and
//      starts the next row. The blitter cannot express half a glyph and #97's
//      floor has no way to ask for one, so the alternative was not "split it",
//      it was "clip it", and clipping in the middle of an edit line loses text
//      the user typed. A soft-wrapped continuation gets NO secondary prompt: it
//      is not a new logical line, and F-36 is about logical lines.
//
//   2. MULTI-LINE BUFFERS (F-2). A line-break cluster in the buffer - U+000A,
//      or a CR LF pair, which UAX #29 makes one cluster - ends the logical line
//      and the next one starts at column 0 with `continuation` drawn at its
//      head. The buffer is always rendered whole; nothing here elides a line.
//
//   3. THE CURSOR IS ALWAYS ON A CELL THE TERMINAL CAN REACH. An empty buffer
//      puts it immediately after the prompt, which is the general rule ("one
//      past the last cluster") applied to zero clusters rather than a special
//      case. When the position one past the last glyph is the phantom column -
//      the buffer ends exactly at the right edge - the cursor moves to column 0
//      of the next row. It has to: #112's blitter moves relatively and starts
//      every move with `\r`, which CANCELS the terminal's pending-wrap state, so
//      a cursor "in" the phantom column is not merely awkward, it is
//      unrepresentable in the bytes the blitter can emit.
//
//      The one ambiguity this leaves is real and is accepted: when a logical
//      line is an exact multiple of the width, the end of that line and the
//      start of the next are the same cell. Every terminal has this ambiguity;
//      the alternatives were an invisible cursor or a blank row the user never
//      typed.
//
//   4. THE VIEW SCROLLS, THE TERMINAL NEVER DOES. When the content needs more
//      rows than the terminal has, the surface is the terminal's height and
//      holds a WINDOW of the content, centred on the cursor's row and clamped
//      to the content's ends. The loop owns the terminal (#98) and F-39 has
//      shell output scrolling above the edit line, so a renderer that scrolled
//      the terminal itself would be scrolling away output it does not own.
//
//      Centred, and derived rather than remembered, because a remembered scroll
//      offset is state - it would have to be reflowed on resize, restored on
//      replay, and compared in `operator==`, which is the whole argument
//      against it. Centring is the derivation that moves the window by at most
//      one row per one-row cursor move, so it reads as scrolling rather than as
//      jumping, and it always shows context on both sides of the cursor.
//
//   5. A PROMPT'S SGR IS A PEN, NOT A DECORATION (#131). Escape sequences in
//      prompt bytes are recognized by #114's measurer and contribute no width
//      and no cells - and an SGR among them now moves the pen the clusters
//      AFTER it are painted with, through `sgr.h`, so `ESC[32m$ ESC[0m` renders
//      as the theme author wrote it instead of as a plain `$`. Every other form
//      - OSC, SS3, a CSI that is not SGR - is skipped exactly as before.
//
//      THE WIDTH INVARIANT SURVIVES IT and is the reason the pen rides the same
//      recognition point rather than a second scan: what this file paints for a
//      prompt is still exactly `grapheme::display_width(prompt)`, a debug
//      assertion in the walk and a test at every shape. The pen does not
//      outlive the text it belongs to - the prompt's colours never bleed into
//      the buffer, which starts from `text_pen` again, and a continuation
//      prompt starts from `prompt_pen` on every logical line.
//
//   6. THE PAGER GOES BELOW, AND TAKES ITS ROWS FIRST (#138, F-28). When one is
//      open, `pager.h` measures and renders it into a surface OF ITS OWN (A-6),
//      and this file COPIES that surface into the rows under the edit line. The
//      pager's rows come off the terminal's height before the edit view is
//      windowed, so the edit line is never pushed off the screen by a list of
//      candidates - `pager_row_budget` never claims the last row.
//
//      Copied rather than painted straight in, and that is the A-6 decision
//      rather than an inefficiency: the pager's coordinates are the pager's, so
//      a test asserts a grid with no prompt in it, the day the §8 surface API
//      exists this is already the object it hands out, and nothing about the
//      pager's picture can depend on what the prompt happened to be.
//
//   7. THE DECORATIONS ARE PAINTED (#141). A span over buffer bytes moves the
//      pen for the clusters inside it, through `theme.h`'s id-to-pen table, and
//      the virtual text #133's autosuggester emits is drawn at its offset as
//      cells that are not in the buffer.
//
//      Three rules, and each of them is a decision:
//
//      THE SPANS ARRIVE NORMALIZED. `decoration.h` flattens the layers into one
//      sorted, non-overlapping list at application time, so the walk answers
//      "what pen is this byte" with a cursor that only moves forward. That is
//      what keeps this function allocation-free with highlighting in it - the
//      alternative, resolving nested spans per cluster, is a scan per cluster
//      and a scratch structure per call.
//
//      THE CURSOR GOES DOWN BEFORE THE VIRTUAL TEXT AT ITS OFFSET. Decision 3
//      says the cursor is always on a cell the terminal can reach, and virtual
//      text must not be able to push it: an offset is a place in the BUFFER, so
//      the cursor is placed where the buffer's clusters put it and the virtual
//      text is drawn after. At the end of the line - the live case, an
//      autosuggestion - that puts the cursor exactly where the user's next
//      keystroke will land, with the muted rest of the suggestion trailing to
//      its right, which is what fish does and what makes the suggestion
//      readable as a suggestion.
//
//      VIRTUAL BYTES ARE PAINTED LIKE BUFFER BYTES, not like prompt bytes: an
//      ESC in them is a control the surface drops rather than a pen change.
//      A reactor styles what it emits with a style id (#133's `_styled` emit),
//      which is the door that exists; a second door made of literal escapes
//      would make a reactor's output measure differently from how it paints.
//
// WHAT IT DOES NOT DECIDE. The right prompt (F-40 declares widths first), and
// prompt expansion (#94 left it lesh-side and undecided). The mapping from a
// theme's NAMES to colours is no longer on this list - #141 put it in `theme.h`
// and decision 7 reads it - but it is still a different path from `sgr.h`,
// which reads the literal colours a prompt's own bytes carry.

// Everything the picture depends on. Inputs only - no output parameters, no
// handles, nothing this function may mutate.
struct layout_input {
	// The left prompt (F-40), opaque bytes from a `Prompt` provider (#94). May
	// carry SGR/OSC/SS3, which is measured and skipped, never painted (#114) -
	// and an SGR among them sets the pen of the cells that follow (#131).
	// A U+000A in it starts a new row at column 0: multi-line prompts are
	// ordinary here rather than a feature.
	std::string_view prompt;

	// The secondary prompt (F-36, F-40), drawn at the head of every logical
	// line after the first. Empty - the default - means none, and is what a
	// caller that has not decided one yet should pass.
	std::string_view continuation;

	// What is being edited, and where the cursor is in it (A-1, F-2).
	std::string_view buffer;
	position cursor;

	// #93's annotations (A-7), and #141 is the ticket that filled the type in.
	// Borrowed and never freed here (ADR-0007), and read exactly as `spans()`
	// and `texts()` present them: sorted, and in the spans' case disjoint.
	// Null - the default - is a picture with no highlighting in it, which is
	// what every test that is about geometry passes.
	const decorations* marks = nullptr;

	// What a span's interned semantic id LOOKS like (#124, theme.h). Borrowed,
	// never owned, and SEPARATE from `marks` because they answer different
	// questions and change on different clocks: the marks change on every
	// keystroke, the table only when a binding interns a name.
	//
	// Null - the default - means the spans are known but unthemed, and the
	// picture is the undecorated one. That is not a degenerate case to be
	// tolerated; it is what `input_for` produces before the loop attaches a
	// registry, and it must be a legible line rather than a blank one.
	const style_table* theme = nullptr;

	// The terminal, as the last resize event reported it (A-3). Zero in either
	// axis means the loop has not asked yet, and the answer is an empty layout:
	// there is no honest picture of a screen whose size is unknown.
	std::uint16_t columns = 0;
	std::uint16_t rows = 0;

	// The pens. Two, not a theme: theming is configuration (#101) and no
	// ticket has decided it. Defaults are the terminal's own colours, which is
	// what a shell prompt looks like before anyone styles it.
	//
	// `prompt_pen` is where the prompt STARTS, not where every span of it
	// stays: its own SGR takes it from there, and an `ESC[0m` in it means the
	// terminal's default - which is what the same bytes mean coming out of the
	// blitter, and a reader that meant something else would be a dialect.
	style prompt_pen{};
	style text_pen{};

	// The pager (#138, F-28), composited below the edit line. Borrowed and
	// never freed here (ADR-0007). Null - the default - is no pager, and so is a
	// pager that is not showing, which is why every call site that predates this
	// field is unchanged and why `input_for` can hand the state's own along
	// unconditionally.
	const pager_state* pager = nullptr;

	// Its two pens, beside the other two and for the same reason: theming is
	// configuration (#101) and no ticket has decided it.
	pager_pens pager_pen{};

	// #108's width seam, passed through to every measurement and to the
	// surface, so one policy answers for the layout and the cells alike.
	grapheme::width_policy width{};
};

// The state's own half of the input, so the loop does not restate it and cannot
// restate it wrongly. The prompt is not in the state - it comes from a provider
// (#94) - so it is the caller's to supply.
[[nodiscard]] inline layout_input input_for(const state& s, std::string_view prompt,
                                            std::string_view continuation = {}) noexcept {
	layout_input in;
	in.prompt = prompt;
	in.continuation = continuation;
	in.buffer = s.buffer.text();
	in.cursor = s.cursor;
	in.marks = &s.marks;
	in.pager = &s.pager;
	in.columns = s.columns;
	in.rows = s.rows;
	return in;
}

// A laid-out picture: the surface the blitter paints, and the geometry that
// produced it.
//
// The geometry is beside the surface rather than inside it because #112's
// surface is deliberately not a layout engine - it is a grid of cells, and a
// content row count means nothing to the differ. It is here because a caller
// that wants to know whether it is looking at all of the buffer should not have
// to lay it out again to find out.
struct layout {
	// Exactly what goes on screen: `columns` wide, and as tall as the content
	// needs up to the terminal's height. NOT the full terminal when the content
	// is shorter - the surface's origin is wherever the terminal's cursor
	// happened to be (F-39 scrolls output above it), so claiming rows the edit
	// line does not use would blank output that is not ours.
	surface screen;

	// Rows the content needs at this width, before the view is applied.
	std::uint16_t content_rows = 0;

	// The first content row `screen` shows. Zero unless the content is taller
	// than the terminal.
	std::uint16_t first_visible_row = 0;

	// Where the cursor is in CONTENT coordinates. `screen.cursor()` carries the
	// same place in screen coordinates, which is what the blitter uses; this is
	// what a caller reasoning about the buffer wants.
	std::uint16_t cursor_row = 0;
	std::uint16_t cursor_column = 0;

	// Rows at the BOTTOM of `screen` that belong to the pager rather than to
	// the edit line (#138). Zero when no pager is showing, which is what keeps
	// every reader that predates it correct.
	std::uint16_t pager_rows = 0;

	// Rows of `screen` the edit line has: everything the pager did not take.
	[[nodiscard]] std::uint16_t edit_rows() const noexcept {
		return static_cast<std::uint16_t>(screen.rows() - pager_rows);
	}

	// True when the view is a window rather than the whole content.
	[[nodiscard]] bool scrolled() const noexcept { return content_rows > edit_rows(); }

	friend bool operator==(const layout&, const layout&) noexcept = default;
};

// The function. Interns into `pool` - the one pool both surfaces of a diff
// share (#112) - and touches nothing else.
[[nodiscard]] layout lay_out(cluster_pool& pool, const layout_input& in);

// F-38, and it is deliberately this thin: a resize is the same content laid out
// at the new size, and there is nothing to reflow because there was nothing
// carried. The caller then PAINTS rather than updates - #112's `update` refuses
// two surfaces of different sizes, because diffing two grids of different
// shapes would be inventing half an answer.
[[nodiscard]] layout reflow(cluster_pool& pool, layout_input in, const resize_event& to);

// Whether `update(previous, desired)` is meaningful between these two, or the
// caller owes a `paint`. F-37's "full repaint only on resize or explicit
// request", asked as a question rather than remembered as a flag.
[[nodiscard]] inline bool can_diff(const layout& previous, const layout& desired) noexcept {
	return previous.screen.columns() == desired.screen.columns()
	    && previous.screen.rows() == desired.screen.rows();
}

} // namespace lesh::leshper
