#pragma once

// Running ONE reactor, here, now, on the calling thread (#135).
//
// THE TOKEN MINT THAT IS NOT THE EDITOR'S. `registry.cpp` builds a
// `request_token` for `loop_harness::react`, which is the editor dispatching a
// reactor on its own stack; this is the host's, and it is the only other one.
// Every reactor the shell runs is minted here, on the one thread, from a fiber's
// stack. It is not a second reactor PATH: it builds exactly the `request_token`
// registry.cpp builds, and asserts `token_is_live` on it, which is what says so.
//
// THE SNAPSHOT LIVES HERE TOO, because the call below is its one reader.

#include "leshper/abi.h"
#include "leshper/host.h"
#include "leshper/registry.h"
#include "leshper/state.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lesh::ui {

// ---------------------------------------------------------------------------
// The snapshot a request is computed against.
//
// Buffer, cursor, selection and generation - what spec §6.1 says the token
// carries, and nothing else. Taken on the host's thread at the moment a reactor
// is notified and owned from there, so the editor may move on the instant the
// notification returns.
// ---------------------------------------------------------------------------

struct request_snapshot {
	std::string buffer;
	std::size_t cursor = 0;
	std::size_t selection_start = 0;
	std::size_t selection_end = 0;
	bool selection_active = false;
	leshper::generation computed_against;
	std::uint32_t event_kind = 0;

	// What the shell knows (#135), for the reactor that asks - today only the
	// highlighter, and only for `command_kind`. NOT copied and not owned: a
	// pointer to the wiring site's adapter over `shell_state`, which outlives
	// every request.
	//
	// ADR-0011 is what makes a bare pointer safe here. The shell owns
	// `shell_state`; a highlight, a port call that writes it, and an execution are
	// serialized on the one thread - a highlight is a fiber ON that thread, which
	// the host resumes only while it is not inside either writer. So this points at
	// state that cannot change while the compute it belongs to is running, which is
	// why #130's copy-on-write definitions version was deleted rather than kept as
	// insurance.
	//
	// Null - the default - is "no host attached": every name classifies as
	// LESH_COMMAND_UNKNOWN. A notification that leaves it null is therefore honest
	// rather than broken, and is what every state-free reactor gets.
	const leshper::host* host = nullptr;
};

// The snapshot of an editor state, as the host would take it.
//
// Selection reads as inactive because #96 has not landed the model; the fields
// exist so that filling them in later is a change here and nowhere else.
[[nodiscard]] request_snapshot snapshot_of(const leshper::state& target,
                                          std::uint32_t event_kind);

// The same, INTO STORAGE THE CALLER ALREADY HAS. `into.buffer` is assigned
// rather than replaced, so a snapshot taken into the same object twice allocates
// only when the line grows past what that object already holds. `snapshot_of` is
// this plus a fresh object, and the two agree field for field - `host` included,
// which is reset rather than left as the caller found it.
void take_snapshot(request_snapshot& into, const leshper::state& target,
                   std::uint32_t event_kind);

// ---------------------------------------------------------------------------
// The call
// ---------------------------------------------------------------------------

// WHO TO YIELD TO, WHILE THE REACTOR IS COMPUTING.
//
// Stamped onto the token, where `lesh_request_superseded` calls it before it
// reads the cancellation flag - see `lesh_request::cooperate` for the whole
// argument. The default, and the only value any caller outside a fiber passes,
// is the empty one: "there is nobody to yield to".
struct reactor_cooperation {
	void (*yield)(void* userdata) = nullptr;
	void* userdata = nullptr;
};

// Runs one reactor against a snapshot ON THE CALLING THREAD, into `into`.
//
// `superseded` is the flag the reactor's cooperative poll reads; it must outlive
// the call.
//
// `snapshot` IS BORROWED AND GIVEN BACK, not consumed. Its buffer moves into the
// token so the bytes are not copied twice, and moves back when the reactor
// returns - so a caller that serves out of a long-lived member keeps the string
// it grew instead of watching the token's destructor free it. Nothing points into
// it afterwards: a reactor's view of the buffer is valid for its call and no
// longer. Every other field is read, not taken.
//
// `cooperate` is what the poll yields to. Empty means the reactor runs to
// completion on this stack, which is what every caller that is not a fiber
// wants - and what the allocation gate measures, so the parameter is defaulted
// rather than threaded through call sites that have no answer for it.
void run_reactor_here(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
                      request_snapshot& snapshot, const std::atomic<bool>& superseded,
                      leshper::reactor_batch& into, reactor_cooperation cooperate = {});

} // namespace lesh::ui
