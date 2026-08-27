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
// a pointer into the sender's state. A `slot` never blocks a sender: `send` over
// an unconsumed value overwrites it. Backpressure would be `queue<T,N>`'s job,
// and v1 has no customer for one (architecture review 2026-08-27: reactor
// emissions append to the UI's pending batch on the same thread), so `queue` is
// not built here.
//
// ---------------------------------------------------------------------------
// HOW `superseded` REACHES THE RECEIVER'S IN-FLIGHT TOKEN
// ---------------------------------------------------------------------------
//
// The two candidate mechanisms, because this was a judgment call:
//
//   (a) A FLAG THE TOKEN POINTS AT, which is what `ui/workers.h` does today: the
//       slot owns `bool superseded`, `recv` clears it, `send` sets it, and the
//       token holds its address. It works, and it comes with two liabilities.
//       The flag has to be cleared at exactly the right moment (workers.cpp does
//       it in three places), and the token's address must stay valid - the
//       comment above `ui/workers.h`'s `struct slot` exists only to warn that a
//       rehash moving the slot would dangle a token mid-compute.
//
//   (b) A SEND COUNTER THE TOKEN SNAPSHOTS, which is what this is. `send`
//       increments a monotonic counter; `recv` hands back a token carrying the
//       counter's value at delivery; `superseded()` is a comparison. Nothing to
//       clear, so there is no clear-at-the-wrong-moment bug; a token from an
//       older `recv` reads superseded forever rather than aliasing a reused
//       flag; and it is the same generation discipline the receivers already use
//       to drop stale emissions, so there is one idea in the system instead of
//       two.
//
// (b), then. The token does hold a pointer to the slot, so a token must not
// outlive its slot - but a token is a local in the receiving fiber's compute
// loop and the slot outlives the fiber by construction, and an outlived token
// reads `superseded() == true`, which is the safe answer.
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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace lesh::fiber {

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
