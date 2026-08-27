// `Fiber*` - the step-0 probe for #82's fiber consolidation (#198).
//
// Four things are on trial here, and only the first is ordinary unit testing:
//
//   1. the switcher and the scheduler do what they say - spawn, yield, park,
//      wake, finish, in a deterministic order;
//   2. THE GUARD PAGE IS REAL. A write one word below a fiber's stack must
//      FAULT, not land silently in minicoro's bookkeeping struct. That is the
//      one test that notices a re-vendor moving the block layout, and it runs in
//      a forked child because the point is that the process dies;
//   3. `slot`'s supersede contract - the intermediate value is never delivered;
//   4. LEAKSANITIZER AND PARKED FIBER STACKS, in both directions. A heap block
//      whose only pointer lives on a parked fiber's mmap'd stack must NOT be
//      reported, and the same block must BE reported once that stack is gone.
//      The research note calls the first one "the single most likely way fibers
//      break the gate on CI"; the second one is what makes a green gate mean
//      something rather than meaning LSan was not looking.
//
// Everything here runs under `ctest --preset debug`, i.e. under
// ASan/UBSan/LSan. That is deliberate: a guard-page test that only runs in
// release proves nothing about the configuration the gate uses.

#include "fiber/scheduler.h"
#include "fiber/slot.h"
#include "fiber/stack.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace lesh::fiber;
using namespace std::chrono_literals;

namespace {

void finish_immediately(scheduler& /*on*/, void* userdata) {
	if (userdata != nullptr)
		++*static_cast<int*>(userdata);
}

// Runs `sched.tick()` until nothing is runnable, with a hard cap so that a
// scheduler bug is a failed test rather than a hung suite.
std::size_t drain(scheduler& sched, std::size_t cap = 100) {
	std::size_t ticks = 0;
	while (sched.runnable() && ticks < cap) {
		(void)sched.tick();
		++ticks;
	}
	return ticks;
}

} // namespace

// ---------------------------------------------------------------------------
// The switcher and the scheduler
// ---------------------------------------------------------------------------

namespace {

void round_trip_body(scheduler& on, void* userdata) {
	auto* const marks = static_cast<std::vector<std::string>*>(userdata);
	marks->emplace_back("fiber: entered");
	on.yield();
	marks->emplace_back("fiber: resumed");
	on.yield();
	marks->emplace_back("fiber: returning");
}

} // namespace

TEST(FiberSwitch, RoundTripReturnsToTheHostAtEveryYield) {
	scheduler sched;
	std::vector<std::string> marks;

	fiber& f = sched.spawn(&round_trip_body, &marks, "round-trip");
	EXPECT_TRUE(f.ready());
	EXPECT_EQ(f.slices(), 0u);
	marks.emplace_back("host: before the first slice");

	EXPECT_TRUE(sched.tick()) << "a fiber that yielded is still runnable";
	marks.emplace_back("host: after the first slice");
	EXPECT_TRUE(f.ready());
	EXPECT_EQ(f.slices(), 1u);

	EXPECT_TRUE(sched.tick());
	marks.emplace_back("host: after the second slice");

	EXPECT_FALSE(sched.tick()) << "the body returned, so nothing is runnable";
	EXPECT_TRUE(f.finished());
	EXPECT_EQ(f.slices(), 3u);

	const std::vector<std::string> expected = {
		"host: before the first slice",
		"fiber: entered",
		"host: after the first slice",
		"fiber: resumed",
		"host: after the second slice",
		"fiber: returning",
	};
	EXPECT_EQ(marks, expected);
}

TEST(FiberSwitch, SpawnRunsTheBodyOnceAndFinishes) {
	scheduler sched;
	int runs = 0;

	fiber& f = sched.spawn(&finish_immediately, &runs, "spawn-finish");
	EXPECT_EQ(sched.fiber_count(), 1u);
	EXPECT_TRUE(sched.runnable());
	EXPECT_EQ(runs, 0) << "spawn does not start the body; a tick does";

	EXPECT_FALSE(sched.tick());
	EXPECT_EQ(runs, 1);
	EXPECT_TRUE(f.finished());
	EXPECT_FALSE(sched.runnable());

	// A finished fiber is inert rather than an error: `run_one_slice` on one is
	// what a stale wake from a channel would do.
	sched.run_one_slice(f);
	EXPECT_EQ(runs, 1);
	EXPECT_EQ(f.slices(), 1u);
}

namespace {

struct ordering_state {
	char id = '?';
	int yields = 0;
	std::string* log = nullptr;
};

void ordering_body(scheduler& on, void* userdata) {
	auto* const me = static_cast<ordering_state*>(userdata);
	for (int i = 0; i < 3; ++i) {
		me->log->push_back(me->id);
		++me->yields;
		on.yield();
	}
	me->log->push_back(static_cast<char>(me->id + ('a' - 'A')));   // lowercase: done
}

} // namespace

TEST(FiberSwitch, ThreeFibersGetOneSlicePerTickInSpawnOrder) {
	scheduler sched;
	std::string log;
	ordering_state a{'A', 0, &log};
	ordering_state b{'B', 0, &log};
	ordering_state c{'C', 0, &log};

	sched.spawn(&ordering_body, &a, "A");
	sched.spawn(&ordering_body, &b, "B");
	sched.spawn(&ordering_body, &c, "C");

	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "ABC") << "one slice each, in spawn order";
	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "ABCABC");
	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "ABCABCABC");

	// Fourth tick: each body falls out of its loop and returns.
	EXPECT_FALSE(sched.tick());
	EXPECT_EQ(log, "ABCABCABCabc");
	EXPECT_EQ(a.yields, 3);
	EXPECT_EQ(b.yields, 3);
	EXPECT_EQ(c.yields, 3);
}

namespace {

struct waker_state {
	fiber* target = nullptr;
	scheduler* sched = nullptr;
	bool woke = false;
};

void parks_forever(scheduler& on, void* /*userdata*/) {
	for (;;)
		on.park();
}

void wakes_the_other(scheduler& on, void* userdata) {
	auto* const s = static_cast<waker_state*>(userdata);
	on.wake(*s->target);
	s->woke = true;
}

} // namespace

TEST(FiberSwitch, AFiberWokenDuringATickWaitsForTheNextTick) {
	// The snapshot rule, asserted rather than merely documented: it is what makes
	// a tick bounded work and its order reproducible, and step 1's loop leans on
	// both.
	scheduler sched;
	fiber& sleeper = sched.spawn(&parks_forever, nullptr, "sleeper");
	waker_state w;
	w.target = &sleeper;
	w.sched = &sched;
	sched.spawn(&wakes_the_other, &w, "waker");

	EXPECT_TRUE(sched.tick()) << "the sleeper was woken, so something is runnable";
	EXPECT_TRUE(w.woke);
	EXPECT_TRUE(sleeper.ready());
	EXPECT_EQ(sleeper.slices(), 1u) << "it parked in this tick and must not run twice in it";

	EXPECT_FALSE(sched.tick()) << "it parked again, so nothing is runnable";
	EXPECT_EQ(sleeper.slices(), 2u);
	EXPECT_TRUE(sleeper.parked());

	// Waking something that is not parked is a no-op, not an error - `slot::send`
	// does not know how far its receiver got.
	sched.wake(sleeper);
	sched.wake(sleeper);
	EXPECT_TRUE(sleeper.ready());
	EXPECT_EQ(drain(sched, 3), 1u) << "one slice, and it parks again";
}

// ---------------------------------------------------------------------------
// Stacks and the guard page
// ---------------------------------------------------------------------------

TEST(FiberStack, SizeIsTarantoolsAndTheGuardIsOnePageDirectlyBelow) {
	EXPECT_EQ(default_stack_size(), built_under_asan() ? 1024u * 1024u : 512u * 1024u)
		<< "512 KB, 1 MB under ASan - SetFiberStackSize.cmake:11-18";
	EXPECT_EQ(default_stack_size() % page_size(), 0u);

	scheduler sched;
	fiber& f = sched.spawn(&finish_immediately, nullptr, "sizes");

	const stack_extents e = f.stack();
	ASSERT_NE(e.stack_base, nullptr);
	EXPECT_EQ(e.stack_size, default_stack_size());
	EXPECT_EQ(e.guard_size, page_size());
	EXPECT_EQ(e.guard_base + e.guard_size, e.stack_base) << "the guard is DIRECTLY below";
	EXPECT_EQ(reinterpret_cast<std::uintptr_t>(e.stack_base) % page_size(), 0u)
		<< "a page-aligned stack base is what makes the guard one whole page";

	EXPECT_FALSE(sched.tick());
	EXPECT_TRUE(f.finished());
	EXPECT_EQ(f.stack().stack_base, nullptr) << "a finished fiber's stack is unmapped";
}

TEST(FiberStack, ASmallerStackIsHonouredAndStillGuarded) {
	scheduler_options opts;
	opts.stack_bytes = 64u * 1024u;
	scheduler sched{opts};
	fiber& f = sched.spawn(&finish_immediately, nullptr, "small");

	const stack_extents e = f.stack();
	ASSERT_NE(e.stack_base, nullptr);
	EXPECT_EQ(e.stack_size, 64u * 1024u);
	EXPECT_EQ(e.guard_size, page_size());
	EXPECT_EQ(e.guard_base + e.guard_size, e.stack_base);
	EXPECT_FALSE(sched.tick());
}

namespace {

// WHAT THE CHILD REPORTS BY EXITING. Three codes, and the whole test is which
// one comes back:
//
//   kSegvExitCode / kBusExitCode - it took a memory fault. THE GUARD IS THERE.
//   kNoFaultExitCode            - the write below the stack landed somewhere
//                                 writable and the child carried on. THERE IS NO
//                                 GUARD, and a stack overflow is silently eating
//                                 minicoro's bookkeeping struct.
//   0                           - the body never ran at all.
constexpr int kSegvExitCode = 101;
constexpr int kBusExitCode = 102;
constexpr int kNoFaultExitCode = 111;

// THE CHILD CATCHES THE FAULT ITSELF, rather than letting the wait status or an
// ASan report describe it. Both of those turned out to be environment noise
// worth designing around:
//
//   - ASan installs its own SIGSEGV handler, reports, and then (on Darwin, where
//     `abort_on_error` defaults on) calls `abort()`, so the wait status says
//     SIGABRT and the words "SEGV" only ever appear in the report text;
//   - and in a FORKED child, ASan's stack-overflow path does not survive at all:
//     measured on this machine, the deep-recursion case dies of SIGILL with no
//     report written. The fault is real - the same code in an unforked process
//     prints `stack-overflow on address ...` inside the guard page - but what
//     kills the process is ASan's own machinery giving up post-fork.
//
// A handler of our own on an alternate stack answers the actual question - "did
// this address fault?" - in one exit code, on any platform, with or without a
// sanitizer, and prints nothing. `_exit` from a signal handler is
// async-signal-safe; nothing else here needs to be.
alignas(16) unsigned char g_fault_stack[64 * 1024];

extern "C" void exit_on_fault(int sig) {
	::_exit(sig == SIGBUS ? kBusExitCode : kSegvExitCode);
}

void catch_faults_in_this_process() {
	stack_t alt{};
	alt.ss_sp = g_fault_stack;
	alt.ss_size = sizeof g_fault_stack;
	alt.ss_flags = 0;
	// An ALTERNATE STACK is not optional here: a stack-overflow fault arrives
	// with no room left to run a handler on, which is why the kernel offers one.
	(void)::sigaltstack(&alt, nullptr);

	struct sigaction sa{};
	sa.sa_handler = &exit_on_fault;
	sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);   // a macro on Darwin, so it cannot be `::`-qualified
	(void)::sigaction(SIGSEGV, &sa, nullptr);
	(void)::sigaction(SIGBUS, &sa, nullptr);
}

struct child_result {
	int status = -1;        // as `waitpid` reports it
	std::string output;     // whatever the child said on stdout+stderr
};

// Runs `body` in a forked child and reports how it ended. A CHILD BECAUSE THE
// POINT IS THAT THE PROCESS DIES: `lesh_tests` has to survive to say so.
//
// Output is captured rather than discarded so that an unexpected death is
// diagnosable from the failure message instead of from a rerun.
child_result run_in_child(void (*body)()) {
	int fds[2] = {-1, -1};
	if (::pipe(fds) != 0)
		return {};

	const ::pid_t child = ::fork();
	if (child < 0) {
		::close(fds[0]);
		::close(fds[1]);
		return {};
	}
	if (child == 0) {
		::close(fds[0]);
		(void)::dup2(fds[1], STDOUT_FILENO);
		(void)::dup2(fds[1], STDERR_FILENO);
		::close(fds[1]);
		catch_faults_in_this_process();
		body();
		::_exit(kNoFaultExitCode);
	}

	::close(fds[1]);
	child_result out;
	// Drained BEFORE the wait: a sanitizer report is larger than a pipe buffer,
	// and a child blocked writing into a full pipe while we block in `waitpid`
	// would be a hang rather than a test.
	char buffer[4096];
	for (;;) {
		const ::ssize_t got = ::read(fds[0], buffer, sizeof buffer);
		if (got > 0)
			out.output.append(buffer, static_cast<std::size_t>(got));
		else if (got == 0 || errno != EINTR)
			break;
	}
	::close(fds[0]);
	if (::waitpid(child, &out.status, 0) != child)
		out.status = -1;
	return out;
}

void expect_faulted(const child_result& r, const char* what) {
	ASSERT_NE(r.status, -1) << what << ": fork/pipe/waitpid failed";
	ASSERT_TRUE(WIFEXITED(r.status))
		<< what << ": the child was killed by signal " << WTERMSIG(r.status)
		<< " instead of reporting a fault through its own handler."
		<< "\n--- child output ---\n" << r.output << "--- end ---";

	const int code = WEXITSTATUS(r.status);
	EXPECT_NE(code, kNoFaultExitCode)
		<< what << ": THE WRITE BELOW THE STACK WENT THROUGH. There is no guard "
		           "page, and a stack overflow is silently eating minicoro's "
		           "bookkeeping struct. Re-check src/fiber/stack.cpp against the "
		           "pinned commit in third_party/minicoro/README.lesh.md.";
	EXPECT_TRUE(code == kSegvExitCode || code == kBusExitCode)
		<< what << ": exited " << code << ", which is neither SIGSEGV nor SIGBUS."
		<< "\n--- child output ---\n" << r.output << "--- end ---";
}

struct guard_probe {
	volatile unsigned char* target = nullptr;
};

void write_at_target(scheduler& /*on*/, void* userdata) {
	auto* const probe = static_cast<guard_probe*>(userdata);
	*probe->target = 0xA5;   // expected to fault
}

void write_just_below_the_stack() {
	scheduler_options opts;
	opts.stack_bytes = 64u * 1024u;
	scheduler sched{opts};
	guard_probe probe;
	fiber& f = sched.spawn(&write_at_target, &probe, "guard-probe");

	// Sixteen bytes below the stack's low end. WITH a guard page this is inside
	// PROT_NONE and faults. WITHOUT one it is inside the padded `storage` region
	// and then, a little further down, inside `_mco_context` and `mco_coro`
	// itself - a silent write that upstream notices, if at all, by a magic-number
	// check at the next yield.
	probe.target = const_cast<volatile unsigned char*>(f.stack().stack_base) - 16;
	(void)sched.tick();
}

__attribute__((noinline)) void eat_stack(int depth) {
	volatile unsigned char frame[2048];
	frame[0] = static_cast<unsigned char>(depth);
	frame[sizeof(frame) - 1] = static_cast<unsigned char>(depth);
	if (frame[0] == 0xFF)   // never; keeps the recursion from being folded away
		return;
	eat_stack(depth + 1);
}

void overflow_by_recursion(scheduler& /*on*/, void* /*userdata*/) {
	eat_stack(0);
}

void recurse_off_the_bottom_of_the_stack() {
	scheduler_options opts;
	opts.stack_bytes = 64u * 1024u;
	scheduler sched{opts};
	sched.spawn(&overflow_by_recursion, nullptr, "overflow");
	(void)sched.tick();
}

} // namespace

TEST(FiberGuardPage, AWriteJustBelowTheStackFaultsInsteadOfCorruptingTheNeighbour) {
	// THE discriminating test. An unbounded recursion (below) would fault with or
	// without a guard - it eventually runs off the bottom of the mapping - so it
	// cannot tell us whether the guard works. A bounded write 16 bytes below the
	// stack base can: that address is PROT_NONE if and only if the guard is
	// exactly where `src/fiber/stack.cpp` computed it.
	expect_faulted(run_in_child(&write_just_below_the_stack), "write below the stack");
}

TEST(FiberGuardPage, AFiberThatOverflowsItsStackFaults) {
	// The realistic shape: deep recursion on a fiber stack. This is what the
	// research note asked for - "a test that writes past the stack and expects
	// SIGSEGV, not corruption" - and with the test above it says both halves:
	// an overflow dies, and it dies at the guard rather than after eating the
	// bookkeeping.
	expect_faulted(run_in_child(&recurse_off_the_bottom_of_the_stack), "stack overflow");
}

// ---------------------------------------------------------------------------
// slot<T>
// ---------------------------------------------------------------------------

namespace {

struct receiver_state {
	slot<int>* inbox = nullptr;
	std::vector<int> delivered;
	std::vector<int> completed;
	int abandoned = 0;
};

void receive_once(scheduler& /*on*/, void* userdata) {
	auto* const s = static_cast<receiver_state*>(userdata);
	s->delivered.push_back(s->inbox->recv());
}

// The reactor shape from the record: recv, compute in slices, poll `superseded`
// at every slice boundary, abandon and loop back to `recv` when it fires. A
// negative value is the "stop" sentinel so the fiber can finish tidily.
void receive_and_compute(scheduler& on, void* userdata) {
	auto* const s = static_cast<receiver_state*>(userdata);
	for (;;) {
		const int value = s->inbox->recv();
		s->delivered.push_back(value);
		if (value < 0)
			return;

		const slot<int>::token work = s->inbox->in_flight();
		EXPECT_FALSE(work.superseded()) << "a freshly delivered value is not stale";

		bool completed = true;
		for (int step = 0; step < 3; ++step) {
			on.yield();                         // a `kPollEvery` boundary
			if (work.superseded()) {
				++s->abandoned;
				completed = false;
				break;
			}
		}
		if (completed)
			s->completed.push_back(value);
	}
}

} // namespace

TEST(FiberSlot, RecvParksWhenEmptyAndSendWakesTheReceiver) {
	scheduler sched;
	slot<int> inbox{sched};
	receiver_state state;
	state.inbox = &inbox;

	fiber& r = sched.spawn(&receive_once, &state, "receiver");
	EXPECT_FALSE(sched.tick()) << "it parked in recv, so nothing is runnable";
	EXPECT_TRUE(r.parked());
	EXPECT_TRUE(inbox.has_waiter());
	EXPECT_TRUE(state.delivered.empty());

	inbox.send(7);
	EXPECT_TRUE(r.ready()) << "send wakes the parked receiver";
	EXPECT_TRUE(sched.runnable());
	EXPECT_FALSE(inbox.has_waiter());

	EXPECT_FALSE(sched.tick());
	EXPECT_TRUE(r.finished());
	EXPECT_EQ(state.delivered, std::vector<int>{7});
	EXPECT_TRUE(inbox.empty());
	EXPECT_EQ(inbox.sends(), 1u);
	EXPECT_EQ(inbox.superseded_sends(), 0u);
}

TEST(FiberSlot, SendingIntoAFullSlotOverwritesAndCounts) {
	scheduler sched;
	slot<int> inbox{sched};
	receiver_state state;
	state.inbox = &inbox;

	// No receiver has run yet, so all three land in the same one-deep slot.
	inbox.send(1);
	inbox.send(2);
	inbox.send(3);
	EXPECT_EQ(inbox.sends(), 3u);
	EXPECT_EQ(inbox.superseded_sends(), 2u) << "two unconsumed values were dropped";
	EXPECT_FALSE(inbox.empty());

	sched.spawn(&receive_once, &state, "receiver");
	EXPECT_FALSE(sched.tick());
	EXPECT_EQ(state.delivered, std::vector<int>{3}) << "latest wins";
}

TEST(FiberSlot, AReceiverMidComputeSeesSupersededAndGetsTheNewestValue) {
	// The ticket's named case, and the whole of v1's cancellation.
	scheduler sched;
	slot<int> inbox{sched};
	receiver_state state;
	state.inbox = &inbox;

	fiber& r = sched.spawn(&receive_and_compute, &state, "reactor");
	EXPECT_FALSE(sched.tick());
	ASSERT_TRUE(r.parked());

	inbox.send(1);
	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(state.delivered, std::vector<int>{1});
	EXPECT_FALSE(inbox.superseded()) << "nothing has been sent since delivery";

	// Two more, while the receiver is mid-compute on 1. The slot was EMPTY when 2
	// arrived (1 had already been consumed) and FULL when 3 arrived, so only the
	// second of these is an overwrite - but both supersede the in-flight token,
	// which is the distinction `slot.h` explains at length.
	inbox.send(2);
	EXPECT_TRUE(inbox.superseded());
	inbox.send(3);
	EXPECT_EQ(inbox.superseded_sends(), 1u);

	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(state.abandoned, 1) << "it noticed at its next poll and gave up on 1";
	EXPECT_EQ(state.delivered, (std::vector<int>{1, 3}))
		<< "2 WAS NEVER DELIVERED: it was overwritten in the slot before any recv";
	EXPECT_TRUE(state.completed.empty()) << "1 was abandoned, 3 is still in flight";

	// Let 3 run to completion - three more polls - and then park in `recv` again.
	EXPECT_TRUE(sched.tick());
	EXPECT_TRUE(sched.tick());
	EXPECT_FALSE(sched.tick()) << "3 completed and the fiber parked waiting for more";
	EXPECT_EQ(state.completed, std::vector<int>{3});

	inbox.send(-1);
	EXPECT_LE(drain(sched), 4u);
	EXPECT_TRUE(r.finished());
	EXPECT_EQ(state.delivered, (std::vector<int>{1, 3, -1}));
}

TEST(FiberSlot, AStaleTokenAndADefaultTokenBothReadSuperseded) {
	scheduler sched;
	slot<int> inbox{sched};
	receiver_state state;
	state.inbox = &inbox;

	const slot<int>::token never_issued;
	EXPECT_FALSE(never_issued.valid());
	EXPECT_TRUE(never_issued.superseded())
		<< "'I do not know what I am holding' has to mean 'abandon it'";

	inbox.send(1);
	sched.spawn(&receive_once, &state, "receiver");
	EXPECT_FALSE(sched.tick());
	const slot<int>::token first = inbox.in_flight();
	EXPECT_TRUE(first.valid());
	EXPECT_FALSE(first.superseded());

	inbox.send(2);
	EXPECT_TRUE(first.superseded()) << "a token from an earlier recv never comes back";
}

// ---------------------------------------------------------------------------
// The debug watchdog
// ---------------------------------------------------------------------------

TEST(FiberWatchdog, TheDefaultBudgetIsFiftyMilliseconds) {
	const scheduler sched;
	EXPECT_EQ(sched.options().watchdog_budget, std::chrono::nanoseconds{50ms});
	EXPECT_EQ(sched.options().on_overrun, watchdog_action::log)
		<< "the shell logs; the test binary is what asks for an abort";
}

#ifndef NDEBUG

namespace {

// Deliberately blocking work: exactly the sin the watchdog exists to name.
void hog_a_slice(scheduler& /*on*/, void* userdata) {
	const auto budget = *static_cast<std::chrono::nanoseconds*>(userdata);
	const auto start = std::chrono::steady_clock::now();
	volatile std::uint64_t sink = 0;
	while (std::chrono::steady_clock::now() - start < budget * 4)
		sink = sink + 1;
	(void)sink;
}

} // namespace

TEST(FiberWatchdog, ASliceThatRunsPastItsBudgetIsCounted) {
	// A 2 ms budget rather than the 50 ms default, so the suite stays in
	// milliseconds. The mechanism is the same one; the number is the default's
	// business and the test above asserts that separately.
	auto budget = std::chrono::nanoseconds{2ms};
	scheduler_options opts;
	opts.watchdog_budget = budget;
	opts.on_overrun = watchdog_action::log;
	scheduler sched{opts};

	sched.spawn(&hog_a_slice, &budget, "hog");
	EXPECT_EQ(sched.watchdog_overruns(), 0u);
	EXPECT_FALSE(sched.tick());
	EXPECT_EQ(sched.watchdog_overruns(), 1u);
}

TEST(FiberWatchdogDeathTest, AbortsWhenTheOwnerAskedItTo) {
	EXPECT_DEATH(
		{
			auto budget = std::chrono::nanoseconds{2ms};
			scheduler_options opts;
			opts.watchdog_budget = budget;
			opts.on_overrun = watchdog_action::abort_;
			scheduler sched{opts};
			sched.spawn(&hog_a_slice, &budget, "hog");
			(void)sched.tick();
		},
		"fiber watchdog");
}

#endif // NDEBUG

// ---------------------------------------------------------------------------
// LeakSanitizer, in both directions
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kLsanBlockBytes = 500u * 1024u;

struct lsan_probe {
	bool allocated = false;
	bool freed = false;
};

// Allocates a block, keeps the ONLY pointer to it in this frame - on this
// fiber's mmap'd stack - and parks. `volatile` on the pointer so that the
// pointer itself is genuinely in the frame rather than in a register the
// compiler happened to have spare.
void hold_a_block_and_park(scheduler& on, void* userdata) {
	auto* const probe = static_cast<lsan_probe*>(userdata);
	unsigned char* volatile block = static_cast<unsigned char*>(std::malloc(kLsanBlockBytes));
	if (block == nullptr)
		return;
	std::memset(block, 0x5A, kLsanBlockBytes);   // committed, not merely reserved
	probe->allocated = true;
	on.park();
	std::free(block);
	probe->freed = true;
}

// Storage for a scheduler that must still exist, with its fiber still parked,
// when LSan runs at process exit.
//
// NOT a heap allocation (it would be a leak candidate itself) and NOT an object
// with a destructor (a static with one is registered for destruction at exit and
// would race the leak check). A raw aligned buffer in .bss is a root LSan
// certainly scans and an object it will never destroy.
alignas(scheduler) unsigned char g_survivor_storage[sizeof(scheduler)];

} // namespace

TEST(FiberLsan, ABlockHeldOnlyByAParkedFiberStackIsNotReported) {
	// THE NEGATIVE CONTROL, and the research note's "single most likely way
	// fibers break the gate": the 500 KB block below is reachable from nowhere in
	// the process except a frame on an `mmap`'d fiber stack. If LeakSanitizer's
	// root set does not cover that mapping, this test passes and then the BINARY
	// fails at exit with a 500 KB leak - which is the finding, not a bug to
	// suppress.
	//
	// Nothing here is freed and nothing is destroyed, on purpose.
	static lsan_probe probe;
	auto* const survivor = new (g_survivor_storage) scheduler();

	fiber& held = survivor->spawn(&hold_a_block_and_park, &probe, "lsan-negative");
	EXPECT_FALSE(survivor->tick()) << "it parked, so nothing is runnable";
	EXPECT_TRUE(held.parked());
	EXPECT_TRUE(probe.allocated);

	// The ticket's shape: tick again with the fiber still parked, then leave.
	EXPECT_FALSE(survivor->tick());
	EXPECT_TRUE(held.parked());
	EXPECT_FALSE(probe.freed);
}

TEST(FiberLsanPositiveControl, DISABLED_DroppedBlockIsReported) {
	// THE POSITIVE CONTROL, so that the test above passing means LSan looked
	// rather than meaning LSan was asleep. Same allocation, same fiber frame -
	// and then the scheduler is destroyed while the fiber is still parked, which
	// unmaps the stack WITHOUT unwinding it (v1 has no cancellation by
	// destruction). The only pointer to the block goes with the mapping, so the
	// process now leaks 500 KB and must exit non-zero.
	//
	// DISABLED_ so the ordinary suite does not run it. `fiber_lsan_positive_control`
	// in CMakeLists.txt runs exactly this one test with WILL_FAIL TRUE: the case
	// passes when the process fails, and it FAILS - loudly, on a green-looking
	// gate - the day LeakSanitizer stops reporting this shape.
	static lsan_probe probe;
	{
		scheduler sched;
		fiber& held = sched.spawn(&hold_a_block_and_park, &probe, "lsan-positive");
		EXPECT_FALSE(sched.tick());
		EXPECT_TRUE(held.parked());
		EXPECT_TRUE(probe.allocated);
	}
	EXPECT_FALSE(probe.freed) << "a parked fiber's stack is not unwound";
}
