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
// The accepting actions (#133, and #149's per-character one) have no twin on the
// enum path and never will: there is nothing for a switch to dispatch to,
// because what they act on is a reactor's proposal and the enum path has no
// reactors.

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

// emacs's `forward-word`, and LESH_MOTION_NEXT_WORD is the motion that means it:
// blank-separated, landing PAST the end of the word rather than on its last
// byte, which is the difference abi.h names between the two word families.
//
// Registered but bound to nothing until #140's table below reaches it through
// `accept_suggestion_or_forward_word` - `<A-f>` had no binding before, and the
// wrapper needs a name to delegate to rather than a motion of its own.
int32_t forward_word(lesh_editor* editor, const lesh_invocation*, void*) {
	return cursor_to(editor, LESH_MOTION_NEXT_WORD);
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

// fish's `forward-single-char` (fish 3.5), and DELIBERATELY BOUND TO NO KEY -
// #149, which is fish's own answer too: fish registers the function and ships no
// binding for it, because the key it would want is `<Right>`, and `<Right>` is
// already the accept-whole. Registered and one
// `bind '<A-Right>' accept_suggestion_char_or_forward_char` away, which is what
// a name with no key is for. The wrapper row below carries the same note.
//
// ONE GRAPHEME CLUSTER PER PRESS, never one byte. The trick is the word twin's:
// stage the whole candidate first, so the suggested text is somewhere the
// editor's own segmentation can be pointed at, then ask `LESH_MOTION_NEXT_CLUSTER`
// where the cluster after the typed prefix ends and truncate there. Two staged
// writes, one undo entry (A-12), and no second boundary rule in the tree - a
// combining sequence, a flag and a ZWJ emoji are each one press because F-3's
// unit says so and this action never learns what a cluster is.
//
// COALESCING: this breaks the typing run, like every other block write, and that
// is #146's RULE READ OFF AS WRITTEN rather than a new decision here. The
// discriminator asks whether the commit wears all three marks of a keystroke -
// plain insertion at a point, exactly one cluster, and the inserted bytes ARE
// the key that dispatched the action. A one-cluster accept wears the first two.
// It fails the third: the cluster comes off the candidate, and the key that
// dispatched it (`<A-Right>`, or whatever a user binds) contributes different
// bytes - usually none at all. So it is a block write, one undo step of its own,
// and the run breaks on both sides. The tempting exception - "this one is
// byte-identical to typing that cluster, let it join" - is exactly the reasoning
// #146 rejected when it made the third mark load-bearing, and it is not reopened
// here.
int32_t accept_autosuggestion_char(lesh_editor* editor, const lesh_invocation*, void*) {
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

	status = lesh_buffer_set(editor, candidate, length);
	if (status != LESH_OK)
		return status;
	size_t boundary = 0;
	status = lesh_position_move(editor, typed, LESH_MOTION_NEXT_CLUSTER, &boundary);
	if (status != LESH_OK)
		return status;
	// A motion that did not move means there is no cluster boundary further on -
	// take the rest, the same answer the word twin gives for the same reason.
	// Unreachable while `length > typed`, and cheaper than a wrong truncation if
	// it ever is not.
	if (boundary <= typed)
		boundary = length;
	if (boundary < length) {
		status = lesh_buffer_replace(editor, boundary, length, nullptr, 0);
		if (status != LESH_OK)
			return status;
	}
	return lesh_cursor_set(editor, boundary);
}

// DELIBERATELY BOUND TO NO KEY, in any keymap, and it is a decision rather than
// an omission (#140 decision 4, fish's answer). A suggestion changes as you
// type, so it dismisses itself; Ctrl-C clears the whole line when it does not.
// A default key here would be a key spent on the case the user can already
// reach - and it is one `bind '<C-g>' dismiss_autosuggestion` away for anyone
// who wants it, which is what registering a name with no key is FOR.
int32_t dismiss_autosuggestion(lesh_editor* editor, const lesh_invocation*, void*) {
	// Requested, never performed: the loop drops the batch once this action's
	// writes have been committed. Answers LESH_OK with nothing showing, because
	// dismissing nothing is not an error either.
	return lesh_proposal_dismiss(editor, LESH_PROPOSAL_AUTOSUGGESTION);
}

// --- Accepting by FALLTHROUGH (#140 decision 2, #147) -----------------------
//
// THE ACCEPTING ACTION IS THE BOUND ACTION AND THE MOTIONS STAY PURE. The
// alternative fish takes - teaching `forward_char` to accept when it is at the
// end of the buffer - was rejected on one argument: vi operators drive motions,
// so `d$` and `dw` with a suggestion showing would accept while deleting. Here
// the `w` in `dw` dispatches inside the opaque operator-pending keymap, which
// binds the pure motion table (#119), and safety is a fact about the tables
// rather than about a condition somebody has to remember to check.
//
// ONE IMPLEMENTATION, six registrations. What differs between `<Right>` and
// `e` is two action NAMES, and a name is data - so it rides in as registration
// userdata (the ABI's third argument, the same door the vi repertoire's context
// comes through) and the wrapper is a lookup and a delegation.
//
// COMPOSED, NEVER REIMPLEMENTED. Both halves go through `lesh_action_invoke`
// (#110), which shares the caller's staging area and undo group - so accepting
// through `<Right>` is the one undo entry accepting through
// `accept_autosuggestion` is, and the fallthrough is the same single motion the
// key meant before. The PLAIN names and not `.name`: a user who replaces
// `forward_char` or `accept_autosuggestion` has replaced what these keys do,
// which is the whole point of a name being rebindable.

struct fallthrough {
	const char* accept;   // what the key means with a suggestion showing
	const char* motion;   // what it means otherwise - and what it always meant
};

// True when the accept half should run.
//
// THREE QUESTIONS, and each is load-bearing. Something is proposed; the cursor
// is at the END of the buffer, because that is where a suggestion is a
// continuation of what you typed and anywhere else `<Right>` means what
// `<Right>` means; and the proposal is LONGER than the buffer, so a key never
// disappears into an accept that had nothing left to accept.
bool suggestion_is_acceptable(lesh_editor* editor) {
	// A LENGTH-ONLY ASK: capacity zero, so a non-empty proposal answers
	// LESH_ERR_TOOSMALL having filled the length in. LESH_ERR_NOTFOUND is the
	// one answer that means nothing is showing, and it is not an error - see
	// `read_suggestion` above. Nothing is copied, which is what keeps a key
	// that only moved the cursor from touching 4 KiB of stack.
	size_t suggested = 0;
	const int32_t showing =
		lesh_proposal_read(editor, LESH_PROPOSAL_AUTOSUGGESTION, 0, nullptr, 0, &suggested);
	if (showing != LESH_OK && showing != LESH_ERR_TOOSMALL)
		return false;

	size_t typed = 0;
	if (lesh_buffer_length(editor, &typed) != LESH_OK)
		return false;
	if (suggested <= typed)
		return false;

	size_t cursor = 0;
	if (lesh_cursor_get(editor, &cursor) != LESH_OK)
		return false;
	return cursor == typed;
}

int32_t accept_suggestion_or(lesh_editor* editor, const lesh_invocation* invocation,
                             void* userdata) {
	const fallthrough* which = static_cast<const fallthrough*>(userdata);
	if (which == nullptr)
		return LESH_ERR_INVAL;
	// The invocation travels UNCHANGED, so a count typed before the key reaches
	// `vi_word_next` the way it would have without the wrapper. A null one - an
	// action invoked from somewhere that was not a key - becomes an empty
	// invocation rather than LESH_ERR_INVAL, on the stack, because a keystroke
	// path takes nothing from the heap.
	lesh_invocation none{};
	const lesh_invocation* how = invocation != nullptr ? invocation : &none;
	return lesh_action_invoke(
		editor, suggestion_is_acceptable(editor) ? which->accept : which->motion, how);
}

// The five rows of #140's table plus #149's unbound sixth, as registration
// userdata. File scope and
// non-const because the ABI's context argument is a `void*`; constant-
// initialized and never written, so nothing here is a mutable global in any
// sense that matters.
fallthrough to_forward_char{"accept_autosuggestion", "forward_char"};
fallthrough to_end_of_line{"accept_autosuggestion", "end_of_line"};
// #149's row, and the one row of this table BOUND TO NOTHING. Same shape as the
// others - accept when a suggestion is acceptable, otherwise the motion the key
// always meant - and the same reason it has no key as the action it composes:
// fish ships `forward-single-char` unbound, because `<Right>` is spent on the
// whole accept. It is here so that `bind '<A-Right>'
// accept_suggestion_char_or_forward_char` is the whole of what a user has to do.
fallthrough to_forward_char_by_one{"accept_autosuggestion_char", "forward_char"};
fallthrough to_forward_word{"accept_autosuggestion_word", "forward_word"};
// vi command mode gets the WORD accepts only, and falls through to its own
// class-aware motions. `b` is not here: a suggestion exists only forward of the
// cursor, so backward has nothing to accept.
fallthrough to_vi_word_next{"accept_autosuggestion_word", "vi_word_next"};
fallthrough to_vi_word_end{"accept_autosuggestion_word", "vi_word_end"};

// --- Completion (#139, F-28/F-30, spec 6.9) ---------------------------------
//
// THE WHOLE TAB BEHAVIOUR, in one action and through the ABI like everything
// else in this file. What is NOT here is any knowledge of what a completion IS:
// where the token starts, which of the trio it belongs to, how a name is quoted
// and whether it is a directory are the `Completer` provider's (#94's override
// point), reached through `lesh_complete`. What is also not here is F-30 - the
// choice between inserting and listing belongs to `lesh_pager_commit`, so that
// this action, the history search and a plugin cannot disagree about it.
//
// So the action is three moves: ask, feed, commit. That it stages no write of
// its own is the point - the pager's insertion is the staged write, and A-12
// holds for a completion exactly as it holds for an accepted suggestion.

// Feeds one completion into the pager and commits it. `*outcome` is what commit
// decided; `*inserted_length` is how many bytes the span grew by, which is how
// the caller below learns whether a directory was just entered.
int32_t offer_completion(lesh_editor* editor, uint32_t* outcome) {
	size_t count = 0;
	int32_t status = lesh_complete(editor, &count);
	// No completer wired up. Not an error to report: Tab in a leshper with no
	// completer is the same ordinary nothing that Tab on an unmatched prefix is.
	if (status == LESH_ERR_NOTFOUND)
		return LESH_OK;
	if (status != LESH_OK)
		return status;

	size_t from = 0;
	size_t to = 0;
	status = lesh_completion_range(editor, &from, &to);
	if (status != LESH_OK)
		return status;
	status = lesh_pager_open(editor, from, to);
	if (status != LESH_OK)
		return status;

	char text[kCandidateBytes];
	for (size_t index = 0; index < count; ++index) {
		size_t length = 0;
		uint32_t kind = LESH_PAGER_WORD;
		// A candidate too long for the scratch is SKIPPED and not truncated: a
		// truncated candidate would insert bytes that name a different file, which
		// is the one failure worse than not offering it. Nothing anyone completes
		// is 4 KiB long; a paste is, and a paste is not a candidate.
		if (lesh_completion_candidate(editor, index, text, sizeof(text), &length, &kind)
		    != LESH_OK)
			continue;
		if (length > sizeof(text))
			continue;
		(void)lesh_pager_add(editor, text, length, kind);
	}
	// COMMIT EVEN WITH NOTHING ADDED. An empty commit closes whatever the pager
	// held and answers NOTHING, which is what Tab on an unmatched prefix should
	// do - and leaving a stale list open instead would be a pager showing
	// candidates for a line that has moved on.
	return lesh_pager_commit(editor, outcome);
}

// True when the byte just before the cursor is a `/`.
//
// How "directories stay open" is detected, and it is deliberately a question
// about the BUFFER rather than about what was inserted: the `/` is appended by
// the pager from the candidate's kind (pager.h: "this is the byte that makes
// re-running find a directory rather than a prefix"), so reading it back is
// reading the pager's own answer rather than guessing at it.
int32_t just_entered_a_directory(lesh_editor* editor, int32_t* answer) {
	*answer = 0;
	size_t cursor = 0;
	const int32_t status = lesh_cursor_get(editor, &cursor);
	if (status != LESH_OK || cursor == 0)
		return status;
	char last = 0;
	size_t length = 0;
	if (lesh_buffer_read(editor, cursor - 1, cursor, &last, 1, &length) != LESH_OK)
		return LESH_OK;
	*answer = length == 1 && last == '/' ? 1 : 0;
	return LESH_OK;
}

int32_t complete_word(lesh_editor* editor, const lesh_invocation*, void*) {
	uint32_t outcome = LESH_PAGER_NOTHING;
	int32_t status = offer_completion(editor, &outcome);
	if (status != LESH_OK)
		return status;

	// SPEC 6.9: "directories complete with `/` and stay open". Staying open is
	// the completer's half - the pager appended the `/` and closed - so the
	// completion runs once more, and what it finds this time is the directory's
	// contents rather than a prefix.
	//
	// ONCE, NOT A LOOP. A second pass that also entered a directory would descend
	// again, and a chain of single-entry directories would walk the tree on one
	// keypress. One re-run is what makes `cd sr<Tab>` show what is in `src/`;
	// going further is the user pressing Tab again, which is a decision they get
	// to take.
	if (outcome != LESH_PAGER_INSERTED)
		return LESH_OK;
	int32_t entered = 0;
	status = just_entered_a_directory(editor, &entered);
	if (status != LESH_OK || entered == 0)
		return status;
	return offer_completion(editor, &outcome);
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
	// The registration-time context. Null for all but the fallthrough wrappers,
	// which are one function registered five times and told apart by it.
	void* userdata = nullptr;
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
	{"forward_word", forward_word},
	{"undo", undo_action},
	{"redo", redo_action},
	// F-25's three, plus #149's. THE ACCEPTS ARE STILL BOUND TO NO KEY
	// THEMSELVES, and that is unchanged by #140: what the default table binds is
	// the five wrappers below, which compose these. A user who wants a key that
	// accepts and does nothing else binds one of these names, and a user who
	// wants no dismissal key gets that for free - see the note above the action.
	{"accept_autosuggestion", accept_autosuggestion},
	{"accept_autosuggestion_word", accept_autosuggestion_word},
	// #149's per-character accept, fish's `forward-single-char`. Bound to no key
	// here and to none through a wrapper either - see the note above the action.
	{"accept_autosuggestion_char", accept_autosuggestion_char},
	{"dismiss_autosuggestion", dismiss_autosuggestion},
	// #140's table, five thin names over one implementation, and #149's sixth
	// beside them with no key. Which accept each composes is in the userdata
	// beside it, not in the name - the fallback is what tells them apart to a
	// reader, and `bind -m emacs` shows the name a key really runs, which was the
	// argument for wrappers over overloading.
	{"accept_suggestion_or_forward_char", accept_suggestion_or, &to_forward_char},
	{"accept_suggestion_or_end_of_line", accept_suggestion_or, &to_end_of_line},
	{"accept_suggestion_or_forward_word", accept_suggestion_or, &to_forward_word},
	{"accept_suggestion_or_word_start_next", accept_suggestion_or, &to_vi_word_next},
	{"accept_suggestion_or_word_end_next", accept_suggestion_or, &to_vi_word_end},
	// #149's sixth row. Registered like the other five, bound like none of them.
	{"accept_suggestion_char_or_forward_char", accept_suggestion_or, &to_forward_char_by_one},
	// #139's. Bound to Tab at the wiring site (read.cpp) rather than in
	// keymap.cpp's default tables, because it is only in a session that a
	// completer exists to complete WITH - the same reason `accept_line` is bound
	// there. zsh calls it `expand-or-complete`, readline `complete`, fish
	// `complete`; `complete_word` says which of the two things Tab could mean.
	{"complete_word", complete_word},
};

} // namespace

// Out of line rather than in registry.h, so that this file needs no leshper
// header but abi.h. Declared in registry.h; see the note at the top.
namespace lesh::leshper {

std::size_t register_builtin_actions(lesh_registry& reg) {
	std::size_t registered = 0;
	for (const builtin& one : builtins) {
		if (lesh_action_register(&reg, one.name, one.fn, one.userdata) == LESH_OK)
			++registered;
	}
	return registered;
}

} // namespace lesh::leshper
