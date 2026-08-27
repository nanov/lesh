#pragma once

// Running ONE reactor, here, now, on the calling thread (#135, #201).
//
// THE TOKEN MINT THAT IS NOT THE WORKER POOL'S. `workers.cpp` builds a
// `request_token` for a reactor it runs on a helper thread and `registry.cpp`
// builds one for `loop_harness::react`; this is the third, and it exists because
// the reactor that reads shell state is run on the thread that OWNS shell state.
// Until #201 that thread was the shell's and `shell_actor` called this from a
// slot; the actor is gone and `event_loop::notify_reactors` calls it in place,
// which is the same sentence with one fewer thread in it.
//
// WHAT IT IS NOT is a second reactor path - it builds exactly the
// `request_token` registry.cpp and workers.cpp build, and asserts `token_is_live`
// on it for the same reason workers.cpp does: the thread key is computed in three
// places, and a disagreement would make every accessor on the token refuse.

#include "leshper/abi.h"
#include "leshper/registry.h"
#include "ui/workers.h"

#include <atomic>
#include <string_view>

namespace lesh::ui {

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
void run_reactor_here(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
                      request_snapshot& snapshot, const std::atomic<bool>& superseded,
                      leshper::reactor_batch& into);

} // namespace lesh::ui
