#pragma once

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
// The two below that are still placeholders are NAMED types with no behaviour,
// so that A-1's state struct already has the field the spec says it has and its
// ticket fills a type in rather than threading a new member through every
// signature. Each says which ticket fills it. Nothing reads them, and the editor
// does not get to grow a use for one before its ticket lands. The keymap stack
// was the third and is one no longer (#118).
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

	friend bool operator==(const keymap_stack&, const keymap_stack&) noexcept = default;
};

// Namespaced annotations anchored to buffer positions (A-7). Fills with #93,
// which defines the reactor subscription interface that produces them; the
// highlighter (F-20) and the autosuggester (F-24) are its first two clients and
// neither exists yet.
struct decorations {
	friend constexpr bool operator==(const decorations&, const decorations&) noexcept {
		return true;
	}
};

// The completion pager's state (F-28 to F-31). Fills when the completion engine
// is charted: map #82 carries it as fog, waiting on the provider interfaces
// (#94).
struct pager_state {
	friend constexpr bool operator==(const pager_state&, const pager_state&) noexcept {
		return true;
	}
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
	decorations marks; // #93
	pending_input pending;
	pager_state pager; // #94
	generation gen;
	undo_history undo;

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

	friend bool operator==(const state& a, const state& b) noexcept {
		return a.buffer == b.buffer && a.cursor == b.cursor && a._selected == b._selected
		    && a.keymaps == b.keymaps && a.marks == b.marks && a.pending == b.pending
		    && a.pager == b.pager && a.gen == b.gen && a.undo == b.undo
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
