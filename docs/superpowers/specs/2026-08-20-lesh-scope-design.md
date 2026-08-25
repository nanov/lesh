# lesh: Project Scope and Architecture

**Date:** 2026-08-20
**Status:** Approved
**Supersedes:** the implicit scope in `README.md`

## 1. What lesh is

A POSIX-conformant shell built as a systems-craft exercise. Allocation discipline,
data layout, and latency are treated as correctness properties rather than
optimizations. POSIX conformance is the scoreboard that keeps the craft honest.

Three things are true at once, in priority order:

1. **It is a craft playground.** The building is the point.
2. **It has a real target.** Full POSIX conformance, plus a curated zsh-inspired layer.
3. **It is not a toy.** Verification is what enforces that word.

Adoption is not a goal. Nobody has to use lesh for it to have succeeded.

## 2. Scope

### 2.1 In scope: the POSIX floor

The full command language: simple commands, pipelines, `&&` / `||` / `;` / `&` lists,
compound commands (`if`, `while`, `until`, `for`, `case`, subshells, brace groups),
functions, every redirection form including here-documents, the complete quoting rules,
the full parameter expansion family (`${x:-y}`, `${x#pat}`, `${#x}`, and the rest),
command substitution, arithmetic expansion, IFS field splitting, pathname globbing,
exit-status semantics, `trap`, `set` options, and the standard builtin set.

### 2.2 In scope: the curated zsh layer

Admitted one feature at a time by explicit decision, never by drift:
brace expansion, array and map variables, lazy aliases, prompt expansion, history hints.

A zsh feature is in scope when it is written down here. Not before.

### 2.3 In scope: lesh's own line editor

lesh owns its terminal rather than renting it: termios raw mode, input decoding
(escape sequences, UTF-8, bracketed paste), an editing buffer with its own undo model,
keymaps with modal bindings, actions as the unit of editing behavior, syntax
highlighting, completion UI, multi-line editing, and prompt rendering.

replxx is a stopgap behind an interface. It is expected to be deleted, not extended.

The editor is configurable from the shell language itself (`bindkey`-style builtins,
keymaps set from `.leshrc`). It does not depend on the extension runtime existing.

### 2.4 In scope: an extension runtime

A plugin API for actions, key bindings, hooks, and completion functions, authorable
without recompiling lesh. **LuaJIT is the first and primary runtime** (ADR-0006),
shipped with `jit.off()` as the default; scripts opt into compilation when they are
doing real work in a loop.

The capability surface is a flat C ABI. LuaJIT's FFI consumes that directly, with no
binding glue — which is the argument for LuaJIT, and is independent of the JIT.

### 2.5 Out of scope, permanently

- zsh's completion system and `compinit` compatibility
- zle widget and module compatibility
- bash-only extensions that POSIX does not require
- Windows (the design assumes a POSIX process model)
- any promise that lesh is a daily driver

These are the boundary that makes the rest finite. They are not "later".

### 2.6 Deferred, with a seam held open

- **The executor back end.** Tree-walking, an own bytecode VM, or lowering to the
  extension runtime. Decided once the syntax layer is solid. The executor is an
  interface so this stays a real choice (ADR-0002).
- **Job control.** `fg`, `bg`, `jobs`, `%1`, process groups, terminal ownership.
  POSIX places these in the User Portability option, not the mandatory core.
  Not scheduled in Phases 0-5. The executor's process and terminal handling must
  not foreclose it.
- **A second, compiled extension tier.** LuaJIT covers the plugin ecosystem
  (ADR-0006). Should a native or sandboxed tier be wanted later, ScriptC, WASM, and
  plain `dlopen` all remain reachable because the capability surface is C-first.

## 3. Binding constraints

These are the properties that make it lesh. A change that violates one is a
regression even if it passes every test.

1. **Near-zero allocation on the command path.** Arena and pool allocation.
   No `std::string` churn, no per-word malloc.
2. **Data-oriented layout.** Flat, contiguous, cache-friendly structures.
   Stack-first hybrid containers. Minimal pointer chasing.
3. **Latency.** Startup and dispatch fast enough to be imperceptible, and
   measured rather than assumed.
4. **No runtime shared-library dependencies.** Everything vendored and statically
   linked into one self-contained binary. A dependency must earn its place
   (ADR-0005).

**Explicitly not a constraint:** single-pass parse-and-execute. It was a means to
constraints 1 and 3, not an end. It is being replaced (ADR-0002).

## 4. Architecture

### 4.1 Subsystems

```
        +------------- Substrate --------------+
        | arena / buffer_pool, hybrid vectors  |   (everything sits on this)
        +--------------------------------------+

  Lexer  --pure, restartable, non-owning-->  tokens + source spans
    |                                              |
    +----------------------+                       |
    v                      v                       v
  Parser              Highlighter              (line editor)
  error-tolerant AST   per keystroke
    |
    v
  Executor <---calls per command---> Expander <---> Shell State
   (interface)                       tilde -> param -> cmdsub     vars, scopes,
                                     -> arith -> split -> glob    aliases, funcs,
                                     -> quote removal             options, $?

  Line Editor --uses--> Lexer, Parser, State      (never the Executor)
       |
       v
  Extension runtime --- flat C ABI ---> actions, hooks, completion
```

### 4.2 The four contracts

1. **The lexer owns no memory and mutates nothing.** It is callable on every
   keystroke and on a half-typed line. This forbids the in-place null-termination
   the current parser relies on.
2. **The parser always returns a tree.** Incomplete or invalid input yields error
   nodes with source spans and a cursor-to-node mapping. It never fails. Completion
   depends on this, and error recovery cannot be retrofitted.
3. **The expander is a pure function of (AST word node, shell state) -> arena-backed
   argv.** This is what makes it unit-testable and what the differential harness
   targets directly.
4. **The executor is an interface.** The initial tree-walking implementation can be
   replaced without touching anything above it.

### 4.3 Expansion is a service, not a stage

Expansion runs at execution time, not between parsing and execution. A `for` loop
body re-expands on every iteration; `$?` changes between commands. The AST is static;
expansion is dynamic. Modeling expansion as a fixed stage would bake a whole class
of conformance failures into the structure.

### 4.4 Dependency rules

- The line editor may depend on the lexer, parser, and shell state. **Never on the
  executor.** When completion needs to run something, it goes through an explicit
  narrow port. Otherwise the interactive and execution layers fuse and the swappable
  back end stops being swappable.
- The extension runtime depends only on the flat C capability surface. No C++ types
  cross that boundary, no pointers into the arena are handed out, no callbacks
  capture lambdas. Violating this once forecloses WASM permanently.
- Everything may depend on the substrate. The substrate depends on nothing.

## 5. Error handling

**Syntax errors** never propagate as failures. The parser returns its tree with error
nodes carrying source spans, which lets the editor underline them live and lets the
non-interactive path report `line N: syntax error near 'X'` and exit non-zero.

**Runtime errors** follow POSIX: a failure in a *special* builtin exits the shell;
the same failure elsewhere does not. Exit status lives in shell state as a
first-class value, read by both expander and executor.

**Invariant violations** (arena exhaustion, impossible states) are programmer errors.
Assert and die loudly. This is what the ASan debug configuration is for.

**Across the plugin boundary**, errors cross as codes. A flat C ABI cannot carry
anything else.

**Signals cross a seam and must be designed, not discovered.** The editor owns the
terminal, so `SIGINT` while typing and `SIGINT` during a running command are
different paths that must agree about who holds the terminal. `trap` interacts with
both.

## 6. Verification

Four layers, in the order they are introduced:

1. **POSIX conformance suite.** An external, unmoving scoreboard. Wired up in Phase 0
   so it reports an honest score from the start.
2. **Per-layer unit tests.** gtest at each seam: lexer tokens, parser tree shape,
   expander results, executor behavior. This is what the architecture's seams exist
   to make possible.
3. **Differential testing against reference shells.** Snippets run through lesh and a
   reference shell; stdout, stderr, and exit code must match. **The target is 100% on
   the committed subset.** A prototype harness already exists.

   The references are split by layer, and the split is authoritative:
   **dash is the reference for the POSIX floor** (it is close to bare POSIX, so it
   will not quietly bless a zsh-ism as conformant), and **zsh is the reference for the
   curated zsh layer only.** Where the two disagree on POSIX behavior, dash wins.
4. **Benchmarks.** Introduced gradually from Phase 0 as a compass ("are we going the
   wrong way?"), promoted to a gate in Phase 4. Allocation counts per command,
   startup time, parse throughput, keystroke latency.

Benchmarks are meaningless until the build produces a real Release configuration.
That is why it is Phase 0 work.

## 7. Roadmap

| Phase | Work | Gate |
|---|---|---|
| 0 | Substrate extraction; build configurations; test harnesses; repo hygiene; dependency removal | Both scoreboards run and report honestly; Release config exists |
| 1 | **Substrate hardening.** Prove the arena and containers before anything is built on them | Leak-free under ASan and UBSan; copy and move semantics verified; no accidental copies |
| 2 | Lexer, parser, expander, tree-walking executor to POSIX conformance; delete `zsh_parser_plus.h` | POSIX suite green; unit tests at every seam |
| 3 | The curated zsh layer re-landed on the new core | Differential vs zsh at 100% on the committed subset |
| 4 | Own line editor replaces replxx; `bindkey` and action builtins; highlighting via the real lexer; completion via the error-tolerant parser | Keystroke latency benchmarked and gated |
| 5 | Extension runtime over the flat C surface | Plugin API frozen and versioned |

Migration is by strangler: the new implementation is built beside the old parser and driven
to parity by the differential harness. There is a working shell at every point.

### Phase 0 detail

- **Remove abseil** (14 MB, commented out in CMake, zero references in `src/`).
- **Remove lua and sol2** (24 MB). LuaJIT replaces PUC Lua when the extension layer
  lands in Phase 5 (ADR-0006); carrying an unused runtime through four phases buys
  nothing.
- **Build configurations via CMake presets**, replacing hand-edited flags:
  `Debug` at `-O0 -g` with ASan and UBSan, `Release` at `-O3` with warnings as
  errors, and a sanitizer configuration CI can point at the suite. Today
  `-fsanitize=address` is hardcoded into `CMAKE_CXX_FLAGS` while `-O3 -Wall -Wextra`
  sits commented out, so every binary built so far is an unoptimized ASan build.
- **Replace `file(GLOB_RECURSE)` with explicit source lists.** Globbing silently
  misses new files until cmake is re-run.
- **Split the monolithic headers** (`utils.h` at 1113 lines, `zsh_parser_plus.h` at
  1007) into the module layout the seams imply.
- **Untrack `.DS_Store` and `tmp.h`.**
- **Add GitHub Actions.** With a conformance score and a differential pass rate, CI
  is the scoreboard, per commit.
- Keep googletest for now. It is test-only and cheap to swap later.

### Phase 1 detail

The substrate is the one layer every other subsystem sits on, and it is currently
unverified. Building the syntax layer on top of it before it is proven means any leak or
silent copy found later is indistinguishable from a bug in the thing above it.

What has to be established:

- **It does not leak.** Arena reset and reallocation paths, the `_to_free` fallback
  when a pool allocation fails, and `transferable_buffer` growth, all exercised under
  ASan and UBSan.
- **It does what it claims.** Growth from inline storage to heap, capacity arithmetic,
  alignment, and the pointer- and iterator-invalidation rules callers are relying on.
- **It is idiomatic C++.** Copy and move semantics are correct and actually move.
  `utils.h:489` and `utils.h:523` currently declare move constructors taking
  `const T&&` — you cannot move out of a `const` object, so both silently perform
  copies. A counting instrumentation type in the tests pins this: an operation that
  should move must be observed not to copy.
- **Its assumptions are checked, not believed.** `utils.h:90` records
  "it is belived emelents won't have destructor" as the reason destructors are skipped.
  Either the containers are constrained to trivially-destructible types and that is
  enforced by a static assertion, or destructors must run.

This phase changes no behaviour. It converts the substrate from assumed-correct to
known-correct, which is the precondition for treating allocation counts from Phase 0
as meaningful.

## 8. Open decisions

- **Executor back end** (Phase 2 close): tree-walk, own bytecode, or lowering.
- **Does sol2 survive?** (Phase 5): its purpose is C++ binding glue, which is what
  LuaJIT's FFI deletes. Likely dropped, not yet decided.
- **Whether a second, compiled tier is wanted at all** (unscheduled): ScriptC, WASM,
  and `dlopen` all remain reachable.
- **Environment-construction cost** (Phase 5): lazy `__index` binding registration
  first; zygote with a warmed `lua_State` only if that proves insufficient.
- **Job control**: not scheduled in Phases 0-5; revisit once the executor is stable.
- **Unicode segmentation**: the first real test of the dependency rule. Hand-rolled
  or statically-linked library, decided when the editor needs it.
