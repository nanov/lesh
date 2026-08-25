#include "leshper/editor.h"
#include "leshper/loop.h"
#include "leshper/event.h"
#include "leshper/state.h"
#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "substrate/log.h"
#include "syntax/parser.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fcntl.h>
#include <memory>
#include <cstddef>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

// Whether this build can count REAL mallocs. Only an ASan build can, which is
// the build the gate runs (`ctest --preset debug`), so the logger's cost rule is
// asserted exactly where it is asserted at all.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LESH_COUNTS_REAL_MALLOCS 1
#include <sanitizer/allocator_interface.h>
#endif
#endif

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
	                        "ls -la -h -R /usr /var /etc /tmp",
	                        // A command substitution's interior is parsed recursively
	                        // (#104) - into the same arena, out of the same pool.
	                        "echo $(ls -l foo | grep bar)",
	                        "echo $(echo $(date)) \"$(uname -a)\""}) {
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

// ---------------------------------------------------------------------------
// The logger's half of the constraint (#109's cost rule, #120).
//
// The counters above see the ARENA - they answer "did the pool overflow into
// malloc". The logger allocates from neither: its buffers are thread-local
// arrays and its sinks are raw descriptors, so an accidental `std::string` on
// the logging path would be invisible to `heap()` and would still be exactly the
// defect the cost rule forbids.
//
// So this section counts REAL mallocs, through the sanitizer's own hook. That
// buys two things a replaced `operator new` would have cost: ASan keeps its
// allocator semantics intact for every other test in this binary, and the count
// includes allocations from inside libc++ rather than only the ones our code
// spells with `new`.
//
// The hook is process-wide once installed, and its cost outside a measured
// window is a relaxed load and a not-taken branch per allocation.
// ---------------------------------------------------------------------------

#ifdef LESH_COUNTS_REAL_MALLOCS

namespace {

std::atomic<size_t> g_mallocs{0};
std::atomic<bool> g_counting{false};

void note_malloc(const volatile void*, size_t) noexcept {
	if (g_counting.load(std::memory_order_relaxed))
		g_mallocs.fetch_add(1, std::memory_order_relaxed);
}
void note_free(const volatile void*) noexcept {}

// ONE INSTALL FOR THE WHOLE FILE, and it has to be one.
//
// ASan keeps a FIXED-SIZE array of malloc hooks - five, in compiler-rt - and
// `__sanitizer_install_malloc_and_free_hooks` APPENDS to it rather than
// replacing. A `static` inside the template below is per-INSTANTIATION, so it
// burned one slot per call site, and the sixth `mallocs_during` in the file
// silently got a zero back and then measured nothing at all. Found by adding
// the loop's two numbers below and watching them pass alone and fail in the
// full binary (#129).
const bool g_hooks_installed =
	__sanitizer_install_malloc_and_free_hooks(note_malloc, note_free) != 0;

// Every malloc the process makes while `work` runs.
template <typename Work>
size_t mallocs_during(Work&& work) {
	EXPECT_TRUE(g_hooks_installed) << "the sanitizer allocator hook is what this measures with";
	g_mallocs.store(0, std::memory_order_relaxed);
	g_counting.store(true, std::memory_order_relaxed);
	work();
	g_counting.store(false, std::memory_order_relaxed);
	return g_mallocs.load(std::memory_order_relaxed);
}

} // namespace

TEST_F(AllocationTest, TheMallocCounterCountsAMallocWhenThereIsOne) {
	// The positive control, and it is not optional: every assertion below is a
	// ZERO, and a broken instrument reports zero for everything. No gtest macro
	// inside the window - an assertion that allocated would be measuring itself.
	size_t observed = 0;
	const size_t counted = mallocs_during([&observed] {
		const std::string big(4096, 'x');
		observed = big.size();
	});
	EXPECT_EQ(observed, 4096u);
	EXPECT_GT(counted, 0u);
}

TEST_F(AllocationTest, LoggingOffAllocatesNothingAndEvaluatesNothing) {
	// #109's cost rule, as the number it is stated in. Every level times every
	// category, with an argument that both allocates and counts itself - so the
	// two ways this can go wrong, evaluating early and formatting early, are told
	// apart by which of the two expectations below fails.
	log::shutdown();

	int evaluations = 0;
	std::string held;
	const auto expensive = [&evaluations, &held]() -> const char* {
		++evaluations;
		held = std::string(4096, 'x');
		return held.c_str();
	};

	const size_t counted = mallocs_during([&] {
		for (int in = 0; in < static_cast<int>(log::category::count_); ++in) {
			for (int of = 1; of < static_cast<int>(log::level::count_); ++of) {
				LESH_LOG(static_cast<log::level>(of), static_cast<log::category>(in), "%s",
				         expensive());
			}
		}
	});

	EXPECT_EQ(counted, 0u) << "a disabled log allocated";
	EXPECT_EQ(evaluations, 0) << "a disabled log evaluated its arguments";
}

TEST_F(AllocationTest, LoggingOffCostsTheCommandPathNoHeapAllocation) {
	// THE ASSERTION THE TICKET ASKS FOR, isolated so it can only be about the
	// hook. A signal event is the one input the editor deliberately does nothing
	// with - no buffer change, no effect pushed, no pending input drained - so
	// every allocation this counts belongs to `log_event` and to nothing else.
	//
	// It goes non-zero the moment somebody builds a record, formats a message, or
	// copies a string on the way to the gate check instead of past it.
	using namespace lesh::leshper;
	log::shutdown();

	state s;
	step(s, signal_event{28});   // warm: the first turn touches lazily-built state

	const size_t counted = mallocs_during([&] {
		for (int i = 0; i < 1000; ++i)
			step(s, signal_event{28});
	});
	EXPECT_EQ(counted, 0u) << "the event hook allocates with logging off";
}

TEST_F(AllocationTest, EvenLoggingOnFormatsWithoutTheHeap) {
	// Not required by the cost rule - a user who turned logging on has accepted
	// its cost - but it is the property the fixed thread-local buffers exist to
	// have, and "a log line that reallocates is an allocation-gate suspect" is
	// only checkable here.
	// A private scratch directory rather than a fixed name in the shared one:
	// parallel agents run this binary at the same time (#60).
	const lesh::testing::temp_path scratch;
	const std::string text = scratch.file("log");
	const std::string replay = scratch.file("replay.jsonl");
	ASSERT_TRUE(log::configure(
		log::settings_from_env("trace", text.c_str(), replay.c_str(), nullptr, nullptr), {}));
	ASSERT_TRUE(log::enabled(log::level::info, log::category::exec));
	ASSERT_TRUE(log::recording());

	// Warm what initialises once: the timezone database `localtime_r` reads, and
	// this thread's cached id.
	LESH_LOG(log::level::info, log::category::exec, "warm %d", 0);
	log::record{log::category::event}.text("kind", "warm").commit();

	const size_t counted = mallocs_during([] {
		for (int i = 0; i < 200; ++i)
			LESH_LOG(log::level::info, log::category::exec, "line %d of %s", i, "a session");
		for (int i = 0; i < 200; ++i)
			log::record{log::category::event}
				.text("kind", "key")
				.number("cp", int64_t{97})
				.flag("named", false)
				.commit();
	});
	EXPECT_EQ(counted, 0u) << "formatting a log line reached the heap";

	log::shutdown();
}

// --- The event loop (#129) -------------------------------------------------
//
// HERE RATHER THAN IN leshper_loop_tests.cpp, and the reason is the instrument:
// ASan permits ONE malloc-hook pair per process, and this file claims it. A
// second `__sanitizer_install_malloc_and_free_hooks` in another translation
// unit does not fail loudly, it just never counts - which would be a green
// allocation test measuring nothing. So the loop's two numbers live beside
// every other number the gate asserts.

namespace {

// A pipe standing in for a terminal, the way leshper_loop_tests.cpp drives the
// loop everywhere: never the process's own tty.
class loop_over_a_pipe {
public:
	loop_over_a_pipe() {
		[&] { ASSERT_EQ(::pipe(_in), 0); }();
		[&] { ASSERT_EQ(::pipe(_out), 0); }();
		::fcntl(_in[0], F_SETFL, O_NONBLOCK);
		::fcntl(_out[0], F_SETFL, O_NONBLOCK);
		leshper::loop_options options;
		options.manage_terminal = false;
		options.prompt = "> ";
		_loop = std::make_unique<leshper::event_loop>(leshper::loop_fds{_in[0], _out[1]},
		                                             std::move(options));
		_loop->enter_read();
	}
	~loop_over_a_pipe() {
		_loop.reset();
		for (int fd : {_in[0], _in[1], _out[0], _out[1]})
			::close(fd);
	}

	loop_over_a_pipe(const loop_over_a_pipe&) = delete;
	loop_over_a_pipe& operator=(const loop_over_a_pipe&) = delete;

	[[nodiscard]] leshper::event_loop& loop() const { return *_loop; }

	void drain_output() const {
		char chunk[4096];
		while (::read(_out[0], chunk, sizeof(chunk)) > 0) {
		}
	}

private:
	int _in[2]{-1, -1};
	int _out[2]{-1, -1};
	std::unique_ptr<leshper::event_loop> _loop;
};

} // namespace

TEST_F(AllocationTest, AWarmIdleLoopTurnCostsNoHeap) {
	// N-2 for the loop's OWN machinery: the pollfd array, the read buffer, the
	// event vector, the signal-number vector and the blitter's output string are
	// all members with capacity taken in the constructor, and this is what says
	// so.
	//
	// It measures THE LOOP, not the editor. A keystroke turn still allocates,
	// for reasons belonging to two other files: `step` returns `effects` by
	// value (effect.h: the small-buffer answer waits on the N-1 latency gate map
	// #82 carries as fog) and `apply_edit` records an undo entry. Widening this
	// to a keystroke is that ticket's work, not something to fake here.
	loop_over_a_pipe driven;
	for (int i = 0; i < 4; ++i)
		driven.loop().turn(0);

	const size_t counted = mallocs_during([&] {
		for (int i = 0; i < 100; ++i)
			driven.loop().turn(0);
	});
	EXPECT_EQ(counted, 0u) << "an idle loop turn reached the heap";
}

TEST_F(AllocationTest, ALoopRepaintCostsAConstantThatDoesNotGrow) {
	// The repaint path is not zero, and claiming it were would be a lie:
	// `lay_out` mints a fresh `surface` on every call, which is #123's
	// layout-as-value - two calls with equal inputs produce equal layouts
	// precisely because nothing is carried between them.
	//
	// What IS assertable, and what a regression would break, is that the cost is
	// CONSTANT PER REPAINT. Everything the loop contributes is reused: the
	// cluster pool is warm, the previous layout is held in the loop, the blitter
	// writes into a member string. So three times the repaints must be three
	// times the allocations and not one more; a term that grew with the count
	// would be state accumulating on the render path.
	loop_over_a_pipe driven;
	for (int i = 0; i < 4; ++i)
		driven.loop().render();
	driven.drain_output();

	const auto repaints = [&](int times) {
		return mallocs_during([&] {
			for (int i = 0; i < times; ++i)
				driven.loop().render();
		});
	};
	const size_t fifty = repaints(50);
	const size_t hundred_and_fifty = repaints(150);

	EXPECT_GT(fifty, 0u) << "a zero here would mean the hook, not the loop, is broken";
	EXPECT_EQ(hundred_and_fifty, fifty * 3) << "the per-repaint cost is a constant";
}
#endif
