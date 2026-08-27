#pragma once

#include "leshper/surface.h"

#include <cstdint>
#include <string>

namespace lesh::leshper {

// The blitter: surfaces in, terminal bytes out (F-37, #97).
//
// The one file in leshper that knows what an escape sequence is, and the one
// place the 256-colour downmap exists. Everything above it thinks in cells;
// everything below it is #98's event loop, which owns the terminal.
//
// IT DOES NOT WRITE. #98's principle is that all tty interaction goes through
// the event loop - `tcsetpgrp`, mode changes, winsize, and the redraw itself.
// So `update` answers with bytes and the loop is what hands them to the fd. No
// syscall in this file, no fd in this file, nothing here to mock in a test.
//
// The output vocabulary is the floor and nothing else (#97: never terminfo, no
// startup queries, the emitted set is hardcoded ANSI):
//
//   \r                 column 0 of the current row, and the one sequence that
//                      reliably cancels a terminal's pending-wrap state
//   ESC [ n A / B      up / down n rows
//   ESC [ n C          right n columns
//   ESC [ K            erase from the cursor to the end of the line
//   ESC [ J            erase from the cursor to the end of the screen
//   ESC [ ... m        SGR: the seven attributes and the two colours
//   ESC [ ? 25 h / l   cursor visibility
//
// Deliberately absent: absolute positioning (CUP). A surface is not the
// screen - F-39 has shell output scrolling above the edit line, so the
// surface's origin is wherever the terminal's cursor happened to be - and an
// absolute row number would be a lie the moment anything scrolled. fish moves
// relatively for exactly this reason. Every move here therefore starts with
// `\r`, which costs one byte and makes the sequence independent of whether the
// last glyph left the terminal in pending-wrap.

// What the terminal on the far side can do beyond the floor.
//
// #97 decision 5: all runtime, one binary. Nothing here is a build variant.
enum class color_depth : std::uint8_t {
	monochrome,   // NO_COLOR, or a terminal that says nothing: no colour SGR at all
	indexed_256,  // the floor
	truecolor,    // opportunistic
};

struct terminal_capabilities {
	color_depth colors = color_depth::indexed_256;
	bool undercurl = false;  // opportunistic; a plain underline below it

	// #97's floor: 256 colours, no undercurl.
	[[nodiscard]] static constexpr terminal_capabilities floor() noexcept { return {}; }

	// #97 decision 2, "assume first": the trivial environment reads and nothing
	// else. Takes the strings rather than calling `getenv`, so it is pure, so a
	// test can ask it about a terminal the machine does not have.
	//
	// It answers what the terminal can DO. It deliberately does not decide what
	// to do about a terminal below the floor - that is a one-line refusal at
	// editor start (#97 decision 3), and start-up is not the blitter.
	[[nodiscard]] static terminal_capabilities from_env(const char* term,
	                                                    const char* colorterm,
	                                                    const char* no_color) noexcept;

	friend constexpr bool operator==(terminal_capabilities,
	                                 terminal_capabilities) noexcept = default;
};

// The 256-colour downmap, and the only copy of it that exists (#97 decision 4:
// "quantizing to the terminal is the blitter's job at emit time").
//
// The xterm cube: 16 + 36r + 6g + b over the levels {0, 95, 135, 175, 215,
// 255}, plus the 24-step grey ramp at 232..255, whichever is nearer in squared
// RGB distance. Indices 0..15 are not candidates on purpose - those slots are
// the user's palette, which the user may have redefined, so mapping a theme
// colour into one substitutes our guess for their choice. tmux and alacritty
// draw the line in the same place.
[[nodiscard]] std::uint8_t quantize_to_256(std::uint8_t r, std::uint8_t g,
                                           std::uint8_t b) noexcept;

// Turns surfaces into bytes. Holds the pool the cells' glyph refs are ids into
// and the capabilities of the terminal; owns nothing else and mutates nothing.
//
// THE CONTRACT, which is what makes the diff a pure function of two values:
//
//   On entry to `update` the terminal shows exactly `previous`, its cursor is
//   at `previous.cursor()`, and its pen is default. On entry to `paint` the
//   cursor is at the surface's own origin - row 0, column 0 - and the pen is
//   default; what the terminal shows is unknown, so every row is rewritten.
//
//   `paint_from` is `paint` for the OTHER repaint, the one where the terminal
//   is still showing a frame: the cursor is `start` rows below that frame's top
//   row, and the caller says how many. It walks back up there, erases what was
//   below, and then paints exactly as `paint` does.
//
//   On exit the terminal shows exactly `desired`, its cursor is at
//   `desired.cursor()`, and its pen is default again.
//
// Which is to say the loop keeps the last surface it painted and hands it back
// as `previous`. Nothing is remembered here between calls - there is no
// "actual screen" member to drift out of step with the terminal, which is the
// failure mode `screen_t` guards against with `s_reset`.
class blitter {
public:
	explicit blitter(const cluster_pool& pool,
	                 terminal_capabilities caps = terminal_capabilities::floor()) noexcept
	    : _pool(&pool), _caps(caps) {}

	[[nodiscard]] const terminal_capabilities& capabilities() const noexcept { return _caps; }

	// The minimal update from `previous` to `desired`: only rows that changed,
	// only the span of each row that changed, one SGR per style transition, and
	// ESC[K rather than a run of spaces where a row got shorter.
	//
	// Empty when nothing changed, cursor included. Surfaces of different sizes
	// fall back to a full repaint: what a resize does to CONTENT is F-38's, and
	// diffing two grids of different shapes would be inventing half an answer.
	[[nodiscard]] std::string update(const surface& previous, const surface& desired) const;
	void update_into(const surface& previous, const surface& desired, std::string& out) const;

	// Every row, unconditionally. The repaint half of F-37's "full repaint only
	// on resize or explicit request".
	//
	// THE FIRST PAINT OF A READ, and only that: the contract above says the
	// cursor is already at the surface's origin, which is true of a prompt the
	// shell has just started drawing and false of every later repaint. It emits
	// no ESC[J, because there is nothing below it that is ours to erase.
	[[nodiscard]] std::string paint(const surface& desired) const;
	void paint_into(const surface& desired, std::string& out) const;

	// The repaint that REPLACES a frame instead of appending one (#185, F-38).
	//
	// `start` is where the terminal's cursor is IN THE FRAME IT IS SHOWING -
	// which, after a resize, is that frame as the terminal has since reflowed
	// it, and the caller is the only side that can know it. Emits `\r`, then
	// ESC[<start.row>A when the row is not already zero, then ESC[J to erase the
	// old frame's rows from there down, then the whole of `desired` from row 0.
	//
	// Only `start.row` is read. The column is not: `\r` goes to column zero
	// whatever it was, which is also the one sequence that resolves a pending
	// wrap, and a frame's top row starts at column zero by construction.
	void paint_from(const cursor_placement& start, const surface& desired,
	                std::string& out) const;

private:
	// The `_into` forms are the primitives and the returning forms wrap them:
	// N-2 wants hot-path allocation bounded and jitter-free, and the loop that
	// calls this every keystroke keeps one buffer and reuses it.
	// `previous` null is a repaint; `from` non-null is a repaint that has a frame
	// to replace rather than a blank origin to paint at. The two are independent
	// only in principle - `paint_from` is the one caller that passes a `from` -
	// but keeping them separate parameters is what keeps the diff path's start
	// (`previous->cursor()`) out of the repaint path's arithmetic.
	void emit(const surface* previous, const cursor_placement* from, const surface& desired,
	          std::string& out) const;

	const cluster_pool* _pool;
	terminal_capabilities _caps;
};

} // namespace lesh::leshper
