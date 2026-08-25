#!/usr/bin/env python3
"""Generate src/substrate/unicode_tables.h from the vendored UCD files.

Regeneration is a deliberate commit, never a build step (#108, ADR-0005): no
network at build time, and the table that ships is the table that was reviewed.

    python3 tools/gen_unicode_tables.py            # regenerate from third_party/ucd
    python3 tools/gen_unicode_tables.py --check    # verify the committed header is current
    python3 tools/gen_unicode_tables.py --fetch    # re-download the pinned UCD files

Bumping Unicode is three edits and one command: change UNICODE_VERSION, run
--fetch, paste the checksums it prints into INPUTS, run the generator. Anything
the generator does not recognise stops it.

WHY THE PARANOIA. Unicode 15.1 changed zero bytes of GraphemeBreakProperty.txt,
EastAsianWidth.txt and emoji-data.txt, and still broke every segmenter in the
world: it added rule GB9c, whose Indic_Conjunct_Break property lives in
DerivedCoreProperties.txt - a file no width-or-break generator was reading. A
generator that only re-runs against the files it already knows would have
emitted a correct-looking, wrong table. So this one refuses to emit on any input
it cannot fully account for: an unknown property value, a version stamp that
disagrees with the pin, a checksum that moved, or - the check that would have
caught 15.1 - a single failure among GraphemeBreakTest.txt's cases, evaluated
here against a Python model of the same sixteen rules the C++ implements.

See docs/superpowers/research/2026-08-25-unicode-segmentation.md for why lesh
owns these tables rather than vendoring utf8proc or ICU.
"""

import argparse
import hashlib
import pathlib
import re
import sys
import urllib.request

# --------------------------------------------------------------------------
# The pin.
# --------------------------------------------------------------------------

UNICODE_VERSION = "17.0.0"

UCD_BASE = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd/"

# filename -> (path under UCD_BASE, sha256 of the vendored copy).
#
# The checksums are enforced, not decorative: a UCD file that changed under us -
# an editorial re-issue, a truncated download, a hand-edit - is a wrong table
# that still compiles. Unicode does re-issue files within a version.
INPUTS = {
	"GraphemeBreakProperty.txt": (
		"auxiliary/GraphemeBreakProperty.txt",
		"d6b51d1d2ae5c33b451b7ed994b48f1f4dc62b2272a5831e7fd418514a6bae89"),
	"GraphemeBreakTest.txt": (
		"auxiliary/GraphemeBreakTest.txt",
		"e2d134d2c52919bace503ebb6a551c1855fe1a1faec18478c78fff254a1793ec"),
	"EastAsianWidth.txt": (
		"EastAsianWidth.txt",
		"ea7ce50f3444a050333448dffef1cadd9325af55cbb764b4a2280faf52170a33"),
	"emoji-data.txt": (
		"emoji/emoji-data.txt",
		"2cb2bb9455cda83e8481541ecf5b6dfda66a3bb89efa3fa7c5297eccf607b72b"),
	"DerivedCoreProperties.txt": (
		"DerivedCoreProperties.txt",
		"24c7fed1195c482faaefd5c1e7eb821c5ee1fb6de07ecdbaa64b56a99da22c08"),
}

# Which UAX #29 rule reads which property, and which file that property comes
# from. This table is documentation with teeth: it is emitted into the generated
# header, so the rule set the tables were built for is recorded next to the
# tables, and a future GB9c cannot be added to the state machine without an
# entry here saying where its data comes from.
RULE_INPUTS = (
	("GB1-GB2",  "sot / eot",                 "-"),
	("GB3-GB5",  "Grapheme_Cluster_Break",    "GraphemeBreakProperty.txt"),
	("GB6-GB8",  "Grapheme_Cluster_Break",    "GraphemeBreakProperty.txt"),
	("GB9-GB9b", "Grapheme_Cluster_Break",    "GraphemeBreakProperty.txt"),
	("GB9c",     "Indic_Conjunct_Break",      "DerivedCoreProperties.txt"),
	("GB11",     "Extended_Pictographic",     "emoji-data.txt"),
	("GB12-GB13", "Grapheme_Cluster_Break",   "GraphemeBreakProperty.txt"),
	("GB999",    "-",                         "-"),
)

# Width is not a UAX #29 rule; it is lesh's own policy layer (UAX #11 plus the
# emoji presentation rules), and it reads two more properties.
WIDTH_INPUTS = (
	("East_Asian_Width",    "EastAsianWidth.txt"),
	("Emoji_Presentation",  "emoji-data.txt"),
)

# --------------------------------------------------------------------------
# The property vocabularies, frozen.
#
# An input file that offers a value not listed here is an input this generator
# does not understand. Unicode adding a Grapheme_Cluster_Break value, an
# Indic_Conjunct_Break value or an East_Asian_Width class means UAX #29 or #11
# grew, and the state machine in src/substrate/grapheme.h has to grow with it.
# Emitting a table that silently maps the new value onto Other would be the
# 15.1 failure again.
# --------------------------------------------------------------------------

GCB_VALUES = ("Other", "CR", "LF", "Control", "Extend", "ZWJ",
              "Regional_Indicator", "Prepend", "SpacingMark",
              "L", "V", "T", "LV", "LVT")
INCB_VALUES = ("None", "Consonant", "Extend", "Linker")
EAW_VALUES = ("N", "Na", "A", "H", "W", "F")

# We read two of these. The other four are listed so that a *new* emoji property
# stops the generator: emoji-data.txt is exactly the kind of file a new rule
# arrives through.
EMOJI_PROPERTIES = ("Emoji", "Emoji_Presentation", "Emoji_Modifier",
                    "Emoji_Modifier_Base", "Emoji_Component",
                    "Extended_Pictographic")

CODESPACE = 0x110000
BLOCK_SHIFT = 7                    # 128-codepoint blocks; see the size table in
BLOCK_SIZE = 1 << BLOCK_SHIFT      # the research note (33 KB, two dependent loads)

REPO = pathlib.Path(__file__).resolve().parent.parent
UCD_DIR = REPO / "third_party" / "ucd"
HEADER = REPO / "src" / "substrate" / "unicode_tables.h"


class Stale(Exception):
	"""An input this generator cannot account for. Never recoverable in-process."""


# --------------------------------------------------------------------------
# Reading the UCD
# --------------------------------------------------------------------------

def read_input(name, enforce_checksum=True):
	path = UCD_DIR / name
	if not path.exists():
		raise Stale(f"{path} is missing.\n"
		            f"  Fix: python3 tools/gen_unicode_tables.py --fetch")
	raw = path.read_bytes()
	digest = hashlib.sha256(raw).hexdigest()
	expected = INPUTS[name][1]
	if enforce_checksum and digest != expected:
		raise Stale(f"{name} is not the pinned Unicode {UNICODE_VERSION} file.\n"
		            f"  expected sha256 {expected}\n"
		            f"  found    sha256 {digest}\n"
		            f"  Fix: re-fetch with --fetch, or - if this is a deliberate\n"
		            f"       Unicode bump - update UNICODE_VERSION and INPUTS together.")
	return raw.decode("utf-8")


def check_version_stamp(name, text):
	"""Every UCD file names its own version. Refuse a mixed-version set."""
	head = text[:2048]
	# Data files stamp `# <Name>-17.0.0.txt`; emoji-data.txt stamps `# Version: 17.0`.
	short = ".".join(UNICODE_VERSION.split(".")[:2])
	if f"-{UNICODE_VERSION}.txt" in head or f"Version: {short}" in head:
		return
	raise Stale(f"{name} does not stamp Unicode {UNICODE_VERSION}.\n"
	            f"  A mixed-version input set produces a table that is right about\n"
	            f"  some codepoints and wrong about others, with nothing to show for it.")


RANGE_RE = re.compile(r"^([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*(.*)$")


def parse_ranges(text, want_field=0):
	"""Yield (first, last, value). `want_field` picks the property field for the
	multi-field DerivedCoreProperties form `cp ; InCB ; Linker`."""
	for line in text.splitlines():
		line = line.split("#", 1)[0].strip()
		if not line:
			continue
		match = RANGE_RE.match(line)
		if not match:
			raise Stale(f"unparsable UCD line: {line!r}")
		first = int(match.group(1), 16)
		last = int(match.group(2), 16) if match.group(2) else first
		fields = [f.strip() for f in match.group(3).split(";")]
		if want_field >= len(fields):
			continue
		yield first, last, fields, fields[want_field]


def load_gcb(text):
	out = bytearray(CODESPACE)          # 0 == Other, the @missing default
	index = {name: i for i, name in enumerate(GCB_VALUES)}
	seen = set()
	for first, last, _fields, value in parse_ranges(text):
		if value not in index:
			raise Stale(f"unknown Grapheme_Cluster_Break value {value!r}.\n"
			            f"  UAX #29 has grown a property value. The rules in\n"
			            f"  src/substrate/grapheme.h must grow with it - see GB9c.")
		seen.add(value)
		for cp in range(first, last + 1):
			out[cp] = index[value]
	unused = set(GCB_VALUES) - seen - {"Other"}
	if unused:
		raise Stale(f"Grapheme_Cluster_Break values {sorted(unused)} vanished from the "
		            f"file. A removed value is as much a rule change as a new one.")
	return out


def load_incb(text):
	out = bytearray(CODESPACE)
	index = {name: i for i, name in enumerate(INCB_VALUES)}
	found = False
	for _first, _last, fields, prop in parse_ranges(text):
		if prop != "InCB":
			continue
		found = True
		if len(fields) < 2:
			raise Stale(f"InCB line without a value: {fields!r}")
		value = fields[1]
		if value not in index:
			raise Stale(f"unknown Indic_Conjunct_Break value {value!r}. GB9c changed.")
		for cp in range(_first, _last + 1):
			out[cp] = index[value]
	if not found:
		raise Stale("DerivedCoreProperties.txt declares no Indic_Conjunct_Break.\n"
		            "  GB9c cannot be implemented without it, and a table built\n"
		            "  without it looks correct on every test that predates 15.1.")
	return out


# EastAsianWidth.txt's @missing line says N for the whole codespace, but its
# prose header carves out four blocks where *unassigned* codepoints default to
# W. That default is not machine-readable anywhere in the UCD, so we parse the
# prose - and fail if the prose stops saying it, rather than quietly narrowing
# 130,000 codepoints.
EAW_W_DEFAULT_RE = re.compile(r"U\+([0-9A-F]{4,6})\.\.U\+([0-9A-F]{4,6})")


def load_eaw(text):
	index = {name: i for i, name in enumerate(EAW_VALUES)}
	default_w = []
	collecting = False
	for line in text.splitlines():
		if not line.startswith("#"):
			break
		found = EAW_W_DEFAULT_RE.findall(line)
		if collecting and found:
			default_w += [(int(a, 16), int(b, 16)) for a, b in found]
		elif not found:
			collecting = False
		if 'default to "W"' in line:
			collecting = True
	if not default_w:
		raise Stale("EastAsianWidth.txt no longer documents the blocks whose\n"
		            "  unassigned codepoints default to W. Either the default moved\n"
		            "  into machine-readable form - use it - or it was withdrawn.")

	out = bytearray([index["N"]]) * CODESPACE
	for first, last in default_w:
		for cp in range(first, last + 1):
			out[cp] = index["W"]
	seen = set()
	for first, last, _fields, value in parse_ranges(text):
		if value not in index:
			raise Stale(f"unknown East_Asian_Width value {value!r}. UAX #11 changed.")
		seen.add(value)
		for cp in range(first, last + 1):
			out[cp] = index[value]
	missing = set(EAW_VALUES) - seen
	if missing:
		raise Stale(f"East_Asian_Width values {sorted(missing)} vanished from the file.")
	return out, default_w


def load_emoji(text):
	ext_pict = bytearray(CODESPACE)
	presentation = bytearray(CODESPACE)
	seen = set()
	for first, last, _fields, value in parse_ranges(text):
		seen.add(value)
		if value == "Extended_Pictographic":
			for cp in range(first, last + 1):
				ext_pict[cp] = 1
		elif value == "Emoji_Presentation":
			for cp in range(first, last + 1):
				presentation[cp] = 1
	unknown = seen - set(EMOJI_PROPERTIES)
	if unknown:
		raise Stale(f"emoji-data.txt declares unknown properties {sorted(unknown)}.\n"
		            f"  A new emoji property is how GB11-shaped rules arrive. Decide\n"
		            f"  whether segmentation or width reads it before regenerating.")
	if not seen >= {"Extended_Pictographic", "Emoji_Presentation"}:
		raise Stale("emoji-data.txt is missing a property GB11 or width depends on.")
	return ext_pict, presentation


# --------------------------------------------------------------------------
# Packing
# --------------------------------------------------------------------------

# Record layout, mirrored by the accessors in the generated header.
GCB_SHIFT, GCB_MASK = 0, 0xF
INCB_SHIFT, INCB_MASK = 4, 0x3
EAW_SHIFT, EAW_MASK = 6, 0x7
EXT_PICT_BIT = 1 << 9
EMOJI_PRESENTATION_BIT = 1 << 10


def build_records(gcb, incb, eaw, ext_pict, presentation):
	records = []
	index = {}
	table = bytearray(CODESPACE)
	for cp in range(CODESPACE):
		value = ((gcb[cp] << GCB_SHIFT) | (incb[cp] << INCB_SHIFT)
		         | (eaw[cp] << EAW_SHIFT)
		         | (EXT_PICT_BIT if ext_pict[cp] else 0)
		         | (EMOJI_PRESENTATION_BIT if presentation[cp] else 0))
		slot = index.get(value)
		if slot is None:
			slot = index[value] = len(records)
			records.append(value)
		table[cp] = slot
	if len(records) > 256:
		raise Stale(f"{len(records)} distinct property tuples: the per-codepoint "
		            f"record no longer fits a byte. Widen stage 2.")
	return records, table


def build_trie(table):
	"""Two-stage trie, 128-codepoint blocks. Two dependent loads, no search."""
	blocks = []
	index = {}
	stage1 = []
	for base in range(0, CODESPACE, BLOCK_SIZE):
		block = bytes(table[base:base + BLOCK_SIZE])
		slot = index.get(block)
		if slot is None:
			slot = index[block] = len(blocks)
			blocks.append(block)
		stage1.append(slot)
	stage2 = b"".join(blocks)
	return stage1, stage2


# --------------------------------------------------------------------------
# The generate-time conformance gate.
#
# A Python model of GB1-GB13 + GB999 run over GraphemeBreakTest.txt. This is the
# check that would have caught Unicode 15.1: when GB9c landed, the conformance
# file grew cases the old rule set fails, so the generator stops instead of
# emitting a table whose data is fine and whose rules are a version behind.
#
# It is deliberately a second implementation. The C++ one is gated by the same
# 766 cases in tests/unit/grapheme_tests.cpp; agreement between two independent
# transcriptions of the rules is worth more than either alone.
# --------------------------------------------------------------------------

def gb_breaks(codepoints, gcb, incb, ext_pict):
	G = {name: i for i, name in enumerate(GCB_VALUES)}
	I = {name: i for i, name in enumerate(INCB_VALUES)}
	breaks = [True]                     # GB1: sot / Any
	prev = None
	ri_odd = False
	incb_state = 0                      # 0 none, 1 consonant, 2 linker seen
	ep_state = 0                        # 0 none, 1 pictographic, 2 pictographic ZWJ
	for cp in codepoints:
		this = gcb[cp]
		if prev is not None:
			p = gcb[prev]
			if p == G["CR"] and this == G["LF"]:
				brk = False                                              # GB3
			elif p in (G["Control"], G["CR"], G["LF"]):
				brk = True                                               # GB4
			elif this in (G["Control"], G["CR"], G["LF"]):
				brk = True                                               # GB5
			elif p == G["L"] and this in (G["L"], G["V"], G["LV"], G["LVT"]):
				brk = False                                              # GB6
			elif p in (G["LV"], G["V"]) and this in (G["V"], G["T"]):
				brk = False                                              # GB7
			elif p in (G["LVT"], G["T"]) and this == G["T"]:
				brk = False                                              # GB8
			elif this in (G["Extend"], G["ZWJ"]):
				brk = False                                              # GB9
			elif this == G["SpacingMark"]:
				brk = False                                              # GB9a
			elif p == G["Prepend"]:
				brk = False                                              # GB9b
			elif incb_state == 2 and incb[cp] == I["Consonant"]:
				brk = False                                              # GB9c
			elif ep_state == 2 and ext_pict[cp]:
				brk = False                                              # GB11
			elif this == G["Regional_Indicator"] and ri_odd:
				brk = False                                              # GB12/GB13
			else:
				brk = True                                               # GB999
			breaks.append(brk)
		else:
			brk = True

		ri_odd = (not ri_odd) if (this == G["Regional_Indicator"] and not brk) \
			else (this == G["Regional_Indicator"])
		if incb[cp] == I["Consonant"]:
			incb_state = 1
		elif incb[cp] == I["Linker"]:
			incb_state = 2 if incb_state in (1, 2) else 0
		elif incb[cp] == I["Extend"]:
			pass
		else:
			incb_state = 0
		if ext_pict[cp]:
			ep_state = 1
		elif ep_state == 1 and this == G["Extend"]:
			ep_state = 1
		elif ep_state == 1 and this == G["ZWJ"]:
			ep_state = 2
		else:
			ep_state = 0
		prev = cp
	breaks.append(True)                 # GB2: Any / eot
	return breaks


def run_conformance(text, gcb, incb, ext_pict):
	total = failures = 0
	for line in text.splitlines():
		line = line.split("#", 1)[0].strip()
		if not line:
			continue
		total += 1
		tokens = line.split()
		expected, codepoints = [], []
		for token in tokens:
			if token == "÷":
				expected.append(True)
			elif token == "×":
				expected.append(False)
			else:
				codepoints.append(int(token, 16))
		if gb_breaks(codepoints, gcb, incb, ext_pict) != expected:
			failures += 1
			if failures <= 3:
				print(f"  conformance failure: {line}", file=sys.stderr)
	return total, failures


# --------------------------------------------------------------------------
# Emitting
# --------------------------------------------------------------------------

def rows(values, per_line, fmt):
	out = []
	for i in range(0, len(values), per_line):
		out.append("\t" + " ".join(fmt(v) for v in values[i:i + per_line]))
	return "\n".join(out)


def emit(records, stage1, stage2, default_w, cases):
	stage1_type = "std::uint8_t" if max(stage1) < 256 else "std::uint16_t"
	stage1_bytes = len(stage1) * (1 if stage1_type.endswith("8_t") else 2)
	total = stage1_bytes + len(stage2) + len(records) * 2

	rule_lines = "\n".join(
		f"//   {rule:<12} {prop:<24} {src}" for rule, prop, src in RULE_INPUTS)
	width_lines = "\n".join(
		f"//   {'width':<12} {prop:<24} {src}" for prop, src in WIDTH_INPUTS)
	w_blocks = ", ".join(f"U+{a:04X}..U+{b:04X}" for a, b in default_w)

	enum_lines = lambda names: "\n".join(
		f"\t{name.lower()} = {i}," for i, name in enumerate(names))

	return f"""// GENERATED by tools/gen_unicode_tables.py from Unicode {UNICODE_VERSION}. Do not edit.
//
// Regenerate:  python3 tools/gen_unicode_tables.py
// Verify:      python3 tools/gen_unicode_tables.py --check
//
// lesh owns these tables rather than linking utf8proc or ICU; the measurements
// behind that decision are in
// docs/superpowers/research/2026-08-25-unicode-segmentation.md. The short of it:
// utf8proc costs 341 KB for eleven bits of a 192-bit record and still answers
// codepoint width where a terminal needs cluster width; ICU is 33 MB.
//
// Shape: a two-stage trie over {BLOCK_SIZE}-codepoint blocks, indexing a table of the
// {len(records)} distinct property tuples that exist across all {CODESPACE} codepoints.
// Two dependent loads per codepoint, no search, no initialiser, nothing touched
// until the first non-ASCII byte arrives.
//
//   stage 1   {len(stage1):>6} x {1 if stage1_type.endswith('8_t') else 2} B = {stage1_bytes:>6} B
//   stage 2   {len(stage2):>6} x 1 B = {len(stage2):>6} B
//   records   {len(records):>6} x 2 B = {len(records) * 2:>6} B
//   total                    {total:>6} B
//
// The rules these tables feed, and where each rule's data comes from. A new UAX
// #29 rule needs a line here before it needs code: Unicode 15.1 added GB9c
// through DerivedCoreProperties.txt without touching any file the generators of
// the day were reading.
//
{rule_lines}
{width_lines}
//
// East_Asian_Width defaults to N except in these blocks, where unassigned
// codepoints default to W ({UCD_BASE}EastAsianWidth.txt, prose header):
//   {w_blocks}
//
// Validated at generation against all {cases} cases of
// auxiliary/GraphemeBreakTest.txt; validated again at test time against the
// same file by tests/unit/grapheme_tests.cpp.

#pragma once

#include <cstddef>
#include <cstdint>

namespace lesh::grapheme {{

inline constexpr const char* UNICODE_VERSION = "{UNICODE_VERSION}";

// UAX #29 Grapheme_Cluster_Break.
enum class gcb : std::uint8_t {{
{enum_lines(GCB_VALUES)}
}};

// UAX #44 Indic_Conjunct_Break - GB9c's property, new in Unicode 15.1.
enum class incb : std::uint8_t {{
{enum_lines(INCB_VALUES)}
}};

// UAX #11 East_Asian_Width. `a` is Ambiguous, which the standard explicitly
// declines to resolve: "Ambiguous characters require additional information not
// contained in the character code". That resolution is width_policy's job.
enum class eaw : std::uint8_t {{
{enum_lines(EAW_VALUES)}
}};

namespace tables {{

// Bit layout of one record. Ten bits used; the spare six are where the next
// property lands.
inline constexpr unsigned GCB_SHIFT = {GCB_SHIFT};
inline constexpr unsigned GCB_MASK = 0x{GCB_MASK:X};
inline constexpr unsigned INCB_SHIFT = {INCB_SHIFT};
inline constexpr unsigned INCB_MASK = 0x{INCB_MASK:X};
inline constexpr unsigned EAW_SHIFT = {EAW_SHIFT};
inline constexpr unsigned EAW_MASK = 0x{EAW_MASK:X};
inline constexpr std::uint16_t EXT_PICT_BIT = 0x{EXT_PICT_BIT:X};
inline constexpr std::uint16_t EMOJI_PRESENTATION_BIT = 0x{EMOJI_PRESENTATION_BIT:X};

inline constexpr unsigned BLOCK_SHIFT = {BLOCK_SHIFT};
inline constexpr std::size_t CODESPACE = 0x{CODESPACE:X};

inline constexpr std::uint16_t RECORDS[{len(records)}] = {{
{rows(records, 12, lambda v: f"0x{v:04X},")}
}};

inline constexpr {stage1_type} STAGE1[{len(stage1)}] = {{
{rows(stage1, 24, lambda v: f"{v},")}
}};

inline constexpr std::uint8_t STAGE2[{len(stage2)}] = {{
{rows(list(stage2), 24, lambda v: f"{v},")}
}};

// The one lookup: two dependent loads and a deref of a 62-byte record table.
//
// Out of range is not a caller error to be asserted away. The UTF-8 decoder
// already folds malformed input to U+FFFD, so nothing above the codespace should
// arrive here; folding again rather than reading off the end means a caller that
// hands us a raw wchar_t degrades instead of corrupting memory.
constexpr std::uint16_t record(char32_t cp) {{
	if (cp >= CODESPACE)
		cp = 0xFFFD;
	return RECORDS[STAGE2[(std::size_t(STAGE1[cp >> BLOCK_SHIFT]) << BLOCK_SHIFT)
	                      | (cp & ((1u << BLOCK_SHIFT) - 1))]];
}}

}} // namespace tables

}} // namespace lesh::grapheme
"""


# --------------------------------------------------------------------------

def fetch():
	UCD_DIR.mkdir(parents=True, exist_ok=True)
	print(f"Unicode {UNICODE_VERSION} from {UCD_BASE}")
	for name, (path, _) in INPUTS.items():
		url = UCD_BASE + path
		with urllib.request.urlopen(url) as response:
			raw = response.read()
		(UCD_DIR / name).write_bytes(raw)
		print(f'\t"{name}": (\n\t\t"{path}",\n\t\t"{hashlib.sha256(raw).hexdigest()}"),')
	print("\nPaste the block above into INPUTS, then re-run the generator.")


def generate(check_only, trust_checksums):
	texts = {}
	for name in INPUTS:
		texts[name] = read_input(name, enforce_checksum=trust_checksums)
		check_version_stamp(name, texts[name])

	gcb = load_gcb(texts["GraphemeBreakProperty.txt"])
	incb = load_incb(texts["DerivedCoreProperties.txt"])
	eaw, default_w = load_eaw(texts["EastAsianWidth.txt"])
	ext_pict, presentation = load_emoji(texts["emoji-data.txt"])

	cases, failures = run_conformance(texts["GraphemeBreakTest.txt"], gcb, incb, ext_pict)
	if failures:
		raise Stale(f"{failures} of {cases} GraphemeBreakTest.txt cases fail against\n"
		            f"  the rule model in this generator. The data files and the rules\n"
		            f"  have diverged - which is exactly what Unicode 15.1 looked like.\n"
		            f"  Update gb_breaks() and src/substrate/grapheme.h together, and\n"
		            f"  add the new rule to RULE_INPUTS.")

	records, table = build_records(gcb, incb, eaw, ext_pict, presentation)
	stage1, stage2 = build_trie(table)
	text = emit(records, stage1, stage2, default_w, cases)

	if check_only:
		if not HEADER.exists() or HEADER.read_text() != text:
			raise Stale(f"{HEADER.relative_to(REPO)} is not what the generator emits.\n"
			            f"  Fix: python3 tools/gen_unicode_tables.py")
		print(f"{HEADER.relative_to(REPO)} is current "
		      f"(Unicode {UNICODE_VERSION}, {cases} conformance cases pass).")
		return

	HEADER.write_text(text)
	size = len(stage1) * (1 if max(stage1) < 256 else 2) + len(stage2) + len(records) * 2
	print(f"{HEADER.relative_to(REPO)}: Unicode {UNICODE_VERSION}, "
	      f"{len(records)} distinct property tuples, "
	      f"{len(stage1)} + {len(stage2)} trie entries, {size} B of tables, "
	      f"{cases}/{cases} conformance cases pass.")


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--fetch", action="store_true",
	                    help="re-download the pinned UCD files and print their checksums")
	parser.add_argument("--check", action="store_true",
	                    help="fail if the committed header is not what the generator emits")
	parser.add_argument("--bless-checksums", action="store_true",
	                    help="skip the checksum pin (only when adopting new INPUTS)")
	args = parser.parse_args()

	try:
		if args.fetch:
			fetch()
		else:
			generate(args.check, trust_checksums=not args.bless_checksums)
	except Stale as error:
		print(f"gen_unicode_tables: {error}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
