#pragma once

// A sequence with room for N inline and a heap fallback past it.
//
// WHY IT EXISTS. `leshper::effects` is returned BY VALUE from `step`, once per
// keystroke, and as a `std::vector` that was one malloc and one free per key
// pressed for a list that is almost always two elements long (a repaint and a
// worker request). N-2 wants the hot path's allocation bounded; this is the
// bound.
//
// WHY IT HAS A FALLBACK RATHER THAN A CAP. The obvious shape - a fixed array and
// an assertion - needs a PROVABLE maximum, and `step` does not have one. Injected
// input (`lesh_inject`) is drained inside the same turn that produced it, one
// dispatch per codepoint, each dispatch free to emit; and an action may inject
// again from inside that drain. So the number of effects one turn can produce is
// bounded by what a binding chooses to inject, which is user code. Dropping the
// overflow would lose a `line_accepted`; asserting on it would turn a legal
// script into a crash. It spills instead, and the spill is a diagnosis - a
// keystroke that reaches it is doing something unusual - not a failure.
//
// ONCE SPILLED, IT STAYS SPILLED, including across `clear()`. The heap block has
// already been paid for and a member that is cleared and refilled every turn
// (the host's carried-effects buffer, the registry's pending queue) would
// otherwise pay for it again on the next long turn.
//
// THE STORAGE IS ONE OR THE OTHER, never both, so `data()` is contiguous in both
// states and a range-for is a pointer walk either way.
//
// `T` must be default-constructible: the inline slots are real objects, not
// aligned bytes. Every current instantiation holds a variant of small values, so
// that costs nothing; a type where it would is a type that wants a different
// container.

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

namespace lesh {

template <typename T, std::size_t N>
class inline_vector {
public:
	using value_type = T;
	using size_type = std::size_t;
	using iterator = T*;
	using const_iterator = const T*;
	using reference = T&;
	using const_reference = const T&;

	inline_vector() = default;
	inline_vector(std::initializer_list<T> items) {
		for (const T& one : items)
			push_back(one);
	}

	[[nodiscard]] size_type size() const noexcept {
		return _spilled ? _spill.size() : _count;
	}
	[[nodiscard]] bool empty() const noexcept { return size() == 0; }
	// How many fit before the heap is touched. A constant, exposed so a test can
	// say what it is measuring.
	[[nodiscard]] static constexpr size_type inline_capacity() noexcept { return N; }
	// True once this object has fallen back to the heap. For tests and logs.
	[[nodiscard]] bool spilled() const noexcept { return _spilled; }

	[[nodiscard]] T* data() noexcept { return _spilled ? _spill.data() : _inline.data(); }
	[[nodiscard]] const T* data() const noexcept {
		return _spilled ? _spill.data() : _inline.data();
	}

	[[nodiscard]] iterator begin() noexcept { return data(); }
	[[nodiscard]] iterator end() noexcept { return data() + size(); }
	[[nodiscard]] const_iterator begin() const noexcept { return data(); }
	[[nodiscard]] const_iterator end() const noexcept { return data() + size(); }
	[[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
	[[nodiscard]] const_iterator cend() const noexcept { return end(); }

	[[nodiscard]] reference operator[](size_type at) noexcept { return data()[at]; }
	[[nodiscard]] const_reference operator[](size_type at) const noexcept {
		return data()[at];
	}
	[[nodiscard]] reference front() noexcept { return data()[0]; }
	[[nodiscard]] const_reference front() const noexcept { return data()[0]; }
	[[nodiscard]] reference back() noexcept { return data()[size() - 1]; }
	[[nodiscard]] const_reference back() const noexcept { return data()[size() - 1]; }

	void push_back(const T& one) {
		if (!_spilled && _count < N) {
			_inline[_count++] = one;
			return;
		}
		if (!_spilled)
			spill();
		_spill.push_back(one);
	}

	void push_back(T&& one) {
		if (!_spilled && _count < N) {
			_inline[_count++] = std::move(one);
			return;
		}
		if (!_spilled)
			spill();
		_spill.push_back(std::move(one));
	}

	template <typename Iterator>
	void append(Iterator first, Iterator last) {
		for (; first != last; ++first)
			push_back(*first);
	}

	// The size goes to zero. The inline slots are NOT destroyed - they are real
	// objects for the life of this container and are overwritten by the next
	// `push_back` - so clearing is two stores rather than a walk, at the cost of
	// holding whatever the tail elements own until something replaces them. For
	// a variant of small values, which is every instantiation here, that is
	// nothing.
	void clear() noexcept {
		_count = 0;
		_spill.clear();
	}

	void swap(inline_vector& other) noexcept {
		_inline.swap(other._inline);
		std::swap(_count, other._count);
		_spill.swap(other._spill);
		std::swap(_spilled, other._spilled);
	}

	friend bool operator==(const inline_vector& a, const inline_vector& b) {
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
	}

private:
	void spill() {
		_spill.reserve(N * 2);
		for (size_type at = 0; at < _count; ++at)
			_spill.push_back(std::move(_inline[at]));
		_count = 0;
		_spilled = true;
	}

	std::array<T, N> _inline{};
	std::vector<T> _spill;
	size_type _count = 0;
	bool _spilled = false;
};

} // namespace lesh
