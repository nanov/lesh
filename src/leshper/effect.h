#pragma once

#include "leshper/state.h"

#include <string>
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

// What one turn of the state machine emits (A-2).
//
// A value, returned, rather than callbacks fired: the test harness A-2 requires
// from the first commit asserts on effects the same way it asserts on state,
// and a callback would have to be intercepted to be observed.
using effect = std::variant<render_request, worker_request, spawn_request>;

// std::vector for now, and the hot path does not yet exist to measure it
// against. N-2 wants hot-path allocation bounded and jitter-free; #100's rule is
// that leshper's containers are decided from measured requirements rather than
// chosen preemptively, and the measurement belongs with the N-1 latency gate,
// which map #82 still carries as fog. A turn emits at most a handful of these,
// so a small-buffer type is the obvious later answer - later.
using effects = std::vector<effect>;

} // namespace lesh::leshper
