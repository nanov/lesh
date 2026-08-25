# ADR-0003: Remove Lua and sol2

**Status:** Superseded by [ADR-0006](0006-luajit-as-the-extension-runtime.md) — Lua returns as LuaJIT
**Date:** 2026-08-20

## Context

Lua was embedded via sol2 as a scripting and configuration layer. The integration is
currently disabled: the init block in `src/main.cpp` is commented out, while
`zsh_parser_plus.h` still calls `_lua_engine.try_execute_lua_function`.

sol2 and Lua together are 24 MB of the roughly 43 MB of vendored submodules. Lua adds a
second value model, a garbage collector, and startup cost to a project whose stated
constraints include near-zero allocation and instant startup.

The extension runtime decision has since moved to TypeScript via ScriptC (ADR-0004),
and the extension layer is scheduled after POSIX conformance rather than before it.

## Decision

**Remove Lua, sol2, `leshrc.lua`, and the `_lua_engine` call sites.** The shell
language is its own configuration language: `.leshrc` is shell.

## Consequences

- 24 MB of vendored dependencies and an entire language runtime leave the startup path.
- One value model instead of two.
- A POSIX shell configured by POSIX shell is more coherent than one that boots a second
  interpreter to configure itself.
- Between now and the extension runtime landing, the line editor has no scripting layer.
  It must therefore be configurable from the shell language itself: `bindkey`-style
  builtins, actions registered natively, keymaps set from `.leshrc`. This is how zsh
  works, and it keeps the editor useful the moment it exists.
- Last session's CMake work to vendor and pin Lua 5.4.8 is superseded.
