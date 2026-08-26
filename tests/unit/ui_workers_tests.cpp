#include "leshper/abi.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "ui/workers.h"
#include "substrate/arena.h"

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace lesh;
using namespace lesh::leshper;
using namespace lesh::ui;

// The worker pool, as tests (#126).
//
// NOT ONE SLEEP IN THIS FILE, and that is a rule rather than a preference: six
// agents share this machine, and a test that waits on a clock passes on an idle
// one and fails under load - which is the worst possible failure mode for a
// concurrency suite, because it teaches everyone to re-run rather than to look.
// Every wait here is on a gate, a condition variable or poll(2). Where a test
// needs a deadline it is a FAILURE deadline (poll's timeout, generous), never a
// synchronisation one.
//
// Two tests would hang rather than fail if the property under test were absent -
// ParkAllReachesALongComputeThroughTheSupersededPoll and its sibling. That is
// deliberate: their subject is a compute that never ends on its own, so the
// honest assertion is "park_all returns", and there is no way to spell it that
// also terminates when it is false.

namespace {

// ---------------------------------------------------------------------------
// Deterministic synchronisation, in fourteen lines.
// ---------------------------------------------------------------------------

// A one-shot gate that also counts arrivals, so a test can wait for "all four
// workers are inside" as easily as for "the first one is".
class gate {
public:
	void open() {
		{
			std::lock_guard lock(_mutex);
			_open = true;
		}
		_changed.notify_all();
	}

	void wait() {
		std::unique_lock lock(_mutex);
		_changed.wait(lock, [this] { return _open; });
	}

	void arrive() {
		{
			std::lock_guard lock(_mutex);
			++_arrived;
		}
		_changed.notify_all();
	}

	void wait_for(std::size_t arrivals) {
		std::unique_lock lock(_mutex);
		_changed.wait(lock, [this, arrivals] { return _arrived >= arrivals; });
	}

private:
	mutable std::mutex _mutex;
	std::condition_variable _changed;
	bool _open = false;
	std::size_t _arrived = 0;
};

// ---------------------------------------------------------------------------
// Reactors. Plain function pointers, because that is the only shape the ABI
// has and a test that reached for std::function would be testing something the
// pool cannot run.
// ---------------------------------------------------------------------------

constexpr std::uint32_t todo_style = 7;

std::string buffer_of(const lesh_request* request) {
	std::size_t length = 0;
	if (lesh_request_buffer_length(request, &length) != LESH_OK)
		return {};
	std::string text(length, '\0');
	std::size_t written = 0;
	if (lesh_request_buffer(request, text.data(), text.size(), &written) != LESH_OK)
		return {};
	text.resize(written);
	return text;
}

std::int32_t mark_todo(lesh_request* request, void*) {
	const std::string text = buffer_of(request);
	const std::size_t at = text.find("TODO");
	if (at != std::string::npos)
		lesh_emit_span(request, at, at + 4, todo_style);
	return LESH_OK;
}

struct run_counter {
	std::mutex mutex;
	std::size_t runs = 0;
	std::vector<std::uint64_t> generations;
};

std::int32_t count_the_run(lesh_request* request, void* userdata) {
	auto& counter = *static_cast<run_counter*>(userdata);
	std::uint64_t gen = 0;
	lesh_request_generation(request, &gen);
	std::lock_guard lock(counter.mutex);
	++counter.runs;
	counter.generations.push_back(gen);
	return LESH_OK;
}

std::int32_t answer_nothing(lesh_request*, void*) { return LESH_OK; }

// Holds the worker until a test opens the gate. The gate is the only thing that
// ends it, so a test that forgets to open one hangs rather than passing by luck.
struct held {
	gate entered;
	gate release;
};

std::int32_t hold_until_released(lesh_request*, void* userdata) {
	auto& held_here = *static_cast<held*>(userdata);
	held_here.entered.arrive();
	held_here.entered.open();
	held_here.release.wait();
	return LESH_OK;
}

// The fine-grained check-in, written as a reactor would write it: an unbounded
// computation that gives up the instant the poll says somebody newer wants the
// worker. yield() rather than sleep - this loop ends on a flag, not a clock.
std::int32_t poll_until_superseded(lesh_request* request, void* userdata) {
	auto& held_here = *static_cast<held*>(userdata);
	held_here.entered.arrive();
	held_here.entered.open();
	for (;;) {
		std::int32_t superseded = 0;
		if (lesh_request_superseded(request, &superseded) != LESH_OK)
			return LESH_ERR_INVAL;
		if (superseded != 0)
			return LESH_ERR_SUPERSEDED;
		std::this_thread::yield();
	}
}

// What the ABI answered on the worker, so a test can assert that a token minted
// off the loop thread is a live one.
struct seen_snapshot {
	gate done;
	std::string buffer;
	std::size_t cursor = 0;
	std::uint64_t generation = 0;
	std::uint32_t event_kind = 0;
	std::int32_t selection_active = -1;
	std::int32_t buffer_status = LESH_ERR_INVAL;
	std::int32_t cursor_status = LESH_ERR_INVAL;
	std::int32_t generation_status = LESH_ERR_INVAL;
	bool token_was_live = false;
};

std::int32_t report_the_snapshot(lesh_request* request, void* userdata) {
	auto& seen = *static_cast<seen_snapshot*>(userdata);
	seen.token_was_live = token_is_live(request);
	std::size_t length = 0;
	seen.buffer_status = lesh_request_buffer_length(request, &length);
	seen.buffer = buffer_of(request);
	seen.cursor_status = lesh_request_cursor(request, &seen.cursor);
	seen.generation_status = lesh_request_generation(request, &seen.generation);
	lesh_request_event_kind(request, &seen.event_kind);
	std::size_t start = 0;
	std::size_t end = 0;
	lesh_request_selection(request, &start, &end, &seen.selection_active);
	return LESH_OK;
}

// What a native reactor does with its worker's arena: the highlighter (#124)
// will parse into exactly this.
struct arena_report {
	std::mutex mutex;
	gate entered;
	gate release;
	std::vector<buffer_pool*> arenas;
	std::vector<std::size_t> used_on_entry;
	std::size_t pool_allocations_on_the_worker = 0;
};

std::int32_t allocate_from_the_worker_arena(lesh_request*, void* userdata) {
	auto& report = *static_cast<arena_report*>(userdata);
	buffer_pool* const arena = current_worker_arena();
	const std::size_t used = arena == nullptr ? ~std::size_t{0} : arena->bytes_used();

	const std::size_t before = metrics::allocations().pool_allocations;
	if (arena != nullptr) {
		char* got = nullptr;
		for (int i = 0; i < 8; ++i)
			arena->allocate(1024, got);
	}
	const std::size_t after = metrics::allocations().pool_allocations;

	{
		std::lock_guard lock(report.mutex);
		report.arenas.push_back(arena);
		report.used_on_entry.push_back(used);
		report.pool_allocations_on_the_worker += after - before;
	}
	report.entered.arrive();
	report.entered.open();
	report.release.wait();
	return LESH_OK;
}

// A state with `text` in it, one generation past empty.
state state_holding(std::string_view text) {
	state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), text);
	s.cursor = s.buffer.end_position();
	s.gen.bump();
	return s;
}

} // namespace

// ---------------------------------------------------------------------------
// How wide the pool is
// ---------------------------------------------------------------------------

TEST(UiWorkers, TheDefaultWidthIsFourOrTheHardwareWhereverItIsNarrower) {
	// Four is the number of independent questions leshper has outstanding at a
	// keystroke - highlighter, autosuggester, completer, history searcher - each
	// with one latest-wins slot of depth at most one, so a fifth worker would
	// have nothing to take.
	const unsigned hardware = std::thread::hardware_concurrency();
	const std::size_t expected =
		hardware == 0 ? std::size_t{1} : std::min<std::size_t>(hardware, 4);
	EXPECT_EQ(worker_pool::default_worker_count(), expected);
	EXPECT_GE(worker_pool::default_worker_count(), 1u);
	EXPECT_LE(worker_pool::default_worker_count(), 4u);
}

TEST(UiWorkers, ThePoolIsAsWideAsItWasAskedToBe) {
	worker_pool pool{3};
	EXPECT_EQ(pool.size(), 3u);
}

// ---------------------------------------------------------------------------
// A request, from submit to completion
// ---------------------------------------------------------------------------

TEST(UiWorkers, AComputationRunsOnAWorkerAndComesBackThroughTheQueue) {
	worker_pool pool{2};
	const state s = state_holding("# TODO: later");

	pool.submit("todo_marker", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED), mark_todo, nullptr);

	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 1), 1u);
	ASSERT_EQ(drained.size(), 1u);

	const reactor_batch& batch = drained[0].batch();
	EXPECT_EQ(batch.reactor, "todo_marker");
	EXPECT_EQ(batch.status, LESH_OK);
	EXPECT_EQ(batch.event_kind, LESH_EVENT_BUFFER_CHANGED);
	EXPECT_TRUE(batch.computed_against == s.gen);
	ASSERT_EQ(batch.spans.size(), 1u);
	EXPECT_EQ(batch.spans[0].start, 2u);
	EXPECT_EQ(batch.spans[0].end, 6u);
	EXPECT_EQ(batch.spans[0].style_id, todo_style);
}

TEST(UiWorkers, TheTokenIsLiveOnTheWorkerAndCarriesTheSnapshotAndNothingElse) {
	worker_pool pool{1};
	state s = state_holding("echo hi");
	s.cursor = position::from_byte_offset(4);

	seen_snapshot seen;
	pool.submit("reporter", snapshot_of(s, LESH_EVENT_CURSOR_MOVED), report_the_snapshot, &seen);
	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 1), 1u);

	// The accessors answering at all is the assertion: they refuse a token whose
	// owning thread is not the caller's, so this is the ABI's own liveness rule
	// agreeing that a worker owns the token it was handed.
	EXPECT_TRUE(seen.token_was_live);
	EXPECT_EQ(seen.buffer_status, LESH_OK);
	EXPECT_EQ(seen.cursor_status, LESH_OK);
	EXPECT_EQ(seen.generation_status, LESH_OK);
	EXPECT_EQ(seen.buffer, "echo hi");
	EXPECT_EQ(seen.cursor, 4u);
	EXPECT_EQ(seen.generation, s.gen.value());
	EXPECT_EQ(seen.event_kind, LESH_EVENT_CURSOR_MOVED);
	EXPECT_EQ(seen.selection_active, 0) << "there is no selection model until #96";
}

TEST(UiWorkers, AWorkersAnswerMeetsTheExistingDropRuleUnchanged) {
	// The pool computes; the loop still decides. A completion is a reactor_batch,
	// so it goes into the applier that already exists and meets N-4 there rather
	// than growing a second staleness rule on this side.
	registry reg;
	loop_harness loop(reg);
	worker_pool pool{2};
	state s = state_holding("TODO");

	pool.submit("todo_marker", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED), mark_todo, nullptr);
	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 1), 1u);

	// The user typed while the worker was thinking.
	s.gen.bump();
	EXPECT_FALSE(apply_batch(s, drained[0].batch()));
	EXPECT_TRUE(s.marks.layers().empty());
}

// ---------------------------------------------------------------------------
// Latest-wins
// ---------------------------------------------------------------------------

TEST(UiWorkers, TheLatestPendingRequestWinsAndTheOneBetweenIsDropped) {
	// Two workers and one slot, so the "at most one in flight" property is being
	// asserted too: a second worker must not pick up the same reactor.
	worker_pool pool{2};
	held first;
	state s = state_holding("a");

	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            hold_until_released, &first);
	first.entered.wait();

	run_counter later;
	s.gen.bump();
	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            count_the_run, &later);
	const std::uint64_t abandoned = s.gen.value();
	s.gen.bump();
	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            count_the_run, &later);

	EXPECT_EQ(pool.dropped(), 1u) << "the middle request was overwritten where it waited";
	EXPECT_EQ(pool.started(), 1u) << "one in flight, and no second worker took the slot";

	first.release.open();
	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 2), 2u);
	EXPECT_EQ(pool.started(), 2u) << "three submissions, two runs";

	ASSERT_EQ(later.generations.size(), 1u);
	EXPECT_NE(later.generations[0], abandoned);
	EXPECT_EQ(later.generations[0], s.gen.value());
	EXPECT_EQ(drained[0].batch().computed_against.value(), 1u);
	EXPECT_EQ(drained[1].batch().computed_against.value(), s.gen.value());
}

TEST(UiWorkers, DifferentReactorsAreDifferentSlotsAndDoNotDisplaceEachOther) {
	worker_pool pool{2};
	held both;
	const state s = state_holding("x");
	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            hold_until_released, &both);
	pool.submit("autosuggest", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            hold_until_released, &both);

	both.entered.wait_for(2);
	EXPECT_EQ(pool.dropped(), 0u) << "two reactors, two slots, nothing displaced";
	both.release.open();

	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 2), 2u);
}

TEST(UiWorkers, SubmittingOverAnInFlightRequestSetsItsSupersededPoll) {
	worker_pool pool{2};
	held thinking;
	state s = state_holding("a");

	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            poll_until_superseded, &thinking);
	thinking.entered.wait();

	run_counter fresh;
	s.gen.bump();
	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            count_the_run, &fresh);

	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 2), 2u);
	EXPECT_EQ(drained[0].batch().status, LESH_ERR_SUPERSEDED)
		<< "the in-flight compute polled and gave up";
	EXPECT_EQ(drained[1].batch().status, LESH_OK);
	EXPECT_EQ(fresh.runs, 1u);
}

TEST(UiWorkers, SupersedeAllDropsWhatIsPendingAndTellsWhatIsInFlight) {
	// One worker, so the second submission is genuinely waiting rather than
	// being picked up the instant it lands.
	worker_pool pool{1};
	held thinking;
	state s = state_holding("a");
	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            poll_until_superseded, &thinking);
	thinking.entered.wait();

	run_counter never;
	s.gen.bump();
	pool.submit("autosuggest", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            count_the_run, &never);

	pool.supersede_all();

	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 1), 1u);
	EXPECT_EQ(drained[0].batch().reactor, "highlighter");
	EXPECT_EQ(drained[0].batch().status, LESH_ERR_SUPERSEDED);
	// The pending one never ran: supersede_all emptied the slot before any
	// worker could reach it, so this is a fact and not a race.
	EXPECT_EQ(pool.completed(), 1u);
	EXPECT_EQ(never.runs, 0u);
}

// ---------------------------------------------------------------------------
// The seam toward the loop
// ---------------------------------------------------------------------------

TEST(UiWorkers, TheWakeupFdIsReadableExactlyWhileSomethingIsWaiting) {
	worker_pool pool{1};
	ASSERT_GE(pool.completions().wakeup_fd(), 0);
	EXPECT_FALSE(pool.completions().armed());

	pollfd watch{pool.completions().wakeup_fd(), POLLIN, 0};
	EXPECT_EQ(::poll(&watch, 1, 0), 0) << "nothing submitted, nothing to wake for";

	const state s = state_holding("x");
	pool.submit("noop", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED), answer_nothing, nullptr);

	// A failure deadline, not a synchronisation one: the wakeup either arrives
	// or this test has found the defect it exists to find.
	watch.revents = 0;
	ASSERT_EQ(::poll(&watch, 1, 5000), 1) << "the worker finished and nothing woke the loop";
	EXPECT_TRUE((watch.revents & POLLIN) != 0);
	EXPECT_TRUE(pool.completions().armed());

	std::vector<completion> drained;
	EXPECT_EQ(pool.completions().drain(drained), 1u);
	EXPECT_FALSE(pool.completions().armed());
	watch.revents = 0;
	EXPECT_EQ(::poll(&watch, 1, 0), 0) << "drain must disarm the wakeup";
}

TEST(UiWorkers, ABurstOfResultsCostsOneWakeupAndNotOnePerResult) {
	// The queue on its own, with no workers anywhere near it - it is a
	// loop-agnostic object and this is what that means.
	//
	// Declared in this order deliberately: the queue dies first and hands its
	// messages back to a pool that is still alive (ADR-0007).
	message_pool messages;
	completion_queue queue;

	for (int i = 0; i < 8; ++i)
		queue.post(messages.acquire());
	EXPECT_EQ(queue.size(), 8u);
	EXPECT_TRUE(queue.armed());

	// Eight results, ONE byte. The fd is armed on the empty-to-non-empty
	// transition only, which is what keeps a keystroke's worth of answers from
	// becoming eight loop turns.
	unsigned char sink[64];
	EXPECT_EQ(::read(queue.wakeup_fd(), sink, sizeof sink), 1);

	std::vector<completion> drained;
	EXPECT_EQ(queue.drain(drained), 8u);
	EXPECT_FALSE(queue.armed());
	EXPECT_TRUE(queue.empty());

	// And the next arrival arms it again.
	queue.post(messages.acquire());
	EXPECT_TRUE(queue.armed());
	pollfd watch{queue.wakeup_fd(), POLLIN, 0};
	EXPECT_EQ(::poll(&watch, 1, 0), 1);
	drained.clear();
	queue.drain(drained);
}

TEST(UiWorkers, MessagesAreDrawnFromAPoolRatherThanAllocatedPerRequest) {
	worker_pool pool{1};
	state s = state_holding("x");
	run_counter counter;

	for (int i = 0; i < 64; ++i) {
		s.gen.bump();
		pool.submit("noop", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED), count_the_run, &counter);
		std::vector<completion> drained;
		ASSERT_EQ(pool.completions().wait_and_drain(drained, 1), 1u);
	}

	EXPECT_EQ(counter.runs, 64u);
	EXPECT_EQ(pool.messages().live(), 0u) << "every message came home";
	EXPECT_LE(pool.messages().minted(), 2u)
		<< "sixty-four requests off a handful of messages: a completion is borrowed";
}

// ---------------------------------------------------------------------------
// The arena (#90)
// ---------------------------------------------------------------------------

TEST(UiWorkers, TheArenaIsResetAtRequestEndSoNothingOutlivesItsRequest) {
	worker_pool pool{1};
	state s = state_holding("x");
	arena_report report;

	for (int i = 0; i < 4; ++i) {
		report.release.open();
		s.gen.bump();
		pool.submit("greedy", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
		            allocate_from_the_worker_arena, &report);
		std::vector<completion> drained;
		ASSERT_EQ(pool.completions().wait_and_drain(drained, 1), 1u);
	}

	ASSERT_EQ(report.used_on_entry.size(), 4u);
	for (std::size_t i = 0; i < report.used_on_entry.size(); ++i) {
		EXPECT_NE(report.arenas[i], nullptr) << "a worker with no arena, at request " << i;
		EXPECT_EQ(report.used_on_entry[i], 0u)
			<< "request " << i << " started on an arena the last one had left dirty";
	}
	EXPECT_EQ(report.arenas[0], report.arenas[3]) << "one worker, one arena";
}

TEST(UiWorkers, EachWorkerOwnsAnArenaOfItsOwn) {
	worker_pool pool{4};
	arena_report report;
	const state s = state_holding("x");
	const char* const keys[] = {"a", "b", "c", "d"};
	for (std::size_t i = 0; i < pool.size(); ++i) {
		pool.submit(keys[i], snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
		            allocate_from_the_worker_arena, &report);
	}

	// Every worker is inside a compute at the same instant, so the arenas
	// collected below are genuinely concurrent ones.
	report.entered.wait_for(pool.size());
	report.release.open();
	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, pool.size()), pool.size());

	std::vector<buffer_pool*> arenas = report.arenas;
	std::sort(arenas.begin(), arenas.end());
	EXPECT_EQ(std::adjacent_find(arenas.begin(), arenas.end()), arenas.end())
		<< "two workers shared an arena, which is the race #90 exists to remove";
}

TEST(UiWorkers, TheArenaIsAWorkersOwnAndNobodyElsesToFind) {
	worker_pool pool{1};
	ASSERT_EQ(pool.size(), 1u);
	EXPECT_EQ(current_worker_arena(), nullptr) << "the loop thread has no worker arena";
}

TEST(UiWorkers, TheAllocationGateCountsPerThreadSoAWorkerCannotPolluteIt) {
	// #90's third decision. The gate in tests/unit/allocation_tests.cpp asserts
	// the COMMAND PATH's allocation counts; a worker parsing a snapshot at the
	// same instant used to land in the same number, which would have made the
	// gate mean something other than what it says.
	worker_pool pool{1};
	state s = state_holding("x");
	arena_report report;
	report.release.open();

	metrics::allocations().reset();
	pool.submit("greedy", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            allocate_from_the_worker_arena, &report);
	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 1), 1u);

	EXPECT_EQ(metrics::allocations().pool_allocations, 0u)
		<< "a worker's arena allocations landed in this thread's counters";
	EXPECT_EQ(metrics::allocations().heap_allocations, 0u);
#ifdef LESH_ENABLE_ASSERTS
	// They were counted - on the worker's own thread, where they belong.
	EXPECT_EQ(report.pool_allocations_on_the_worker, 8u);
#endif
}

// ---------------------------------------------------------------------------
// Quiesce (#91)
// ---------------------------------------------------------------------------

TEST(UiWorkers, AnIdlePoolParksAndResumes) {
	worker_pool pool{3};
	EXPECT_FALSE(pool.is_quiesced());
	pool.park_all();
	EXPECT_TRUE(pool.is_quiesced());
	pool.assert_quiesced();
	pool.resume();
	EXPECT_FALSE(pool.is_quiesced());
}

TEST(UiWorkers, ParkAllReachesALongComputeThroughTheSupersededPoll) {
	// #115's finding, as a test: quiesce cost is task granularity, so the lever
	// is how finely a compute checks in. The reactor below runs forever and
	// nothing in this test ever releases it. What ends it is that PARKING
	// SUPERSEDES WHAT IS IN FLIGHT - the poll the ABI already has, used for a
	// second reason - and if that rule were absent this test would hang, which
	// is the only honest way to assert "park_all returns".
	worker_pool pool{2};
	held forever;
	const state s = state_holding("a long line to think about");

	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            poll_until_superseded, &forever);
	forever.entered.wait();

	pool.park_all();
	EXPECT_TRUE(pool.is_quiesced());
	pool.assert_quiesced();
	EXPECT_EQ(pool.completed(), 1u) << "the compute finished before the pool parked";

	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().drain(drained), 1u);
	EXPECT_EQ(drained[0].batch().status, LESH_ERR_SUPERSEDED);

	pool.resume();
	EXPECT_FALSE(pool.is_quiesced());
}

TEST(UiWorkers, ParkAllWaitsForAComputeThatNeverPolls) {
	// Not polling is safe - the ABI says so - it just costs the wait. The
	// release comes from another thread because this one is about to block, and
	// either order works: park_all returns only once the worker has checked in.
	worker_pool pool{2};
	held stubborn;
	const state s = state_holding("x");
	pool.submit("stubborn", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            hold_until_released, &stubborn);
	stubborn.entered.wait();

	std::thread releaser([&stubborn] { stubborn.release.open(); });
	pool.park_all();
	releaser.join();

	EXPECT_TRUE(pool.is_quiesced());
	EXPECT_EQ(pool.completed(), 1u);
	pool.resume();
}

TEST(UiWorkers, AParkedPoolAcceptsWorkAndRunsNoneOfItUntilResume) {
	worker_pool pool{2};
	pool.park_all();
	ASSERT_TRUE(pool.is_quiesced());

	const state s = state_holding("x");
	run_counter counter;
	pool.submit("a", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED), count_the_run, &counter);
	pool.submit("b", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED), count_the_run, &counter);

	// Not "probably not yet". park_all returned, so every worker is blocked at a
	// check-in, so no worker can have taken either task.
	EXPECT_EQ(pool.started(), 0u);
	EXPECT_TRUE(pool.completions().empty());

	pool.resume();
	std::vector<completion> drained;
	ASSERT_EQ(pool.completions().wait_and_drain(drained, 2), 2u);
	EXPECT_EQ(counter.runs, 2u);
}

TEST(UiWorkers, ParkingNestsSoAForkSiteInsideAParkedScopeIsNotADeadlock) {
	worker_pool pool{3};
	{
		parked_scope outer(pool);
		EXPECT_TRUE(pool.is_quiesced());
		{
			parked_scope inner(pool);
			pool.assert_quiesced();
			EXPECT_TRUE(pool.is_quiesced());
		}
		EXPECT_TRUE(pool.is_quiesced()) << "the outer hold outlives the inner release";
	}
	EXPECT_FALSE(pool.is_quiesced());
}

TEST(UiWorkers, AParkedPoolIsSafeToForkThrough) {
	// #91's requirement, executed rather than argued: a parked worker is blocked
	// in condition_variable::wait, which holds no mutex, so the child of a fork
	// taken here inherits every lock in this file unlocked. If a worker held one
	// at the instant of the fork, the child would block forever on a lock whose
	// owner does not exist in it, and the waitpid below would never return.
	worker_pool pool{3};
	held thinking;
	const state s = state_holding("a line");
	pool.submit("highlighter", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
	            poll_until_superseded, &thinking);
	thinking.entered.wait();

	parked_scope quiet(pool);
	pool.assert_quiesced();

	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0) {
		// Three locks, one per mutex this file owns.
		const bool taken = pool.is_quiesced() && pool.completions().size() == 1
		                && pool.messages().live() == 1;
		::_exit(taken ? 0 : 1);
	}

	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
}

// ---------------------------------------------------------------------------
// Shutdown (ADR-0007)
// ---------------------------------------------------------------------------

TEST(UiWorkers, ShutdownJoinsEveryWorkerAndFreesEveryMessage) {
	// Torn down mid-flight with results undrained and tasks still pending. The
	// assertion is the sanitized gate: ADR-0007 says the expected leak count is
	// exactly zero, with no suppression and no baseline.
	state s = state_holding("x");
	run_counter counter;
	{
		worker_pool pool{4};
		for (int i = 0; i < 200; ++i) {
			s.gen.bump();
			pool.submit(i % 2 == 0 ? "a" : "b", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED),
			            count_the_run, &counter);
		}
	}
	SUCCEED();
}

TEST(UiWorkers, APoolTornDownWhileParkedStillLetsItsWorkersOut) {
	state s = state_holding("x");
	run_counter counter;
	{
		worker_pool pool{3};
		pool.submit("a", snapshot_of(s, LESH_EVENT_BUFFER_CHANGED), count_the_run, &counter);
		pool.park_all();
		ASSERT_TRUE(pool.is_quiesced());
		// No resume(). The destructor has to release them anyway, or this hangs.
	}
	SUCCEED();
}
