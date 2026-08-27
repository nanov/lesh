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

	// Erase from the cursor to the end of the line. The pen goes back to
	// default first: with background-colour erase, ESC[K paints the erased span
	// in whatever background is in force, and the span is meant to be blank.
	void clear_to_end_of_line() {
		reset_pen();
		_out.append("\x1b[K");
	}

	// Erase from the cursor to the end of the SCREEN, and for ESC[K's reason:
	// background-colour erase would paint the span in whatever background is in
	// force, and the span is meant to be blank.
	void clear_to_end_of_screen() {
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

		const bool clear_tail = static_cast<int>(blank_tail) <= last;
		const int run_end = clear_tail ? std::max(first, static_cast<int>(blank_tail)) : last + 1;

		terminal.move_to(row, static_cast<std::uint16_t>(first));
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
