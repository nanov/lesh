# ADR-0004: TypeScript via ScriptC as the extension runtime

**Status:** Superseded by [ADR-0006](0006-luajit-as-the-extension-runtime.md) — ScriptC is unscheduled
**Date:** 2026-08-20

## Context

lesh needs plugins: widgets, key bindings, hooks, and completion functions authorable
without recompiling the shell. Candidates considered were Lua/LuaJIT, C compiled at load
time (CJIT/TinyCC), WebAssembly, and TypeScript compiled to native via ScriptC.

Neovim's Vimscript-and-Lua split is the cautionary example: two peer scripting languages
produce two ecosystems, two idioms, and a decade-long migration. lesh's equivalent of
Vimscript is the shell language itself.

ScriptC compiles ordinary TypeScript to native binaries with no JavaScript engine linked
in, with an opt-in dynamic tier and hard compile errors for what it cannot handle. It is
young.

## Decision

**TypeScript via ScriptC is the first and primary extension runtime.** Lua is removed
(ADR-0003). The extension layer lands in Phase 5, after POSIX conformance.

**The capability surface is a flat C ABI, designed C-first and TypeScript-second.**
Opaque handles, a versioned capability struct, plain data. No C++ types across the
boundary, no pointers into the arena handed to plugins, no callbacks capturing lambdas.

## Consequences

- Every candidate mechanism stays reachable: ScriptC, WASM, CJIT, and plain `dlopen` are
  all just ways to turn a source file into callable behavior behind the same surface.
  Choosing wrong costs a binding, not a redesign.
- A stable C ABI means plugins in Zig, Rust, or C++ work for free.
- ABI stability becomes a real commitment. Compiled plugins cannot absorb API churn the
  way a scripting binding can. Additive-only discipline and a version field from day one.
- Native plugins can crash the shell. WASM remains the answer if isolation becomes
  necessary; the surface design is what keeps that option open.
- Sandboxing addresses crashes, not hangs. An infinite loop in a widget freezes the
  keystroke loop regardless of tier, so execution limits are needed either way.
- **Pending spike:** whether ScriptC can emit a loadable shared library and consume a
  C ABI. It is pitched as producing executables. If it cannot, the mechanism changes;
  the surface does not.
