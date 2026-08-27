// `Fiber*` - the step-0 probe for #82's fiber consolidation (#198), plus
// scheduler groups (#200).
//
// Five things are on trial here, and only the first is ordinary unit testing:
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
//      something rather than meaning LSan was not looking. It DOES break the
//      gate on Darwin, and #202 found out why #198 thought otherwise - see the
//      negative control below and the note in `src/fiber/scheduler.cpp`;
//   5. GROUPS - parking a SET with one bit, and the two ordering decisions
//      `scheduler.h` records: a resumed group's queued wakes replay in WAKE
//      order, and a group parked mid-tick loses its own remaining slices while
//      the rest of the tick's snapshot finishes. Both are promises about
//      determinism, so both have to be observable or they are not promises.
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
#include <string_view>
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

// THE RECURSION HAS TO SURVIVE THE OPTIMIZER, and until #203 it did not.
//
// The call was in TAIL POSITION, so Release rewrote it as a jump: one frame,
// reused for ever, a stack that never grows, and a child that exits cleanly -
// `AFiberThatOverflowsItsStackFaults` failed in the release binary while passing
// under the sanitized gate, which is the shape of failure that makes a test
// worth nothing. Two things that do NOT fix it, both tried:
//
//   `[[gnu::noinline]]` forbids INLINING, not the sibling-call rewrite; and
//   using the callee's result in an arithmetic expression is exactly the
//   ACCUMULATOR pattern LLVM's tail-recursion pass also folds into a loop.
//
// What does fix it is a SIDE EFFECT ORDERED AFTER THE CALL. The volatile store
// below has to happen once the callee has returned, so the call cannot become a
// jump and the frame cannot be reused - and the volatile read of `g_recurse`
// keeps the exit test unpredictable, so nothing bounds the depth either.
volatile int g_recurse = 1;
volatile unsigned char g_deep_sink = 0;

__attribute__((noinline)) unsigned char eat_stack(int depth) {
	volatile unsigned char frame[2048];
	frame[0] = static_cast<unsigned char>(depth);
	frame[sizeof(frame) - 1] = static_cast<unsigned char>(depth);
	if (g_recurse == 0)   // never; the compiler cannot know that
		return frame[sizeof(frame) - 1];
	const unsigned char deeper = eat_stack(depth + 1);
	g_deep_sink = static_cast<unsigned char>(deeper + frame[0]);
	return frame[0];
}

void overflow_by_recursion(scheduler& /*on*/, void* /*userdata*/) {
	g_deep_sink = eat_stack(0);
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
	//
	// IN RELEASE TOO, since #203. It used to pass only where the optimizer left
	// the recursion alone; see `eat_stack` for what made the call a jump and what
	// makes it a call again. The gate is the debug binary, but a guard-page test
	// that cannot fault in the build users run was measuring the compiler.
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
// The message may not point into the sender's own fiber stack (#203)
// ---------------------------------------------------------------------------

namespace {

// A static one, so the bytes outlive every stack in the process.
constexpr std::string_view kBorrowedFromNobody = "a line nobody's frame owns";

void send_a_view_of_something_that_outlives_me(scheduler& /*on*/, void* userdata) {
	auto* const inbox = static_cast<slot<std::string_view>*>(userdata);
	inbox->send(kBorrowedFromNobody);
}

} // namespace

TEST(FiberSlot, AViewOfBytesTheSenderDoesNotOwnIsFine) {
	// The negative control, and it is the whole point of the trait being narrow:
	// a view is not suspect because it is a view, it is suspect because of WHERE
	// it points. This one points at a string literal.
	scheduler sched;
	slot<std::string_view> inbox{sched};
	sched.spawn(&send_a_view_of_something_that_outlives_me, &inbox, "sender");
	EXPECT_FALSE(sched.tick());
	EXPECT_FALSE(inbox.empty());
}

TEST(FiberSlot, TheHostMaySendAViewOfItsOwnStack) {
	// The host's stack outlives every fiber on it - the loop is the sole resumer
	// and a fiber never runs while the frame that sent to it has returned - so
	// the check is about the RUNNING FIBER's stack and nothing else. `event_loop`
	// sends from exactly here.
	scheduler sched;
	slot<std::string_view> inbox{sched};
	char frame[32] = "typed at the prompt";
	inbox.send(std::string_view{frame, 19});
	EXPECT_FALSE(inbox.empty());
}

TEST(FiberSlot, AnOwningMessageIsNotInspectedAtAll) {
	// `std::string` has `data()` and `size()` and is NOT trivially copyable, so
	// the trait leaves it alone - which it must, because a short string's `data()`
	// points inside the object, i.e. at the sender's own frame, while the bytes
	// are moved into the slot and are perfectly safe. This test is that reasoning
	// written down: it would fail if the trait ever widened to "has data()".
	scheduler sched;
	slot<std::string> inbox{sched};
	std::string tiny = "short";
	inbox.send(std::move(tiny));
	EXPECT_FALSE(inbox.empty());
}

// ---------------------------------------------------------------------------
// Groups: parking a SET with one bit (#200)
// ---------------------------------------------------------------------------
//
// Five behaviours, and two of them are the decisions `scheduler.h` records:
// replayed wakes run in WAKE order (not spawn order), and a group parked
// mid-tick takes effect at once for its own members while the rest of the
// snapshot finishes. The tests are how those two stop being prose.

namespace {

struct group_state {
	char id = '?';
	std::string* log = nullptr;
};

// Logs its id on every slice, then PARKS. Nothing but a wake - direct, or
// replayed by `resume_group` - gives it another slice, so the log is the exact
// order the scheduler handed slices out.
void logs_then_parks(scheduler& on, void* userdata) {
	auto* const me = static_cast<group_state*>(userdata);
	for (;;) {
		me->log->push_back(me->id);
		on.park();
	}
}

// Logs its id on every slice and YIELDS, so it is perpetually runnable and the
// only thing that can stop it is its group's bit.
void logs_then_yields(scheduler& on, void* userdata) {
	auto* const me = static_cast<group_state*>(userdata);
	for (;;) {
		me->log->push_back(me->id);
		on.yield();
	}
}

struct group_parker_state {
	std::uint8_t target = 0;
	std::string* log = nullptr;
	int parks = 0;
};

// Parks somebody ELSE's group, from inside its own slice - the shape the
// grilling record's phase writer has ("the execution fiber on return"). Once,
// then it gets out of the way and logs nothing more.
void parks_another_group_once(scheduler& on, void* userdata) {
	auto* const me = static_cast<group_parker_state*>(userdata);
	me->log->push_back('P');
	on.park_group(me->target);
	++me->parks;
	for (;;)
		on.yield();
}

} // namespace

TEST(FiberGroups, AParkedGroupIsNotRunnableAndATickSkipsIt) {
	scheduler sched;
	std::string log;
	group_state host{'a', &log};
	group_state reactor{'R', &log};
	sched.spawn(&logs_then_yields, &host, "a", 0);
	sched.spawn(&logs_then_yields, &reactor, "R", 1);

	EXPECT_FALSE(sched.group_parked(0));
	EXPECT_FALSE(sched.group_parked(1));
	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "aR");

	sched.park_group(1);
	EXPECT_TRUE(sched.group_parked(1));
	EXPECT_FALSE(sched.group_parked(0));
	EXPECT_TRUE(sched.runnable()) << "group 0 still is";
	EXPECT_FALSE(sched.runnable(group_mask_of(1))) << "group 1 is not runnable while parked";

	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "aRa") << "only group 0 got a slice";
	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "aRaa");

	// Both verbs are idempotent, so a host deriving group bits from a phase can
	// write the same bits twice without bookkeeping.
	sched.park_group(1);
	EXPECT_TRUE(sched.group_parked(1));
	EXPECT_EQ(sched.resume_group(0), 0u) << "group 0 was never parked";

	EXPECT_EQ(sched.resume_group(1), 0u) << "it yielded rather than parked: no wake was queued";
	EXPECT_FALSE(sched.group_parked(1));
	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "aRaaaR") << "and its place in the arrival order never moved";
}

TEST(FiberGroups, SpawningIntoAParkedGroupWaitsForTheResumeWithNoQueuedWake) {
	scheduler sched;
	std::string log;
	group_state reactor{'R', &log};

	sched.park_group(1);
	const fiber& f = sched.spawn(&logs_then_yields, &reactor, "R", 1);
	EXPECT_TRUE(f.ready()) << "ready, and nonetheless not runnable";
	EXPECT_EQ(f.group(), 1u);
	EXPECT_FALSE(sched.runnable());

	EXPECT_FALSE(sched.tick());
	EXPECT_TRUE(log.empty());
	EXPECT_EQ(sched.queued_wakes(), 0u) << "it has never parked, so there is nothing to replay";

	EXPECT_EQ(sched.resume_group(1), 0u);
	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "R");
}

TEST(FiberGroups, WakesQueuedWhileParkedReplayInArrivalOrder) {
	// DECISION 1, and the reason it had to reach the tick at all: "replay in
	// arrival order" is the record's determinism promise, and a promise nothing
	// can observe is not one.
	scheduler sched;
	std::string log;
	group_state one{'1', &log};
	group_state two{'2', &log};
	group_state three{'3', &log};
	fiber& f1 = sched.spawn(&logs_then_parks, &one, "1", 1);
	fiber& f2 = sched.spawn(&logs_then_parks, &two, "2", 1);
	fiber& f3 = sched.spawn(&logs_then_parks, &three, "3", 1);

	EXPECT_FALSE(sched.tick()) << "each logged once and parked";
	EXPECT_EQ(log, "123") << "spawn order, because none of them had ever parked";
	ASSERT_TRUE(f1.parked());
	ASSERT_TRUE(f2.parked());
	ASSERT_TRUE(f3.parked());

	sched.park_group(1);

	// Woken in an order deliberately unlike spawn order.
	sched.wake(f3);
	sched.wake(f1);
	sched.wake(f2);
	EXPECT_EQ(sched.queued_wakes(1), 3u);
	EXPECT_EQ(sched.queued_wakes(), 3u);
	EXPECT_TRUE(f1.parked()) << "queued, not applied";
	EXPECT_FALSE(sched.runnable());
	EXPECT_FALSE(sched.tick()) << "a tick of a parked group is a tick of nothing";
	EXPECT_EQ(log, "123");

	// A repeat wake is a level and not an edge: it must not move f3 to the back.
	sched.wake(f3);
	EXPECT_EQ(sched.queued_wakes(1), 3u);

	EXPECT_EQ(sched.resume_group(1), 3u);
	EXPECT_EQ(sched.queued_wakes(), 0u);
	EXPECT_TRUE(sched.runnable());

	EXPECT_FALSE(sched.tick()) << "each ran once and parked again";
	EXPECT_EQ(log, "123312") << "WAKE order, not spawn order - which would have been 123";
}

TEST(FiberGroups, EachResumeReplaysOnlyItsOwnGroupAndTheRestKeepTheirPlaces) {
	// One queue for the whole scheduler, so a wake's position is its arrival
	// among ALL wakes - and another group's resume neither steals it nor
	// reorders it.
	scheduler sched;
	std::string log;
	group_state a{'a', &log};
	group_state b{'b', &log};
	group_state c{'c', &log};
	fiber& fa = sched.spawn(&logs_then_parks, &a, "a", 1);
	fiber& fb = sched.spawn(&logs_then_parks, &b, "b", 2);
	fiber& fc = sched.spawn(&logs_then_parks, &c, "c", 1);

	EXPECT_FALSE(sched.tick());
	EXPECT_EQ(log, "abc");

	sched.park_group(1);
	sched.park_group(2);
	sched.wake(fc);
	sched.wake(fb);
	sched.wake(fa);
	EXPECT_EQ(sched.queued_wakes(), 3u);
	EXPECT_EQ(sched.queued_wakes(1), 2u);
	EXPECT_EQ(sched.queued_wakes(2), 1u);

	EXPECT_EQ(sched.resume_group(1), 2u);
	EXPECT_EQ(sched.queued_wakes(), 1u) << "b's wake is still group 2's business";
	EXPECT_EQ(sched.queued_wakes(2), 1u);
	EXPECT_FALSE(sched.tick());
	EXPECT_EQ(log, "abcca") << "c before a: group 1's two wakes, in arrival order";

	EXPECT_EQ(sched.resume_group(2), 1u);
	EXPECT_EQ(sched.queued_wakes(), 0u);
	EXPECT_FALSE(sched.tick());
	EXPECT_EQ(log, "abccab");
}

TEST(FiberGroups, AMaskedTickRunsOnlyTheMaskedGroups) {
	// What lets step 1's host run "emitters" and "observers" as different sets
	// in the two positions of its loop.
	scheduler sched;
	std::string log;
	group_state emitter{'E', &log};
	group_state observer{'O', &log};
	group_state ungrouped{'U', &log};
	sched.spawn(&logs_then_yields, &emitter, "E", 1);
	sched.spawn(&logs_then_yields, &observer, "O", 2);
	sched.spawn(&logs_then_yields, &ungrouped, "U", 0);

	EXPECT_TRUE(sched.tick(group_mask_of(1)));
	EXPECT_EQ(log, "E");
	EXPECT_TRUE(sched.tick(group_mask_of(2)));
	EXPECT_EQ(log, "EO");

	EXPECT_TRUE(sched.tick(static_cast<std::uint8_t>(group_mask_of(1) | group_mask_of(2))));
	EXPECT_EQ(log, "EOEO") << "both masked groups, in arrival order";

	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "EOEOEOU") << "tick() is tick(all_groups)";

	// A masked tick reports on ITS mask: group 3 holds nothing, so it runs
	// nothing and says nothing is runnable, while the scheduler as a whole is.
	EXPECT_FALSE(sched.tick(group_mask_of(3)));
	EXPECT_EQ(log, "EOEOEOU");
	EXPECT_TRUE(sched.runnable());

	// A parked group is absent from a mask that names it, which is the property
	// that makes the two mechanisms composable rather than redundant.
	sched.park_group(1);
	EXPECT_FALSE(sched.tick(group_mask_of(1)));
	EXPECT_EQ(log, "EOEOEOU");
	EXPECT_TRUE(sched.tick(static_cast<std::uint8_t>(group_mask_of(1) | group_mask_of(2))));
	EXPECT_EQ(log, "EOEOEOUO");
}

TEST(FiberGroups, AGroupParkedByAFiberMidTickDoesNotStopTheRestOfTheSnapshot) {
	// DECISION 2. The snapshot bounds WHICH fibers may run and in WHAT order; it
	// never promised that each of them WILL. So the park lands at once for its
	// own members - `r2` never gets the slice its snapshot entry reserved - and
	// everything else in the snapshot finishes, `h` included.
	scheduler sched;
	std::string log;
	group_state r1{'x', &log};
	group_parker_state parker{1, &log, 0};
	group_state r2{'y', &log};
	group_state h{'h', &log};

	const fiber& f1 = sched.spawn(&logs_then_yields, &r1, "r1", 1);
	sched.spawn(&parks_another_group_once, &parker, "parker", 0);
	const fiber& f2 = sched.spawn(&logs_then_yields, &r2, "r2", 1);
	const fiber& fh = sched.spawn(&logs_then_yields, &h, "h", 0);

	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(parker.parks, 1);
	EXPECT_TRUE(sched.group_parked(1));
	EXPECT_EQ(log, "xPh")
		<< "the tick did not bail out: 'h' sits after the parker in the snapshot and ran";
	EXPECT_EQ(f1.slices(), 1u) << "group 1's earlier member had already had its slice";
	EXPECT_EQ(f2.slices(), 0u) << "and its later one is skipped the moment the bit is set";
	EXPECT_EQ(fh.slices(), 1u);

	// Nothing was queued: neither reactor ever parked, so there is nothing to
	// replay - the park cost group 1 exactly the slices it was not runnable for.
	EXPECT_EQ(sched.resume_group(1), 0u);
	EXPECT_TRUE(sched.tick());
	EXPECT_EQ(log, "xPhxyh") << "arrival order is unchanged: r1, parker (silent now), r2, h";
	EXPECT_EQ(f2.slices(), 1u);
}

TEST(FiberGroups, ASendToAParkedGroupsReceiverQueuesTheWakeAndKeepsTheValue) {
	// `slot.h` did not change for #200 and did not need to: `send` calls `wake`,
	// and whether a wake schedules or queues is the scheduler's business alone.
	scheduler sched;
	slot<int> inbox{sched};
	receiver_state state;
	state.inbox = &inbox;

	fiber& r = sched.spawn(&receive_once, &state, "receiver", 1);
	EXPECT_FALSE(sched.tick());
	ASSERT_TRUE(r.parked());
	EXPECT_TRUE(inbox.has_waiter());

	sched.park_group(1);
	inbox.send(7);
	EXPECT_TRUE(r.parked()) << "the wake was queued, not applied";
	EXPECT_EQ(sched.queued_wakes(1), 1u);
	EXPECT_FALSE(sched.runnable());
	EXPECT_FALSE(sched.tick());
	EXPECT_TRUE(state.delivered.empty());
	EXPECT_FALSE(inbox.empty()) << "and the value waited in the slot the whole time";

	EXPECT_EQ(sched.resume_group(1), 1u);
	EXPECT_FALSE(sched.tick());
	EXPECT_EQ(state.delivered, std::vector<int>{7});
	EXPECT_TRUE(r.finished());
}

#ifdef LESH_ENABLE_ASSERTS

namespace {

// THE FATAL ACTS LIVE IN FUNCTIONS, not inline in `EXPECT_DEATH`. The
// preprocessor groups by parentheses only, so a braced block containing a
// brace-initialiser reads as extra macro arguments and does not compile - which
// is why the watchdog death test above has no commas in it either.
//
// They live INSIDE the `#ifdef` for the release build's sake: with the asserts
// compiled out there is no death test to call them, and `-Wunused-function` is
// an error.

void slice_a_fiber_whose_group_is_parked() {
	scheduler sched;
	std::string log;
	group_state reactor{'R', &log};
	fiber& f = sched.spawn(&logs_then_yields, &reactor, "R", 1);
	sched.park_group(1);
	sched.run_one_slice(f);
}

void parks_its_own_group(scheduler& on, void* userdata) {
	auto* const me = static_cast<group_parker_state*>(userdata);
	on.park_group(me->target);
	on.park();
}

void park_the_group_the_running_fiber_is_in() {
	scheduler sched;
	std::string log;
	group_parker_state its_own{1, &log, 0};
	sched.spawn(&parks_its_own_group, &its_own, "its-own", 1);
	(void)sched.tick();
}

void send_a_view_of_my_own_stack(scheduler& /*on*/, void* userdata) {
	auto* const inbox = static_cast<slot<std::string_view>*>(userdata);
	// `volatile` so the frame is real storage and not something the optimizer
	// folds into the literal it was copied from.
	volatile char frame[64] = "a line that dies at the next switch";
	inbox->send(std::string_view{const_cast<const char*>(frame), 35});
}

void send_a_message_pointing_into_my_own_stack() {
	scheduler sched;
	slot<std::string_view> inbox{sched};
	sched.spawn(&send_a_view_of_my_own_stack, &inbox, "sender");
	(void)sched.tick();
}

void spawn_into_a_ninth_group() {
	scheduler sched;
	std::string log;
	group_state stray{'?', &log};
	sched.spawn(&logs_then_yields, &stray, "stray", group_count);
}

} // namespace

TEST(FiberGroupsDeathTest, AnExplicitSliceOfAParkedGroupFiberAsserts) {
	// `run_one_slice` is the host's other door - step 1 uses it for the "reactors
	// before and after the UI part" ordering - and it must not be a way round the
	// bit. This assert is also why decision 2 could only go one way.
	EXPECT_DEATH(slice_a_fiber_whose_group_is_parked(),
	             "a fiber in a parked group is not runnable");
}

TEST(FiberGroupsDeathTest, AFiberCannotParkTheGroupItIsRunningIn) {
	// The ticket's invariant - "a fiber that was mid-slice cannot be in a parked
	// group" - held by the narrowest assert that holds it.
	EXPECT_DEATH(park_the_group_the_running_fiber_is_in(),
	             "a fiber cannot park the group it is running in");
}

TEST(FiberSlotDeathTest, AMessagePointingIntoTheSendersOwnStackIsRefused) {
	// coost's `on_stack` check, and the reason it earns its keep: this send is
	// legal C++, the slot holds the view happily, and the bug appears in whatever
	// reads it after the sender's next yield - on one machine and not another,
	// because it depends on what ran next. The assert moves the failure to the
	// line that did the wrong thing.
	EXPECT_DEATH(send_a_message_pointing_into_my_own_stack(),
	             "a message may not point into the sending fiber's own stack");
}

TEST(FiberGroupsDeathTest, ThereAreEightGroupsAndNoMore) {
	EXPECT_EQ(group_count, 8u);
	EXPECT_EQ(all_groups, 0xffu);
	EXPECT_DEATH(spawn_into_a_ninth_group(), "up to 8 groups");
}

#endif // LESH_ENABLE_ASSERTS

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
	// AND THAT IS WHAT IT FOUND, one step later than #198 thought (#202). #198
	// recorded "Darwin's LSan root set does cover the anonymous mapping"; it does
	// NOT. What made this pass then was minicoro's ASan defect - it left the
	// THREAD's recorded stack bounds pointing at the fiber's stack after a yield,
	// so the leak check scanned the fiber stack as if it were the thread's and
	// found the block. `scheduler.cpp` corrects those bounds now, because
	// `__asan_handle_no_return` needs them right, and this test went red the moment
	// it did. It passes again because `scheduler::spawn` registers every live fiber
	// stack as an LSan ROOT REGION - the documented interface, and tracing rather
	// than suppressing, so what a parked frame points at stays honestly reachable.
	// See the LeakSanitizer note at the top of `src/fiber/scheduler.cpp`.
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
	// destruction). The only pointer to the block goes with the mapping - and so
	// does the stack's registration as an LSan root region (#202), which is why
	// this stays a leak now that a LIVE fiber stack is traced - so the process
	// leaks 500 KB and must exit non-zero.
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
