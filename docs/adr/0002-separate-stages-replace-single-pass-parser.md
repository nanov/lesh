# ADR-0002: Replace the single-pass parser with separate stages

**Status:** Accepted
**Date:** 2026-08-20

## Context

`zsh_parser_plus.h` parses and executes in one traversal, templated on `is_executing`,
mutating the input buffer in place with null terminators. This was chosen to serve
allocation and latency goals, not as an end in itself.

It has structural consequences:

- Variable expansion breaks when a word has both a prefix and a suffix. `echo a$HOME-b`
  prints `a`, because the name scan consumes `HOME-b` and null-terminates the buffer at
  the terminator, destroying the rest. Repeated expansion in one word (`$HOME$HOME`)
  overwrites rather than accumulates.
- There are no seams to unit-test. An expander that does not exist as a component
  cannot be tested as one.
- Live syntax highlighting needs a lexer runnable on every keystroke over a buffer it
  does not own. In-place mutation forbids this.
- Completion needs a tree for incomplete input. A traversal that executes as it reads
  cannot produce one.

## Decision

Replace it with **lexer, parser, expander, and executor as separate components**,
built beside the old parser and driven to parity by the differential harness
(strangler migration), then delete `zsh_parser_plus.h`.

Contracts:

1. The lexer owns no memory and mutates nothing.
2. The parser always returns a tree, with error nodes for invalid input.
3. The expander is a pure function of (AST word node, shell state) to arena-backed argv.
4. The executor is an interface.

**Expansion is a service the executor calls per command, not a fixed stage.**
POSIX requires expansion at execution time: loop bodies re-expand each iteration,
`$?` changes between commands.

## Consequences

- Allocation and layout constraints still bind. They are met with arena-backed
  structures rather than by avoiding the tree.
- Per-layer unit testing becomes possible, which two of the four verification layers
  require.
- The executor back end (tree-walk, bytecode, or lowering to the extension runtime)
  becomes a decision that can be deferred and revisited.
- The existing substrate (`buffer_pool`, `hybrid_vector`, the `char*`-first discipline)
  carries over unchanged. It is the part worth keeping.
- There is a working shell at every point during the migration.
