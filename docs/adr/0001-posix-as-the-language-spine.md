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
