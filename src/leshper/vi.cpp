// The vi repertoire, registered through the ABI and reachable by no other route
// (#99, #119, ADR-0008, architecture spec §6.5).
//
// LOOK AT THE INCLUDES, the same way builtin_actions.cpp asks you to. This file
// sees `leshper/abi.h` and its own header - which itself includes nothing from
// leshper - and so it cannot reach `state`, cannot call `keymap_stack::push`,
// and cannot touch the kill store except through the door abi.h opens. Every
// capability the vi mode has is one a Lua binding has, held there by the
// compiler.
//
// That is the whole argument for the mode-entry decision #118 delegated here:
// `i` could have been three C++ lines over `state`, and then entering a mode
// would have been a thing only the native mode could do. See the note above
// `lesh_mode_get` in abi.h.
//
// The boundary paragraph - what vi does NOT get in v1 and why - is in vi.h,
// where a reader looking for the module's shape will find it.

#include "leshper/vi.h"

#include "leshper/abi.h"
#include "substrate/numeric.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// The vi mode's own memory. See vi.h for why it is registration-time context
// and not editor state.
struct lesh::leshper::vi_context {
	// What `;` repeats. The kinds are spelled as small integers rather than an
	// enum class so that the type stays a plain aggregate this file can hand
	// around as `void*` without a cast anyone has to think about.
	static constexpr std::uint8_t find_none = 0;
	static constexpr std::uint8_t find_forward = 1;      // f
	static constexpr std::uint8_t find_backward = 2;     // F
	static constexpr std::uint8_t till_forward = 3;      // t
	static constexpr std::uint8_t till_backward = 4;     // T

	std::uint8_t last_find = find_none;
	std::uint8_t arming = find_none;   // which of the four is waiting for its target
	std::string target;                // the bytes `;` looks for

	// The object that just ran telling the operator its span was a whole line.
	// Shape lives in the mode (spec §6.3), and this is the mode holding it for
	// exactly the one dispatch between the object and its verb.
	bool span_is_line = false;

	// Scratch, grown once and reused. A kill is an EDIT and may allocate; what
	// must not allocate is the keystroke that did not edit, and none of the
	// motions below touch these.
	std::string scratch;
	std::string line;
};

namespace {

using lesh::leshper::vi_context;

constexpr std::size_t kNameBytes = 64;

vi_context& context_of(void* userdata) { return *static_cast<vi_context*>(userdata); }

// The count an action acts on. Absent means one, and a count is bounded: a
// prompt is not a text editor, and `999999999dd` should not be a hang.
std::int64_t count_of(const lesh_invocation* how) {
	if (how == nullptr || how->has_numeric_argument == 0)
		return 1;
	const std::int64_t given = how->numeric_argument;
	if (given < 1)
		return 1;
	constexpr std::int64_t ceiling = 100000;
	return given > ceiling ? ceiling : given;
}

// The last byte of the key sequence that reached this action - which is the
// delimiter of `i(`, the digit of `3`, the target of `f`. Zero when the
// invocation carried no keys.
char last_key_byte(const lesh_invocation* how) {
	if (how == nullptr || how->keys == nullptr || how->keys_length == 0)
		return 0;
	return how->keys[how->keys_length - 1];
}

bool operator_is_pending(lesh_editor* editor) {
	char name[kNameBytes];
	std::size_t length = 0;
	if (lesh_pending_operator_get(editor, name, sizeof(name), &length) != LESH_OK)
		return false;
	return length != 0;
}

// An operator that cannot be completed gives up cleanly: the slot clears AND
// the layer it pushed goes, because dispatch only pops the layer when it
// consumes the slot. Half of that would leave operator-pending mode wedged.
void abort_operator(lesh_editor* editor) {
	if (!operator_is_pending(editor))
		return;
	lesh_pending_operator_set(editor, nullptr);
	lesh_keymap_pop(editor);
}

std::size_t cursor_now(lesh_editor* editor) {
	std::size_t at = 0;
	lesh_cursor_get(editor, &at);
	return at;
}

std::size_t moved(lesh_editor* editor, std::size_t from, lesh_motion motion) {
	std::size_t to = from;
	lesh_position_move(editor, from, motion, &to);
	return to;
}

// Reads `[from, to)` into the context's scratch.
void read_span(lesh_editor* editor, vi_context& self, std::size_t from, std::size_t to) {
	std::size_t length = 0;
	lesh_buffer_read(editor, from, to, nullptr, 0, &length);
	self.scratch.resize(length);
	if (length != 0)
		lesh_buffer_read(editor, from, to, self.scratch.data(), length, &length);
}

bool byte_at(lesh_editor* editor, std::size_t at, char& out) {
	char one = 0;
	std::size_t length = 0;
	if (lesh_buffer_read(editor, at, at + 1, &one, 1, &length) != LESH_OK || length != 1)
		return false;
	out = one;
	return true;
}

constexpr bool is_blank_byte(char byte) { return byte == ' ' || byte == '\t'; }

// The one place text leaves the buffer for the store (#99 answer 3). Every
// delete, change and yank below comes through here, which is what makes "every
// kill feeds the store" a property of the code rather than of forty call sites.
void kill_span(lesh_editor* editor, vi_context& self, std::size_t from, std::size_t to,
               std::uint32_t flags) {
	if (to < from)
		return;
	read_span(editor, self, from, to);
	// A linewise register always ends with a newline, whether or not the last
	// line of the buffer had one. `p` then has one rule instead of two, and the
	// rule it has is the one vim's is.
	if ((flags & LESH_KILL_LINEWISE) != 0
	    && (self.scratch.empty() || self.scratch.back() != '\n'))
		self.scratch.push_back('\n');
	lesh_kill_set(editor, nullptr, self.scratch.data(), self.scratch.size(), flags);
}

// --- Motions ----------------------------------------------------------------

// vi's inclusiveness, applied where vi applies it: `e`, `E`, `f` and `t` include
// the character they land on when an operator is waiting, and do not when the
// user is only moving. Spec §6.3 puts this projection in the MODE, and this is
// the mode - three lines, in the only place that knows an operator is pending.
void extend_inclusive(lesh_editor* editor) {
	if (!operator_is_pending(editor))
		return;
	const std::size_t at = cursor_now(editor);
	lesh_cursor_set(editor, moved(editor, at, LESH_MOTION_NEXT_CLUSTER));
}

std::int32_t move_repeated(lesh_editor* editor, lesh_motion motion, std::int64_t count) {
	std::size_t at = cursor_now(editor);
	for (std::int64_t i = 0; i < count; ++i) {
		const std::size_t next = moved(editor, at, motion);
		if (next == at)
			break;   // the motion ran out of buffer; vi stops rather than failing
		at = next;
	}
	return lesh_cursor_set(editor, at);
}

std::int32_t simple_motion(lesh_editor* editor, lesh_motion motion) {
	return lesh_cursor_set(editor, moved(editor, cursor_now(editor), motion));
}

// --- The find family (`f F t T ; ,`) ----------------------------------------

// Performs a find that already knows its target. Searches WITHIN THE LINE, as
// vi does: a `f;` must not walk off into the next command of a multi-line edit.
std::int32_t perform_find(lesh_editor* editor, vi_context& self, std::uint8_t kind,
                          std::int64_t count) {
	if (kind == vi_context::find_none || self.target.empty())
		return LESH_OK;
	const std::size_t at = cursor_now(editor);
	const std::size_t line_begin = moved(editor, at, LESH_MOTION_LINE_START);
	const std::size_t line_finish = moved(editor, at, LESH_MOTION_LINE_END);
	read_span(editor, self, line_begin, line_finish);
	self.line.assign(self.scratch);

	const bool forward =
		kind == vi_context::find_forward || kind == vi_context::till_forward;
	const bool till = kind == vi_context::till_forward || kind == vi_context::till_backward;
	std::size_t relative = at - line_begin;

	for (std::int64_t i = 0; i < count; ++i) {
		if (forward) {
			// `t` repeated has to step off the character it is sitting just before,
			// or `;` after `tx` would never move.
			std::size_t from = relative + 1;
			if (till && i == 0 && from < self.line.size()
			    && self.line.compare(from, self.target.size(), self.target) == 0)
				++from;
			const std::size_t found = from > self.line.size()
				? std::string::npos
				: self.line.find(self.target, from);
			if (found == std::string::npos)
				return LESH_OK;   // not there: vi does nothing, and that is not an error
			relative = found;
		} else {
			if (relative == 0)
				return LESH_OK;
			const std::size_t found = self.line.rfind(self.target, relative - 1);
			if (found == std::string::npos)
				return LESH_OK;
			relative = found;
		}
	}

	std::size_t landing = line_begin + relative;
	if (till) {
		landing = forward ? moved(editor, landing, LESH_MOTION_PREV_CLUSTER)
		                  : moved(editor, landing, LESH_MOTION_NEXT_CLUSTER);
	}
	lesh_cursor_set(editor, landing);
	if (forward)
		extend_inclusive(editor);
	return LESH_OK;
}

std::int32_t arm_find(lesh_editor* editor, void* userdata, std::uint8_t kind) {
	vi_context& self = context_of(userdata);
	self.arming = kind;
	// The target character has not been typed yet, so a one-shot opaque keymap
	// catches it: its default action is the other half of this pair. That is the
	// #117 machinery doing what it was built for - a keymap that swallows
	// everything and routes it to one action - rather than a second input mode
	// invented for `f`.
	return lesh_keymap_push(editor, "vi_find_char");
}

std::int32_t vi_find_forward(lesh_editor* e, const lesh_invocation*, void* u) {
	return arm_find(e, u, vi_context::find_forward);
}
std::int32_t vi_find_backward(lesh_editor* e, const lesh_invocation*, void* u) {
	return arm_find(e, u, vi_context::find_backward);
}
std::int32_t vi_till_forward(lesh_editor* e, const lesh_invocation*, void* u) {
	return arm_find(e, u, vi_context::till_forward);
}
std::int32_t vi_till_backward(lesh_editor* e, const lesh_invocation*, void* u) {
	return arm_find(e, u, vi_context::till_backward);
}

std::int32_t vi_find_char_target(lesh_editor* editor, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	lesh_keymap_pop(editor);
	const std::uint8_t kind = self.arming;
	self.arming = vi_context::find_none;
	if (how == nullptr || how->keys == nullptr || how->keys_length == 0
	    || kind == vi_context::find_none) {
		// Escape, or a key that types nothing: the find is abandoned, and so is
		// any operator waiting on it.
		abort_operator(editor);
		return LESH_OK;
	}
	self.target.assign(how->keys, how->keys_length);
	self.last_find = kind;
	return perform_find(editor, self, kind, count_of(how));
}

std::uint8_t reversed(std::uint8_t kind) {
	switch (kind) {
	case vi_context::find_forward:
		return vi_context::find_backward;
	case vi_context::find_backward:
		return vi_context::find_forward;
	case vi_context::till_forward:
		return vi_context::till_backward;
	case vi_context::till_backward:
		return vi_context::till_forward;
	default:
		return vi_context::find_none;
	}
}

std::int32_t vi_find_repeat(lesh_editor* e, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	return perform_find(e, self, self.last_find, count_of(how));
}

std::int32_t vi_find_repeat_reverse(lesh_editor* e, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	return perform_find(e, self, reversed(self.last_find), count_of(how));
}

// --- The motions proper -----------------------------------------------------

std::int32_t vi_backward_char(lesh_editor* e, const lesh_invocation* how, void*) {
	return move_repeated(e, LESH_MOTION_PREV_CLUSTER, count_of(how));
}
std::int32_t vi_forward_char(lesh_editor* e, const lesh_invocation* how, void*) {
	return move_repeated(e, LESH_MOTION_NEXT_CLUSTER, count_of(how));
}
std::int32_t vi_line_up(lesh_editor* e, const lesh_invocation* how, void*) {
	return move_repeated(e, LESH_MOTION_LINE_UP, count_of(how));
}
std::int32_t vi_line_down(lesh_editor* e, const lesh_invocation* how, void*) {
	return move_repeated(e, LESH_MOTION_LINE_DOWN, count_of(how));
}
std::int32_t vi_first_nonblank(lesh_editor* e, const lesh_invocation*, void*) {
	return simple_motion(e, LESH_MOTION_LINE_FIRST_NONBLANK);
}
std::int32_t vi_word_next(lesh_editor* e, const lesh_invocation* how, void*) {
	return move_repeated(e, LESH_MOTION_WORD_START_NEXT, count_of(how));
}
std::int32_t vi_word_prev(lesh_editor* e, const lesh_invocation* how, void*) {
	return move_repeated(e, LESH_MOTION_WORD_START_PREV, count_of(how));
}
std::int32_t vi_word_end(lesh_editor* e, const lesh_invocation* how, void*) {
	const std::int32_t status = move_repeated(e, LESH_MOTION_WORD_END_NEXT, count_of(how));
	extend_inclusive(e);
	return status;
}
std::int32_t vi_blank_word_next(lesh_editor* e, const lesh_invocation* how, void*) {
	return move_repeated(e, LESH_MOTION_BLANK_WORD_START_NEXT, count_of(how));
}
std::int32_t vi_blank_word_prev(lesh_editor* e, const lesh_invocation* how, void*) {
	return move_repeated(e, LESH_MOTION_BLANK_WORD_START_PREV, count_of(how));
}
std::int32_t vi_blank_word_end(lesh_editor* e, const lesh_invocation* how, void*) {
	const std::int32_t status =
		move_repeated(e, LESH_MOTION_BLANK_WORD_END_NEXT, count_of(how));
	extend_inclusive(e);
	return status;
}

// --- Counts (#99: `d2w`, `3dd`) ---------------------------------------------

std::int32_t vi_digit_argument(lesh_editor* editor, const lesh_invocation* how, void*) {
	const int digit = lesh::digit_value(last_key_byte(how), 10);
	if (digit < 0)
		return LESH_ERR_INVAL;
	// The argument this key arrived with is the count SO FAR: dispatch cleared it
	// before the call, so appending one digit is the whole of accumulation and
	// there is nowhere for a half-typed count to be forgotten.
	//
	// Through the substrate's one accumulator, like every other digit read in the
	// tree (#59, #62): a count typed by a hand resting on `9` is exactly the
	// overflow that guard exists for, and a second hand-rolled `*10 +` is exactly
	// what it exists to prevent.
	std::uint64_t value =
		how->has_numeric_argument != 0 && how->numeric_argument > 0
			? static_cast<std::uint64_t>(how->numeric_argument)
			: 0;
	const lesh::numeric_policy& policy =
		lesh::policy_for(lesh::numeric_site::editor_repeat_count);
	// The overflow answer is DELIBERATELY not consulted: `accumulate_digit`
	// clamps to the limit when it says false, and a count clamped at the top is
	// what a hand resting on `9` asked for as closely as anything can be.
	(void)lesh::accumulate_digit(value, static_cast<std::uint64_t>(digit), 10,
	                             lesh::numeric_limit(policy, false));
	// The ceiling is the EDITOR's knowledge and not the policy's: a prompt is not
	// a text editor, and `999999999dd` should be a large number rather than a
	// hang.
	constexpr std::uint64_t ceiling = 100000;
	if (value > ceiling)
		value = ceiling;
	return lesh_numeric_argument_set(editor, static_cast<std::int64_t>(value));
}

// `0` is a digit only when a count is already being typed; otherwise it is the
// motion to column zero. vi's one genuinely ambiguous key, and the ambiguity is
// resolved by exactly the state that makes it ambiguous.
std::int32_t vi_digit_or_line_start(lesh_editor* editor, const lesh_invocation* how, void* u) {
	if (how != nullptr && how->has_numeric_argument != 0 && how->numeric_argument > 0)
		return vi_digit_argument(editor, how, u);
	return simple_motion(editor, LESH_MOTION_LINE_START);
}

// --- Operators --------------------------------------------------------------

std::int32_t start_operator(lesh_editor* editor, const lesh_invocation* how,
                            const char* verb) {
	// The count carried across to the motion, which is what makes `3dd` mean
	// three lines: the count arrived at `d` and the object that will use it is
	// the next dispatch. `d2w` needs nothing here - its count arrives after.
	if (how != nullptr && how->has_numeric_argument != 0)
		lesh_numeric_argument_set(editor, how->numeric_argument);
	const std::int32_t status = lesh_pending_operator_set(editor, verb);
	if (status != LESH_OK)
		return status;
	return lesh_keymap_push(editor, "vi_operator_pending");
}

std::int32_t vi_delete_operator(lesh_editor* e, const lesh_invocation* how, void*) {
	return start_operator(e, how, "vi_delete");
}
std::int32_t vi_change_operator(lesh_editor* e, const lesh_invocation* how, void*) {
	return start_operator(e, how, "vi_change");
}
std::int32_t vi_yank_operator(lesh_editor* e, const lesh_invocation* how, void*) {
	return start_operator(e, how, "vi_yank");
}

std::int32_t vi_operator_abort(lesh_editor* editor, const lesh_invocation*, void*) {
	abort_operator(editor);
	return LESH_OK;
}

// The three verbs. Each reads the SELECTION and nothing else, which is the whole
// of #99 answer 5: an operator and a visual-mode verb are the same action,
// because operator-pending and visual both hand it a region.
struct span {
	std::size_t from = 0;
	std::size_t to = 0;
	bool linewise = false;
};

span span_for_verb(lesh_editor* editor, vi_context& self) {
	span out;
	std::int32_t active = 0;
	lesh_selection_get(editor, &out.from, &out.to, &active);
	out.linewise = self.span_is_line;
	self.span_is_line = false;
	if (!active)
		out.to = out.from;
	return out;
}

std::int32_t vi_delete(lesh_editor* editor, const lesh_invocation*, void* u) {
	vi_context& self = context_of(u);
	const span what = span_for_verb(editor, self);
	kill_span(editor, self, what.from, what.to,
	          what.linewise ? LESH_KILL_LINEWISE : LESH_KILL_CHARWISE);
	if (what.to > what.from)
		lesh_buffer_replace(editor, what.from, what.to, nullptr, 0);
	lesh_cursor_set(editor, what.from);
	return lesh_selection_clear(editor);
}

std::int32_t vi_change(lesh_editor* editor, const lesh_invocation*, void* u) {
	vi_context& self = context_of(u);
	span what = span_for_verb(editor, self);
	// `cc` keeps the line and empties it; `dd` takes the line away. The only
	// difference is one newline, and this is it.
	if (what.linewise && what.to > what.from) {
		char last = 0;
		if (byte_at(editor, what.to - 1, last) && last == '\n')
			--what.to;
	}
	kill_span(editor, self, what.from, what.to,
	          what.linewise ? LESH_KILL_LINEWISE : LESH_KILL_CHARWISE);
	if (what.to > what.from)
		lesh_buffer_replace(editor, what.from, what.to, nullptr, 0);
	lesh_cursor_set(editor, what.from);
	lesh_selection_clear(editor);
	return lesh_mode_set(editor, "vi_insert");
}

std::int32_t vi_yank(lesh_editor* editor, const lesh_invocation*, void* u) {
	vi_context& self = context_of(u);
	const span what = span_for_verb(editor, self);
	kill_span(editor, self, what.from, what.to,
	          what.linewise ? LESH_KILL_LINEWISE : LESH_KILL_CHARWISE);
	lesh_cursor_set(editor, what.from);
	return lesh_selection_clear(editor);
}

// --- Text objects (#99 answer 2) --------------------------------------------
//
// An object is ONE ACTION THAT SETS THE SELECTION TO A RANGE. That sentence is
// the whole reason objects were affordable: #96 made operators read a region and
// #117 made operator-pending a push, so nothing here is operator machinery - it
// is a range, set the same way visual mode sets one, and it works in visual mode
// for free (`viw`).

std::int32_t set_object(lesh_editor* editor, std::size_t from, std::size_t to) {
	return lesh_selection_set(editor, from, to);
}

std::int32_t object_word(lesh_editor* editor, const lesh_invocation* how, lesh_span which,
                         bool around) {
	const std::size_t at = cursor_now(editor);
	std::size_t from = at;
	std::size_t to = at;
	if (lesh_span_at(editor, at, which, &from, &to) != LESH_OK)
		return LESH_OK;
	// A count extends the object word by word, which is what `d3aw` means.
	for (std::int64_t i = 1; i < count_of(how); ++i) {
		std::size_t more_from = to;
		std::size_t more_to = to;
		if (lesh_span_at(editor, to, which, &more_from, &more_to) != LESH_OK
		    || more_to == to)
			break;
		to = more_to;
	}
	if (around) {
		// vim's rule, and it is not symmetric: `aw` takes the trailing whitespace
		// if there is any, and the leading whitespace only when there is not.
		std::size_t blank_from = to;
		std::size_t blank_to = to;
		char byte = 0;
		if (byte_at(editor, to, byte) && is_blank_byte(byte)
		    && lesh_span_at(editor, to, which, &blank_from, &blank_to) == LESH_OK
		    && blank_to > to) {
			to = blank_to;
		} else if (from > 0 && byte_at(editor, from - 1, byte) && is_blank_byte(byte)
		           && lesh_span_at(editor, from - 1, which, &blank_from, &blank_to) == LESH_OK) {
			from = blank_from;
		}
	}
	return set_object(editor, from, to);
}

std::int32_t vi_object_inner_word(lesh_editor* e, const lesh_invocation* how, void*) {
	return object_word(e, how, LESH_SPAN_WORD, false);
}
std::int32_t vi_object_a_word(lesh_editor* e, const lesh_invocation* how, void*) {
	return object_word(e, how, LESH_SPAN_WORD, true);
}
std::int32_t vi_object_inner_blank_word(lesh_editor* e, const lesh_invocation* how, void*) {
	return object_word(e, how, LESH_SPAN_BLANK_WORD, false);
}
std::int32_t vi_object_a_blank_word(lesh_editor* e, const lesh_invocation* how, void*) {
	return object_word(e, how, LESH_SPAN_BLANK_WORD, true);
}

// The delimiter the key named. `b` and `B` are vim's aliases for the two most
// typed pairs and cost one line each.
bool pair_for(char key, char& open, char& close) {
	switch (key) {
	case '(': case ')': case 'b': open = '('; close = ')'; return true;
	case '[': case ']':           open = '['; close = ']'; return true;
	case '{': case '}': case 'B': open = '{'; close = '}'; return true;
	case '<': case '>':           open = '<'; close = '>'; return true;
	case '"':                     open = '"'; close = '"'; return true;
	case '\'':                    open = '\''; close = '\''; return true;
	case '`':                     open = '`'; close = '`'; return true;
	default:                      return false;
	}
}

std::int32_t object_pair(lesh_editor* editor, const lesh_invocation* how, bool around) {
	char open = 0;
	char close = 0;
	if (!pair_for(last_key_byte(how), open, close)) {
		abort_operator(editor);
		return LESH_OK;
	}
	std::size_t from = 0;
	std::size_t to = 0;
	if (lesh_match_pair(editor, cursor_now(editor), static_cast<std::uint32_t>(open),
	                    static_cast<std::uint32_t>(close), &from, &to) != LESH_OK) {
		// Not inside a pair: vi does nothing at all, and the operator waiting on
		// this object gives up with it rather than running on an empty region.
		abort_operator(editor);
		return LESH_OK;
	}
	if (around)
		return set_object(editor, from, to);
	// The delimiters are ASCII by construction (lesh_match_pair refuses anything
	// else), so stepping past them is a byte and not a cluster question.
	return set_object(editor, from + 1, to - 1);
}

std::int32_t vi_object_inner_pair(lesh_editor* e, const lesh_invocation* how, void*) {
	return object_pair(e, how, false);
}
std::int32_t vi_object_a_pair(lesh_editor* e, const lesh_invocation* how, void*) {
	return object_pair(e, how, true);
}

// The doubled line forms, `dd` `cc` `yy`.
//
// The doubling has to MATCH: `dc` is not `dd`, and vi refuses it. The check is
// one comparison against the slot, which is the only place that knows which verb
// is waiting.
std::int32_t line_span(lesh_editor* editor, std::int64_t count, std::size_t& from,
                       std::size_t& to) {
	const std::size_t at = cursor_now(editor);
	from = moved(editor, at, LESH_MOTION_LINE_START);
	to = moved(editor, at, LESH_MOTION_LINE_END);
	std::size_t length = 0;
	lesh_buffer_length(editor, &length);
	for (std::int64_t i = 1; i < count && to < length; ++i)
		to = moved(editor, to + 1, LESH_MOTION_LINE_END);
	if (to < length)
		++to;   // the newline belongs to the line it ends
	return LESH_OK;
}

std::int32_t vi_line_object(lesh_editor* editor, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	char waiting[kNameBytes];
	std::size_t length = 0;
	if (lesh_pending_operator_get(editor, waiting, sizeof(waiting), &length) != LESH_OK
	    || length == 0) {
		return LESH_OK;
	}
	const char key = last_key_byte(how);
	const char* expected = key == 'd' ? "vi_delete"
	                     : key == 'c' ? "vi_change"
	                     : key == 'y' ? "vi_yank"
	                                  : nullptr;
	if (expected == nullptr || std::strlen(expected) != length
	    || std::memcmp(waiting, expected, length) != 0) {
		abort_operator(editor);
		return LESH_OK;
	}
	std::size_t from = 0;
	std::size_t to = 0;
	line_span(editor, count_of(how), from, to);
	self.span_is_line = true;
	return set_object(editor, from, to);
}

// --- The single-key edits: x s r ~ D C Y p P --------------------------------

std::int32_t vi_delete_char(lesh_editor* editor, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	const std::size_t at = cursor_now(editor);
	const std::size_t line_finish = moved(editor, at, LESH_MOTION_LINE_END);
	std::size_t to = at;
	for (std::int64_t i = 0; i < count_of(how); ++i) {
		const std::size_t next = moved(editor, to, LESH_MOTION_NEXT_CLUSTER);
		if (next == to || next > line_finish)
			break;   // `x` does not eat the newline, because vi's does not
		to = next;
	}
	if (to == at)
		return LESH_OK;
	kill_span(editor, self, at, to, LESH_KILL_CHARWISE);
	return lesh_buffer_replace(editor, at, to, nullptr, 0);
}

std::int32_t vi_substitute_char(lesh_editor* editor, const lesh_invocation* how, void* u) {
	const std::int32_t status = vi_delete_char(editor, how, u);
	if (status != LESH_OK)
		return status;
	return lesh_mode_set(editor, "vi_insert");
}

std::int32_t vi_replace_char(lesh_editor* editor, const lesh_invocation* how, void*) {
	// The character to replace WITH has not been typed yet; the same one-shot
	// opaque keymap trick `f` uses catches it.
	if (how != nullptr && how->has_numeric_argument != 0)
		lesh_numeric_argument_set(editor, how->numeric_argument);
	return lesh_keymap_push(editor, "vi_replace_char");
}

std::int32_t vi_replace_char_with(lesh_editor* editor, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	lesh_keymap_pop(editor);
	if (how == nullptr || how->keys == nullptr || how->keys_length == 0)
		return LESH_OK;   // Escape, or a key that types nothing: nothing replaced
	const std::size_t at = cursor_now(editor);
	const std::size_t line_finish = moved(editor, at, LESH_MOTION_LINE_END);
	const std::int64_t count = count_of(how);
	std::size_t to = at;
	for (std::int64_t i = 0; i < count; ++i) {
		const std::size_t next = moved(editor, to, LESH_MOTION_NEXT_CLUSTER);
		if (next == to || next > line_finish)
			return LESH_OK;   // vi refuses `3r` on two remaining characters
		to = next;
	}
	self.scratch.clear();
	for (std::int64_t i = 0; i < count; ++i)
		self.scratch.append(how->keys, how->keys_length);
	const std::int32_t status =
		lesh_buffer_replace(editor, at, to, self.scratch.data(), self.scratch.size());
	if (status != LESH_OK)
		return status;
	// vi leaves the cursor ON the last replaced character, not past it.
	return lesh_cursor_set(editor, moved(editor, cursor_now(editor), LESH_MOTION_PREV_CLUSTER));
}

std::int32_t vi_toggle_case(lesh_editor* editor, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	const std::size_t at = cursor_now(editor);
	const std::size_t line_finish = moved(editor, at, LESH_MOTION_LINE_END);
	std::size_t to = at;
	for (std::int64_t i = 0; i < count_of(how); ++i) {
		const std::size_t next = moved(editor, to, LESH_MOTION_NEXT_CLUSTER);
		if (next == to || next > line_finish)
			break;
		to = next;
	}
	if (to == at)
		return LESH_OK;
	read_span(editor, self, at, to);
	// ASCII only, deliberately: case folding outside ASCII is a Unicode table
	// this shell does not carry, and inventing a partial one here would be worse
	// than leaving the character alone.
	for (char& byte : self.scratch) {
		if (byte >= 'a' && byte <= 'z')
			byte = static_cast<char>(byte - 'a' + 'A');
		else if (byte >= 'A' && byte <= 'Z')
			byte = static_cast<char>(byte - 'A' + 'a');
	}
	return lesh_buffer_replace(editor, at, to, self.scratch.data(), self.scratch.size());
}

std::int32_t vi_delete_to_line_end(lesh_editor* editor, const lesh_invocation*, void* u) {
	vi_context& self = context_of(u);
	const std::size_t at = cursor_now(editor);
	const std::size_t line_finish = moved(editor, at, LESH_MOTION_LINE_END);
	if (line_finish <= at)
		return LESH_OK;
	kill_span(editor, self, at, line_finish, LESH_KILL_CHARWISE);
	return lesh_buffer_replace(editor, at, line_finish, nullptr, 0);
}

std::int32_t vi_change_to_line_end(lesh_editor* editor, const lesh_invocation* how, void* u) {
	const std::int32_t status = vi_delete_to_line_end(editor, how, u);
	if (status != LESH_OK)
		return status;
	return lesh_mode_set(editor, "vi_insert");
}

// `Y` is `yy`, not `y$` - vi's own inconsistency, kept because muscle memory is
// what a repertoire is for.
std::int32_t vi_yank_line(lesh_editor* editor, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	std::size_t from = 0;
	std::size_t to = 0;
	line_span(editor, count_of(how), from, to);
	kill_span(editor, self, from, to, LESH_KILL_LINEWISE);
	return LESH_OK;
}

std::int32_t put(lesh_editor* editor, const lesh_invocation* how, void* u, bool after) {
	vi_context& self = context_of(u);
	std::size_t length = 0;
	std::uint32_t flags = LESH_KILL_CHARWISE;
	// A zero-capacity read is the ABI's "how long is it" question, and its
	// answer for a non-empty entry is LESH_ERR_TOOSMALL WITH the length filled
	// in - that is the shape every copy-out accessor has. Only NOTFOUND means
	// nothing has been killed, and `p` with nothing killed does nothing.
	if (lesh_kill_get(editor, nullptr, nullptr, 0, &length, &flags) == LESH_ERR_NOTFOUND)
		return LESH_OK;
	self.line.resize(length);
	if (length != 0)
		lesh_kill_get(editor, nullptr, self.line.data(), length, &length, &flags);

	self.scratch.clear();
	for (std::int64_t i = 0; i < count_of(how); ++i)
		self.scratch.append(self.line);
	if (self.scratch.empty())
		return LESH_OK;

	const std::size_t at = cursor_now(editor);
	if ((flags & LESH_KILL_LINEWISE) != 0) {
		std::size_t buffer_length = 0;
		lesh_buffer_length(editor, &buffer_length);
		if (!after) {
			const std::size_t line_begin = moved(editor, at, LESH_MOTION_LINE_START);
			lesh_buffer_replace(editor, line_begin, line_begin, self.scratch.data(),
			                    self.scratch.size());
			return lesh_cursor_set(editor, line_begin);
		}
		const std::size_t line_finish = moved(editor, at, LESH_MOTION_LINE_END);
		if (line_finish < buffer_length) {
			const std::size_t target = line_finish + 1;
			lesh_buffer_replace(editor, target, target, self.scratch.data(),
			                    self.scratch.size());
			return lesh_cursor_set(editor, target);
		}
		// The last line has no newline of its own, so one is supplied and the
		// register's trailing newline is dropped rather than doubled.
		if (self.scratch.back() == '\n')
			self.scratch.pop_back();
		self.scratch.insert(self.scratch.begin(), '\n');
		lesh_buffer_replace(editor, line_finish, line_finish, self.scratch.data(),
		                    self.scratch.size());
		return lesh_cursor_set(editor, line_finish + 1);
	}

	const std::size_t line_finish = moved(editor, at, LESH_MOTION_LINE_END);
	std::size_t target = at;
	if (after && at < line_finish)
		target = moved(editor, at, LESH_MOTION_NEXT_CLUSTER);
	lesh_buffer_replace(editor, target, target, self.scratch.data(), self.scratch.size());
	// vi leaves the cursor on the LAST character put, not past it.
	return lesh_cursor_set(editor,
	                       moved(editor, cursor_now(editor), LESH_MOTION_PREV_CLUSTER));
}

std::int32_t vi_put_after(lesh_editor* e, const lesh_invocation* how, void* u) {
	return put(e, how, u, true);
}
std::int32_t vi_put_before(lesh_editor* e, const lesh_invocation* how, void* u) {
	return put(e, how, u, false);
}

// The emacs side of the ONE store (#99 answer 3). Not a vi action at all, and it
// lives here because this file is where the store's only other reader is - two
// readers of one table, in one place, is how they stay one table.
//
// NOT `vi_put_before` under another name: emacs's `C-y` inserts at point and
// leaves point AFTER the text, where vi's `P` leaves the cursor on its last
// character. The two paradigms disagree about where the cursor goes and agree
// about where the bytes come from, which is exactly the split #99 predicted.
std::int32_t yank(lesh_editor* editor, const lesh_invocation* how, void* u) {
	vi_context& self = context_of(u);
	std::size_t length = 0;
	std::uint32_t flags = LESH_KILL_CHARWISE;
	if (lesh_kill_get(editor, nullptr, nullptr, 0, &length, &flags) == LESH_ERR_NOTFOUND
	    || length == 0)
		return LESH_OK;
	self.line.resize(length);
	if (lesh_kill_get(editor, nullptr, self.line.data(), length, &length, &flags) != LESH_OK)
		return LESH_OK;
	self.scratch.clear();
	for (std::int64_t i = 0; i < count_of(how); ++i)
		self.scratch.append(self.line);
	const std::size_t at = cursor_now(editor);
	// lesh_buffer_replace leaves the cursor at the end of what it wrote, which is
	// where emacs's point goes; nothing more to say.
	return lesh_buffer_replace(editor, at, at, self.scratch.data(), self.scratch.size());
}

// --- Mode entries -----------------------------------------------------------
//
// #118's open question, answered: these are ABI capabilities, so a plugin can
// write them too. See the note above `lesh_mode_get` in abi.h.

std::int32_t vi_insert_mode(lesh_editor* e, const lesh_invocation*, void*) {
	return lesh_mode_set(e, "vi_insert");
}

std::int32_t vi_insert_at_line_start(lesh_editor* e, const lesh_invocation*, void*) {
	simple_motion(e, LESH_MOTION_LINE_FIRST_NONBLANK);
	return lesh_mode_set(e, "vi_insert");
}

std::int32_t vi_append(lesh_editor* editor, const lesh_invocation*, void*) {
	const std::size_t at = cursor_now(editor);
	const std::size_t line_finish = moved(editor, at, LESH_MOTION_LINE_END);
	if (at < line_finish)
		lesh_cursor_set(editor, moved(editor, at, LESH_MOTION_NEXT_CLUSTER));
	return lesh_mode_set(editor, "vi_insert");
}

std::int32_t vi_append_at_line_end(lesh_editor* e, const lesh_invocation*, void*) {
	simple_motion(e, LESH_MOTION_LINE_END);
	return lesh_mode_set(e, "vi_insert");
}

std::int32_t vi_open_below(lesh_editor* editor, const lesh_invocation*, void*) {
	const std::size_t line_finish = moved(editor, cursor_now(editor), LESH_MOTION_LINE_END);
	lesh_buffer_replace(editor, line_finish, line_finish, "\n", 1);
	return lesh_mode_set(editor, "vi_insert");
}

std::int32_t vi_open_above(lesh_editor* editor, const lesh_invocation*, void*) {
	const std::size_t line_begin = moved(editor, cursor_now(editor), LESH_MOTION_LINE_START);
	lesh_buffer_replace(editor, line_begin, line_begin, "\n", 1);
	lesh_cursor_set(editor, line_begin);
	return lesh_mode_set(editor, "vi_insert");
}

std::int32_t vi_command_mode(lesh_editor* editor, const lesh_invocation*, void*) {
	const std::int32_t status = lesh_mode_set(editor, "vi_command");
	// vi's Escape steps back onto the last character typed, because command mode
	// has no position past the end of a line.
	const std::size_t at = cursor_now(editor);
	const std::size_t line_begin = moved(editor, at, LESH_MOTION_LINE_START);
	if (at > line_begin)
		lesh_cursor_set(editor, moved(editor, at, LESH_MOTION_PREV_CLUSTER));
	return status;
}

// Escape in command mode: a half-typed command is thrown away. Nothing else -
// there is no mode to leave.
std::int32_t vi_normal_reset(lesh_editor* editor, const lesh_invocation*, void*) {
	lesh_numeric_argument_clear(editor);
	abort_operator(editor);
	return LESH_OK;
}

std::int32_t vi_visual_mode(lesh_editor* editor, const lesh_invocation*, void*) {
	const std::size_t at = cursor_now(editor);
	lesh_selection_set(editor, at, at);   // anchor here, head here, live
	return lesh_keymap_push(editor, "vi_visual");
}

std::int32_t vi_visual_exit(lesh_editor* editor, const lesh_invocation*, void*) {
	lesh_selection_clear(editor);
	return lesh_keymap_pop(editor);
}

// vi's `o`: the head becomes the tail. #96 put the swap in the state precisely
// so that this is one call and not a shape.
std::int32_t vi_visual_swap_ends(lesh_editor* editor, const lesh_invocation*, void*) {
	std::size_t from = 0;
	std::size_t to = 0;
	std::int32_t active = 0;
	if (lesh_selection_get(editor, &from, &to, &active) != LESH_OK || active == 0)
		return LESH_OK;
	const std::size_t head = cursor_now(editor);
	// The getter reports the SORTED pair; which end the cursor is on is what says
	// which way the selection points.
	return head == to ? lesh_selection_set(editor, to, from)
	                  : lesh_selection_set(editor, from, to);
}

// The visual-mode verbs. Each is the operator-pending verb with vi's
// inclusiveness applied and the visual layer popped - the projection §6.3 keeps
// in the mode, in the mode.
std::int32_t visual_finish(lesh_editor* editor) {
	std::size_t from = 0;
	std::size_t to = 0;
	std::int32_t active = 0;
	lesh_selection_get(editor, &from, &to, &active);
	if (active != 0)
		lesh_selection_set(editor, from, moved(editor, to, LESH_MOTION_NEXT_CLUSTER));
	return lesh_keymap_pop(editor);
}

std::int32_t vi_visual_delete(lesh_editor* e, const lesh_invocation* how, void* u) {
	visual_finish(e);
	return vi_delete(e, how, u);
}
std::int32_t vi_visual_change(lesh_editor* e, const lesh_invocation* how, void* u) {
	visual_finish(e);
	return vi_change(e, how, u);
}
std::int32_t vi_visual_yank(lesh_editor* e, const lesh_invocation* how, void* u) {
	visual_finish(e);
	return vi_yank(e, how, u);
}

// --- `.` (#99 answer 4) ------------------------------------------------------

std::int32_t vi_repeat(lesh_editor* editor, const lesh_invocation*, void* u) {
	vi_context& self = context_of(u);
	std::int32_t replayable = 0;
	if (lesh_last_change_replayable(editor, &replayable) != LESH_OK || replayable == 0) {
		// THE DOCUMENTED NO-OP. Either nothing has changed the buffer yet, or the
		// last change carried an insert (`ciw`foo) - and replaying its keys would
		// perform the change and then sit in insert mode waiting for text nobody
		// is about to type. Repeating an insert needs the typed text recorded as
		// well, which is invocation recording, which waits for N-3's replay
		// harness. See vi.h's boundary paragraph.
		return LESH_OK;
	}
	std::size_t length = 0;
	// The length question, whose answer is LESH_ERR_TOOSMALL with the length set;
	// only NOTFOUND means there is no record, and `replayable` has already ruled
	// that out.
	if (lesh_last_change_keys(editor, nullptr, 0, &length) == LESH_ERR_NOTFOUND
	    || length == 0)
		return LESH_OK;
	self.scratch.resize(length);
	if (lesh_last_change_keys(editor, self.scratch.data(), length, &length) != LESH_OK)
		return LESH_OK;
	// Through F-7's input stack, which means through the keymap: `.` re-types the
	// change rather than re-running an action, so a user who has rebound `w` gets
	// their `w` on the repeat too.
	return lesh_push_input(editor, self.scratch.data(), self.scratch.size());
}

// --- The table ---------------------------------------------------------------

struct entry {
	const char* name;
	lesh_action_fn fn;
};

constexpr entry actions[] = {
	// motions
	{"vi_backward_char", vi_backward_char},
	{"vi_forward_char", vi_forward_char},
	{"vi_line_up", vi_line_up},
	{"vi_line_down", vi_line_down},
	{"vi_first_nonblank", vi_first_nonblank},
	{"vi_word_next", vi_word_next},
	{"vi_word_prev", vi_word_prev},
	{"vi_word_end", vi_word_end},
	{"vi_blank_word_next", vi_blank_word_next},
	{"vi_blank_word_prev", vi_blank_word_prev},
	{"vi_blank_word_end", vi_blank_word_end},
	{"vi_find_forward", vi_find_forward},
	{"vi_find_backward", vi_find_backward},
	{"vi_till_forward", vi_till_forward},
	{"vi_till_backward", vi_till_backward},
	{"vi_find_char_target", vi_find_char_target},
	{"vi_find_repeat", vi_find_repeat},
	{"vi_find_repeat_reverse", vi_find_repeat_reverse},
	// counts
	{"vi_digit_argument", vi_digit_argument},
	{"vi_digit_or_line_start", vi_digit_or_line_start},
	// operators and their verbs
	{"vi_delete_operator", vi_delete_operator},
	{"vi_change_operator", vi_change_operator},
	{"vi_yank_operator", vi_yank_operator},
	{"vi_operator_abort", vi_operator_abort},
	{"vi_delete", vi_delete},
	{"vi_change", vi_change},
	{"vi_yank", vi_yank},
	// text objects
	{"vi_object_inner_word", vi_object_inner_word},
	{"vi_object_a_word", vi_object_a_word},
	{"vi_object_inner_blank_word", vi_object_inner_blank_word},
	{"vi_object_a_blank_word", vi_object_a_blank_word},
	{"vi_object_inner_pair", vi_object_inner_pair},
	{"vi_object_a_pair", vi_object_a_pair},
	{"vi_line_object", vi_line_object},
	// the single-key edits
	{"vi_delete_char", vi_delete_char},
	{"vi_substitute_char", vi_substitute_char},
	{"vi_replace_char", vi_replace_char},
	{"vi_replace_char_with", vi_replace_char_with},
	{"vi_toggle_case", vi_toggle_case},
	{"vi_delete_to_line_end", vi_delete_to_line_end},
	{"vi_change_to_line_end", vi_change_to_line_end},
	{"vi_yank_line", vi_yank_line},
	{"vi_put_after", vi_put_after},
	{"vi_put_before", vi_put_before},
	// mode entries
	{"vi_insert_mode", vi_insert_mode},
	{"vi_insert_at_line_start", vi_insert_at_line_start},
	{"vi_append", vi_append},
	{"vi_append_at_line_end", vi_append_at_line_end},
	{"vi_open_below", vi_open_below},
	{"vi_open_above", vi_open_above},
	{"vi_command_mode", vi_command_mode},
	{"vi_normal_reset", vi_normal_reset},
	{"vi_visual_mode", vi_visual_mode},
	{"vi_visual_exit", vi_visual_exit},
	{"vi_visual_swap_ends", vi_visual_swap_ends},
	{"vi_visual_delete", vi_visual_delete},
	{"vi_visual_change", vi_visual_change},
	{"vi_visual_yank", vi_visual_yank},
	// repeat
	{"vi_repeat", vi_repeat},
	// the emacs side of the one store
	{"yank", yank},
};

} // namespace

namespace lesh::leshper {

vi_context* vi_context_create() { return new vi_context; }

void vi_context_destroy(vi_context* self) noexcept { delete self; }

std::size_t register_vi_actions(::lesh_registry& reg, vi_context& self) {
	std::size_t registered = 0;
	for (const entry& one : actions) {
		if (lesh_action_register(&reg, one.name, one.fn, &self) == LESH_OK)
			++registered;
	}
	return registered;
}

} // namespace lesh::leshper
