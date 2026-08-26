#include "leshper/layout.h"

#include "leshper/pager.h"
#include "leshper/sgr.h"
#include "substrate/assert.h"
#include "substrate/measure.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

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
		place(_in.prompt, _in.prompt_pen, escapes::skip, no_cursor, secondary::no);

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
		      secondary::yes);
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

	// `pen` BY VALUE, and that is the whole of #131 on this side: an SGR in
	// prompt bytes moves it, and it moves for the clusters that FOLLOW. One
	// copy per call, eleven bytes, and no pen state outlives the text it
	// belongs to - the prompt's colours cannot bleed into the buffer, because
	// the buffer's `place` starts from `text_pen` again.
	void place(std::string_view text, style pen, escapes on_escape,
	           std::size_t cursor_at, secondary continuations) {
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
					      secondary::no);
				i = end;
				continue;
			}

			open_run(text, i, pen);
			_run_end = end;
			advance(width);
			i = end;
		}
		// A run belongs to one source view and one pen, and the next `place`
		// brings both.
		flush();
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

	// THE PAGER FIRST, and that ordering is the decision (#138, layout.h note 6).
	// Its rows come off the terminal's height BEFORE the edit view is windowed,
	// so a long list narrows the window the buffer scrolls in rather than
	// pushing the edit line off the screen. It renders into a surface of its own
	// (A-6); this file only copies it.
	surface page;
	if (in.pager != nullptr && in.pager->showing()) {
		const pager_grid grid =
			measure_pager(*in.pager, in.columns, pager_row_budget(in.rows), in.width);
		if (!grid.empty())
			page = render_pager(pool, *in.pager, grid, in.pager_pen, in.width);
	}
	const std::uint16_t pager_rows = page.rows();
	// `pager_row_budget` never takes the last row, so this is at least one.
	const std::uint16_t edit_rows = static_cast<std::uint16_t>(in.rows - pager_rows);
	LESH_ASSERT(edit_rows >= 1);

	const geometry content =
		walker{in, [](std::uint16_t, std::uint16_t, std::string_view, const style&) {}}.run();

	made.content_rows = content.content_rows;
	made.cursor_row = content.cursor_row;
	made.cursor_column = content.cursor_column;
	made.first_visible_row = first_visible_row(content, edit_rows);

	const std::uint16_t shown = std::min(content.content_rows, edit_rows);
	made.screen.resize(in.columns, static_cast<std::uint16_t>(shown + pager_rows));
	made.pager_rows = pager_rows;

	surface& screen = made.screen;
	const std::uint16_t top = made.first_visible_row;
	walker{in, [&](std::uint16_t row, std::uint16_t column, std::string_view text,
	               const style& pen) {
		       if (row < top)
			       return;
		       const std::uint16_t on_screen = static_cast<std::uint16_t>(row - top);
		       if (on_screen >= shown)
			       return;
		       screen.write(pool, on_screen, column, text, pen, in.width);
	       }}.run();

	// The pager's cells, copied in under the edit line. Cell for cell rather
	// than through `write`, because the pager already decided what every one of
	// them is - re-deriving them here would be a second layout engine, and the
	// two would disagree the first time a candidate was two columns wide.
	for (std::uint16_t row = 0; row < pager_rows; ++row) {
		for (std::uint16_t column = 0; column < page.columns(); ++column)
			screen.at(static_cast<std::uint16_t>(shown + row), column) = page.at(row, column);
	}

	// The window is derived FROM the cursor, so it contains it by construction.
	LESH_ASSERT(made.cursor_row >= top
	            && static_cast<std::uint16_t>(made.cursor_row - top) < shown);
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
