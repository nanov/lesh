#include "leshper/layout.h"

#include "leshper/sgr.h"
#include "substrate/assert.h"
#include "substrate/measure.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace lesh::leshper {

namespace {

// A cluster that ends a logical line: U+000A on its own, or the CR LF pair that
// UAX #29's GB3 keeps together as ONE cluster - which is why this asks the
// segmenter rather than scanning for a byte. A lone CR is not a line break; it
// is a control, and the surface drops it like any other.
bool is_line_break(std::string_view cluster) {
	const grapheme::decoded first = grapheme::decode(cluster, 0);
	if (first.length == 0)
		return false;
	switch (grapheme::lookup(first.cp).cluster_break) {
		case grapheme::gcb::lf:
			return true;
		case grapheme::gcb::cr:
			return cluster.size() > first.length;  // CR LF, and nothing else
		default:
			return false;
	}
}

// The largest row the walk may reach. Past it content is truncated rather than
// allowed to wrap a uint16: a five-million-column paste is the only way here,
// and a short picture beats an overflowed one (N-4).
constexpr std::uint16_t last_row = 0xFFFEu;

// "This text has no cursor in it" - the prompts.
constexpr std::size_t no_cursor = static_cast<std::size_t>(-1);

// What one walk answers.
struct geometry {
	std::uint16_t content_rows = 1;
	std::uint16_t cursor_row = 0;
	std::uint16_t cursor_column = 0;
};

// The one traversal, run twice.
//
// Pass one counts rows and finds the cursor; only then can the view window be
// derived, because it depends on both. Pass two paints the rows the window
// covers. Two passes rather than a content-sized scratch grid: the walk is
// O(bytes) and allocates nothing, where a grid would be an allocation
// proportional to a pasted buffer on every keystroke (N-2).
//
// It emits RUNS, not clusters: the longest stretch of bytes that lands on one
// row with one pen, handed to `surface::write` in a single call. That is not an
// optimisation, it is how the two sides are kept from disagreeing - write's
// rules for a zero-width cluster joining the cell before it, and for a control
// that paints nothing, hold within a call and would be lost if this file fed it
// one cluster at a time and re-derived them here.
template <typename Emit>
class walker {
public:
	walker(const layout_input& in, Emit emit) : _in(in), _emit(std::move(emit)) {}

	geometry run() {
		place(_in.prompt, _in.prompt_pen, escapes::skip, no_cursor, secondary::no,
		      annotate::no);

		// THE WIDTH INVARIANT, checked rather than only tested. #131 made a
		// prompt's SGR set a pen instead of being merely skipped, and the thing
		// that must survive that is #114's contract: what this paints for a
		// prompt is exactly what `display_width` measures for it, so a theme
		// author never does width arithmetic and `%{ %}` folklore never exists.
		//
		// Single-row prompts only, and that is not a weakening. A prompt that
		// broke a line or wrapped has its columns spread over rows and no single
		// number to compare against; `_column == _in.columns` is pending wrap,
		// where `advance` has clamped and the column stopped being a sum. Both
		// are excluded by the guard rather than by a flag, because both are
		// exactly "the walk left row 0's column arithmetic".
		LESH_ASSERT(_row != 0 || _column >= _in.columns
		            || _column == grapheme::display_width(_in.prompt, _in.width));

		// The one place this file reads a byte offset out of a `position`
		// (text.h: every such call is a site #88 must revisit). It is a
		// comparison against buffer offsets and never arithmetic on them.
		place(_in.buffer, _in.text_pen, escapes::paint, _in.cursor.byte_offset(),
		      secondary::yes, annotate::yes);
		flush();

		// A cursor past the last cluster - the empty buffer, and every buffer
		// whose cursor is at its end - lands here, on the general rule rather
		// than on a special case.
		if (!_cursor_placed)
			record_cursor();

		geometry answer;
		answer.content_rows = static_cast<std::uint16_t>(_max_row + 1);
		answer.cursor_row = _cursor_row;
		answer.cursor_column = _cursor_column;
		return answer;
	}

private:
	enum class escapes { skip, paint };
	enum class secondary { no, yes };
	// Whether this text is the BUFFER, and therefore the text the decorations
	// are anchored to. The prompt, the continuation prompt and a virtual text's
	// own bytes are all `no`: a span's offsets are buffer offsets and mean
	// nothing anywhere else, and a virtual text that could carry virtual text
	// would be a recursion with no bottom.
	enum class annotate { no, yes };

	// `pen` BY VALUE, and that is the whole of #131 on this side: an SGR in
	// prompt bytes moves it, and it moves for the clusters that FOLLOW. One
	// copy per call, eleven bytes, and no pen state outlives the text it
	// belongs to - the prompt's colours cannot bleed into the buffer, because
	// the buffer's `place` starts from `text_pen` again.
	void place(std::string_view text, style pen, escapes on_escape,
	           std::size_t cursor_at, secondary continuations, annotate marks) {
		std::size_t i = 0;
		while (i < text.size()) {
			// Prompt bytes only. #114's measurer decides what an escape IS, and
			// asking it here is what makes the width this paints equal to the
			// width it measures. Buffer bytes are what the user typed: an ESC
			// there is a control cluster and the surface drops it.
			if (on_escape == escapes::skip
			    && static_cast<unsigned char>(text[i]) == 0x1B) {
				const std::size_t length = grapheme::measure_detail::escape_length(text, i);
				if (length != 0) {
					flush();  // the escape's bytes are not paintable text,
					          // and a pen change belongs to what comes AFTER
					pen = apply_sgr(text.substr(i, length), pen);
					i += length;
					continue;
				}
			}

			// What happens AT an offset, before the cluster that starts
			// there: the cursor, and then any virtual text drawn here. The
			// order is the rule - see layout.h, decision 6.
			if (marks == annotate::yes)
				at_offset(i, cursor_at);

			const std::size_t end = grapheme::next_boundary(text, i);
			const std::string_view cluster = text.substr(i, end - i);
			const bool ends_the_line = is_line_break(cluster);

			// Room first, THEN the cursor: a cluster that wraps takes the
			// cursor with it, and recording before the wrap would leave the
			// cursor a row above the character it is on.
			const int width =
				ends_the_line ? 0 : grapheme::cluster_width(cluster, _in.width);
			if (!ends_the_line)
				reserve(width);

			// The cursor sits on the cluster that CONTAINS its offset, so an
			// offset handed to us mid-cluster lands on a cell rather than
			// between two.
			if (cursor_at < end && !_cursor_placed)
				record_cursor();

			if (ends_the_line) {
				break_line();
				if (continuations == secondary::yes)
					place(_in.continuation, _in.prompt_pen, escapes::skip, no_cursor,
					      secondary::no, annotate::no);
				i = end;
				continue;
			}

			// #141: a span over these bytes moves the pen, and a run belongs to
			// ONE pen - so a span's edge ends the run in flight exactly as an
			// SGR in prompt bytes does.
			const style ink = marks == annotate::yes ? span_pen(i, pen) : pen;
			if (_run_open && !(_run_pen == ink))
				flush();

			open_run(text, i, ink);
			_run_end = end;
			advance(width);
			i = end;
		}
		// The end of the buffer is an offset like any other, and it is the one
		// an autosuggestion is emitted at (#133): the cursor lands after what
		// was typed, and the suggestion is drawn from there.
		if (marks == annotate::yes)
			at_offset(text.size(), cursor_at);
		// A run belongs to one source view and one pen, and the next `place`
		// brings both.
		flush();
	}

	// The pen a buffer byte is painted in: the text pen, unless a span covers
	// the byte and the theme knows what its interned id means.
	//
	// ONE CURSOR, MOVING FORWARD ONLY, which is what `decoration.h`'s normal
	// form buys: the spans are sorted and disjoint, `place` visits buffer
	// offsets in increasing order, and no other `place` on the stack touches
	// this cursor because only the buffer is annotated. So the whole of
	// highlighting costs one comparison per cluster and allocates nothing.
	[[nodiscard]] style span_pen(std::size_t offset, style under) {
		if (_in.marks == nullptr || _in.theme == nullptr)
			return under;
		const std::vector<decoration_span>& spans = _in.marks->spans();
		while (_span_at < spans.size() && spans[_span_at].end <= offset)
			++_span_at;
		if (_span_at >= spans.size() || spans[_span_at].start > offset)
			return under;
		return _in.theme->over(spans[_span_at].style_id, under);
	}

	// The two things that happen at a buffer offset before its cluster does.
	//
	// It returns immediately unless a virtual text is due here, and that early
	// return is load bearing: the cursor is ordinarily recorded further down,
	// AFTER `reserve` has wrapped for the cluster it sits on, because a cluster
	// that wraps takes the cursor with it. Recording it up here would move the
	// cursor a row too early - so it is only done up here when there is virtual
	// text at this offset to keep it away from, and then the column it is
	// recorded at is the one right after the real text, which is where a buffer
	// offset is.
	void at_offset(std::size_t offset, std::size_t cursor_at) {
		if (_in.marks == nullptr)
			return;
		const std::vector<virtual_text>& texts = _in.marks->texts();
		// Sorted by offset, so `<=` and not `==`: a text whose offset landed
		// inside a cluster - or inside an escape the prompt walk skipped - is
		// drawn at the next boundary rather than lost. A text past the end of a
		// buffer that has since been shortened is simply never reached, which
		// is the right answer for a decoration the editor has moved past.
		if (_text_at >= texts.size() || texts[_text_at].at > offset)
			return;

		if (cursor_at != no_cursor && cursor_at <= offset && !_cursor_placed)
			record_cursor();

		while (_text_at < texts.size() && texts[_text_at].at <= offset) {
			const virtual_text& one = texts[_text_at];
			++_text_at;
			if (one.bytes.empty())
				continue;
			flush();
			const style ink = _in.theme == nullptr
			                      ? _in.text_pen
			                      : _in.theme->over(one.style_id, _in.text_pen);
			place(one.bytes, ink, escapes::paint, no_cursor, secondary::no,
			      annotate::no);
		}
	}

	// Make room for a cluster `width` columns wide, wrapping if it does not fit
	// on this row. This is soft wrap, and it is one rule: the pending-wrap
	// column and the wide cluster that would straddle the edge are the same
	// case, and the answer to both is that the cluster starts the next row
	// whole rather than being split or clipped.
	void reserve(int width) {
		if (_column == 0)
			return;  // a new row is no wider; nothing to gain
		if (static_cast<int>(_column) + width <= static_cast<int>(_in.columns))
			return;
		flush();
		next_row();
		_column = 0;
	}

	void advance(int width) {
		const int next = static_cast<int>(_column) + width;
		// Clamped, so a cluster wider than the whole terminal leaves the column
		// at the edge rather than past it. `_column == columns` is pending
		// wrap: the row is full, and the next cluster - or the cursor -
		// resolves it.
		_column = static_cast<std::uint16_t>(std::min(next, static_cast<int>(_in.columns)));
		note_row(_row);
	}

	void break_line() {
		flush();
		next_row();
		_column = 0;
		// Noted even though nothing may land on it: a buffer ending in a
		// newline HAS a last, empty line, and F-2 renders the buffer whole.
		note_row(_row);
	}

	// The pen is COPIED into the run, not pointed at. It has to be: `place`'s
	// pen is a local that the next SGR moves, and a pointer would emit the run
	// that came before an SGR in the colour that came after it.
	void open_run(std::string_view source, std::size_t at, const style& pen) {
		if (_run_open)
			return;
		_run_source = source;
		_run_begin = at;
		_run_end = at;
		_run_row = _row;
		_run_column = _column;
		_run_pen = pen;
		_run_open = true;
	}

	void flush() {
		if (_run_open && _run_end > _run_begin)
			_emit(_run_row, _run_column,
			      _run_source.substr(_run_begin, _run_end - _run_begin), _run_pen);
		_run_open = false;
	}

	void record_cursor() {
		std::uint16_t row = _row;
		std::uint16_t column = _column;
		if (column >= _in.columns) {
			// The phantom column is not a cell the blitter can reach: every
			// move it emits starts with `\r`, which cancels the terminal's
			// pending wrap. See layout.h, decision 3.
			if (row < last_row)
				++row;
			column = 0;
		}
		_cursor_row = row;
		_cursor_column = column;
		_cursor_placed = true;
		note_row(row);
	}

	void next_row() {
		if (_row < last_row)
			++_row;
	}

	void note_row(std::uint16_t row) {
		if (row > _max_row)
			_max_row = row;
	}

	const layout_input& _in;
	Emit _emit;

	std::uint16_t _row = 0;
	std::uint16_t _column = 0;
	std::uint16_t _max_row = 0;

	bool _cursor_placed = false;
	std::uint16_t _cursor_row = 0;
	std::uint16_t _cursor_column = 0;

	// The two monotone cursors into the normalized decorations, reset by
	// construction on each of the two passes.
	std::size_t _span_at = 0;
	std::size_t _text_at = 0;

	bool _run_open = false;
	std::string_view _run_source;
	std::size_t _run_begin = 0;
	std::size_t _run_end = 0;
	std::uint16_t _run_row = 0;
	std::uint16_t _run_column = 0;
	style _run_pen;
};

// Which content row the top of the screen shows (layout.h, decision 4):
// centred on the cursor, clamped to the content's two ends, and derived rather
// than remembered.
std::uint16_t first_visible_row(const geometry& content, std::uint16_t rows) {
	if (content.content_rows <= rows)
		return 0;
	const int centred = static_cast<int>(content.cursor_row) - static_cast<int>(rows) / 2;
	const int furthest = static_cast<int>(content.content_rows) - static_cast<int>(rows);
	return static_cast<std::uint16_t>(std::clamp(centred, 0, furthest));
}

} // namespace

layout lay_out(cluster_pool& pool, const layout_input& in) {
	layout made;
	// A size nobody has reported is not a size. An empty layout diffs to
	// nothing and paints nothing, which is the honest answer before the loop's
	// first winsize query.
	if (in.columns == 0 || in.rows == 0)
		return made;

	const geometry content =
		walker{in, [](std::uint16_t, std::uint16_t, std::string_view, const style&) {}}.run();

	made.content_rows = content.content_rows;
	made.cursor_row = content.cursor_row;
	made.cursor_column = content.cursor_column;
	made.first_visible_row = first_visible_row(content, in.rows);

	made.screen.resize(in.columns, std::min(content.content_rows, in.rows));

	surface& screen = made.screen;
	const std::uint16_t top = made.first_visible_row;
	walker{in, [&](std::uint16_t row, std::uint16_t column, std::string_view text,
	               const style& pen) {
		       if (row < top)
			       return;
		       const std::uint16_t on_screen = static_cast<std::uint16_t>(row - top);
		       if (on_screen >= screen.rows())
			       return;
		       screen.write(pool, on_screen, column, text, pen, in.width);
	       }}.run();

	// The window is derived FROM the cursor, so it contains it by construction.
	LESH_ASSERT(made.cursor_row >= top
	            && static_cast<std::uint16_t>(made.cursor_row - top) < screen.rows());
	screen.cursor().row = static_cast<std::uint16_t>(made.cursor_row - top);
	screen.cursor().column = made.cursor_column;
	screen.cursor().visible = true;
	return made;
}

layout reflow(cluster_pool& pool, layout_input in, const resize_event& to) {
	in.columns = to.columns;
	in.rows = to.rows;
	return lay_out(pool, in);
}

} // namespace lesh::leshper
