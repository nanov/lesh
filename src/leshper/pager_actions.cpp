// The pager's actions, registered through the ABI and reachable by no other
// route (#138, ADR-0008, spec §6.9).
//
// LOOK AT THE INCLUDES, the same way builtin_actions.cpp and vi.cpp ask you to.
// This file sees `leshper/abi.h` and nothing else from leshper - not state.h,
// not pager.h, not surface.h. It cannot touch `pager_state`, cannot push the
// pager's keymap, and cannot decide what an accepted candidate inserts; every
// one of those is a door abi.h opens, which means a Lua binding can write this
// file's contents and a plugin can replace any action in it.
//
// THAT IS WHY THE PAGER IS TWO TRANSLATION UNITS. pager.cpp renders and decides
// and must see `state` and `surface` to do it; this one must NOT see them, or
// the compiler stops being what holds the boundary. Splitting them is the
// cheapest way to have both, and registry.h says so where the declaration
// crosses.
//
// #117 CALLED THIS ONE OUT BY NAME: "the pager needs zero special dispatch".
// Nothing below is reached by a branch in editor.cpp. The pager's keymap is
// opaque with a default action, and that is the whole mechanism - unbound
// printables arrive at `pager_filter_key` through the same per-keymap default
// that hands `f` its target character in vi.

#include "leshper/abi.h"

#include <cstddef>
#include <cstdint>

namespace {

// The longest candidate these actions will copy through a stack buffer, and the
// same number builtin_actions.cpp picked for the same reason: an action runs on
// the keystroke path and the alternative is a heap allocation per keypress. A
// completion candidate past 4 KiB is not a candidate anybody is reading.
constexpr size_t kCandidateBytes = 4096;

// --- Moving ----------------------------------------------------------------

int32_t move_by(lesh_editor* editor, int64_t by, uint32_t axis) {
	const int32_t status = lesh_pager_move(editor, by, axis);
	// A move with no pager is not an error: a key bound in the pager's map can
	// only arrive while it is pushed, and a user who bound one elsewhere gets
	// nothing rather than a beep.
	return status == LESH_ERR_NOTFOUND ? LESH_OK : status;
}

int32_t pager_next(lesh_editor* editor, const lesh_invocation*, void*) {
	return move_by(editor, 1, LESH_PAGER_BY_CANDIDATE);
}

int32_t pager_previous(lesh_editor* editor, const lesh_invocation*, void*) {
	return move_by(editor, -1, LESH_PAGER_BY_CANDIDATE);
}

int32_t pager_next_row(lesh_editor* editor, const lesh_invocation*, void*) {
	return move_by(editor, 1, LESH_PAGER_BY_ROW);
}

int32_t pager_previous_row(lesh_editor* editor, const lesh_invocation*, void*) {
	return move_by(editor, -1, LESH_PAGER_BY_ROW);
}

// --- Accepting and closing --------------------------------------------------

// A-12's accepting action, and it is one call because the decision it carries
// out - what a candidate of this kind inserts - belongs to the editor, where
// F-30's common-prefix insertion also reads it. Two copies of the trailing-byte
// rule would be two answers to "does a directory get a slash".
int32_t pager_accept(lesh_editor* editor, const lesh_invocation*, void*) {
	const int32_t status = lesh_pager_accept(editor);
	return status == LESH_ERR_NOTFOUND ? LESH_OK : status;
}

int32_t pager_close(lesh_editor* editor, const lesh_invocation*, void*) {
	return lesh_pager_close(editor);
}

// --- Filtering (F-29) -------------------------------------------------------

// The keymap's DEFAULT ACTION, which is the whole of F-29's mechanism. #117
// decision 4 built the per-keymap default for exactly this: an unbound printable
// reaches the filter because the pager's map is opaque and names a default, and
// editor.cpp needed no branch to make it so.
//
// `keys` is the text the sequence would have typed - a named key contributes
// nothing, so `<F5>` bound to nothing filters by nothing rather than by garbage.
int32_t pager_filter_key(lesh_editor* editor, const lesh_invocation* how, void*) {
	if (how == nullptr || how->keys == nullptr || how->keys_length == 0)
		return LESH_OK;
	const int32_t status = lesh_pager_filter_push(editor, how->keys, how->keys_length);
	return status == LESH_ERR_NOTFOUND ? LESH_OK : status;
}

// Backspace: shorten the filter, and CLOSE when there is nothing left to
// shorten. Closing is the decision - a pager whose filter is empty is a pager
// the user has backed out of, and leaving it open would make Backspace a key
// that does nothing at exactly the moment it looks like it should undo the
// opening.
int32_t pager_filter_backspace(lesh_editor* editor, const lesh_invocation*, void*) {
	if (lesh_pager_filter_pop(editor) == LESH_ERR_NOTFOUND)
		return lesh_pager_close(editor);
	return LESH_OK;
}

// --- The three clients (#137 decision 3: one pager, three clients) ----------

// Fills the pager from the proposals of one kind and commits it.
//
// This is what makes "one pager, three clients" a fact rather than a claim: the
// three actions below differ in a proposal kind and a span, and in nothing else.
// A reactor proposed; an action decides (ADR-0008's asymmetry) - and the
// deciding is F-30's, taken by `lesh_pager_commit` on everyone's behalf.
int32_t show_proposals(lesh_editor* editor, uint32_t proposal_kind, uint32_t candidate_kind,
                       size_t from, size_t to) {
	int32_t status = lesh_pager_open(editor, from, to);
	if (status != LESH_OK)
		return status;

	char candidate[kCandidateBytes];
	for (size_t index = 0;; ++index) {
		size_t length = 0;
		status = lesh_proposal_read(editor, proposal_kind, index, candidate,
		                            sizeof(candidate), &length);
		if (status == LESH_ERR_NOTFOUND)
			break;
		// A candidate too long to copy is SKIPPED rather than truncated: half a
		// pathname inserted into a command line is worse than an absent one.
		if (status == LESH_ERR_TOOSMALL)
			continue;
		if (status != LESH_OK)
			return status;
		status = lesh_pager_add(editor, candidate, length, candidate_kind);
		if (status != LESH_OK)
			return status;
	}
	return lesh_pager_commit(editor, nullptr);
}

int32_t pager_show_completions(lesh_editor* editor, const lesh_invocation*, void*) {
	size_t cursor = 0;
	const int32_t status = lesh_cursor_get(editor, &cursor);
	if (status != LESH_OK)
		return status;
	// `[cursor, cursor)` is the honest default and not the last word: a
	// completer knows the token it classified (C-6) and opens the pager over
	// that span itself. This action exists for a reactor that proposed
	// completions with no completer in front of it.
	return show_proposals(editor, LESH_PROPOSAL_COMPLETION, LESH_PAGER_WORD, cursor, cursor);
}

int32_t pager_show_history_matches(lesh_editor* editor, const lesh_invocation*, void*) {
	size_t length = 0;
	const int32_t status = lesh_buffer_length(editor, &length);
	if (status != LESH_OK)
		return status;
	// A history entry replaces the WHOLE line, and nothing trails it - which is
	// what LESH_PAGER_PLAIN means and why the kind is not a file-type set.
	return show_proposals(editor, LESH_PROPOSAL_HISTORY_MATCH, LESH_PAGER_PLAIN, 0, length);
}

int32_t pager_show_suggestions(lesh_editor* editor, const lesh_invocation*, void*) {
	size_t cursor = 0;
	const int32_t status = lesh_cursor_get(editor, &cursor);
	if (status != LESH_OK)
		return status;
	// A suggestion is a CONTINUATION: it is inserted at the cursor and replaces
	// nothing, which is the same span an empty completion uses and a different
	// meaning for it.
	return show_proposals(editor, LESH_PROPOSAL_AUTOSUGGESTION, LESH_PAGER_PLAIN, cursor,
	                      cursor);
}

struct builtin {
	const char* name;
	lesh_action_fn fn;
};

// snake_case, per #93.
//
// NOT BOUND OUTSIDE THE PAGER'S OWN KEYMAP, and deliberately. The keys that
// OPEN a pager belong to the completer (#139) and to the suggestion-binding
// ticket (#140); what is decided here is only what the keys inside one mean.
constexpr builtin builtins[] = {
	{"pager_next", pager_next},
	{"pager_previous", pager_previous},
	{"pager_next_row", pager_next_row},
	{"pager_previous_row", pager_previous_row},
	{"pager_accept", pager_accept},
	{"pager_close", pager_close},
	{"pager_filter_key", pager_filter_key},
	{"pager_filter_backspace", pager_filter_backspace},
	{"pager_show_completions", pager_show_completions},
	{"pager_show_history_matches", pager_show_history_matches},
	{"pager_show_suggestions", pager_show_suggestions},
};

} // namespace

// Out of line rather than in registry.h, so that this file needs no leshper
// header but abi.h. Declared in registry.h; see the note at the top.
namespace lesh::leshper {

std::size_t register_pager_actions(lesh_registry& reg) {
	std::size_t registered = 0;
	for (const builtin& one : builtins) {
		if (lesh_action_register(&reg, one.name, one.fn, nullptr) == LESH_OK)
			++registered;
	}
	return registered;
}

} // namespace lesh::leshper
