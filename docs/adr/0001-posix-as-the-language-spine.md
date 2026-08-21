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
difference is written down here and carried as an `[xfail: divergence ...]` case in
`tests/spec`, so it stays a decision rather than becoming a drift.

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
