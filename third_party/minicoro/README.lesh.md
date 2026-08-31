# minicoro (vendored single-header stackful coroutine switcher)

Upstream: <https://github.com/edubart/minicoro>
Commit: **`02dad0f8b7cbb12fe6e216ae7a76db15ca55cd7b`** (short `02dad0f8`, 2024-12-07) —
the commit the research note built, ran and measured; see
`docs/superpowers/research/2026-08-26-stackful-fibers-tarantool-minicoro.md`.

Files fetched from that commit, raw, and committed here byte for byte:

| file | source | SHA-256 |
|---|---|---|
| `minicoro.h` | <https://raw.githubusercontent.com/edubart/minicoro/02dad0f8/minicoro.h> | `c4205e8db0a95456dfde9f73f071609c6d2cad2ebfd1d74ed0a9254f121caa2f` |
| `LICENSE` | <https://raw.githubusercontent.com/edubart/minicoro/02dad0f8/LICENSE> | `2b3ac34bfa39ff1f84b1d07e7dd6d317a1e767542229f6c48c43494dc2496553` |

Licence: **Unlicense OR MIT No Attribution**, the recipient's choice — the
upstream `LICENSE`, copied here verbatim. Neither alternative requires
attribution; this file is the record anyway, because a vendored tree that does
not say where its bytes came from is a tree nobody can re-vendor.

Segregated under `third_party/` because these bytes are Eduardo Bart's, not
lesh's.

## NOTHING HERE IS MODIFIED

`minicoro.h` is the published header, unpatched. Every choice lesh makes is made
from **our** side, in `src/fiber/`:

- the `MCO_*` configuration macros are defined in `src/fiber/minicoro.cpp`, the
  one translation unit that defines `MINICORO_IMPL` and includes this header;
- the stack allocator is ours, installed through minicoro's documented
  `mco_desc.alloc_cb` / `dealloc_cb` seam (`src/fiber/stack.cpp`).

The research note weighed patching `_mco_create_context` to page-align
`stack_addr` and `mprotect` below it, and called it "the more honest
alternative". We took the other road deliberately: an unpatched vendored file is
one that `curl | shasum` can verify against upstream forever, and the allocator
seam is a *documented* API where the internal layout is not. The price is that
`src/fiber/stack.cpp` does arithmetic against an internal invariant (the
`co | context | storage | stack` block layout), which is why the commit above is
**pinned** and why `stack.cpp` verifies its own arithmetic at every spawn and
aborts rather than placing a guard page somewhere harmless. See that file's
header comment.

## ADR-0005 is satisfied

One header, no build system of its own, no object code but what our own
translation unit emits, and no dependency beyond libc. It compiles into the
`lesh_fiber` static archive and disappears; there is no new runtime shared
library and no new toolchain dependency. A checkout builds with the compiler and
CMake it already had.

## What lesh uses it for, and what it does not give us

minicoro is the **switcher** and nothing else — `mco_create`, `mco_resume`,
`mco_yield`, `mco_status`, `mco_destroy`, plus the hand-written aarch64 and
x86-64 assembly behind them (`MCO_USE_ASM`, auto-selected). The scheduler, the
park/wake bookkeeping, the channels and the stacks are `src/fiber/`'s.

Two things it does not do, and what we do about each:

1. **No guard pages.** `mprotect` occurs zero times in the file, and the block
   layout puts the coroutine's own bookkeeping struct *below* the stack, so an
   overflow eats `storage` and then `mco_coro` itself and is noticed — if at all
   — by a magic-number heuristic at the next yield. `src/fiber/stack.cpp` supplies
   an `mmap`-and-`mprotect(PROT_NONE)` allocator so that an overflow **faults**.
   `FiberGuardPage.*` in `tests/unit/fiber_tests.cpp` is the proof.
2. **A 56 KB default stack** (2040 KB with `MCO_USE_VMEM_ALLOCATOR`). We set
   512 KB, and 1 MB under ASan — Tarantool's numbers, from a decade of running
   LuaJIT on fibers in production.

**Sanitizers are automatic.** There is no `MCO_USE_ASAN` knob: the internal
`_MCO_USE_ASAN` is set from `__has_feature(address_sanitizer)` /
`__SANITIZE_ADDRESS__` (`minicoro.h:532-545`), so
`__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` are compiled
in exactly when the debug preset asks for `-fsanitize=address` and are absent
otherwise. `__tsan_*_fiber` the same way. This is the reason minicoro was chosen
over Tarantool's `coro.c`, which ships none of it.

## Owning it

Last **functional** upstream commit is `ff5321d9`, 2023-11-15; `02dad0f8` is a
FUNDING.yml edit. The repository has **no CI**, so its platform matrix is a
README claim, not a test result — `ctest --preset debug` is now its CI. Budget
for owning the file; do not expect an upstream fix. The documented fallback, if
the layout arithmetic in `src/fiber/stack.cpp` ever proves unmaintainable, is
Tarantool's `coro.c` plus hand-written sanitizer annotations (research note §5.3).

## Re-vendoring

```sh
C=<new commit sha>
curl -sSL -o third_party/minicoro/minicoro.h \
  "https://raw.githubusercontent.com/edubart/minicoro/$C/minicoro.h"
curl -sSL -o third_party/minicoro/LICENSE \
  "https://raw.githubusercontent.com/edubart/minicoro/$C/LICENSE"
shasum -a 256 third_party/minicoro/minicoro.h third_party/minicoro/LICENSE
```

Record the commit and both hashes above, then run
`./build/debug/lesh_tests --gtest_filter='Fiber*'`. **`FiberGuardPage.*` is the
test that matters after a re-vendor**: it is what notices that the block layout
`src/fiber/stack.cpp` computes against has moved.
