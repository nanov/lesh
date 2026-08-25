#pragma once

// SGR read backwards (#131): the bytes blit.cpp emits for a pen, turned back
// into the pen that produced them.
//
// #114's measurer decides what an escape IS and hands the whole sequence back
// as zero width; #123's layout skipped every one of them, which is why a theme
// author who wrote `ESC[32m$ ESC[0m` got a plain `$`. This file is the missing
// half: given one recognized escape, it answers what the pen becomes. A
// sequence that is not SGR answers with the pen unchanged, so the caller applies
// it to every escape and the "is this one mine?" question stays here.
//
// WHY IT IS ITS OWN HEADER, between the two rings rather than inside either.
//
// It is not layout's. Nothing in it is a layout decision - no row, no column,
// no wrap - and burying it in `layout.cpp` would make the one file that must
// stay readable as geometry also the file that knows 38;2;r;g;b. It would also
// be untestable except through a cell grid, which is a long way to ask "does
// 4:3 mean undercurl".
//
// It is not the blitter's either, and that is the load-bearing one. `blit.h`
// owns the FORWARD direction and it owns much more besides - the terminal's
// capabilities, the 256-colour downmap, the diff. Putting the reader there
// would make `layout.cpp` include `blit.h`, and layout does not depend on the
// blitter: layout produces a surface and the blitter consumes one. They are
// siblings over `surface.h`, and a header that both may include without either
// including the other is the shape that keeps them siblings.
//
// So: a header beside them, depending on `surface.h` for the `style` vocabulary
// and on `substrate/numeric.h` for the digits, and on nothing else.
// Header-only and `constexpr` throughout, exactly like `measure.h` - the two
// halves of "what does this escape mean" end up the same kind of thing.
//
// THE READING IS THE BLITTER'S, EXACTLY. Every mapping here is the inverse of a
// line in `blit.cpp`'s `set_pen`, including the ones an SGR table would spell
// differently:
//
//   * `ESC[0m` is `style{}` - the terminal's own colours, no attributes. That
//     is literally what `reset_pen` means by the same bytes. It is NOT "back to
//     the caller's `prompt_pen`": a theme author writes literal escapes and
//     expects literal terminal behaviour, and a reader whose `0` meant
//     something the writer's `0` does not is a second dialect, not an inverse.
//     `prompt_pen` is the pen the prompt starts in; its own bytes take it from
//     there.
//   * `4:3` is undercurl, because that is what the blitter emits for #97's one
//     opportunistic attribute. A reader that only knew `ESC[4m` would round-trip
//     undercurl into a plain underline.
//   * An indexed colour stays indexed and a truecolour stays 24-bit. `surface.h`
//     is explicit that these are different statements and that the surface never
//     quantizes; reading `38;5;4` into an RGB triple would substitute our guess
//     for the user's palette on the way IN, which is the same mistake at the
//     other end.
//
// A MALFORMED OR UNKNOWN PARAMETER IS IGNORED AND THE SEQUENCE KEEPS PARSING
// (N-4: malformed bytes degrade, they do not abort or stall). `ESC[1;53;31m`
// leaves bold and red set and drops the overline nobody here implements;
// `ESC[38;5;999;1m` leaves the foreground alone and still sets bold. The
// alternative - abandoning the rest of the sequence at the first parameter we do
// not know - would make a prompt's colours depend on whether some unrelated
// terminal feature appeared earlier in the same `ESC[...m`, which is a worse
// failure than dropping the one parameter that caused it.

#include "leshper/surface.h"
#include "substrate/numeric.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lesh::leshper {

namespace sgr_detail {

// Whether `sequence` - one whole escape, as #114's `escape_length` measured it -
// is an SGR sequence this file reads: `ESC [ parameters m`, with nothing in the
// parameter bytes but digits and the two separators.
//
// A private-use marker (`<`, `=`, `>`, `?`) or an intermediate byte says the
// sequence belongs to somebody else's grammar, and reading its parameters as
// colours would be inventing a meaning for bytes nothing here emits. Those
// sequences keep the treatment #114 and #123 already gave them: consumed whole,
// zero width, no pen.
[[nodiscard]] constexpr bool is_sgr(std::string_view sequence) noexcept {
	if (sequence.size() < 3 || sequence.front() != '\x1B' || sequence[1] != '['
	    || sequence.back() != 'm')
		return false;
	for (std::size_t i = 2; i + 1 < sequence.size(); ++i) {
		const char one = sequence[i];
		if (!(one >= '0' && one <= '9') && one != ';' && one != ':')
			return false;
	}
	return true;
}

// The parameter bytes of an SGR sequence, read one field at a time and never
// stored. No vector, no fixed array, no maximum: the two extended-colour forms
// are the only ones that read more than one field, and each reads exactly as
// many as it needs from a cursor the caller already holds. Which is also what
// keeps `lay_out` allocation-free with this in it.
class parameters {
public:
	constexpr explicit parameters(std::string_view fields) noexcept : _fields(fields) {}

	// The next parameter, or false once the sequence has none left. `sub` says
	// it arrived after a `:` rather than a `;` - a sub-parameter qualifying the
	// field before it, which is how the blitter spells undercurl (`4:3`).
	//
	// An EMPTY field is 0, which is ECMA-48's default parameter and what makes
	// `ESC[m` a reset: the caller cannot tell "no digits" from "the digit 0"
	// here, and must not, because the terminal cannot either.
	constexpr bool next(unsigned& value, bool& sub) noexcept {
		if (_done)
			return false;
		sub = _sub;

		// The digits are the one numeric parser's (substrate/numeric.h), at the
		// site the decoder already named for exactly these bytes: `ESC[1;5C` and
		// `ESC[1;31m` are the same grammar position, so `csi_parameter` is the
		// row and no new one is needed. A parameter past the limit clamps there
		// and lands in the ignored range below, which is what a malformed one
		// should do.
		const std::uint64_t limit =
			static_cast<std::uint64_t>(policy_for(numeric_site::csi_parameter).high);
		const digit_run run = scan_digits(_fields.substr(_at), 10, limit);
		value = static_cast<unsigned>(run.value);
		_at += run.consumed;

		if (_at >= _fields.size()) {
			_done = true;
			return true;
		}
		const char separator = _fields[_at];
		if (separator != ';' && separator != ':') {
			// `is_sgr` already refused everything else, so this is unreachable
			// on a sequence that got here - and stopping rather than looping is
			// what makes that true by construction rather than by argument.
			_done = true;
			return true;
		}
		_sub = separator == ':';
		++_at;
		return true;
	}

	constexpr bool next(unsigned& value) noexcept {
		bool sub = false;
		return next(value, sub);
	}

private:
	std::string_view _fields;
	std::size_t _at = 0;
	bool _sub = false;
	bool _done = false;
};

// `4 : n`, the underline-style sub-parameter. Only 3 is a distinct attribute at
// #97's floor; the double, dotted and dashed forms degrade to a plain underline
// because that is what the surface can hold, and 0 is underline off.
[[nodiscard]] constexpr attribute underline_style(unsigned which) noexcept {
	switch (which) {
		case 0:  return attribute::none;
		case 3:  return attribute::undercurl;
		default: return attribute::underline;
	}
}

// The extended-colour forms after a 38 or a 48: `5 ; n` for a palette slot,
// `2 ; r ; g ; b` for 24 bits. `out` is left alone unless a whole well-formed
// form was read, so a truncated or out-of-range one drops the colour and
// nothing else - the parameters it did consume are gone either way, which is
// correct: they were that colour's, not the next parameter's.
constexpr void read_extended_color(parameters& params, color& out) noexcept {
	unsigned form = 0;
	if (!params.next(form))
		return;

	if (form == 5) {
		unsigned index = 0;
		if (!params.next(index) || index > 255)
			return;
		out = color::of_index(static_cast<std::uint8_t>(index));
		return;
	}
	if (form == 2) {
		unsigned channel[3] = {0, 0, 0};
		for (unsigned& one : channel)
			if (!params.next(one) || one > 255)
				return;
		out = color::of_rgb(static_cast<std::uint8_t>(channel[0]),
		                    static_cast<std::uint8_t>(channel[1]),
		                    static_cast<std::uint8_t>(channel[2]));
		return;
	}
	// 0, 1, 3 and 4 are T.416 forms - implementation-defined, transparent, CMY,
	// CMYK - that nothing at this floor emits and the cell cannot hold.
}

} // namespace sgr_detail

// `pen`, as `sequence` leaves it. `sequence` is one whole escape as
// `grapheme::measure_detail::escape_length` measured it; anything that is not
// SGR answers with `pen` unchanged, which is what lets a caller apply this to
// every escape it skips and keep exactly #114's treatment of the rest.
//
// Pure, allocation-free and `constexpr`: it reads its arguments and the
// constexpr policy table in `numeric.h`, and touches nothing else.
[[nodiscard]] constexpr style apply_sgr(std::string_view sequence, style pen) noexcept {
	if (!sgr_detail::is_sgr(sequence))
		return pen;

	sgr_detail::parameters params{sequence.substr(2, sequence.size() - 3)};
	unsigned code = 0;
	bool sub = false;
	unsigned qualified = 0;   // the top-level parameter a `:` field belongs to

	while (params.next(code, sub)) {
		if (sub) {
			// A sub-parameter qualifies the field before it. `4:n` is the only
			// one the floor speaks - the extended-colour forms read their own
			// sub-parameters above, separator and all - so anything else is
			// dropped rather than mistaken for a parameter in its own right.
			if (qualified == 4)
				pen.attrs = (pen.attrs & ~(attribute::underline | attribute::undercurl))
				          | sgr_detail::underline_style(code);
			continue;
		}
		qualified = code;

		switch (code) {
			// The blitter's `reset_pen`, read backwards: these bytes and the
			// default style are the same statement in the two directions.
			case 0:  pen = style{}; break;

			case 1:  pen.attrs |= attribute::bold; break;
			case 2:  pen.attrs |= attribute::dim; break;
			case 3:  pen.attrs |= attribute::italic; break;
			// A plain `4` is a plain underline, and it REPLACES an undercurl
			// rather than joining it: the two are one attribute at the
			// terminal, and holding both would emit `4:3` for a prompt that
			// asked for `4`.
			case 4:  pen.attrs = (pen.attrs & ~attribute::undercurl) | attribute::underline;
			         break;
			case 7:  pen.attrs |= attribute::reverse; break;
			case 9:  pen.attrs |= attribute::strikethrough; break;

			// 22 clears bold AND dim, which is not this file being lossy - it is
			// the reason `set_pen` resets and restates rather than turning one
			// attribute off. The off-switches do not partition the attributes in
			// either direction.
			case 22: pen.attrs = pen.attrs & ~(attribute::bold | attribute::dim); break;
			case 23: pen.attrs = pen.attrs & ~attribute::italic; break;
			case 24: pen.attrs = pen.attrs & ~(attribute::underline | attribute::undercurl);
			         break;
			case 27: pen.attrs = pen.attrs & ~attribute::reverse; break;
			case 29: pen.attrs = pen.attrs & ~attribute::strikethrough; break;

			case 38: sgr_detail::read_extended_color(params, pen.fg); break;
			case 39: pen.fg = color::of_default(); break;
			case 48: sgr_detail::read_extended_color(params, pen.bg); break;
			case 49: pen.bg = color::of_default(); break;

			default:
				// The palette's first sixteen slots, which SGR spells in four
				// ranges rather than one. They stay INDEXED - the user may have
				// redefined slot 2, and resolving it to a green of our choosing
				// here would be the quantization `surface.h` refuses at the
				// other end.
				if (code >= 30 && code <= 37)
					pen.fg = color::of_index(static_cast<std::uint8_t>(code - 30));
				else if (code >= 90 && code <= 97)
					pen.fg = color::of_index(static_cast<std::uint8_t>(code - 90 + 8));
				else if (code >= 40 && code <= 47)
					pen.bg = color::of_index(static_cast<std::uint8_t>(code - 40));
				else if (code >= 100 && code <= 107)
					pen.bg = color::of_index(static_cast<std::uint8_t>(code - 100 + 8));
				// Anything else - 5 blink, 53 overline, a parameter that
				// overflowed - is ignored, and the sequence keeps parsing.
				break;
		}
	}
	return pen;
}

} // namespace lesh::leshper
