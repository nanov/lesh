#include "leshper/editor.h"

#include "leshper/keymap.h"
#include "leshper/registry.h"
#include "leshper/text.h"
#include "leshper/undo.h"
#include "substrate/assert.h"
#include "substrate/log.h"

#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace lesh::leshper {
namespace {

// THE EVENT HOOK (#109, #120), and the only logging call site in the editor.
//
// Every loop input passes through here on its way into `step()`, which is what
// makes the replay file complete: N-3 asks that a recorded sequence reproduce an
// identical state, and a sequence missing the events some branch forgot to
// record reproduces something else. Recording at the ENTRANCE rather than in the
// branches is the structure that buys that - an eighth kind of event cannot be
// added without passing this line.
//
// TWO SINKS, ONE RECORD. The jsonl object is what the harness replays; the text
// line is what a human reads. They are built from the same `incoming` and there
// is no second event serialization anywhere, which is the bug #109 named.
//
// REDACTION LIVES HERE. `debug` says what KIND of event arrived and how big it
// was; the codepoint a user typed and the text they pasted appear only at
// `trace`, which is compiled out of release entirely. So a `debug` log is safe
// to attach to a bug report and a `trace` log is not, and the difference is
// visible in one place rather than asserted in prose. The replay file is the
// deliberate exception - it necessarily holds the full input, which is what it
// is for, and it is opt-in through its own variable.
//
// The named keys go in as their enumerator's NUMBER rather than a name. There is
// no name table for `named_key` yet and inventing one here would be a second
// vocabulary for keys; #93 brings the keymap's own, and the reader on the far
// side of this file changes with it.
void log_event(const event& incoming) {
	// ONE relaxed atomic load for both questions - see substrate/log.h. This is
	// the keystroke path, and the whole cost of logging being off is this line.
	if (!log::enabled_or_recording(log::level::debug, log::category::event))
		return;

	log::record entry{log::category::event};
	if (const auto* key = std::get_if<key_event>(&incoming)) {
		entry.text("kind", "key")
			.number("cp", static_cast<uint64_t>(key->codepoint))
			.flag("named", key->named)
			.number("key", static_cast<uint64_t>(key->key))
			.flag("shift", key->modifiers.shift)
			.flag("alt", key->modifiers.alt)
			.flag("ctrl", key->modifiers.ctrl);
		LESH_LOG(log::level::debug, log::category::event, "key named=%d modifiers=%d",
		         static_cast<int>(key->named), static_cast<int>(key->modifiers.any()));
		LESH_LOG_TRACE(log::category::event, "key codepoint=U+%04X", static_cast<unsigned>(key->codepoint));
	} else if (const auto* resize = std::get_if<resize_event>(&incoming)) {
		entry.text("kind", "resize")
			.number("columns", static_cast<uint64_t>(resize->columns))
			.number("rows", static_cast<uint64_t>(resize->rows));
		LESH_LOG(log::level::debug, log::category::event, "resize %ux%u",
		         static_cast<unsigned>(resize->columns), static_cast<unsigned>(resize->rows));
	} else if (const auto* result = std::get_if<worker_result>(&incoming)) {
		entry.text("kind", "worker_result").number("gen", result->computed_against.value());
		LESH_LOG(log::level::debug, log::category::event, "worker_result gen=%llu",
		         static_cast<unsigned long long>(result->computed_against.value()));
	} else if (const auto* job = std::get_if<job_notice>(&incoming)) {
		entry.text("kind", "job")
			.number("pid", static_cast<int64_t>(job->pid))
			.number("status", static_cast<int64_t>(job->status));
		LESH_LOG(log::level::debug, log::category::event, "job pid=%d status=%d", job->pid, job->status);
	} else if (const auto* injected = std::get_if<injected_input>(&incoming)) {
		entry.text("kind", "injected").text("text", injected->text);
		LESH_LOG(log::level::debug, log::category::event, "injected %zu bytes", injected->text.size());
		LESH_LOG_TRACE(log::category::event, "injected text=%.*s",
		               static_cast<int>(injected->text.size()), injected->text.data());
	} else if (const auto* signal = std::get_if<signal_event>(&incoming)) {
		entry.text("kind", "signal").number("signal", static_cast<int64_t>(signal->signal_number));
		LESH_LOG(log::level::debug, log::category::event, "signal %d", signal->signal_number);
	} else if (const auto* paste = std::get_if<paste_event>(&incoming)) {
		entry.text("kind", "paste").text("text", paste->text);
		LESH_LOG(log::level::debug, log::category::event, "paste %zu bytes", paste->text.size());
		LESH_LOG_TRACE(log::category::event, "paste text=%.*s",
		               static_cast<int>(paste->text.size()), paste->text.data());
	}
	entry.commit();
}

// Reads one scalar value out of UTF-8 text, answering how many bytes it took.
//
// Used only to drain injected input (F-7) - real keys arrive decoded. A
// malformed byte is handed back as the codepoint of its own value rather than
// dropped or replaced: N-4 asks that malformed bytes degrade gracefully, and
// this degrades, though re-encoding turns one bad byte into two. F-5's
// incremental decoder is the ticket that does this properly, for both producers
// at once.
struct decoded {
	char32_t codepoint;
	size_t length;
};

decoded decode_utf8(std::string_view text) {
	const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
	const unsigned char lead = byte(0);
	const auto continues = [&](size_t i) {
		return i < text.size() && (byte(i) & 0xC0) == 0x80;
	};
	if (lead < 0x80)
		return {lead, 1};
	if ((lead & 0xE0) == 0xC0 && continues(1))
		return {static_cast<char32_t>(((lead & 0x1F) << 6) | (byte(1) & 0x3F)), 2};
	if ((lead & 0xF0) == 0xE0 && continues(1) && continues(2))
		return {static_cast<char32_t>(((lead & 0x0F) << 12) | ((byte(1) & 0x3F) << 6)
		                              | (byte(2) & 0x3F)),
		        3};
	if ((lead & 0xF8) == 0xF0 && continues(1) && continues(2) && continues(3))
		return {static_cast<char32_t>(((lead & 0x07) << 18) | ((byte(1) & 0x3F) << 12)
		                              | ((byte(2) & 0x3F) << 6) | (byte(3) & 0x3F)),
		        4};
	return {lead, 1};
}

// Dispatch: one key event, through the keymap stack and into the action
// registry (#117, #118, spec §6.4).
//
// WHAT USED TO BE HERE was `perform`, a switch over an `action` enum, and it was
// a SECOND implementation of the nine built-ins - builtin_actions.cpp being the
// first, written against the ABI. `LeshperAbiEquivalence` existed to catch the
// two drifting apart. There is one implementation now: this resolves a NAME and
// `loop_harness::invoke` runs whatever is registered under it, so a user's
// rebinding of `backward_char` and the built-in are reached by the identical
// path, which is the whole of what F-13 asks for.
//
// One consequence worth naming, because a test used to pin the opposite: motion
// is grapheme-cluster-wise everywhere now. The enum path stepped scalar values
// through `text_buffer::next_position`; the ABI asks the editor, which asks
// #108's segmenter. `LeshperAbiBuiltins.MotionIsGraphemeWiseWhereTheEnumPathIs
// StillScalarWise` pinned that disagreement and is retired with this ticket -
// the disagreement is gone because one of the two paths is.
void invoke_action(state& current, std::string_view name, std::string_view keys,
                   effects& out) {
	editing_context& context = context_of(current);
	invocation how;
	// The bytes the sequence would have typed, which is what `self_insert`
	// inserts and what the ABI documents `keys` to be. A named key contributes
	// nothing: `<Up>` is not text.
	encoded_keys_as_text(keys, how.keys);
	// THE COUNT (#119), read and CLEARED before the call. An action that leaves
	// a numeric argument behind (`lesh_numeric_argument_set` - vi's digits,
	// emacs's `universal-argument`) is always setting the NEXT dispatch's and
	// never re-reading its own, which is what makes `d2w` two dispatches that
	// compose instead of one that has to remember.
	how.numeric_argument = current.keymaps.pending_count;
	how.has_numeric_argument = current.keymaps.has_pending_count;
	current.keymaps.clear_count();

	action_result ran = context.loop().invoke(current, name, how);
	// A miss is a miss and not a crash (ADR-0008): a keymap naming an action
	// nobody registered leaves the state alone, exactly as an unbound key does.
	if (ran.status == LESH_ERR_NOTFOUND) {
		LESH_LOG(log::level::debug, log::category::dispatch,
		         "no action registered for a bound name");
		return;
	}
	// Taken rather than copied, and the empty case is taken WHOLE. One key
	// producing effects into an empty list is the overwhelmingly common turn, and
	// appending would buy a second allocation on the keystroke path for nothing.
	if (out.empty()) {
		out = std::move(ran.produced);
		return;
	}
	out.insert(out.end(), std::make_move_iterator(ran.produced.begin()),
	           std::make_move_iterator(ran.produced.end()));
}

// --- The count, the operator, and the record: dispatch's three hooks (#119) --

// One dispatch, with everything that has to happen AROUND an action wrapped
// where every caller gets it: `handle_key` and `keymap_expire` both come here.
//
// The three hooks, and what each of them is:
//
//   THE OPERATOR-PENDING CONSUMPTION (#117 decision 6, zle's `viopp` written
//   down). A verb parked its name in `pending_operator` and pushed; the next
//   key's action runs with the anchor dropped where the cursor was, so whatever
//   it moves the cursor to IS the span; then the layer pops, the verb runs on
//   the region, and the slot clears. The verb sees a selection and nothing else,
//   which is why an operator and a visual-mode verb are the same action.
//
//   THE COUNT, handed to the action in its invocation and cleared (see
//   invoke_action).
//
//   THE CHANGE RECORD, accumulated as keys and completed by a generation bump.
//
// Nothing here names vi. An operator slot no mode sets costs one comparison
// against an empty string per keystroke; helix mode, which never sets it, pays
// exactly that and gets the record for its own `.` for free.
void run_binding(state& current, std::string_view name, std::string_view candidate,
                 effects& out) {
	change_replay& record = current.repeat;
	if (record.in_progress.empty())
		record.begin(current.keymaps.mode(), current.keymaps.layers.size());
	encoded_keys_as_text(candidate, record.in_progress);
	record.in_progress_typable =
		record.in_progress_typable && encoded_keys_are_text(candidate);

	const generation before = current.gen;
	const std::size_t depth_before = current.keymaps.layers.size();
	const bool operator_pending = !current.keymaps.pending_operator.empty();
	if (operator_pending) {
		// EXTEND SEMANTICS, and they are one line because #96 made them one: the
		// anchor goes where the cursor is and the cursor is the head, so a motion
		// that moves is a motion that selected. A text object overrides both ends
		// and is no special case at all.
		current.set_anchor(current.cursor);
	}

	invoke_action(current, name, candidate, out);

	// Consume the pending operator unless the machine is still asking for input.
	// Four ways it can be: the action cleared the slot (an abort, or a doubled
	// form that did not match its verb), it pushed a layer (`f`, waiting for the
	// character to find), it left a count behind (the `2` of `d2w`), or a prefix
	// is being held.
	const bool still_asking = current.keymaps.pending_operator.empty()
	                       || current.keymaps.layers.size() > depth_before
	                       || current.keymaps.has_pending_count
	                       || current.keymaps.holding();
	if (operator_pending && !still_asking) {
		std::string verb;
		verb.swap(current.keymaps.pending_operator);
		current.keymaps.pop();   // the operator-pending layer the verb pushed
		invoke_action(current, verb, {}, out);
	}

	// The record. A generation bump - and only that - completes a change (A-10
	// makes the generation the one honest answer to "did the buffer move").
	if (!(current.gen == before)) {
		record.present = true;
		record.keys.assign(record.in_progress);
		record.mode.assign(record.started_in);
		record.mode_changed = current.keymaps.mode() != record.started_in;
		record.typable = record.in_progress_typable;
		record.abandon();
		return;
	}
	// No change, and nothing is waiting for more of this command: the sequence is
	// over and was not a change, so it is forgotten rather than prefixed onto the
	// next one.
	const bool mid_command = current.keymaps.holding()
	                      || !current.keymaps.pending_operator.empty()
	                      || current.keymaps.has_pending_count
	                      || current.keymaps.layers.size() > record.in_progress_depth;
	if (!mid_command)
		record.abandon();
}

void handle_key(state& current, const key_event& key,
                std::optional<std::chrono::steady_clock::time_point> now, effects& out) {
	editing_context& context = context_of(current);

	// The candidate is what is held plus what was just pressed. Short by
	// construction - six bytes a key - so the copy stays inside the string's
	// small-buffer and the keystroke path allocates nothing for it.
	std::string candidate = current.keymaps.pending;
	encode_key(key, candidate);

	const resolution what = resolve_keys(context.keymaps(), current.keymaps, candidate,
	                                     is_self_inserting(key));
	if (what.what == resolution::kind::hold) {
		current.keymaps.pending = std::move(candidate);
		// The deadline is the LOOP's instant plus the configured timeout, never a
		// clock read here (F-5). A caller with no instant to give leaves the hold
		// unarmed, and the next key resolves it.
		current.keymaps.hold_deadline =
			now.has_value() ? std::optional{*now + context.key_timeout} : std::nullopt;
		return;
	}

	current.keymaps.clear_hold();
	if (what.what == resolution::kind::dispatch)
		run_binding(current, what.action, candidate, out);
}

void drain_pending(state& current, effects& out) {
	while (!current.pending.empty()) {
		std::string text;
		text.swap(current.pending.injected);
		std::string_view rest{text};
		while (!rest.empty()) {
			const decoded next = decode_utf8(rest);
			rest.remove_prefix(next.length);
			// No instant: injected text is not something a terminal sent, so
			// there is no arrival time to anchor a hold to. A prefix begun by
			// injected input waits for the next real key rather than for a clock.
			handle_key(current, key_event::of(next.codepoint), std::nullopt, out);
		}
	}
}

} // namespace

namespace {

// The marker rules, and the whole of what an edit does to a selection (#96
// decision 4, spec §6.3). `[begin, end)` is the replaced span, already clamped
// and ordered the way text_buffer::replace clamps and orders it, and `inserted`
// is how many bytes went in.
//
// Three cases, and they are total:
//
//   the edit lies entirely AFTER the anchor  -> the anchor does not move
//   the edit lies entirely BEFORE it         -> the anchor shifts by the delta
//   the anchor is INSIDE the replaced span   -> it clamps to the edit's start
//
// The first branch also settles the gravity question at a pure insertion, where
// `begin == end == anchor` and the other two readings would both apply: the
// anchor stays put and the typed text falls INSIDE the region. That is emacs's
// default marker insertion type, and emacs's mark is the paradigm §6.3 says
// projects onto this one exactly. It is also the only answer under which typing
// into an empty active region grows it rather than leaving it forever empty.
//
// `active` is not consulted and not changed. An inactive selection's anchor
// still tracks the buffer - emacs's mark survives `deactivate-mark` and has to
// still mean something when the region comes back - and a region collapsed to
// nothing by an edit that ate it stays live and renders as nothing.
//
// The one place in leshper outside text_buffer that does position arithmetic,
// and it is here rather than in state.h because a marker rule is a fact about
// an EDIT. text.h's rule survives it: the offsets go straight back into
// position::from_byte_offset and no caller sees a size_t.
void adjust_anchor_for_edit(state& current, position begin, position end, size_t inserted) {
	const size_t anchor = current.selection_anchor().byte_offset();
	const size_t first = begin.byte_offset();
	const size_t last = end.byte_offset();

	if (anchor <= first)
		return;  // the edit is at or after the anchor: nothing moved under it
	if (anchor >= last) {
		// Entirely before: shift by the delta, in two unsigned steps so that a
		// deletion larger than the insertion cannot underflow on the way.
		current.move_anchor(position::from_byte_offset(anchor - (last - first) + inserted));
		return;
	}
	current.move_anchor(begin);  // inside the replaced span
}

} // namespace

// The one buffer mutation. See the note in editor.h for why it is declared
// there: #93's ABI commit is its second caller, and a second copy of these
// eight lines would be a second mutation path.
void apply_edit(state& current, position from, position to, std::string_view with,
                const position* cursor_after) {
	// The span the buffer will actually replace, computed BEFORE the replace
	// because the marker rules are about the old text's coordinates. Clamped and
	// ordered here the same way text_buffer::replace does it internally, so the
	// anchor is adjusted against the edit that happened rather than the edit that
	// was asked for.
	const position begin = current.buffer.clamped(from);
	position end = current.buffer.clamped(to);
	if (end < begin)
		end = begin;

	edit_record edit;
	edit.at = from;
	edit.removed = std::string(current.buffer.slice(from, to));
	edit.inserted = std::string(with);
	edit.cursor_before = current.cursor;
	edit.anchor_before = current.selection_anchor();
	edit.selection_active_before = current.selection_active();
	const position landed = current.buffer.replace(from, to, with);
	current.cursor = cursor_after != nullptr ? *cursor_after : landed;
	edit.cursor_after = current.cursor;
	adjust_anchor_for_edit(current, begin, end, with.size());
	edit.anchor_after = current.selection_anchor();
	edit.selection_active_after = current.selection_active();
	current.undo.record(std::move(edit));
	current.gen.bump();
}

// F-5's hold, and the loop's two calls around it. See editor.h.
std::optional<std::chrono::steady_clock::time_point>
keymap_deadline(const state& current) noexcept {
	return current.keymaps.holding() ? current.keymaps.hold_deadline : std::nullopt;
}

effects keymap_expire(state& current, std::chrono::steady_clock::time_point now) {
	effects out;
	if (!current.keymaps.holding())
		return out;
	// An early wake - poll(2) returning on a signal, a replay stepping time
	// coarsely - must not resolve a sequence that was still in flight.
	if (current.keymaps.hold_deadline.has_value() && now < *current.keymaps.hold_deadline)
		return out;

	editing_context& context = context_of(current);
	std::string held;
	held.swap(current.keymaps.pending);
	current.keymaps.hold_deadline.reset();

	// The longest exact match is the held sequence's own, if it has one: nothing
	// longer can arrive now. A lone printable that was held only because
	// something longer was bound falls to the floor and types itself, which is
	// what makes `bind gg ...` survivable for anyone who wanted a single `g`.
	// Anything else is a prefix the user abandoned, and it is DROPPED rather than
	// replayed - typing the `x` out of an unfinished `<C-x>x` is the wrong
	// recovery from a mistake.
	const resolution what = resolve_expired_keys(context.keymaps(), current.keymaps, held);
	if (what.what == resolution::kind::dispatch)
		run_binding(current, what.action, held, out);
	return out;
}

effects step(state& current, const event& incoming,
             std::optional<std::chrono::steady_clock::time_point> now) {
	log_event(incoming);   // #109's `event` category, and N-3's replay file
	effects out;

	if (const auto* key = std::get_if<key_event>(&incoming)) {
		handle_key(current, *key, now, out);
	} else if (const auto* resize = std::get_if<resize_event>(&incoming)) {
		current.columns = resize->columns;
		current.rows = resize->rows;
		// F-37 keeps full repaints for resize and explicit requests only.
		out.push_back(render_request{});
	} else if (const auto* result = std::get_if<worker_result>(&incoming)) {
		// N-4, and the reason the generation exists at all: a result computed
		// against a buffer the user has since changed is DROPPED, unlooked at.
		// #90 makes the same rule structural on the worker's side - its arena is
		// reset per request, so a stale parse's memory is gone rather than merely
		// ignored. There is nothing to apply yet; #93 gives results a payload.
		if (result->computed_against == current.gen)
			out.push_back(render_request{});
	} else if (std::holds_alternative<job_notice>(incoming)) {
		// F-39: the loop prints the notice above the prompt and the edit line
		// repaints intact beneath it. No editor state changes - that is the
		// requirement, not an omission.
		out.push_back(render_request{});
	} else if (const auto* injected = std::get_if<injected_input>(&incoming)) {
		current.pending.injected += injected->text;
	} else if (std::holds_alternative<signal_event>(incoming)) {
		// The entrance exists, which is what A-9 asks for; the binding does not.
		// #98 settled the behaviour - Ctrl-C while typing runs the rebindable
		// `cancel-line` action AND fires the INT trap, the zsh way - but a signal
		// binding is keymap data like any other, and keymaps are #93's.
	} else if (const auto* pasted = std::get_if<paste_event>(&incoming)) {
		// F-6, #111's decoder having already established the wholeness: the
		// payload lands through the one apply_edit as ONE buffer mutation, ONE
		// undo entry, ONE generation bump, at the cursor, landing (apply_edit's
		// default) after the inserted text. A pasted newline rides straight into
		// `with` as a newline, not Enter - this is a mutation, not a key, so
		// F-35's accept-or-insert decision never runs.
		//
		// Coalescing is broken on BOTH sides, not just before: breaking only
		// before stops a paste from extending a preceding typing run, but
		// apply_edit's record() would otherwise leave the history "coalescing"
		// again afterwards (a paste is shaped like a plain insertion), letting
		// the next typed character fold into it. A paste is its own undo step,
		// full stop.
		current.undo.break_coalescing();
		apply_edit(current, current.cursor, current.cursor, pasted->text);
		current.undo.break_coalescing();
		// Mirrors what a mutating action's commit emits (loop_harness::invoke):
		// a redraw, plus a worker request tagged with the generation the mutation
		// just produced.
		out.push_back(render_request{});
		out.push_back(worker_request{current.gen});
	}

	// Pending input is drained before the next real event is read, so a lesh
	// function that injects text sees it take effect within the turn that
	// injected it rather than on the next keystroke.
	drain_pending(current, out);
	return out;
}

} // namespace lesh::leshper
