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

Early, and openly incomplete — but the language is there. The separate-stages front
end ([ADR-0002](docs/adr/0002-separate-stages-replace-single-pass-parser.md)) passes
**99.8% of the yash POSIX suite**: pipelines, compound commands, functions, all of
parameter and arithmetic expansion, field splitting, pathname expansion,
here-documents, redirections, traps and the POSIX builtins. The single-pass parser it
replaced has been deleted.

Interactive works too, on lesh's own line editor. `lesh -i` gets syntax
highlighting driven by the real lexer, autosuggestions from history, path
completion, rebindable keys (`bind`), and a prompt built from a template
(`prompt`, with `{path}`, `{git}`, `{status}`, `{time}` and the rest).

What does not work yet: **job control** — a stopped or backgrounded child is
reported and the terminal reclaimed, but there is no job table and no `fg`/`bg`.
Brace expansion and array/map variables went with the old shell; they return with
the curated zsh layer, on the new core.

## What it is aiming at

- **Full POSIX sh conformance** as the language floor, with a curated zsh-inspired
  layer on top admitted one feature at a time.
- **Its own line editor.** replxx was a stopgap and is gone; the replacement is in,
  written against the real lexer and the error-tolerant parser, and still growing.
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
- [mastering-zsh on aliases](https://github.com/rothgar/mastering-zsh/blob/master/docs/helpers/aliases.md)
