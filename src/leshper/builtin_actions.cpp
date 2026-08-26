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
// through a switch, and the first nine below are the same behaviours written
// against the ABI. The keymap stack that makes this the only implementation is
// the rest of #93's work; until it lands, LeshperAbiEquivalence in the unit
// tests asserts the two paths agree on buffer, cursor, generation and undo
// history, so the duplication is caught by a test rather than by a reader.
//
// The three accepting actions (#133) have no twin on the enum path and never
// will: there is nothing for a switch to dispatch to, because what they act on
// is a reactor's proposal and the enum path has no reactors.

#include "leshper/abi.h"

#include <cstddef>
#include <cstdint>

namespace {

// The longest text these actions will copy through a stack buffer. A stack
// buffer, because an action runs on the keystroke path and the alternative is a
// heap allocation per keypress; a command line past 4 KiB is a paste, and a
// paste is not something anyone is autosuggesting or killing a word out of.
// Past it a read answers LESH_ERR_TOOSMALL, which is the honest "the bytes are
// there and they did not fit" rather than a truncation that would mean
// something else.
constexpr size_t kCandidateBytes = 4096;

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

// emacs's `backward-kill-word`, and the word KILL is the operative half: #99
// answer 3 says every kill feeds the one store, and this is the emacs side's
// only killing built-in. `delete_backward_char` deliberately does not - emacs's
// backspace does not kill either, and a store that filled up with single
// characters would make `C-y` useless.
int32_t delete_backward_word(lesh_editor* editor, const lesh_invocation*, void*) {
	size_t cursor = 0;
	int32_t status = lesh_cursor_get(editor, &cursor);
	if (status != LESH_OK)
		return status;
	size_t from = 0;
	status = lesh_position_move(editor, cursor, LESH_MOTION_PREV_WORD, &from);
	if (status != LESH_OK)
		return status;
	if (from >= cursor)
		return LESH_OK;
	// A word longer than the scratch is deleted and not stored, rather than
	// stored truncated: half a word in the register is worse than none, and a
	// `C-w` over 4 KiB of one word is a paste being unpicked, not editing.
	char killed[kCandidateBytes];
	size_t length = 0;
	if (lesh_buffer_read(editor, from, cursor, killed, sizeof(killed), &length) == LESH_OK)
		lesh_kill_set(editor, nullptr, killed, length, LESH_KILL_CHARWISE);
	return lesh_buffer_replace(editor, from, cursor, nullptr, 0);
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

// --- Accepting an autosuggestion (#133, F-25) -------------------------------
//
// A proposal becomes buffer text HERE and nowhere else. The reactor that found
// it cannot write - there is no apply path on a request token - and these three
// write the way every other action does, by staging (A-12), so accepting a
// suggestion is one undo entry and one generation bump and undoing it puts back
// exactly what the user had typed.
//
// Nothing here knows what a history is. `lesh_proposal_read` answers with bytes
// under a kind; whether they came from history (#125), from a completer, or
// from a plugin nobody has written yet is the proposing reactor's business.

// Reads the suggestion currently on screen. `*length` is its full length.
//
// LESH_ERR_NOTFOUND when nothing is suggested, which every caller below turns
// into LESH_OK having done nothing: pressing the accept key with no suggestion
// showing is the ordinary case, not an error - the same rule undo already
// follows for pressing undo once too often.
int32_t read_suggestion(lesh_editor* editor, char* out, size_t capacity, size_t* length) {
	return lesh_proposal_read(editor, LESH_PROPOSAL_AUTOSUGGESTION, 0, out, capacity, length);
}

int32_t accept_autosuggestion(lesh_editor* editor, const lesh_invocation*, void*) {
	char candidate[kCandidateBytes];
	size_t length = 0;
	int32_t status = read_suggestion(editor, candidate, sizeof(candidate), &length);
	if (status == LESH_ERR_NOTFOUND)
		return LESH_OK;
	if (status != LESH_OK)
		return status;

	// The WHOLE candidate, not the buffer plus a tail. The proposal is what the
	// line becomes, and a batch is applied only against the generation it was
	// computed for (N-4), so the bytes on screen and the bytes here agree by
	// construction. Appending a tail instead would be this action deriving the
	// answer a second time from two halves - and getting it wrong exactly when
	// they had drifted, which is the case worth being right about.
	//
	// The commit diffs this against the buffer and records the one replacement
	// that explains the difference, so "set the whole line" is still one
	// insertion at the end in the undo history.
	status = lesh_buffer_set(editor, candidate, length);
	if (status != LESH_OK)
		return status;
	// lesh_buffer_set deliberately leaves the cursor where it was ($BUFFER= in
	// the lesh binding), and accepting a suggestion is exactly the case that
	// wants it at the end.
	return lesh_cursor_set(editor, length);
}

int32_t accept_autosuggestion_word(lesh_editor* editor, const lesh_invocation*, void*) {
	char candidate[kCandidateBytes];
	size_t length = 0;
	int32_t status = read_suggestion(editor, candidate, sizeof(candidate), &length);
	if (status == LESH_ERR_NOTFOUND)
		return LESH_OK;
	if (status != LESH_OK)
		return status;

	size_t typed = 0;
	status = lesh_buffer_length(editor, &typed);
	if (status != LESH_OK)
		return status;
	if (length <= typed)
		return LESH_OK;  // nothing past what is typed: nothing to accept

	// WHERE THE NEXT WORD ENDS, asked of the editor rather than worked out here.
	//
	// The trick is that the staging area is what `lesh_position_move` reads, so
	// staging the whole candidate first puts the suggested text somewhere the
	// editor's own segmentation can be pointed at, and the truncation that
	// follows is a second staged write in the same action. One undo entry either
	// way (A-12), and no second word-boundary rule anywhere in the tree - which
	// is the whole reason the motion is on the ABI at all.
	status = lesh_buffer_set(editor, candidate, length);
	if (status != LESH_OK)
		return status;
	size_t boundary = 0;
	status = lesh_position_move(editor, typed, LESH_MOTION_NEXT_WORD, &boundary);
	if (status != LESH_OK)
		return status;
	// A motion that did not move means there is no further word - trailing
	// blanks, or a continuation that is one unbroken run already consumed. Take
	// the rest, which is what the user asked for and what accept-whole would
	// have done.
	if (boundary <= typed)
		boundary = length;
	if (boundary < length) {
		status = lesh_buffer_replace(editor, boundary, length, nullptr, 0);
		if (status != LESH_OK)
			return status;
	}
	return lesh_cursor_set(editor, boundary);
}

int32_t dismiss_autosuggestion(lesh_editor* editor, const lesh_invocation*, void*) {
	// Requested, never performed: the loop drops the batch once this action's
	// writes have been committed. Answers LESH_OK with nothing showing, because
	// dismissing nothing is not an error either.
	return lesh_proposal_dismiss(editor, LESH_PROPOSAL_AUTOSUGGESTION);
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
	// F-25's three. NOT BOUND TO ANYTHING YET, and deliberately: the default
	// keymap is #118's, and a name registered with no key is exactly what an rc
	// file binds. Fish's defaults are Right/Ctrl-F for the whole, Alt-Right/
	// Alt-F for the word, and Escape or Ctrl-C for the dismissal.
	{"accept_autosuggestion", accept_autosuggestion},
	{"accept_autosuggestion_word", accept_autosuggestion_word},
	{"dismiss_autosuggestion", dismiss_autosuggestion},
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
