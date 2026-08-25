#pragma once

#include "leshper/event.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::leshper {

// When the caller says it is. See input_decoder below: the decoder never asks.
//
// steady_clock rather than system_clock because the only arithmetic done on it
// is "has the escape timeout elapsed", and a wall clock stepping backwards over
// an NTP correction would answer that wrongly.
using input_instant = std::chrono::steady_clock::time_point;

// How long an ambiguous ESC prefix waits for the rest of its sequence (F-5).
//
// 25 ms, the value vim's `ttimeoutlen` and neovim's default settled on: long
// enough that a terminal's own escape sequence arrives whole even across a slow
// pty, short enough that pressing Escape does not feel stuck. Configurable
// because F-5 says configurable - a serial line or a laggy ssh session wants
// more - and the constructor takes it.
inline constexpr std::chrono::milliseconds default_escape_timeout{25};

// Bytes in, events out (F-5, F-6).
//
// The whole of input decoding, and deliberately the whole of it in one class:
// UTF-8 assembly, escape-sequence recognition and bracketed paste all consume
// from the same held prefix, because they are not separable. `ESC [ 2 0 0 ~`
// arriving split across two read()s is simultaneously an incomplete escape
// sequence and the start of a paste, and a pipeline of two objects would have
// to hand the ambiguity back and forth.
//
// THREE PROPERTIES, in the order they constrain the design.
//
// 1. No syscalls, and no terminal. #98 gives the event loop ownership of the
//    tty; this class has never heard of a file descriptor. It is handed the
//    bytes somebody else read. That is what lets the N-3 replay harness drive
//    it from a recorded byte log with no pty in the process.
//
// 2. No clock. Time is an ARGUMENT - `now` on the way in, a deadline on the way
//    out - never a call to a clock inside. A decoder that read the clock would
//    make replay a race: the same bytes would resolve an ESC prefix differently
//    depending on how fast the test machine ran. So the loop supplies the
//    instant, the decoder answers when to wake it, and a replay supplies the
//    instants the recording captured.
//
// 3. Pure in the sense that matters: the same bytes and the same instants yield
//    the same events, every time. There is exactly one piece of hidden state -
//    the prefix held back between feeds - and reset() clears it.
//
// USING IT, from the loop's side:
//
//	std::vector<event> events;
//	decoder.feed(just_read, now, events);
//	// ... poll(2) with a timeout derived from decoder.deadline() ...
//	decoder.expire(now_after_the_wait, events);
//
// deadline() answers nullopt when nothing is being held back, which is the
// ordinary case and means the loop may block indefinitely.
//
// WHAT IT KNOWS about terminals is the #97 floor and nothing else: the ANSI
// CSI/SS3 repertoire, hardcoded, assumed present. Never terminfo - linking
// ncurses violates ADR-0005 - and never a startup query, because a DA1
// round-trip is a latency tax on every session. An unrecognised sequence is
// consumed and dropped rather than guessed at, so a terminal that speaks more
// than the floor cannot inject garbage into the buffer.
//
// ALLOCATION (ADR-0007): two std::strings, both members, both freed by the
// destructor. The paste buffer is moved out into the event rather than copied,
// so a 100 KiB paste is not retained after it is delivered.
class input_decoder {
public:
	explicit input_decoder(
	    std::chrono::milliseconds escape_timeout = default_escape_timeout) noexcept
	    : _escape_timeout(escape_timeout) {}

	// Decodes `bytes`, appending whatever they completed to `out`. Bytes that do
	// not yet complete anything are held for the next call - which is F-5's
	// incremental requirement, and applies equally to a codepoint split across
	// two reads and to an escape sequence split across two reads.
	//
	// `now` is when these bytes arrived, and is used for one thing: anchoring the
	// deadline of an ESC prefix that is left held.
	void feed(std::string_view bytes, input_instant now, std::vector<event>& out);

	// Nothing more arrived by `now`. If an ESC prefix has been waiting past its
	// deadline, this is where it resolves: the ESC was the Escape key, and what
	// followed it is ordinary input (F-5).
	//
	// Takes `now` and checks rather than trusting the caller, so an early wake -
	// poll(2) returning on a signal, a replay stepping time coarsely - does not
	// resolve a sequence that was still legitimately in flight.
	void expire(input_instant now, std::vector<event>& out);

	// When expire() must be called if no more bytes arrive; nullopt when nothing
	// is waiting on time.
	//
	// A partial UTF-8 sequence deliberately does NOT arm this. F-5 scopes the
	// timeout to ambiguous key-sequence prefixes, and a lead byte is not
	// ambiguous - it is a promise of a fixed number of continuation bytes that a
	// terminal always keeps. Neither does an in-flight paste: F-6's payload can
	// take many reads to arrive, and timing one out would split it in two.
	[[nodiscard]] std::optional<input_instant> deadline() const noexcept { return _deadline; }

	// True while any input is held back undelivered.
	[[nodiscard]] bool holding() const noexcept { return !_held.empty() || _pasting; }

	// Drops everything held. For the loop's use when the terminal is handed to a
	// child and taken back (#98), where the bytes in flight belonged to the child.
	void reset() noexcept;

private:
	void drain(std::vector<event>& out);
	void arm(input_instant now) noexcept;

	// Each answers how many bytes of `rest` it consumed, and zero for "I need
	// more" - which is the one signal the whole incremental design rests on.
	size_t consume_one(std::string_view rest, std::vector<event>& out);
	size_t consume_escape(std::string_view rest, std::vector<event>& out);
	size_t consume_csi(std::string_view rest, std::vector<event>& out);
	size_t consume_ss3(std::string_view rest, std::vector<event>& out);
	size_t consume_paste(std::string_view rest, std::vector<event>& out);

	std::chrono::milliseconds _escape_timeout;
	std::optional<input_instant> _deadline;

	// The prefix that did not complete an event. Short by construction: at most
	// one partial UTF-8 scalar or one partial escape sequence.
	std::string _held;

	// The paste being accumulated (F-6). Grows to the size of the paste and is
	// moved out when the closing marker arrives.
	std::string _paste;
	bool _pasting = false;
};

// Replaces malformed UTF-8 with U+FFFD, taking ownership (N-4).
//
// Exposed because the paste path and the typed path must degrade IDENTICALLY -
// a byte that becomes U+FFFD when typed cannot survive into the buffer intact
// because it arrived in a paste - and because it is the one piece of this file
// worth testing on its own.
//
// The substitution is per MAXIMAL SUBPART (Unicode 5.2's recommended practice):
// a truncated three-byte sequence followed by an ASCII letter yields one U+FFFD
// and then the letter, not three replacement characters and then the letter.
//
// Substitution rather than pass-through, and the tension is real: text.h says
// the buffer carries malformed input as bytes without validating. It still
// does - the buffer validates nothing, and this is not the buffer. The decoder
// is the boundary where bytes become text, and it is the last place that can
// keep the invariant everything downstream assumes. #88's grapheme segmenter
// and #108's width tables are written against well-formed UTF-8; feeding them a
// stray 0x80 does not degrade gracefully, it puts the cursor in the wrong
// column and leaves it there. Two bytes on screen instead of one is a
// degradation the user can see and undo, which is what N-4 asks for.
[[nodiscard]] std::string well_formed(std::string text);

} // namespace lesh::leshper
