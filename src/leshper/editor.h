#pragma once

#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/state.h"

#include <cstdint>

namespace lesh::leshper {

// The built-in actions of the first editing slice (F-1 subset).
//
// Named, because F-13 says every built-in behaviour is a named action and
// nothing is unnamed or unrebindable. An enum is NOT the action registry #93
// will build - that one is language-neutral (NG-4), holds native and lesh-script
// actions side by side, and is looked up by name at runtime. This enum is the
// placeholder standing in its doorway, and it is deliberately small: the whole
// slice is insert, delete, move, undo.
enum class action : uint8_t {
	none, // the key is bound to nothing
	self_insert,
	delete_backward_char,
	delete_backward_word,
	backward_char,
	forward_char,
	beginning_of_line,
	end_of_line,
	undo,
	redo,
};

// The name of an action (F-13). The string is what #93's registry will key on,
// so it is written the way a user will type it into a binding.
[[nodiscard]] const char* name_of(action a) noexcept;

// The default keymap, as a function rather than as data (A-8, F-8).
//
// PLACEHOLDER, and the largest one in this module. Keymaps are first-class
// mutable data in the design - created, copied, pushed and popped at runtime,
// with modal input being a stack of them and never a second dispatch system -
// and that machinery is #93's, downstream of the action ABI. A hardcoded
// key-to-action table is what stands here until then. It is exposed so tests can
// assert the binding separately from the behaviour, which is the seam #93 will
// cut along.
[[nodiscard]] action binding_for(const key_event& key) noexcept;

// One turn of the state machine (A-2).
//
// The whole editor is this function: an event goes in, the state is advanced in
// place, and the effects the loop must carry out come back as a value. No
// terminal is touched, no file is read, no worker is waited on, and nothing is
// called back - which is why the test harness needs nothing but a state and a
// list of events.
//
// Mutating in place rather than returning a new state: A-2 writes the signature
// as `(state, event) -> (state, effects)`, and the state is a struct with a
// buffer in it. Copying one per keystroke to honour the notation literally would
// buy nothing that matters and cost N-1's millisecond.
effects step(state& current, const event& incoming);

} // namespace lesh::leshper
