#include "leshper/decode.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lesh::leshper {
namespace {

constexpr char escape_byte = '\x1B';
constexpr char32_t escape_codepoint = 0x1B;
constexpr char32_t replacement_character = 0xFFFD;
constexpr std::string_view replacement_bytes = "\xEF\xBF\xBD";

// The bracketed paste markers (F-6, #97's floor). The opening one is recognised
// through the ordinary CSI parser - it IS a CSI sequence - but the closing one
// has to be found by byte search, because everything between the two is opaque
// payload rather than input to decode.
constexpr std::string_view paste_end_marker = "\x1B[201~";
constexpr unsigned paste_start_parameter = 200;
constexpr unsigned paste_end_parameter = 201;

constexpr unsigned char byte_at(std::string_view text, size_t index) noexcept {
	return static_cast<unsigned char>(text[index]);
}

constexpr bool is_continuation(unsigned char value) noexcept {
	return (value & 0xC0) == 0x80;
}

// ---------------------------------------------------------------------------
// UTF-8 (F-5's incremental half, N-4's malformed half).
// ---------------------------------------------------------------------------

// What one scan of the front of the input found.
//
// `incomplete` and `malformed` are different answers and the difference is the
// whole ticket: incomplete means "these bytes could still become a character,
// wait for the rest", malformed means "these bytes never can, they are already
// wrong". Confusing the two is how a decoder either stalls on a bad byte or
// mangles a codepoint that arrived split across two read()s.
struct scalar_scan {
	char32_t codepoint = 0;
	size_t length = 0; // the bytes accounted for; meaningless when incomplete
	bool incomplete = false;
	bool malformed = false;
};

// How many bytes the lead byte promises, or zero for a byte that cannot lead.
//
// 0xC0 and 0xC1 are excluded from the two-byte range because the only things
// they can encode are overlong forms of ASCII, and 0xF5..0xFF because they
// encode past U+10FFFF. Rejecting them at the lead byte means the caller never
// has to decode-then-validate.
constexpr size_t sequence_length(unsigned char lead) noexcept {
	if (lead < 0x80)
		return 1;
	if (lead < 0xC2)
		return 0;
	if (lead < 0xE0)
		return 2;
	if (lead < 0xF0)
		return 3;
	if (lead < 0xF5)
		return 4;
	return 0;
}

// The Unicode table's legal range for the SECOND byte, which is where the
// remaining ill-formed sequences die: 0xE0 0x80 is an overlong form, 0xED 0xA0
// is a surrogate, 0xF4 0x90 is past U+10FFFF. Three comparisons instead of a
// decode followed by three range checks on the result.
constexpr std::pair<unsigned char, unsigned char> second_byte_range(unsigned char lead) noexcept {
	if (lead == 0xE0)
		return {0xA0, 0xBF};
	if (lead == 0xED)
		return {0x80, 0x9F};
	if (lead == 0xF0)
		return {0x90, 0xBF};
	if (lead == 0xF4)
		return {0x80, 0x8F};
	return {0x80, 0xBF};
}

// Reads one scalar value off the front, and on failure reports the MAXIMAL
// SUBPART - the longest prefix that was still a plausible beginning (Unicode
// 5.2's recommended practice). That is what makes `\xE4\xB8` followed by `a`
// come out as one U+FFFD and then `a`, rather than as two replacement
// characters, or as one that swallowed the letter.
scalar_scan scan_scalar(std::string_view rest) noexcept {
	const unsigned char lead = byte_at(rest, 0);
	const size_t want = sequence_length(lead);
	if (want == 0)
		return {.length = 1, .malformed = true};
	if (want == 1)
		return {.codepoint = lead, .length = 1};

	if (rest.size() < 2)
		return {.incomplete = true};
	const unsigned char second = byte_at(rest, 1);
	const auto [low, high] = second_byte_range(lead);
	if (second < low || second > high)
		return {.length = 1, .malformed = true};
	if (want == 2)
		return {.codepoint = static_cast<char32_t>(((lead & 0x1F) << 6) | (second & 0x3F)),
		        .length = 2};

	if (rest.size() < 3)
		return {.incomplete = true};
	const unsigned char third = byte_at(rest, 2);
	if (!is_continuation(third))
		return {.length = 2, .malformed = true};
	if (want == 3)
		return {.codepoint = static_cast<char32_t>(((lead & 0x0F) << 12) | ((second & 0x3F) << 6)
		                                           | (third & 0x3F)),
		        .length = 3};

	if (rest.size() < 4)
		return {.incomplete = true};
	const unsigned char fourth = byte_at(rest, 3);
	if (!is_continuation(fourth))
		return {.length = 3, .malformed = true};
	return {.codepoint = static_cast<char32_t>(((lead & 0x07) << 18) | ((second & 0x3F) << 12)
	                                           | ((third & 0x3F) << 6) | (fourth & 0x3F)),
	        .length = 4};
}

// Emits one key from the front of `rest`, or answers zero for "still arriving".
size_t emit_text(std::string_view rest, key_modifiers modifiers, std::vector<event>& out) {
	const scalar_scan scan = scan_scalar(rest);
	if (scan.incomplete)
		return 0;
	const char32_t codepoint = scan.malformed ? replacement_character : scan.codepoint;
	out.push_back(key_event::of(codepoint, modifiers));
	return scan.length;
}

// ---------------------------------------------------------------------------
// Escape sequences, at the #97 floor and no further.
// ---------------------------------------------------------------------------

// The parameters of a CSI sequence, reduced to the two the floor reads.
//
// Two, because that is all the floor's sequences carry: `ESC [ 1 ; 5 C` is
// Ctrl-Right, `ESC [ 3 ; 2 ~` is Shift-Delete, and no key at this level has a
// third. A general parameter vector would be an allocation per keystroke buying
// nothing that is read.
struct csi_parameters {
	unsigned first = 0;
	unsigned second = 0;
	bool has_first = false;
	// A `<`, `=`, `>` or `?` marker: a private-mode string or a device report.
	// #97 forbids startup queries, so nothing here ever asked for one, and an
	// unsolicited report is consumed and dropped rather than typed into the line.
	bool private_use = false;
};

csi_parameters parse_parameters(std::string_view text) noexcept {
	csi_parameters parsed;
	unsigned value = 0;
	bool seen = false;
	bool in_sub_parameter = false;
	size_t field = 0;

	const auto store = [&] {
		if (field == 0) {
			parsed.first = value;
			parsed.has_first = seen;
		} else if (field == 1) {
			parsed.second = value;
		}
	};

	for (const char raw : text) {
		const unsigned char value_byte = static_cast<unsigned char>(raw);
		if (value_byte >= '<' && value_byte <= '?') {
			parsed.private_use = true;
		} else if (raw == ';') {
			store();
			++field;
			value = 0;
			seen = false;
			in_sub_parameter = false;
		} else if (raw == ':') {
			// Sub-parameters (ECMA-48's colon form) carry nothing the floor
			// reads; skip the digits after one rather than folding them into the
			// parameter they qualify.
			in_sub_parameter = true;
		} else if (raw >= '0' && raw <= '9' && !in_sub_parameter) {
			if (value < 100000) // a bound, not a limit: no floor parameter exceeds 201
				value = value * 10 + static_cast<unsigned>(raw - '0');
			seen = true;
		}
	}
	store();
	return parsed;
}

// xterm's modifier encoding, shared by the CSI and the `~` forms: the parameter
// is one more than a bit-set. Anything below 2 means no modifiers, which covers
// both an absent parameter and the literal 1.
constexpr key_modifiers modifiers_from(unsigned parameter) noexcept {
	if (parameter < 2)
		return {};
	const unsigned mask = parameter - 1;
	return key_modifiers{
	    .shift = (mask & 1) != 0, .alt = (mask & 2) != 0, .ctrl = (mask & 4) != 0};
}

// The letter-final forms, which CSI and SS3 spell identically: `ESC [ A` and
// `ESC O A` are both Up, and which one a terminal sends depends on its keypad
// mode. Both are in the floor, so both are here.
constexpr std::optional<named_key> key_for_final(unsigned char final_byte) noexcept {
	switch (final_byte) {
	case 'A':
		return named_key::up;
	case 'B':
		return named_key::down;
	case 'C':
		return named_key::right;
	case 'D':
		return named_key::left;
	case 'H':
		return named_key::home;
	case 'F':
		return named_key::end;
	case 'P':
		return named_key::f1;
	case 'Q':
		return named_key::f2;
	case 'R':
		return named_key::f3;
	case 'S':
		return named_key::f4;
	default:
		return std::nullopt;
	}
}

// The `ESC [ n ~` forms. The gaps are real - there is no 9, 10, 16 or 22 - and
// 1/7 and 4/8 are both spellings of Home and End that different terminals
// disagree about.
constexpr std::optional<named_key> key_for_tilde(unsigned parameter) noexcept {
	switch (parameter) {
	case 1:
	case 7:
		return named_key::home;
	case 2:
		return named_key::insert;
	case 3:
		return named_key::delete_forward;
	case 4:
	case 8:
		return named_key::end;
	case 5:
		return named_key::page_up;
	case 6:
		return named_key::page_down;
	case 11:
		return named_key::f1;
	case 12:
		return named_key::f2;
	case 13:
		return named_key::f3;
	case 14:
		return named_key::f4;
	case 15:
		return named_key::f5;
	case 17:
		return named_key::f6;
	case 18:
		return named_key::f7;
	case 19:
		return named_key::f8;
	case 20:
		return named_key::f9;
	case 21:
		return named_key::f10;
	case 23:
		return named_key::f11;
	case 24:
		return named_key::f12;
	default:
		return std::nullopt;
	}
}

} // namespace

std::string well_formed(std::string text) {
	// The overwhelmingly common answer is "it already is", and that path must not
	// copy: a 100 KiB paste of ordinary text has an N-1 budget of 50 ms and no
	// reason to spend any of it rebuilding itself byte by byte.
	size_t at = 0;
	while (at < text.size()) {
		const scalar_scan scan = scan_scalar(std::string_view{text}.substr(at));
		if (scan.incomplete || scan.malformed)
			break;
		at += scan.length;
	}
	if (at == text.size())
		return text;

	std::string clean;
	clean.reserve(text.size());
	clean.append(text, 0, at);

	std::string_view rest = std::string_view{text}.substr(at);
	while (!rest.empty()) {
		const scalar_scan scan = scan_scalar(rest);
		if (scan.incomplete) {
			// Truncated at the end of the text, and nothing more is coming: what
			// was incomplete a byte ago is malformed now.
			clean.append(replacement_bytes);
			break;
		}
		if (scan.malformed) {
			clean.append(replacement_bytes);
		} else {
			clean.append(rest.substr(0, scan.length));
		}
		rest.remove_prefix(scan.length);
	}
	return clean;
}

void input_decoder::feed(std::string_view bytes, input_instant now, std::vector<event>& out) {
	_held.append(bytes);
	drain(out);
	arm(now);
}

void input_decoder::expire(input_instant now, std::vector<event>& out) {
	if (!_deadline || now < *_deadline)
		return;
	_deadline.reset();
	if (_held.empty() || _held.front() != escape_byte)
		return;

	// The prefix waited out its timeout with nothing to complete it, so the ESC
	// was the Escape key. Whatever followed - the `[` of a CSI the terminal never
	// finished, a letter that would have been Alt-something had it arrived in
	// time - goes back through the ordinary path as literal input. That is what
	// every editor with an escape timeout does, and the alternative (dropping the
	// tail) loses keystrokes.
	out.push_back(key_event::of(escape_codepoint));
	_held.erase(0, 1);
	drain(out);
	arm(now);
}

void input_decoder::reset() noexcept {
	_held.clear();
	_paste.clear();
	_pasting = false;
	_deadline.reset();
}

void input_decoder::arm(input_instant now) noexcept {
	// Only an ESC prefix waits on time. A partial UTF-8 scalar does not (a lead
	// byte is a promise the terminal keeps), and neither does a paste in flight
	// (F-6's payload may take many reads, and a timeout would cut it in half).
	if (_pasting || _held.empty() || _held.front() != escape_byte) {
		_deadline.reset();
		return;
	}
	// Anchored to when the ESC first arrived, not to the latest feed: bytes
	// trickling in one at a time must not push the deadline out forever.
	if (!_deadline)
		_deadline = now + _escape_timeout;
}

void input_decoder::drain(std::vector<event>& out) {
	size_t at = 0;
	while (at < _held.size()) {
		const std::string_view rest = std::string_view{_held}.substr(at);
		const size_t taken = _pasting ? consume_paste(rest, out) : consume_one(rest, out);
		if (taken == 0)
			break; // incomplete: hold it and wait for the next read
		at += taken;
	}
	_held.erase(0, at);
}

size_t input_decoder::consume_one(std::string_view rest, std::vector<event>& out) {
	if (byte_at(rest, 0) == static_cast<unsigned char>(escape_byte))
		return consume_escape(rest, out);
	// Everything else, control characters included, is text. C0 controls arrive
	// as their own codepoints because that is literally what the terminal sends
	// (event.h): Ctrl-W is U+0017, and there is no second name for it.
	return emit_text(rest, key_modifiers{}, out);
}

size_t input_decoder::consume_escape(std::string_view rest, std::vector<event>& out) {
	if (rest.size() < 2)
		return 0; // F-5's ambiguity: the timeout decides, in expire()

	const unsigned char next = byte_at(rest, 1);
	if (next == '[')
		return consume_csi(rest, out);
	if (next == 'O')
		return consume_ss3(rest, out);
	if (next == static_cast<unsigned char>(escape_byte)) {
		// ESC ESC. The first is a key press in its own right and the second
		// begins whatever follows it; resolving that here rather than waiting is
		// what lets Escape repeat when it is held down.
		out.push_back(key_event::of(escape_codepoint));
		return 1;
	}

	// ESC then anything else, arriving before the timeout: Alt (F-5). The tail
	// may itself be a multi-byte character split across reads, in which case
	// emit_text answers zero and the whole prefix waits.
	const size_t taken = emit_text(rest.substr(1), key_modifiers{.alt = true}, out);
	return taken == 0 ? 0 : taken + 1;
}

size_t input_decoder::consume_csi(std::string_view rest, std::vector<event>& out) {
	size_t at = 2; // past the ESC and the '['
	while (at < rest.size() && byte_at(rest, at) >= 0x30 && byte_at(rest, at) <= 0x3F)
		++at;
	const size_t parameters_end = at;
	while (at < rest.size() && byte_at(rest, at) >= 0x20 && byte_at(rest, at) <= 0x2F)
		++at;
	if (at == rest.size())
		return 0; // still arriving

	const unsigned char final_byte = byte_at(rest, at);
	if (final_byte < 0x40 || final_byte > 0x7E) {
		// Not a final byte at all: the terminal aborted mid-sequence, or a
		// keystroke raced one. Drop what was collected and leave the offending
		// byte for the next pass, so a Ctrl-C that interrupted a sequence still
		// reaches the editor instead of being swallowed with it.
		return at;
	}

	const size_t consumed = at + 1;
	const csi_parameters parameters = parse_parameters(rest.substr(2, parameters_end - 2));
	if (parameters.private_use)
		return consumed;

	if (final_byte == '~') {
		if (parameters.first == paste_start_parameter) {
			// F-6 opens here; everything until the closing marker is payload.
			_pasting = true;
			return consumed;
		}
		if (parameters.first == paste_end_parameter)
			return consumed; // a close with no open: there is nothing to end
		if (const std::optional<named_key> key = key_for_tilde(parameters.first))
			out.push_back(key_event::of(*key, modifiers_from(parameters.second)));
		return consumed;
	}

	if (const std::optional<named_key> key = key_for_final(final_byte)) {
		// `ESC [ C` and `ESC [ 1 ; 5 C` are Right and Ctrl-Right: the first
		// parameter is the vestigial 1 and the second carries the modifiers. A
		// first parameter that is neither absent nor 1 is a sequence the floor
		// does not speak, and guessing at it would type garbage into the line.
		if (!parameters.has_first || parameters.first == 1)
			out.push_back(key_event::of(*key, modifiers_from(parameters.second)));
		return consumed;
	}

	// Above the floor - a mouse report, a kitty-protocol key, a status reply.
	// Consumed, so it cannot be mistaken for typing; never guessed at.
	return consumed;
}

size_t input_decoder::consume_ss3(std::string_view rest, std::vector<event>& out) {
	if (rest.size() < 3)
		return 0;
	const unsigned char final_byte = byte_at(rest, 2);
	if (final_byte < 0x40 || final_byte > 0x7E)
		return 2; // aborted, by the same rule CSI uses
	if (const std::optional<named_key> key = key_for_final(final_byte))
		out.push_back(key_event::of(*key));
	return 3;
}

size_t input_decoder::consume_paste(std::string_view rest, std::vector<event>& out) {
	const size_t marker = rest.find(paste_end_marker);
	if (marker != std::string_view::npos) {
		_paste.append(rest.substr(0, marker));
		_pasting = false;

		// ONE event for the whole payload (F-6). Moved rather than copied, so a
		// large paste is not held a second time and the buffer is not retained
		// after delivery (ADR-0007).
		std::string text = std::move(_paste);
		_paste.clear();
		out.push_back(paste_event{well_formed(std::move(text))});
		return marker + paste_end_marker.size();
	}

	// No terminator yet. Everything is payload except a tail that could still
	// turn into one - a paste ending exactly on `ESC [ 20` must not be delivered
	// with those three bytes in it and then closed by the `1~` of the next read.
	size_t keep = std::min(rest.size(), paste_end_marker.size() - 1);
	while (keep > 0 && rest.substr(rest.size() - keep) != paste_end_marker.substr(0, keep))
		--keep;

	const size_t take = rest.size() - keep;
	if (take == 0)
		return 0;
	_paste.append(rest.substr(0, take));
	return take;
}

} // namespace lesh::leshper
