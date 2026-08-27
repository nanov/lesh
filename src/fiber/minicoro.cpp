// THE ONE TRANSLATION UNIT THAT COMPILES minicoro.
//
// `minicoro.h` is a single-header library: including it declares, defining
// MINICORO_IMPL first compiles it. Exactly one TU in the tree may do the second
// thing, and this is that TU. Everything else in `src/fiber/` includes the
// header without the macro and links against these symbols.
//
// The vendored bytes are unpatched (third_party/minicoro/README.lesh.md), so
// every configuration choice lesh makes is made here, and the list is short:
//
//   MCO_USE_VMEM_ALLOCATOR
//     minicoro's default allocator is `calloc`, which commits the whole stack
//     up front. This one is `mmap`/`munmap`: reserve, commit on touch. lesh
//     never actually reaches it - `stack.cpp` installs its own `alloc_cb` and
//     `dealloc_cb` on every descriptor, because the default has no guard page -
//     but a descriptor built without going through us gets a lazily committed
//     mmap rather than a memset of half a megabyte, which is the right
//     fallback. It also moves MCO_DEFAULT_STACK_SIZE from 56 KB to 2040 KB;
//     irrelevant, since `stack.cpp` always passes an explicit size.
//
//   MCO_MIN_STACK_SIZE / MCO_DEFAULT_STACK_SIZE
//     left alone. The sizes are `fiber::default_stack_size()`'s business and
//     they are passed explicitly; a second copy of the number here could only
//     ever disagree with the first.
//
// AND THE LIST OF THINGS DELIBERATELY NOT DEFINED, because each is a trap:
//
//   MCO_NO_MULTITHREAD - NOT defined, though the research note's 5.4 ns
//     round-trip was measured with it (it turns the current-coroutine pointer
//     from `thread_local` into a plain global, and the note's bracket for the
//     TLS load alone is 5.4 ns vs 17.3 ns with debug asserts also on). The cord
//     door stays open: a `scheduler` is instantiable and per-thread by
//     construction, and the day a second one runs on an I/O thread for history
//     persistence, a global current-coroutine pointer would be silent
//     corruption rather than a compile error. We pay the TLS load for that.
//
//   MCO_NO_DEBUG - NOT defined, because it does not need to be: minicoro turns
//     its own asserts off under NDEBUG (`minicoro.h:409-411`), which Release and
//     RelWithDebInfo both set and Debug does not. That is exactly the split we
//     want - the magic-number stack-overflow heuristic and the state asserts
//     live in the sanitized gate and vanish from the shipped binary - and it
//     happens without a second knob that could disagree with CMAKE_BUILD_TYPE.
//
//   MCO_USE_ASAN - DOES NOT EXIST. The research note §7.1 records the ticket's
//     wrong premise here: the macro is internal `_MCO_USE_ASAN`, set from
//     `__has_feature(address_sanitizer)` / `__SANITIZE_ADDRESS__`. The
//     `__sanitizer_{start,finish}_switch_fiber` annotations are therefore
//     compiled in exactly when the debug preset asks for `-fsanitize=address`,
//     and there is nothing to remember to define. Same for `__tsan_*_fiber`.
//     A probe that goes looking for the named knob concludes the feature is
//     missing; it is not.
//
// This file is held to the vendoring promise at the top of the root
// CMakeLists.txt - somebody else's header is not held to our warning policy -
// so the target compiles this one source with `-w`. Nothing of ours lives here;
// if you are tempted to add code below, it belongs in `stack.cpp`.

#define MINICORO_IMPL
#define MCO_USE_VMEM_ALLOCATOR

#include "minicoro.h"
