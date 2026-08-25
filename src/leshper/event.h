#pragma once

#include "leshper/state.h"

#include <cstdint>
#include <string>
#include <variant>

namespace lesh::leshper {

// A key that is not a character: what a terminal sends as an escape sequence
// and the decoder resolves before the editor ever sees it.
//
// Small and closed on purpose. F-12 says any key sequence binds to any action,
// which makes the real key model keymap data (#93) over a decoder's output (F-5,
// map #82's input-decoding fog). This enum is neither - it is the handful of
// named keys the first editing slice binds, and it grows only until #93 replaces
// it with the sequence-to-action table.
enum class named_key : uint8_t {
	backspace,
	left,
	right,
	home,
	end,
};

// One key press (A-9).
//
// A key is EITHER a decoded codepoint or one of the named keys above. C0
// controls arrive as codepoints, because that is literally what the terminal
// sends: Ctrl-W is U+0017 and there is no reason to invent a second name for
// it. Backspace is both - terminals send DEL (U+007F), and the table binds the
// codepoint and the named key to the same action.
//
// Decoded ALREADY, by something that is not this ticket. F-5 wants incremental
// UTF-8 (a codepoint may arrive split across reads) and a configurable timeout
// to resolve ambiguous sequence prefixes; both live in the decoder, which map
// #82 carries as fog behind #97's terminal floor. Until it exists a test
// constructs key events directly, which is exactly what A-2 promises.
struct key_event {
	char32_t codepoint = 0;
	bool named = false;
	named_key key = named_key::backspace;

	[[nodiscard]] static key_event of(char32_t codepoint) noexcept {
		return key_event{codepoint, false, named_key::backspace};
	}
	[[nodiscard]] static key_event of(named_key key) noexcept {
		return key_event{0, true, key};
	}
};

// The terminal changed size (A-3): SIGWINCH, re-queried by the loop and
// delivered here as an ordinary event rather than as a signal handler poking
// editor state.
struct resize_event {
	uint16_t columns = 0;
	uint16_t rows = 0;
};

// A reactor's answer, arriving from a worker (A-4, A-9).
//
// The generation is the whole point and the only field this ticket needs: the
// result was computed against a snapshot, the buffer may have moved on, and a
// result whose generation is not the current one is DROPPED. #90 makes that
// cheap to trust from the other side too - a worker parses into its own arena
// and resets it per request, so a stale parse's backing memory is gone rather
// than merely ignored.
//
// What a result CARRIES - decorations for the highlighter (F-20), a proposal
// for the autosuggester (F-24) - waits on #93's reactor interface, and so does
// the identity of which reactor answered. Nothing is added here before then;
// the drop rule is testable without a payload, and that is what this ticket
// owes.
struct worker_result {
	generation computed_against;
};

// Something happened to a job (F-39): a child finished, was stopped, was
// reaped. leshper's interest is narrow - the loop prints above the prompt and
// the edit line repaints intact beneath - so the event exists to trigger that
// repaint without state loss.
//
// Job control as a user-visible feature is out of scope for map #82; the
// terminal-ownership plumbing that produces these is #98's. The fields stay at
// what a repaint needs until a supervisor exists to say more.
struct job_notice {
	int pid = 0;
	int status = 0;
};

// Synthetic input from lesh code (F-7) - zle's `zle -U`.
//
// Text, not keys: `zle -U` pushes characters onto the input stack and they are
// read back as though typed, including through the keymap. So this lands in
// pending_input and is drained through the same dispatch a real key takes,
// rather than being spliced into the buffer behind the user's back (A-12 would
// not survive the shortcut).
struct injected_input {
	std::string text;
};

// A signal, delivered as an event rather than handled inside the editor (A-3,
// A-9, #98).
//
// #98 settled what the shell does about each one - Ctrl-C while typing runs the
// rebindable `cancel-line` action AND fires the INT trap, the zsh way - but a
// signal binding is keymap data like any other and the keymap stack is #93's.
// So this ticket carries the event in and binds it to nothing: the entrance
// exists, which is what A-9 asks for.
struct signal_event {
	int signal_number = 0;
};

// The only way into the editor (A-9).
//
// A closed variant rather than an interface with methods, so that "no side
// channel calls" is a property of the type: to make the editor do something you
// construct one of these six and hand it to step(). There is no other entry
// point, and a seventh kind of input has to be argued for by adding an
// alternative here.
using event = std::variant<key_event, resize_event, worker_result, job_notice,
                           injected_input, signal_event>;

} // namespace lesh::leshper
