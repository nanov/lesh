#pragma once

// The kill store (#99 answer 3, #119, architecture spec §6.5).
//
// ONE MECHANISM, TWO PARADIGMS' WORDS FOR IT. F-1's kill ring and vi's unnamed
// register are the same object: a KEYED store with the unnamed key as the
// default. Every kill, delete and yank writes it; vi's `p`/`P` and the emacs
// side's `yank` read it. That is the whole of what v1 needs, and it is one
// table rather than two that would have to be kept in step.
//
// WHY KEYED WHEN V1 HAS ONE KEY. Because the two things the owner has already
// named as coming - named registers `"a`-`"z` (an addressable view over this
// same store) and a clipboard-backed key (vim's `"+` shape, OSC 52 as the
// #97-floor-compatible transport) - are then ADDITIVE: a new key, not a new
// store and not a second read path in `p`. Neither is built here, and the
// keying is what makes building them later not a rewrite.
//
// SHAPE TRAVELS WITH THE TEXT, not with the store. `dd` kills a line and `dw`
// kills a word, and `p` has to put them back differently - below the line in
// one case, after the cursor in the other. vim keeps that bit on the register;
// so do we, as `flags`. It is the ONLY thing here that is not bytes, and it is
// the mode's projection (spec §6.3: shape lives in the mode) recorded at the
// moment the mode made it, because the moment `p` runs is too late to ask.
//
// A vector, not a map: v1 has one entry, v2 has twenty-six, and a linear walk
// over that with a length compare first is what a map would spend its
// allocation to avoid.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::leshper {

// What `p` needs to know about how the text was taken.
//
// A bitmask rather than an enum, and reserved for additive growth exactly as
// the ABI's masks are: blockwise (out of v1, spec §6.3) is a bit, not a third
// enumerator that would make every switch over this incomplete.
inline constexpr std::uint32_t kill_charwise = 0x0u;
inline constexpr std::uint32_t kill_linewise = 0x1u;

class kill_store {
public:
	// The default key, and the one v1 has. Empty rather than a word, because a
	// caller that passes no key and a caller that passes the unnamed register
	// must reach the same entry and an empty string is the only spelling both
	// can produce without agreeing on a name first.
	static constexpr std::string_view unnamed{};

	struct entry {
		std::string key;
		std::string text;
		std::uint32_t flags = kill_charwise;

		friend bool operator==(const entry&, const entry&) noexcept = default;
	};

	// Writes `key`, replacing what was there. Killing nothing is still a write:
	// `d` over an empty region leaves the register empty, which is what the next
	// `p` must then put back.
	void put(std::string_view key, std::string_view text, std::uint32_t flags);

	// What is under `key`, or null when nothing has ever been put there. Null
	// rather than an empty entry, so that `p` with nothing killed can do nothing
	// rather than insert nothing and record an undo step for it.
	[[nodiscard]] const entry* get(std::string_view key) const noexcept;

	[[nodiscard]] bool empty() const noexcept { return _entries.empty(); }
	[[nodiscard]] std::size_t size() const noexcept { return _entries.size(); }
	[[nodiscard]] const std::vector<entry>& entries() const noexcept { return _entries; }

	void clear() noexcept { _entries.clear(); }

	friend bool operator==(const kill_store&, const kill_store&) noexcept = default;

private:
	std::vector<entry> _entries;
};

} // namespace lesh::leshper
