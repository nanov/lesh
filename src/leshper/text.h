#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace lesh::leshper {

// The two text primitives the rest of leshper is written in: where the cursor
// is, and what it is in.
//
// Separate from state.h only to break a cycle: A-1's state struct owns the undo
// history (F-1), the undo history is written in terms of positions and buffer
// text, and a header cannot include the one that includes it. This is the lower
// half. Nothing here knows about events, effects, or the editor.

// A place in the buffer. See A-1 and F-3.
//
// Opaque on purpose. Underneath it is a byte offset today; #88 decides how
// grapheme clusters are segmented, and when it lands the offset either gains a
// cluster index beside it or stops being a byte count altogether. Every call
// site that only holds, compares and passes positions survives that unchanged -
// which is the point, because F-3 says cursor motion and word boundaries are
// grapheme-wise and there is no cheap way to retrofit that onto arithmetic
// scattered across the editor.
//
// The rule this type exists to enforce: nothing outside src/leshper/ does
// arithmetic on the offset, and inside it only text_buffer does. Ask the buffer
// to step a position; do not add one to it.
class position {
public:
	constexpr position() noexcept = default;

	[[nodiscard]] static constexpr position from_byte_offset(size_t offset) noexcept {
		return position{offset};
	}

	// The byte offset underneath. Named long deliberately: every call is a site
	// #88 must revisit, so they should be easy to find and uncomfortable to add.
	[[nodiscard]] constexpr size_t byte_offset() const noexcept { return _offset; }

	friend constexpr bool operator==(position, position) noexcept = default;
	friend constexpr auto operator<=>(position, position) noexcept = default;

private:
	explicit constexpr position(size_t offset) noexcept : _offset(offset) {}

	size_t _offset = 0;
};

// The text being edited (spec §1), and the only thing that knows how positions
// map onto bytes.
//
// Storage is std::string, and that is a decision rather than a default. #100's
// rule is "unused is thrown, used must answer our needs", with leshper's
// containers decided from MEASURED requirements rather than preemptively: a
// hand-rolled byte vector would be a replacement chosen before anything was
// measured. std::string already gives N-2 what it asks for - amortized growth,
// and a small-string optimisation that keeps a short command line off the heap
// entirely. What this class buys is that the decision is revisitable in one
// file: no caller sees a std::string, so replacing it is an edit here and
// nowhere else. The measurement that would justify replacing it belongs with
// the N-1 latency gate, which does not exist yet.
//
// UTF-8 bytes throughout. Nothing here validates them: malformed input degrades
// gracefully (N-4) by being carried as bytes, and stepping over it is #88's.
class text_buffer {
public:
	text_buffer() = default;

	[[nodiscard]] std::string_view text() const noexcept { return _bytes; }
	[[nodiscard]] size_t size_in_bytes() const noexcept { return _bytes.size(); }
	[[nodiscard]] bool empty() const noexcept { return _bytes.empty(); }

	[[nodiscard]] position begin_position() const noexcept {
		return position::from_byte_offset(0);
	}
	[[nodiscard]] position end_position() const noexcept {
		return position::from_byte_offset(_bytes.size());
	}

	// The text between two positions, borrowed. Callers copy what they intend to
	// keep: an edit record must own its old text, because the buffer it came from
	// is about to change.
	[[nodiscard]] std::string_view slice(position from, position to) const noexcept {
		const size_t begin = clamp(from.byte_offset());
		const size_t end = clamp(to.byte_offset());
		if (end <= begin)
			return {};
		return std::string_view{_bytes}.substr(begin, end - begin);
	}

	// The next and previous positions, which is where #88's tables slot in.
	//
	// Today they step one UTF-8 SCALAR VALUE, by skipping continuation bytes -
	// enough to keep the cursor off the middle of a multi-byte character, which
	// is the property the tests assert. It is NOT F-3: a combining mark or a ZWJ
	// sequence is several scalar values and one grapheme cluster, and the cursor
	// will stop inside it until #88 replaces these two function bodies.
	[[nodiscard]] position next_position(position from) const noexcept {
		size_t at = clamp(from.byte_offset());
		if (at >= _bytes.size())
			return end_position();
		++at;
		while (at < _bytes.size() && is_continuation(_bytes[at]))
			++at;
		return position::from_byte_offset(at);
	}
	[[nodiscard]] position previous_position(position from) const noexcept {
		size_t at = clamp(from.byte_offset());
		if (at == 0)
			return begin_position();
		--at;
		while (at > 0 && is_continuation(_bytes[at]))
			--at;
		return position::from_byte_offset(at);
	}

	// Replaces [from, to) with `with`, and answers where the replacement ends.
	//
	// One entry point for every mutation, so that the undo record (F-1) and the
	// generation bump (A-10) cannot be forgotten at one call site out of six -
	// the editor's apply_edit is the only caller, and it does both.
	position replace(position from, position to, std::string_view with) {
		const size_t begin = clamp(from.byte_offset());
		const size_t end = clamp(to.byte_offset()) < begin ? begin : clamp(to.byte_offset());
		_bytes.replace(begin, end - begin, with);
		return position::from_byte_offset(begin + with.size());
	}

	// True when `at` is inside a multi-byte character rather than on its first
	// byte. An edit that lands here would split a character in two; the editor
	// asserts against it rather than repairing it silently.
	[[nodiscard]] bool is_boundary(position at) const noexcept {
		const size_t offset = at.byte_offset();
		if (offset > _bytes.size())
			return false;
		if (offset == _bytes.size())
			return true;
		return !is_continuation(_bytes[offset]);
	}

	// Clamps a position that a caller derived arithmetically into range. The
	// editor never needs it; the replay harness (N-3) feeding recorded events at
	// a shorter buffer does.
	[[nodiscard]] position clamped(position at) const noexcept {
		return position::from_byte_offset(clamp(at.byte_offset()));
	}

	friend bool operator==(const text_buffer& a, const text_buffer& b) noexcept {
		return a._bytes == b._bytes;
	}

private:
	static constexpr bool is_continuation(char byte) noexcept {
		return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
	}
	[[nodiscard]] size_t clamp(size_t offset) const noexcept {
		return offset < _bytes.size() ? offset : _bytes.size();
	}

	std::string _bytes;
};

} // namespace lesh::leshper
