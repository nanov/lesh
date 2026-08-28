#pragma once

// A-5: what the editing session needs from the shell, and nothing more
// (ADR-0009, #134, #136).
//
// THIS WAS THE BOTTOM HALF OF `shell_actor.h` UNTIL #201, and it is what
// survived that file: the actor, its three latest-wins slots, its condition
// variable and the `shell` topic they answered on are gone, because there is no
// second thread to reach across. `event_loop` calls the two methods below
// DIRECTLY, on the one thread the interactive shell has, and this interface is
// unchanged - the same two methods, the same contract, the same twelve-line
// fake in every test.
//
// WHY IT IS STILL AN INTERFACE now that both sides are in `lesh_ui` and the
// call is a call. Not a link rule - `lesh_ui` links `lesh_runtime` - but the
// reason it was written for: THE LOOP MUST BE DRIVABLE WITH NO SHELL BEHIND IT,
// which is what every test in `ui_loop_tests.cpp` does. A direct call into
// `shell_state` would make a shell a prerequisite for editing at all.
//
// TWO METHODS, and their narrowness is the decision. Everything else the shell
// knows stays HOST-SIDE in `shell_knowledge` - stamped on the token for a
// reactor, read directly by the completer - and never crosses to the editor as
// itself; what crosses is `leshper::host`, the one door, and never a call. What
// is here is only what has to BE a call, and the rule that decides it is that
// both of these RUN CODE: they change the world, and the world is the shell's.
// #139 added a third for the completer's name list and #151 took it away again,
// once the owner's reading of ADR-0009 made the direct read legal - which is the
// rule holding rather than being bent.

#include <cstdint>
#include <string_view>

namespace lesh::ui {

// Both run ON THE CALLING THREAD, synchronously, with nothing else of the
// shell's running - and since #201 the calling thread is the loop's, which is
// main. Neither may touch the terminal: the loop has restored the modes and
// given up the foreground group before `execute` is called, and an action's
// `port_call` runs with the EDITOR'S modes still in force - fish #7770, never
// re-enable ECHO around an action's shell code.
class shell_side {
public:
	virtual ~shell_side() = default;

	// Runs an accepted command line to completion. Answers its exit status.
	//
	// The fork happens inside here, on this thread, which is the main thread and
	// since #202 the only one - and it is legal because the loop has already
	// parked the emitters and given the terminal back before it made the call. The child claims the terminal
	// itself between fork and exec, so nothing here needs anybody's cooperation
	// on process groups.
	virtual std::int32_t execute(std::string_view line) = 0;

	// #92's port: runs an action's shell code. Answers its status.
	//
	// Lane discipline is the implementation's, not this interface's: builtins
	// and functions in-process, externals through spawn, fork-requiring forms
	// refused loudly. What this seam fixes is only that the call is synchronous
	// from the action's point of view, which is #92's contract - and since #201
	// it is synchronous by being a call rather than by a round trip that ends in
	// one.
	virtual std::int32_t port_call(std::string_view code) = 0;
};

} // namespace lesh::ui
