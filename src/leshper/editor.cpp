#include "leshper/editor.h"

#include "leshper/text.h"
#include "leshper/undo.h"
#include "substrate/assert.h"
#include "substrate/log.h"

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

constexpr char32_t control_w = 0x17;  // Ctrl-W
constexpr char32_t control_a = 0x01;  // Ctrl-A
constexpr char32_t control_e = 0x05;  // Ctrl-E
constexpr char32_t control_b = 0x02;  // Ctrl-B
constexpr char32_t control_f = 0x06;  // Ctrl-F
constexpr char32_t control_h = 0x08;  // Ctrl-H, backspace on terminals that send it
constexpr char32_t control_underscore = 0x1F; // Ctrl-_, emacs undo
constexpr char32_t delete_character = 0x7F;   // DEL, what most terminals send for backspace

void encode_utf8(char32_t codepoint, std::string& out) {
	if (codepoint < 0x80) {
		out.push_back(static_cast<char>(codepoint));
	} else if (codepoint < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else if (codepoint < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
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

constexpr bool is_blank(char byte) noexcept {
	return byte == ' ' || byte == '\t' || byte == '\n';
}

// The start of the line the position is on (F-2: the buffer is a 2D text object,
// so `beginning-of-line` means this line rather than the whole buffer).
position line_start(const text_buffer& buffer, position at) {
	const std::string_view text = buffer.text();
	size_t offset = at.byte_offset();
	while (offset > 0 && text[offset - 1] != '\n')
		--offset;
	return position::from_byte_offset(offset);
}

position line_end(const text_buffer& buffer, position at) {
	const std::string_view text = buffer.text();
	size_t offset = at.byte_offset();
	while (offset < text.size() && text[offset] != '\n')
		++offset;
	return position::from_byte_offset(offset);
}

// Where a backward word deletion stops.
//
// Blank-separated, and that is a placeholder too: C-6 makes the lexer
// independently callable precisely so word-wise movement can use TOKEN
// boundaries, which is what makes `rm foo/bar` and `rm 'foo bar'` behave the way
// a shell user expects rather than the way a text editor does. Wiring that is
// work for the ticket that gives leshper its first look at the syntax layer;
// until then, skip trailing blanks, then take the run of non-blanks.
position backward_word_start(const text_buffer& buffer, position from) {
	const std::string_view text = buffer.text();
	size_t offset = from.byte_offset();
	while (offset > 0 && is_blank(text[offset - 1]))
		--offset;
	while (offset > 0 && !is_blank(text[offset - 1]))
		--offset;
	return position::from_byte_offset(offset);
}

// True when the action can change the buffer. Non-mutating actions must not
// touch the generation: a cursor move that bumped it would make every reactor
// recompute for nothing, which is the cost A-10's separation exists to avoid.
constexpr bool may_mutate(action a) noexcept {
	switch (a) {
	case action::self_insert:
	case action::delete_backward_char:
	case action::delete_backward_word:
	case action::undo:
	case action::redo:
		return true;
	default:
		return false;
	}
}

void perform(state& current, action chosen, const key_event& key, effects& out) {
	if (chosen == action::none)
		return; // bound to nothing: no state change, and nothing to redraw

	const generation before = current.gen;
	const position cursor_before = current.cursor;

	// Any action that is not plain typing ends the coalescing run (F-4): typing
	// `ab`, moving the cursor, then typing `cd` is two undo steps, because the
	// user watched the cursor move in between.
	if (chosen != action::self_insert)
		current.undo.break_coalescing();

	switch (chosen) {
	case action::none:
		return;

	case action::self_insert: {
		std::string text;
		encode_utf8(key.codepoint, text);
		apply_edit(current, current.cursor, current.cursor, text);
		break;
	}

	case action::delete_backward_char: {
		if (current.cursor == current.buffer.begin_position())
			return;
		const position from = current.buffer.previous_position(current.cursor);
		apply_edit(current, from, current.cursor, {});
		break;
	}

	case action::delete_backward_word: {
		const position from = backward_word_start(current.buffer, current.cursor);
		if (from == current.cursor)
			return;
		apply_edit(current, from, current.cursor, {});
		break;
	}

	case action::backward_char:
		current.cursor = current.buffer.previous_position(current.cursor);
		break;

	case action::forward_char:
		current.cursor = current.buffer.next_position(current.cursor);
		break;

	case action::beginning_of_line:
		current.cursor = line_start(current.buffer, current.cursor);
		break;

	case action::end_of_line:
		current.cursor = line_end(current.buffer, current.cursor);
		break;

	case action::undo:
		if (!current.undo.undo(current.buffer, current.cursor))
			return;
		current.gen.bump();
		break;

	case action::redo:
		if (!current.undo.redo(current.buffer, current.cursor))
			return;
		current.gen.bump();
		break;
	}

	// The half of A-10 a test cannot see: a non-mutating action must leave the
	// generation alone. Asserted rather than commented, because the failure is
	// silent - every reactor recomputing on a cursor move costs exactly what the
	// action/reactor split exists to save.
	LESH_ASSERT(may_mutate(chosen) || current.gen == before);

	// An action that changed nothing asks for nothing. `backward-char` at the
	// start of the line is the ordinary case, and a redraw per held-down arrow
	// key against a cursor that cannot move is a cost worth not paying.
	if (current.gen == before && current.cursor == cursor_before)
		return;

	// The A-10 loop, in three lines: an action edited the buffer, the generation
	// bumped, and the reactors are asked to recompute against the new one. The
	// answer comes back as a worker_result event carrying this same generation,
	// and is dropped if the buffer has moved on by then.
	out.push_back(render_request{});
	if (may_mutate(chosen) && current.gen != before)
		out.push_back(worker_request{current.gen});
}

void handle_key(state& current, const key_event& key, effects& out) {
	perform(current, binding_for(key), key, out);
}

// Drains what lesh code injected (F-7), through the same dispatch a typed key
// takes.
//
// Through the keymap, not around it: `zle -U` pushes characters onto the input
// stack and they are read back as though typed, so injecting a control
// character invokes its binding. Splicing the text into the buffer instead
// would be quicker and would break A-12 - leshper would be editing the line by
// a route the user's own bindings never see.
void drain_pending(state& current, effects& out) {
	while (!current.pending.empty()) {
		std::string text;
		text.swap(current.pending.injected);
		std::string_view rest{text};
		while (!rest.empty()) {
			const decoded next = decode_utf8(rest);
			rest.remove_prefix(next.length);
			handle_key(current, key_event::of(next.codepoint), out);
		}
	}
}

} // namespace

// The one buffer mutation. See the note in editor.h for why it is declared
// there: #93's ABI commit is its second caller, and a second copy of these
// eight lines would be a second mutation path.
void apply_edit(state& current, position from, position to, std::string_view with,
                const position* cursor_after) {
	edit_record edit;
	edit.at = from;
	edit.removed = std::string(current.buffer.slice(from, to));
	edit.inserted = std::string(with);
	edit.cursor_before = current.cursor;
	const position landed = current.buffer.replace(from, to, with);
	current.cursor = cursor_after != nullptr ? *cursor_after : landed;
	edit.cursor_after = current.cursor;
	current.undo.record(std::move(edit));
	current.gen.bump();
}

const char* name_of(action a) noexcept {
	switch (a) {
	case action::none:
		return "none";
	case action::self_insert:
		return "self-insert";
	case action::delete_backward_char:
		return "delete-backward-char";
	case action::delete_backward_word:
		return "delete-backward-word";
	case action::backward_char:
		return "backward-char";
	case action::forward_char:
		return "forward-char";
	case action::beginning_of_line:
		return "beginning-of-line";
	case action::end_of_line:
		return "end-of-line";
	case action::undo:
		return "undo";
	case action::redo:
		return "redo";
	}
	return "none";
}

action binding_for(const key_event& key) noexcept {
	if (key.named) {
		switch (key.key) {
		case named_key::backspace:
			return action::delete_backward_char;
		case named_key::left:
			return action::backward_char;
		case named_key::right:
			return action::forward_char;
		case named_key::home:
			return action::beginning_of_line;
		case named_key::end:
			return action::end_of_line;
		// The rest of the #97 floor's repertoire, which #111's decoder now
		// produces and this placeholder table binds to nothing. Deliberately a
		// default rather than eighteen cases returning action::none: the table is
		// #93's to replace, and enumerating keys here would be writing the keymap
		// this file exists to stand in for.
		default:
			break;
		}
		return action::none;
	}

	switch (key.codepoint) {
	case delete_character:
	case control_h:
		return action::delete_backward_char;
	case control_w:
		return action::delete_backward_word;
	case control_a:
		return action::beginning_of_line;
	case control_e:
		return action::end_of_line;
	case control_b:
		return action::backward_char;
	case control_f:
		return action::forward_char;
	case control_underscore:
		return action::undo;
	default:
		break;
	}

	// Everything printable types itself. The newline is deliberately absent:
	// F-35 makes Enter a decision the parser takes part in (complete → accept,
	// incomplete → insert a newline and keep editing), and binding it to
	// self-insert here would answer that question wrongly and quietly.
	if (key.codepoint >= 0x20 && key.codepoint != delete_character)
		return action::self_insert;
	return action::none;
}

effects step(state& current, const event& incoming) {
	log_event(incoming);   // #109's `event` category, and N-3's replay file
	effects out;

	if (const auto* key = std::get_if<key_event>(&incoming)) {
		handle_key(current, *key, out);
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
	}

	// Pending input is drained before the next real event is read, so a lesh
	// function that injects text sees it take effect within the turn that
	// injected it rather than on the next keystroke.
	drain_pending(current, out);
	return out;
}

} // namespace lesh::leshper
