#include "leshper/kill_store.h"

#include <string>
#include <string_view>

namespace lesh::leshper {

void kill_store::put(std::string_view key, std::string_view text, std::uint32_t flags) {
	for (entry& one : _entries) {
		if (one.key == key) {
			// assign rather than construct: a register written on every `x` keeps
			// the capacity it grew to, so the steady state stops allocating.
			one.text.assign(text);
			one.flags = flags;
			return;
		}
	}
	_entries.push_back(entry{std::string{key}, std::string{text}, flags});
}

const kill_store::entry* kill_store::get(std::string_view key) const noexcept {
	for (const entry& one : _entries) {
		if (one.key == key)
			return &one;
	}
	return nullptr;
}

} // namespace lesh::leshper
