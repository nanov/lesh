#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "syntax/parser.h"

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

using namespace lesh;

// The allocation constraint, as tests rather than as a printout.
//
// The compass (tools/lesh_bench) shows the numbers; these assert the ones that
// must not change. Counts rather than timings, because a count is deterministic:
// it needs no quiet machine and it fails the same way on every run.
//
// A heap_allocations increase is the one that matters. It means the arena
// overflowed and fell back to malloc, which is the failure the whole design
// exists to avoid.

namespace {

class AllocationTest : public ::testing::Test {
protected:
	buffer_pool pool{BUFFER_POOL_SIZE};

	void SetUp() override { metrics::allocations().reset(); }

	static size_t heap() { return metrics::allocations().heap_allocations; }
	static size_t pooled() { return metrics::allocations().pool_allocations; }
};

} // namespace

TEST_F(AllocationTest, ParsingNeverFallsBackToTheHeap) {
	for (const char* src : {"echo",
	                        "echo hello world",
	                        "cat f | grep p | wc -l",
	                        "a && b || c",
	                        "cmd arg > out 2>&1",
	                        "ls -la -h -R /usr /var /etc /tmp"}) {
		metrics::allocations().reset();
		buffer_pool fresh{BUFFER_POOL_SIZE};
		const syntax::tree t = syntax::parse(fresh, src);
		EXPECT_EQ(heap(), 0u) << "parsing \"" << src << "\" fell back to malloc";
		EXPECT_GT(t.node_count(), 0u);
	}
}

TEST_F(AllocationTest, ParseCostIsFlatInInputComplexity) {
	// Four blocks - nodes, children, tokens, scratch - regardless of what is being
	// parsed. If this grows, growth is no longer amortised and the arena is being
	// used as a general allocator.
	auto allocations_for = [](const char* src) {
		metrics::allocations().reset();
		buffer_pool fresh{BUFFER_POOL_SIZE};
		const syntax::tree t = syntax::parse(fresh, src);
		(void)t.node_count();
		return metrics::allocations().pool_allocations;
	};

	const size_t simple = allocations_for("echo");
	const size_t complex_input = allocations_for("a b c | d e f && g h || i j > k 2>&1");
	EXPECT_EQ(simple, complex_input)
		<< "parse allocations should not scale with input complexity";
}

TEST_F(AllocationTest, LiteralWordsExpandWithoutAllocatingAtAll) {
	// The literal fast path. A word with no quoting, no expansion and no glob
	// characters is handed back as a view into the source. Most words in most
	// command lines are literal, so this is the common case, and paying an
	// allocation for it would defeat the constraint outright.
	buffer_pool fresh{BUFFER_POOL_SIZE};
	const syntax::tree t = syntax::parse(fresh, "echo hello world");
	const syntax::node_index cmd = t.child_of(t[t.root()], 0);

	runtime::shell_state state;
	runtime::expander ex{fresh, state};
	arena_array<std::string_view> fields{fresh, 8};

	metrics::allocations().reset();
	for (uint32_t i = 0; i < t[cmd].children_count; ++i)
		ex.expand_word(t, t.child_of(t[cmd], i), fields);

	EXPECT_EQ(pooled(), 0u) << "literal words must expand with no allocation";
	EXPECT_EQ(heap(), 0u);
	ASSERT_EQ(fields.size(), 3u);
	EXPECT_EQ(fields[0], "echo");
}

TEST_F(AllocationTest, ExpansionNeverFallsBackToTheHeap) {
	runtime::shell_state state;
	std::ignore = state.set("VAR", "value");
	for (const char* src : {"echo $VAR", "echo pre$VAR-post", "echo \"a $VAR b\"",
	                        "echo '$VAR'", "echo $VAR$VAR"}) {
		buffer_pool fresh{BUFFER_POOL_SIZE};
		const syntax::tree t = syntax::parse(fresh, src);
		const syntax::node_index cmd = t.child_of(t[t.root()], 0);

		metrics::allocations().reset();
		runtime::expander ex{fresh, state};
		arena_array<std::string_view> fields{fresh, 8};
		for (uint32_t i = 0; i < t[cmd].children_count; ++i)
			ex.expand_word(t, t.child_of(t[cmd], i), fields);

		EXPECT_EQ(heap(), 0u) << "expanding \"" << src << "\" fell back to malloc";
	}
}

TEST_F(AllocationTest, LexingAllocatesNothingWhatsoever) {
	// The lexer owns no memory at all - that is what makes it safe to run on every
	// keystroke over a buffer the editor owns.
	metrics::allocations().reset();
	syntax::lexer lx{"echo \"hello $USER\" | grep x > out"};
	while (lx.next().kind != syntax::token_kind::end) {}
	EXPECT_EQ(pooled(), 0u);
	EXPECT_EQ(heap(), 0u);
}
