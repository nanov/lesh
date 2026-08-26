#include "leshper/pager.h"

#include "substrate/assert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lesh::leshper {

namespace {

// The blank columns between one column of candidates and the next. Two, which
// is `ls`'s, and enough that a marker and the next name never read as one word.
constexpr std::uint16_t gutter = 2;

// Everything the grid arithmetic must not overflow. A `uint16_t` count of rows
// is the surface's own limit, and a candidate list long enough to exceed it at
// one column per row is a list nobody is reading; the picture truncates rather
// than wrapping the row count (N-4, and the same rule layout.cpp's `last_row`
// states).
constexpr std::size_t row_ceiling = 0xFFFEu;

std::uint16_t clamp_u16(std::size_t value) noexcept {
	return static_cast<std::uint16_t>(std::min<std::size_t>(value, row_ceiling));
}

} // namespace

void pager_insertion(const pager_candidate& one, std::string& into) {
	into.assign(one.text);
	into.append(pager_trailer(one.kind));
}

std::uint16_t pager_row_budget(std::uint16_t rows) noexcept {
	if (rows <= 1)
		return 0;
	// Half the screen, and always at least one row left for the edit line.
	const std::uint16_t half = static_cast<std::uint16_t>(rows / 2);
	const std::uint16_t most = static_cast<std::uint16_t>(rows - 1);
	return std::max<std::uint16_t>(1, std::min(half, most));
}

pager_grid measure_pager(const pager_state& pager, std::uint16_t columns,
                         std::uint16_t max_rows, const grapheme::width_policy& policy) {
	pager_grid grid;
	grid.count = pager.matching.size();
	if (grid.count == 0 || columns == 0 || max_rows == 0)
		return grid;

	// The column is as wide as the widest thing that goes in one - the spec's
	// "columns sized to the longest candidate", with the marker counted because
	// the marker is drawn inside the column.
	int widest = 1;
	for (const std::uint32_t index : pager.matching) {
		const pager_candidate& one = pager.candidates[index];
		int width = grapheme::string_width(one.text, policy);
		if (pager_marker(one.kind) != '\0')
			++width;
		widest = std::max(widest, width);
	}
	// A candidate wider than the screen gets the screen and is clipped by the
	// surface's own rule; it does not get to make the column width meaningless.
	grid.entry_width = static_cast<std::uint16_t>(std::min<int>(widest, columns));
	grid.column_width = static_cast<std::uint16_t>(grid.entry_width + gutter);

	// The last column needs no gutter after it, which is why the width is added
	// back before the division rather than subtracted from the column.
	const int fit = (static_cast<int>(columns) + gutter) / static_cast<int>(grid.column_width);
	// Never more columns than there are candidates: four names do not get ten
	// columns, and a surface as wide as the terminal for a two-item list would
	// be the pager claiming space it drew nothing in.
	grid.columns = static_cast<std::uint16_t>(
		std::min<std::size_t>(static_cast<std::size_t>(std::max(1, fit)), grid.count));

	const std::size_t needed = (grid.count + grid.columns - 1) / grid.columns;
	grid.rows = clamp_u16(needed);
	grid.visible_rows = std::min(grid.rows, max_rows);

	// The scroll offset, clamped and then made to contain the selection. Both
	// halves are here rather than in the mover, so that a resize - which changes
	// `visible_rows` and nothing else - cannot leave the selection off screen.
	const std::uint16_t furthest = static_cast<std::uint16_t>(grid.rows - grid.visible_rows);
	std::uint16_t first = std::min(pager.scroll_row, furthest);
	const std::size_t selected = std::min(pager.selected, grid.count - 1);
	const std::uint16_t selected_row =
		clamp_u16(selected / static_cast<std::size_t>(grid.columns));
	if (selected_row < first)
		first = selected_row;
	else if (selected_row >= static_cast<std::uint16_t>(first + grid.visible_rows))
		first = static_cast<std::uint16_t>(selected_row - grid.visible_rows + 1);
	grid.first_row = first;
	return grid;
}

surface render_pager(cluster_pool& pool, const pager_state& pager, const pager_grid& grid,
                     const pager_pens& pens, const grapheme::width_policy& policy) {
	if (grid.empty() || grid.columns == 0)
		return surface{};

	// As wide as the columns it drew, minus the gutter the last one does not
	// need. Never wider than the terminal: `measure_pager` capped the entry at
	// the width and fitted at least one column, so this cannot exceed it.
	const std::uint16_t width =
		static_cast<std::uint16_t>(grid.columns * grid.column_width - gutter);
	surface page{width, grid.visible_rows};
	// The picture is exactly as wide as the columns it drew, never as wide as
	// the terminal: A-6 says this surface is the pager's own, and a caller that
	// wants it centred or padded does that when it composites. layout.cpp
	// deliberately does neither - it copies it flush left, which is where every
	// shell's pager has always been.

	std::string marker(1, ' ');
	for (std::uint16_t row = 0; row < grid.visible_rows; ++row) {
		for (std::uint16_t column = 0; column < grid.columns; ++column) {
			const std::size_t at = (static_cast<std::size_t>(grid.first_row) + row)
			                           * grid.columns + column;
			if (at >= grid.count)
				break;
			const pager_candidate& one = pager.candidates[pager.matching[at]];
			const bool is_selected = at == pager.selected;
			const style& pen = is_selected ? pens.selected : pens.entry;
			const std::uint16_t start =
				static_cast<std::uint16_t>(column * grid.column_width);

			// The whole entry slot carries the pen, not only the glyphs. A
			// selection drawn as a reverse-video BAR is what every pager looks
			// like; painting only the letters would leave a ragged highlight
			// whose width was the candidate's rather than the column's.
			for (std::uint16_t at_column = start;
			     at_column < start + grid.entry_width && at_column < page.columns();
			     ++at_column) {
				cell& one_cell = page.at(row, at_column);
				one_cell = blank_cell;
				one_cell.pen = pen;
			}

			const std::uint16_t after =
				page.write(pool, row, start, one.text, pen, policy);
			const char mark = pager_marker(one.kind);
			if (mark != '\0') {
				marker[0] = mark;
				page.write(pool, row, after, marker, pen, policy);
			}
		}
	}
	return page;
}

// ---------------------------------------------------------------------------
// The operations.
// ---------------------------------------------------------------------------

void pager_refilter(pager_state& pager) {
	// The candidate the selection was on, so that filtering keeps pointing at
	// it where it can. Typing a filter that narrows past the selected entry is
	// the ordinary case and lands on the first match, which is what a user
	// typing to find something means.
	const pager_candidate* was =
		pager.selected < pager.matching.size()
			? &pager.candidates[pager.matching[pager.selected]]
			: nullptr;

	pager.matching.clear();
	for (std::size_t index = 0; index < pager.candidates.size(); ++index) {
		if (pager.filter.empty()
		    || pager.candidates[index].text.find(pager.filter) != std::string::npos)
			pager.matching.push_back(static_cast<std::uint32_t>(index));
	}

	pager.selected = 0;
	if (was != nullptr) {
		for (std::size_t at = 0; at < pager.matching.size(); ++at) {
			if (&pager.candidates[pager.matching[at]] == was) {
				pager.selected = at;
				break;
			}
		}
	}
	if (pager.matching.empty())
		pager.scroll_row = 0;
}

void pager_move(pager_state& pager, std::int64_t by) {
	const std::int64_t count = static_cast<std::int64_t>(pager.matching.size());
	if (count == 0) {
		pager.selected = 0;
		return;
	}
	// Wrapping, so Tab is a cycle. The modulus is taken twice because C++'s is
	// signed: `-1 % 5` is -1, and the second fold is what turns it into 4.
	std::int64_t at = static_cast<std::int64_t>(pager.selected) + by;
	at %= count;
	if (at < 0)
		at += count;
	pager.selected = static_cast<std::size_t>(at);
}

void pager_reveal(pager_state& pager, const pager_grid& grid) {
	if (grid.columns == 0 || grid.visible_rows == 0 || grid.count == 0) {
		pager.scroll_row = 0;
		return;
	}
	const std::uint16_t selected_row =
		clamp_u16(pager.selected / static_cast<std::size_t>(grid.columns));
	const std::uint16_t furthest =
		grid.rows > grid.visible_rows
			? static_cast<std::uint16_t>(grid.rows - grid.visible_rows)
			: 0;
	std::uint16_t first = std::min(pager.scroll_row, furthest);
	if (selected_row < first)
		first = selected_row;
	else if (selected_row >= static_cast<std::uint16_t>(first + grid.visible_rows))
		first = static_cast<std::uint16_t>(selected_row - grid.visible_rows + 1);
	pager.scroll_row = first;
}

bool pager_filter_pop(pager_state& pager) {
	if (pager.filter.empty())
		return false;
	// The last CLUSTER, not the last byte: a filter is text a human typed and
	// backspace removes what they see (F-3's rule, at the one place the pager
	// shortens text).
	std::size_t last = 0;
	for (std::size_t at = 0; at < pager.filter.size();) {
		last = at;
		at = grapheme::next_boundary(pager.filter, at);
		LESH_ASSERT(at > last);
	}
	pager.filter.resize(last);
	pager_refilter(pager);
	return true;
}

const pager_candidate* pager_selected(const pager_state& pager) noexcept {
	if (pager.selected >= pager.matching.size())
		return nullptr;
	return &pager.candidates[pager.matching[pager.selected]];
}

// ---------------------------------------------------------------------------
// F-30.
// ---------------------------------------------------------------------------

std::string pager_common_prefix(const pager_state& pager) {
	if (pager.matching.empty())
		return {};

	std::string_view shared{pager.candidates[pager.matching.front()].text};
	for (const std::uint32_t index : pager.matching) {
		const std::string_view one{pager.candidates[index].text};
		std::size_t agree = 0;
		while (agree < shared.size() && agree < one.size() && shared[agree] == one[agree])
			++agree;
		shared = shared.substr(0, agree);
		if (shared.empty())
			break;
	}
	if (shared.empty())
		return {};

	// Back off to the last cluster boundary the agreement reached. Two names
	// agreeing on the first two bytes of a three-byte character agree on nothing
	// anybody can insert.
	const std::string_view whole{pager.candidates[pager.matching.front()].text};
	std::size_t boundary = 0;
	for (std::size_t at = 0; at < whole.size();) {
		const std::size_t next = grapheme::next_boundary(whole, at);
		if (next > shared.size())
			break;
		boundary = next;
		at = next;
	}
	return std::string{shared.substr(0, boundary)};
}

pager_decision decide_pager(const pager_state& pager, std::string_view typed) {
	pager_decision answer;
	if (pager.matching.empty())
		return answer;

	if (pager.matching.size() == 1) {
		// One candidate is never ambiguous, and it inserts WHOLE - text plus its
		// kind's trailer - even when the user had already typed all of it. That
		// is what makes a second Tab on a finished word add the space rather
		// than open a pager over one entry.
		answer.what = pager_decision::kind::insert;
		pager_insertion(pager.candidates[pager.matching.front()], answer.text);
		return answer;
	}

	std::string shared = pager_common_prefix(pager);
	// It must EXTEND what is there, not merely be longer than it. A history
	// search's candidates are whole lines and share a prefix that has nothing to
	// do with the query; inserting it would replace the line with a stranger's
	// beginning. Completion candidates start with the token by construction, so
	// this costs the ordinary case one comparison and buys the general case its
	// correctness.
	if (shared.size() > typed.size() && std::string_view{shared}.substr(0, typed.size()) == typed) {
		// The agreement goes further than the user has typed: insert it and stay
		// closed (F-30). No trailer - the candidates disagree about what comes
		// next, which is the whole reason there is more than one of them.
		answer.what = pager_decision::kind::insert;
		answer.text = std::move(shared);
		return answer;
	}

	answer.what = pager_decision::kind::open;
	return answer;
}

} // namespace lesh::leshper
