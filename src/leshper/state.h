#pragma once

#include "leshper/text.h"
#include "leshper/undo.h"

#include <cstdint>
#include <string>

namespace lesh::leshper {

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
// Placeholders.
//
// Each of the five below is a NAMED type with no behaviour, so that A-1's state
// struct already has the field the spec says it has and its ticket fills a type
// in rather than threading a new member through every signature. Each says
// which ticket fills it. Nothing reads them, and the editor does not get to
// grow a use for one before its ticket lands.
// ---------------------------------------------------------------------------

// The selection region (A-1, F-10). Fills with #96.
//
// F-10 makes this a core primitive with defined semantics under every edit
// rather than a vi-visual afterthought, which is the decision #96 owes. The
// field exists so the state struct is complete; nothing reads it.
struct selection {
	friend constexpr bool operator==(const selection&, const selection&) noexcept {
		return true;
	}
};

// The stack of keymaps dispatch runs against (A-8, F-8 to F-12). Fills with
// #93, which decides the language-neutral action ABI a keymap binds to.
//
// Until then the editor dispatches from the hardcoded table in editor.cpp. That
// table is not a small keymap - it is a placeholder standing where the stack
// goes, and the action enum it dispatches to is not the action registry either.
struct keymap_stack {
	friend constexpr bool operator==(const keymap_stack&, const keymap_stack&) noexcept {
		return true;
	}
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
	selection selected;   // #96
	keymap_stack keymaps; // #93
	decorations marks;    // #93
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

	friend bool operator==(const state& a, const state& b) noexcept {
		return a.buffer == b.buffer && a.cursor == b.cursor && a.selected == b.selected
		    && a.keymaps == b.keymaps && a.marks == b.marks && a.pending == b.pending
		    && a.pager == b.pager && a.gen == b.gen && a.undo == b.undo
		    && a.columns == b.columns && a.rows == b.rows;
	}
};

} // namespace lesh::leshper
