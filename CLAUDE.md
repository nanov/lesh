## Agent skills

### Issue tracker

Issues live as GitHub issues on `nanov/lesh`, managed via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, each label string equal to its name. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context — one `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

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

**`spec_run.py` needs `--frontend next`.** With no flag it runs the *legacy*
front end and reports about 35 passes instead of ~690. Legacy is the old
implementation being replaced; it is not the shell under development.

**Never invoke `third_party/yash-tests/run-test.sh` directly.** It does not set
`LESH_FRONTEND`, so it measures the *legacy* front end and scores near zero.
`tools/conformance.py` sets it. Six times on this map a runner defect has
masqueraded as a shell bug, and this is the cheapest one to fall into.

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
