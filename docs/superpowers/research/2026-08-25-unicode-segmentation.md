# Unicode segmentation: hand-rolled or vendored

**Date:** 2026-08-25
**Ticket:** [#88](https://github.com/nanov/lesh/issues/88) (parent [#82](https://github.com/nanov/lesh/issues/82))
**Status:** Research complete, recommendation pending human review
**Decides:** spec §8 open decision, "Unicode segmentation: the first real test of the dependency rule"
**Constrained by:** [ADR-0005](../../adr/0005-no-runtime-shared-library-dependencies.md)

All measurements in this document were taken on 2026-08-25, Apple Silicon, clang `-O2`,
against Unicode 17.0.0 data files. They are reproducible; the commands are given inline.

---

## Bottom line

Generate the tables ourselves. The job lesh actually needs is UAX #29 grapheme cluster
boundaries plus a *tailored* column-width policy, and that is **25 distinct property values
over the whole codespace, packed into a 33 KB two-stage trie with a 16-rule state machine
over it, validated by the 766 test cases the standard already ships**. utf8proc does the
job well but does a much larger job: it costs a **measured 341 KB** of binary for a 24-byte
property record of which lesh needs 11 bits, and it still does not answer the question that
matters — it gives codepoint width, not *cluster* width (measured: it sums a ZWJ family
emoji to 4 columns where a clustering terminal renders 2). ICU is not a candidate at any
price: a hello-world that calls nothing but `BreakIterator::createCharacterInstance`,
statically linked, is a **measured 33.06 MB** and pays 0.553 ms on first construction.
The decisive evidence is convergent practice — kitty generates its own tables, wezterm
vendors a generated table, fish vendors a generated table, and **none of the four programs
surveyed uses ICU**; the one that took the library route (helix) had to pin its width crate
*backwards* because upstream's correctness updates "disagree with common width definitions
in terminals". That last point is the real argument: because of version skew, the width
table has to be a runtime *policy* lesh can bend toward whatever the terminal believes, and
owning the generator is what makes that possible. ADR-0005's test — "a library that does a
job better than a hand-rolled equivalent is a reason to take the dependency" — is not met
here, because utf8proc does not do *this* job better; it does a bigger job at 10x the size
and leaves the hard half undone.

---

## 1. What is actually needed

### Two problems, separable data, non-composable answers

The editor needs two distinct things, and F-3 conflates them at its peril:

1. **Segmentation** — where may the cursor rest, what does one Backspace delete, what does
   one `forward-char` traverse. This is UAX #29 extended grapheme cluster boundaries.
2. **Column arithmetic** — how many terminal cells does that cluster occupy, so the prompt
   can be redrawn and the cursor placed.

Their *input data* is separable. Segmentation needs `GraphemeBreakProperty.txt`,
`emoji-data.txt` (for `Extended_Pictographic`) and `DerivedCoreProperties.txt` (for `InCB`).
Width needs `EastAsianWidth.txt`. No file is shared.

Their *answers* are not composable, and this is the finding that shapes everything below.
**Cluster width is not the sum of codepoint widths.** Measured against utf8proc 2.11.3:

| input | `utf8proc_charwidth` sum | what a clustering terminal renders |
|---|---|---|
| U+1F469 U+200D U+1F466 (woman-ZWJ-boy family) | **4** | 2 |
| U+1F469 U+1F3FB (woman + skin tone) | **4** | 2 |
| U+1F1E9 U+1F1EA (regional indicator pair, DE flag) | 2 | 2 |
| U+0041 U+0301 (A + combining acute) | 1 | 1 |

```
cc -O2 -o w w.c utf8proc/utf8proc.c -I. && ./w
```

No library surveyed exposes a cluster-width function. utf8proc's public API is
`utf8proc_charwidth(utf8proc_int32_t codepoint)` and `utf8proc_charwidth_ambiguous`
([utf8proc.h:713,721](https://github.com/JuliaStrings/utf8proc/blob/master/utf8proc.h)) —
per codepoint, both. **Whatever candidate is chosen, lesh writes the cluster-width policy
layer itself.** That halves the value of any vendored option before the comparison starts.

### UAX #11 disclaims the use we would be putting it to

This is worth quoting exactly, because it is the specification telling implementers not to
do the obvious thing ([UAX #11, revision 42, Unicode 17.0.0](https://www.unicode.org/reports/tr11/)):

> Note: The East_Asian_Width property is not intended for use by modern terminal emulators
> without appropriate tailoring on a case-by-case basis. Such terminal emulators need a way
> to resolve the halfwidth/fullwidth dichotomy that is necessary for such environments, but
> the East_Asian_Width property does not provide an off-the-shelf solution for all situations.

And on the Ambiguous class specifically:

> ED6. East Asian Ambiguous (A): All characters that can be sometimes wide and sometimes
> narrow. Ambiguous characters require additional information not contained in the character
> code to further resolve their width.

So width is *definitionally* a policy question with a local answer, not a lookup. There are
138,739 Ambiguous codepoints in Unicode 17 (measured from `EastAsianWidth.txt`). A library
that returns a number for them has made a choice on lesh's behalf; utf8proc at least admits
it, which is why `utf8proc_charwidth_ambiguous` exists as a separate predicate.

### Word boundaries: F-3 does not mean UAX #29

There are **three** unrelated notions of "word" in play, and only one of them is a Unicode
question. Conflating them would be the expensive mistake here:

1. **Shell token boundaries** — operator-delimited runs, `token_kind::word`. lesh's lexer
   already owns this entirely (`src/syntax/token.h:14-60`), including the segment model
   (`seg_literal`, `seg_double_quoted`, …) that keeps quoting rules in one place. Nothing
   Unicode-shaped is missing here.
2. **Editor word motion** (`Alt-B` / `Alt-F` / `backward-kill-word`) — this is what F-3 means
   by "word boundaries", and it is *configuration*, not Unicode. zsh defines it by the
   `WORDCHARS` parameter, documented as "A list of non-alphanumeric characters considered
   part of a word by the line editor"
   ([zsh Parameters](https://zsh.sourceforge.io/Doc/Release/Parameters.html)). GNU Readline
   is blunter still: for `forward-word` and `backward-word`, "Words are composed of letters
   and digits" ([readline manual](https://tiswww.case.edu/php/chet/readline/readline.html)),
   with no mention of Unicode, grapheme clusters, or multibyte handling anywhere in word
   motion.
3. **UAX #29 word boundaries** (WB1–WB16 + WB999, over the `Word_Break` property with
   `Katakana`, `Hebrew_Letter`, `MidNumLet`, `WSegSpace` and the rest) — these segment
   *natural-language* text for search-and-highlight and double-click selection. They would
   make `foo.bar` and `x=1` break in ways a shell user would find surprising, and they are
   the wrong model for a command line.

**Recommendation: do not implement UAX #29 word boundaries.** Editor word motion gets a
`WORDCHARS`-style configurable predicate, matching zsh — which is also what §2.2's curated
zsh layer would want anyway. This drops `WordBreakProperty.txt` (114,445 bytes, 1,541 lines)
and 17 rules from scope entirely.

---

## 2. The candidates

### utf8proc

- **License:** MIT "expat", Unicode data under the Unicode license
  ([LICENSE.md](https://github.com/JuliaStrings/utf8proc/blob/master/LICENSE.md)). Clean.
- **Language:** C. Vendors and statically links without friction — exactly what ADR-0005
  contemplates.
- **Version:** 2.11.3, Unicode **17.0.0**. Historically tracks Unicode within months.
- **Source size (measured):**

  | file | lines | bytes |
  |---|---:|---:|
  | `utf8proc.c` | 842 | 33,209 |
  | `utf8proc.h` | 815 | 34,910 |
  | `utf8proc_data.c` | 17,141 | **2,360,214** |

- **API for our purposes:** `utf8proc_grapheme_break_stateful(c1, c2, &state)` — a genuine
  UAX #29 implementation carrying `InCB` state for GB9c; `utf8proc_charwidth(cp)`;
  `utf8proc_charwidth_ambiguous(cp)`. No cluster-width call.
- **Table breakdown (measured from `utf8proc_data.c`, `sizeof(utf8proc_property_t) == 24`):**

  | table | entries | bytes | does lesh need it? |
  |---|---:|---:|---|
  | `utf8proc_properties` | 8,385 × 24 B | 201,240 | 11 bits of 192 |
  | `utf8proc_stage2table` | 46,336 × 2 B | 92,672 | yes (trie index) |
  | `utf8proc_sequences` | 12,961 × 2 B | 25,922 | no (decomposition) |
  | `utf8proc_stage1table` | 4,352 × 2 B | 8,704 | yes (trie index) |
  | `utf8proc_combinations_*` | 961 × 4 B × 2 | 7,688 | no (NFC composition) |
  | **total `__const`** | | **336,606** | |

The 24-byte property record carries general category, uppercase/lowercase/titlecase mappings,
decomposition type and index, combining class, bidi class and mirroring, composition
exclusion — and, in eleven bits, the `charwidth`, `ambiguous_width`, `boundclass` and
`indic_conjunct_break` fields lesh would actually read. **It cannot be stripped**: it is one
array reached through one trie by every entry point, so `--gc-sections` has nothing to cut.

### ICU — "almost certainly too large", quantified

Measured against ICU4C 78 (Homebrew `icu4c@78`):

| artifact | size |
|---|---:|
| `libicudata.a` | 31.6 MB |
| `libicui18n.a` | 5.3 MB |
| `libicuuc.a` | 2.7 MB |
| **hello-world calling only `createCharacterInstance`, statically linked** | **33.06 MB** |

```
c++ -O2 -std=c++17 -o icu_static icu_t.cpp -I$I/include \
    $I/lib/libicui18n.a $I/lib/libicuuc.a $I/lib/libicudata.a
```

That is **97x utf8proc** and **~1000x the hand-rolled table** measured in §3, for a program
whose entire use of ICU is one character break iterator. ICU's own documentation confirms
this is by design: "as of ICU 64, the standard data library is over 20 MB in size", reducible
to roughly 3.6–4.7 MB only by stripping all conversion tables via the ICU Data Build Tool
([ICU Data user guide](https://unicode-org.github.io/icu/userguide/icu_data/)). Even the
floor is 10x utf8proc, and reaching it means adopting ICU's data build tooling into lesh's
build — which is the opposite of what ADR-0005 is protecting. ICU is also C++ with its own
symbol-versioning scheme, which makes "vendored and statically linked" materially harder
than for a C library. **Not a candidate.**

### What the terminal programs actually ship

| project | grapheme segmentation | width | Unicode ver. |
|---|---|---|---|
| **fish 3.x** (C++) | **none** — codepoints | vendored `widechar_width.h`, 1,522 lines | 15.0.0 |
| **fish 4.x** (Rust) | **none** — codepoints | vendored `widechar_width.rs`, 1,694 lines / 42,277 B, CC0-1.0 | 17.0.0 |
| **kitty** | own precomputed UAX #29 DFA | own multi-stage tables, ~61 KB runtime | 17.0.0 |
| **wezterm** | `finl_unicode` crate | vendored widecharwidth port, 1,682 lines / 41,989 B | 16.0.0 |
| **helix** | `unicode-segmentation` crate | `unicode-width` **pinned at `=0.1.12`** | (crate-defined) |

**fish** is the most directly comparable program, and its answer is the most striking. Both
eras ship the *same* generated table from
[ridiculousfish/widecharwidth](https://github.com/ridiculousfish/widecharwidth) — fish 3.7.1
at `src/widecharwidth/widechar_width.h` (Unicode 15.0.0), fish 4.x as a workspace crate
`fish-widecharwidth` at
[`crates/widecharwidth/src/widechar_width.rs`](https://github.com/fish-shell/fish-shell/blob/master/crates/widecharwidth/src/widechar_width.rs)
(Unicode 17.0.0, CC0-1.0). The generator reads only `UnicodeData.txt`, `EastAsianWidth.txt`
and `emoji-data.txt` — **not** `GraphemeBreakProperty.txt`. fish does no grapheme
segmentation in its line editor at all; there are 10 mentions of "grapheme" in the entire
source tree, and the one in the reader is an admission:

> `// The position calculations work on codepoints rather than graphemes, which can result in additional issues.`
> — [`src/reader/reader.rs:4095`](https://github.com/fish-shell/fish-shell/blob/master/src/reader/reader.rs)

`unicode-segmentation` appears in fish only in the `printf` crate, for `%s` precision. So the
most widely used modern interactive shell ships **42 KB of generated width table and nothing
else**, and works on codepoints.

widecharwidth's design is itself instructive: rather than returning a number, it returns a
*category* — `One`, `Two`, `NonPrint`, `Combining`, `Ambiguous`, `PrivateUse`, `Unassigned`,
`WidenedIn9`, `NonCharacter` — pushing the policy decision back to the application. Its
README cites the same UAX #11 disclaimer quoted in §1 as the reason. That is the shape lesh
should copy regardless of which table it uses.

**kitty** went the furthest and built everything. Generator
[`gen/wcwidth.py`](https://github.com/kovidgoyal/kitty/blob/master/gen/wcwidth.py) (1,247
lines) downloads `PropList.txt`, `UnicodeData.txt`, `emoji-sequences.txt`,
`EastAsianWidth.txt`, `GraphemeBreakProperty.txt`, `DerivedCoreProperties.txt` (for `InCB`
only), `emoji-data.txt`, and `GraphemeBreakTest.txt` for conformance. Output is
`kitty/char-props-data.h`, 3,046 lines / 933,369 bytes of source that compiles to **~61 KB of
runtime tables** (`CharProps_t1[4352]`, `CharProps_t2[46592]`, `CharProps_t3[106]`,
`GraphemeSegmentationResult_t1[4096]`, `GraphemeSegmentationResult_t2[2880]`). UAX #29 is
implemented as a **precomputed DFA**, not runtime rule evaluation — a 9-bit
`GraphemeSegmentationState` and a 10-bit result, so the whole engine in
[`char-props.c`](https://github.com/kovidgoyal/kitty/blob/master/kitty/char-props.c) is 38
lines. Notably kitty *deleted* its old `wcwidth-std.h` (3,322 lines) in commit
[`3d0e45ace8`](https://github.com/kovidgoyal/kitty/commit/3d0e45ace8), 2025-03-24, replacing
it with the unified multi-stage table. Full UAX #29 segmentation landed in 0.42.0: "Now kitty
does full grapheme segmentation following the Unicode 16 spec when splitting text into
cells".

**wezterm** splits the two problems across two sources, and swapped its segmenter for
performance. Commit [`96c4e7e9b9`](https://github.com/wezterm/wezterm/commit/96c4e7e9b9),
2022-09-09, "Switch to finl_unicode for grapheme clustering":

> "According to its benchmarks, it's almost 2x faster than unicode_segmentation. It doesn't
> appear to make a visible difference to `time cat bigfile`, but I'll take anything that
> gives more headroom for such little effort of switching."

Width is a vendored widecharwidth port,
[`wezterm-char-props/src/widechar_width.rs`](https://github.com/wezterm/wezterm/blob/main/wezterm-char-props/src/widechar_width.rs),
1,682 lines, Unicode 16.0.0, expanded at runtime into a flat `[WcWidth; 65536]` 64 KiB BMP
array with binary search above U+FFFF. wezterm does **not** use `unicode-width` (it appears
in `Cargo.lock` only transitively).

**helix** is the one project that took the pure-library route, and its `Cargo.toml` records
the bill:

> ```toml
> # unicode-width is changing width definitions
> # that both break our logic and disagree with common
> # width definitions in terminals, we need to replace it.
> # For now lets lock the version to avoid rendering glitches
> # when installing without `--locked`
> unicode-width = "=0.1.12"
> ```
> — [`helix-core/Cargo.toml`](https://github.com/helix-editor/helix/blob/master/helix-core/Cargo.toml)

And its width function concedes the cluster-width gap this document opened with:

> ```rust
> // TODO properly handle unicode width for all codepoints
> // example of where unicode width is currently wrong: 🤦🏼‍♂️
> UnicodeWidthStr::width(g).max(1)
> ```
> — [`helix-core/src/graphemes.rs:116-118`](https://github.com/helix-editor/helix/blob/master/helix-core/src/graphemes.rs)

**What they concluded, collectively:** nobody uses ICU. Three of five generate or vendor a
static table rather than depend on a general Unicode library. Two of five (kitty, wezterm)
do genuine UAX #29 segmentation; fish deliberately does not; helix does but knows its cluster
widths are wrong. The single project that depends on an upstream width library had to pin it
backwards.

---

## 3. The hand-rolled option, sized honestly

### The rules

UAX #29 revision 47 (Unicode 17.0.0) defines grapheme cluster boundaries in
**GB1–GB13 plus GB999 — 16 rules** ([UAX #29](https://www.unicode.org/reports/tr29/)). They
depend on exactly three properties: `Grapheme_Cluster_Break` (13 values plus Other),
`Extended_Pictographic`, and `Indic_Conjunct_Break`. This is a small state machine; kitty
compiles it to a DFA with a 9-bit state, which bounds the whole thing.

### The table — measured, three ways

Built from the Unicode 17.0.0 UCD files, packing `Grapheme_Cluster_Break` + `Extended_Pictographic`
+ `InCB` + `East_Asian_Width` into one record per codepoint. The key measurement:

> Across all 1,114,112 codepoints there are only **25 distinct property tuples.**

So the record is one byte, and the whole table is a question of how to index it:

| representation | size | lookup |
|---|---:|---|
| 2-stage trie, 128-cp blocks (8,704 + 24,960 B) | **33,664 B (32.9 KB)** | 2 array loads, O(1) |
| 3-stage trie (2^5 / 2^4 blocks) | **14,800 B (14.5 KB)** | 3 array loads, O(1) |
| sorted ranges, 2,618 × (u32, u8) | **13,090 B (12.8 KB)** | ~12-step binary search |

For comparison, utf8proc's equivalent data costs 336,606 bytes — **10x** the two-stage trie
and **23x** the three-stage one, for a strict superset of function.

Lookup cost, benchmarked over 20 full sweeps of the codespace:

```
hand trie : 0.43 ns/lookup
utf8proc  : 1.09 ns/lookup
```

The two-stage trie is recommended over the smaller variants: 33 KB is already negligible
against the binary, and two dependent loads with a hot 8.7 KB stage-1 beats both a third
indirection and a 12-step search on the keystroke path that spec §3 constraint 3 governs.

### Verification is free and it is rigorous

Unicode ships `auxiliary/GraphemeBreakTest.txt` — **766 test cases** for Unicode 17.0.0,
machine-readable, covering exactly the GB rules. kitty runs it
(`kitty_tests/GraphemeBreakTest.json`, 159,699 bytes); so does utf8proc. This is a
data-driven gtest and it is the thing that makes hand-rolling defensible: the correctness
claim is checkable, not asserted. N-4's combining marks / ZWJ emoji / wide CJK properties sit
on top of it, and malformed-byte degradation belongs on the `ctest --preset debug` sanitized
gate where #59, #62 and #63 already established the precedent.

### Churn per Unicode release — measured

Property assignments diffed across six releases:

| version | GCB-assigned cps | Δ | EAW W/F | Δ | Ext_Pictographic | Δ |
|---|---:|---:|---:|---:|---:|---:|
| 12.0.0 | 17,811 | – | 181,886 | – | 3,793 | – |
| 13.0.0 | 17,840 | 29 | 182,440 | 558 | 3,537 | 256 |
| 14.0.0 | 17,953 | 118 | 182,494 | 54 | 3,537 | 0 |
| 15.0.0 | 18,003 | 50 | 182,516 | 22 | 3,537 | 0 |
| 15.1.0 | 18,003 | **0** | 182,521 | 5 | 3,537 | 0 |
| 16.0.0 | 18,060 | 84 | 182,719 | 198 | 3,537 | 0 |
| 17.0.0 | 18,101 | 43 | 182,876 | 157 | 2,848 | **689** |

Two entries deserve comment, because they are the two failure modes of a stale table:

- **15.1.0 shows zero data churn but is the most disruptive release in the window.** It added
  **rule GB9c** and with it a brand-new property, `Indic_Conjunct_Break`, read from
  `DerivedCoreProperties.txt` — a file no prior width-or-break generator was parsing.
  Verified directly: `GB9c` is absent from UAX #29 revisions 37, 39 and 41 (Unicode 13, 14,
  15) and present from revision 43 onward. A generator that only re-runs against the files it
  already knows about would have silently produced a correct-looking, wrong table.
- **17.0.0 *removed* 689 codepoints from `Extended_Pictographic`** — non-emoji dingbats such
  as U+2605 BLACK STAR and U+2612 BALLOT BOX WITH X. Removals change GB11 behaviour for ZWJ
  sequences, and a table that is merely *old* gets them wrong in the opposite direction from
  a table that is merely *incomplete*.

**Honest update burden:** a ~150-line generator, re-run once a year, producing a diff of a
few dozen table entries, gated by 766 conformance cases. Over five years that was one new
rule and one new input file. It is not free. It is bounded, mechanical, and — critically —
*detectably wrong* when neglected, because the conformance data ships with the standard.

---

## 4. Binary size and startup cost

ADR-0005 constrains the shipped artifact; spec §3 constraint 3 requires latency to be
measured rather than assumed. Both, measured:

| candidate | binary delta | startup cost | per-lookup |
|---|---:|---:|---:|
| hand-rolled, 3-stage trie | **~15 KB** | 0 | ~0.5 ns |
| hand-rolled, 2-stage trie | **~33 KB** | 0 | **0.43 ns** |
| utf8proc (whole library) | **+341 KB** (measured 349,024 B) | 0 | 1.09 ns |
| ICU4C 78, static | **33.06 MB** | **0.553 ms** first `createCharacterInstance` | – |

The utf8proc figure is a real link, not an estimate: a program calling only
`utf8proc_charwidth` and `utf8proc_grapheme_break_stateful` linked against `utf8proc.c`
measured 382,456 bytes against a 33,432-byte baseline, with `__TEXT.__const` at 336,606 bytes.

**Static tables cost binary size and nothing else.** They live in `.rodata`, are
demand-paged, and touch zero pages until the first non-ASCII codepoint arrives — which for a
shell prompt is often never. There is no initialiser, no allocation, no `std::locale`, and
nothing to do at `main()`. Both hand-rolled and utf8proc score identically here: zero.

**ICU does not.** 0.553 ms to construct the first break iterator is a real, unavoidable,
per-process cost — the data file must be located, mapped and validated, and the rule-based
break iterator built from its compiled rule table. Subsequent constructions are 4.3 µs. For
a shell whose startup budget is "imperceptible, and measured", spending half a millisecond
before the prompt paints, to answer a question about characters that may never appear, is
not a trade worth discussing.

One caveat on ICU's process-level numbers: end-to-end `fork`+`exec` of the 33 MB static
binary measured only ~0.07 ms above baseline, because the data blob is mapped lazily. The
0.553 ms lands on first *use*, not on load — which for an interactive shell means it lands on
the first keystroke that contains a non-trivial character, i.e. exactly where it hurts most.

---

## 5. Version skew

### The problem, stated by the people who hit it hardest

This is the question the ticket was right to flag, and the answer is that it is a coordination
problem with no library-shaped solution. kitty states it best
([text-sizing-protocol.rst](https://github.com/kovidgoyal/kitty/blob/master/docs/text-sizing-protocol.rst),
shipped in kitty 0.40.0):

> "Fundamentally, this is a co-ordination problem. Both the client program and the terminal
> have to somehow share the same database of character properties and the same algorithm for
> computing string lengths in cells based on that shared database. Sadly, there is no such
> shared database in reality. The closest we have is the Unicode standard. Unfortunately, the
> Unicode standard has a new version almost every year and actually changes the width assigned
> to some characters in different versions… Expecting all terminals and all terminal programs
> to have both up-to-date character databases and a bug free implementation of this algorithm
> is not realistic."

wezterm's source carries the same conclusion plus the detail that kills the obvious fix
([`wezterm-cell/src/lib.rs`](https://github.com/wezterm/wezterm/blob/main/wezterm-cell/src/lib.rs),
doc comment on `grapheme_column_width`):

> "Differing opinions about the width leads to visual artifacts in text and line editors,
> especially with respect to cursor placement. There aren't any really great solutions to this
> problem, as a given terminal emulator may be fine locally but essentially breaks when ssh'ing
> into a remote system with a divergent wcwidth implementation. This means that a global
> understanding of the unicode version that is in use isn't a good solution."

### How bad it actually is, measured by someone else

Jeff Quast's `ucs-detect` survey of roughly 35 terminals
([Perfecting Terminal Character Width Using Correction Tables](https://www.jeffquast.com/post/perfecting-terminal-character-width-using-correction-tables/))
found **23 distinct implementations** of which characters count as Wide, **19** of ZWJ emoji
handling, and **21** of language grapheme support. Even among terminals that implement mode
2027, Contour measures 54 emoji differently from its peers. Mitchell Hashimoto's survey
([Grapheme Clusters and Terminal Emulators](https://mitchellh.com/writing/grapheme-clusters-in-terminals))
found the farmer emoji 🧑‍🌾 occupying **2 to 6 cells** depending on terminal; Terminal.app
reports 6.

### Does any candidate help? No — and newer is worse

**No library helps, and this is structural.** Every candidate hands lesh *one* Unicode
version's opinion. Skew is not caused by having a bad opinion; it is caused by having a
*different* opinion from the terminal on the other side of the pty. A more correct, more
current table makes the disagreement worse whenever the terminal is older, which is most of
the time.

The evidence for this is that the ecosystem pins *backwards*, deliberately:

- **helix** pinned `unicode-width` at `=0.1.12` because newer versions "disagree with common
  width definitions in terminals" — quoted in full in §2.
- **wezterm** defaults `unicode_version` to **9**, documenting the reasoning:
  "defaults to unicode version 9 as that is the most widely used version (from the perspective
  of width) at the time of writing, which means that the default experience has the lowest
  chance of mismatched expectations"
  ([config docs](https://github.com/wezterm/wezterm/blob/main/docs/config/lua/config/unicode_version.md)).
  Its `LATEST_UNICODE_VERSION` constant is hardcoded to 14 even though its tables are Unicode 16.
- **fish** sidesteps it by not clustering at all.

### The protocol answers, and their state

| mechanism | what it does | status |
|---|---|---|
| **mode 2027** (`DECSET/DECRST/DECRQM ? 2027`) | terminal MUST do UAX #29 clustering; ZWJ emoji are one image of width 2; extending a cluster does not move the cursor | [contour-terminal/terminal-unicode-core](https://github.com/contour-terminal/terminal-unicode-core) — **still a 2021 draft, "ALPHA STAGE", never released**. Implemented: contour, foot (build-gated, uses libutf8proc), ghostty (off by default), wezterm (declared, permanently on), Windows Terminal, mintty. **kitty refuses.** |
| **kitty text-sizing** (`OSC _text_size_code ; w=N ; text ST`) | client declares the cell count; terminal obeys | shipped in kitty 0.40.0. kitty-only. |
| **`OSC 1337 ; UnicodeVersion=N ST`** | negotiate a Unicode version out of band, with push/pop | iTerm2, wezterm |
| **`cell_widths` override table** | app tells the terminal its table | wezterm |
| **CPR round-trip (`CSI 6 n`)** | ask where the cursor actually landed | universal, but too slow per-grapheme |

Kovid Goyal's refusal of 2027 is explicit ([kitty#7799](https://github.com/kovidgoyal/kitty/issues/7799)):
"I probably wont support 2027 as I have something better in mind" — the something better being
the text-sizing protocol, whose stated design is "removing the co-ordination problem and
putting only one actor in charge of determining string width."

Crucially, **mode 2027 fixes segmentation agreement but not version agreement**: the spec
explicitly declines to pin a Unicode version, noting that Unicode "had a major breakage
between version 8 and 9 with regards to some codepoints having their east asian width
changed… we do not expect that to happen that soon nor that frequent to address future
incompatibilities as of this spec and leave this for a later point."

And there is no standards body converging this. The freedesktop terminal-wg has **36 of 37
issues still open**, including
[#9 "Double-width characters in Unicode 9+"](https://gitlab.freedesktop.org/terminal-wg/specifications/-/issues/9),
open since 2019-01-30 with 82 comments. The de-facto standards were both set unilaterally by
terminal authors.

### What this means for lesh's architecture

The width table must be a **replaceable policy object, not a hardcoded function**, because
the correct answer is a property of the terminal on the other end, not of Unicode. Concretely
lesh needs to be able to: default conservatively; accept a `unicode_version`-style override;
accept an explicit per-codepoint override table; and, later, detect and use a protocol when
one is present. A vendored library bakes its numbers into a call you cannot bend.

There is also a cheap structural mitigation that costs nothing to adopt now: **never place the
cursor by relative motion over text whose width is uncertain — redraw the line and place the
cursor absolutely.** kitty documents the more surgical version of this (emit each grapheme,
walk back with `CSI n D`, re-insert with `CSI n @`, so the net delta equals `wcswidth`
regardless of the terminal's opinion). A line editor that repaints from a known column is
robust to being wrong about width in a way that one doing incremental cursor arithmetic is
not. That is a §4 line-editor design constraint, and it is worth writing down now.

---

## Recommendation

**Generate the tables ourselves. Do not vendor utf8proc. Do not consider ICU.**

ADR-0005 sets the test: "a library that does a job better than a hand-rolled equivalent is a
reason to take the dependency, not to avoid it." Applied honestly, utf8proc fails it — not on
the dependency rule, which it satisfies comfortably, but on the *better* clause:

1. **It does not do this job better; it does a bigger job.** The measured cost is 341 KB
   against 33 KB — 10x — for a strict superset in which normalization, case folding,
   decomposition, bidi and general category are dead weight lesh will never call and cannot
   strip. The eleven bits lesh reads sit inside a 192-bit record.
2. **It leaves the hard half undone.** Cluster width — the thing F-3 actually requires for
   correct column arithmetic — has no API in utf8proc, or in any candidate. Measured, summing
   its per-codepoint widths over a ZWJ family emoji gives 4 where the terminal renders 2. lesh
   writes that policy layer either way, so the dependency buys the easy half only.
3. **The genuinely hard part is small and pre-verified.** 16 rules, 25 distinct property
   values, and 766 conformance cases that Unicode ships in the box. This is a bounded piece of
   work whose correctness is *checkable* rather than trusted — which is the standard §6 sets,
   and the standard a vendored blob quietly exempts itself from.
4. **Convergent practice is unambiguous.** kitty generates its own tables and its own UAX #29
   DFA. wezterm vendors a generated table. fish vendors the same generated table and doesn't
   cluster at all. None of the four uses ICU. The one project that depended on an upstream
   width crate had to pin it backwards to stop the rendering glitches.
5. **Version skew converts table ownership from a liability into an asset.** lesh must be able
   to answer "which Unicode version am I pretending to be" at *runtime*, because the right
   answer is the terminal's, not the newest one. Owning the generator is what makes the
   policy layer possible; a vendored table makes it a fight.

The case for utf8proc is real and should be recorded: it is 30 minutes of integration against
a week of careful work, it is MIT, and it tracks Unicode faster than we will. If the editor
schedule slips, vendoring utf8proc behind lesh's own `grapheme_break()` / `cluster_width()`
interface is a *sound* fallback that costs 341 KB and can be swapped out later — the seam is
the same either way. That optionality is the reason to define the interface first.

### Scope of the recommended work

- **`tools/gen_unicode_tables.py`** (~150 lines). Reads `GraphemeBreakProperty.txt`,
  `emoji-data.txt`, `DerivedCoreProperties.txt` (`InCB` only) and `EastAsianWidth.txt` for a
  Unicode version pinned *in the script*. Emits a two-stage trie header. Regeneration is a
  deliberate commit, never a build step — no network at build time, which is ADR-0005's
  spirit even though its letter is about linking.
- **`src/ui/unicode/grapheme_break.{h,cpp}`** — the GB1–GB13 + GB999 state machine over the
  table. Consider kitty's precomputed-DFA approach; a 9-bit state makes the transition table
  itself trivially small and removes branching from the keystroke path.
- **`src/ui/unicode/width.{h,cpp}`** — cluster width as a **policy struct**, not a free
  function: ambiguous → 1 or 2, emoji presentation and VS15/VS16 handling, unassigned and
  private-use fallbacks, plus an override map. Model the return type on widecharwidth's
  category enum rather than an `int`, so callers must confront the ambiguous cases.
- **Tests.** `GraphemeBreakTest.txt`'s 766 cases as a data-driven gtest. N-4's combining-mark
  / ZWJ / wide-CJK properties on top. Malformed-byte degradation on the sanitized
  `ctest --preset debug` gate.
- **Explicitly out of scope:** UAX #29 word boundaries. Editor word motion gets a
  `WORDCHARS`-style configurable predicate (§1).
- **Deferred, with a seam:** protocol negotiation. Ship conservative defaults (ambiguous = 1,
  no clustering assumed of the terminal, repaint rather than relative cursor motion), and
  leave the policy object replaceable so `unicode_version` config, mode-2027 detection and
  kitty's text-sizing protocol can land later without touching the segmenter.

**Budget:** ~33 KB of binary, zero startup cost, ~0.4 ns per property lookup.

---

## Sources

**Primary specifications**
- [UAX #29, Unicode Text Segmentation](https://www.unicode.org/reports/tr29/) (rev. 47, Unicode 17.0.0); revisions [43](https://www.unicode.org/reports/tr29/tr29-43.html), [41](https://www.unicode.org/reports/tr29/tr29-41.html), [39](https://www.unicode.org/reports/tr29/tr29-39.html), [37](https://www.unicode.org/reports/tr29/tr29-37.html) for GB9c dating
- [UAX #11, East Asian Width](https://www.unicode.org/reports/tr11/) (rev. 42, Unicode 17.0.0)
- Unicode 17.0.0 UCD: [`GraphemeBreakProperty.txt`](https://www.unicode.org/Public/17.0.0/ucd/auxiliary/GraphemeBreakProperty.txt), [`GraphemeBreakTest.txt`](https://www.unicode.org/Public/17.0.0/ucd/auxiliary/GraphemeBreakTest.txt), [`EastAsianWidth.txt`](https://www.unicode.org/Public/17.0.0/ucd/EastAsianWidth.txt), [`emoji-data.txt`](https://www.unicode.org/Public/17.0.0/ucd/emoji/emoji-data.txt), [`DerivedCoreProperties.txt`](https://www.unicode.org/Public/17.0.0/ucd/DerivedCoreProperties.txt)

**Libraries**
- [JuliaStrings/utf8proc](https://github.com/JuliaStrings/utf8proc) — v2.11.3, [`utf8proc.h`](https://github.com/JuliaStrings/utf8proc/blob/master/utf8proc.h), [`LICENSE.md`](https://github.com/JuliaStrings/utf8proc/blob/master/LICENSE.md), [`data/Makefile`](https://github.com/JuliaStrings/utf8proc/blob/master/data/Makefile)
- [ICU Data user guide](https://unicode-org.github.io/icu/userguide/icu_data/); ICU4C 78 measured locally
- [ridiculousfish/widecharwidth](https://github.com/ridiculousfish/widecharwidth)

**Programs**
- fish: [`crates/widecharwidth/src/widechar_width.rs`](https://github.com/fish-shell/fish-shell/blob/master/crates/widecharwidth/src/widechar_width.rs), [`src/reader/reader.rs`](https://github.com/fish-shell/fish-shell/blob/master/src/reader/reader.rs), [`Cargo.toml`](https://github.com/fish-shell/fish-shell/blob/master/Cargo.toml); v3.7.1 `src/widecharwidth/widechar_width.h`
- kitty: [`gen/wcwidth.py`](https://github.com/kovidgoyal/kitty/blob/master/gen/wcwidth.py), [`kitty/char-props.c`](https://github.com/kovidgoyal/kitty/blob/master/kitty/char-props.c), [`kitty/char-props-data.h`](https://github.com/kovidgoyal/kitty/blob/master/kitty/char-props-data.h), [text-sizing protocol](https://sw.kovidgoyal.net/kitty/text-sizing-protocol/), [#7799](https://github.com/kovidgoyal/kitty/issues/7799), [#8533](https://github.com/kovidgoyal/kitty/issues/8533)
- wezterm: [`wezterm-cell/src/lib.rs`](https://github.com/wezterm/wezterm/blob/main/wezterm-cell/src/lib.rs), [`wezterm-char-props/src/widechar_width.rs`](https://github.com/wezterm/wezterm/blob/main/wezterm-char-props/src/widechar_width.rs), [commit 96c4e7e9b9](https://github.com/wezterm/wezterm/commit/96c4e7e9b9), [`unicode_version` docs](https://github.com/wezterm/wezterm/blob/main/docs/config/lua/config/unicode_version.md)
- helix: [`helix-core/Cargo.toml`](https://github.com/helix-editor/helix/blob/master/helix-core/Cargo.toml), [`helix-core/src/graphemes.rs`](https://github.com/helix-editor/helix/blob/master/helix-core/src/graphemes.rs)

**Version skew**
- [contour-terminal/terminal-unicode-core](https://github.com/contour-terminal/terminal-unicode-core) — mode 2027 spec, [`spec/terminal-unicode-core.tex`](https://github.com/contour-terminal/terminal-unicode-core/blob/master/spec/terminal-unicode-core.tex), [issue #4](https://github.com/contour-terminal/terminal-unicode-core/issues/4)
- [Mitchell Hashimoto, Grapheme Clusters and Terminal Emulators](https://mitchellh.com/writing/grapheme-clusters-in-terminals)
- [Jeff Quast, Perfecting Terminal Character Width Using Correction Tables](https://www.jeffquast.com/post/perfecting-terminal-character-width-using-correction-tables/)
- [terminal-wg/specifications issue #9](https://gitlab.freedesktop.org/terminal-wg/specifications/-/issues/9)

**Shell word-motion semantics**
- [zsh Parameters — `WORDCHARS`](https://zsh.sourceforge.io/Doc/Release/Parameters.html)
- [GNU Readline manual — `forward-word` / `backward-word`](https://tiswww.case.edu/php/chet/readline/readline.html)
