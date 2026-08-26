#include "leshper/decoration.h"

#include <utility>

namespace lesh::leshper {

// `edge` is `decorations`' own private nested type since #168 Phase B - the
// sweep's buffers are members now and a member needs a complete type, so the
// struct had to leave this file's anonymous namespace for the header. Nothing
// outside `rebuild` constructs one.

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
	//
	// `_scratch` IS NOT NAMED HERE and that is deliberate: it holds nothing
	// between sweeps that could be stale (every buffer is cleared at the top of
	// `rebuild`), and it is the one thing here whose whole value is its capacity.
	// The strings inside `_texts` DO go with the elements, which costs the next
	// line's first suggestion one allocation - per LINE, not per keystroke.
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
// register. It, and the two buffers beside it, are MEMBERS - see
// `decorations::rebuild_scratch` for why this function allocates nothing once
// the first line of a given size has been painted.
void decorations::rebuild() {
	// THE THREE BUFFERS BELOW ARE MEMBERS AND ARE ONLY CLEARED (#168 Phase B).
	// `clear()` on a vector destroys the elements and keeps the capacity, so a
	// warm line's sweep reaches the heap zero times; a line with more spans than
	// any before it grows one of them once and never again. They were locals, and
	// a local vector on a function that runs once per reactor per keystroke is
	// three mallocs and three frees per character typed.
	_painted.clear();
	_scratch.flat.clear();
	_scratch.edges.clear();
	_scratch.active.clear();

	// `_texts` IS NOT CLEARED, and that is the same argument one level down. A
	// `virtual_text` carries its BYTES, so clearing would free every suggestion's
	// string and the next sweep would allocate it again - once per keystroke, on
	// the one decoration that changes on every keystroke. Assigning over an
	// element that is already there lets `std::string::operator=` reuse the
	// buffer it grew. `written` counts what this sweep produced and the resize at
	// the bottom is what makes the vector's SIZE honest again.
	std::size_t written = 0;

	// Emission order across the layers, which is application order: the layer
	// applied later paints over the one applied earlier.
	std::size_t total = 0;
	for (const layer& one : _layers)
		total += one.spans.size();
	_scratch.flat.reserve(total);
	for (const layer& one : _layers) {
		for (const decoration_span& span : one.spans) {
			// An empty range paints nothing, and a range carrying no style is an
			// annotation with nothing to say. Neither is dropped for tidiness:
			// letting either into the sweep would let it OCCLUDE a span
			// underneath it, which is a visible wrong answer rather than a
			// wasted entry.
			if (span.end <= span.start || span.style_id == 0)
				continue;
			_scratch.flat.push_back(span);
		}
		for (const virtual_text& text : one.texts) {
			if (text.bytes.empty())
				continue;
			if (written < _texts.size())
				_texts[written] = text;
			else
				_texts.push_back(text);
			++written;
		}
	}
	// Shrinks the SIZE and keeps the CAPACITY, and destroys nothing at all when
	// the count did not change - which is the steady state.
	_texts.resize(written);

	// Stable, so two texts at one offset stay in the order they were emitted.
	//
	// AN INSERTION SORT BY HAND, AND `std::stable_sort` IS THE BUG IT REPLACES.
	// libc++'s `__stable_sort_switch` is `128 * is_trivially_copy_assignable<T>`,
	// and a `virtual_text` holds a `std::string` - so the threshold is ZERO for
	// this element type and `stable_sort` took a temporary buffer for ANY
	// non-empty range, including the one-element range the shipped reactor set
	// produces. One malloc and one free per applied batch, per reactor, per
	// keystroke, for a sort of a single element.
	//
	// The loop below is stable by construction (it stops at the first element
	// that is not strictly greater), allocation-free at every size (moves only,
	// and moving a string steals its buffer), and O(n^2) in a number that is the
	// count of reactors drawing virtual text - one, today. If that ever becomes a
	// number worth an O(n log n) sort, the answer is a sort over an index
	// permutation, not a buffer of `virtual_text`.
	for (std::size_t i = 1; i < _texts.size(); ++i) {
		for (std::size_t j = i; j > 0 && _texts[j].at < _texts[j - 1].at; --j)
			std::swap(_texts[j], _texts[j - 1]);
	}

	if (_scratch.flat.empty())
		return;

	_scratch.edges.reserve(_scratch.flat.size() * 2);
	for (std::size_t i = 0; i < _scratch.flat.size(); ++i) {
		const auto which = static_cast<std::uint32_t>(i);
		_scratch.edges.push_back(edge{_scratch.flat[i].start, which, true});
		_scratch.edges.push_back(edge{_scratch.flat[i].end, which, false});
	}
	// Closings before openings at the same offset: a span ending where the next
	// begins must be out of the set before the interval starting there is
	// answered, or an abutting pair would read as an overlap.
	//
	// `std::sort` and not `stable_sort`: introsort carries no buffer, and the
	// comparator is a total order on (offset, closing-first) already.
	std::sort(_scratch.edges.begin(), _scratch.edges.end(), [](const edge& a, const edge& b) {
		return a.at != b.at ? a.at < b.at : (!a.opening && b.opening);
	});

	std::size_t previous = _scratch.edges.front().at;
	for (std::size_t i = 0; i < _scratch.edges.size();) {
		const std::size_t at = _scratch.edges[i].at;

		// The interval [previous, at) belongs to whichever open span was emitted
		// last, and to nothing at all when none is open.
		if (at > previous && !_scratch.active.empty()) {
			const std::uint32_t winner =
				*std::max_element(_scratch.active.begin(), _scratch.active.end());
			const std::uint32_t style = _scratch.flat[winner].style_id;
			// Adjacent and identical is one span, not two: the renderer walks
			// these with a cursor and a seam it cannot see is a seam it should
			// not be given.
			if (!_painted.empty() && _painted.back().end == previous
			    && _painted.back().style_id == style)
				_painted.back().end = at;
			else
				_painted.push_back(decoration_span{previous, at, style});
		}

		for (; i < _scratch.edges.size() && _scratch.edges[i].at == at; ++i) {
			if (_scratch.edges[i].opening) {
				_scratch.active.push_back(_scratch.edges[i].which);
			} else {
				const auto found = std::find(_scratch.active.begin(),
				                             _scratch.active.end(),
				                             _scratch.edges[i].which);
				if (found != _scratch.active.end())
					_scratch.active.erase(found);
			}
		}
		previous = at;
	}
}

} // namespace lesh::leshper
