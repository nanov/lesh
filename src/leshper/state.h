#pragma once

#include "leshper/decoration.h"
#include "leshper/kill_store.h"
#include "leshper/text.h"
#include "leshper/undo.h"
#include "substrate/assert.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lesh::leshper {

// The action and keymap registries dispatch runs through, owned outside `state`
// and reached from it. Defined in keymap.h; forward-declared here because a
// state is copied and compared without ever needing to know what is in one.
class editing_context;

// The counter that makes a stale async result droppable (spec §1, N-4).
//
// Bumped on every buffer mutation and on nothing else. A worker result carries
// the generation it was computed against; the editor compares and drops. A type
// of its own rather than a bare integer, so a result cannot be compared against
// a row count, a job id, or another state's counter by accident - N-4 asks that
// applying a stale result be structurally impossible, and the first structure
// that buys is not being able to name the wrong number.
class generation {
public:
	constexpr generation() noexcept = default;

	constexpr void bump() noexcept { ++_value; }
	[[nodiscard]] constexpr uint64_t value() const noexcept { return _value; }

	friend constexpr bool operator==(generation, generation) noexcept = default;

private:
	uint64_t _value = 0;
};

// ---------------------------------------------------------------------------
// The selection (A-1, F-10, spec §6.3), and the region derived from it.
// ---------------------------------------------------------------------------

// Half of the selection: an anchor and whether the region is live. The other
// half is the CURSOR, which is the head - there is no second stored position.
//
// #96's resolution, in one type: emacs's mark/point projects onto it exactly,
// vi visual projects onto it with `o` as a swap and inclusiveness as a +1
// grapheme the vi MODE applies, and helix uses it raw (a helix-mode motion
// writes `anchor := old head`, which makes noun-then-verb a keymap behaviour
// rather than a core variant). zle's `MARK`/`CURSOR`/`REGION_ACTIVE` is the
// same trio; zle's one divergence - linewise as `REGION_ACTIVE=2` - is
// information we keep in the mode, so nothing here knows about shape.
//
// The fields are private and only `state` may write them. That is the whole of
// what future-proofs multi-cursor (#96 decision 5): the primary selection is a
// scalar today and a plural accessor arrives additively, and neither is
// possible if callers poke fields.
class selection {
public:
	constexpr selection() noexcept = default;

	[[nodiscard]] constexpr position anchor() const noexcept { return _anchor; }
	[[nodiscard]] constexpr bool active() const noexcept { return _active; }

	friend constexpr bool operator==(const selection&, const selection&) noexcept = default;

private:
	friend struct state;

	position _anchor;
	bool _active = false;
};

// The derived region: exclusive half-open, in buffer positions, endpoints on
// grapheme-cluster boundaries.
//
// Exclusive to match the parser's spans and #93's decoration ranges, so that a
// selection can be handed to either without a fence-post translation at the
// boundary. Empty when `from == to`, which is what an active selection whose
// anchor sits on the cursor is - it renders as nothing and is still live.
struct region {
	position from;
	position to;

	[[nodiscard]] constexpr bool empty() const noexcept { return from == to; }

	friend constexpr bool operator==(const region&, const region&) noexcept = default;
};

// ---------------------------------------------------------------------------
// The keymap stack, and what is still a placeholder beside it.
//
// NOTHING BELOW IS A PLACEHOLDER ANY MORE. Each of the three began as a NAMED
// type with no behaviour, so that A-1's state struct already had the field the
// spec says it has and its ticket could fill a type in rather than thread a new
// member through every signature. The keymap stack was the first and stopped
// being one with #118; the pager was the second and stopped being one with
// #138; the decorations were the third and stopped being one with #141.
// ---------------------------------------------------------------------------

// The stack of keymaps dispatch runs against (A-8, F-8 to F-12, spec §6.4).
//
// NOT a placeholder any more (#118). The mode is the BASE of this stack and the
// enum that used to be a mode never existed: `set_mode` swaps `layers.front()`,
// sub-modes - visual, operator-pending, the pager, history search - are pushes
// above it, and dispatch walks top-down.
//
// It holds NAMES, not keymaps. Three things follow, and each of them is why:
// the tables themselves live in the keymap registry, which is environment
// rather than editor state; a state stays copyable and comparable for N-3's
// replay, which a pointer into a registry would not survive; and `bind`
// replacing `emacs` re-points every stack that names it, with no stack able to
// outlive the table it points into.
//
// It holds ENCODED KEY BYTES for the held prefix, not key events, which is what
// lets this type live here rather than in keymap.h: event.h includes this file,
// so a member of type `std::vector<key_event>` would close a cycle. The
// encoding is keymap.h's `encode_key` - a canonical six bytes per symbolic key
// event, not a terminal's bytes.
struct keymap_stack {
	// Base first. `layers.front()` is the mode; everything above it is a push.
	// Default-constructed to emacs, which is what every state in leshper meant
	// before there was anything else to mean.
	std::vector<std::string> layers{std::string(default_mode)};

	// The prefix being held while something longer might still arrive (F-5).
	// Encoded key events, empty when nothing is held.
	std::string pending;

	// WHEN the hold resolves, and the whole of F-5's "take a deadline, do not own
	// a clock" in one member. leshper never calls a clock: the loop, which read
	// the bytes and therefore knows when they arrived, hands the instant in and
	// asks for it back to arm its poll timeout. The same steady_clock instant
	// decode.h calls `input_instant`, spelled out so this header need not include
	// the decoder. Unset when nothing is held, or when a hold began on a path
	// that had no instant to give (a test, or the replay harness - both of which
	// resolve the hold by pressing the next key instead).
	std::optional<std::chrono::steady_clock::time_point> hold_deadline;

	// zle's `viopp`, written down (#117 decision 6): the verb stores itself here
	// and pushes the operator-pending keymap; the next motion runs with extend
	// semantics; dispatch pops, invokes this, and clears. Empty means none.
	// Helix mode never sets it - its verbs read the always-present selection.
	std::string pending_operator;

	// The COUNT a digit action has accumulated and dispatch has not yet handed
	// to anything (#99: counts that multiply, `d2w` and `3dd`).
	//
	// Here rather than in the vi module for the same reason `pending_operator`
	// is here: it is dispatch state, read and cleared by the loop's own turn,
	// and a mode swap has to be able to drop it. It is also not vi's alone -
	// emacs's `universal-argument` is the identical mechanism under another
	// name, which is why the ABI door onto it (`lesh_numeric_argument_set`) is
	// spelled in neither paradigm's vocabulary.
	//
	// An integer pair rather than an accumulating string, because this is the
	// keystroke path and a digit must not allocate.
	std::int64_t pending_count = 0;
	bool has_pending_count = false;

	// The mode every state starts in.
	static constexpr std::string_view default_mode = "emacs";

	[[nodiscard]] std::string_view mode() const noexcept {
		return layers.empty() ? std::string_view{} : std::string_view{layers.front()};
	}

	// A full mode switch: the base is replaced and everything pushed above it
	// goes with it.
	//
	// Dropping the pushes is the decision. A sub-mode is a modifier ON a mode -
	// visual over vi_command, operator-pending over the map whose verb pushed it -
	// and leaving one stranded over a base it was never pushed onto would shadow
	// the new mode with bindings that no longer mean anything. `i` from visual
	// mode lands in insert mode, not in visual-over-insert.
	void set_mode(std::string_view name) {
		layers.assign(1, std::string(name));
		clear_hold();
		pending_operator.clear();
		clear_count();
	}

	// A sub-mode arrives (visual, operator-pending, the pager, history search).
	void push(std::string_view name) {
		layers.emplace_back(name);
		clear_hold();
	}

	// Pops the topmost sub-mode. False - and nothing happens - at the base: a
	// mode is not something one can pop out of, only something one swaps.
	bool pop() {
		if (layers.size() <= 1)
			return false;
		layers.pop_back();
		clear_hold();
		return true;
	}

	[[nodiscard]] bool holding() const noexcept { return !pending.empty(); }

	void clear_hold() noexcept {
		pending.clear();
		hold_deadline.reset();
	}

	void clear_count() noexcept {
		pending_count = 0;
		has_pending_count = false;
	}

	friend bool operator==(const keymap_stack&, const keymap_stack&) noexcept = default;
};

// ---------------------------------------------------------------------------
// What `.` repeats (#99 answer 4, spec §6.5): the last change, recorded as the
// keys that made it.
//
// THE DEPARTURE, AND THE ARGUMENT. #99 writes the record as "(action name,
// count)". That is not enough for the four things #99's own resolution names as
// having to repeat: `dw`, `x`, `dd`, `diw`. Three of those four are TWO OR THREE
// dispatches - an operator that pushes, a count, a motion or a text object, and
// then the operator run by dispatch on the region - so there is no single
// (action, count) that is the change. What there is, is the KEY SEQUENCE, which
// is one thing for all four, and which F-7 already has a door for: a replay is
// `lesh_push_input` of what was typed, read back through the keymap exactly as
// though the user had typed it again. vim's `.` is literally this.
//
// So the record is the typed text of the sequence, plus the two questions that
// decide whether replaying it HERE would mean what it meant THERE:
//
//   `mode_changed` - the change also left the mode it was made in. That is what
//   an insert-carrying change looks like from dispatch's side: `ciw` deletes and
//   lands in insert mode, and replaying the keys alone would delete and then sit
//   waiting for text nobody is about to type. #99's "an insert-carrying last
//   change makes `.` a no-op" is this bit, and dispatch can see it without
//   knowing what an insert mode is.
//
//   `mode` - the mode base the sequence began in. This is what keeps the letters
//   of an insert-mode word out of the record's way: typing `foo` in insert mode
//   IS three buffer changes, and each is recorded, but each is recorded as having
//   been made in `vi_insert`, so `.` pressed in `vi_command` declines them.
//
// `typable` is the honest limit of the mechanism: F-7's door carries BYTES, so a
// sequence containing a key that types nothing - `<Up>`, `<C-w>` - cannot be
// pushed back through it. Such a change is recorded and marked unreplayable
// rather than half-replayed.
//
// Paradigm-neutral by construction: nothing here names vi, and helix's `.` -
// when helix mode arrives - reads the same record.
struct change_replay {
	// --- The sequence being typed now ------------------------------------
	std::string in_progress;
	std::string started_in;
	bool in_progress_typable = true;
	// The stack depth the sequence began at. What makes "is the machine still
	// mid-command" answerable without dispatch knowing what a mode is: a layer
	// pushed DURING the sequence (`f` waiting for its target character) is the
	// command asking for more input, where a layer that was already there
	// (visual mode, which one sits in) is not.
	std::size_t in_progress_depth = 1;

	// --- The last completed change ---------------------------------------
	bool present = false;
	std::string keys;
	std::string mode;
	bool mode_changed = false;
	bool typable = false;

	[[nodiscard]] bool recorded() const noexcept { return present; }

	// Whether replaying the record in `current_mode` would mean what it meant.
	[[nodiscard]] bool replayable(std::string_view current_mode) const noexcept {
		return present && typable && !mode_changed && mode == current_mode;
	}

	// A new sequence begins. `clear()` rather than assignment on the accumulator
	// keeps the capacity it grew to, so a steady state of `x x x` allocates once.
	void begin(std::string_view in_mode, std::size_t depth) {
		in_progress.clear();
		started_in.assign(in_mode);
		in_progress_typable = true;
		in_progress_depth = depth;
	}

	// The sequence is over and was not a change. Everything the accumulator held
	// goes back to its default, not just the keys: two states that are both
	// between commands must COMPARE equal (N-3), and a leftover "the last
	// sequence started in emacs" would make a state that had pressed a key
	// differ from one that had not.
	void abandon() {
		in_progress.clear();
		started_in.clear();
		in_progress_typable = true;
		in_progress_depth = 1;
	}

	friend bool operator==(const change_replay&, const change_replay&) noexcept = default;
};

// Namespaced annotations anchored to buffer positions (A-7) - NOT a placeholder
// any more (#141). The type is `decoration.h`'s, which this file includes: the
// spans #93's reactors emit with interned semantic style ids, and the virtual
// text #133's autosuggester draws beyond the end of what was typed. It lives in
// its own header because the normalization that turns nested spans into the
// sorted, disjoint list the renderer walks wants a translation unit, and this
// header is included by nearly everything in leshper.

// ---------------------------------------------------------------------------
// The pager (#138, F-28 to F-30, spec §6.9). NOT a placeholder any more.
//
// ONE PAGER, THREE CLIENTS (#137's resolution): tab completion, history search
// (F-32) and the autosuggestion candidate view are the same surface with the
// same keymap over the same list. Nothing below names any of the three, which
// is what makes that true rather than aspirational - a client fills the list,
// says which span of the buffer an accepted candidate replaces, and commits.
// ---------------------------------------------------------------------------

// What a candidate IS, in `ls -F`'s vocabulary, and the whole of v1's
// description (#137 decision 3: "a kind marker as the only description").
//
// The kind decides two things and only two: the one-character marker drawn
// after the candidate, and what an accepted candidate is followed by. A richer
// description column is recorded as later work and arrives as a FIELD on the
// candidate, not as a change here.
//
// `plain` and `word` are both marker-less and differ only in the second
// question. A history line is `plain` - it replaces the buffer and nothing
// trails it; a command name or a plain file is `word`, and a space follows,
// because the next thing the user types is an argument. That distinction is
// the reason the enum is not simply the file-type set.
//
// Numbered explicitly and appended to, never reordered: the ABI's
// LESH_PAGER_* constants are these numbers (registry.cpp static_asserts it).
enum class pager_kind : std::uint32_t {
	plain = 0,       // no marker, nothing trails it
	word = 1,        // no marker, a space trails it
	directory = 2,   // `/`, and a `/` trails it
	executable = 3,  // `*`, a space trails it
	symlink = 4,     // `@`, a space trails it
};

struct pager_candidate {
	// What is SHOWN and what is inserted, before the kind's trailing byte. The
	// bare name: the completer hands `bin`, not `bin/`, and the kind says the
	// rest. Two spellings of the same candidate would otherwise disagree the
	// first time one of them was filtered.
	std::string text;
	pager_kind kind = pager_kind::plain;

	friend bool operator==(const pager_candidate&, const pager_candidate&) noexcept = default;
};

// The pager's whole state (F-28: candidates, selection, scroll; F-29: filter).
//
// IN THE REPLAY COMPARE, CANDIDATES INCLUDED, and that is the deliberate answer
// N-3 asks for. `decorations` is compared-always-equal because a reactor's
// output arrives asynchronously and the loop applies it whenever it lands;
// this list does not. §6.9 decision 2 makes v1 completion SYNCHRONOUS on the
// loop thread - the list is written by an action, inside the turn the key
// started, exactly like a buffer edit - and everything a turn of the machine
// writes is what a replay compares. A replayed session that offered a different
// list offered a different pager, and the one component the comparison could
// not see would be the newest one.
struct pager_state {
	// Everything the client offered, in the order it offered it.
	std::vector<pager_candidate> candidates;

	// Indices into `candidates` that match `filter`, recomputed whenever either
	// changes (F-29). Materialized rather than re-derived per read so that
	// `selected` indexes ONE list and the renderer and the movement actions
	// cannot disagree about which one.
	std::vector<std::uint32_t> matching;

	// What the user has typed since the pager opened (F-29). NOT in the buffer:
	// the pager's own keymap routes unbound printables here through its default
	// action, so filtering never edits the line it is completing.
	std::string filter;

	// Into `matching`, never into `candidates`.
	std::size_t selected = 0;

	// The first grid row shown. A HINT rather than the answer: the renderer
	// clamps it and overrides it when the selection would fall outside, which is
	// what makes a resize need no reflow here (#123's argument, kept). It is
	// still stored, because it is where an explicit page-scroll writes and
	// because what a key did to the pager belongs in the replay compare.
	std::uint16_t scroll_row = 0;

	// Whether the pager is SHOWING. False while a client is filling it, and
	// false after F-30 inserted a common prefix instead of opening.
	bool open = false;

	// The span an accepted candidate replaces. The client's, set at open: the
	// token under the cursor for completion, the whole buffer for history
	// search, `[cursor, cursor)` for a suggestion.
	position replace_from;
	position replace_to;

	[[nodiscard]] bool showing() const noexcept { return open && !matching.empty(); }

	void clear() {
		candidates.clear();
		matching.clear();
		filter.clear();
		selected = 0;
		scroll_row = 0;
		open = false;
		replace_from = position{};
		replace_to = position{};
	}

	friend bool operator==(const pager_state&, const pager_state&) noexcept = default;
};

// Input delivered but not yet consumed (A-1, F-7).
//
// Today it holds only what lesh code injected (zle's `zle -U`), because that is
// this ticket's one producer. The other producer is F-5's incremental decoder -
// a codepoint may arrive split across reads and the tail waits here for the
// rest. That half is a later ticket: map #82 carries input decoding as fog,
// unblocked by #97's terminal floor.
struct pending_input {
	std::string injected;

	[[nodiscard]] bool empty() const noexcept { return injected.empty(); }

	friend bool operator==(const pending_input& a, const pending_input& b) noexcept {
		return a.injected == b.injected;
	}
};

// ---------------------------------------------------------------------------

// The one editor state (A-1).
//
// Explicit and whole: no editor state in globals. That is the thing zle got
// wrong - `zsh Src/Zle/zle.h` scatters it across globals - and fish got right
// with `reader_data_t`. Everything a turn of the state machine may read or
// write is reachable from here, which is what lets a test build a state, feed
// it events and assert on the result with no terminal anywhere (A-2, N-3).
//
// Copyable and comparable, both for N-3: a recorded event sequence replayed
// against a fresh state must produce a state EQUAL to the first run's, and
// equality has to be one operator over every field rather than a comparison of
// whichever fields a test remembered to check.
struct state {
	text_buffer buffer;
	position cursor;
	keymap_stack keymaps;
	// What the reactors have said about the buffer, applied (#93, #141). The
	// loop writes it in `take_batch` under the generation drop rule, and
	// `lay_out` reads it: this is the whole of the highlighter's and the
	// autosuggester's visible output.
	//
	// DERIVED, AND DELIBERATELY OUT OF `operator==` - see the argument there.
	decorations marks;
	pending_input pending;
	pager_state pager; // #138
	generation gen;
	undo_history undo;

	// The one kill store (#99 answer 3, spec §6.5): emacs's kill ring and vi's
	// unnamed register, keyed, with the unnamed key as the default.
	//
	// EDITOR STATE, not environment, and that is the decision. It goes here
	// beside the undo history rather than into `editing_context` because it is
	// something a TURN of the machine writes - `dw` puts text in it - and because
	// N-3's replay compares states: a recorded session that killed and then put
	// must reproduce what was put, and a store living in the shared environment
	// would be invisible to that comparison. It outlives a line for the same
	// reason the loop's state does: the loop clears the buffer at accept and
	// keeps the state, so `dd` on one line and `p` on the next reach the same
	// entry.
	kill_store kills;

	// What `.` repeats, and the in-progress half dispatch accumulates into.
	change_replay repeat;

	// The terminal size the last resize event reported. Here rather than in the
	// renderer because A-3 delivers resize as an event on the ordinary input path
	// and the state machine needs somewhere to put it. Reflow (F-38) belongs to
	// the renderer, which does not exist yet.
	uint16_t columns = 0;
	uint16_t rows = 0;

	// The environment dispatch runs through: the action registry (#110), the
	// keymap registry (#118), and the loop-side dispatcher over both.
	//
	// SHARED, not owned by value, and not a global. A registry is what the user
	// has bound and registered - environment, not editor state - so copying a
	// state must not fork it and N-3's equality must not compare it. Shared
	// rather than borrowed so that ADR-0007's "everything has an owner that frees
	// it" holds with no wiring: the last state referring to a context frees it.
	// Null until the first dispatch, which builds the default environment; the
	// real loop constructs one explicitly and hands it to the states it owns.
	std::shared_ptr<editing_context> context;

	// --- The selection (spec §6.3) ------------------------------------------
	//
	// Methods, not a field, and that is the decision rather than a style: #96's
	// answer to multi-cursor is access discipline. Every read and every write
	// goes through here, so the day the primary selection becomes one of several
	// these bodies change and no call site does.

	[[nodiscard]] bool selection_active() const noexcept { return _selected.active(); }
	[[nodiscard]] position selection_anchor() const noexcept { return _selected.anchor(); }

	// The region, derived: `active ? [min(anchor, cursor), max(anchor, cursor)) : none`.
	//
	// Derived rather than stored, because a stored pair is a pair that can
	// disagree with the cursor - and the cursor moves on every motion, which is
	// exactly when helix says the selection moved too.
	[[nodiscard]] std::optional<region> selection_range() const noexcept {
		if (!_selected.active())
			return std::nullopt;
		const position at = _selected.anchor();
		return at <= cursor ? region{at, cursor} : region{cursor, at};
	}

	// Starts a selection: anchor here, region live. The cursor is untouched -
	// it is the head, and the caller is about to move it.
	void set_anchor(position at) noexcept {
		assert_on_boundary(at);
		_selected._anchor = at;
		_selected._active = true;
	}

	// Deactivates the region and KEEPS the anchor.
	//
	// Emacs's mark outliving `deactivate-mark` is the reason the model is an
	// anchor plus a flag rather than an optional position: `C-x C-x` after the
	// region has gone inactive still has somewhere to go. Nothing in leshper
	// exercises that yet; the primitive is what #96 decided, not a subset of it.
	void drop_selection() noexcept { _selected._active = false; }

	// vi visual's `o`, and helix's `;`-family flip: the head becomes the tail.
	// Unconditional, because swapping an inactive pair is a no-op a caller
	// should not have to guard.
	void swap_anchor_and_cursor() noexcept {
		std::swap(_selected._anchor, cursor);
	}

	// Both halves at once. The undo restore and the ABI commit are its callers:
	// each has an anchor AND a flag to put back, and doing it in two calls would
	// leave a turn of the loop looking at half a restored selection.
	void set_selection(position at, bool active) noexcept {
		assert_on_boundary(at);
		_selected._anchor = at;
		_selected._active = active;
	}

	// Moves the anchor and leaves `active` alone. The marker rules in
	// `apply_edit` are the one caller: an edit relocates the anchor, it never
	// decides whether a region is live.
	void move_anchor(position to) noexcept {
		assert_on_boundary(to);
		_selected._anchor = to;
	}

	// One step of history, through the state (F-4, spec §6.3): text, cursor AND
	// selection go back together.
	//
	// Here rather than at each call site because the alternative is every caller
	// of `undo.undo` remembering to restore two more things - and the caller that
	// forgets leaves the anchor pointing into text that no longer exists. The
	// history itself still takes the pieces by reference and still knows nothing
	// about `state`, which is what keeps it testable against a bare buffer.
	bool undo_one() {
		if (!undo.undo(buffer, cursor, _selected._anchor, _selected._active))
			return false;
		assert_on_boundary(_selected._anchor);
		return true;
	}

	bool redo_one() {
		if (!undo.redo(buffer, cursor, _selected._anchor, _selected._active))
			return false;
		assert_on_boundary(_selected._anchor);
		return true;
	}

	// N-3's equality: one operator over every field a replay must reproduce,
	// rather than a comparison of whichever fields a test remembered to check.
	//
	// `marks` IS NOT IN IT, and that is a decision rather than an omission.
	//
	// N-3 replays a RECORDED EVENT SEQUENCE against a fresh state and demands an
	// equal state at the end. Decorations are not in that sequence: they are what
	// a reactor computed, off this thread, against a snapshot - a worker's, or
	// the shell thread's (ADR-0009) - and whether the answer had come back by the
	// time the last recorded event was consumed is a matter of scheduling. Two
	// runs of the same recording can differ in `marks` while agreeing on every
	// byte the user typed and every byte the buffer holds, so comparing them
	// would make N-3's guarantee a race. Worse, the answers themselves are a
	// function of things the recording does not carry: `$PATH`, the filesystem,
	// and which functions the shell has defined (#135). A replay on another
	// machine paints a different picture of the same line, correctly.
	//
	// #118 kept the registries out for the neighbouring reason - they are
	// ENVIRONMENT, reached through `editing_context` rather than owned - and this
	// is the same boundary approached from the other side: `marks` is owned here,
	// because a turn of the loop writes it and it must travel with a copied
	// state, but it is DERIVED from environment and not from input. What
	// `operator==` compares is what the typed input determines.
	//
	// `decorations::operator==` still exists, so a test that wants to assert on
	// the applied spans compares them directly and says so - which is what the
	// decoration tests do.
	friend bool operator==(const state& a, const state& b) noexcept {
		return a.buffer == b.buffer && a.cursor == b.cursor && a._selected == b._selected
		    && a.keymaps == b.keymaps && a.pending == b.pending
		    && a.pager == b.pager && a.gen == b.gen && a.undo == b.undo
		    && a.kills == b.kills && a.repeat == b.repeat
		    && a.columns == b.columns && a.rows == b.rows;
	}

private:
	// F-3's invariant on the anchor, the same discipline the cursor is held to:
	// an endpoint of a region sits ON a cluster, never inside one. Debug-only,
	// and deliberately asked of the BUFFER rather than of #108's segmenter
	// directly - the buffer is the one thing that knows how positions map onto
	// bytes, and when text.h's stepping becomes grapheme-wise this assertion
	// tightens with it instead of having to be found and edited. The ABI's
	// clamp-and-snap (registry.cpp) already enforces the cluster-level rule on
	// every offset a binding hands in, which is where untrusted offsets enter.
	void assert_on_boundary([[maybe_unused]] position at) const noexcept {
		LESH_ASSERT(buffer.is_boundary(at));
	}

	selection _selected;
};

} // namespace lesh::leshper
