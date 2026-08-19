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
Everything is vendored and statically linked. Dependencies are permitted but must earn
their place; test-time dependencies are unrestricted.

## Consequences

- abseil is deleted. It earns nothing.
- After ADR-0003 and the line editor landing, the runtime dependency set is empty.
- Writing lesh's own line editor is now a structural consequence, not only a preference.
- Unicode segmentation (grapheme clusters, display width) is the first real test of this
  rule. Static linking is permitted, so the choice is hand-rolled versus vendored, not
  rule versus exception.
- Binary size and startup time stay measurable and attributable.
