# ADR-0006: LuaJIT as the extension runtime

**Status:** Accepted
**Date:** 2026-08-20
**Supersedes:** ADR-0003 (Remove Lua and sol2), ADR-0004 (TypeScript via ScriptC)

## Context

ADR-0003 removed Lua on the grounds that it added a second value model, a garbage
collector, and startup cost. ADR-0004 then chose TypeScript compiled to native via
ScriptC, with a spike pending on whether ScriptC can even emit a loadable shared
library.

New information reframes both:

**LuaJIT's FFI is not part of the JIT.** `ffi.C.execve(...)` works with `jit.off()`.
The JIT knows how to inline FFI calls when it fires, but the FFI is a separate
subsystem. The glue-code deletion that is the real argument for LuaJIT survives with
compilation disabled entirely.

**LuaJIT's interpreter is hand-written assembly**, roughly 2-3x PUC Lua's C
interpreter on typical code. That is a permanent win on a shell's workload — many
short scripts, little sustained looping — and it does not depend on the JIT.

**The JIT rarely amortizes on this workload.** Compiling twenty lines that run once
costs more than it saves.

**Startup cost was misattributed.** The expense is not compiling small scripts; it is
constructing the environment — global tables, metatables, bindings.

## Decision

**LuaJIT is lesh's extension runtime**, and the first one shipped.

- **`jit.off()` is the default.** A script opts into compilation with a pragma when it
  is doing real work in a loop.
- **Cache bytecode, not source.** `string.dump` / `luajit -b` skips parse and codegen.
  Marginal at this scale, but free and correct.
- **Attack environment construction, not compilation.** Register bindings lazily behind
  an `__index` metamethod so nothing is built until a script touches it; most scripts
  touch three functions. If that proves insufficient, go zygote: a resident process
  holding a fully warmed `lua_State`, `fork()` per script, so copy-on-write hands over
  the prepared environment at fork cost.

## Consequences

- **The flat C capability surface from ADR-0004 survives, and is now load-bearing
  rather than a hedge.** LuaJIT's FFI calls C, not C++. A surface of flat C-callable
  functions over opaque handles is precisely what the FFI consumes with no glue. The
  constraint that has now outlived three different runtime decisions is evidence it is
  the right invariant.
- **sol2 is probably unnecessary.** Its purpose is C++ binding glue, which is the thing
  the FFI deletes. Open question rather than a decision here.
- **Plugin authors write Lua 5.1**, LuaJIT's language level, plus its 5.2 borrowings.
  Not 5.4. The vendored PUC Lua 5.4.8 submodule is replaced by LuaJIT, superseding last
  session's pin for the second time.
- **Phase 0 still removes Lua and sol2 from the tree now.** The extension layer is
  Phase 5; carrying an unused runtime through four phases buys nothing. LuaJIT gets
  vendored when Phase 5 starts.
- **Feasibility risk drops sharply.** ADR-0004 rested on an unproven ScriptC spike, and
  on TinyCC-family tooling whose arm64 macOS support is the weakest corner of that
  ecosystem. LuaJIT supports arm64 macOS. The ScriptC spike no longer blocks anything.
- **ScriptC is not ruled out**, it is unscheduled. Should a native or sandboxed tier be
  wanted later, the capability surface still admits it — that was the point of designing
  it C-first.
- **`.leshrc` stays shell.** That decision was independent of which runtime exists, and
  a POSIX shell configured by POSIX shell remains more coherent than one that boots an
  interpreter to configure itself.
- **The line editor stays configurable from the shell language** (`bindkey`-style
  builtins). The extension runtime is an additional way to register actions, never the
  only way.
- **The zygote option brushes against job control.** A resident supervisor holding a
  warmed state is close to what job control and history want anyway. Job control is
  ruled out of Phases 0-5; if the zygote lands, that boundary deserves rereading.
