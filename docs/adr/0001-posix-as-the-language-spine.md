# ADR-0001: POSIX sh as the language spine

**Status:** Accepted
**Date:** 2026-08-20

## Context

lesh began as a zsh-inspired shell with an ad-hoc feature list driven by whatever was
interesting to build next. That makes "out of scope" indistinguishable from "not yet",
so scope never closes and nothing can be called finished.

The project's goal is systems craft, not adoption. But craft without a target degrades
into a toy, and the stated bar is "more crafted than a toy".

## Decision

**Full POSIX sh conformance is the language spine.** A curated zsh-inspired layer sits
on top, admitted one feature at a time by explicit decision.

## Consequences

- Scope becomes finite and externally defined. A feature is in scope when POSIX
  requires it, or when it is written into the scope document.
- Conformance is measurable. An external test suite becomes the scoreboard, and
  "how done are we" has a number.
- The surface is large: IFS field splitting, globbing, the full parameter expansion
  family, here-documents, `trap`, special-builtin error semantics.
- Job control is not scheduled in Phases 0-5. POSIX places it in the User
  Portability option rather than the mandatory core.
- The single-pass parser cannot reach this target. See ADR-0002.

## Recorded divergences from dash

dash is the reference, not the specification. Where lesh deliberately differs, the
difference is written down and carried as a `[divergence: ...]` case in `tests/spec`,
so it stays a decision rather than becoming a drift.

Such a case is **not compared against dash**. dash cannot be the expectation of a
case whose whole content is that lesh answers differently, so the case states its own
expected output instead:

```
--- name [divergence: why, and what dash does instead]
code
=== expect [status: 127] [stderr]
the exact stdout lesh is expected to produce
```

`tools/spec_run.py` reports these in their own tally - neither a pass nor a
known-fail, so `known-fail` counts only gaps that are not yet implemented - and
FAILs if a recorded expectation stops holding. There are 59 such cases today.

The entries below are the divergences argued in this ADR. The rest are argued where
the cases are, in the block comments of `tests/spec/posix_gaps.spec` and
`tests/spec/invocation.spec` - 21 of the 59, chiefly the `getopts` state rules, the
POSIX `--` separator on the operand-only special builtins, the file-descriptor access
mode checks, `pipefail`, and how `command -v` writes a pathname. **They belong here
and are not here yet**; the marker text of each case carries its reason and the yash
assertion that settles it, which is the source to write them up from.

- **`kill -l` lists what the PLATFORM has, not what dash has** (issue #38). dash
  carries a fixed table of 32 signals; on glibc that omits `SIGRTMIN..SIGRTMAX`,
  which the platform really does define and which `trap` there really can take.
  lesh derives the list from `<signal.h>` and the C library, so the two lists agree
  on macOS and lesh's is longer on Linux. The corpus therefore asserts the shape and
  the round-trip properties of the list rather than its full text.
- **The `SIG` prefix is accepted on a signal name** (issue #38). `trap : SIGURG` and
  `kill -s SIGURG` work; dash recognises only the unprefixed spellings POSIX lists
  and rejects both. bash, ksh, yash and zsh all accept the prefix, POSIX does not
  forbid the wider set, and no case in the conformance suite asserts rejection.
- **`trap` lists nothing for a signal it cannot trap** (issue #37). A signal ignored
  on entry to a non-interactive shell cannot be trapped or reset. dash and zsh
  nevertheless accept the trap command, list it back, and then never run it; bash
  records nothing and lists nothing, and lesh follows bash. A listing that names an
  action the shell will not take is the same defect as a builtin that succeeds
  without doing anything, and the listing is required to be re-inputtable.
- **The "ignored on entry" rule applies only to a NON-interactive shell** (issue
  #37). POSIX XCU 2.11 scopes it that way; dash and bash both apply it to an
  interactive shell as well. The conformance suite agrees with POSIX rather than
  with them - `sigurg6-p.tst` runs the testee as `sh -i` with SIGURG already
  ignored and requires the trap to fire - so this divergence is measured by the
  suite rather than carried as a corpus case, which would only assert dash's bug.
- **An interactive shell CATCHES SIGINT rather than dying of it** (issue #52). POSIX
  XCU 2.11 has an interactive shell ignore SIGQUIT and SIGTERM and catch SIGINT, so
  the interrupt abandons the command being run and the shell reads on. dash gets the
  first two right and dies on the third: with input that is not a terminal its
  top-level unwind exits with status 130 instead of reading the next command, which
  is five assertions per file that dash fails in `sigint5-p.tst` and `sigint6-p.tst`.
  bash abandons the command and carries on, the conformance suite requires that, and
  lesh follows bash. `$?` afterwards is `128 + SIGINT`, which is zsh's answer and
  dash's exit status; bash answers 1 and is the outlier.
- **The interactive default action does not survive into a subshell** (issue #52).
  It belongs to the process that reads commands and has a prompt to return to, so a
  subshell, a command substitution and any command the shell runs take the default
  action back. dash and bash both keep sparing the subshell; the suite says both are
  wrong - `sigterm5-p.tst` requires `( "$TESTEE" -c 'kill -s TERM $PPID' )` under
  `sh -i +m` to be killed - so this one is settled by the suite rather than by either
  shell. dash additionally hands its interactive `SIG_IGN` straight to a forked child,
  which then survives a SIGTERM aimed at itself; it is inconsistent with its own
  `exec`, which does drop the ignore. bash and lesh drop it in both.
- **`return` with no operand inside a TRAP ACTION reports the status the trap
  interrupted** (issue #33's machinery, extended by the control-flow work). POSIX
  gives `exit` and `return` the same default - the status of the last command
  executed - and says of `exit` that when it "is executed in a trap action, the
  last command is considered to be the command that executed immediately preceding
  the trap action". dash applies that to `exit` and not to `return`; bash and zsh
  apply it to neither. The conformance suite asserts it for `exit` four times in
  `exit-p.tst`, all four of which dash passes, and once for `return` in
  `return-p.tst`'s 'default exit status in function in trap', which expects 19
  where dash, bash and zsh all answer 0. A shell whose two spellings of "the last
  command" answer the same question differently is the real defect, so lesh
  applies the rule to both.
- **`test -nt` and `-ot` compare against a file that does NOT exist** (issue #35's
  builtin, extended by the control-flow work). A file that is not there has no
  modification time, so an existing file is newer than it and it is older than
  every existing file: `test XXXXX -ot newer` and `test newer -nt XXXXX` are both
  true, which is what `test-p.tst` asserts and what bash answers. dash, zsh and
  macOS's own `test(1)` all report false the moment either `stat` fails, which
  makes an ABSENT file indistinguishable from one with an identical timestamp -
  and telling those two apart is the entire purpose of the operator, since
  `[ out -nt in ]` is how a hand-written build rule spells "rebuild" and there is
  no `out` on the first run. POSIX did not define these operators when dash
  adopted them; the suite and bash define them the useful way.
