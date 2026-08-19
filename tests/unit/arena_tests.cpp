#include "substrate/arena.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The arena is the one substrate component kept rather than replaced (issue #13),
// so it is the one that has to be proven. Everything above it allocates through
// this, and the constraint it exists to serve - near-zero allocation on the
// command path - is only meaningful if it is correct.

namespace {

// The overflow protocol: allocate() and reallocate() return false when the
// request did not fit, meaning "this came from the heap and you own it". Callers
// release with free() or grow with realloc(), so the arena must hand back
// malloc-family memory. It previously used new char[] here, making both
// undefined - see commit eb32f5a.
void release_if_heap(bool from_pool, char* p) {
	if (!from_pool)
		std::free(p);
}

} // namespace

TEST(Arena, AllocatesFromItsOwnStorageWhileItFits) {
	lesh::buffer_pool pool{256};
	char* a = nullptr;
	ASSERT_TRUE(pool.allocate(16, a));
	ASSERT_NE(a, nullptr);
	EXPECT_GE(a, pool.at() - 16) << "the pointer should sit inside the pool";
}

TEST(Arena, SuccessiveAllocationsDoNotOverlap) {
	lesh::buffer_pool pool{256};
	char *a = nullptr, *b = nullptr;
	ASSERT_TRUE(pool.allocate(32, a));
	ASSERT_TRUE(pool.allocate(32, b));
	EXPECT_GE(b, a + 32) << "b must start at or after the end of a";

	std::memset(a, 'a', 32);
	std::memset(b, 'b', 32);
	for (int i = 0; i < 32; ++i) {
		EXPECT_EQ(a[i], 'a') << "writing b corrupted a at " << i;
		EXPECT_EQ(b[i], 'b');
	}
}

TEST(Arena, ContentsSurviveLaterAllocations) {
	lesh::buffer_pool pool{1024};
	char* first = nullptr;
	ASSERT_TRUE(pool.allocate(8, first));
	std::memcpy(first, "abcdefg", 8);
	for (int i = 0; i < 20; ++i) {
		char* filler = nullptr;
		ASSERT_TRUE(pool.allocate(8, filler));
	}
	EXPECT_STREQ(first, "abcdefg");
}

TEST(Arena, OverflowFallsBackToTheHeapAndSaysSo) {
	lesh::buffer_pool pool{64};
	char* big = nullptr;
	const bool from_pool = pool.allocate(4096, big);
	EXPECT_FALSE(from_pool) << "a request larger than the pool cannot come from it";
	ASSERT_NE(big, nullptr);
	std::memset(big, 'x', 4096);  // ASan proves this is a real 4096-byte region
	EXPECT_EQ(big[0], 'x');
	EXPECT_EQ(big[4095], 'x');
	release_if_heap(from_pool, big);
}

TEST(Arena, OverflowMemoryIsFreeCompatible) {
	// This is the regression test for eb32f5a. Allocating with new char[] and
	// releasing with free() is undefined; ASan catches it. Only fires on the
	// overflow path, which is why it went unnoticed for so long.
	lesh::buffer_pool pool{16};
	for (int i = 0; i < 8; ++i) {
		char* p = nullptr;
		const bool from_pool = pool.allocate(1024, p);
		ASSERT_FALSE(from_pool);
		std::memset(p, i, 1024);
		std::free(p);  // must match how the arena allocated it
	}
}

TEST(Arena, ResetRewindsToAMark) {
	lesh::buffer_pool pool{256};
	char* mark = pool.at();
	char* a = nullptr;
	ASSERT_TRUE(pool.allocate(64, a));
	EXPECT_NE(pool.at(), mark);
	pool.reset(mark);
	EXPECT_EQ(pool.at(), mark);

	char* b = nullptr;
	ASSERT_TRUE(pool.allocate(64, b));
	EXPECT_EQ(a, b) << "after rewinding, the next allocation reuses the same storage";
}

TEST(Arena, ResetToAnInnerMarkUnwindsOnlyThatFar) {
	// Nested parse states rely on this: each records where it started and rewinds
	// to exactly there, so an inner state cannot free an outer state's memory.
	lesh::buffer_pool pool{512};
	char* outer_mark = pool.at();
	char* outer = nullptr;
	ASSERT_TRUE(pool.allocate(32, outer));
	std::memcpy(outer, "outer", 6);

	char* inner_mark = pool.at();
	char* inner = nullptr;
	ASSERT_TRUE(pool.allocate(32, inner));
	pool.reset(inner_mark);

	EXPECT_STREQ(outer, "outer") << "rewinding the inner scope must not disturb the outer one";
	EXPECT_EQ(pool.at(), inner_mark);
	EXPECT_NE(pool.at(), outer_mark);
}

TEST(Arena, ReallocateGrowsInPlaceWhenItFits) {
	lesh::buffer_pool pool{1024};
	char* p = nullptr;
	ASSERT_TRUE(pool.allocate(16, p));
	std::memcpy(p, "hello", 6);

	char* grown = nullptr;
	ASSERT_TRUE(pool.reallocate(p, 64, grown));
	EXPECT_EQ(grown, p) << "growing the last allocation should not move it";
	EXPECT_STREQ(grown, "hello");
}

TEST(Arena, ReallocateBeyondCapacityCopiesToTheHeap) {
	lesh::buffer_pool pool{64};
	char* p = nullptr;
	ASSERT_TRUE(pool.allocate(16, p));
	std::memcpy(p, "hello", 6);

	char* grown = nullptr;
	const bool from_pool = pool.reallocate(p, 4096, grown);
	EXPECT_FALSE(from_pool);
	ASSERT_NE(grown, nullptr);
	EXPECT_STREQ(grown, "hello") << "contents must survive the move to the heap";
	std::free(grown);
}

TEST(Arena, InPlaceGrowthIsWhatDistinguishesItFromStdPmr) {
	// Recorded as a test because it is the reason the arena was kept rather than
	// replaced by std::pmr::monotonic_buffer_resource (issue #16): pmr offers
	// release() - free everything - but no rewind-to-a-mark, and libc++ allocates
	// downward, which makes growing the last allocation in place impossible.
	lesh::buffer_pool pool{4096};
	char* p = nullptr;
	ASSERT_TRUE(pool.allocate(8, p));
	char* grown = nullptr;
	ASSERT_TRUE(pool.reallocate(p, 4000, grown));
	EXPECT_EQ(grown, p);
}

TEST(Arena, ManyAllocationsUpToCapacity) {
	lesh::buffer_pool pool{1024};
	std::vector<char*> ptrs;
	int from_pool_count = 0;
	for (int i = 0; i < 200; ++i) {
		char* p = nullptr;
		if (pool.allocate(8, p))
			++from_pool_count;
		else
			std::free(p);
		ASSERT_NE(p, nullptr);
	}
	EXPECT_GT(from_pool_count, 0);
	EXPECT_LT(from_pool_count, 200) << "1024 bytes cannot satisfy 200 allocations of 8";
}

// --- arena_array -------------------------------------------------------------

#include "substrate/arena_array.h"

TEST(ArenaArray, AppendAndIndexRoundTrip) {
	lesh::buffer_pool pool{4096};
	lesh::arena_array<int> a{pool, 4};
	for (int i = 0; i < 50; ++i)
		EXPECT_EQ(a.push(i), static_cast<uint32_t>(i));
	ASSERT_EQ(a.size(), 50u);
	for (int i = 0; i < 50; ++i)
		EXPECT_EQ(a[i], i) << "element " << i << " lost while growing";
}

TEST(ArenaArray, TruncateUnwindsWithoutLosingWhatRemains) {
	lesh::buffer_pool pool{4096};
	lesh::arena_array<int> a{pool, 4};
	for (int i = 0; i < 10; ++i)
		a.push(i);
	a.truncate(3);
	ASSERT_EQ(a.size(), 3u);
	for (int i = 0; i < 3; ++i)
		EXPECT_EQ(a[i], i);
	a.push(99);
	EXPECT_EQ(a[3], 99);
}

TEST(ArenaArray, ReleasesBlocksTakenFromTheArenaHeapFallback) {
	// Regression test. Growth abandons pooled blocks deliberately - the arena
	// reclaims those on rewind - but blocks from the arena's HEAP fallback are not
	// tracked by the arena, so the array owns them. Getting that distinction wrong
	// leaked 13 KB across 17 blocks, and LeakSanitizer is what caught it.
	//
	// The pool here is deliberately far too small, so every growth overflows.
	lesh::buffer_pool pool{8};
	for (int round = 0; round < 20; ++round) {
		lesh::arena_array<int> a{pool, 2};
		for (int i = 0; i < 100; ++i)
			a.push(i);
		EXPECT_EQ(a[99], 99);
	}
	SUCCEED() << "clean under LeakSanitizer means the fallback blocks were released";
}

TEST(ArenaArray, MoveLeavesTheSourceHarmless) {
	// A defaulted move would leave both objects holding the same pointer and both
	// destructors would free it.
	lesh::buffer_pool pool{8};
	lesh::arena_array<int> a{pool, 2};
	for (int i = 0; i < 40; ++i)
		a.push(i);

	lesh::arena_array<int> b{std::move(a)};
	EXPECT_EQ(b.size(), 40u);
	EXPECT_EQ(b[39], 39);
	EXPECT_EQ(a.size(), 0u);
	EXPECT_EQ(a.data(), nullptr);
}
