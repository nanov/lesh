#include "leshper/proposal.h"

namespace lesh::leshper {

void applied_proposals::apply(std::string_view reactor, std::vector<proposal>& items) {
	for (layer& held : _layers) {
		if (held.reactor == reactor) {
			held.items.swap(items);
			return;
		}
	}
	_layers.emplace_back();
	_layers.back().reactor.assign(reactor);
	_layers.back().items.swap(items);
}

bool applied_proposals::dismiss(std::uint32_t kind, decorations& marks) {
	bool dropped = false;
	for (auto it = _layers.begin(); it != _layers.end();) {
		bool carries = false;
		for (const proposal& one : it->items) {
			if (one.kind == kind) {
				carries = true;
				break;
			}
		}
		if (!carries) {
			++it;
			continue;
		}
		// The painted half, taken with it. `forget` answers whether the reactor
		// had a layer at all, which it need not have: a reactor may propose
		// without drawing anything.
		marks.forget(it->reactor);
		it = _layers.erase(it);
		dropped = true;
	}
	return dropped;
}

void applied_proposals::clear() noexcept {
	// `clear()` and not assignment, for `decorations::clear`'s reason: the
	// capacity a steady state of keystrokes grew to is why a warm line does not
	// allocate, and a line boundary is not a reason to give it back.
	_layers.clear();
}

bool applied_proposals::empty() const noexcept {
	for (const layer& one : _layers) {
		if (!one.items.empty())
			return false;
	}
	return true;
}

const proposal* applied_proposals::find(std::uint32_t kind,
                                        std::size_t index) const noexcept {
	std::size_t seen = 0;
	for (const layer& one : _layers) {
		for (const proposal& offer : one.items) {
			if (offer.kind != kind)
				continue;
			if (seen++ == index)
				return &offer;
		}
	}
	return nullptr;
}

} // namespace lesh::leshper
