#pragma once

#include "substrate/grapheme.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace lesh::leshper {

// The surface: the grid of styled cells leshper renders into (CONTEXT.md,
// A-6, F-37).
//
// Nothing here knows what an escape sequence is. That is the whole point:
// N-3 says the renderer is tested by asserting exact cell grids and never
// golden byte streams, so the type the editor renders into must be a value
// with no terminal in it. blit.h is the only file that turns one of these
// into bytes, and #97's quantization lives there and only there.
//
// This is NOT a layout engine. `write` clips at the right edge and never
// wraps; there is no line breaking, no reflow, no prompt geometry. F-37's
// layout-as-value and F-38's reflow are the next ring and they build ON this,
// deciding WHICH cells go where. The seam they need is the one already here:
// a surface is a value, comparable and copyable, so a layout is a function
// that returns one.

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

// Which of the three colour vocabularies a `color` is speaking (#97).
enum class color_kind : std::uint8_t {
	terminal_default,  // SGR 39 / 49: whatever the user's terminal calls fg/bg
	indexed,           // 0-255, already a terminal index; passes through as-is
	truecolor,         // 24-bit, quantized by the blitter when the terminal lacks it
};

// One colour, tagged (#97 decision 4).
//
// The surface ALWAYS stores what the theme authored, at full precision. It
// never quantizes, because quantizing here would mean a theme is authored
// once per terminal instead of once, and would put the 256-colour downmap in
// two places the first time somebody needed it in a test. The blitter owns
// that conversion at emit time.
//
// `indexed` is not "truecolor we have not converted yet" - it is a different
// statement. An indexed colour names a slot in the user's palette, which the
// user may have redefined; resolving it to RGB would be substituting our
// guess for their choice. So it passes through both depths untouched.
//
// Five bytes, alignment one, no padding: see the cell's static_assert.
struct color {
	color_kind kind = color_kind::terminal_default;
	std::uint8_t index = 0;                  // meaningful only when kind == indexed
	std::uint8_t r = 0, g = 0, b = 0;        // meaningful only when kind == truecolor

	[[nodiscard]] static constexpr color of_default() noexcept { return color{}; }

	[[nodiscard]] static constexpr color of_index(std::uint8_t which) noexcept {
		color made;
		made.kind = color_kind::indexed;
		made.index = which;
		return made;
	}

	[[nodiscard]] static constexpr color of_rgb(std::uint8_t red, std::uint8_t green,
	                                            std::uint8_t blue) noexcept {
		color made;
		made.kind = color_kind::truecolor;
		made.r = red;
		made.g = green;
		made.b = blue;
		return made;
	}

	friend constexpr bool operator==(color, color) noexcept = default;
};

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

// The attribute bit-set #97 fixed. Seven bits, one byte, no growth planned:
// this is the set every terminal at the floor understands, plus undercurl,
// which is opportunistic and degrades to a plain underline.
enum class attribute : std::uint8_t {
	none          = 0,
	bold          = 1u << 0,
	dim           = 1u << 1,
	italic        = 1u << 2,
	underline     = 1u << 3,
	undercurl     = 1u << 4,  // opportunistic (#97); plain underline below it
	strikethrough = 1u << 5,
	reverse       = 1u << 6,
};

[[nodiscard]] constexpr attribute operator|(attribute a, attribute b) noexcept {
	return static_cast<attribute>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr attribute operator&(attribute a, attribute b) noexcept {
	return static_cast<attribute>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr attribute operator~(attribute a) noexcept {
	return static_cast<attribute>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(a)) & 0x7F);
}
constexpr attribute& operator|=(attribute& a, attribute b) noexcept { return a = a | b; }
[[nodiscard]] constexpr bool has(attribute set, attribute one) noexcept {
	return (set & one) != attribute::none;
}

// Everything about a cell that is not its glyph (#97: two tagged colours plus
// the attribute bits). Eleven bytes, alignment one.
struct style {
	color fg = color::of_default();
	color bg = color::of_default();
	attribute attrs = attribute::none;

	friend constexpr bool operator==(const style&, const style&) noexcept = default;
};

// ---------------------------------------------------------------------------
// The grapheme reference
// ---------------------------------------------------------------------------

// A cell's glyph, by reference (#97: "grapheme ref", zero per-cell allocation).
//
// A grapheme cluster is up to a few dozen bytes - a flag is eight, a
// four-person ZWJ family with skin tones is thirty-five - and a cell may not
// own that. So the bytes live once in a `cluster_pool` and the cell holds a
// 32-bit id into it.
//
// Two ids are fixed by construction rather than by interning, because the
// differ and the default cell both need them before any pool exists:
//
//   0            the CONTINUATION marker - the second column of a two-column
//                cluster. It has no bytes of its own and the blitter never
//                emits it; writing the wide cluster already moved the
//                terminal's cursor across it.
//   1 + byte     the single-byte cluster `byte`, for every ASCII byte. This
//                is what makes a blank cell a compile-time constant and an
//                ASCII glyph a lookup with no hashing, which is most of a
//                shell prompt.
//
// The pool seeds itself to agree with both, and asserts that it did.
class grapheme_ref {
public:
	constexpr grapheme_ref() noexcept = default;

	[[nodiscard]] static constexpr grapheme_ref continuation() noexcept {
		return grapheme_ref{0};
	}
	[[nodiscard]] static constexpr grapheme_ref of_ascii(char byte) noexcept {
		return grapheme_ref{1u + static_cast<std::uint32_t>(static_cast<unsigned char>(byte))};
	}
	[[nodiscard]] static constexpr grapheme_ref blank() noexcept { return of_ascii(' '); }

	// The interning side's constructor. Only `cluster_pool` should call it; it
	// is public because a private friend would buy nothing a comment does not.
	[[nodiscard]] static constexpr grapheme_ref from_id(std::uint32_t id) noexcept {
		return grapheme_ref{id};
	}

	[[nodiscard]] constexpr std::uint32_t id() const noexcept { return _id; }
	[[nodiscard]] constexpr bool is_continuation() const noexcept { return _id == 0; }

	friend constexpr bool operator==(grapheme_ref, grapheme_ref) noexcept = default;

private:
	explicit constexpr grapheme_ref(std::uint32_t id) noexcept : _id(id) {}

	std::uint32_t _id = grapheme_ref::blank_id;

	static constexpr std::uint32_t blank_id = 1u + static_cast<std::uint32_t>(' ');
};

// Where cluster bytes live, once each (ADR-0007: owned here, freed here).
//
// ONE pool backs every surface that will be diffed against another, and that
// is the price of #97's "memcmp-comparable for the differ": two cells are the
// same cell when their bytes are equal, and the only way a 32-bit id can carry
// that is for equal clusters to intern to equal ids. A per-surface pool would
// make the differ compare ids from two different address spaces, which is a
// silent wrong answer rather than a compile error - so the pool is a separate
// object a caller holds and hands to both surfaces.
//
// The renderer holds one for the editor's lifetime. It grows to the set of
// distinct non-ASCII clusters that have ever been on screen, which is bounded
// by what a human has typed, and it never shrinks - a screenful of flags is a
// few hundred bytes.
class cluster_pool {
public:
	cluster_pool();

	// Copying would leave the map's keys pointing at the original's bytes.
	// Moving is fine: a deque's elements do not move with the deque.
	cluster_pool(const cluster_pool&) = delete;
	cluster_pool& operator=(const cluster_pool&) = delete;
	cluster_pool(cluster_pool&&) = default;
	cluster_pool& operator=(cluster_pool&&) = default;

	// The id for `cluster`, interning it if this is the first sighting. Empty
	// input is the continuation marker. ASCII single bytes never touch the map.
	[[nodiscard]] grapheme_ref intern(std::string_view cluster);

	// The bytes behind a ref, or empty for the continuation marker. Borrowed
	// from the pool, so it lives exactly as long as the pool does.
	[[nodiscard]] std::string_view cluster_of(grapheme_ref ref) const noexcept;

	[[nodiscard]] std::size_t size() const noexcept { return _clusters.size(); }

private:
	// A deque and not a vector, and that is load-bearing. The map below is
	// keyed by a view of the stored bytes, and for a cluster short enough to
	// live in a std::string's small-string buffer those bytes ARE the element -
	// so a vector's reallocation would move them and leave every key in the map
	// dangling. A deque never moves an element that is already in it.
	std::deque<std::string> _clusters;
	std::unordered_map<std::string_view, std::uint32_t> _by_bytes;
};

// ---------------------------------------------------------------------------
// The cell
// ---------------------------------------------------------------------------

// One terminal cell (#97 decision 4): a grapheme ref, a width, two tagged
// colours and the attribute bits. Sixteen bytes, trivially copyable, no
// padding, no allocation, no ownership.
struct cell {
	grapheme_ref glyph;                 // 4
	style pen;                          // 11
	std::uint8_t width = 1;             // 1: columns this cell's cluster occupies

	friend constexpr bool operator==(const cell&, const cell&) noexcept = default;
};

// #97's constraint, made a build failure rather than a comment. Unique object
// representations is exactly "memcmp answers the same question as ==": no
// padding bytes, no two bit patterns meaning the same value. The differ walks
// rows with std::equal over these, and a padding hole would make two identical
// rows compare different and repaint the screen every keystroke.
static_assert(sizeof(cell) == 16);
static_assert(std::is_trivially_copyable_v<cell>);
static_assert(std::has_unique_object_representations_v<cell>);

// The cell every surface starts and clears to: a blank, unstyled, one column.
inline constexpr cell blank_cell{};

// ---------------------------------------------------------------------------
// The surface
// ---------------------------------------------------------------------------

// Where the terminal's cursor should be once this surface is on screen.
//
// Part of the surface rather than beside it because it is part of what the
// screen looks like, and the blitter's contract is "leave the terminal exactly
// what `desired` says" - a cursor left where the last write happened is a
// cursor in the wrong place.
struct cursor_placement {
	std::uint16_t row = 0;
	std::uint16_t column = 0;
	bool visible = true;

	friend constexpr bool operator==(const cursor_placement&,
	                                 const cursor_placement&) noexcept = default;
};

class surface {
public:
	surface() = default;
	surface(std::uint16_t columns, std::uint16_t rows);

	[[nodiscard]] std::uint16_t columns() const noexcept { return _columns; }
	[[nodiscard]] std::uint16_t rows() const noexcept { return _rows; }

	// Resizes and clears. Deliberately not "resizes and preserves": F-38 owns
	// what a resize does to content, and a half-answer here would be the thing
	// its ticket has to unpick.
	void resize(std::uint16_t columns, std::uint16_t rows);

	// Every cell back to `blank_cell`. The cursor placement is left alone.
	void clear();

	[[nodiscard]] const cell& at(std::uint16_t row, std::uint16_t column) const noexcept;
	[[nodiscard]] cell& at(std::uint16_t row, std::uint16_t column) noexcept;

	[[nodiscard]] cursor_placement& cursor() noexcept { return _cursor; }
	[[nodiscard]] const cursor_placement& cursor() const noexcept { return _cursor; }

	// Paints `text` starting at (row, column) and answers the column one past
	// the last one it touched.
	//
	// This is where #108's seam is consumed and the only place in the renderer
	// that asks Unicode anything: clusters come from `next_boundary`, columns
	// from `cluster_width` under `policy`. Pass a different policy and a CJK
	// ambiguous character changes width without anything here changing - which
	// is all "leave the seam, do not build it" asks for. There is no config
	// surface and this ticket does not add one.
	//
	// Three rules, each of which is a decision:
	//
	//   Clipping, not wrapping. Text that runs past the right edge stops, and a
	//   two-column cluster that would straddle the edge is not written at all -
	//   the columns it would have covered are left as they were. Where a line
	//   breaks is layout (F-37), and layout is the next ring.
	//
	//   Controls are not paintable. A control byte in the stream would corrupt
	//   the blitter's output, and HOW a control is displayed - `^C`, a tab
	//   expanded to a stop - is an editor decision, not a renderer one. The
	//   surface takes printable text and drops anything else.
	//
	//   A zero-width cluster joins the cell before it. A combining mark after a
	//   base is already inside that base's cluster and never reaches this case;
	//   what does is a mark with no base, which the terminal will attach to
	//   whatever precedes it. Attaching it here keeps the invariant that a
	//   column index IS a screen column - the one property the differ, the
	//   blitter and every test depend on.
	std::uint16_t write(cluster_pool& pool, std::uint16_t row, std::uint16_t column,
	                    std::string_view text, const style& pen,
	                    const grapheme::width_policy& policy = grapheme::default_width_policy);

	friend bool operator==(const surface& a, const surface& b) noexcept {
		return a._columns == b._columns && a._rows == b._rows && a._cursor == b._cursor
		    && a._cells == b._cells;
	}

private:
	std::uint16_t _columns = 0;
	std::uint16_t _rows = 0;
	cursor_placement _cursor;
	std::vector<cell> _cells;
};

} // namespace lesh::leshper
