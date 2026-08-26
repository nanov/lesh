#pragma once

// prmt's spellings, into `surface.h`'s `style` value: `"cyan.bold"`,
// `"#89dceb"`, `"#fff+#333"`.
//
// WHY THIS FILE, WHY NOW. Architecture spec §6.10's last paragraph left the
// string grammar deliberately unbuilt for v1: "the string grammar arrives with
// its first string source ... and lives below leshper then, shared with the
// highlighter's theme (#141) so one parser serves both." That first string
// source has arrived - the template parser and `lesh_prompt_add_style` on the
// ABI, landing beside this ticket - and this is the parser #6.10 promised: one
// grammar, called from both sides, rather than a copy that drifts from #141's
// the first time somebody fixes a bug in only one of them.
//
// WHY IT SITS BESIDE `sgr.h` RATHER THAN IN THE SUBSTRATE. `style` and `color`
// are `surface.h`'s - the renderer's vocabulary, shared with the blitter and
// the differ, and this file adds nothing to that vocabulary. Both of this
// parser's callers, the prompt's string sites and #141's theme, are
// leshper-side; a substrate header exists for code more than one layer needs,
// and nothing below leshper has any use for a colour name. So it is a sibling
// of `sgr.h`, not a citizen of `substrate/`: `sgr.h` turns terminal bytes into
// a `style`, this turns a theme author's bytes into one, and the two headers
// are the same kind of thing at the same address.
//
// LAST COLOR ITEM WINS, AND THAT IS NOT AN OVERSIGHT. prmt lets a theme spell
// `"cyan+#222.dim"` - foreground, then background, then a modifier - and nowhere
// does prmt's grammar require the two colour halves to arrive together or in a
// fixed order. Refusing a second colour item as a conflict would reject specs
// prmt itself accepts; assigning fields as each item is seen and letting a
// later one overwrite an earlier one's slot is the only rule that parses both
// `"cyan+#222"` and `"cyan.blue"` (fg ends up blue) without inventing an error
// prmt never had.
//
// STRICT LOWERCASE, ON PURPOSE. `"CYAN"` is an error, not a color, even though
// nothing stops a case-insensitive compare. Set-time validation - refusing a
// bad spec the moment it is written rather than silently drawing a default
// pen - is the whole reason this parser exists rather than a lenient one that
// falls back on failure: a theme author who typo'd the case gets told, instead
// of a prompt that quietly renders in nobody's colour.
//
// `purple` IS `magenta`. Slot 5 is ANSI magenta everywhere else in this
// codebase (`sgr.h`, `prompt.h`'s `kMagenta`), but prmt spells it `purple`, and
// a theme carried over from prmt should not have to relearn the palette to
// keep working here. The alias costs one extra table row and buys back every
// prmt theme's `+bg` and bare colour items for free.
//
// `undercurl` IS OURS, BEYOND PRMT'S MODIFIER LIST. prmt has no such attribute;
// `surface.h` does (#97, opportunistic, degrading to a plain underline below
// the floor), and a grammar that serves this codebase's theme has to be able to
// ask for what this codebase can draw.
//
// EVERYTHING HERE IS `constexpr`. A theme is validated at parse time, which for
// the compile-time default table (`prompt.h`) means a bad literal spec is a
// build failure rather than a style nobody sees fail; a runtime spec pays
// nothing this function does not already cost as a value computation.

#include "leshper/surface.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lesh::leshper {

// The result of parsing one spec: the style if `ok`, or the offset of the item
// that refused if not. `error_at` is a byte offset INTO THE SPEC - not into the
// failing item - so a caller can point at the exact byte in the theme file or
// config line the author needs to fix; it is 0 both when the spec is empty (the
// legal "no styling" case) and, harmlessly, when it is not needed at all.
struct style_parse {
	style value{};
	bool ok = false;
	std::size_t error_at = 0;   // byte offset of the item that failed; 0 when ok
};

namespace style_grammar_detail {

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept {
	return c >= '0' && c <= '9';
}

// A hex digit's value, or -1. Both cases (upper and lower) are prmt's; #89dceb
// and #89DCEB name the same colour.
[[nodiscard]] constexpr int hex_nibble(char c) noexcept {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

// The 8 ANSI names, ANSI order, `purple` riding along as slot 5's second
// spelling (see the file header). `bright-<name>` reuses this same table with
// +8 added by the caller, so `purple` is `bright-purple`'s base too.
struct named_color {
	std::string_view name;
	std::uint8_t index;
};
inline constexpr named_color kNamedColors[] = {
	{"black", 0}, {"red", 1}, {"green", 2}, {"yellow", 3},
	{"blue", 4}, {"magenta", 5}, {"purple", 5}, {"cyan", 6}, {"white", 7},
};

// One colour segment: a name, `purple`, `bright-<name>`, a decimal 0-255, or a
// `#rgb`/`#rrggbb` truecolor spec. Never called on an empty segment - that is
// the "leave this side alone" case and it is the item parser's to handle, not
// this function's, so an empty `seg` here falls through every branch and
// answers false, which is also the correct answer for it.
[[nodiscard]] constexpr bool parse_color(std::string_view seg, color& out) noexcept {
	if (seg.empty())
		return false;

	if (seg.front() == '#') {
		const std::string_view hex = seg.substr(1);
		if (hex.size() == 6) {
			std::uint8_t channel[3];
			for (std::size_t i = 0; i < 3; ++i) {
				const int hi = hex_nibble(hex[i * 2]);
				const int lo = hex_nibble(hex[i * 2 + 1]);
				if (hi < 0 || lo < 0)
					return false;
				channel[i] = static_cast<std::uint8_t>((hi << 4) | lo);
			}
			out = color::of_rgb(channel[0], channel[1], channel[2]);
			return true;
		}
		if (hex.size() == 3) {
			// Each nibble doubled: `#f` is `0xFF`, not `0x0F` - the short form
			// names the same colour a 6-digit spec would if every pair repeated.
			std::uint8_t channel[3];
			for (std::size_t i = 0; i < 3; ++i) {
				const int v = hex_nibble(hex[i]);
				if (v < 0)
					return false;
				channel[i] = static_cast<std::uint8_t>((v << 4) | v);
			}
			out = color::of_rgb(channel[0], channel[1], channel[2]);
			return true;
		}
		return false;
	}

	bool all_digits = true;
	for (const char c : seg)
		if (!is_ascii_digit(c)) {
			all_digits = false;
			break;
		}
	if (all_digits) {
		// More than 3 digits cannot be in [0, 255] and is refused before it is
		// even accumulated - "0001" is not a leading-zero spelling of 1, it is
		// four digits, which this grammar never accepts regardless of value.
		if (seg.size() > 3)
			return false;
		unsigned value = 0;
		for (const char c : seg) {
			value *= 10;
			value += static_cast<unsigned>(c - '0');
		}
		if (value > 255)
			return false;
		out = color::of_index(static_cast<std::uint8_t>(value));
		return true;
	}

	constexpr std::string_view kBrightPrefix = "bright-";
	std::string_view name = seg;
	unsigned offset = 0;
	if (seg.size() >= kBrightPrefix.size() && seg.substr(0, kBrightPrefix.size()) == kBrightPrefix) {
		name = seg.substr(kBrightPrefix.size());
		offset = 8;
	}
	for (const named_color& candidate : kNamedColors)
		if (name == candidate.name) {
			out = color::of_index(static_cast<std::uint8_t>(candidate.index + offset));
			return true;
		}
	return false;
}

// One modifier word to the attribute bit it sets. Strict equality only - there
// is no abbreviation and no case folding, for the same reason a colour name
// gets none (see the file header).
[[nodiscard]] constexpr bool parse_modifier(std::string_view seg, attribute& out) noexcept {
	if (seg == "bold")          { out = attribute::bold; return true; }
	if (seg == "dim")           { out = attribute::dim; return true; }
	if (seg == "italic")        { out = attribute::italic; return true; }
	if (seg == "underline")     { out = attribute::underline; return true; }
	if (seg == "undercurl")     { out = attribute::undercurl; return true; }
	if (seg == "strikethrough") { out = attribute::strikethrough; return true; }
	if (seg == "reverse")       { out = attribute::reverse; return true; }
	return false;
}

// One item - the text between two dots. Never called on an empty item; the
// caller refuses those itself so this function's every path can assume it has
// at least one byte to look at.
[[nodiscard]] constexpr bool parse_item(std::string_view item, style& value) noexcept {
	const std::size_t plus = item.find('+');
	if (plus != std::string_view::npos) {
		const std::string_view fg_spec = item.substr(0, plus);
		const std::string_view bg_spec = item.substr(plus + 1);
		// A bare "+" or a "fg+" with nothing after it names no background at
		// all, which is not "leave it alone" (that is what an item with no `+`
		// is for) - it is a malformed item.
		if (bg_spec.empty())
			return false;
		if (!fg_spec.empty()) {
			color fg;
			if (!parse_color(fg_spec, fg))
				return false;
			value.fg = fg;
		}
		color bg;
		if (!parse_color(bg_spec, bg))
			return false;
		value.bg = bg;
		return true;
	}

	color fg;
	if (parse_color(item, fg)) {
		value.fg = fg;
		return true;
	}

	attribute bit;
	if (parse_modifier(item, bit)) {
		value.attrs |= bit;
		return true;
	}

	return false;
}

} // namespace style_grammar_detail

// `spec`, split on `.`, each item folded into `value` in order - a colour item
// sets `fg` and/or `bg`, a modifier item sets one attribute bit, and a later
// colour item overwrites an earlier one's assignment rather than conflicting
// with it (see the file header). An empty spec is the legal "no styling" and
// answers `style{}`; an empty item - leading, trailing or doubled dot - is not
// legal and is refused like any other malformed item.
//
// Parsing stops at the first item that fails: `value` past that point is
// whatever partial assignments preceding items already made, which is exactly
// as unspecified as the contract says - `ok` is what a caller must check
// before touching `value`, not the other way round.
[[nodiscard]] constexpr style_parse parse_style(std::string_view spec) noexcept {
	style_parse result;
	if (spec.empty()) {
		result.ok = true;
		return result;
	}

	std::size_t pos = 0;
	for (;;) {
		const std::size_t dot = spec.find('.', pos);
		const std::size_t end = dot == std::string_view::npos ? spec.size() : dot;
		const std::string_view item = spec.substr(pos, end - pos);

		if (item.empty() || !style_grammar_detail::parse_item(item, result.value)) {
			result.ok = false;
			result.error_at = pos;
			return result;
		}

		if (dot == std::string_view::npos)
			break;
		pos = dot + 1;
	}

	result.ok = true;
	result.error_at = 0;
	return result;
}

// ---------------------------------------------------------------------------
// Selftests
// ---------------------------------------------------------------------------

// The same discipline as `prompt.h`'s `selftest`: these are proofs the compiler
// checks, not a runtime suite. They stay within `surface.h`'s vocabulary only -
// the SGR round trip through `sgr.h` and `prompt.h`'s `emit_sgr` lives in
// `tests/unit/leshper_style_grammar_tests.cpp`, where all three headers may
// meet without this one having to include `prompt.h` and risk the cycle #157's
// template-parser ticket would otherwise close.
namespace style_grammar_selftest {

static_assert(parse_style("").ok);
static_assert(parse_style("").value == style{});

static_assert(parse_style("cyan").ok);
static_assert(parse_style("cyan").value.fg == color::of_index(6));

static_assert(parse_style("purple").value.fg == color::of_index(5));
static_assert(parse_style("magenta").value.fg == color::of_index(5));

static_assert(parse_style("bright-red").value.fg == color::of_index(9));

static_assert(parse_style("7").ok);
static_assert(parse_style("7").value.fg == color::of_index(7));
static_assert(parse_style("255").ok);
static_assert(parse_style("255").value.fg == color::of_index(255));

static_assert(parse_style("#89dceb").ok);
static_assert(parse_style("#89dceb").value.fg == color::of_rgb(0x89, 0xdc, 0xeb));
static_assert(parse_style("#fff").value.fg == color::of_rgb(255, 255, 255));

static_assert(parse_style("cyan.bold").ok);
static_assert(parse_style("cyan.bold").value.fg == color::of_index(6));
static_assert(has(parse_style("cyan.bold").value.attrs, attribute::bold));

static_assert(parse_style("red.dim.italic").ok);
static_assert(has(parse_style("red.dim.italic").value.attrs, attribute::dim));
static_assert(has(parse_style("red.dim.italic").value.attrs, attribute::italic));

static_assert(parse_style("cyan.bold.underline").ok);
static_assert(has(parse_style("cyan.bold.underline").value.attrs, attribute::bold));
static_assert(has(parse_style("cyan.bold.underline").value.attrs, attribute::underline));

static_assert(parse_style("#ffffff+#333333").ok);
static_assert(parse_style("#ffffff+#333333").value.fg == color::of_rgb(255, 255, 255));
static_assert(parse_style("#ffffff+#333333").value.bg == color::of_rgb(0x33, 0x33, 0x33));

static_assert(parse_style("+blue").ok);
static_assert(parse_style("+blue").value.bg == color::of_index(4));
static_assert(parse_style("+blue").value.fg == color::of_default());

static_assert(parse_style("cyan+#222.dim").ok);
static_assert(parse_style("cyan+#222.dim").value.fg == color::of_index(6));
static_assert(parse_style("cyan+#222.dim").value.bg == color::of_rgb(0x22, 0x22, 0x22));
static_assert(has(parse_style("cyan+#222.dim").value.attrs, attribute::dim));

// Last colour item wins.
static_assert(parse_style("red.blue").ok);
static_assert(parse_style("red.blue").value.fg == color::of_index(4));

static_assert(!parse_style("CYAN").ok);
static_assert(parse_style("CYAN").error_at == 0);

static_assert(!parse_style("cyan..bold").ok);
static_assert(parse_style("cyan..bold").error_at == 5);

static_assert(!parse_style("cyan.blod").ok);
static_assert(parse_style("cyan.blod").error_at == 5);

static_assert(!parse_style("#12345").ok);
static_assert(parse_style("#12345").error_at == 0);

static_assert(!parse_style("256").ok);
static_assert(parse_style("256").error_at == 0);

static_assert(!parse_style("+").ok);
static_assert(parse_style("+").error_at == 0);

static_assert(!parse_style("bright-").ok);
static_assert(parse_style("bright-").error_at == 0);

} // namespace style_grammar_selftest

} // namespace lesh::leshper
