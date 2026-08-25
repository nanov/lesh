# Serena's tools vs. the built-ins: a measured delta

**Subject:** `nanov/lesh` — a POSIX shell in C++23. ~21.7k lines of first-party
code across `src/syntax`, `src/runtime`, `src/substrate`, plus `tests/unit`.
Vendored dependencies (abseil, sol2, googletest, lua, replxx) sit under
`include/`.

**Backend:** Serena 1.5.3, LSP backend, clangd 19.1.2, `claude-code` context
(20 tools exposed). Every number below came from running the tool, not from
reasoning about it. Every edit was applied to disk, compiled with
`clang++ -fsyntax-only -std=gnu++23`, and reverted with `git checkout`; the
working tree was confirmed clean between experiments.

**Scope note.** This measures what Serena adds where it applies. Tasks it does
not target (reading a changelog, grepping for a log string) are listed in §8 as
context, not as shortcomings.

---

## 1. Headline: what Serena changes

Serena is a symbol-addressing layer over clangd. It replaces *"find the text,
compute the line range, send the text back"* with *"name the symbol."* That
single change produces three different outcomes depending on the task, and they
are not the same size.

**(a) Where Serena adds capability.** Four things it does that the built-ins
cannot do at any call count:

1. **Resolve a use site to a definition through the compiler's index**,
   including into vendored third-party headers, with hover facts attached. From
   `sol::state&` in `src/main.cpp` it returned
   `include/sol2/include/sol/state.hpp:31-58` plus `Size: 48 bytes, alignment 8
   bytes` — a compiler-derived fact with no textual equivalent.
2. **Distinguish two symbols that share a name.** Renaming one of the two
   distinct `FakeVars` classes touched 1 file / 2 sites. A word-boundary `perl`
   rename, used correctly, touched 2 files / 6 sites and silently renamed an
   unrelated class in another translation unit.
3. **Attribute a reference to its enclosing symbol.** References to `policy_for`
   came back tagged `parser_impl/parse_redirect`, `evaluator/parse_number`,
   `lexer::next` — grep returns `file:line` and stops there.
4. **Report compiler diagnostics without a build.** `get_diagnostics_for_file`
   agreed with `clang++ -fsyntax-only` on every file I checked.

**(b) Where Serena applies but adds nothing measurable.** Two findings, both
real:

- **Cross-file rename of a distinctive identifier.** Renaming
  `has_pattern_characters` → `has_glob_metacharacters` across 5 files / 12
  sites: Serena's 1 call and a 2-call `grep -rlw | xargs perl -pi` produced
  **byte-for-byte identical diffs**. Zero delta.
- **Listing direct implementations of an interface.** `find_implementations` on
  `parameter_source` returned `shell_state` and `FakeParams`; `grep -rn "public
  parameter_source"` returned the same two. This codebase's hierarchies are one
  level deep, `final`, and list their bases on the declaring line — the
  conditions under which text search is exactly as good.

**(c) Where Serena has no tool at all.** `move`, `inline`, and `type_hierarchy`
exist only on the JetBrains backend (`jet_brains_move`,
`jet_brains_inline_symbol`, `jet_brains_type_hierarchy`), verified against the
tool registry. On the LSP backend, moving a symbol between modules, moving a
file, inlining a helper, and asking for a class's *supertypes* are all built-in
work.

**Verdict:** Serena's delta is large and specific on symbol identity — resolving,
disambiguating, and attributing names — moderate and payload-shaped on edits over
~15 lines, and genuinely zero on mechanical renames of unambiguous identifiers.

---

## 2. Added value and differences by area

Ordered by frequency × value per hit.

- **Symbol-addressed reading replaces grep-then-compute-a-range.** *Frequency:
  very high* — every code task opens this way. *Value per hit: moderate.* Recall
  was identical (47 members found both ways in `parser_impl`), and grep's output
  was smaller (2,677 B vs ~5,000 B). What Serena adds is **end lines** and
  correct handling of nested/anonymous namespaces; what grep adds is full
  signatures. The saving is 1–2 calls and the elimination of a format-discovery
  step (my first grep returned zero matches because the file is tab-indented).

- **Edit payload inverts around ~15 lines.** *Frequency: high.* *Value per hit:
  ~2x input tokens on large edits, ~74x against Serena on tiny ones.* Measured on
  identical, byte-identical-outcome edits to `src/runtime/glob.cpp`:

  | Edit size | `Edit` bytes in | `replace_symbol_body` bytes in | ratio |
  |---|---|---|---|
  | ~1 logical line (rename a local, 5 sites) | 14 | 1,035 | **0.014x** |
  | ~26 lines (restructure a method) | 1,630 | 1,035 | **1.57x** |
  | ~77 lines (whole body) | 5,504 | 2,842 | **1.94x** |

  The asymptote is exactly 2x and it is structural: `Edit` must quote the old
  text *and* the new text; `replace_symbol_body` quotes only the new.

- **Reference precision scales with how word-like the identifier is.**
  *Frequency: high.* *Value per hit: 0x to ~50x, bimodal.* For `policy_for`, grep
  found 11 mentions (1 definition + 10 uses) and Serena found the same 10 uses —
  no advantage. For the class `executor`, "who uses this type" was **2 semantic
  hits vs 106 grep hits** (125 including docs), because 90 of the grep hits were
  comments and prose. Distinctive names: no delta. Names that are also English
  words or filename stems: large delta.

- **Scope disambiguation prevents a class of wrong edit that correct grep usage
  does not.** *Frequency: low-to-medium* (needs an actual name collision).
  *Value per hit: very high* — it is the difference between a correct refactor
  and a silently broken unrelated file. Demonstrated on `FakeVars` (§5).

- **External/vendored symbol lookup.** *Frequency: medium-low.* *Value per hit:
  high.* 1 call vs 3+, and it removes a disambiguation judgment: grepping
  `class state` in `include/sol2` returns 16 candidates spanning a forward
  declaration, an `.rst` doc, and the real definition.

- **Refactoring verbs that don't exist here.** *Frequency: medium.* *Value: a
  negative delta against expectations, not against the built-ins.* If you plan a
  session around "Serena will move this symbol," you will find no such tool on
  the LSP backend.

**Verdict:** The reliable, everyday gains are payload on large edits and one-call
symbol resolution; the dramatic gains are rare but high-stakes, and the
advertised refactoring surface is narrower than the tool list suggests.

---

## 3. Detailed evidence, grouped by capability

### 3.1 Codebase understanding

**Repo layout (task 1).** Serena's `list_dir` / `find_file` are excluded by the
`claude-code` context as duplicates of Glob/Bash. Done with `ls` + `find` +
`wc -l` in one Bash call. **Not applicable to Serena.**

**Structural overview of a 1,500-line file (task 2).**

| | Serena | Built-in |
|---|---|---|
| Calls | 2 (`get_symbols_overview` depth 3; `find_symbol` depth 1 on the class) | 3 (grep top-level; inspect indentation; tab-aware grep) |
| Output | ~450 B + ~5,000 B | 607 B + 2,677 B |
| Members found | 46 methods + 1 ctor + 2 nested classes + 11 fields | 47 signatures |
| Extra it gives | **end lines**, field/nested-class inventory, `(anonymous namespace)` nesting | full signatures with types |

The built-in path cost an extra call because my first grep pattern assumed space
indentation and returned zero matches; the file is tab-indented. That is a
one-time-per-repo cost, not a per-task one.

**Next step on each side.** Read `parse_redirect`'s body.
Serena: `find_symbol("parser_impl/parse_redirect", include_body=true)` → 1 call,
exact body, no file read.
Built-in: `Read(offset=1308, limit=94)`, where 1308 came from the grep and 94 from
the *next* grep match at 1403. This works, but it is arithmetic on ephemeral
numbers, and it is off by one: Serena's `body_location` is **0-based** while
`Read`/grep are **1-based**, so my read covered 1308–1401 while the method
actually spans 1309–1402 — clipping the closing brace. Converting between the two
addressing schemes is a real, recurring friction cost.

**Verdict:** Comparable recall and cost for the overview itself; Serena's end
lines are what make the *follow-up* read a single unambiguous call.

### 3.2 References and hierarchy

**References (task 4).** Two cases, opposite outcomes.

`policy_for` (11 textual mentions, 7 files): Serena returned exactly the 10 use
sites, each tagged with its enclosing symbol and a 3-line context window. grep
returned 11 lines including the definition. **Same recall; Serena adds
attribution.** Getting the enclosing function from grep alone would require
reading around each of 10 hits or doing range arithmetic against an overview.

`executor` (class): **2 semantic hits vs 106 grep hits.** 90 of the grep hits are
comments and prose; 3 are filenames. This is the case where "who uses this in
code" and "where does this text appear" are different questions.

**Implementations and supertypes (task 5).** `find_implementations` on
`parameter_source` → `shell_state`, `FakeParams`. `grep -rn "public
parameter_source"` → the same two, one call. **No delta.** The codebase has no
transitive hierarchy to test: every derived class in `src/` is `final` and one
level deep, so "no suitable candidate in this codebase" for transitivity.
Supertypes have **no Serena tool** on the LSP backend — reading
`shell_state.h:42-45` shows all four bases in one call.

**External dependency (task 6).**
Serena: 1 call from the use site → `include/sol2/include/sol/state.hpp:31-58`,
plus the full declaration with its two base classes and `Size: 48 bytes,
alignment 8 bytes`.
Built-in: find the include (`#include <sol/sol.hpp>`), locate the vendor tree,
`grep -rn "class state" include/sol2` → 16 candidates, then judge which of
`forward.hpp:183` (forward declaration), `state.rst:10` (documentation), and
`state.hpp:32` (definition) is the answer. ~3 calls plus a judgment. Size and
alignment are not obtainable without compiling a probe.

Notably this worked **despite `include/sol2/**` being in Serena's
`ignored_paths`** — that list governs Serena's own file traversal, not clangd's
index.

**Verdict:** Serena is decisively better on external symbols and on word-like
identifiers, and exactly equal on distinctive identifiers and on this repo's flat
hierarchies.

### 3.3 Single-file edits

All three edits were applied both ways and produced **identical diffs**; 7a was
verified byte-for-byte (`4 insertions, 4 deletions`, same 4 lines).

**7a — rename a local, 5 occurrences in one method.** `Edit` with
`replace_all` = 1 call, 14 bytes in. Serena's only symbolic path is
`replace_symbol_body` with the full 29-line body = 1,035 bytes in. `rename_symbol`
**cannot** target it: locals are not in the document-symbol tree
(`find_symbol("expand_component/matches")` → `[]`). Serena's size-appropriate
tool here is `replace_content`, which is the direct analogue of `Edit`.
**Built-ins are 74x cheaper on input.**

**7b — 26-line restructure.** `Edit` 1,630 B in (770 old + 860 new) vs Serena
1,035 B. **Serena 1.57x cheaper.**

**7c — 77-line whole-body rewrite.** `Edit` 5,504 B in vs Serena 2,842 B.
**Serena 1.94x cheaper.** Compiles clean.

**Forced reads differ in scope.** `Edit` requires a prior in-conversation `Read`
of the file; `replace_symbol_body` requires a prior `find_symbol(include_body)`
of *the symbol*. For a 29-line method inside a 3,351-line file, that is a
meaningful difference in what you are obliged to pull into context.

**8 — structural insert.** `insert_after_symbol("intern", …)` placed the new
function after `intern`'s closing brace and before the next comment, with correct
blank lines, addressed only by name. The `Edit` equivalent needs a textually
unique anchor — a bare `}` will not do, so you quote surrounding context.

**9 — file-local rename.** Serena: 1 call, 3 sites, compiles. Built-in: `grep -cw`
vs `grep -c` to confirm no substring collisions (3 vs 3), then `Read`, then `Edit
replace_all` — 3 calls. Serena removes the verification step rather than the work.

**Verdict:** `Edit` dominates below ~15 lines by more than an order of magnitude;
Serena dominates above it by up to 2x; the crossover is structural, not tunable.

### 3.4 Multi-file changes

**10 — cross-file rename, unambiguous name.** `has_pattern_characters` →
`has_glob_metacharacters`, 5 files, 12 sites.
Serena: 1 call. Built-in: 2 calls (`grep -rlw | xargs perl -pi -e`).
`diff` of the two resulting patches: **IDENTICAL — byte-for-byte.** Both compile.
**Zero delta.**

**11 / 12-move — move a symbol or a file, updating imports.** No LSP-backend
tool. Built-in plan: `git mv`, rewrite the `#include` lines with perl, update
`CMakeLists.txt`, rebuild. **Built-in only.**

**12-delete — safe delete.** On a referenced symbol, `safe_delete_symbol("intern")`
returned `Cannot delete, … referenced in: {"src/runtime/glob.cpp": [126, 131]}` —
correct contract behaviour, and the reference list is the useful part. On a
genuinely unused symbol it deleted the function but **left the preceding doc
comment and two blank lines orphaned** (3 residual lines), because the LSP symbol
range excludes the leading comment. That is a real tradeoff under correct use: a
line-range delete via `sed`/`Edit` takes the comment with it.

**13 — delete and propagate to call sites.** No tool: `safe_delete_symbol` refuses
whenever references exist, by design. **Built-in only**, and arguably not a
well-defined refactoring.

**13-inline — inline a helper.** `jet_brains_inline_symbol` is JetBrains-only.
`intern` (single-expression-ish, no side effects beyond the arena) would have been
a legal candidate; there is no LSP-backend tool to attempt it. **Built-in only.**

**Verdict:** Serena's multi-file surface is one verb — `rename_symbol` — and on
unambiguous names that verb is matched exactly by a two-call shell pipeline.

### 3.5 Where no meaningful difference exists

Direct-implementation lookup (§3.2), cross-file rename of distinctive names
(§3.4), and structural overview recall (§3.1) all came out even. These are not
Serena failures; they are cases where the textual and semantic answers coincide
because the codebase is well-named and conventionally formatted.

**Verdict:** On a codebase with distinctive identifiers and flat hierarchies, a
large share of Serena's navigation surface is a tie rather than a gain.

---

## 4. Token-efficiency analysis

**Input payload across edit sizes** — the central quantitative result, measured:

```
edit size        Edit(in)   Serena(in)   ratio
~1 line              14        1,035      0.014x   <- Edit 74x cheaper
~26 lines         1,630        1,035      1.57x    <- Serena cheaper
~77 lines         5,504        2,842      1.94x    <- Serena cheaper (asymptote)
```

The 2x ceiling is structural. `Edit` sends `old_string + new_string`; a whole-body
replacement makes `old_string` the entire body, so `Edit` is always ≈2x Serena on
full rewrites and always far cheaper when a short unique anchor suffices.

**Output payload.** Serena's edit confirmations are terser: `{"result":"OK"}` (15 B)
and `Successfully renamed … (5 changes applied)` vs `Edit`'s ~130 B sentence. Small
but consistent, and it favours Serena.

One counting detail worth knowing when reading those confirmations: `N changes
applied` counts **operations (files)**, not sites. The 3-site single-file rename
reported "1 changes applied"; the 12-site five-file rename reported "5".

**Read payloads.** Serena's reads are JSON with escaped tabs and newlines, which
inflates them relative to raw text: `parse_redirect`'s body came back as ~4,300
chars of JSON for ~3,000 chars of code. `Read` adds line-number prefixes,
~6 bytes/line. These roughly cancel; neither side has a real advantage on read
volume. The overview comparison actually favoured grep (2,677 B vs ~5,000 B).

**Forced reads.** This is where Serena's read-side advantage actually lives, and
it is about *scope*, not volume: `Edit` obliges a whole-file `Read`;
`replace_symbol_body` obliges only a symbol-scoped `find_symbol`. On a
3,351-line file that is the difference between pulling in a method and pulling in
a module.

**Stable vs ephemeral addressing.** After two chained edits to `glob.cpp`,
`expand_component` moved 26→35 and `expand_pathnames` moved 58→62. Every line
number captured before the first edit was wrong; both name paths still resolved.
`Edit` is partly insulated from this because it matches strings rather than
offsets — but any workflow that *records* line numbers for later use (a plan, a
todo list, a second read) pays the staleness cost, and Serena's does not.

**Verdict:** Built-ins win decisively on small edits and tie on reads; Serena wins
by up to 2x on large edits and, more durably, by keeping addresses valid across a
session.

---

## 5. Reliability & correctness (under correct use)

**Matching precision.** The decisive experiment. `FakeVars` is declared twice —
`tests/unit/arithmetic_tests.cpp:13` and `tests/unit/expander_tests.cpp:104` —
both inside anonymous namespaces, so they are genuinely distinct types that share
a spelling.

| | files touched | sites | correct? |
|---|---|---|---|
| `rename_symbol` | 1 | 2 | yes |
| `grep -rlw FakeVars tests \| xargs perl -pi -e 's/\bFakeVars\b/…/g'` | 2 | 6 | **no** |

The `perl` invocation is correct usage: word-boundary anchored, scoped to
`tests/`. It still renamed an unrelated class in another translation unit,
because text has no notion of a translation unit. The built-in path *can* be made
correct by scoping to a single file — but only if you already know the collision
exists. **That knowledge is the delta**, and Serena does not require it.

**Scope disambiguation.** `parser_impl` has two `require` overloads (lines
359–361, 365–370). `find_symbol("parser_impl/require[1]")` returned exactly the
second, by index. Text search for `require(` returns both definitions plus every
call site.

**Atomicity — the claim does not hold as stated.** I read the implementation.
`serena/code_editor.py:344`:

```python
def _apply_workspace_edit(self, workspace_edit) -> int:
    operations = self._workspace_edit_to_edit_operations(workspace_edit)
    for operation in operations:
        operation.apply()
    return len(operations)
```

A plain sequential loop: no transaction, no rollback, no pre-flight check that
all targets are writable. If operation 3 of 5 raised, operations 1–2 would
already be on disk. **Serena's cross-file rename is not atomic in the
transactional sense.** What it *does* guarantee is that the **change set is
computed atomically** — one `textDocument/rename` against clangd's index yields
the complete edit list before any write. That rules out a *partially specified*
refactor, which is the failure mode a hand-rolled chain of `Edit` calls actually
has. It does not rule out a partially *applied* one. The practical protection on
both sides is the same: git.

**Semantic queries vs text search.** Covered in §3.2 — bimodal, 0x on distinctive
names and ~50x on word-like ones.

**External dependency lookup and its infrastructure cost.** This is Serena's most
infrastructure-dependent capability. It required a valid `compile_commands.json`
at the repo root covering the file in question, a clangd binary, and a completed
background index. When the compilation database is stale or missing entries,
symbol resolution silently degrades to guessed flags. Grep has no such
dependency. That is a genuine tradeoff: the capability is real, and it is
contingent on build-system state that the built-ins never touch.

**Success signals.** `Edit` returns a confirmation sentence. Serena returns `OK`
or a change count. Neither confirms *correctness*. The meaningful signal is
`get_diagnostics_for_file`, which agreed with `clang++ -fsyntax-only` on every
file I tested — worth real money in C++, where the built-in equivalent is a
compile (0.5s for one TU here; minutes for the project).

**Verdict:** Serena's correctness advantage is precision of *targeting*, not
durability of *application* — and its strongest queries rest on build-system
state that grep does not require.

---

## 6. Workflow effects across a session

**Chained edits.** I ran three edits to `glob.cpp` in sequence — insert a
`join_path` helper, rewrite `expand_component` to call it, then collapse a third
copy of the same idiom via `replace_content` — addressing everything by name
path, with **zero re-reads between edits**. The result compiled clean and reduced
the join idiom from 3 copies to 1.

`Edit` would also have completed this chain without re-reads, because it matches
strings rather than offsets and the three regions were disjoint. The advantage is
narrower than it first appears: it materialises when edits **nest or overlap**
(editing a method you have already partly rewritten), where `Edit` needs the
current text and Serena needs only the name.

**Where the advantage compounds.** The overview → find_symbol → replace_symbol_body
chain reuses one address (`parser_impl/parse_redirect`) at every stage. Nothing
recomputed, nothing re-read.

**Where it diminishes.** Twice per experiment, a `git checkout` invalidated the
harness's file-state tracking and forced a fresh `Read` before the next `Edit` —
a cost Serena's tools do not pay, but also one that only arises from
revert-heavy workflows like this evaluation.

**A cost that grows with session length.** Every Serena tool schema occupies
context for the whole session. At 20 tools that is the working set; the six
context-excluded duplicates (`read_file`, `execute_shell_command`, `find_file`,
`list_dir`, `search_for_pattern`, `create_text_file`) would add schema weight for
tools the context prompt then instructs you not to use.

**Verdict:** Advantages compound mildly for symbol-dense editing sessions and are
roughly neutral for sessions dominated by disjoint small edits.

---

## 7. Unique capabilities

Capabilities with no practical built-in equivalent:

1. **Use-site → definition resolution through the compiler index, including into
   vendored code, with hover facts** (size, alignment, resolved base classes).
   *Frequency: medium-low. Impact: high* — the layout facts are simply
   unobtainable textually.
2. **Distinguishing same-named symbols in different scopes/TUs.**
   *Frequency: low. Impact: very high per occurrence* — it is the difference
   between a correct and a silently incorrect refactor.
3. **Reference results attributed to the enclosing symbol.**
   *Frequency: high. Impact: low-moderate* — reconstructible from grep plus an
   overview, but only with extra calls and range arithmetic.
4. **Compiler diagnostics without invoking the build.**
   *Frequency: medium. Impact: moderate-high in C++* specifically, where the
   built-in equivalent is slow.
5. **Overload addressing by index** (`require[1]`).
   *Frequency: low in C++ free functions, higher in method-heavy code.
   Impact: moderate.*

Explicitly **not** unique: cross-file rename (matched exactly by a 2-call perl
pipeline on unambiguous names), interface implementation lookup, structural
overview.

**Verdict:** Five genuine uniques, of which only reference attribution is
high-frequency, and only scope disambiguation is high-stakes.

---

## 8. Tasks outside Serena's scope (built-in only)

Listed for completeness, not as shortcomings:

- Reading non-code files — `CMakeLists.txt`, `CMakePresets.json`, `README.md`,
  `.clang-format`, JSON, logs. `Read`.
- Free-text search — log strings, magic constants, URLs, TODO markers,
  `#include` lines, build flags. `Grep`.
- Repository layout and file discovery — Serena's `list_dir`/`find_file` are
  excluded by the `claude-code` context as built-in duplicates.
- Running anything — builds, `ctest`, the conformance sweep, git operations,
  `clang++ -fsyntax-only`. `Bash`.
- Creating new files, moving files, editing build configuration.
- Moving symbols between modules, inlining helpers, querying supertypes — no
  LSP-backend tool exists (§3.4).

**Share of a session.** In this evaluation, 23 of ~45 tool calls were Bash or
Read doing work Serena does not target — measurement, compilation, reverting,
config inspection. That over-samples shell work because reverting was the
methodology. For ordinary feature work on this repo I would estimate **30–45%**
of calls fall outside Serena's scope, which bounds how much of a session its
augmentation can touch.

**Verdict:** Roughly a third to a half of real work is outside Serena's target
area, so its ceiling as an augmentation layer is the remainder.

---

## 9. Practical usage rule

Decide by **task type**, not by preference:

| Task | Use | Why |
|---|---|---|
| Edit < ~15 lines, anchor is unique | `Edit` / `replace_content` | 74x less input at 1 line |
| Rewrite most or all of a symbol's body | `replace_symbol_body` | ~2x less input; no line arithmetic |
| Rename anything that might be ambiguous | `rename_symbol` | only tool that knows scope |
| Rename a distinctive identifier repo-wide | either | byte-identical results; pick by habit |
| "Who calls this?" for a word-like name | `find_referencing_symbols` | 2 hits vs 106 |
| "Who calls this?" for a distinctive name | `grep -rw` | same recall, fewer moving parts |
| Read one method out of a large file | `find_symbol(include_body)` | one call, exact range, no off-by-one |
| Find a third-party symbol's definition | `find_declaration` | 1 call vs 3 + a judgment call |
| Check a change compiles, fast | `get_diagnostics_for_file` | agrees with clang, no build |
| Delete a symbol | `safe_delete_symbol`, then tidy the comment | reference check is the value; expect 3 orphan lines |
| Move / inline / supertypes | built-ins | no LSP-backend tool exists |
| Non-code files, free text, shell, builds | built-ins | outside Serena's scope |

The single highest-value habit: **let Serena decide *what* to change (find,
disambiguate, resolve), and choose the writer by edit size.**

**Verdict:** Use Serena to establish symbol identity and to rewrite whole bodies;
use the built-ins for everything textual, everything small, and everything that
isn't code.

---

## Appendix: methodology

- Every edit applied to disk, compiled with
  `clang++ -fsyntax-only -DLESH_ENABLE_ASSERTS -I src -std=gnu++23 -Wall`,
  then reverted with `git checkout -- .`.
- `git status --short` confirmed clean between every experiment. Final state
  clean; the only remaining artifact is this file.
- Payload byte counts measured in Python against the actual strings sent,
  not estimated.
- Diff equivalence checked with `diff -q` on captured `git diff` output.
- Atomicity assessed by reading
  `serena-agent/.../serena/code_editor.py:344-390`, not by inducing a failure.
- Tool availability verified against `serena.tools.ToolRegistry`, not
  documentation.
- Files exercised: `src/runtime/glob.cpp`, `src/syntax/parser.cpp`,
  `src/runtime/pattern.{h,cpp}`, `src/runtime/expander.cpp`,
  `src/substrate/numeric.h`, `src/runtime/executor.h`, `src/main.cpp`,
  `tests/unit/{arithmetic,expander,pattern}_tests.cpp`.

### Known limits of this evaluation

- **One language, one codebase.** C++/clangd. Serena's overload addressing and
  rename precision would likely show larger deltas in Java or C#; its hierarchy
  tools would show more in a deep OO codebase than in this flat one.
- **Favourable and unfavourable reference cases were both sampled** (`executor`
  at 106:2, `policy_for` at 10:10), but two points do not define the
  distribution.
- **Atomicity was assessed by code reading**, not by inducing a mid-write
  failure.
- **The `move`/`inline` verdict is backend-specific.** On the JetBrains backend
  those tools exist and this section of the analysis would not apply.
