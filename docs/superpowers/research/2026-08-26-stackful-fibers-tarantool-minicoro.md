# Stackful C fibers for lesh: can we use what Tarantool uses?

**Date:** 2026-08-26
**Ticket:** [#145](https://github.com/nanov/lesh/issues/145) (parent [#82](https://github.com/nanov/lesh/issues/82))
**Status:** Research complete, recommendation pending human review
**Answers:** the owner's queued grilling on #145, questions 1-5
**Constrained by:** [ADR-0005](../../adr/0005-no-runtime-shared-library-dependencies.md) (vendored + static is fine),
[ADR-0006](../../adr/0006-luajit-as-the-extension-runtime.md) (LuaJIT is the extension runtime),
[ADR-0009](../../adr/0009-two-owner-threads.md) (shell thread + leshper loop thread)

Sources read at these commits:

| repo | commit | date |
|---|---|---|
| `tarantool/tarantool` (master) | `990019b3afc9483396ba3ebddad9aec469dd584c` | 2026-08-11 |
| `edubart/minicoro` (main) | `02dad0f8b7cbb12fe6e216ae7a76db15ca55cd7b` | 2024-12-07 |
| `tarantool/luajit` (submodule pin, branch `tarantool-1.7`) | `712e6d859f90f90cf8beed1ffb3f0a77e9c80488` | — |

All measurements were taken on 2026-08-26, Apple Silicon (`arm64-apple-darwin25.2.0`),
Homebrew clang 22.1.8. Commands are given inline and reproduce.

---

## Bottom line

**Vendor minicoro as the switcher, and write the stack allocator ourselves.** minicoro
is 465 lines of live code compiling to **1,768 bytes of `__text`** on arm64, is MIT-0 /
Unlicense (cleaner than anything else on the table), ships correct `__sanitizer_*_switch_fiber`
annotations that engage *automatically* under `-fsanitize=address`, has hand-written aarch64
asm with explicit Mach-O symbol handling, and switches **2.2x faster than Tarantool's coro**
on this machine (5.4 ns vs 12.2 ns per round-trip). Its testsuite is green under lesh's exact
debug flags — `-fsanitize=address,undefined -fno-sanitize-recover=undefined
-fno-omit-frame-pointer`, `ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1` —
on Darwin arm64. That is the sacred gate, cleared, measured, today.

The one thing it does not do is the one thing LuaJIT makes non-negotiable: **minicoro has no
guard pages. `mprotect` appears zero times in the whole file.** Its default stack is 56 KB in
a `calloc` block with the coroutine's own bookkeeping struct sitting directly *below* the
stack, so an overflow silently eats the coroutine before a magic-number heuristic notices at
the next yield. Tarantool, running the same workload lesh wants to run, uses **512 KB** (1 MB
under ASan) with a real `PROT_NONE` guard page. So the deliverable is minicoro's switcher plus
roughly 40 lines of our own `mmap`-and-`mprotect` stack allocator — which minicoro's
`alloc_cb`/`dealloc_cb` seam is designed to accept.

**Upstream libcoro is not a candidate at all.** Its changelog ends 2012-12-21
(`coro.h:81`) and it has no aarch64 backend; its auto-selection would drop Darwin arm64
to `ucontext` (`coro.h:298-315`). **The aarch64 assembly in Tarantool's copy is
Tarantool's own work**, added across `98d994c3` (2015-12-03), `8e17ce15` (2015-12-04) and
`7e722763` (2021-05-11, "fix build for Apple ARM M1"). Tarantool's coro is therefore not
"libcoro"; it is a 2015 fork of libcoro 6.41 plus eight local patches, one of which is the
only reason it runs on the dev machine.

Tarantool's `fiber.c` is **not** liftable and does not need to be. Of its 2,359 lines,
**~411 (17%) are stack management** and the rest is a scheduler bolted to libev, cbus-era
cord threading, `diag`/`say`/`trigger`, the `small` slab allocator, and introspection.
The libcoro API surface it actually consumes is **three call sites** —
`coro_create` at `fiber.c:1561`, `coro_transfer` at `fiber.c:508` and `fiber.c:856`. The
liftable asset is the 17%: the guard-page layout, the madvise watermark, the ASan macros,
the stack-direction probe. Take the pattern, not the file. lesh's `poll(2)` loop stays the
scheduler, as suspected.

Five of issue #145's stated premises turn out to be wrong or incomplete. They are collected
in §7 and two of them change the design.

---

## 1. License: Tarantool's `third_party/coro`

**The two files that matter are dual-licensed BSD-2 *or* GPLv2-or-later, at the recipient's
election.** Vendoring under the BSD-2 branch is clean for lesh. The `LICENSE` file in that
directory is BSD-2 with a pointer to the exception:

> `tarantool@third_party/coro/LICENSE:1` — `Copyright (c) 2000-2009 Marc Alexander Lehmann <schmorp@schmorp.de>`
> `:24-25` — "Alternatively, the following files carry an additional notice that explicitly allows relicensing under the GPLv2: coro.c, coro.h."

and `coro.h:25-34` carries the notice itself:

> "Alternatively, the contents of this file may be used under the terms of the GNU General
> Public License ("GPL") version 2 or any later version, in which case the provisions of the
> GPL are applicable instead of the above. **If you do not delete the provisions above, a
> recipient may use your version of this file under either the BSD or the GPL.**"

This is the standard BSD/GPL dual grant: the GPL is an *option the recipient may take*, not a
condition imposed on them. Tarantool has not deleted the BSD provisions, so a downstream
vendor (lesh) may take the BSD-2 branch. Tarantool's own tree is BSD-2
(`tarantool@LICENSE:1` — "Copyright 2010-2020 Tarantool AUTHORS"), which is consistent.

**Two traps.** First, `tarantool@third_party/coro/README:2-3` says plainly: *"the file
conftest.c in this distribution is under the GPL. It is not needed for proper operation of
this library though, for that, coro.h and coro.c suffice."* Vendoring the directory wholesale
would drag a GPL file into a BSD-2 tree. Copy exactly two files.

Second, **Tarantool's `coro.c` is no longer drop-in**: `coro.c:43` now does
`#include "trivia/config.h"` (for `ENABLE_BACKTRACE`). Building it standalone required
stubbing that header — verified, see §2.

**Verdict:** clean, with the `conftest.c` exclusion and the `trivia/config.h` stub. But see
§5 — the license is not what decides this.

---

## 2. aarch64 / Apple Silicon: `CORO_ASM`, and it is Tarantool's own asm

**Plainly: the asm path covers aarch64, including Darwin, and does not fall back.** But the
aarch64 support is Tarantool's, not upstream libcoro's, and that distinction decides
question 5.

**Backend selection.** Upstream libcoro's auto-selection (`coro.h:298-315`) picks `CORO_ASM`
only for Windows x86/x64 and `__linux && (__i386 || __x86_64)`, falling through to
`CORO_UCONTEXT` when `HAVE_UCONTEXT_H` is set — which is what Darwin arm64 would get. **Tarantool
overrides this from CMake before anything is compiled.** `tarantool@cmake/BuildLibCORO.cmake:12-18`:

```cmake
if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "86" OR ${CMAKE_SYSTEM_PROCESSOR} MATCHES "amd64")
    add_definitions("-DCORO_ASM")
elseif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm" OR ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64")
    add_definitions("-DCORO_ASM")
else()
    add_definitions("-DCORO_SJLJ")
endif()
```

On macOS/Apple Silicon `CMAKE_SYSTEM_PROCESSOR` is `arm64`, which matches `"arm"` — so
**Darwin arm64 gets `CORO_ASM`**. `sjlj` is the fallback for everything exotic, never for
aarch64. `tarantool@CMakeLists.txt:760-765` explains why the switch is set at top level:
*"Since coro.h is included universally, define the underlying implementation switch in the
top level CMakeLists.txt, to ensure a consistent header file layout across the entire project."*
That is a real hazard for a vendor — the backend macro changes `coro_context`'s layout, so it
must be defined identically in every TU.

**The asm.** `coro.c:287-315` is the aarch64 `coro_transfer`: 20 saved slots
(`#define NUM_SAVED 20` at `coro.c:289`), `x19`-`x30` and `d8`-`d15` via `stp`/`ldp`, `sp`
stashed in the context. Darwin is handled explicitly — `coro.c:164-170` emits `_coro_transfer`
with the Mach-O underscore prefix under `#if _WIN32 || __CYGWIN__ || __APPLE__`, and
`coro.c:324-331` does the same for `coro_startup`, omitting the `.type ... %function` directive
Mach-O rejects.

aarch64 also uses a different creation path: because the return address lives in `lr` rather
than on the stack, `coro.c:510-511` pushes nothing, and `coro.c:531-534` seeds
`sp[0]=coro`(x19), `sp[1]=arg`(x20), `sp[11]=coro_startup`(lr). The `coro_startup`
trampoline (`coro.c:346-368`) is arm-only — `coro.c:87-90` sets `CORO_STARTUP 1` for
`__arm__ || __aarch64__` — and it exists to lift the entry parameters straight out of saved
registers instead of the two extra `coro_transfer` calls the x86 path needs.

**Provenance.** Upstream libcoro's changelog in `coro.h:46-82` ends at `2012-12-21`. The
aarch64 code is not in it. `gh api repos/tarantool/tarantool/commits?path=third_party/coro`
gives the real story — the last upstream sync was `fadf260e` (2015-01-21, *"update libcoro to
6.41 version"*), and everything arm since is Tarantool's:

| commit | date | subject |
|---|---|---|
| `ebac0b8c` | 2026-05-06 | coro: build fix on recent gcc |
| `f34fbff4` | 2026-02-02 | coro: do not clear rbp in coro_init |
| `215630e6` | 2022-11-14 | coro: update sp before saving registers to a stack frame |
| `761053f0` | 2022-02-09 | coro: fix `coro_{init,startup}` unwind information |
| **`7e722763`** | **2021-05-11** | **libcoro: fix build for Apple ARM M1** |
| **`8e17ce15`** | **2015-12-04** | **coro_transfer arm64** |
| **`98d994c3`** | **2015-12-03** | **libcoro: add fast coroutine switching for arm** |

`7e722763`'s message is the clearest statement of what Apple Silicon actually costs:
*"Mach-O assembler does not support .type directive... according to OS X calling convention
symbols... are mangled with underscore ("_") prefix: Undefined symbols for architecture arm64:
"_coro_startup", referenced from: _coro_create"*.

**CI evidence.** `tarantool@.github/workflows/osx.yml:19-29` runs a nightly matrix of
`osx-version: [13, 14]` x `machine-arch: [x86_64, aarch64]` x `build-type: [debug, release]`
on self-hosted macOS runners. Darwin arm64 is a tested platform, not an aspiration.

**Measured here.** Extracting `coro.c` + `coro.h`, stubbing `trivia/config.h`, and building a
switch harness on this machine:

```
clang -std=gnu11 -O1 -g -DCORO_ASM -I. -Istub probe.c coro.c -o coro_plain
→ stack sptr=0x107410000 ssze=2097152
  steps=5 OK
```

Two notes from that run. `-std=c99` **fails** — `coro.c:162` uses the bare `asm` keyword,
which needs `-std=gnu*`. And `coro_stack_alloc(&st, 256*1024)` returned a **2 MB** stack,
because `coro.c:802` computes `size * sizeof(void *)` — **the size argument is in machine
words, not bytes.** An easy 8x footgun.

---

## 3. Extractability of `fiber.c`

### 3.1 Coupling

`src/lib/core/fiber.h` is 1,332 lines and pulls in eleven Tarantool headers before it declares
anything (`fiber.h:33-50`): `trivia/config.h`, `tt_pthread.h`, `tarantool_ev.h` (libev),
`cord_on_demand.h`, `diag.h`, `trivia/util.h`, `small/mempool.h`, `small/region.h`,
`small/rlist.h`, `salad/stailq.h`, `clock_lowres.h`, `backtrace.h`, `exception.h`, and finally
`<coro/coro.h>`. `fiber.c` adds `assoc.h`, `memory.h`, `trigger.h`, `errinj.h`, `clock.h`,
`tt_sigaction.h`, `tt_static.h` (`fiber.c:42-48`) plus `<valgrind/memcheck.h>` (`:153`) and
`<sanitizer/asan_interface.h>` (`:158`).

Symbol-level, counted with `grep -oE ... | sort -u` over `fiber.c`:

| subsystem | distinct symbols used | notes |
|---|---|---|
| **libev** | **32** — `ev_async{,_init,_send,_start,_stop}`, `ev_break`, `ev_check*`, `ev_default_loop`, `ev_feed_event`, `ev_idle*`, `ev_loop{,_destroy,_new}`, `ev_monotonic_now`, `ev_now`, `ev_prepare*`, `ev_run`, `ev_timer*`, `ev_tstamp`, `ev_watcher` | the scheduler *is* libev |
| `small` slab/region/mempool | 18 — `slab_cache{,_create,_destroy,_set_thread}`, `slab_data`, `slab_put`, `slab_sizeof`, `xslab_get`, `region_*` (9), `mempool_create`, `mempool_free`, `xmempool_alloc` | stacks come from the slab cache |
| `rlist` | 10 | intrusive lists, header-only |
| `diag` | 8 — `diag_{clear,create,destroy,get,is_empty,last_error,move,set}` | error channel |
| `clock` | 6 real (`clock_monotonic64`, `clock_thread64`, `clock_lowres_*`, …) | per-fiber cpu accounting |
| `trigger` | 7 — `trigger_{add,cb,clear,destroy,free_in_thread,init_in_thread,run}` | on_yield/on_stop/on_destroy hooks |
| `mh_i64ptr` (khash) | 8 | the fid → fiber registry |
| `backtrace` | 6 | `ENABLE_BACKTRACE` only |
| `say` | 2 — `say_error`, `say_syserror` | |
| **cbus** | **0** | one comment at `fiber.c:792`. `cbus.c` depends on `fiber.h`, not the reverse. |

The suspicion in the ticket that cbus is a coupling is **wrong in the right direction** —
cbus sits above fiber, so it is not in the way.

### 3.2 LOC split

`fiber.c` is 2,359 lines. Stack management, by contiguous range:

| range | what | lines |
|---|---|---|
| `153-181` | valgrind include, `fiber_invoke`, the ASan switch macros | 29 |
| `184-284` | `fiber_madvise`, `fiber_mprotect`, `signal_stack_init/free` | 101 |
| `324-336` | `page_size`, `stack_direction`, `FIBER_STACK_SIZE_*` | 13 |
| `344-364` | the poison pool | 21 |
| `1269-1294` | `page_align_down/up`, `fiber_madvise_unaligned` | 26 |
| `1296-1419` | watermark create / check / recycle (+ the `#else` stubs) | 124 |
| `1421-1517` | `fiber_stack_destroy`, `fiber_stack_create` | 97 |
| | **total** | **411 (17.4%)** |

The remaining ~1,950 lines are scheduler (`fiber_call`, `fiber_yield`, `fiber_wakeup`,
`fiber_schedule_*`, the libev prepare/check/idle watchers), cord thread lifecycle
(`cord_create/start/join/cojoin/destroy`, ~350 lines), fiber lifecycle and the `region` GC-leak
checker, cancellation and join, and `fiber.top()` introspection. **None of it is what lesh
wants**, because lesh's `poll(2)` loop already is the scheduler and ADR-0009 already fixes the
thread topology.

**The libcoro API surface consumed is three lines:**

- `fiber.c:1561-1562` — `coro_create(&fiber->ctx, fiber_loop, NULL, fiber->stack, fiber->stack_size);`
- `fiber.c:508` — `coro_transfer(&caller->ctx, &callee->ctx);` (in `fiber_call_impl`)
- `fiber.c:856` — `coro_transfer(&caller->ctx, &callee->ctx);` (in `fiber_yield_impl`)

plus `coro_context ctx;` in `struct fiber` (`fiber.h:626`). Note that **Tarantool does not use
`coro_stack_alloc` at all** — every guard page, madvise and watermark below is theirs.

### 3.3 ASan annotations

`fiber.c:157-181`. The macro pair is thin and correct, and the `will_switch_back` parameter is
the part worth stealing:

```c
#define ASAN_START_SWITCH_FIBER(fake_stack_save, will_switch_back, bottom, size)   \
    /* When leaving a fiber definitely, NULL must be passed as the first           \
     * argument so that the fake stack is destroyed. */                            \
    void *fake_stack_save = NULL;                                                  \
    __sanitizer_start_switch_fiber((will_switch_back) ? &fake_stack_save : NULL,   \
                                   (bottom), (size))
```

`ASAN_FINISH_SWITCH_FIBER` branches on `ASAN_INTERFACE_OLD` for the 1-arg vs 3-arg form
(`fiber.c:170-176`). Call sites: `fiber.c:505-509` (`fiber_call_impl`), `fiber.c:854-857`
(`fiber_yield_impl`, passing `will_switch_back=false` from `fiber_yield_final` — the last
switch out of a dead fiber, where the fake stack must be destroyed), and `fiber.c:1149`
(`ASAN_FINISH_SWITCH_FIBER(NULL)` as the first statement of `fiber_loop`, closing the switch
that started the fiber). The scheduler fiber runs on the OS thread stack, whose extents are
recorded **only under ASan** via `pthread_attr_getstack` at `fiber.c:1860-1867`, so the macro
has real bounds to hand the sanitizer.

**Tarantool's coro copy itself contains zero sanitizer support** — `grep -ci sanitiz coro.c
coro.h` returns `0` and `0`. Every annotation is in `fiber.c`. A vendor of `coro.c` writes
them from scratch.

### 3.4 Guard pages, and what ASan does to them

`fiber_stack_create` at `fiber.c:1469-1517`. The stack is a slab from the cord's `small` cache
(`xslab_get`, `:1474`), and the guard page is carved out of it by hand, at whichever end the
stack grows toward (`:1477-1498`), with the direction probed at runtime
(`check_stack_direction`, `fiber.c:2229-2232`, `__builtin_frame_address(0) < prev ? -1 : 1`).

Then, decisively for lesh:

```c
#ifndef ENABLE_ASAN
	fiber->has_guard = fiber_mprotect(guard, page_size, PROT_NONE) == 0;
	if (!fiber->has_guard)
		say_syserror("can't create guard page");
#else
	/*
	 * If we panic then fiber stacks remain protected which cause leak
	 * sanitizer failures. Disable memory protection under ASAN.
	 */
	fiber->has_guard = false;
#endif
```
— `fiber.c:1504-1514`.

**Under ASan, Tarantool has no stack guard pages.** They traded overflow detection for a clean
LeakSanitizer run. lesh does not have to make the same trade — Tarantool's problem is that a
`PROT_NONE` page inside a *slab-cache* block confuses LSan's scan of that block; an
independently `mmap`'d stack with a guard outside the reported region does not have that
problem. But it is a landmine worth knowing about before copying the pattern verbatim.
`fiber_stack_destroy` (`fiber.c:1421-1467`) has the matching horror: if restoring
`PROT_READ|PROT_WRITE` fails it deliberately **leaks the slab** and calls
`LSAN_IGNORE_OBJECT(fiber->stack_slab)` (`:1462`) rather than return a `PROT_NONE` page to the
pool.

### 3.5 madvise / watermarks

`fiber.c:1296-1419`, all behind `#ifdef HAVE_MADV_DONTNEED` (probed at
`tarantool@CMakeLists.txt:176`, `check_symbol_exists(MADV_DONTNEED sys/mman.h ...)`).

The scheme: at creation, `MADV_DONTNEED` the whole stack, then write eight 64-bit poison words
(`poison_pool`, `fiber.c:349-354`) spaced 128 bytes apart (`POISON_OFF`, `:362`) at a
**randomly offset** watermark 64 KB (`FIBER_STACK_SIZE_WATERMARK`, `:335`) from the far end —
`fiber_stack_watermark_create`, `:1370-1404`. The randomization comment at `:1388-1391` is
explicit: *"To increase probability of stack overflow detection we put the first mark at a
random position."*

On recycle, `fiber_stack_recycle` (`:1338-1365`) checks the watermark and, **only if it has
been overwritten**, calls `madvise(MADV_DONTNEED)` on everything past it — *"To avoid a
pointless syscall invocation in case the fiber hasn't touched memory above the watermark"*.
Watermarking is skipped entirely for `FIBER_CUSTOM_STACK` fibers (`:1376-1378`), and such
fibers are also not reusable (`fiber_is_reusable`, `fiber.c:469-473`).

This is a pooled-fiber RSS optimization. For lesh, where fiber counts will be in the tens, not
the tens of thousands, **it is the least valuable part of the pattern.** The guard page is the
valuable part.

### 3.6 Stack sizing

`tarantool@cmake/SetFiberStackSize.cmake` is the whole story, and it is short enough to quote
the decisive part (`:11-18`):

```cmake
if (ENABLE_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(FIBER_STACK_SIZE "1Mb" CACHE STRING "Fiber stack size")
    set(FIBER_STACK_SIZE_IN_BYTES_MIN 1048576)
else()
    set(FIBER_STACK_SIZE "512Kb" CACHE STRING "Fiber stack size")
    set(FIBER_STACK_SIZE_IN_BYTES_MIN 524288)
endif()
```

**512 KB default; 1 MB under Clang + ASan + Debug** — exactly lesh's debug configuration.
Rounded up to a 4 KB multiple (`:36-38`), propagated as `-DFIBER_STACK_SIZE_DEFAULT`
(`:50`), and `fiber.c:327-329` refuses to compile without it. `FIBER_STACK_SIZE_MINIMAL` is
16,384 (`fiber.c:333`), enforced at `fiber.c:394-398`. The header cites
`tarantool/tarantool#3418` and `tarantool/security#153` as the rationale for the floor.

**There is no Lua-specific stack size.** A Lua fiber is created with plain
`fiber_new("lua", lua_fiber_run_f)` at `src/lua/fiber.c:477`, taking `fiber_attr_default`.
The 512 KB *is* the LuaJIT number — it is what a production database found sufficient for
interpreter frames, JIT-compiled trace frames, C functions and FFI callbacks on one stack.

### 3.7 Verdict on question 3

**The coro core plus the stack-management *pattern* is liftable; `fiber.c` is not, and does not
need to be.** Concretely, what lesh would reimplement from §3.3-§3.6 is on the order of 150
lines: the ASan macro pair with `will_switch_back`, an `mmap` + `mprotect(PROT_NONE)` stack
allocator with a runtime direction probe, and a stack-size constant that is 512 KB / 1 MB
rather than anybody's default. The scheduler stays lesh's `poll(2)` loop. The suspicion in the
ticket is confirmed on both halves.

---

## 4. The LuaJIT pairing, and the correction it forces

**The mental model in issue #145 — "a `lua_newthread` coroutine married to each C fiber,
switched together" — is wrong in its second half, and the error matters.** Tarantool never
switches the Lua coroutine. `lua_resume` and `lua_yield` do not appear in `src/lua/fiber.c`,
`src/lua/utils.c` or `src/lua/init.c` at all. `lua_newthread` is used purely as a **value-stack
allocator**; all switching is a C-stack swap underneath a still-running `lua_pcall`.

### 4.1 The structure

`struct fiber` carries the Lua handle in its fiber-local storage — `fiber.h:728-765`. Note
this is a plain struct, not a union; the `lua` sub-struct is `fiber.h:740-758`:

```c
struct {
        struct lua_State *stack;   /* fiber.h:748 */
        int fid_ref;               /* fiber.h:753 */
        int storage_ref;           /* fiber.h:757 */
} lua;
```

Core deliberately does not include Lua headers: `fiber.h:60` defines
`#define FIBER_LUA_NOREF (-2)` and `src/lua/fiber.c:43` asserts
`static_assert(FIBER_LUA_NOREF == LUA_NOREF, ...)`. **That trick is directly reusable in lesh**
— it is how `src/lib/core` stays Lua-agnostic while carrying a Lua slot.

Creation, `src/lua/fiber.c:469-502`:

- `:472` `lua_State *child_L = luaT_newthread(L);` — a coroutine of the *main* state, sharing
  its heap, registry, globals and string table.
- `:475` `int coro_ref = luaL_ref(L, LUA_REGISTRYINDEX);` — the GC anchor. The only handle.
- `:477` `struct fiber *f = fiber_new("lua", lua_fiber_run_f);` — a plain C fiber, default attrs.
- `:492` `lua_xmove(L, child_L, lua_gettop(L));` — function and arguments onto the child's stack.
- `:500` `lua_pushinteger(child_L, coro_ref);` — the registry ref rides on the child's own stack.
- `:501` `f->storage.lua.stack = child_L;`

`luaT_newthread` (`src/lua/utils.c:1021-1031`) is a protected wrapper so an OOM inside
`lua_newthread` returns `NULL` instead of throwing.

There are **three registry anchors with three lifetimes**: `coro_ref` (released at
`src/lua/fiber.c:461` for non-joinable fibers, `:909` in `fiber:join` for joinable ones),
`fid_ref` (released via an `on_destroy` trigger, `:115-116`/`:132-135`), and `storage_ref`
(released via an `on_stop` trigger, `:103-104`/`:677-680` — deliberately `on_stop` rather than
`on_destroy`, per the comment at `:90-97`, because a pooled non-Lua fiber can create a Lua
`fiber.storage` and then be reused without ever being destroyed). `fiber_recycle`
(`src/lib/core/fiber.c:1043-1077`) `memset`s the whole storage struct (`:1060`) and restores
both refs to `FIBER_LUA_NOREF` (`:1061-1062`), so a reused fiber never inherits a stale
coroutine. The Lua-visible fiber handle stores the **64-bit fid, not a pointer**
(`src/lua/fiber.c:139-141`), re-resolved through `fiber_find()` on every access
(`:155-169`) — because fibers are recycled and pointers go stale.

The fiber body is an ordinary C function that ignores its `va_list` —
`lua_fiber_run_f`, `src/lua/fiber.c:444-463`:

```c
struct lua_State *L = f->storage.lua.stack;
int coro_ref = lua_tointeger(L, -1);
lua_pop(L, 1);
result = luaT_call(L, lua_gettop(L) - 1, LUA_MULTRET);
```

`luaT_call` (`src/lua/utils.c:742-748`) is `lua_pcall` plus `luaT_toerror`.

### 4.2 Why the C boundary is not crossed

LuaJIT's `LJ_ERR_CYIELD` — *"attempt to yield across C-call boundary"* — fires when
`lua_yield` finds a C frame between the coroutine entry and the yield point. **Tarantool
never calls `lua_yield`.** Take `fiber.sleep(1)`:

1. `lbox_fiber_sleep` (`src/lua/fiber.c:711-722`) is a plain `lua_CFunction`; its C frame is on
   the fiber's own 512 KB libcoro stack.
2. → `fiber_sleep` (`src/lib/core/fiber.c:922-940`) arms an `ev_timer` and yields.
3. → `fiber_yield_impl` (`fiber.c:822-857`) reaches `coro_transfer(&caller->ctx, &callee->ctx)`
   at `fiber.c:856`.

The entire C stack — the `lua_pcall` frame, LuaJIT's `lj_vm_pcall` cframe with its
`SAVE_L`/`SAVE_CFRAME`/`SAVE_ERRF` slots, `lbox_fiber_sleep`'s frame, `fiber_sleep`'s frame —
is **left sitting there untouched** while `sp` moves to another stack. Nothing unwinds.
`L->cframe` still points at perfectly valid memory that simply is not the current stack. On
resume, `sp` comes back and the `lua_pcall` continues.

**That is the whole trick, and it is exactly what #145 wants.** LuaJIT's restriction is about
*unwinding* a C stack it does not own; swapping stacks never unwinds.

The invariant it buys: **two fibers may never share one `lua_State`**, because a state's cframe
chain is threaded through one specific C stack. `fiber.h:742-748` says so — *"Should not be
used in other fibers."* Hence one coroutine per fiber.

Cancellation rides the same rails: `fiber_cancel` (`fiber.c:616-634`) only sets a flag and
wakes; the Lua side throws from `luaL_testcancel` (`src/lua/fiber.c:45-52`) at the first check
point after resumption, and `lua_error`'s longjmp unwinds **within the fiber's own stack** back
to its own `lua_pcall` at `src/lua/fiber.c:452`. Per-fiber C stacks are what make that
well-defined.

### 4.3 The price: `cord_on_yield`, and the JIT is *not* indifferent

Issue #145 asserts *"the JIT is indifferent to which stack a `lua_State` runs on, but not to
how big it is"*. **The first half is false, and Tarantool pays for it with a hard `exit()`.**

`cord_on_yield` is declared as a bare `extern void cord_on_yield(void);` in core
(`src/lib/core/fiber.c:50`) so `fiber.c` need not know Lua exists, implemented in
`src/lua/utils.c:1161-1220`, and called on **every** switch (`fiber.c:524` in `fiber_call`,
`fiber.c:834` in `fiber_yield_impl`). It does three things:

- **`src/lua/utils.c:1173`** — `if (unlikely(tvref(g->jit_base)))` → **panic and
  `exit(EXIT_FAILURE)`**. A non-NULL `jit_base` means the yield happened *inside JIT-compiled
  machine code*, where a trace holds live values in registers and an mcode frame the switch
  would strand. The message names the exact hazard: *"fiber %llu is switched while running the
  compiled code (it's likely a function with a yield underneath called via LuaJIT FFI)"*. This
  is the one case a C-stack swap genuinely cannot survive, and Tarantool chooses to die loudly.
- **`src/lua/utils.c:1195`** — `lj_trace_abort(g);` unconditionally. Any trace *recording* in
  progress is discarded, because a recorder that saw two fibers interleaved would compile
  garbage. **Standing tax: yields defeat trace formation.**
- **`src/lua/utils.c:1208`** — panic if `g->hookmask & HOOK_GC`; yielding inside a `__gc`
  finalizer would peg the collector's penalty threshold for arbitrarily long.

For lesh this is close to free, because **ADR-0006 already sets `jit.off()` as the default**.
With the JIT off, `jit_base` is never set and there is no trace to abort. But it means the
`jit.on()` pragma ADR-0006 offers and a `lesh.await(...)` that yields the fiber are
**mutually exclusive on the same call path**, and the design has to say so. That is a new
constraint on ADR-0006 that #145 did not anticipate.

### 4.4 What the LuaJIT fork changes: essentially nothing, for fibers

Two corrections here.

**There is no `exdata` in Tarantool's LuaJIT.** `grep -rn exdata src/` in the fork returns
nothing, and neither `lua_setexdata` nor `lua_getexdata` appears anywhere in Tarantool's
`src/lua` or `src/lib/core`. `exdata` is an **OpenResty/Cloudflare** LuaJIT extension. The
premise in the research brief is misattributed.

**The fork contains no fiber-specific VM changes.** Diffed against upstream `LuaJIT/LuaJIT@v2.1`:
`lj_err.c`, `lj_state.c`, `lj_frame.h`, `lj_api.c`, `lj_obj.h`, `lj_dispatch.c`, `lib_base.c`,
`lj_arch.h`. Every fiber-relevant mechanism — `cur_L`, `jit_base`, `lj_trace_abort`, `cframe`,
`lj_vm_cpcall` — is **stock LuaJIT**. The cframe size *does* differ (x64 `CFRAME_SIZE` 10\*8 →
12\*8) but the cause traces to a new `SAVE_VMSTATE` slot in `vm_x86.dasc:202,318,491-522`, which
is memprof/sysprof work, not fiber support. `LUAJIT_UNWIND_EXTERNAL` is a stock knob
(`lj_arch.h:636-642`) that Tarantool's CMake never sets.

What the fork *does* add: `lua_hash`/`lua_hashstring` (`lj_api.c:540-547`),
`LUAJIT_SMART_STRINGS`, `LUAJIT_ENABLE_CHECKHOOK`, memprof/sysprof/`lmisclib.h`, and a CMake
build system. None of it is why fibers work.

**The actual integration mechanism is that Tarantool includes LuaJIT's private headers.**
`cmake/luajit.cmake:154` does `include_directories(${LUAJIT_SOURCE_ROOT}/src)` so Tarantool can
`#include <lj_obj.h>`, `<lj_state.h>`, `<lj_ctype.h>`, `<lj_cdata.h>` (`src/lua/utils.h:47-50`)
and `"lj_trace.h"` (`src/lua/utils.c:44`). `cord_on_yield` reaching into `g->jit_base`,
`g->cur_L` and `g->hookmask` is not a patched API — it is reaching directly into LuaJIT's
private structs from outside. The file's own comment (`cmake/luajit.cmake:145-148`) is candid:
*"Since Tarantool use LuaJIT internals to implement all required interfaces, several defines
and flags need to be set for Tarantool too. FIXME: Hope everything below will have gone in a
future."*

**What lesh actually needs from all this** is small and does not require a LuaJIT fork:

1. One `lua_State` (coroutine of the main state) per fiber, anchored by a registry ref.
2. `lua_xmove` in, `lua_xmove` out; the fiber body is a C function running `lua_pcall`.
3. A `cord_on_yield`-equivalent hook on every switch that, at minimum, calls `lj_trace_abort`
   and refuses to switch when `jit_base` is set. With `jit.off()` default this is a cheap
   assertion, but it must exist before any `jit.on()` pragma ships.
4. Accept that reaching `g->jit_base` means including `lj_obj.h`, a private header. That is a
   real coupling to a vendored LuaJIT's internals and should be an explicit decision, not a
   discovery.

---

## 5. Head-to-head: minicoro vs Tarantool's coro

### 5.1 Facts, cited

| | **minicoro** `02dad0f8` | **Tarantool coro** `990019b3` |
|---|---|---|
| **License** | Unlicense **or** MIT-0, recipient's choice — `LICENSE:1-3`, `:5`, `:30-32`. No attribution required. | BSD-2 **or** GPLv2+ — `coro.h:2`, `:25-34`. Attribution required. `conftest.c` is GPL (`README:2`) and must be excluded. |
| **aarch64 asm** | Yes — `minicoro.h:1170-1251`. Auto-selected at `:392-397` (`__aarch64__` → `MCO_USE_ASM`). Apple handled at `:1184-1192` and `:1227-1235` (`__mco_switch` / `__mco_wrap_main`, `.type`/`.size` omitted). | Yes — `coro.c:287-315` (transfer), `:346-368` (startup), `:531-534` (create). Apple handled at `:164-170`, `:324-331`, `:364-368`. **Backend must be forced from the build system** (`cmake/BuildLibCORO.cmake:14-15`); the library's own auto-selection (`coro.h:298-315`) picks ucontext on Darwin arm64. |
| **ASan** | **Auto-detected and always on** — `:532-542` via `__has_feature(address_sanitizer)` / `__SANITIZE_ADDRESS__` → internal `_MCO_USE_ASAN`; prototypes at `:546-549`; annotations at `:577-585` (`_mco_prepare_jumpin`) and `:602-610` (`_mco_prepare_jumpout`). | **None.** `grep -ci sanitiz coro.c coro.h` → `0`, `0`. Tarantool's annotations live in `fiber.c:157-181` and would have to be rewritten by a vendor. |
| **TSan** | Yes — `:550-555`, `:586-589`, `:611-615` (`__tsan_{create,destroy,switch_to}_fiber`). | None. |
| **Valgrind** | Opt-in `MCO_USE_VALGRIND` — `:1347-1348`, `:1359-1364`. | Opt-in `CORO_USE_VALGRIND` — `coro.c:839-841`. |
| **Guard pages** | **None. `mprotect` occurs 0 times in the file.** Stack is the tail of one block: `co` \| `context` \| `storage` \| `stack` (`:1330-1341`, sizes at `:1370-1375`). Overflow eats `storage` then `co`, caught heuristically by `MCO_MAGIC_NUMBER` at the next `mco_yield` (`:1822-1829`) — the comment says *"This check happens when the stack overflow already happened, but better later than never."* | `coro_stack_alloc` has guard-page code (`coro.c:828-830`) but **`coro.c:768-770` `#undef`s `CORO_GUARDPAGES` on any arch that is not i386/x86_64/powerpc/m68k/alpha/mips/sparc64 — i.e. aarch64 gets none.** It also maps `PROT_READ\|PROT_WRITE\|PROT_EXEC` by default (`:816`). Moot in practice: **Tarantool never calls it**, allocating from its own slab cache and mprotecting by hand (`fiber.c:1469-1517`). |
| **Stack allocation** | `calloc` by default (`:517`), or `mmap`/`VirtualAlloc` with `MCO_USE_VMEM_ALLOCATOR` (`:499-511`, `:487-498`). Pluggable via `mco_desc.alloc_cb`/`dealloc_cb` + `coro_size`. | `mmap` with lazy commit (`coro.c:816`), falling back to non-exec then `malloc`. **Size argument is in machine words, not bytes** (`:802`) — measured: asked 256 KB, got 2 MB. |
| **Default stack** | **56 KB** (`:370`), or 2,040 KB with VMEM (`:368`). Min 32 KB (`:361`). | libcoro default 256 K *words* = 2 MB (`:798-799`). Tarantool overrides: **512 KB / 1 MB under ASan**, min 16 KB (`SetFiberStackSize.cmake:11-18`, `fiber.c:333`). |
| **Size** | 2,033 raw lines / 72,526 bytes, single header. **465 own preprocessed LOC** on arm64; **1,768 B `__text` + 239 B `__cstring`** at `-O2 -DNDEBUG -DMCO_NO_DEBUG -DMCO_NO_MULTITHREAD`. | 1,288 raw lines across two files. **106 own preprocessed LOC** with `-DCORO_ASM` on arm64; **564 B `__text`** at `-O2 -DNDEBUG`. |
| **Maintenance** | Last **functional** commit `ff5321d9`, 2023-11-15 ("growable stacks with the virtual memory backed allocator"). Last commit at all `02dad0f8`, 2024-12-07, a FUNDING.yml edit. 979 stars, 8 open issues, not archived. **No CI workflows in the repo.** | Actively maintained as part of Tarantool; last touch `ebac0b8c`, 2026-05-06. Nightly macOS 13/14 x86_64+aarch64 CI (`.github/workflows/osx.yml:19-29`) and `debug_aarch64.yml`. |

### 5.2 Measured on this machine, today

**minicoro under lesh's exact sanitizer flags — green.**

```
clang -std=c99 -O1 -g -fno-omit-frame-pointer \
      -fsanitize=address,undefined -fsanitize=leak -I. tests/testsuite.c -o ts_asan
ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1 ./ts_asan
→ Test suite succeeded!   EXIT=0
```

`nm ts_asan | grep switch_fiber` confirms `___sanitizer_start_switch_fiber` and
`___sanitizer_finish_switch_fiber` are genuinely linked, so the annotations are compiled in,
not compiled out.

**Tarantool's coro also builds and switches correctly on Darwin arm64** (§2), and — worth
saying, since it is the opposite of what one might assume — a 200-round stress harness with
16 KB frames escaping across switches, deep recursion on both stacks, and
`detect_stack_use_after_return=1` **passed unannotated**. Unannotated switching does not
automatically blow up; the annotations prevent a class of false positives that a toy will not
provoke. Do not use "it didn't crash" as evidence either way.

**LSan and suspended fiber stacks.** A heap block whose only reference lives in a *suspended*
fiber's stack frame was **not** reported at exit (verified with a control that does report a
real 500 KB leak, so LSan is genuinely active under Homebrew clang on Darwin arm64). Darwin's
LSan root set evidently covers the `mmap`'d stack. **This was not verified on Linux, where
LSan's root set is more precise, and it is the single most likely way fibers break the gate on
CI.** Put it in the probe: allocate in a fiber, suspend it holding the only reference, exit,
and assert no leak — on Linux x86_64 and aarch64 both. If it fails, the fix is
`__lsan_register_root_region` over each live fiber stack, which neither library does today.

**Switch cost**, 20,000,000 round-trips, `-O2`, three runs each, ±0.1 ns:

| | ns / round-trip |
|---|---|
| minicoro, `-DMCO_NO_DEBUG -DMCO_NO_MULTITHREAD` | **5.4** |
| Tarantool coro, bare `coro_transfer` | 12.2 |
| minicoro, default (debug asserts + TLS current-coro) | 17.3 |

minicoro is **2.2x faster while doing strictly more work** (state machine, `prev_co` chain,
stack-overflow check at every yield). The difference is in the asm: libcoro's aarch64
`coro_transfer` pushes 20 slots onto the coroutine's own stack (`coro.c:290-314`), while
minicoro stores directly into a small `_mco_ctxbuf` and interleaves its `stp`/`ldp` pairs
(`minicoro.h:1194-1219`). Per CLAUDE.md this is a same-environment before/after, quoted as a
delta on one machine — not a portable constant.

### 5.3 Verdict for lesh

**Vendor minicoro. Replace its allocator. Do not vendor Tarantool's coro, and do not consider
upstream libcoro.**

**Upstream libcoro is out on the gate.** It has no aarch64 backend at any version — the
changelog ends 2012-12-21 (`coro.h:81`) and the arm64 asm in Tarantool's tree was written by
Tarantool in 2015 and fixed for Apple Silicon in 2021 (§2). Its auto-selection would hand the
dev machine `ucontext`, which means a `sigprocmask` syscall on every switch. Dead on arrival.

**minicoro wins on four of lesh's five constraints and ties on the fifth:**

- *Sanitized gate is sacred* — minicoro ships correct ASan annotations that turn on
  automatically under `-fsanitize=address`, plus TSan annotations that ADR-0009's two-thread
  topology makes non-hypothetical. Tarantool's coro ships **none**; choosing it means writing
  `fiber.c:157-181` ourselves and owning its correctness under a gate we have declared sacred.
  This is the decisive difference.
- *Darwin arm64 is the dev machine* — both work; minicoro needs no build-system incantation to
  select the backend, where Tarantool's coro requires `-DCORO_ASM` set identically in every TU
  or `coro_context`'s layout silently changes (`CMakeLists.txt:760-765`).
- *Linux x86_64/aarch64 matter* — both cover both.
- *ADR-0005 permits vendored-static* — minicoro is one self-contained header with no includes
  beyond libc; Tarantool's coro now needs a `trivia/config.h` stub and `-std=gnu*`.
- *Licensing* — MIT-0/Unlicense requires no attribution at all; BSD-2 does, plus a `conftest.c`
  exclusion. Both clean; minicoro cleaner.

**The two things minicoro does not give us, and the answer to each:**

1. **No guard pages.** Non-negotiable with LuaJIT on the stack — a JIT frame overrunning 56 KB
   would silently corrupt the `mco_coro` struct. **Fix:** supply `mco_desc.alloc_cb`/`dealloc_cb`
   that `mmap` `coro_size` rounded up plus two pages and `mprotect(PROT_NONE)` a page below
   where `_mco_create_context` (`minicoro.h:1330-1341`) will place `stack_base`. The layout is
   fixed and documented (`co` \| `context` \| `storage` \| `stack`, sizes at `:1370-1375`), so
   the arithmetic is stable — but it is arithmetic against an internal invariant, so pin the
   version and add a test that writes past the stack and expects SIGSEGV, not corruption.
   A ~20-line patch to `_mco_create_context` that aligns `stack_addr` to a page and mprotects
   the page below it is the more honest alternative; we vendor the file, so we may patch it.
   **Either way, do not ship the default allocator.**
2. **56 KB default stack.** Set it to **512 KB, and 1 MB in the Debug/ASan preset.** Those are
   Tarantool's numbers (`SetFiberStackSize.cmake:11-18`) from a decade of running LuaJIT on
   fibers in production, and #145's instinct that "8-64 KiB defaults" are wrong is confirmed —
   minicoro's default is exactly the 56 KB the ticket warned about.

**On minicoro's staleness.** Last functional commit 2023-11-15 — 2 years 9 months. This is the
strongest argument against it and #145 does not mention it. It cuts less than usual: it is a
single dependency-free header solving a frozen problem (aarch64 and x86-64 calling conventions
do not move), we vendor and own it under ADR-0005, and the parts we most depend on (the asm,
the ASan annotations) are the parts least likely to need changes. But it means **no upstream
will fix it** — budget for owning it, and note there is **no CI in that repo**, so its
platform matrix is a claim, not a test result. Our gate becomes its gate.

**Keep Tarantool's coro as the documented fallback.** If the guard-page patch against
minicoro's internal layout proves too fragile to maintain, `coro.c`'s 106 live lines plus a
hand-written stack allocator modelled on `fiber.c:1469-1517` is a working plan B — it is what
Tarantool actually does, and it has nightly aarch64 CI behind it. The cost is writing the ASan
annotations ourselves.

**Probe prescription (revising #145's).** Unchanged: minicoro first, one fiber-shaped reactor
behind the existing ABI, a LuaJIT script suspended mid-C-call. Added, in priority order:

1. **The custom stack allocator with a guard page, before anything else** — with a test that
   overruns and expects SIGSEGV. Everything else is downstream of this.
2. **512 KB / 1 MB stack sizes from the start.** Do not measure anything on 56 KB.
3. **The LSan-versus-suspended-fiber-stack test on Linux x86_64 and aarch64** (§5.2). This is
   the likeliest way fibers break the gate on CI and it is cheap to run early.
4. **A `cord_on_yield` equivalent** that aborts trace recording and refuses to switch when
   `jit_base` is set (§4.3), even though `jit.off()` is ADR-0006's default. It costs ten lines
   and it is the difference between a diagnosable panic and silent corruption the day someone
   writes `jit.on()`.

---

## 6. fork / `posix_spawn`

`src/lib/core/fiber.c` contains **no** `fork`, `vfork`, `posix_spawn`, `execve` or
`pthread_atfork` — grep returns nothing. Fibers are single-threaded cooperative constructs and
Tarantool installs no atfork handlers anywhere in the tree.

All spawning is in `src/lib/core/popen.c`, and it uses `vfork` for a reason that is **not**
about fibers — `popen.c:1334-1342`:

> *"We have to use vfork here because libev has own at_fork helpers with mutex, so we will have
> double lock here and stuck forever otherwise."*

`handle->pid = vfork();` at `popen.c:1351`, wrapped in a deprecation pragma
(`:1344-1352`) with a TODO to move to `posix_spawn` on macOS (gh-6674, `:1346-1349`).
`posix_spawn` is **never actually called** — the only occurrence in the file is that comment.
The child does the minimum before `execve` (`popen.c:1496`): `log_set_fd` (`:1379`),
`signal_reset` (`:1388`), `setsid`/`setpgrp` (`:1390-1411`), `close_inherited_fds` (`:1414`).
`ev_loop_fork` is deliberately not called (`:1357-1366`). Locals are `volatile` against vfork
clobber (`:1208-1213`). macOS gets a specific workaround because its `vfork` behaves like
`fork` and does not suspend the parent (`popen.c:1079-1084`), so the parent spins in
`popen_wait_group_leadership` (`:1515-1519`) until the child is a group leader or `killpg`
races with `ESRCH`; and since Darwin disallows `setsid()` after vfork, the child uses
`ioctl(TIOCNOTTY)` + `setpgrp()` instead (`:1396-1410`).

**For lesh the conclusion is favourable but for a different reason than #145 states.** #145
says *"a fiber is memory, fork from one is safe, `posix_spawn` never touches the fiber stack"*.
The first and third clauses hold — a fiber stack is anonymous memory, copied or shared like any
other, and `posix_spawn` in the parent does not touch it. The second clause is what Tarantool's
evidence complicates: **they could not use plain `fork` at all**, and the blocker was libev's
`pthread_atfork` handlers taking a mutex. **lesh has no libev** — the loop is hand-rolled
`poll(2)` — so this specific hazard does not transfer. But the general form does: *anything
that registers an atfork handler makes forking from a fiber-bearing process a deadlock risk*,
and that is worth writing into the fiber design as a standing prohibition rather than
rediscovering.

One correction of record: the fork invariants are **not** in issue #143, which is about signal
dispositions (`signal_hub::take_dispositions`, `SIG_DFL`/`SIG_IGN`/chaining). Whatever ticket
holds the exec-mask/fork invariants, #143 is not it.

---

## 7. Where this contradicts issue #145's comments

Five items. Two change the design.

1. **"minicoro — built-in ASan fiber annotations (`MCO_USE_ASAN` → `__sanitizer_start_switch_fiber`)."**
   There is **no `MCO_USE_ASAN` option.** The macro is internal `_MCO_USE_ASAN`, set
   automatically from `__has_feature(address_sanitizer)` / `__SANITIZE_ADDRESS__`
   (`minicoro.h:532-542`). This is *better* than described — nothing to remember to define —
   but the named knob does not exist, and a probe that goes looking for it will conclude the
   feature is missing.

2. **"Tarantool vendors third_party/coro — Marc Lehmann's libcoro lineage."** True as far as it
   goes, but incomplete in the way that matters: **the aarch64 asm is Tarantool's own**, added
   in 2015 and fixed for Apple Silicon in 2021 (§2). Upstream libcoro has no aarch64 backend
   and its changelog stops in 2012. This removes "upstream libcoro" from the candidate list
   entirely, and it means Tarantool's copy is a nine-year-old fork carrying the only reason it
   runs on our dev machine.

3. **"a `lua_newthread` coroutine married to each C fiber, switched together."** ⚠️ **The
   coroutines are never switched.** `lua_resume` and `lua_yield` do not appear in Tarantool's
   fiber path at all. The `lua_State` is a value-stack allocator and a GC anchor; 100% of
   switching is the C-stack swap under a live `lua_pcall` (§4.2). The corrected model is
   simpler than the one in the ticket and should replace it, because "switched together"
   suggests a resume/yield pairing that would in fact reintroduce the C-boundary error.

4. **"the JIT is indifferent to *which* stack a `lua_State` runs on."** ⚠️ **False, and
   Tarantool `exit()`s over it.** `cord_on_yield` (`src/lua/utils.c:1161-1220`) panics if
   `g->jit_base` is set — a yield from inside compiled trace code — and unconditionally calls
   `lj_trace_abort` on every switch, so **yielding defeats trace formation as a standing tax**
   (§4.3). Under ADR-0006's `jit.off()` default this is nearly free, but it makes the
   `jit.on()` pragma and a yielding `lesh.await(...)` mutually exclusive on the same call path.
   That constraint belongs in ADR-0006 and is not there today.

5. **"the exec-mask/fork invariants from #143."** #143 is about signal dispositions, not fork.
   And Tarantool's fork story is `vfork`-because-libev-deadlocks (§6) — a hazard lesh's
   hand-rolled `poll(2)` loop does not inherit, so the conclusion survives, but not via the
   stated reasoning.

Two further items the ticket does not mention and a probe would hit blind:

6. **minicoro has no guard pages at all** — `mprotect` occurs zero times. With LuaJIT frames on
   a 56 KB default stack this is the single largest risk in the plan (§5.3).
7. **minicoro's last functional commit is 2023-11-15 and the repo has no CI.** Its platform
   matrix is a README claim, not a test result.

Unresearched, per the brief's scope: **boost.context's fcontext** and **libaco**. If the
minicoro guard-page patch proves unmaintainable, fcontext deserves the same treatment before
falling back to Tarantool's coro — it is the one candidate with a larger tested platform matrix
than either library here.

---

## Sources

All read as source, at the commits in the header table. No blog posts or secondary write-ups
were used.

**Tarantool** (`tarantool/tarantool@990019b3`)
- `third_party/coro/LICENSE`, `README`, `coro.h`, `coro.c`
- `cmake/BuildLibCORO.cmake`, `cmake/SetFiberStackSize.cmake`, `cmake/luajit.cmake`, `CMakeLists.txt`
- `src/lib/core/fiber.h`, `src/lib/core/fiber.c`, `src/lib/core/popen.c`
- `src/lua/fiber.c`, `src/lua/utils.c`, `src/lua/utils.h`, `src/lua/init.c`, `src/lua/error.c`, `src/lua/fiber_channel.c`
- `src/main.cc`, `LICENSE`, `.github/workflows/osx.yml`
- commit history of `third_party/coro` via `gh api repos/tarantool/tarantool/commits?path=third_party/coro`; individual commits `8e17ce15`, `7e722763`, `98d994c3`, `fadf260e`

**minicoro** (`edubart/minicoro@02dad0f8`)
- `minicoro.h`, `LICENSE`, `README.md`, `tests/testsuite.c`, `tests/Makefile`
- commit history via `gh api repos/edubart/minicoro/commits`

**LuaJIT** (`tarantool/luajit@712e6d85`, branch `tarantool-1.7`, diffed against `LuaJIT/LuaJIT@v2.1`)
- `src/lj_errmsg.h`, `src/lj_api.c`, `src/lj_frame.h`, `src/lj_state.c`, `src/lj_err.c`, `src/lj_arch.h`, `src/lj_obj.h`, `src/vm_x86.dasc`, `CMakeLists.txt`

**lesh**
- `docs/adr/0005-no-runtime-shared-library-dependencies.md`, `0006-luajit-as-the-extension-runtime.md`, `0009-two-owner-threads.md`
- `CMakeLists.txt`, `CMakePresets.json` (sanitizer flags the measurements matched)
- issues [#145](https://github.com/nanov/lesh/issues/145), [#143](https://github.com/nanov/lesh/issues/143)
