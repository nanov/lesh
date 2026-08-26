#pragma once

#include "leshper/state.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
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

	// The rest of the #97 floor's CSI/SS3 repertoire, added by #111 because a
	// decoder that resolves `ESC [ A` has to have somewhere to put the answer.
	// Arrows without their siblings would have been the odd shape: `up` and
	// `down` arrive down the same code path as `left` and `right`, off the same
	// three-byte sequence, and leaving them out would mean the decoder either
	// dropped them or invented a second vocabulary for them.
	//
	// `escape` is deliberately NOT here: a bare ESC is U+001B, and the rule two
	// paragraphs down - C0 controls arrive as codepoints - already answers it.
	up,
	down,
	page_up,
	page_down,
	insert,
	// Forward delete (`ESC [ 3 ~`), which is a different key from `backspace`
	// even though both are called Delete on some keyboards.
	delete_forward,
	f1,
	f2,
	f3,
	f4,
	f5,
	f6,
	f7,
	f8,
	f9,
	f10,
	f11,
	f12,
};

// What the terminal reported alongside a key (F-5).
//
// Three bools rather than a bit-set: the flags are read one at a time by a
// keymap lookup and never combined arithmetically, and a POD with named fields
// compares and prints without an operator overload set to maintain.
//
// The asymmetry is the terminal's, not ours. Ctrl-A does NOT arrive as `a` with
// `ctrl` set - it arrives as U+0001, because that is literally the byte, and
// inventing the other spelling here would give the same key two names. `ctrl`
// is set only where a terminal says so in a CSI parameter (`ESC [ 1 ; 5 C` is
// Ctrl-Right, which has no control-character spelling at all). `alt` is set by
// the decoder's ESC-prefix rule: `ESC` then `a` within the timeout is Alt-a.
struct key_modifiers {
	bool shift = false;
	bool alt = false;
	bool ctrl = false;

	[[nodiscard]] constexpr bool any() const noexcept { return shift || alt || ctrl; }

	friend constexpr bool operator==(key_modifiers, key_modifiers) noexcept = default;
};

// One key press (A-9).
//
// A key is EITHER a decoded codepoint or one of the named keys above. C0
// controls arrive as codepoints, because that is literally what the terminal
// sends: Ctrl-W is U+0017 and there is no reason to invent a second name for
// it. Backspace is both - terminals send DEL (U+007F), and the table binds the
// codepoint and the named key to the same action.
//
// Decoded ALREADY, by `input_decoder` in decode.h (#111). F-5's incremental
// UTF-8 - a codepoint may arrive split across reads - and the configurable
// timeout that resolves an ambiguous ESC prefix both live there, on the far
// side of this type. A test may still construct key events directly, which is
// what A-2 promises and what every editor test below does.
struct key_event {
	char32_t codepoint = 0;
	bool named = false;
	named_key key = named_key::backspace;
	key_modifiers modifiers;

	[[nodiscard]] static key_event of(char32_t codepoint, key_modifiers modifiers = {}) noexcept {
		return key_event{codepoint, false, named_key::backspace, modifiers};
	}
	[[nodiscard]] static key_event of(named_key key, key_modifiers modifiers = {}) noexcept {
		return key_event{0, true, key, modifiers};
	}

	friend constexpr bool operator==(const key_event&, const key_event&) noexcept = default;
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

// A bracketed paste, whole (F-6), added by #111.
//
// ONE event carrying the entire payload, and that is the requirement rather
// than an optimisation: F-6 says one buffer mutation, one undo step, one
// generation bump, one redraw, never per-character. A paste delivered as a
// thousand key events cannot be any of those things afterwards - the undo
// history would already hold a thousand records - so the wholeness has to be
// established here, at the decoder, before anything downstream gets a say.
//
// NOT injected_input, which is the other text-shaped event. That one is `zle
// -U`: text pushed onto the input stack and read back AS THOUGH TYPED, through
// the keymap, one key at a time. Routing a paste through it would re-introduce
// exactly the per-character path F-6 forbids, and would run every pasted
// control character's binding besides - which is the attack bracketed paste
// exists to stop.
//
// The payload is well-formed UTF-8: the decoder substitutes U+FFFD for
// malformed bytes here the same way it does for typed input (N-4). Nothing else
// is stripped - a pasted newline is a newline in the buffer, not Enter, because
// the event is a mutation rather than a key.
struct paste_event {
	std::string text;
};

// An armed timer came due (#168; #128 decision 3's `timer` topic).
//
// THE HOST SAYS WHEN AND THE EDITOR SAYS WHAT. The driver owns the clock and the
// due instants, so it is the only side that can notice; the dispatch is the
// editor's, because dispatching a name through the action registry is what
// `step` does for every other input and a second dispatcher in the driver was a
// second implementation of it. Before this, the loop looked the name up in the
// registry's timer table and invoked it through a harness of its own - which is
// how a timer expiry and a keystroke came to run through two different objects
// (#144's defect, in the one place it had survived).
//
// THE ACTION IS AN INTERNED HANDLE (see `registry::timer_actions`). It travels
// beside the id because the driver holds the timer table now and is the side that
// knows which action an id means - and it is a handle rather than a name because
// this event is constructed on every expiry, once a second on a `{time}` prompt,
// and an event that allocates is an event that allocates forever. `step` turns it
// back into a name with one indexed read.
struct timer_fired {
	std::uint64_t id = 0;
	std::uint32_t action = 0;
};

// The HOST's answer to `want_completion` (#168 Phase B).
//
// BORROWED, AND THE HOST OWNS THE STORAGE. `items` points into a list the host
// keeps across Tabs - `ui::editor_host` reuses one `completion_result` - and it
// is valid until the next `want_completion` that host carries out. That is the
// whole lifetime contract, and it is enough because the only consumer is
// `lesh_complete`'s caller reading candidates out one at a time inside the call
// that asked (`offer_completion` in builtin_actions.cpp copies each into its own
// scratch before it feeds the pager). A `std::vector<pager_candidate>` here
// would put the shell's candidate list, strings and all, on a channel N-2 says
// allocates nothing - and would be a second copy of a list the host already has.
//
// The generation is the one it was computed against, so the pair reads the same
// as `worker_request`/`worker_result` and stays droppable if #139's index stage
// ever moves this off the loop. Today the round trip is inside one turn and the
// generation cannot have moved.
struct completion_candidates {
	generation computed_against;
	const pager_candidate* items = nullptr;
	std::size_t count = 0;
	// The half-open byte range of the buffer the candidates REPLACE - the
	// COMPONENT under the cursor, not the whole token, so a `~/` or a `$` stays
	// in the buffer. `lesh_completion_range` hands these two out unchanged.
	std::size_t replace_from = 0;
	std::size_t replace_to = 0;
};

// NOTHING THE HOST CONSTRUCTS PER TURN ALLOCATES (N-2), and the compiler says so.
//
// TWO EXCEPTIONS, both of them text the user produced and neither of them a
// per-turn event: `injected_input` is `zle -U`'s payload and `paste_event` is a
// whole bracketed paste, which F-6 requires to arrive as ONE event precisely
// because the alternative is a thousand of them. A pasted kilobyte has to live
// somewhere; a timer's action name does not.
static_assert(std::is_trivially_copyable_v<key_event>);
static_assert(std::is_trivially_copyable_v<resize_event>);
static_assert(std::is_trivially_copyable_v<worker_result>);
static_assert(std::is_trivially_copyable_v<job_notice>);
static_assert(std::is_trivially_copyable_v<signal_event>);
static_assert(std::is_trivially_copyable_v<timer_fired>);
static_assert(std::is_trivially_copyable_v<completion_candidates>);

// The only way into the editor (A-9).
//
// A closed variant rather than an interface with methods, so that "no side
// channel calls" is a property of the type: to make the editor do something you
// construct one of these eight and hand it to step(). There is no other entry
// point, and a ninth kind of input has to be argued for by adding an
// alternative here - which is what #111 did for paste_event, F-6 being
// unstateable in the other six, and what #168 did for timer_fired.
using event = std::variant<key_event, resize_event, worker_result, job_notice,
                           injected_input, signal_event, paste_event, timer_fired>;

} // namespace lesh::leshper
