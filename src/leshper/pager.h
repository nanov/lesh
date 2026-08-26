#pragma once

// The pager (#138, F-28 to F-30, architecture spec §6.9).
//
// ONE SURFACE, THREE CLIENTS. #137's resolution is that tab completion, history
// search (F-32) and the autosuggestion candidate view are not three UIs but one
// - fish proved the generalization by reusing its own pager for Ctrl-R - so
// nothing in this file names any of them. A client fills a list, says which span
// of the buffer an accepted candidate replaces, and commits; what comes back is
// a picture and a keymap, identical whichever client asked.
//
// THE THREE HALVES, and why they are in one header:
//
//   THE STATE is in state.h, because A-1 says the state struct holds it and N-3
//   compares it. `pager_state` is data with two helpers and no policy.
//
//   THE POLICY is here: what the grid looks like at a given width, which
//   candidates the filter admits, where the selection goes, what an accepted
//   candidate inserts, and F-30's decision. Pure functions of state - no
//   terminal, no handle, no registry - so every rule below is testable by
//   calling it.
//
//   THE PICTURE is here too, and it is A-6's "own internal surface, unexposed":
//   `render_pager` mints a `surface` of its own and layout COPIES it below the
//   edit line. Not painted straight into the screen, and the difference is the
//   whole of A-6 - the pager's coordinates are the pager's, so a test asserts a
//   grid that has no prompt in it, and the day the §8 surface API exists this
//   is already the object it would hand out.
//
// WHAT IS NOT HERE. Candidate generation - #139 fills the list through the ABI
// doors abi.h declares (`lesh_pager_open` / `_add` / `_commit`), and nothing in
// leshper knows what a `$PATH` is. Descriptions beyond the kind marker (#137
// decision 3). The keymap, which is data in keymap.cpp like every other keymap,
// and the actions, which are in pager_actions.cpp against abi.h alone.

#include "leshper/state.h"
#include "leshper/surface.h"
#include "substrate/grapheme.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lesh::leshper {

// ---------------------------------------------------------------------------
// The kind, and the two questions it answers.
// ---------------------------------------------------------------------------

// The `ls -F` marker drawn after a candidate, or '\0' for the kinds that have
// none. The only description v1 has (#137 decision 3).
[[nodiscard]] constexpr char pager_marker(pager_kind kind) noexcept {
	switch (kind) {
		case pager_kind::directory:
			return '/';
		case pager_kind::executable:
			return '*';
		case pager_kind::symlink:
			return '@';
		case pager_kind::plain:
		case pager_kind::word:
			break;
	}
	return '\0';
}

// What follows an accepted candidate in the buffer (spec §6.9: "directories
// complete with `/` and stay open, files with a space").
//
// The `/` is here rather than in the candidate's own text so that ONE spelling
// of a candidate exists: the filter matches what is shown, the pager inserts
// what the kind says, and a completer cannot hand in a name that displays one
// way and inserts another. "Stay open" is the completer's half - it re-runs
// itself after the accept - and this is the byte that makes re-running find a
// directory rather than a prefix.
[[nodiscard]] constexpr std::string_view pager_trailer(pager_kind kind) noexcept {
	switch (kind) {
		case pager_kind::directory:
			return "/";
		case pager_kind::word:
		case pager_kind::executable:
		case pager_kind::symlink:
			return " ";
		case pager_kind::plain:
			break;
	}
	return {};
}

// The bytes an accepted candidate replaces its span with: the text, then the
// kind's trailer. Appended to `into`, which is cleared first.
void pager_insertion(const pager_candidate& one, std::string& into);

// ---------------------------------------------------------------------------
// The grid.
// ---------------------------------------------------------------------------

// How many rows a pager may take of a terminal `rows` tall.
//
// Half the screen, and never the last row: the edit line is what the user is
// looking at, and a pager that pushed it off the top would be a pager one could
// not complete into. Zero for a one-row terminal, which is the honest answer -
// there is nowhere to put a pager - and the caller then draws none.
[[nodiscard]] std::uint16_t pager_row_budget(std::uint16_t rows) noexcept;

// The geometry of one picture, derived and never stored (#123's rule, kept).
struct pager_grid {
	// Columns of candidates across the screen, at least one.
	std::uint16_t columns = 0;
	// Rows the whole filtered set needs at this width.
	std::uint16_t rows = 0;
	// Rows this picture actually shows, `min(rows, budget)`.
	std::uint16_t visible_rows = 0;
	// The width of one column INCLUDING the gutter that separates it from the
	// next; `entry_width` is the same without it.
	std::uint16_t column_width = 0;
	std::uint16_t entry_width = 0;
	// The first grid row shown: the state's `scroll_row`, clamped, and moved if
	// it would have left the selection off screen.
	std::uint16_t first_row = 0;
	// Candidates the filter admits.
	std::size_t count = 0;

	[[nodiscard]] bool empty() const noexcept { return count == 0 || visible_rows == 0; }
	[[nodiscard]] bool scrolled() const noexcept { return rows > visible_rows; }

	friend bool operator==(const pager_grid&, const pager_grid&) noexcept = default;
};

// The grid for `pager` at this width, given at most `max_rows` rows.
//
// ROW-MAJOR: candidates fill across and then down. fish's pager does the same,
// and it is what makes scrolling a window of ROWS rather than a reshuffle of
// which candidate is in which column - `ls`'s column-major order is right for a
// listing that is never scrolled and wrong for one that is.
[[nodiscard]] pager_grid measure_pager(
	const pager_state& pager, std::uint16_t columns, std::uint16_t max_rows,
	const grapheme::width_policy& policy = grapheme::default_width_policy);

// The two pens the picture is painted with. Two, not a theme, for the reason
// layout_input's pair gives: theming is configuration (#101) and no ticket has
// decided it. The selected entry defaults to reverse video, which is what a
// terminal has always had and what needs no colour at all.
struct pager_pens {
	style entry{};
	style selected{style{color::of_default(), color::of_default(), attribute::reverse}};

	friend bool operator==(const pager_pens&, const pager_pens&) noexcept = default;
};

// The picture, into a surface of this pager's own (A-6).
//
// `grid` is passed rather than re-measured so that the caller that already
// asked - layout, which needed the row count to size the screen - asks once.
[[nodiscard]] surface render_pager(
	cluster_pool& pool, const pager_state& pager, const pager_grid& grid,
	const pager_pens& pens = {},
	const grapheme::width_policy& policy = grapheme::default_width_policy);

// ---------------------------------------------------------------------------
// The operations a key performs. Pure, over the state; the ABI doors in
// registry.cpp are one call each into here.
// ---------------------------------------------------------------------------

// Recomputes `matching` from `filter` and clamps the selection into it (F-29).
//
// A candidate matches when its text CONTAINS the filter, byte for byte. Case
// sensitive, deliberately: a fold that is right for ASCII and wrong for
// everything above it is the half-answer §9 refuses, and a real one is a
// Unicode question no ticket has asked. Substring rather than prefix because
// the filter is a search within a list the user is already looking at, not a
// second prefix on the token being completed.
void pager_refilter(pager_state& pager);

// Moves the selection by `by`, WRAPPING at both ends. Wrapping is what makes
// Tab a cycle rather than a walk that stops (menu completion's behaviour in
// every shell that has it). No-op on an empty list.
void pager_move(pager_state& pager, std::int64_t by);

// Moves `scroll_row` the least it can to put the selection on screen. Called
// after every selection move; the renderer applies the same rule again, so a
// resize that shrank the window cannot leave the selection invisible.
void pager_reveal(pager_state& pager, const pager_grid& grid);

// Drops the last grapheme cluster of the filter (F-29's backspace) and
// refilters. False - having changed nothing - when the filter was empty, which
// is what lets the action close the pager instead.
bool pager_filter_pop(pager_state& pager);

// The selected candidate, or null when nothing is selected.
[[nodiscard]] const pager_candidate* pager_selected(const pager_state& pager) noexcept;

// ---------------------------------------------------------------------------
// F-30: the decision that happens BEFORE a pager opens.
// ---------------------------------------------------------------------------

// The longest common prefix of every matching candidate's text, on a grapheme
// cluster boundary. Empty when the list is empty or the candidates share
// nothing.
//
// Cluster-wise rather than byte-wise because a byte-wise prefix of two names
// that agree for two bytes of a three-byte character would insert half of it -
// F-3's invariant, applied to the one place in the pager that cuts text.
[[nodiscard]] std::string pager_common_prefix(const pager_state& pager);

// What a filled pager should do (F-30, and the only decision this ticket makes
// on a client's behalf).
//
// `typed` is what the replaced span currently holds - what the user has typed of
// the token. Three answers, and they are total:
//
//   nothing   the list is empty. Nobody opens a pager over no candidates.
//   insert    the list agrees on more than the user has typed. `text` is what
//             the span becomes, and the pager does NOT open: one candidate
//             inserts itself whole with its kind's trailer, and several
//             insert their common prefix and wait for the next Tab. That is
//             F-30 exactly, and it is bash's and fish's behaviour - the second
//             Tab lists, because by then the prefix IS the agreement.
//   open      the list is ambiguous and has nothing left to insert.
struct pager_decision {
	enum class kind : std::uint8_t { nothing, insert, open };

	kind what = kind::nothing;
	std::string text;   // set for `insert`, empty otherwise
};

[[nodiscard]] pager_decision decide_pager(const pager_state& pager, std::string_view typed);

} // namespace lesh::leshper
