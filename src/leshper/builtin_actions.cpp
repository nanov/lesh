// The built-in actions, registered through the ABI and reachable by no other
// route (#93, ADR-0008).
//
// LOOK AT THE INCLUDES. This file sees `leshper/abi.h` and nothing else from
// leshper - not state.h, not text.h, not registry.h. It cannot reach the buffer
// except by copying it out, cannot move the cursor except by staging a write,
// and cannot ask where a grapheme cluster ends except by asking the editor. It
// is a binding written in C++, held to exactly the surface the Lua binding will
// get, by the compiler rather than by anyone's care.
//
// That is what "no native side door" means, and it is why the friction is worth
// paying: ADR-0008 measured the copy-accessor cost as noise against N-1's
// millisecond and took it, so that the ABI is exercised by the thing that runs
// on every keystroke instead of rotting until the first plugin arrives.
//
// The one place the shortcut is visible: `register_builtin_actions` is declared
// in registry.h so the loop can call it, and defined here. The declaration
// crosses; nothing else does.
//
// NOT WIRED TO THE KEYMAP YET. editor.cpp still dispatches its `action` enum
// through a switch, and these are the same nine behaviours written against the
// ABI. The keymap stack that makes this the only implementation is the rest of
// #93's work; until it lands, LeshperAbiEquivalence in the unit tests asserts
// the two paths agree on buffer, cursor, generation and undo history, so the
// duplication is caught by a test rather than by a reader.

#include "leshper/abi.h"

#include <cstddef>
#include <cstdint>

namespace {

// Every action here is: read the cursor, ask the editor a geometry question,
// stage one write. Two accessor calls and a motion, per keystroke, which is the
// friction the ADR priced.

int32_t cursor_to(lesh_editor* editor, lesh_motion motion) {
	size_t cursor = 0;
	int32_t status = lesh_cursor_get(editor, &cursor);
	if (status != LESH_OK)
		return status;
	size_t target = 0;
	status = lesh_position_move(editor, cursor, motion, &target);
	if (status != LESH_OK)
		return status;
	return lesh_cursor_set(editor, target);
}

// Deletes from the cursor back to wherever `motion` lands. Answers LESH_OK
// having done nothing when the motion did not move - backspace at the start of
// the line is the ordinary case, not an error.
int32_t delete_back_to(lesh_editor* editor, lesh_motion motion) {
	size_t cursor = 0;
	int32_t status = lesh_cursor_get(editor, &cursor);
	if (status != LESH_OK)
		return status;
	size_t from = 0;
	status = lesh_position_move(editor, cursor, motion, &from);
	if (status != LESH_OK)
		return status;
	if (from >= cursor)
		return LESH_OK;
	return lesh_buffer_replace(editor, from, cursor, nullptr, 0);
}

int32_t self_insert(lesh_editor* editor, const lesh_invocation* invocation, void*) {
	if (invocation == nullptr)
		return LESH_ERR_INVAL;
	if (invocation->keys == nullptr || invocation->keys_length == 0)
		return LESH_OK;  // invoked without a key: there is nothing to insert
	size_t cursor = 0;
	const int32_t status = lesh_cursor_get(editor, &cursor);
	if (status != LESH_OK)
		return status;
	return lesh_buffer_replace(editor, cursor, cursor,
	                           invocation->keys, invocation->keys_length);
}

int32_t delete_backward_char(lesh_editor* editor, const lesh_invocation*, void*) {
	return delete_back_to(editor, LESH_MOTION_PREV_CLUSTER);
}

int32_t delete_backward_word(lesh_editor* editor, const lesh_invocation*, void*) {
	return delete_back_to(editor, LESH_MOTION_PREV_WORD);
}

int32_t backward_char(lesh_editor* editor, const lesh_invocation*, void*) {
	return cursor_to(editor, LESH_MOTION_PREV_CLUSTER);
}

int32_t forward_char(lesh_editor* editor, const lesh_invocation*, void*) {
	return cursor_to(editor, LESH_MOTION_NEXT_CLUSTER);
}

int32_t beginning_of_line(lesh_editor* editor, const lesh_invocation*, void*) {
	return cursor_to(editor, LESH_MOTION_LINE_START);
}

int32_t end_of_line(lesh_editor* editor, const lesh_invocation*, void*) {
	return cursor_to(editor, LESH_MOTION_LINE_END);
}

int32_t undo_action(lesh_editor* editor, const lesh_invocation*, void*) {
	return lesh_undo(editor);
}

int32_t redo_action(lesh_editor* editor, const lesh_invocation*, void*) {
	return lesh_redo(editor);
}

struct builtin {
	const char* name;
	lesh_action_fn fn;
};

// snake_case, per #93. The names are what a user types into a binding, what an
// rc file re-sources idempotently, and what `.name` shadows.
constexpr builtin builtins[] = {
	{"self_insert", self_insert},
	{"delete_backward_char", delete_backward_char},
	{"delete_backward_word", delete_backward_word},
	{"backward_char", backward_char},
	{"forward_char", forward_char},
	{"beginning_of_line", beginning_of_line},
	{"end_of_line", end_of_line},
	{"undo", undo_action},
	{"redo", redo_action},
};

} // namespace

// Out of line rather than in registry.h, so that this file needs no leshper
// header but abi.h. Declared in registry.h; see the note at the top.
namespace lesh::leshper {

std::size_t register_builtin_actions(lesh_registry& reg) {
	std::size_t registered = 0;
	for (const builtin& one : builtins) {
		if (lesh_action_register(&reg, one.name, one.fn, nullptr) == LESH_OK)
			++registered;
	}
	return registered;
}

} // namespace lesh::leshper
