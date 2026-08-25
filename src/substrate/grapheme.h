#pragma once

// UAX #29 extended grapheme cluster boundaries, and cluster width (#108).
//
// WHY THIS LIVES IN THE SUBSTRATE. The ticket left the placement open between
// here and src/leshper/. Three facts settle it:
//
//   It depends on nothing. The state machine reads one generated table and the
//   standard library, which is the substrate's entry requirement, and it is the
//   only module whose entry requirement it meets.
//
//   It has two consumers in two different modules. leshper's `position` (#107)
//   needs boundaries - where the cursor may rest, what one Backspace deletes.
//   Rendering needs cluster_width to place that cursor in a column. A type both
//   need and neither owns is what the substrate is for; putting it in leshper
//   would make the renderer link the editor to ask how wide a flag is.
//
//   syntax/ does not need it, and must not grow a reason to. POSIX tokenisation
//   is defined over bytes; a lexer that started asking about grapheme clusters
//   would be answering a question the shell language does not pose.
//
// Header-only, so lesh_substrate stays an INTERFACE target. The 33 KB of tables
// in unicode_tables.h are `inline constexpr`: one copy after linking, in
// .rodata, demand-paged, with no initialiser and nothing touched until the
// first non-ASCII byte arrives.
//
// WHAT IS DELIBERATELY ABSENT. UAX #29 word boundaries: editor word motion is a
// WORDCHARS-style configurable predicate, not a Unicode question - see §1 of
// docs/superpowers/research/2026-08-25-unicode-segmentation.md.

#include "substrate/unicode_tables.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lesh::grapheme {

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

struct properties {
	gcb cluster_break;
	incb conjunct;
	eaw east_asian_width;
	bool extended_pictographic;
	bool emoji_presentation;
};

constexpr properties lookup(char32_t cp) {
	using namespace tables;
	const std::uint16_t r = record(cp);
	return properties{
		static_cast<gcb>((r >> GCB_SHIFT) & GCB_MASK),
		static_cast<incb>((r >> INCB_SHIFT) & INCB_MASK),
		static_cast<eaw>((r >> EAW_SHIFT) & EAW_MASK),
		(r & EXT_PICT_BIT) != 0,
		(r & EMOJI_PRESENTATION_BIT) != 0,
	};
}

// ---------------------------------------------------------------------------
// UTF-8 decoding
//
// N-4: malformed input degrades, it does not abort and it does not stall. Every
// byte that cannot begin a well-formed sequence is consumed alone and reported
// as U+FFFD, so a caller walking a buffer of arbitrary bytes advances by at
// least one byte per step and terminates. Overlongs, surrogates and values above
// U+10FFFF are malformed by that definition, not merely unusual: accepting an
// overlong is how a length check gets bypassed downstream.
// ---------------------------------------------------------------------------

struct decoded {
	char32_t cp = 0xFFFD;
	std::size_t length = 0;   // bytes consumed; zero only at end of input
	bool valid = false;
};

constexpr bool is_continuation(unsigned char b) { return (b & 0xC0) == 0x80; }

constexpr decoded decode(std::string_view text, std::size_t pos) {
	if (pos >= text.size())
		return decoded{};

	const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
	const decoded bad{0xFFFD, 1, false};

	const unsigned char lead = byte(pos);
	if (lead < 0x80)
		return decoded{lead, 1, true};

	std::size_t length = 0;
	char32_t cp = 0;
	if ((lead & 0xE0) == 0xC0) { length = 2; cp = lead & 0x1F; }
	else if ((lead & 0xF0) == 0xE0) { length = 3; cp = lead & 0x0F; }
	else if ((lead & 0xF8) == 0xF0) { length = 4; cp = lead & 0x07; }
	else return bad;                                   // continuation byte, or F8..FF

	if (pos + length > text.size())
		return bad;                                    // truncated at end of input
	for (std::size_t i = 1; i < length; ++i) {
		if (!is_continuation(byte(pos + i)))
			return bad;                                // truncated by a new lead byte
		cp = (cp << 6) | (byte(pos + i) & 0x3F);
	}

	constexpr char32_t MIN_FOR_LENGTH[5] = {0, 0, 0x80, 0x800, 0x10000};
	if (cp < MIN_FOR_LENGTH[length])
		return bad;                                    // overlong
	if (cp >= 0xD800 && cp <= 0xDFFF)
		return bad;                                    // surrogate half
	if (cp > 0x10FFFF)
		return bad;                                    // outside the codespace

	return decoded{cp, length, true};
}

// ---------------------------------------------------------------------------
// The boundary state machine: GB1-GB13 plus GB999, sixteen rules.
//
// Evaluated at runtime rather than compiled to a DFA. The rules stay legible
// against UAX #29 revision 47, which is what makes the next GB9c a diff instead
// of an archaeology exercise; the cost is a handful of predictable branches over
// one table lookup, and the lookup is the part that misses cache.
// ---------------------------------------------------------------------------

class breaker {
public:
	constexpr breaker() = default;

	constexpr void reset() { *this = breaker{}; }

	// True if a cluster boundary falls immediately before `cp`. The first call
	// after construction or reset() is GB1 (sot / Any) and always says yes.
	constexpr bool boundary_before(char32_t cp) {
		const properties p = lookup(cp);
		const bool brk = _started ? decide(p) : true;
		advance(p, brk);
		return brk;
	}

private:
	bool _started = false;
	gcb _prev = gcb::other;
	bool _ri_odd = false;            // GB12/GB13: parity of the RI run ending at _prev
	std::uint8_t _conjunct = 0;      // GB9c: 0 none, 1 consonant seen, 2 linker seen
	std::uint8_t _pictographic = 0;  // GB11: 0 none, 1 ExtPict Extend*, 2 ...ZWJ

	constexpr bool decide(const properties& p) const {
		const gcb next = p.cluster_break;

		if (_prev == gcb::cr && next == gcb::lf)
			return false;                                                    // GB3
		if (_prev == gcb::control || _prev == gcb::cr || _prev == gcb::lf)
			return true;                                                     // GB4
		if (next == gcb::control || next == gcb::cr || next == gcb::lf)
			return true;                                                     // GB5
		if (_prev == gcb::l && (next == gcb::l || next == gcb::v
		                        || next == gcb::lv || next == gcb::lvt))
			return false;                                                    // GB6
		if ((_prev == gcb::lv || _prev == gcb::v) && (next == gcb::v || next == gcb::t))
			return false;                                                    // GB7
		if ((_prev == gcb::lvt || _prev == gcb::t) && next == gcb::t)
			return false;                                                    // GB8
		if (next == gcb::extend || next == gcb::zwj)
			return false;                                                    // GB9
		if (next == gcb::spacingmark)
			return false;                                                    // GB9a
		if (_prev == gcb::prepend)
			return false;                                                    // GB9b
		if (_conjunct == 2 && p.conjunct == incb::consonant)
			return false;                                                    // GB9c
		if (_pictographic == 2 && p.extended_pictographic)
			return false;                                                    // GB11
		if (next == gcb::regional_indicator && _ri_odd)
			return false;                                              // GB12/GB13
		return true;                                                       // GB999
	}

	constexpr void advance(const properties& p, bool brk) {
		const gcb next = p.cluster_break;

		// GB12/GB13 count RIs from the start of the run, and a break restarts it.
		_ri_odd = next == gcb::regional_indicator && (brk || !_ri_odd);

		// GB9c: Consonant [Extend Linker]* Linker [Extend Linker]* x Consonant.
		// Anything outside {Extend, Linker} after a Consonant abandons the run.
		if (p.conjunct == incb::consonant)
			_conjunct = 1;
		else if (p.conjunct == incb::linker)
			_conjunct = _conjunct == 0 ? 0 : 2;
		else if (p.conjunct != incb::extend)
			_conjunct = 0;

		// GB11: ExtPict Extend* ZWJ x ExtPict. Tested in this order because an
		// Extended_Pictographic codepoint restarts the run rather than extending it.
		if (p.extended_pictographic)
			_pictographic = 1;
		else if (_pictographic == 1 && next == gcb::extend)
			_pictographic = 1;
		else if (_pictographic == 1 && next == gcb::zwj)
			_pictographic = 2;
		else
			_pictographic = 0;

		_prev = next;
		_started = true;
	}
};

// ---------------------------------------------------------------------------
// Boundaries over a byte buffer
//
// Offsets are byte offsets, and `pos` is expected to sit on a codepoint start -
// which is what #107's opaque `position` will guarantee. A `pos` in the middle
// of a sequence still terminates and still returns a codepoint start, because
// the decoder treats every stray byte as its own unit.
// ---------------------------------------------------------------------------

// Index of the first boundary after `pos`, or text.size(). O(cluster).
constexpr std::size_t next_boundary(std::string_view text, std::size_t pos) {
	if (pos >= text.size())
		return text.size();

	// Two ASCII codepoints always break, the sole exception being CR LF. No
	// codepoint below U+0080 carries Extended_Pictographic, Indic_Conjunct_Break,
	// or any Grapheme_Cluster_Break value but Other, Control, CR and LF, so no
	// rule can join them - the generator asserts that vocabulary, so this stays
	// true or the tables stop building. A shell prompt is mostly this path.
	if (static_cast<unsigned char>(text[pos]) < 0x80
	    && (pos + 1 == text.size()
	        || static_cast<unsigned char>(text[pos + 1]) < 0x80))
		return text[pos] == '\r' && pos + 1 < text.size() && text[pos + 1] == '\n'
			? pos + 2 : pos + 1;

	breaker state;
	std::size_t i = pos;
	decoded d = decode(text, i);
	state.boundary_before(d.cp);          // GB1, consumed to prime the state
	i += d.length;

	while (i < text.size()) {
		d = decode(text, i);
		if (state.boundary_before(d.cp))
			return i;
		i += d.length;
	}
	return text.size();
}

namespace detail {

// A codepoint before which a boundary is guaranteed whatever precedes it -
// provided what precedes it is not Prepend, which joins anything (GB9b).
constexpr bool is_safe_start(char32_t cp) {
	const properties p = lookup(cp);
	switch (p.cluster_break) {
		case gcb::extend: case gcb::zwj: case gcb::spacingmark:   // GB9, GB9a
		case gcb::lf:                                             // GB3
		case gcb::l: case gcb::v: case gcb::t:                    // GB6-GB8
		case gcb::lv: case gcb::lvt:
		case gcb::regional_indicator:                             // GB12/GB13
			return false;
		default:
			break;
	}
	return !p.extended_pictographic                               // GB11
	       && p.conjunct != incb::consonant;                      // GB9c
}

constexpr std::size_t prev_codepoint_start(std::string_view text, std::size_t pos) {
	std::size_t k = pos - 1;
	while (k > 0 && pos - k < 4 && is_continuation(static_cast<unsigned char>(text[k])))
		--k;
	const decoded d = decode(text, k);
	return d.valid && d.length == pos - k ? k : pos - 1;
}

} // namespace detail

// Index of the last boundary strictly before `pos`, or 0.
//
// Grapheme breaking is not decidable backwards - GB12/GB13 need the parity of
// the whole regional-indicator run, GB9c the whole conjunct sequence - so this
// backs up to a codepoint no rule can reach across and re-runs forwards. That is
// O(cluster) for ordinary text and O(run) for a line of flags, which is inherent
// rather than an implementation choice.
constexpr std::size_t prev_boundary(std::string_view text, std::size_t pos) {
	if (pos > text.size())
		pos = text.size();
	if (pos == 0)
		return 0;

	std::size_t safe = pos;
	while (safe > 0) {
		const std::size_t start = detail::prev_codepoint_start(text, safe);
		safe = start;
		if (start == 0 || !detail::is_safe_start(decode(text, start).cp))
			continue;
		const std::size_t before = detail::prev_codepoint_start(text, start);
		if (lookup(decode(text, before).cp).cluster_break != gcb::prepend)
			break;
	}

	breaker state;
	std::size_t last = safe;
	for (std::size_t i = safe; i < pos;) {
		const decoded d = decode(text, i);
		if (state.boundary_before(d.cp))
			last = i;
		i += d.length;
	}
	return last;
}

// ---------------------------------------------------------------------------
// Width
//
// UAX #11 tells implementers not to do the obvious thing: "The East_Asian_Width
// property is not intended for use by modern terminal emulators without
// appropriate tailoring on a case-by-case basis." So the tables answer with a
// CLASS, after widecharwidth's design, and a policy object resolves the class to
// a column count. Callers that want a number get one; callers that care are made
// to see that Ambiguous was a choice somebody made for them.
//
// And the width is of the CLUSTER, not the sum of its codepoints - the half of
// the problem no surveyed library solves. Summing utf8proc's per-codepoint
// widths over U+1F469 U+200D U+1F466 gives 4 where a clustering terminal renders
// 2, which is a cursor two columns from where the user can see it.
//
// The policy is a hook, not a configuration surface: #108 asks only that the
// answer not be hardcoded somewhere unreachable, because the right answer
// belongs to the terminal on the far side of the pty and changes with it. helix
// had to pin unicode-width *backwards* to stop rendering glitches; wezterm
// defaults to Unicode 9. Whatever lesh eventually negotiates - mode 2027, kitty
// text sizing, OSC 1337 UnicodeVersion - lands as a different width_policy and
// touches nothing here.
// ---------------------------------------------------------------------------

enum class width_class : std::uint8_t {
	zero,        // combining marks, controls, ZWJ: no cell of their own
	one,
	two,         // East_Asian_Width W or F, and emoji in emoji presentation
	ambiguous,   // UAX #11 A: "require additional information not contained in
	             // the character code". 138,739 codepoints; nobody can be sure.
};

struct width_policy {
	// UAX #11 Ambiguous. 1 matches a terminal in a Latin locale, which is the
	// conservative default; CJK locales widen it.
	std::uint8_t ambiguous = 1;
	// Extended_Pictographic with Emoji_Presentation=Yes. 2 is every terminal
	// since Unicode 9; 1 is what a Unicode 8-era terminal shows.
	std::uint8_t emoji_presentation = 2;
	// A ZWJ sequence is one image. True on a clustering terminal; false makes the
	// width the sum of the joined pictographs, which is what the others show.
	bool zwj_sequence_is_one_image = true;

	constexpr int resolve(width_class w) const {
		switch (w) {
			case width_class::zero: return 0;
			case width_class::two: return 2;
			case width_class::ambiguous: return ambiguous;
			case width_class::one: break;
		}
		return 1;
	}
};

inline constexpr width_policy default_width_policy{};

constexpr width_class codepoint_width_class(char32_t cp) {
	const properties p = lookup(cp);
	switch (p.cluster_break) {
		case gcb::control: case gcb::cr: case gcb::lf:
		case gcb::extend: case gcb::zwj:
			return width_class::zero;
		default:
			break;
	}
	switch (p.east_asian_width) {
		case eaw::w: case eaw::f: return width_class::two;
		case eaw::a: return width_class::ambiguous;
		default: return width_class::one;
	}
}

// Columns for one grapheme cluster. Given more than one cluster it measures the
// first, which keeps a caller that mis-segmented from silently getting a sum.
constexpr int cluster_width(std::string_view cluster,
                            const width_policy& policy = default_width_policy) {
	constexpr char32_t VS15 = 0xFE0E;   // text presentation
	constexpr char32_t VS16 = 0xFE0F;   // emoji presentation

	std::size_t i = 0;
	char32_t base = 0;
	bool have_base = false;
	bool has_zwj = false, has_vs15 = false, has_vs16 = false;
	int pictographs = 0, regional_indicators = 0;

	const std::size_t end = next_boundary(cluster, 0);
	while (i < end) {
		const decoded d = decode(cluster, i);
		if (!have_base) { base = d.cp; have_base = true; }
		const properties p = lookup(d.cp);
		if (p.cluster_break == gcb::zwj) has_zwj = true;
		if (p.cluster_break == gcb::regional_indicator) ++regional_indicators;
		if (p.extended_pictographic) ++pictographs;
		if (d.cp == VS15) has_vs15 = true;
		if (d.cp == VS16) has_vs16 = true;
		i += d.length;
	}
	if (!have_base)
		return 0;

	// A regional-indicator pair is one flag. A lone RI is a letter-shaped tile,
	// which terminals render narrow.
	if (regional_indicators >= 2)
		return 2;

	if (has_zwj && pictographs >= 2) {
		if (policy.zwj_sequence_is_one_image)
			return policy.emoji_presentation;
		int sum = 0;                                  // a terminal that does not cluster
		for (std::size_t j = 0; j < end;) {
			const decoded d = decode(cluster, j);
			if (lookup(d.cp).emoji_presentation)
				sum += policy.emoji_presentation;
			else
				sum += policy.resolve(codepoint_width_class(d.cp));
			j += d.length;
		}
		return sum;
	}

	// A variation selector is an explicit request, and it outranks the default
	// presentation of the base - including on bases that are not pictographic at
	// all, which is how U+2764 U+FE0F becomes a two-column heart.
	if (has_vs15)
		return 1;
	if (has_vs16)
		return policy.emoji_presentation;

	const properties p = lookup(base);
	if (p.extended_pictographic && p.emoji_presentation)
		return policy.emoji_presentation;

	return policy.resolve(codepoint_width_class(base));
}

// Columns for a run of text, cluster by cluster. Two trie lookups per codepoint,
// one pass, no allocation.
constexpr int string_width(std::string_view text,
                           const width_policy& policy = default_width_policy) {
	int total = 0;
	for (std::size_t i = 0; i < text.size();) {
		const std::size_t next = next_boundary(text, i);
		total += cluster_width(text.substr(i, next - i), policy);
		i = next;
	}
	return total;
}

} // namespace lesh::grapheme
