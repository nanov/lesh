#include "leshper/blit.h"

#include "substrate/assert.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace lesh::leshper {

namespace {

void append_uint(std::string& out, unsigned value) {
	char digits[8];
	int written = 0;
	do {
		digits[written++] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value != 0);
	while (written > 0)
		out.push_back(digits[--written]);
}

// The terminal, as the blitter believes it to be while a single update is
// being built: where the cursor is, and what SGR state is in force.
//
// Both are derivable from the bytes already emitted, which is why this is a
// local of one call and not a member of the blitter. A member would be a
// second model of the terminal living beside the caller's `previous` surface,
// and two models of one terminal is how a renderer ends up with a `s_reset`.
//
// THE RIGHT EDGE, which is most of what this class is about (#112, #189).
// Writing a glyph into the last column does NOT move the cursor to the next
// row; it leaves the terminal in PENDING WRAP, holding the cursor on the last
// column and the decision until the next glyph arrives. Three rules follow,
// and each of them is a bug somebody else already had:
//
//   1. MOVING resolves the ambiguity with `\r`, always. After it the cursor is
//      at column zero of THIS row whatever the terminal was thinking, and the
//      row above stays a HARD line - a line of its own, which a resize will not
//      rewrap. `move_to` and `move_to_frame_top` are the two movers.
//
//   2. WRITING THROUGH the wrap is how a SOFT row is reached, and the only way:
//      the next glyph, emitted with no positioning at all, lands at column zero
//      of the next row AND tells the terminal the two rows are one logical line.
//      `wrap_through` is that - it emits nothing and only says where the next
//      `put` will land. Anything else here, `\r` included, cancels the wrap and
//      makes the soft row hard, which is the defect #189 is about.
//
//   3. ERASING WHILE A WRAP IS PENDING IS FORBIDDEN. The cursor is still ON the
//      last column, so `ESC[K` eats the glyph just written (fish's screen.rs,
//      zsh's "clearing eol would be evil") and `ESC[J` eats it and the rows
//      below (fish #6951). There is nothing between the cursor and the end of
//      that line to erase anyway, so the erase is skipped rather than reordered.
class emitter {
public:
	emitter(std::string& out, const cluster_pool& pool, terminal_capabilities caps,
	        cursor_placement start, std::uint16_t columns) noexcept
	    : _out(out), _pool(pool), _caps(caps), _row(start.row), _column(start.column),
	      _columns(columns) {}

	void move_to(std::uint16_t row, std::uint16_t column) {
		if (_row == row && _column == column && !_wrap_pending)
			return;

		// `\r` first, always, when the column is not already known to be zero.
		// It is the one sequence that resolves the pending-wrap ambiguity a
		// glyph in the last column leaves behind: after it the cursor is at
		// column zero of THIS row, whatever the terminal was thinking.
		if (_column != 0 || _wrap_pending) {
			_out.push_back('\r');
			_column = 0;
			_wrap_pending = false;
		}
		if (row < _row) {
			_out.append("\x1b[");
			append_uint(_out, static_cast<unsigned>(_row - row));
			_out.push_back('A');
		} else if (row > _row) {
			_out.append("\x1b[");
			append_uint(_out, static_cast<unsigned>(row - _row));
			_out.push_back('B');
		}
		_row = row;
		if (column > 0) {
			_out.append("\x1b[");
			append_uint(_out, static_cast<unsigned>(column));
			_out.push_back('C');
		}
		_column = column;
	}

	void put(const cell& one) {
		set_pen(one.pen);
		std::string_view bytes = _pool.cluster_of(one.glyph);
		if (bytes.empty())
			bytes = " ";  // a continuation cell has no glyph and should not reach here
		_out.append(bytes);
		_column = static_cast<std::uint16_t>(_column + one.width);
		if (_column >= _columns) {
			// The terminal is now in pending wrap, or has already wrapped, and
			// which one is not knowable from here. The next move starts with \r.
			_column = _columns;
			_wrap_pending = true;
		}
	}

	// Whether the terminal is holding a pending wrap at the end of `row` - the
	// last glyph emitted landed in that row's last column, and the next one will
	// fall through to the row below it.
	[[nodiscard]] bool pending_wrap_at(std::uint16_t row) const noexcept {
		return _wrap_pending && _row == row;
	}

	// WRITE THROUGH THE WRAP (#189), and note that it emits nothing: the next
	// `put` IS the move. The terminal is holding a pending wrap, so the glyph
	// that comes next lands at column zero of `row` on its own - and, unlike
	// every `\r`-led move, leaves the row above marked as SOFT-WRAPPED, which is
	// what makes the terminal's picture of the frame agree with the layout's
	// when the window is resized.
	//
	// The caller owes one glyph after this. Erasing instead would erase the row
	// ABOVE (see the class comment, rule 3), and moving instead would cancel the
	// wrap - either way the write-through is off by a row.
	void wrap_through(std::uint16_t row) {
		LESH_ASSERT(_wrap_pending && static_cast<std::uint16_t>(_row + 1) == row);
		_row = row;
		_column = 0;
		_wrap_pending = false;
	}

	// Erase from the cursor to the end of the line. The pen goes back to
	// default first: with background-colour erase, ESC[K paints the erased span
	// in whatever background is in force, and the span is meant to be blank.
	void clear_to_end_of_line() {
		// Class comment, rule 3: the span is empty and the erase would eat the
		// last column instead.
		if (_wrap_pending)
			return;
		reset_pen();
		_out.append("\x1b[K");
	}

	// Erase from the cursor to the end of the SCREEN, and for ESC[K's reason:
	// background-colour erase would paint the span in whatever background is in
	// force, and the span is meant to be blank.
	void clear_to_end_of_screen() {
		// Rule 3 again, and here the erase cannot simply be skipped - it is the
		// whole point of the call - so the wrap is resolved first. `paint_from`,
		// the only caller today, has just done that in `move_to_frame_top`; the
		// rule is in the code rather than in that call's comment because an
		// erase reached with a wrap pending is fish #6951 and costs a row.
		if (_wrap_pending) {
			_out.push_back('\r');
			_column = 0;
			_wrap_pending = false;
		}
		reset_pen();
		_out.append("\x1b[J");
	}

	// Up to the top-left of the frame the terminal is showing, from wherever the
	// last frame left the cursor (#185). Not `move_to`: that one moves within
	// the surface being emitted, whose row zero is where this lands, so it has
	// nothing to say about a frame that is not on screen yet.
	//
	// `\r` UNCONDITIONALLY, before the count. It is the only sequence that
	// resolves the pending-wrap ambiguity a glyph in the last column leaves
	// behind, and after a resize the terminal's own reflow means even the
	// column this class thinks it is in is not knowable from here.
	void move_to_frame_top(std::uint16_t rows_above) {
		_out.push_back('\r');
		_column = 0;
		_wrap_pending = false;
		if (rows_above > 0) {
			_out.append("\x1b[");
			append_uint(_out, rows_above);
			_out.push_back('A');
		}
		_row = 0;
	}

	void reset_pen() {
		if (_pen == style{})
			return;
		_out.append("\x1b[0m");
		_pen = style{};
	}

private:
	// What the terminal can actually show, given its capabilities. Doing this
	// once, here, is what lets the pen comparison stay a plain equality: on a
	// monochrome terminal two cells that differ only in colour resolve to the
	// same pen and emit nothing between them.
	[[nodiscard]] style resolved(const style& raw) const noexcept {
		style out = raw;
		if (_caps.colors == color_depth::monochrome) {
			out.fg = color::of_default();
			out.bg = color::of_default();
		}
		if (!_caps.undercurl && has(out.attrs, attribute::undercurl))
			out.attrs = (out.attrs & ~attribute::undercurl) | attribute::underline;
		return out;
	}

	void add_param(std::string& params, std::string_view text) const {
		if (!params.empty())
			params.push_back(';');
		params.append(text);
	}
	void add_param(std::string& params, unsigned value) const {
		if (!params.empty())
			params.push_back(';');
		append_uint(params, value);
	}

	void add_color(std::string& params, bool foreground, color which) const {
		const unsigned base = foreground ? 30u : 40u;
		switch (which.kind) {
			case color_kind::terminal_default:
				add_param(params, base + 9);  // 39 / 49
				break;
			case color_kind::indexed:
				// Already a palette slot. Never rewritten: the user may have
				// redefined it, and resolving it to RGB would replace their
				// choice with ours.
				add_param(params, base + 8);
				add_param(params, 5u);
				add_param(params, which.index);
				break;
			case color_kind::truecolor:
				add_param(params, base + 8);
				if (_caps.colors == color_depth::truecolor) {
					add_param(params, 2u);
					add_param(params, which.r);
					add_param(params, which.g);
					add_param(params, which.b);
				} else {
					add_param(params, 5u);
					add_param(params, quantize_to_256(which.r, which.g, which.b));
				}
				break;
		}
	}

	void set_pen(const style& raw) {
		const style want = resolved(raw);
		if (want == _pen)
			return;

		if ((_pen.attrs & ~want.attrs) != attribute::none) {
			// Something has to turn OFF, and the off-switches do not partition
			// the attributes: SGR 22 clears bold and dim together, so "dim off,
			// bold still on" cannot be said in one parameter. Reset and restate.
			_out.append("\x1b[0m");
			_pen = style{};
			if (want == _pen)
				return;
		}

		std::string params;
		const attribute added = want.attrs & ~_pen.attrs;
		if (has(added, attribute::bold))
			add_param(params, 1u);
		if (has(added, attribute::dim))
			add_param(params, 2u);
		if (has(added, attribute::italic))
			add_param(params, 3u);
		if (has(added, attribute::undercurl))
			add_param(params, "4:3");  // resolved() already removed it when unsupported
		else if (has(added, attribute::underline))
			add_param(params, 4u);
		if (has(added, attribute::reverse))
			add_param(params, 7u);
		if (has(added, attribute::strikethrough))
			add_param(params, 9u);
		if (want.fg != _pen.fg)
			add_color(params, true, want.fg);
		if (want.bg != _pen.bg)
			add_color(params, false, want.bg);

		LESH_ASSERT(!params.empty());
		_out.append("\x1b[");
		_out.append(params);
		_out.push_back('m');
		_pen = want;
	}

	std::string& _out;
	const cluster_pool& _pool;
	terminal_capabilities _caps;
	std::uint16_t _row = 0;
	std::uint16_t _column = 0;
	std::uint16_t _columns = 0;
	bool _wrap_pending = false;
	style _pen;
};

} // namespace

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

terminal_capabilities terminal_capabilities::from_env(const char* term, const char* colorterm,
                                                      const char* no_color) noexcept {
	terminal_capabilities caps;
	if (no_color != nullptr && no_color[0] != '\0')
		caps.colors = color_depth::monochrome;
	else if (term == nullptr || term[0] == '\0' || std::strcmp(term, "dumb") == 0)
		caps.colors = color_depth::monochrome;
	else if (colorterm != nullptr && (std::strcmp(colorterm, "truecolor") == 0
	                                  || std::strcmp(colorterm, "24bit") == 0))
		caps.colors = color_depth::truecolor;
	// Undercurl is left off. No environment variable announces it and #97
	// forbids asking the terminal, so the answer arrives - if ever - as an
	// explicit setting rather than as a guess made here.
	return caps;
}

// ---------------------------------------------------------------------------
// Quantization
// ---------------------------------------------------------------------------

std::uint8_t quantize_to_256(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
	constexpr int LEVELS[6] = {0, 95, 135, 175, 215, 255};

	const auto nearest_level = [](int value) {
		int best = 0;
		int best_distance = value - LEVELS[0];
		best_distance = best_distance < 0 ? -best_distance : best_distance;
		for (int i = 1; i < 6; ++i) {
			int distance = value - LEVELS[i];
			distance = distance < 0 ? -distance : distance;
			if (distance < best_distance) {
				best = i;
				best_distance = distance;
			}
		}
		return best;
	};
	const auto squared = [](int a, int b_, int c) { return a * a + b_ * b_ + c * c; };

	const int red = r, green = g, blue = b;
	const int ri = nearest_level(red), gi = nearest_level(green), bi = nearest_level(blue);
	const int cube_distance = squared(red - LEVELS[ri], green - LEVELS[gi], blue - LEVELS[bi]);

	// The grey ramp, 232..255, at 8 + 10i. Worth checking separately: a near-grey
	// lands between two cube levels 40 apart, where the ramp is 10 apart.
	const int average = (red + green + blue) / 3;
	int step = average < 8 ? 0 : (average - 8 + 5) / 10;
	step = std::clamp(step, 0, 23);
	const int grey = 8 + 10 * step;
	const int grey_distance = squared(red - grey, green - grey, blue - grey);

	if (grey_distance < cube_distance)
		return static_cast<std::uint8_t>(232 + step);
	return static_cast<std::uint8_t>(16 + 36 * ri + 6 * gi + bi);
}

// ---------------------------------------------------------------------------
// The diff
// ---------------------------------------------------------------------------

void blitter::emit(const surface* previous, const cursor_placement* from,
                   const surface& desired, std::string& out) const {
	// A size change is a repaint. fish computes a whole new layout on resize
	// rather than diffing across one, and for the same reason: the two grids
	// stop being comparable cell by cell, and answering what a row means at a
	// different width is F-38's reflow, not the differ's.
	//
	// A REPAINT REPLACES A FRAME; IT DOES NOT APPEND ONE (#185). The cursor is
	// not at the origin of anything - it is wherever the last frame left it, at
	// the end of the buffer - so painting from there put a second copy of the
	// prompt below the first, once per resize. `from` is the caller's answer to
	// "how far above me does the frame I am replacing start", which only the
	// caller can know, because after a resize the terminal has reflowed that
	// frame and the surface we painted no longer describes it. Null - which is
	// `paint`, the first paint of a read - means the cursor really is at the
	// origin and there is nothing below it that is ours to erase.
	//
	// AND THE FRAME IT PAINTS IS SHAPED THE WAY THE TERMINAL SHAPES ONE (#189):
	// a row the layout produced by soft-wrapping is written THROUGH the right
	// edge, so the terminal joins it to the row above as one logical line and
	// reflows the pair exactly as `frame_top_above_cursor` assumes it will. A
	// row that begins after a hard newline is still positioned to. Before this,
	// every row was a hard line to the terminal: a shrink clipped each one
	// separately and left fragments, and a grow never rejoined them.
	const bool repaint = previous == nullptr || previous->columns() != desired.columns()
	                  || previous->rows() != desired.rows();

	const cursor_placement start =
		repaint ? cursor_placement{} : previous->cursor();
	emitter terminal{out, *_pool, _caps, start, desired.columns()};
	if (from != nullptr) {
		terminal.move_to_frame_top(from->row);
		terminal.clear_to_end_of_screen();
	}

	const std::uint16_t columns = desired.columns();
	for (std::uint16_t row = 0; row < desired.rows() && columns > 0; ++row) {
		// DOES THE ROW BELOW CONTINUE THIS ONE THROUGH THE WRAP (#189)? Then
		// this row is written out to its LAST COLUMN - trailing blanks and all,
		// and with no `ESC[K` - because that last glyph is the only thing that
		// makes the terminal wrap, and a wrap is the only thing that makes the
		// two rows one logical line to it.
		//
		// A soft row is nearly always full by construction; the exception is the
		// row that wrapped because a two-column cluster would have straddled the
		// edge, which leaves one blank column behind. That blank is written as a
		// space - it is what the layout says is in that cell, and the alternative
		// is emitting half a wide glyph, which #97's floor cannot express.
		//
		// REPAINTS ONLY. The diff path positions absolutely (see below).
		//
		// NEVER PAST THE FRAME'S LAST ROW, which is what keeps the bottom-right
		// corner safe: there is no row below it to write through to, so the last
		// row keeps the ordinary "last cell, then `\r`" ending and the terminal
		// is never handed the glyph that would scroll the screen. That the
		// frame's last row may also be the SCREEN's last row costs nothing for
		// the same reason.
		const bool wrapped_into_by_next =
			repaint && row + 1 < desired.rows()
			&& !desired.row_starts_hard_line(static_cast<std::uint16_t>(row + 1));

		int first = -1;
		int last = -1;
		if (repaint) {
			first = 0;
			last = columns - 1;
		} else {
			for (std::uint16_t column = 0; column < columns; ++column) {
				if (previous->at(row, column) == desired.at(row, column))
					continue;
				if (first < 0)
					first = column;
				last = column;
			}
			if (first < 0)
				continue;  // this row is already right
			// Never start on the second half of a two-column cluster - in
			// either surface. Beginning there would leave the terminal holding
			// the left half of a glyph whose right half has been overwritten.
			while (first > 0
			       && (desired.at(row, static_cast<std::uint16_t>(first)).glyph.is_continuation()
			           || previous->at(row, static_cast<std::uint16_t>(first))
			                  .glyph.is_continuation()))
				--first;
		}

		// The blank tail of the desired row. Where it starts before the last
		// difference does, ESC[K says in three bytes what a run of spaces would
		// say in as many bytes as the row is wide - fish's trick, and the reason
		// deleting to end of line does not cost a screen width of output.
		std::uint16_t blank_tail = columns;
		while (blank_tail > 0 && desired.at(row, static_cast<std::uint16_t>(blank_tail - 1))
		                             == blank_cell)
			--blank_tail;

		bool clear_tail = static_cast<int>(blank_tail) <= last;
		int run_end = clear_tail ? std::max(first, static_cast<int>(blank_tail)) : last + 1;
		if (wrapped_into_by_next) {
			clear_tail = false;
			run_end = columns;
		}

		// HOW THIS ROW IS REACHED (#189): by writing through the wrap the row
		// above left pending, when this row is a soft continuation of it and the
		// write starts at column zero - or, in every other case, by positioning.
		//
		// THE DIFF PATH GETS HERE TOO, and deliberately. `pending_wrap_at` is a
		// fact about the bytes already emitted, not about the surface: it is true
		// only when this update happened to rewrite the row above right up to its
		// last column, and then writing through is what keeps a rewrite from
		// turning a soft row hard. When it did NOT rewrite that last column the
		// terminal's own wrap flag was never disturbed, and `move_to` is free to
		// position - which is why the diff path needs no forcing of its own.
		const bool through_the_wrap =
			row > 0 && first == 0
			&& !desired.row_starts_hard_line(row)
			&& terminal.pending_wrap_at(static_cast<std::uint16_t>(row - 1));
		if (through_the_wrap) {
			terminal.wrap_through(row);
			// The write-through owes the terminal a glyph: it is the glyph that
			// performs the move. A row with nothing to write - the empty row a
			// buffer ending at the right edge puts the cursor on - pays with the
			// blank in its first column, and only then may the tail be erased.
			if (run_end == first)
				run_end = first + 1;
		} else {
			terminal.move_to(row, static_cast<std::uint16_t>(first));
		}
		for (int column = first; column < run_end; ++column) {
			const cell& one = desired.at(row, static_cast<std::uint16_t>(column));
			if (one.glyph.is_continuation())
				continue;  // the cluster to its left already covered this column
			terminal.move_to(row, static_cast<std::uint16_t>(column));
			terminal.put(one);
		}
		if (clear_tail)
			terminal.clear_to_end_of_line();
	}

	// The contract's other half: pen default, cursor where the surface says.
	terminal.reset_pen();
	terminal.move_to(desired.cursor().row, desired.cursor().column);

	const bool was_visible = repaint ? true : previous->cursor().visible;
	if (desired.cursor().visible != was_visible)
		out.append(desired.cursor().visible ? "\x1b[?25h" : "\x1b[?25l");
}

void blitter::update_into(const surface& previous, const surface& desired,
                          std::string& out) const {
	emit(&previous, nullptr, desired, out);
}

std::string blitter::update(const surface& previous, const surface& desired) const {
	std::string out;
	emit(&previous, nullptr, desired, out);
	return out;
}

void blitter::paint_into(const surface& desired, std::string& out) const {
	emit(nullptr, nullptr, desired, out);
}

std::string blitter::paint(const surface& desired) const {
	std::string out;
	emit(nullptr, nullptr, desired, out);
	return out;
}

void blitter::paint_from(const cursor_placement& start, const surface& desired,
                         std::string& out) const {
	emit(nullptr, &start, desired, out);
}

} // namespace lesh::leshper
