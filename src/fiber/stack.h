#pragma once

// FIBER STACKS: mmap'd, page-aligned, with a PROT_NONE guard page BELOW them.
//
// minicoro allocates the coroutine bookkeeping struct and the stack as ONE
// block, in this order:
//
//     [ mco_coro | _mco_context | storage | stack ]
//                                          ^ stack_base, grows UPWARD in address
//                                            terms, i.e. the machine stack
//                                            pointer walks DOWN toward `storage`
//
// So an overflow does not run off the end of anything. It walks down out of the
// stack and into `storage`, then into `_mco_context`, then into `mco_coro`
// itself - the state word, the `dealloc_cb`, the `prev_co` chain - and the only
// thing that ever notices is a magic-number check at the *next* `mco_yield`,
// whose own upstream comment reads "This check happens when the stack overflow
// already happened, but better later than never." That is a silent-corruption
// bug with a delayed, misleading report, and it is the one thing the research
// note said to fix before measuring anything else.
//
// `install_guarded_allocator` fixes it through minicoro's documented allocator
// seam: `mco_desc.alloc_cb`/`dealloc_cb`. See `stack.cpp` for the arithmetic and
// for why the guard page ends up in padding rather than in live bookkeeping.
//
// THE GUARD STAYS ON UNDER ASan - deliberately, and unlike Tarantool.
//
//   Tarantool disables its guard page under ASan (`fiber.c:1504-1514`, "If we
//   panic then fiber stacks remain protected which cause leak sanitizer
//   failures"). Their guard is carved by hand out of a slab from the cord's
//   `small` cache: a PROT_NONE hole INSIDE a block that the allocator - and
//   therefore LeakSanitizer - will later walk. LSan reads the block, hits the
//   hole, and dies. That is a property of allocating stacks out of a pooled
//   heap, not a property of guard pages.
//
//   Ours is an independent `mmap` per fiber. Nothing but this file ever walks
//   it; it is not a malloc chunk, so no allocator scans it; and the guard page
//   sits OUTSIDE the [stack_base, stack_base + stack_size) range that minicoro
//   hands `__sanitizer_start_switch_fiber`, so ASan's own fiber bookkeeping
//   never touches it either. There is nothing for a sanitizer to trip over, and
//   ASan's fiber-stack instrumentation is not a substitute for the guard in any
//   case: it tracks which stack is live across a switch, it does not bound the
//   stack. Turning the guard off in the one configuration where the gate
//   actually runs would mean the overflow test only ever proves something about
//   release. So: on in both, and `FiberGuardPage.*` runs under the sanitized
//   gate.
//
// SIZES are Tarantool's, from `cmake/SetFiberStackSize.cmake:11-18`: 512 KB, and
// 1 MB under Clang + ASan + Debug, which is exactly lesh's debug preset. They
// are a decade-old production answer for LuaJIT interpreter frames, JIT trace
// frames, C functions and FFI callbacks on one stack. minicoro's own default -
// 56 KB - is precisely the "8-64 KiB default" #145 warned about.

#include <cstddef>

struct mco_coro;
struct mco_desc;

namespace lesh::fiber {

// The system page size, cached. The guard page is exactly one of these.
[[nodiscard]] std::size_t page_size() noexcept;

// 512 KB, or 1 MB when this translation unit was compiled under ASan. Rounded
// to a page multiple, so it is also the mapping's stack region size.
[[nodiscard]] std::size_t default_stack_size() noexcept;

// True when `src/fiber/` was compiled with -fsanitize=address. Reported rather
// than inferred, because "which stack size am I actually getting" is the first
// question of any fiber stack-depth bug.
[[nodiscard]] bool built_under_asan() noexcept;

// Points `desc` at our allocator and re-derives its size fields so that the
// stack base lands on a page boundary with a PROT_NONE page immediately below
// it. Call it on a descriptor freshly returned from `mco_desc_init`, before
// `mco_create`; `desc.stack_size` must already be the size you want and is
// rounded up to a page multiple here.
void install_guarded_allocator(mco_desc& desc) noexcept;

// Where a coroutine allocated by that allocator keeps its stack and its guard.
// Derived from the coroutine, so it needs no side table - and it is what the
// guard-page test writes just below.
struct stack_extents {
	const unsigned char* guard_base = nullptr;   // PROT_NONE, `guard_size` bytes
	std::size_t guard_size = 0;
	const unsigned char* stack_base = nullptr;   // guard_base + guard_size
	std::size_t stack_size = 0;
};
[[nodiscard]] stack_extents extents_of(const mco_coro& co) noexcept;

// Verifies that minicoro placed the stack exactly where `install_guarded_allocator`
// computed it would - i.e. that the guard page is directly below the stack and
// not sitting harmlessly in the middle of nowhere while overflows still eat the
// bookkeeping struct.
//
// ALWAYS ON, in every build type. It costs one comparison per spawn, and the
// failure it catches is a vendored-layout change that makes every other test in
// `Fiber*` pass while the guard protects nothing. It does not return a failure
// for a caller to ignore: a misplaced guard is not a recoverable condition.
void verify_guard_placement(const mco_coro& co) noexcept;

} // namespace lesh::fiber
