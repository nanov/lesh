#pragma once

// What an interned semantic style id LOOKS like (#93, #124, #141, F-21).
//
// #124 fixed a VOCABULARY and refused to name a colour: `command.builtin` says
// what a span MEANS, and the theme decides what it looks like. This file is the
// smallest thing that can be a theme - a table from name to pen, and a lookup
// from the interned id to the pen - so that the trail from a reactor's emit to a
// coloured cell is complete. It is deliberately NOT the theme system: #101 owns
// configuration, no ticket has decided a user-facing theme table, and what is
// here is the DEFAULT that a real one replaces by replacing one array.
//
// WHY ITS OWN HEADER, and not `registry.{h,cpp}` beside the interning. The
// interning is an ABI service - a binding asks for an id by name and the
// registry hands one back - and it lives in a header that drags `abi.h`,
// `effect.h`, `shell_knowledge.h` and the three handle definitions with it.
// `layout.cpp` must not include that: layout produces a surface and the blitter
// consumes one, and they are siblings over `surface.h` rather than a stack.
// This is the same shape and the same argument `sgr.h` was given in #131 - a
// header between the rings, depending on `surface.h` for the `style` vocabulary
// and on nothing else - and it keeps the one file that must read as geometry
// from also being the file that knows what green is.
//
// TRUECOLOR-VALUED, AND QUANTIZED NOWHERE HERE (#97). The pens below are 24-bit
// because that is what a theme author authors; `blit.cpp` owns the 256-colour
// downmap and owns it exactly once, and a table that pre-quantized would mean a
// theme is authored once per terminal instead of once.
//
// AN ID THIS TABLE DOES NOT KNOW IS NOT AN ERROR AND IS NOT BLACK. It leaves the
// pen it was asked to paint over exactly as it was - see `over` - so a plugin
// that interns `rainbow.sparkle` and emits it gets ordinary text rather than the
// terminal's default colour substituted for the caller's. That is the same
// degradation `lesh_emit_virtual_text_styled` already promises for an id nobody
// interned.

#include "leshper/surface.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::leshper {

// One row of the default theme.
struct theme_entry {
	std::string_view name;
	style pen;
};

// The default mapping, and it is MINIMAL on purpose.
//
// #124's vocabulary is sixteen names plus #133's `suggestion`, and this table
// collapses several of them onto one pen: the four "the shell would run this"
// kinds are all green, the three string kinds are all yellow, the four expansion
// kinds are all cyan. That is not the table being unfinished - it is the table
// declining to invent a palette. The names stay distinct because they MEAN
// distinct things and a theme that wants a different green for an alias changes
// one row; a default that shipped seventeen colours would be a design nobody
// chose being hard to undo.
//
// Legibility is the only criterion applied: every pen is a mid-tone that reads
// on a light background and on a dark one, and no pen sets a background.
inline constexpr theme_entry default_theme[] = {
	// The shell would run it.
	{"command.path", style{color::of_rgb(0x5F, 0xAF, 0x5F)}},
	{"command.builtin", style{color::of_rgb(0x5F, 0xAF, 0x5F)}},
	{"command.function", style{color::of_rgb(0x5F, 0xAF, 0x5F)}},
	{"command.alias", style{color::of_rgb(0x5F, 0xAF, 0x5F)}},
	// It would not.
	{"command.unknown", style{color::of_rgb(0xD7, 0x5F, 0x5F)}},

	{"keyword", style{color::of_rgb(0xAF, 0x87, 0xD7)}},
	{"comment", style{color::of_rgb(0x80, 0x80, 0x80)}},

	{"string.single", style{color::of_rgb(0xD7, 0xAF, 0x5F)}},
	{"string.double", style{color::of_rgb(0xD7, 0xAF, 0x5F)}},
	{"string.ansi_c", style{color::of_rgb(0xD7, 0xAF, 0x5F)}},

	{"expansion.parameter", style{color::of_rgb(0x5F, 0xAF, 0xD7)}},
	{"expansion.command", style{color::of_rgb(0x5F, 0xAF, 0xD7)}},
	{"expansion.arithmetic", style{color::of_rgb(0x5F, 0xAF, 0xD7)}},
	{"expansion.tilde", style{color::of_rgb(0x5F, 0xAF, 0xD7)}},

	{"redirect.target", style{color::of_rgb(0x5F, 0x87, 0xD7)}},

	// The one row that is not a colour alone. An error and an unknown command
	// are both red, and they are not the same statement: the undercurl is what
	// separates "the shell cannot parse this" from "the shell cannot find this",
	// and #97 made it the one opportunistic attribute precisely so a terminal
	// without it degrades to a plain underline rather than to nothing.
	{"error.syntax",
	 style{color::of_rgb(0xD7, 0x5F, 0x5F), color::of_default(), attribute::undercurl}},

	// #133's, and the only one whose job is to be UNOBTRUSIVE: a suggestion the
	// user has not accepted must not read as text they typed.
	{"suggestion", style{color::of_rgb(0x6C, 0x6C, 0x6C)}},
};

// The default pen for a semantic name, or null when this table has no opinion.
[[nodiscard]] constexpr const theme_entry* find_default_style(std::string_view name) noexcept {
	for (const theme_entry& one : default_theme) {
		if (one.name == name)
			return &one;
	}
	return nullptr;
}

// The interned id, resolved.
//
// A VECTOR INDEXED BY ID and not a map, because the id IS an index: the registry
// interns by appending to a vector and hands back the position (registry.cpp),
// so the lookup on the render path is a bounds check and a load. Nothing here
// keeps the names - `sync` reads them once, when a new one appears, and the
// render walk never sees a string.
class style_table {
public:
	// Whether this table knows what `id` looks like. False for id 0, which is
	// abi.h's LESH_STYLE_NONE and is never a name, and false for an id that was
	// interned under a name the theme has no row for.
	[[nodiscard]] bool knows(std::uint32_t id) const noexcept {
		return id != 0 && id < _pens.size() && _pens[id].known;
	}

	// The pen for `id` OVER `under`: the theme's pen when it has one, and
	// otherwise `under` untouched. The whole reason a caller passes what it was
	// going to paint in - an unknown id degrades to ordinary text rather than to
	// the terminal's default colour.
	[[nodiscard]] style over(std::uint32_t id, style under) const noexcept {
		return knows(id) ? _pens[id].pen : under;
	}

	// Binds one id to one pen. The seam a real theme arrives at: a configured
	// table calls this instead of `sync`, and nothing else in leshper changes.
	void bind(std::uint32_t id, style pen) {
		grow(id);
		_pens[id] = entry{pen, true};
	}

	// Binds every id the registry has interned and this table has not seen yet,
	// through the default table above. `names` is the registry's own style
	// vector, whose index IS the id.
	//
	// INCREMENTAL, because the render path calls it: after the first turn it is
	// a size comparison, and a binding that interns a new name mid-session
	// costs exactly the new names. That is what keeps the repaint pin in
	// allocation_tests.cpp - a constant per repaint - true with a theme in it.
	void sync(const std::vector<std::string>& names) {
		for (std::size_t id = _synced; id < names.size(); ++id) {
			const theme_entry* found = find_default_style(names[id]);
			grow(static_cast<std::uint32_t>(id));
			_pens[id] = entry{found == nullptr ? style{} : found->pen, found != nullptr};
		}
		_synced = names.size();
	}

	void clear() noexcept {
		_pens.clear();
		_synced = 0;
	}

	[[nodiscard]] std::size_t size() const noexcept { return _pens.size(); }

private:
	struct entry {
		style pen;
		bool known = false;
	};

	void grow(std::uint32_t id) {
		if (id >= _pens.size())
			_pens.resize(static_cast<std::size_t>(id) + 1);
	}

	std::vector<entry> _pens;
	std::size_t _synced = 0;
};

} // namespace lesh::leshper
