# ADR-0007: Free everything on shutdown

**Status:** Accepted
**Date:** 2026-08-20

## Context

LeakSanitizer cannot distinguish "leaked" from "deliberately owned for process
lifetime". Its first run against real input reported a 14-byte leak at startup: a
`strdup` in `SimpleParsingState`'s `const char*` constructor. That allocation is not a
leak — alias ASTs point into the buffer, so it must outlive the parsing state. Freeing
it produced a heap-use-after-free in `normalize_aliases()`.

That leaves three options for a project whose Phase 1 gate is "the substrate does not
leak":

1. Annotate the deliberate allocations (`__lsan_ignore_object`, suppression files).
2. Accept a non-zero baseline and compare against it.
3. Free everything on shutdown, so the expected count is zero.

Options 1 and 2 both produce a gate that reports noise, and a gate that reports noise
is one everyone learns to ignore.

## Decision

**lesh frees all memory on shutdown.** Every allocation has an owner that releases it
before `main` returns, including allocations that live for the whole process.

The leak gate's expected result is therefore **exactly zero leaks**, with no
suppression file and no baseline.

## Consequences

- The gate becomes binary and trustworthy. Any LeakSanitizer output is a real defect,
  and no one has to judge whether a given report is expected.
- Ownership becomes explicit everywhere. Allocations that currently have no owner —
  the alias text buffer being the known case — must acquire one. That is substrate and
  shell-state design work, not cleanup.
- It costs shutdown time. With arena-based allocation this is a handful of frees rather
  than a graph walk, and the process is exiting regardless, so the latency constraint is
  not meaningfully affected. If a measurement ever shows otherwise, this decision is
  worth revisiting — but it is cheap to hold until then.
- It runs against the common C and C++ practice of letting the OS reclaim at exit. That
  practice is fine for programs without a leak gate. It is incompatible with having one
  that means anything.
- Anything genuinely unable to have an owner — a third-party library's own one-time
  allocations, for instance — is the only case for a suppression, and each one needs a
  written reason.
