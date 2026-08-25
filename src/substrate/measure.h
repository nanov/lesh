#pragma once

// The one measurer (#114): display width of opaque bytes that may contain
// terminal escape sequences, closing #94's Prompt resolution - "leshper
// measures width itself... so theme authors never do width arithmetic and
// `%{ %}` folklore never exists". A `Prompt` provider (#94, A-5) hands back
// raw bytes for one of F-40's surfaces; this is the pure function that turns
// those bytes into a column count, so no caller - and no theme author - ever
// counts columns by hand.
//
// WHAT IS RECOGNIZED AND SKIPPED (zero width, consumed whole):
//
//   CSI   ESC '[' parameter-bytes(0x30-0x3F)* intermediate-bytes(0x20-0x2F)*
//         final-byte(0x40-0x7E)         - ECMA-48/ANSI. SGR (`CSI ... 'm'`)
//         is the one the blitter itself emits: #97 fixed the floor at ANSI
//         + 256 colors + truecolor-opportunistic, and all three depths are
//         SGR, so one CSI recognizer covers every color a theme can request.
//   OSC   ESC ']' ... string, terminated by BEL (0x07) or ST (`ESC '\'`).
//   SS3   ESC 'O' one byte.
//
// Nothing else is recognized, on purpose: no C1 8-bit introducers (0x9B for
// CSI, 0x9D for OSC - the blitter never emits 8-bit controls, #97's floor is
// 7-bit), no DCS, no APC, no SS2. "At minimum CSI/OSC/SS3" (#114) is read as
// a floor, not an invitation to guess at forms nothing in this codebase emits
// yet; teaching this function a new one is a decision for whoever teaches the
// blitter that sequence, made at that time, not defaulted here.
//
// MALFORMED INPUT (N-4: malformed bytes degrade, they do not abort or stall).
// An ESC that does not introduce one of the three forms above, or one that
// does but runs off the end of `bytes` before its terminator, is simply not
// treated as an escape: scanning continues past the ESC byte as ordinary
// text. Nothing is hidden that could not be confidently recognized as one of
// the three forms, and nothing loops or aborts - the grapheme decoder
// underneath already guarantees forward progress on arbitrary bytes, and the
// stray ESC itself still measures as zero width, because U+001B carries
// Grapheme_Cluster_Break=Control. Malformed UTF-8 inside a visible span
// degrades exactly as grapheme.h already documents: one U+FFFD per bad byte.
//
// WHY THIS IS SPAN-BASED, NOT A REWRITE-THEN-MEASURE. A recognized escape
// sequence is always a hard grapheme-cluster boundary: the bytes before it
// and the bytes after it are measured as two separate runs through #108's
// segmenter, never re-joined into one. A theme author who wraps a combining
// mark's base character in its own SGR reset gets that mark measured against
// whatever precedes it in ITS OWN span (usually nothing, so it is dropped as
// a leading zero-width mark - which is what a terminal that does not
// special-case SGR mid-cluster shows too) rather than silently reattached to
// a base several spans back. No caller depends on the other answer, and
// spans let this stay pure and allocation-free: each one is measured in
// place via `grapheme::string_width` over a `substr` view, nothing copied.
//
// NOT IN SCOPE (#114): prompt expansion (lesh-side, undecided), caching or
// pre-compilation (lesh-side, later), any layout decision. This function
// answers one question - how many columns - and nothing else.

#include "substrate/grapheme.h"

#include <cstddef>
#include <string_view>

namespace lesh::grapheme {

namespace measure_detail {

constexpr bool is_csi_param(unsigned char b) { return b >= 0x30 && b <= 0x3F; }
constexpr bool is_csi_intermediate(unsigned char b) { return b >= 0x20 && b <= 0x2F; }
constexpr bool is_csi_final(unsigned char b) { return b >= 0x40 && b <= 0x7E; }

// Length in bytes of the escape sequence starting at `bytes[pos]`, which must
// be ESC, or 0 if it is not the start of a recognized CSI/OSC/SS3 form - a
// lone ESC, an unrecognized introducer, or a truncated candidate all return
// 0. The caller then treats `bytes[pos]` as an ordinary byte and resumes
// scanning at pos + 1, per N-4.
constexpr std::size_t escape_length(std::string_view bytes, std::size_t pos) {
	const std::size_t n = bytes.size();
	if (pos + 1 >= n)
		return 0;                                            // lone ESC at EOF

	const unsigned char intro = static_cast<unsigned char>(bytes[pos + 1]);

	if (intro == 'O') {                                       // SS3: ESC 'O' x
		if (pos + 2 >= n)
			return 0;                                         // truncated
		return 3;
	}

	if (intro == '[') {                                       // CSI
		std::size_t i = pos + 2;
		while (i < n && is_csi_param(static_cast<unsigned char>(bytes[i])))
			++i;
		while (i < n && is_csi_intermediate(static_cast<unsigned char>(bytes[i])))
			++i;
		if (i >= n || !is_csi_final(static_cast<unsigned char>(bytes[i])))
			return 0;                                         // unterminated
		return i - pos + 1;
	}

	if (intro == ']') {                                       // OSC
		std::size_t i = pos + 2;
		while (i < n) {
			const unsigned char b = static_cast<unsigned char>(bytes[i]);
			if (b == 0x07)
				return i - pos + 1;                           // BEL terminator
			if (b == 0x1B && i + 1 < n && static_cast<unsigned char>(bytes[i + 1]) == '\\')
				return i - pos + 2;                           // ST terminator
			++i;
		}
		return 0;                                             // unterminated
	}

	return 0;                                                 // unrecognized introducer
}

} // namespace measure_detail

// Display width of `bytes`: the sum of `string_width` over the spans between
// recognized escape sequences (CSI/OSC/SS3, see above), which contribute
// nothing themselves. Pure, allocation-free, thread-safe on an immutable
// `bytes` view (C-4): reads only its arguments and the constexpr tables
// `string_width` already reads, so it is as callable from a worker as from
// the loop thread.
constexpr int display_width(std::string_view bytes,
                            const width_policy& policy = default_width_policy) {
	int total = 0;
	std::size_t span_start = 0;
	std::size_t i = 0;

	while (i < bytes.size()) {
		if (static_cast<unsigned char>(bytes[i]) != 0x1B) {
			++i;
			continue;
		}
		const std::size_t len = measure_detail::escape_length(bytes, i);
		if (len == 0) {
			++i;                                              // not a recognized escape
			continue;
		}
		total += string_width(bytes.substr(span_start, i - span_start), policy);
		i += len;
		span_start = i;
	}
	total += string_width(bytes.substr(span_start, bytes.size() - span_start), policy);
	return total;
}

} // namespace lesh::grapheme
