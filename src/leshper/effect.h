#pragma once

#include "leshper/state.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace lesh::leshper {

// Redraw (CONTEXT.md: an effect is the loop's, synchronous, and emitted
// alongside the new state).
//
// Opaque, and it stays opaque this ticket. #97 fixed the terminal floor and the
// cell's shape, but the renderer - surface, blitter, diff-based update,
// layout-as-value (F-37) - is not built, and inventing half of it here to give
// this struct a field would be building the thing the ticket says not to build.
// What the type buys today is the SHAPE: a turn that changed something visible
// says so once, rather than the editor calling a renderer it must not know
// about.
struct render_request {
	friend constexpr bool operator==(const render_request&, const render_request&) noexcept {
		return true;
	}
};

// Recompute derived state on a worker (A-4, A-10).
//
// Generation-tagged on the way out and matched on the way back: this is the
// other half of worker_result's drop rule. The editor emits one when the buffer
// moved, and whatever comes back tagged with an older generation is dropped
// without being looked at.
//
// Which reactor is being asked, and what for, arrives with #93's subscription
// interface. #90 already fixed the machinery on the far side - one in-flight
// request plus one pending slot per reactor, overwritten on supersede, so queue
// depth is structurally at most one - and none of that changes this field.
struct worker_request {
	generation computed_against;

	friend bool operator==(const worker_request& a, const worker_request& b) noexcept {
		return a.computed_against == b.computed_against;
	}
};

// Run an external program (#92's lane 2).
//
// The port maps editing-time shell code onto three lanes: builtins and
// functions in-process (lane 1), externals via posix_spawn (lane 2), and forms
// that genuinely need a forked shell - subshells, `&`, non-external pipeline
// stages - refused loudly (lane 3). Only lane 2 leaves the process, so only
// lane 2 is an EFFECT; the other two are resolved inside the turn.
//
// exec-only, and argv is already expanded. That is #91's constraint, not a
// simplification: during editing the editor never forks, because fish's answer
// since 2.0 - "eliminate dangerous post-fork code" - is the one lesh took. A
// spawn request therefore carries what posix_spawn needs and nothing that would
// require a shell to exist between fork and exec.
struct spawn_request {
	std::vector<std::string> argv;
	generation computed_against;

	friend bool operator==(const spawn_request& a, const spawn_request& b) noexcept {
		return a.argv == b.argv && a.computed_against == b.computed_against;
	}
};

// The line is finished, and what happens next is the HOST's (#168).
//
// These four used to be a latch. An action asks for an outcome through the ABI
// (`lesh_accept_line`, `lesh_cancel_line`, `lesh_exit`, `lesh_recursive_edit`),
// and the request had to reach the driver - which on the keystroke path runs
// through `step`, a function of state and events that returns effects and
// nothing else. So the request was parked on `loop_harness` and the driver came
// back for it with `take_outcome`: a PULL, out of an editor object the host was
// not supposed to be reaching into. It is an effect now, returned by the turn
// that produced it, which is the one channel out of the editor there is.
//
// They carry what the latch carried and no more. `line_accepted` says nothing
// about WHICH line: the buffer is the editor's state and the host reads it
// there, as `accept_current_line` always has.
struct line_accepted {
	friend constexpr bool operator==(const line_accepted&, const line_accepted&) noexcept {
		return true;
	}
};

struct line_cancelled {
	friend constexpr bool operator==(const line_cancelled&, const line_cancelled&) noexcept {
		return true;
	}
};

// `lesh_exit`'s status, which the host reports as the shell's.
struct end_of_file {
	std::int32_t status = 0;

	friend constexpr bool operator==(const end_of_file& a, const end_of_file& b) noexcept {
		return a.status == b.status;
	}
};

// F-18's recovery shape, requested and not implemented in v1. It is an effect
// rather than a silence for the reason the other three are: the host is the one
// side that could ever honour it, and an outcome with no way out of the editor
// is an outcome that cannot be honoured later either.
struct recursive_edit_request {
	friend constexpr bool operator==(const recursive_edit_request&,
	                                 const recursive_edit_request&) noexcept {
		return true;
	}
};

// Arm a repeating timer (#168; #128 decision 3's ABI verbs, re-plumbed).
//
// THE INTERVAL TRAVELS AND THE DUE INSTANT DOES NOT, which was already the
// rule - leshper asks no clock (F-5) - but the declaration used to live in the
// registry as a table the driver walked every turn to diff against its own. Half
// a scheduler on each side. Now the arming is an EVENT'S WORTH OF NEWS, said
// once, and `{id, action, interval, due}` is the host's, whole.
//
// The action is an INTERNED HANDLE, not a name and not a pointer: an effect is a
// value on the channel out of the editor and carries no allocation (see
// `registry::timer_actions` for why the handle is an index). It is resolved to a
// name when the timer FIRES, not here, so a timer armed before its action is
// registered is still legal and re-registering the action still replaces what the
// timer runs - the late-binding rule a key follows.
struct arm_timer {
	std::uint64_t id = 0;
	std::uint64_t interval_ms = 0;
	std::uint32_t action = 0;

	friend constexpr bool operator==(const arm_timer& a, const arm_timer& b) noexcept {
		return a.id == b.id && a.action == b.action && a.interval_ms == b.interval_ms;
	}
};

struct disarm_timer {
	std::uint64_t id = 0;

	friend constexpr bool operator==(const disarm_timer& a, const disarm_timer& b) noexcept {
		return a.id == b.id;
	}
};

// NOTHING ON THIS CHANNEL ALLOCATES (N-2), and the compiler is what says so.
//
// An effect is emitted per turn and, for a timer, per expiry - once a second on a
// `{time}` prompt, forever. A `std::string` field would be a malloc and a free on
// each of those, which is why `arm_timer` carries an interned handle rather than
// the action's name.
//
// `spawn_request` is the ONE exception and is listed by name rather than left
// silent: it carries an already-expanded argv, it has no producer anywhere in the
// tree (#168 kept it as lane 2's future hook), and giving it a bounded shape is
// the work of the ticket that gives it a caller.
static_assert(std::is_trivially_copyable_v<render_request>);
static_assert(std::is_trivially_copyable_v<worker_request>);
static_assert(std::is_trivially_copyable_v<line_accepted>);
static_assert(std::is_trivially_copyable_v<line_cancelled>);
static_assert(std::is_trivially_copyable_v<end_of_file>);
static_assert(std::is_trivially_copyable_v<recursive_edit_request>);
static_assert(std::is_trivially_copyable_v<arm_timer>);
static_assert(std::is_trivially_copyable_v<disarm_timer>);

// What one turn of the state machine emits (A-2).
//
// A value, returned, rather than callbacks fired: the test harness A-2 requires
// from the first commit asserts on effects the same way it asserts on state,
// and a callback would have to be intercepted to be observed.
using effect = std::variant<render_request, worker_request, spawn_request, line_accepted,
                            line_cancelled, end_of_file, recursive_edit_request, arm_timer,
                            disarm_timer>;

// std::vector for now, and the hot path does not yet exist to measure it
// against. N-2 wants hot-path allocation bounded and jitter-free; #100's rule is
// that leshper's containers are decided from measured requirements rather than
// chosen preemptively, and the measurement belongs with the N-1 latency gate,
// which map #82 still carries as fog. A turn emits at most a handful of these,
// so a small-buffer type is the obvious later answer - later.
using effects = std::vector<effect>;

} // namespace lesh::leshper
