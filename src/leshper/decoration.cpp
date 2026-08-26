#include "leshper/decoration.h"

#include <utility>

namespace lesh::leshper {

namespace {

// One end of one span, in the sweep below.
struct edge {
	std::size_t at = 0;
	std::uint32_t which = 0;   // the span's index in emission order
	bool opening = false;
};

} // namespace

void decorations::apply(std::string_view reactor, std::vector<decoration_span>& spans,
                        std::vector<virtual_text>& texts) {
	for (layer& held : _layers) {
		if (held.reactor == reactor) {
			held.spans.swap(spans);
			held.texts.swap(texts);
			rebuild();
			return;
		}
	}
	_layers.emplace_back();
	_layers.back().reactor.assign(reactor);
	_layers.back().spans.swap(spans);
	_layers.back().texts.swap(texts);
	rebuild();
}

bool decorations::forget(std::string_view reactor) {
	for (auto it = _layers.begin(); it != _layers.end(); ++it) {
		if (it->reactor == reactor) {
			_layers.erase(it);
			rebuild();
			return true;
		}
	}
	return false;
}

void decorations::clear() noexcept {
	// `clear()` and not assignment, on all three: the capacity a steady state of
	// keystrokes grew to is the whole reason a warm line does not allocate, and
	// a line boundary is not a reason to give it back.
	_layers.clear();
	_painted.clear();
	_texts.clear();
}

// The normalization, and it is a sweep rather than a sort.
//
// A SORT CANNOT ANSWER THIS. "Later wins" over ranges that NEST is not an
// ordering of the spans - `"$x"` emits [0,4) string.double and then [1,3)
// expansion.parameter, and the answer is three spans none of which was emitted:
// [0,1) string, [1,3) parameter, [3,4) string. So the spans are turned into
// edges, the sweep keeps the set that is open at each offset, and the winner is
// the highest emission index in it. That is exactly "paint them in order and
// look at the result", computed once.
//
// The active set is a small vector scanned for its maximum rather than a heap:
// the set is the NESTING DEPTH of a command line's highlights, which is two or
// three, and a heap would be a data structure for a number that fits in a
// register.
void decorations::rebuild() {
	_painted.clear();
	_texts.clear();

	// Emission order across the layers, which is application order: the layer
	// applied later paints over the one applied earlier.
	std::vector<decoration_span> flat;
	std::size_t total = 0;
	for (const layer& one : _layers)
		total += one.spans.size();
	flat.reserve(total);
	for (const layer& one : _layers) {
		for (const decoration_span& span : one.spans) {
			// An empty range paints nothing, and a range carrying no style is an
			// annotation with nothing to say. Neither is dropped for tidiness:
			// letting either into the sweep would let it OCCLUDE a span
			// underneath it, which is a visible wrong answer rather than a
			// wasted entry.
			if (span.end <= span.start || span.style_id == 0)
				continue;
			flat.push_back(span);
		}
		for (const virtual_text& text : one.texts) {
			if (text.bytes.empty())
				continue;
			_texts.push_back(text);
		}
	}

	// Stable, so two texts at one offset stay in the order they were emitted.
	std::stable_sort(_texts.begin(), _texts.end(),
	                 [](const virtual_text& a, const virtual_text& b) { return a.at < b.at; });

	if (flat.empty())
		return;

	std::vector<edge> edges;
	edges.reserve(flat.size() * 2);
	for (std::size_t i = 0; i < flat.size(); ++i) {
		const auto which = static_cast<std::uint32_t>(i);
		edges.push_back(edge{flat[i].start, which, true});
		edges.push_back(edge{flat[i].end, which, false});
	}
	// Closings before openings at the same offset: a span ending where the next
	// begins must be out of the set before the interval starting there is
	// answered, or an abutting pair would read as an overlap.
	std::sort(edges.begin(), edges.end(), [](const edge& a, const edge& b) {
		return a.at != b.at ? a.at < b.at : (!a.opening && b.opening);
	});

	std::vector<std::uint32_t> active;
	std::size_t previous = edges.front().at;
	for (std::size_t i = 0; i < edges.size();) {
		const std::size_t at = edges[i].at;

		// The interval [previous, at) belongs to whichever open span was emitted
		// last, and to nothing at all when none is open.
		if (at > previous && !active.empty()) {
			const std::uint32_t winner =
				*std::max_element(active.begin(), active.end());
			const std::uint32_t style = flat[winner].style_id;
			// Adjacent and identical is one span, not two: the renderer walks
			// these with a cursor and a seam it cannot see is a seam it should
			// not be given.
			if (!_painted.empty() && _painted.back().end == previous
			    && _painted.back().style_id == style)
				_painted.back().end = at;
			else
				_painted.push_back(decoration_span{previous, at, style});
		}

		for (; i < edges.size() && edges[i].at == at; ++i) {
			if (edges[i].opening) {
				active.push_back(edges[i].which);
			} else {
				const auto found =
					std::find(active.begin(), active.end(), edges[i].which);
				if (found != active.end())
					active.erase(found);
			}
		}
		previous = at;
	}
}

} // namespace lesh::leshper
