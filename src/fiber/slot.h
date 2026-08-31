#pragma once

// `slot<T>` - A CAPACITY-1 CONFLATING CHANNEL. The only channel v1 has.
//
// This is the generalization of what #90 and #126 already built with pools and
// generations: the UI writes the latest line into a reactor's slot, the reactor
// reads it, computes, and is told to stop if a newer line arrived while it was
// working. The grilling record's words: "The existing latest-wins slot is a
// channel policy, not a separate mechanism", and "overwrite IS cancellation".
//
// Bounded by construction (capacity one), the message is MOVED and therefore
// owned - ADR-0007's rule, and the reason `send` takes `T` by value rather than
// a pointer into the sender's state. A `T` that is ITSELF an address is the hole
// in that sentence, and a debug assert closes it: see THE MESSAGE MAY NOT POINT
// INTO THE SENDER'S OWN FIBER STACK below. A `slot` never blocks a sender: `send` over
// an unconsumed value overwrites it. Backpressure would be `queue<T,N>`'s job,
// and v1 has no customer for one (architecture review 2026-08-27: reactor
// emissions append to the UI's pending batch on the same thread), so `queue` is
// not built here.
//
// ---------------------------------------------------------------------------
// HOW `superseded` REACHES THE RECEIVER'S IN-FLIGHT TOKEN
// ---------------------------------------------------------------------------
//
// A SEND COUNTER THE TOKEN SNAPSHOTS, not a flag the token points at. `send`
// increments a monotonic counter; `recv` hands back a token carrying the
// counter's value at delivery; `superseded()` is a comparison. There is nothing
// to clear, so there is no clear-at-the-wrong-moment bug; a token from an older
// `recv` reads superseded forever rather than aliasing a reused flag; and it is
// the same generation discipline the receivers already use to drop stale
// emissions, so there is one idea in the system instead of two.
//
// The token does hold a pointer to the slot, so a token must not outlive its
// slot - but a token is a local in the receiving fiber's compute loop and the
// slot outlives the fiber by construction, and an outlived token reads
// `superseded() == true`, which is the safe answer.
//
// THE COUNTER MOVES ON EVERY `send`, NOT ONLY ON AN OVERWRITE. The ticket words
// the rule as "a `send` over an unconsumed value overwrites it and sets a
// `superseded` flag", but the case that actually matters in the shell is the
// other one: the reactor has already CONSUMED line 1 and is computing when line
// 2 arrives. The slot is empty at that moment - nothing is being overwritten -
// and the in-flight work is nonetheless obsolete. So every `send` supersedes
// every outstanding token, and `superseded_sends()` counts the narrower case
// separately, for the debug counter the ticket asks channels to carry.
//
// SINGLE THREAD, SINGLE RECEIVER. No atomics: a `scheduler` and everything
// hanging off it live on one thread, and the switch points are explicit. Two
// fibers calling `recv` on one slot is a design error and is asserted, not
// handled - it would make "the latest value" ambiguous.

#include "fiber/scheduler.h"

#include "substrate/assert.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace lesh::fiber {

// ---------------------------------------------------------------------------
// THE MESSAGE MAY NOT POINT INTO THE SENDER'S OWN FIBER STACK
// ---------------------------------------------------------------------------
//
// coost's one-liner, taken deliberately (`Sched::on_stack(p)`: is `p` inside
// `[stack_base, stack_base + size)` of the running coroutine?). ADR-0007 says a
// message in flight has exactly one owner, and `send` takes `T` by value so that
// an owned `T` is what the slot holds - but a `T` that IS an address, a
// `string_view` or a span borrows bytes it did not bring, and if those bytes are
// the sender's own frame they are gone at its next yield. The receiver would
// read a stack that has been reused by whatever ran next, which is the worst
// shape a bug can take: it depends on the schedule, so it reproduces on one
// machine and not another.
//
// TRAIT-GATED, AND THE TRAIT IS "BORROWS RATHER THAN OWNS". Two shapes qualify:
// a raw pointer, and a view - trivially copyable, with `data()` and `size()`.
// The trivial-copyability clause is what keeps `std::string` and `std::vector`
// out, and keeping them out is not fussiness: a short `std::string`'s `data()`
// points INSIDE the object, so a `slot<std::string>` sent from a fiber would
// trip on every short string while being perfectly correct - the bytes are moved
// into the slot. Anything else - an int, an event mask, a struct with a pointer
// buried in it - is not inspected, and the comment is the honest limit: this
// catches the shapes whose whole content is an address, not every address a
// message could hide.
//
// DEBUG ONLY. The whole expression sits inside `LESH_ASSERT`, so a release build
// computes nothing.

namespace detail {

// A view: it hands out an address it does not own.
template <typename T>
concept borrowing_view = std::is_trivially_copyable_v<T> && requires(const T& value) {
	{ value.data() } -> std::convertible_to<const void*>;
	{ value.size() };
};

// The address a message would hand its receiver, or null when it hands out none.
template <typename T>
[[nodiscard]] constexpr const void* borrowed_bytes(const T& value) noexcept {
	if constexpr (std::is_pointer_v<T>)
		return static_cast<const void*>(value);
	else if constexpr (borrowing_view<T>)
		return static_cast<const void*>(value.data());
	else
		return nullptr;
}

// `Sched::on_stack`, for the fiber that is running right now. False when the
// host is the sender: the host's stack outlives every fiber on it, so a pointer
// into it is not the hazard this exists for.
[[nodiscard]] inline bool points_into_the_running_fiber_stack(const scheduler& on,
                                                              const void* p) noexcept {
	if (p == nullptr)
		return false;
	const fiber* const running = on.current();
	if (running == nullptr)
		return false;
	const stack_extents where = running->stack();
	if (where.stack_base == nullptr)
		return false;
	// Through integers rather than by comparing unrelated pointers, which the
	// standard leaves unordered and a sanitizer is entitled to say so about.
	const auto at = reinterpret_cast<std::uintptr_t>(p);
	const auto base = reinterpret_cast<std::uintptr_t>(where.stack_base);
	return at >= base && at < base + where.stack_size;
}

} // namespace detail

template <typename T>
class slot {
public:
	// THE RECEIVER'S IN-FLIGHT TOKEN. `request_token`-shaped: the one question a
	// reactor asks at its `kPollEvery` points.
	class token {
	public:
		token() = default;

		// True when a newer value has been sent since this token was handed out -
		// and true for a token that was never handed out at all, because "abandon
		// the work" is the safe answer to "I do not know what I am holding".
		[[nodiscard]] bool superseded() const noexcept {
			return _owner == nullptr || _owner->_sends != _at;
		}

		[[nodiscard]] bool valid() const noexcept { return _owner != nullptr; }

	private:
		friend class slot;
		token(const slot& owner, std::uint64_t at) noexcept : _owner(&owner), _at(at) {}

		const slot* _owner = nullptr;
		std::uint64_t _at = 0;
	};

	explicit slot(scheduler& on) noexcept : _sched(&on) {}
	slot(const slot&) = delete;
	slot& operator=(const slot&) = delete;

	// Latest wins. Never blocks, never fails, wakes a parked receiver. Callable
	// from a fiber or from the host loop - the UI's send at a keystroke is the
	// second of those.
	void send(T value) {
		LESH_ASSERT(!detail::points_into_the_running_fiber_stack(
		                *_sched, detail::borrowed_bytes(value))
		            && "a message may not point into the sending fiber's own stack");
		if (_value.has_value())
			++_superseded_sends;   // the narrow case: nobody consumed the last one
		_value.emplace(std::move(value));
		++_sends;                  // supersedes every outstanding token
		if (_waiter != nullptr) {
			fiber* const waiter = _waiter;
			_waiter = nullptr;
			_sched->wake(*waiter);
		}
	}

	// Parks until there is something to take. Callable ONLY from inside a fiber.
	// The returned value is moved out; the slot is empty afterwards, and
	// `in_flight()` is the token for what was just handed over.
	[[nodiscard]] T recv() {
		fiber* const me = _sched->current();
		LESH_ASSERT(me != nullptr && "slot::recv is called from inside a fiber");
		while (!_value.has_value()) {
			LESH_ASSERT((_waiter == nullptr || _waiter == me) &&
			            "one receiver per slot: 'the latest value' has to have one reader");
			_waiter = me;
			_sched->park();
			// A spurious wake (somebody called `scheduler::wake` directly) sends us
			// round again rather than returning an empty value - the loop is the
			// condition, as with a condition variable.
		}
		_waiter = nullptr;
		_in_flight = token(*this, _sends);
		T out = std::move(*_value);
		_value.reset();
		return out;
	}

	// THE HOST'S HALF OF `recv` (#208). `recv` parks and therefore belongs to a
	// fiber; the host has no stack to park and asks instead. Nothing else
	// differs - the value is moved out, the slot is empty afterwards, and
	// `in_flight()` is the token for what was just handed over - so a host
	// polling a slot and a fiber waiting on one see the same channel.
	//
	// The one customer is `event_loop`'s `done` slot: the execution fiber sends
	// the status back and the host, which cannot park, turns until it is there.
	[[nodiscard]] std::optional<T> try_recv() {
		if (!_value.has_value())
			return std::nullopt;
		_in_flight = token(*this, _sends);
		std::optional<T> out = std::move(_value);
		_value.reset();
		return out;
	}

	// The token for the value `recv` last delivered. Handed out separately rather
	// than returned alongside the value so that `recv`'s result can be a plain
	// `T` and the receiver can keep the token for as long as its compute lasts.
	[[nodiscard]] token in_flight() const noexcept { return _in_flight; }

	// Shorthand for the poll a reactor actually writes.
	[[nodiscard]] bool superseded() const noexcept { return _in_flight.superseded(); }

	[[nodiscard]] bool empty() const noexcept { return !_value.has_value(); }
	[[nodiscard]] bool has_waiter() const noexcept { return _waiter != nullptr; }

	// Totals, for tests and for the debug counters the ticket asks channels to
	// carry. `superseded_sends` counts only sends that dropped an unconsumed
	// value - the loss a `queue` would have to report; `sends` counts every one.
	[[nodiscard]] std::uint64_t sends() const noexcept { return _sends; }
	[[nodiscard]] std::size_t superseded_sends() const noexcept { return _superseded_sends; }

private:
	scheduler* _sched;
	std::optional<T> _value;
	fiber* _waiter = nullptr;
	token _in_flight;
	std::uint64_t _sends = 0;
	std::size_t _superseded_sends = 0;
};

} // namespace lesh::fiber
