#include "leshper/surface.h"

#include "substrate/assert.h"

namespace lesh::leshper {

namespace {

// True when a cluster is a control character, a carriage return or a line
// feed. Such a cluster is always exactly one codepoint - GB4 and GB5 break on
// both sides of one - so looking at the first is looking at all of it.
bool is_control_cluster(std::string_view cluster) {
	const grapheme::decoded first = grapheme::decode(cluster, 0);
	if (first.length == 0)
		return false;
	switch (grapheme::lookup(first.cp).cluster_break) {
		case grapheme::gcb::control:
		case grapheme::gcb::cr:
		case grapheme::gcb::lf:
			return true;
		default:
			return false;
	}
}

} // namespace

// ---------------------------------------------------------------------------
// cluster_pool
// ---------------------------------------------------------------------------

cluster_pool::cluster_pool() {
	// Seeded so that the two ids `grapheme_ref` fixes at compile time - the
	// continuation marker and every single ASCII byte - are already the ids
	// interning would hand out. A blank cell is then a constant rather than a
	// lookup, and an ASCII glyph costs an index rather than a hash, which is
	// what a shell prompt is made of.
	_clusters.emplace_back();  // id 0: the continuation marker, no bytes
	for (unsigned byte = 0; byte < 128; ++byte)
		_clusters.emplace_back(1, static_cast<char>(byte));

	// The seeded entries are reachable by arithmetic and deliberately absent
	// from the map: `intern` answers every one of them without hashing.
	LESH_ASSERT(_clusters[grapheme_ref::continuation().id()].empty());
	LESH_ASSERT(_clusters[grapheme_ref::blank().id()] == " ");
}

grapheme_ref cluster_pool::intern(std::string_view cluster) {
	if (cluster.empty())
		return grapheme_ref::continuation();
	if (cluster.size() == 1 && static_cast<unsigned char>(cluster[0]) < 0x80)
		return grapheme_ref::of_ascii(cluster[0]);

	if (const auto found = _by_bytes.find(cluster); found != _by_bytes.end())
		return grapheme_ref::from_id(found->second);

	const auto id = static_cast<std::uint32_t>(_clusters.size());
	_clusters.emplace_back(cluster);
	// Keyed on the stored copy, never on the caller's view: the caller's bytes
	// are usually a slice of a buffer that is about to be edited.
	_by_bytes.emplace(std::string_view{_clusters.back()}, id);
	return grapheme_ref::from_id(id);
}

std::string_view cluster_pool::cluster_of(grapheme_ref ref) const noexcept {
	LESH_ASSERT(ref.id() < _clusters.size());
	if (ref.id() >= _clusters.size())
		return {};
	return _clusters[ref.id()];
}

// ---------------------------------------------------------------------------
// surface
// ---------------------------------------------------------------------------

surface::surface(std::uint16_t columns, std::uint16_t rows) { resize(columns, rows); }

void surface::resize(std::uint16_t columns, std::uint16_t rows) {
	_columns = columns;
	_rows = rows;
	_cells.assign(static_cast<std::size_t>(columns) * rows, blank_cell);
	_hard_rows.assign(rows, true);
}

void surface::clear() {
	_cells.assign(_cells.size(), blank_cell);
	_hard_rows.assign(_hard_rows.size(), true);
}

bool surface::row_starts_hard_line(std::uint16_t row) const noexcept {
	LESH_ASSERT(row < _rows);
	// Hard when the answer is not known, which is the whole of the default:
	// positioning to a row is what the blitter did before #189 and is correct
	// for every row that is not a continuation of the one above it.
	return row >= _hard_rows.size() || _hard_rows[row];
}

void surface::set_row_starts_hard_line(std::uint16_t row, bool starts) noexcept {
	LESH_ASSERT(row < _rows);
	if (row < _hard_rows.size())
		_hard_rows[row] = starts;
}

const cell& surface::at(std::uint16_t row, std::uint16_t column) const noexcept {
	LESH_ASSERT(row < _rows && column < _columns);
	return _cells[static_cast<std::size_t>(row) * _columns + column];
}

cell& surface::at(std::uint16_t row, std::uint16_t column) noexcept {
	LESH_ASSERT(row < _rows && column < _columns);
	return _cells[static_cast<std::size_t>(row) * _columns + column];
}

std::uint16_t surface::write(cluster_pool& pool, std::uint16_t row, std::uint16_t column,
                             std::string_view text, const style& pen,
                             const grapheme::width_policy& policy) {
	if (row >= _rows || column >= _columns)
		return column;

	std::uint16_t col = column;
	// Where the last cluster that owned a column landed, so a zero-width
	// cluster arriving with no base of its own can join it. Absent until the
	// first glyph lands, in which case such a cluster is dropped: there is
	// nothing on this surface for the terminal to attach it to.
	bool have_base = false;
	std::uint16_t base_column = 0;

	for (std::size_t i = 0; i < text.size();) {
		const std::size_t end = grapheme::next_boundary(text, i);
		const std::string_view cluster = text.substr(i, end - i);
		i = end;

		if (is_control_cluster(cluster))
			continue;

		const int width = grapheme::cluster_width(cluster, policy);
		if (width <= 0) {
			if (have_base) {
				cell& base = at(row, base_column);
				std::string joined{pool.cluster_of(base.glyph)};
				joined.append(cluster);
				base.glyph = pool.intern(joined);
			}
			continue;
		}

		if (static_cast<int>(col) + width > static_cast<int>(_columns))
			break;  // clipped at the right edge; wrapping is F-38's, not ours

		cell& target = at(row, col);
		target.glyph = pool.intern(cluster);
		target.pen = pen;
		target.width = static_cast<std::uint8_t>(width);
		base_column = col;
		have_base = true;

		// The columns the cluster covers but does not own. The blitter never
		// emits one: writing the cluster already moved the terminal across them.
		for (int extra = 1; extra < width; ++extra) {
			cell& follower = at(row, static_cast<std::uint16_t>(col + extra));
			follower.glyph = grapheme_ref::continuation();
			follower.pen = pen;
			follower.width = 0;
		}
		col = static_cast<std::uint16_t>(col + width);
	}

	return col;
}

} // namespace lesh::leshper
