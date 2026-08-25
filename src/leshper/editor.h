#pragma once

#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/state.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lesh::leshper {

// The keymap path's two entry points beside `step`, both the loop's.
//
// #118 deleted what used to stand here: an `action` enum, a `name_of` for it and
// a `binding_for` that was a hardcoded switch over key events. All three were
// placeholders for the keymap stack, and the stack has landed - dispatch resolves
// an action NAME through the keymap registry and invokes it through #110's action
// registry, which makes builtin_actions.cpp the one implementation of the nine
// built-ins rather than the second one.

// F-5's prefix-hold, resolved by TIME rather than by the next key.
//
// leshper owns no clock (the rule decode.h states and this obeys): the loop, which
// read the bytes and therefore knows when they arrived, passes `now` into `step`
// and gets a deadline back from `keymap_deadline`. When that deadline passes with
// nothing more typed, this is where the held sequence resolves - to its own exact
// match if it has one, and to nothing if it does not.
//
// Takes `now` and checks it rather than trusting the caller, for the reason
// input_decoder::expire gives: an early wake must not resolve a hold that was
// still legitimately in flight.
effects keymap_expire(state& current, std::chrono::steady_clock::time_point now);

// When keymap_expire must be called if nothing more is typed; nullopt when
// nothing is being held.
[[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
keymap_deadline(const state& current) noexcept;

// The one buffer mutation (F-1, F-4, A-10).
//
// Does the three things that must never be done separately: change the buffer,
// record how to undo it, bump the generation. Every buffer mutation in leshper
// goes through here, which is what makes "exactly one generation bump per
// mutating action" a property of the code rather than of the author's memory.
//
// Declared rather than kept private because it has a second caller: #93's ABI
// commit (registry.cpp), which diffs an action's staged writes into one
// replacement and lands it here. A private copy over there would be a second
// mutation path, and a second mutation path is the whole thing this function
// exists to prevent.
//
// `cursor_after` is where the cursor ends up, and it is recorded on the undo
// entry so F-4's "undo restores text AND cursor" survives. Null means the
// natural landing - the end of the replacement - which is what every caller on
// the keymap path wants; the ABI commit passes the cursor the action asked for.
void apply_edit(state& current, position from, position to, std::string_view with,
                const position* cursor_after = nullptr);

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
// `now` is when this event arrived, and leshper uses it for exactly one thing:
// anchoring the deadline of a prefix-hold that this key begins (F-5). Never a
// clock call - the same discipline decode.h is written to, and for the same
// reason, which is that a state machine reading a clock cannot be replayed.
//
// Defaulted to nullopt so that a test and the N-3 replay harness, neither of
// which has a clock, still drive the editor: a hold begun with no instant simply
// never expires on time, and is resolved by the next key instead.
effects step(state& current, const event& incoming,
             std::optional<std::chrono::steady_clock::time_point> now = std::nullopt);

} // namespace lesh::leshper
