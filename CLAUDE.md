## Agent skills

### Issue tracker

Issues live as GitHub issues on `nanov/lesh`, managed via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, each label string equal to its name. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context — one `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

## Model tiers

Owner's standing directive, repo-wide: implementation task tickets are
dispatched to subagents, and **Opus-class is the gate for implementation**.
Drop to Sonnet-class only when the ticket is really clear — every decision
made, the brief precise, nothing left to interpret; Haiku-class only for
mechanical sweeps. The ticket brief plus the spec index is a task agent's
whole context; briefs are written for that. Grilling, charting, review and
merge judgment stay on the orchestrating session's model. A task agent that
hits a genuine design question stops and returns it to the tracker rather
than deciding on a cheaper tier.

## Measuring

Two numbers, two binaries, and they are not interchangeable.

**The gate is sanitized.** `ctest --preset debug` runs the unit tests and the
differential corpus under ASan/UBSan/LSan. This is where boundary inputs live -
#59, #62 and #63 each found real undefined behaviour from a one-line input the
conformance suite never sends. Never report a fix as verified without it.

**The scoreboard runs release.** `python3 tools/conformance.py` defaults to
`./build/release/lesh` and takes about two minutes; the same sweep on the debug
binary takes about twenty-eight and scores **six lower**, because ASan intercepts
the signals `kill2-p.tst` asserts. Build it with `cmake --preset release &&
cmake --build --preset release -j8`. The tool refuses and tells you how if the
binary is absent, rather than quietly measuring the wrong thing.

**There is ONE shell.** `LESH_FRONTEND` and `spec_run.py --frontend` are gone with
`src/legacy/` (#28); nothing reads the variable, and the corpus runs on one axis.
A command remembered from an older session that still sets it is harmless, and its
output is the shell you wanted either way.

**Never invoke `third_party/yash-tests/run-test.sh` directly.** It has no timeout
and reaps nothing, so one hung case spins a core until you notice - nineteen
minutes, once, on this machine. `tools/conformance.py` gives every case its own
process group and kills by group, and it clears the stale `.trs` files that would
otherwise be counted as this run's. Six times on this map a runner defect has
masqueraded as a shell bug, and this is the cheapest one to fall into.

**Editor-only changes need no sweep.** A change confined to `src/leshper/` or
`src/leshnici/` cannot move the corpus or the conformance score - not because
the binary leaves that code out (`lesh` links `lesh_leshper`, and `lesh_leshnici`
above `lesh_ui` since #170), but because the conformance corpus is
non-interactive: every case runs a script on a pipe, and nothing in it ever
enters the line editor. Iterate with
`./build/debug/lesh_tests --gtest_filter='Leshper*:Leshnici*'` (milliseconds),
adding `:Grapheme*` when positions or width are involved. The full sanitized gate
still runs once at merge; the exemption is from the sweeps, not the gate.
**`src/ui/` is NOT exempt** - the host is the interactive path itself, and
`UiPty` execs the real `lesh` binary, so a change there can move both. Its
suites are the `Ui*` ones (`UiLoop*`, `UiSession*`, `UiWorkers`, `UiKnowledge*`,
`UiLoopProposals`, and since #168 Phase B `UiHighlight`, `UiAutosuggest`,
`UiHistorySearch`, `UiComplete*`, `UiCommandKind`, `UiReactorTheme`, `UiPty` -
which was `LeshperPty` and is renamed for the same rule: a suite that execs the
real binary over a pty is the host's, not the editor's - and since #170
`UiPrompt*`, which was `LeshperPrompt*`: the prompt engine is `src/ui/prompt/`
because a prompt renders shell facts), and the filter above deliberately leaves
them out: `Leshper*` is the editor, `Ui*` is the host that drives it (#168). A
`src/ui/` change runs
`--gtest_filter='Ui*:Leshper*'` and then the sweeps. **`src/ui/` is where the
KNOWLEDGE lives now** - the highlighter, the autosuggester, the history searcher,
the completion sources, the shell's tables and, since #170, the prompt engine
all live there - so a change that looks like "just the editor" is a
`src/leshper/` change and a change to any of those is not. The one exception the
exemption still leans on:
`lesh_leshper` links `lesh_substrate` and nothing else, so a `src/leshper/` change
provably cannot reach syntax, the runtime or a file descriptor.

**Per-file scores reproduce exactly; totals do not across environments.** Measure
before and after in the same environment and quote the delta, never a remembered
baseline. Four tickets on this map have opened with a headline number that was
already stale, so re-measure before working one. Under parallel agents, quote
per-file numbers only - a total measured under load is worthless.

## Tool routing

Serena and the built-ins overlap; each capability has one owner. Numbers below
were measured on this repo - see `serena-evaluation.md`.

**Serena's context prompt says Read and Edit are FORBIDDEN. That is wrong here,
and this file overrides it.** For a one-line change `Edit` sends 14 bytes where
`replace_symbol_body` sends the whole 1,035-byte body. The crossover is around
**15 lines**: below it use `Edit`, at or above it use `replace_symbol_body`,
which sends only the new text where `Edit` must quote old and new (~2x at 77
lines). Neither is forbidden.

**Serena owns symbol identity.** `find_symbol`, `find_referencing_symbols`,
`find_declaration`, `rename_symbol`. It is the only side that knows a
translation unit: renaming one of the two same-named `FakeVars` test classes
touched the right 2 sites, while a correct word-boundary regex silently
corrupted an unrelated file. Use `rename_symbol` whenever a name might be
ambiguous.

**Grep owns distinctive names.** Renaming `has_pattern_characters` across 5
files produced a byte-identical diff via `rename_symbol` and via
`grep -rlw | xargs perl -pi`. Reach for Serena when the identifier is also an
English word - "who uses `executor`" is 2 semantic hits against 106 grep hits.

**`get_diagnostics_for_file` before you build.** It agreed with
`clang++ -fsyntax-only` on every file tested. Cheaper than a build for
type errors; it is not a substitute for `ctest --preset debug`, which is still
the gate.

**Serena has no move, inline, or supertype lookup** on the LSP backend - those
are JetBrains-only. Use `git mv` plus perl, and read the class declaration.

**Serena line numbers are 0-based; `Read` and grep are 1-based.** Add one when
crossing over, or the closing brace goes missing.
