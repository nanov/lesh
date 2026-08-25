# Unicode Character Database (vendored subset)

Upstream: <https://www.unicode.org/Public/17.0.0/ucd/>
Version: **17.0.0**; the UCD ReadMe dates this drop 2025-08-15.
Licence: the terms the files themselves point at —
<https://www.unicode.org/terms_of_use.html>. Permissive and attribution-only;
redistribution of the data files is what it is written for.

Segregated under `third_party/` because these files are Unicode's, not lesh's.
Nothing here is modified: each file is the published one, byte for byte, and
`tools/gen_unicode_tables.py` enforces that with a pinned SHA-256 per file.

## What is vendored, and why each one

Five files, not the fifty the UCD ships. The generator reads four of them and
the test suite reads the fifth.

| file | read by | for |
|---|---|---|
| `GraphemeBreakProperty.txt` | generator | `Grapheme_Cluster_Break` — GB3–GB9b, GB12–GB13 |
| `DerivedCoreProperties.txt` | generator | `Indic_Conjunct_Break` — GB9c, and nothing else |
| `emoji-data.txt` | generator | `Extended_Pictographic` (GB11), `Emoji_Presentation` (width) |
| `EastAsianWidth.txt` | generator | UAX #11 width classes |
| `GraphemeBreakTest.txt` | generator **and** `tests/unit/grapheme_tests.cpp` | 766 conformance cases |

`DerivedCoreProperties.txt` is 1.1 MB for the 506 lines that declare `InCB`, and
it is vendored whole anyway. A trimmed copy would be a file Unicode never
published, and the reason it is here at all — Unicode 15.1 added GB9c through a
file no segmenter was reading — is precisely the reason not to start hand-editing
inputs to save a megabyte.

`GraphemeBreakTest.txt` is read at run time by the unit test rather than baked
into a generated fixture. A generated fixture would go stale in lockstep with the
table it is supposed to be checking.

`UnicodeData.txt` is deliberately *not* here. Nothing lesh computes needs general
category, decomposition or case mapping; adding it would be adopting utf8proc's
scope without utf8proc.

## Re-vendoring

```
python3 tools/gen_unicode_tables.py --fetch   # download, print checksums
# paste the printed block into INPUTS in the generator
python3 tools/gen_unicode_tables.py           # regenerate src/substrate/unicode_tables.h
ctest --preset debug -R Grapheme              # 766 cases, under ASan/UBSan
```

Bumping the Unicode version means editing `UNICODE_VERSION` first. The generator
refuses a mixed-version input set, an unrecognised property value, a checksum
that moved, and any conformance case its own rule model fails — see the module
docstring for why each of those is a way a table goes quietly wrong.

Regeneration is never a build step: no network at build time (ADR-0005), and the
table that ships is the table somebody reviewed.

## Status

A **gate**, not a scoreboard. `ctest --preset debug` fails if the committed
header is not what the generator emits from these files, and fails if any of the
766 cases breaks.
