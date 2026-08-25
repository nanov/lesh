# ADR-0005: No runtime shared-library dependencies

**Status:** Accepted
**Date:** 2026-08-20

## Context

Vendored submodules totalled roughly 43 MB. abseil (14 MB) was commented out in CMake
with zero references anywhere in `src/`. sol2 and Lua (24 MB) are removed by ADR-0003.
replxx is a stopgap to be deleted when lesh's own line editor lands.

Dependency creep is the standard way a project with performance constraints loses them,
because each individual addition is defensible.

## Decision

**lesh ships as one self-contained binary with no runtime shared-library dependencies.**
Everything is vendored and statically linked. **Dependencies are permitted** - built
from source by us and statically linked is fully acceptable, and a library that does a
job better than a hand-rolled equivalent is a reason to take the dependency, not to
avoid it. The rule constrains the shipped artifact, not the source tree. Test-time
dependencies are unrestricted.

Per platform: on Linux libc++ is statically linked. On macOS the platform libc++ is
linked dynamically - it ships with the OS, so it is not something a user must install,
which is what this rule actually protects against.

## Consequences

- abseil is deleted. It earns nothing.
- After ADR-0003 and the line editor landing, the runtime dependency set is empty.
- Writing lesh's own line editor remains a scope decision on its own merits, not a
  consequence of this rule - replxx could have been vendored and statically linked.
- Unicode segmentation (grapheme clusters, display width) is the first real test of this
  rule. Static linking is permitted, so the choice is hand-rolled versus vendored, not
  rule versus exception.
- The same applies inward: the hand-rolled arena and containers are not privileged by
  this rule. If a library does the job better and fits the data-oriented constraints, it
  is a candidate. See the substrate hardening phase.
- Binary size and startup time stay measurable and attributable.
