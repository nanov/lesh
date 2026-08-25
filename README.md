# lesh - a mess of a shell

```
      ..               .x+=:.
x .d88"               z`    ^%    .uef^"
 5888R                   .   <k :d88E
 '888R        .u       .@8Ned8" `888E
888R     ud8888.   .@^%8888"   888E .z8k
888R   :888'8888. x88:  `)8b.  888E~?888L
  888R   d888 '88%" 8888N=*8888  888E  888E
  888R   8888.+"     %8"    R88  888E  888E
  888R   8888L        @8Wou 9%   888E  888E
 .888B . '8888c. .+ .888888P`    888E  888E
^*888%   "88888%   `   ^"F     m888N= 888>
   "%       "YP'                 `Y"   888
J88"
@%
:"
```

A POSIX shell, built from scratch in C++23 as a systems-craft exercise.

Allocation discipline, data layout, and latency are treated as correctness properties
rather than optimizations. POSIX conformance is the scoreboard that keeps that honest.
Nobody has to use lesh for it to have succeeded.

## Status

Early, and openly incomplete. What works today: pipelines, aliases, quoting, command
substitution, brace expansion, and scalar/array/map variables, behind a replxx-based
line editor with history.

What does not work yet: control flow, functions, here-documents, field splitting,
pathname expansion, and most of parameter expansion. The single-pass parser that got
lesh this far is being replaced — see
[ADR-0002](docs/adr/0002-separate-stages-replace-single-pass-parser.md) for why.

## What it is aiming at

- **Full POSIX sh conformance** as the language floor, with a curated zsh-inspired
  layer on top admitted one feature at a time.
- **Its own line editor.** replxx is a stopgap behind an interface, expected to be
  deleted rather than extended.
- **A plugin API** in LuaJIT, over a flat C capability surface its FFI consumes directly.
- **One self-contained binary.** Everything vendored and statically linked; no runtime
  shared-library dependencies.

## Documentation

| Document | What it holds |
|---|---|
| [`CONTEXT.md`](CONTEXT.md) | The glossary. What the project's words mean, and which ones to avoid. |
| [Scope and architecture](docs/superpowers/specs/2026-08-20-lesh-scope-design.md) | Scope boundaries, binding constraints, subsystem contracts, verification strategy, roadmap. |
| [`docs/adr/`](docs/adr/) | The decisions with teeth, and why they were made. |

Planned work lives on the issue tracker, not in this file. The current effort is
charted as a [wayfinder map](https://github.com/nanov/lesh/issues/1); its open child
issues are the decisions still to be made.

## Building

```sh
git submodule update --init --recursive
cmake -B build
cmake --build build -j8
./build/lesh
```

Tests: `./build/lesh_tests`

## Reading

- [zsh prompt expansion](https://zsh.sourceforge.io/Doc/Release/Prompt-Expansion.html#Prompt-Expansion)
- [replxx](https://github.com/AmokHuginnsson/replxx) — line editing, done right
- [mastering-zsh on aliases](https://github.com/rothgar/mastering-zsh/blob/master/docs/helpers/aliases.md)
