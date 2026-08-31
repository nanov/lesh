#include "fiber/stack.h"

#include "minicoro.h"
#include "substrate/assert.h"
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LESH_FIBER_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define LESH_FIBER_ASAN 1
#endif
#ifndef LESH_FIBER_ASAN
#define LESH_FIBER_ASAN 0
#endif

namespace lesh::fiber {
namespace {

[[nodiscard]] std::size_t round_up(std::size_t value, std::size_t to) noexcept {
	return (value + (to - 1)) & ~(to - 1);
}

// THE ARITHMETIC, and why it is a process-wide constant rather than a per-fiber
// computation. `_mco_create_context` lays the block out like this, with `base`
// the pointer our `alloc_cb` returned:
//
//   context_addr = align16(base + sizeof(mco_coro))
//   storage_addr = align16(context_addr + sizeof(_mco_context))
//   stack_addr   = align16(storage_addr + desc->storage_size)
//
// and `_mco_init_desc_sizes` makes
//
//   coro_size    = align16(sizeof(mco_coro)) + align16(sizeof(_mco_context))
//                + align16(storage_size) + stack_size + 16
//
// `sizeof(_mco_context)` is internal and we do not get to see it - but we do not
// need to. Both expressions are the same three aligned spans, so with a
// page-aligned (hence 16-aligned) base:
//
//   header_span := stack_addr - base = align16(sizeof(mco_coro))
//                                    + align16(sizeof(_mco_context))
//                                    + storage_size
//                = coro_size - stack_size - 16      // for ANY stack_size
//
// which is exactly what `mco_desc_init` will tell us if we ask it once with any
// stack size at all. It does not depend on the stack size, so neither does the
// guard offset, so `alloc_cb` - which is handed only a byte count - can place
// the guard from a cached constant and needs no side table and no
// `allocator_data`.
//
// WE THEN GROW `storage_size` UNTIL THE STACK IS PAGE-ALIGNED, plus one page:
//
//   header_span = round_up(header_span_default, page) + page
//
// The first term pushes the stack base up to a page boundary; the `+ page` buys
// the guard. The guard page is [stack_base - page, stack_base), which by
// construction starts at or above `round_up(header_span_default, page)` >=
// `header_span_default` - i.e. ABOVE every byte minicoro actually uses. It lands
// in the storage region, in the part of it that exists only because we asked for
// it, and `storage` is touched only by `mco_push`/`mco_pop`/`mco_peek`, which
// `src/fiber/` never calls. So the PROT_NONE page is padding, and the layout
// invariant we depend on is re-checked at every spawn by
// `verify_guard_placement` - which reads `co.storage`, the public field that
// says where the untouchable region begins, and refuses to continue if the
// guard is not inside it.
struct block_layout {
	std::size_t page = 0;
	std::size_t header_span = 0;   // base -> stack_base, guard included
	std::size_t storage_size = 0;  // padded, so that header_span comes out right
};

void layout_probe_entry(mco_coro* /*co*/) {}

[[nodiscard]] const block_layout& layout() noexcept {
	static const block_layout computed = [] {
		block_layout l;
		l.page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
		LESH_ASSERT(l.page >= 4096 && (l.page & (l.page - 1)) == 0);

		// Any stack size at all: `plain` below is derived from the size the probe
		// came back with, and `header_span` does not depend on it. 32 KB is
		// minicoro's own MCO_MIN_STACK_SIZE, which is not visible outside the
		// translation unit that defines MINICORO_IMPL - hence the literal.
		mco_desc probe = mco_desc_init(&layout_probe_entry, 32u * 1024u);
		const std::size_t plain = probe.coro_size - probe.stack_size - 16;
		l.header_span = round_up(plain, l.page) + l.page;
		l.storage_size = probe.storage_size + (l.header_span - plain);
		return l;
	}();
	return computed;
}

void* guarded_alloc(std::size_t size, void* /*allocator_data*/) {
	const block_layout& l = layout();
	const std::size_t mapped = round_up(size, l.page);

	void* base = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
	                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return nullptr;

	// The reserve is lazy - anonymous pages commit on first touch - so a 512 KB
	// stack costs one page of RSS until a fiber goes deep.
	unsigned char* const guard = static_cast<unsigned char*>(base) + l.header_span - l.page;
	if (::mprotect(guard, l.page, PROT_NONE) != 0) {
		// Not "carry on without a guard": that is the silent-corruption mode this
		// whole file exists to remove, and a caller cannot do anything useful
		// with the distinction. Fail the allocation; `mco_create` reports
		// MCO_OUT_OF_MEMORY and `spawn` turns that into a hard stop.
		::munmap(base, mapped);
		return nullptr;
	}
	return base;
}

void guarded_dealloc(void* ptr, std::size_t size, void* /*allocator_data*/) {
	// `munmap` does not care about the PROT_NONE page, which is the other half of
	// why an independent mapping beats Tarantool's slab: they have to restore
	// PROT_READ|PROT_WRITE before returning the block to a pool, and when that
	// fails they deliberately leak the slab (`fiber.c:1462`). We have no pool.
	const std::size_t mapped = round_up(size, layout().page);
	const int res = ::munmap(ptr, mapped);
	LESH_ASSERT(res == 0);
	(void)res;
}

} // namespace

std::size_t page_size() noexcept { return layout().page; }

bool built_under_asan() noexcept { return LESH_FIBER_ASAN != 0; }

std::size_t default_stack_size() noexcept {
	// Tarantool's SetFiberStackSize.cmake:11-18. 1 MB under ASan because the
	// sanitizer's redzones and its own frame bookkeeping make every frame
	// fatter, and a stack size that only fits in release is a guard-page test
	// that only fires in the configuration without the gate.
	constexpr std::size_t kBytes = LESH_FIBER_ASAN ? 1024u * 1024u : 512u * 1024u;
	return round_up(kBytes, layout().page);
}

void install_guarded_allocator(mco_desc& desc) noexcept {
	const block_layout& l = layout();
	const std::size_t stack = round_up(desc.stack_size, l.page);

	desc.alloc_cb = &guarded_alloc;
	desc.dealloc_cb = &guarded_dealloc;
	desc.allocator_data = nullptr;
	desc.storage_size = l.storage_size;
	desc.stack_size = stack;
	desc.coro_size = l.header_span + stack + 16;
}

stack_extents extents_of(const mco_coro& co) noexcept {
	const block_layout& l = layout();
	stack_extents out;
	if (co.stack_base == nullptr)
		return out;
	out.stack_base = static_cast<const unsigned char*>(co.stack_base);
	out.stack_size = co.stack_size;
	out.guard_size = l.page;
	out.guard_base = out.stack_base - l.page;
	return out;
}

void verify_guard_placement(const mco_coro& co) noexcept {
	const block_layout& l = layout();
	const auto* const base = reinterpret_cast<const unsigned char*>(&co);
	const auto* const stack = static_cast<const unsigned char*>(co.stack_base);
	const auto* const storage = static_cast<const unsigned char*>(
		static_cast<const void*>(co.storage));

	const bool placed_where_we_computed = stack == base + l.header_span;
	// A page-aligned stack base is what makes [stack - page, stack) one whole
	// page rather than two partial ones.
	const bool page_aligned =
		(reinterpret_cast<std::uintptr_t>(stack) & (l.page - 1)) == 0;
	// And this is the one that matters: the guard must lie inside the storage
	// padding, not inside `mco_coro` or `_mco_context`, or we have PROT_NONE'd
	// live bookkeeping and the next switch dies for the wrong reason.
	const bool guard_inside_padding = storage != nullptr && storage <= stack - l.page;

	if (placed_where_we_computed && page_aligned && guard_inside_padding)
		return;

	// Deliberately not an assert: a misplaced guard page passes every other test
	// in `Fiber*` while protecting nothing, and that is precisely the failure a
	// release build must not ship, so the check is on in every build type. The
	// pinned commit in third_party/minicoro/README.lesh.md is the thing to
	// re-check when this fires.
	//
	// stderr, on LESH_ASSERT's precedent: #98's "never stderr while the host owns
	// the terminal" governs routine diagnostics, and this is the last thing the
	// process ever prints.
	std::fprintf(stderr,
	             "lesh: minicoro's block layout is not what src/fiber/stack.cpp computed;\n"
	             "  the guard page would protect nothing. Re-derive the arithmetic against\n"
	             "  the pinned commit (third_party/minicoro/README.lesh.md).\n"
	             "  base=%p stack_base=%p storage=%p header_span=%zu page=%zu\n",
	             static_cast<const void*>(base), static_cast<const void*>(stack),
	             static_cast<const void*>(storage), l.header_span, l.page);
	std::abort();
}

} // namespace lesh::fiber
