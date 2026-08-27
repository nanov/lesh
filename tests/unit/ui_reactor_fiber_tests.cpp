#include "fiber/scheduler.h"
#include "leshper/abi.h"
#include "leshper/editor.h"
#include "leshper/keymap.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "ui/loop.h"
#include "ui/reactor_call.h"
#include "ui/shell_side.h"

#include "ui_fakes.h"

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace lesh;
using namespace lesh::leshper;
using namespace lesh::ui;
using lesh::testing::fake_tty;

// THE REACTOR FIBERS (#202, step 1d of #145). What `ui_workers_tests.cpp` was.
//
// The suite it replaces was a CONCURRENCY suite: fourteen lines of gates and
// condition variables at the top, a standing rule that not one sleep appears in
// the file, and two tests whose honest assertion was "park_all returns" because
// their subject was a compute that never ends on its own. None of that is here,
// and its absence is the ticket: cancellation, supersede and ownership are the
// same four properties, asserted on a single thread where the scheduler decides
// when each side runs. Every test below is a sequence, not a race - there is
// nothing to wait for and nothing that can pass by luck.
//
// WHAT REPLACED THE GATE is the reactor's own cancellation poll. A `walker`
// below polls once per unit of work, and since #202 a poll is also a YIELD - so
// `units` is exactly how many slices a compute takes, and a test says "get this
// reactor mid-walk" by taking one turn.

namespace {

// A loop over a pipe, with the terminal management off - there is nothing to
// manage, and leaving it on would put `tcsetpgrp` in the path of every test.
loop_options pipe_options() {
	loop_options options;
	options.manage_terminal = false;
	options.prompt = "> ";
	// The fiber-facing suite wants a slice that never checks in to be a failure
	// rather than a log line (#198): `abort_` is what `lesh_tests` asks for.
	options.reactor_watchdog = fiber::watchdog_action::abort_;
	return options;
}

std::string buffer_of(const event_loop& loop) {
	return std::string{loop.editor().buffer.text()};
}

// ---------------------------------------------------------------------------
// The reactor under test
// ---------------------------------------------------------------------------

// A reactor whose length is a parameter and whose every unit of work polls.
//
// This is every real reactor's shape: the highlighter polls every `kPollEvery`
// tokens and at every `simple_command`, the autosuggester polls between history
// entries, and `classify_command` polls immediately before each `$PATH` stat. A
// unit here is one of those, so `units` is the walk's length in yields.
struct walker {
	std::size_t units = 1;

	std::size_t started = 0;      // computes entered
	std::size_t finished = 0;     // computes that ran to the end
	std::size_t gave_up = 0;      // computes that saw the poll and abandoned
	std::size_t polls = 0;        // polls across every compute
	std::size_t polls_when_it_gave_up = 0;
	std::vector<std::uint64_t> generations;   // one per compute entered
	std::vector<std::string> buffers;         // ditto
};

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

std::int32_t walk(lesh_request* request, void* userdata) {
	auto& self = *static_cast<walker*>(userdata);
	++self.started;
	std::uint64_t gen = 0;
	lesh_request_generation(request, &gen);
	self.generations.push_back(gen);
	self.buffers.push_back(buffer_of(request));

	for (std::size_t unit = 0; unit < self.units; ++unit) {
		++self.polls;
		std::int32_t superseded = 0;
		if (lesh_request_superseded(request, &superseded) != LESH_OK)
			return LESH_ERR_INVAL;
		if (superseded != 0) {
			++self.gave_up;
			self.polls_when_it_gave_up = self.polls;
			return LESH_ERR_SUPERSEDED;
		}
	}

	std::size_t length = 0;
	lesh_request_buffer_length(request, &length);
	if (length > 0)
		lesh_emit_span(request, 0, length, LESH_STYLE_NONE + 1);
	++self.finished;
	return LESH_OK;
}

// What the ABI answered from inside the fiber, so a test can assert that a token
// minted on a fiber's stack is a live one.
struct seen_snapshot {
	std::size_t calls = 0;
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
	++seen.calls;
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

// --- The shell side (A-5), faked, exactly as the loop suite fakes it --------

class fake_shell : public shell_side {
public:
	std::int32_t execute(std::string_view line) override {
		executed.assign(line);
		if (on_execute)
			on_execute();
		return execute_status;
	}

	std::int32_t port_call(std::string_view code) override {
		called.assign(code);
		return port_status;
	}

	std::string executed;
	std::string called;
	std::int32_t execute_status = 0;
	std::int32_t port_status = 0;
	std::function<void()> on_execute;
};

// A loop with a registry, ready to have reactors registered into it. Bundled
// because every test below wants the same four lines - and because of the
// declaration order, which is load-bearing rather than tidy.
struct driven {
	fake_tty tty;
	registry reg;
	// DECLARED BEFORE THE LOOP, SO IT IS DESTROYED AFTER IT, and this is the one
	// ownership rule the fibers add. `~event_loop` runs every emitter out to its
	// next cancellation poll (see `drain_emitters`), and that poll is a READ of the
	// reactor's userdata - so a reactor context that died first would be read after
	// its lifetime ended, which is a stack-use-after-scope ASan catches at once.
	// `ui/session.cpp` orders `_highlighter` and `_autosuggester` before `_loop`
	// for exactly this reason; a test that got it wrong found the rule.
	std::vector<std::unique_ptr<walker>> walkers;
	event_loop loop{tty.fds(), pipe_options()};

	driven() { loop.attach_registry(reg); }

	// A `walk` reactor of `units` units, owned here.
	walker& reactor(const char* name, std::uint32_t mask, std::size_t units = 1) {
		walkers.push_back(std::make_unique<walker>());
		walker& made = *walkers.back();
		made.units = units;
		EXPECT_EQ(lesh_reactor_register(&reg, name, mask, &walk, &made), LESH_OK);
		return made;
	}

	void start() { loop.enter_read(); }

	// Turns until `predicate` holds or the budget runs out. BOUNDED RATHER THAN A
	// WAIT: everything here runs on this thread, so the budget is a failure
	// deadline and never a synchronisation one.
	template <typename Predicate>
	bool turn_until(Predicate predicate, int budget = 200) {
		for (int i = 0; i < budget; ++i) {
			if (predicate())
				return true;
			loop.turn(0);
		}
		return predicate();
	}
};

} // namespace

// ===========================================================================
// One fiber per reactor
// ===========================================================================

TEST(UiReactorFiber, EachRegisteredReactorGetsOneLongLivedFiberInTheEmittersGroup) {
	driven d;
	walker& one = d.reactor("one", LESH_EVENT_BUFFER_CHANGED);
	walker& two = d.reactor("two", LESH_EVENT_BUFFER_CHANGED);
	(void)one;
	(void)two;
	d.start();

	// The dispatch table - and therefore the fibers - is built on the first
	// notification, which is the first keystroke that changes anything.
	EXPECT_EQ(d.loop.reactor_fibers(), 0u);
	d.tty.type("a");
	d.loop.turn(0);

	EXPECT_EQ(d.loop.reactor_fibers(), 2u);
	EXPECT_EQ(d.loop.reactors().fiber_count(), 2u);
	for (const char* name : {"one", "two"}) {
		EXPECT_GE(d.loop.reactor_slices(name), 1u) << name << " never ran";
		EXPECT_EQ(d.loop.reactor_computes(name), 1u) << name;
	}
	// Both are in `emitters`, which is the group `quiesce` parks with one store.
	EXPECT_FALSE(d.loop.reactors().group_parked(group_index(fiber_group::emitters)));
	EXPECT_EQ(d.loop.session_phase(), phase::editing);
}

TEST(UiReactorFiber, TheFiberIsTheSameOneAcrossEveryKeystrokeAndEveryTableRebuild) {
	// LONG-LIVED, which is the whole shape: `for(;;){ recv; compute; apply }`. A
	// fiber per event would be a stack per keystroke, and #145's inventory says
	// "one long-lived fiber each, none spawned per event".
	driven d;
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED);
	d.start();

	for (int i = 0; i < 8; ++i) {
		d.tty.type("x");
		d.loop.turn(0);
	}
	EXPECT_EQ(d.loop.reactor_fibers(), 1u);
	EXPECT_EQ(probe.started, 8u);

	// A second reactor registering bumps the registry's generation and rebuilds the
	// table; the first reactor's fiber is not respawned.
	walker& later = d.reactor("later", LESH_EVENT_BUFFER_CHANGED);
	d.tty.type("y");
	d.loop.turn(0);
	EXPECT_EQ(d.loop.reactor_fibers(), 2u);
	EXPECT_EQ(probe.started, 9u);
	EXPECT_EQ(later.started, 1u);
}

TEST(UiReactorFiber, TheTokenIsLiveOnTheFiberAndCarriesTheSnapshotAndNothingElse) {
	// The reporter's userdata outlives the loop for the reason `driven::walkers`
	// exists: `~event_loop` drains the emitters, which reads it.
	seen_snapshot seen;
	driven d;
	ASSERT_EQ(lesh_reactor_register(&d.reg, "reporter", LESH_EVENT_BUFFER_CHANGED,
	                                &report_the_snapshot, &seen),
	          LESH_OK);
	d.start();
	d.tty.type("echo hi");
	d.loop.turn(0);

	ASSERT_EQ(seen.calls, 1u) << "the reactor never ran on its fiber";
	// The accessors answering at all is the assertion: they refuse a token whose
	// owning thread is not the caller's, so this is the ABI's own liveness rule
	// agreeing that a fiber's stack is still this thread.
	EXPECT_TRUE(seen.token_was_live);
	EXPECT_EQ(seen.buffer_status, LESH_OK);
	EXPECT_EQ(seen.cursor_status, LESH_OK);
	EXPECT_EQ(seen.generation_status, LESH_OK);
	EXPECT_EQ(seen.buffer, "echo hi");
	EXPECT_EQ(seen.cursor, 7u);
	EXPECT_EQ(seen.generation, d.loop.editor().gen.value());
	EXPECT_EQ(seen.event_kind & LESH_EVENT_BUFFER_CHANGED, LESH_EVENT_BUFFER_CHANGED);
	EXPECT_EQ(seen.selection_active, 0) << "there is no selection model until #96";
}

// ===========================================================================
// Latest wins: the send IS the cancellation
// ===========================================================================

TEST(UiReactorFiber, TheLatestNotificationWinsAndTheOnesBetweenAreDropped) {
	// #90's rule, and #198's slot as the mechanism: "if something is waiting then
	// we drop it, as we need only the latest". Three keystrokes in one turn are
	// three sends into a capacity-one conflating slot, so the reactor computes ONCE
	// and it computes for the newest line.
	driven d;
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED);
	d.start();

	d.tty.type("abc");
	d.loop.turn(0);

	EXPECT_EQ(d.loop.reactor_sends("probe"), 3u);
	EXPECT_EQ(d.loop.reactor_superseded_sends("probe"), 2u)
		<< "two notifications should have been dropped where they waited";
	ASSERT_EQ(probe.started, 1u) << "three sends, one compute";
	EXPECT_EQ(probe.buffers.front(), "abc") << "and the compute is for the newest line";
	EXPECT_EQ(probe.generations.front(), d.loop.editor().gen.value());

	// AND IT LANDS ON THE NEXT TURN, which the record says in those words: a
	// reactor that polls has yielded, so the compute finishes in a LEADING slice
	// and the next turn is immediate because a runnable emitter is a zero timeout.
	// One further turn, not a wait.
	EXPECT_EQ(d.loop.applied_batches(), 0u);
	d.loop.turn(0);
	EXPECT_EQ(probe.finished, 1u);
	EXPECT_EQ(d.loop.applied_batches(), 1u);
	EXPECT_EQ(d.loop.reactor_sends("probe"), 3u) << "and nothing else was sent";
}

TEST(UiReactorFiber, AKeystrokeSupersedesAnInFlightWalkPartWayThroughIt) {
	// THE TEST THE OLD SUITE NEEDED TWO THREADS AND A GATE FOR. A walk of fifty
	// units yields at every one of them, so one turn puts the reactor mid-walk;
	// the next keystroke's send raises the flag its own poll is about to read.
	driven d;
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED, 50);
	d.start();

	d.tty.type("a");
	d.loop.turn(0);
	ASSERT_EQ(probe.started, 1u);
	ASSERT_EQ(probe.finished, 0u) << "a fifty-unit walk cannot have finished in one slice";
	ASSERT_TRUE(d.loop.reactors().runnable(group_mask(fiber_group::emitters)))
		<< "the walk yielded, so its fiber is still runnable";

	d.tty.type("b");
	d.loop.turn(0);

	EXPECT_EQ(probe.gave_up, 1u) << "the in-flight walk polled and abandoned";
	EXPECT_LT(probe.polls_when_it_gave_up, probe.units)
		<< "it should have given up part way through, not at the end";
	EXPECT_EQ(d.loop.reactor_abandoned("probe"), 1u)
		<< "and the batch it left behind was not applied";
	EXPECT_EQ(d.loop.applied_batches(), 0u);
	// And it went straight back for the newest line rather than stopping.
	ASSERT_EQ(probe.started, 2u);
	EXPECT_EQ(probe.buffers.back(), "ab");
}

TEST(UiReactorFiber, ACursorMoveDoesNotBumpTheGenerationAndCancelsNothing) {
	// Cancellation is the BUFFER-CHANGE generation bump and nothing else (#90's
	// rule, restated in #145's tick sketch). A cursor move reaches a reactor that
	// asked for `cursor_moved` and reaches this one not at all, so the walk in
	// flight is left alone.
	driven d;
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED, 20);
	d.start();

	d.tty.type("hello");
	d.loop.turn(0);
	ASSERT_EQ(probe.started, 1u);
	ASSERT_EQ(probe.finished, 0u);
	const std::uint64_t sends_before = d.loop.reactor_sends("probe");

	// <Left> is `backward_char`: the cursor moves, the buffer does not.
	d.tty.type("\x1b[D");
	ASSERT_TRUE(d.turn_until([&] { return probe.finished == 1u; }))
		<< "the walk was cancelled by a key that changed no bytes";

	EXPECT_EQ(d.loop.reactor_sends("probe"), sends_before) << "nothing was sent";
	EXPECT_EQ(probe.gave_up, 0u);
	EXPECT_EQ(probe.started, 1u) << "one walk, run to the end across many slices";
	EXPECT_EQ(d.loop.applied_batches(), 1u);
}

TEST(UiReactorFiber, DifferentReactorsAreDifferentSlotsAndDoNotDisplaceEachOther) {
	driven d;
	walker& typed = d.reactor("typed", LESH_EVENT_BUFFER_CHANGED);
	walker& moved = d.reactor("moved", LESH_EVENT_CURSOR_MOVED);
	d.start();

	// A typed character changes the buffer AND moves the cursor, so both hear.
	d.tty.type("ab");
	d.loop.turn(0);
	EXPECT_EQ(d.loop.reactor_sends("typed"), 2u);
	EXPECT_EQ(d.loop.reactor_sends("moved"), 2u);
	EXPECT_EQ(typed.started, 1u);
	EXPECT_EQ(moved.started, 1u);

	// A cursor move reaches one slot and not the other.
	d.tty.type("\x1b[D");
	ASSERT_TRUE(d.turn_until([&] { return moved.started == 2u; }));
	EXPECT_EQ(d.loop.reactor_sends("typed"), 2u) << "the buffer did not change";
	EXPECT_EQ(d.loop.reactor_sends("moved"), 3u);
	EXPECT_EQ(typed.started, 1u);
}

// ===========================================================================
// The tick: a slice before AND after the UI part
// ===========================================================================

TEST(UiReactorFiber, BothReactorsGetASliceBeforeAndAfterADispatchedKeyInOneTurn) {
	// The owner's ordering, counted. Two reactors, each mid-walk when the turn
	// begins, so the leading tick gives each one a slice; the key is then read and
	// dispatched, which sends into both slots; and the trailing tick gives each
	// another. Two slices per reactor in one turn, with a dispatch between them.
	driven d;
	walker& one = d.reactor("one", LESH_EVENT_BUFFER_CHANGED, 50);
	walker& two = d.reactor("two", LESH_EVENT_BUFFER_CHANGED, 50);
	d.start();

	// Turn one puts both mid-walk: the fibers are spawned, the key is dispatched,
	// and the trailing tick starts each walk, which yields at its first poll.
	d.tty.type("a");
	d.loop.turn(0);
	const std::size_t one_before = d.loop.reactor_slices("one");
	const std::size_t two_before = d.loop.reactor_slices("two");
	ASSERT_EQ(one_before, 1u);
	ASSERT_EQ(two_before, 1u);
	ASSERT_EQ(one.gave_up, 0u);

	d.tty.type("b");
	d.loop.turn(0);

	// EXACTLY TWO MORE EACH: one before the dispatch, one after. Not "at least" -
	// a turn that ticked once would read 1 and a turn that looped until quiet would
	// read 50, and both are different designs from the one the record describes.
	EXPECT_EQ(d.loop.reactor_slices("one") - one_before, 2u);
	EXPECT_EQ(d.loop.reactor_slices("two") - two_before, 2u);
	// The dispatch happened BETWEEN them, which is what the abandon proves: the
	// leading slice ran under the old line and the trailing one saw the new send.
	EXPECT_EQ(buffer_of(d.loop), "ab");
	EXPECT_EQ(one.gave_up, 1u);
	EXPECT_EQ(two.gave_up, 1u);
	EXPECT_EQ(one.started, 2u);
	EXPECT_EQ(two.started, 2u);
}

TEST(UiReactorFiber, AKeystrokeArrivingMidWalkIsDispatchedBeforeTheWalkFinishes) {
	// THE POINT OF THE YIELD, and the property #201 had to give up for one step:
	// "a reactor that walks `$PATH` holds the keystroke it was computed for until
	// it returns". It does not any more. The walk below is a thousand units long;
	// eight keystrokes are typed one turn at a time, and every one of them reaches
	// the editor while a walk is still in flight.
	driven d;
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED, 1000);
	d.start();

	std::string expected;
	for (char c : std::string_view{"abcdefgh"}) {
		const char key[2] = {c, '\0'};
		d.tty.type(key);
		d.loop.turn(0);
		expected.push_back(c);
		// The keystroke landed on THIS turn, with a walk unfinished throughout.
		EXPECT_EQ(buffer_of(d.loop), expected);
		EXPECT_EQ(probe.finished, 0u) << "a thousand units cannot have finished";
	}

	// Eight keystrokes, eight walks started and seven abandoned mid-way; the
	// eighth is still in flight, and nothing has been applied because nothing has
	// been allowed to finish.
	EXPECT_EQ(probe.started, 8u);
	EXPECT_EQ(probe.gave_up, 7u);
	EXPECT_EQ(d.loop.applied_batches(), 0u);
	EXPECT_EQ(d.loop.reactor_abandoned("probe"), 7u);
	// And the poll yielded every time it was asked, which is what gave the loop
	// the thread back between keystrokes.
	EXPECT_EQ(d.loop.reactor_yields("probe"), probe.polls);

	// Left alone, the last walk finishes - across many turns, one slice each.
	ASSERT_TRUE(d.turn_until([&] { return probe.finished == 1u; }, 4000));
	EXPECT_EQ(d.loop.applied_batches(), 1u);
	EXPECT_EQ(buffer_of(d.loop), "abcdefgh");
}

TEST(UiReactorFiber, ARunnableReactorMakesThePollTimeoutZero) {
	// "If no new keys arrived, tick them" is `poll(0)` between slices. A loop with
	// a walk in flight must not sit in `poll` waiting for a deadline it does not
	// have - it owes work.
	driven d;
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED, 50);
	d.start();

	EXPECT_EQ(d.loop.poll_timeout_ms(), -1) << "nothing to do: block";
	d.tty.type("a");
	d.loop.turn(0);
	ASSERT_TRUE(d.loop.reactors().runnable(group_mask(fiber_group::emitters)));
	EXPECT_EQ(d.loop.poll_timeout_ms(), 0);

	ASSERT_TRUE(d.turn_until([&] { return probe.finished == 1u; }));
	EXPECT_EQ(d.loop.poll_timeout_ms(), -1) << "the walk is done: block again";
}

// ===========================================================================
// The phase, and dying at accept
// ===========================================================================

TEST(UiReactorFiber, AtAcceptTheGroupIsParkedAndTheDeadLinesEmissionIsNeverApplied) {
	// F-22, and the owner's "cancel - park, something like that". The emitters do
	// not die at accept; they are superseded and their group's bit goes down, so
	// nothing computes for a line that has already run and nothing it had computed
	// is applied.
	fake_shell shell;
	driven d;
	d.loop.attach_shell(shell);
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED, 500);
	d.start();

	d.tty.type("echo hi");
	d.loop.turn(0);
	ASSERT_EQ(probe.started, 1u);
	ASSERT_EQ(probe.finished, 0u) << "the walk must still be in flight at accept";
	ASSERT_EQ(d.loop.applied_batches(), 0u);

	bool parked_during_execute = false;
	phase phase_during_execute = phase::editing;
	shell.on_execute = [&] {
		parked_during_execute =
			d.loop.reactors().group_parked(group_index(fiber_group::emitters));
		phase_during_execute = d.loop.session_phase();
	};

	const std::optional<std::int32_t> status = d.loop.accept_current_line();
	ASSERT_TRUE(status.has_value());
	EXPECT_EQ(shell.executed, "echo hi");

	EXPECT_TRUE(parked_during_execute) << "the emitters group was runnable during a fork";
	EXPECT_EQ(phase_during_execute, phase::executing);
	// And back, with the group runnable again.
	EXPECT_EQ(d.loop.session_phase(), phase::editing);
	EXPECT_FALSE(d.loop.reactors().group_parked(group_index(fiber_group::emitters)));

	// The dead line's walk abandons at its next poll and applies nothing.
	ASSERT_TRUE(d.turn_until([&] { return probe.gave_up == 1u; }));
	EXPECT_EQ(d.loop.applied_batches(), 0u)
		<< "an emission computed for the line that just ran was applied";
	EXPECT_EQ(d.loop.reactor_abandoned("probe"), 1u);
}

TEST(UiReactorFiber, AfterResumeTheNextLinesFirstSendIsReceived) {
	// The other half of "cancel, park": the fibers are ALIVE. Nothing was killed
	// at accept, so the next line's first notification finds a receiver.
	fake_shell shell;
	driven d;
	d.loop.attach_shell(shell);
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED);
	d.start();

	d.tty.type("first");
	d.loop.turn(0);
	ASSERT_EQ(probe.started, 1u);
	ASSERT_EQ(d.loop.reactor_fibers(), 1u);

	ASSERT_TRUE(d.loop.accept_current_line().has_value());
	ASSERT_EQ(buffer_of(d.loop), "");

	d.tty.type("second");
	ASSERT_TRUE(d.turn_until([&] { return probe.finished >= 1u; }))
		<< "the next line's first send never reached the fiber";
	EXPECT_EQ(d.loop.reactor_fibers(), 1u) << "and it is the same fiber";
	EXPECT_EQ(probe.buffers.back(), "second");
	EXPECT_GE(d.loop.applied_batches(), 1u);
}

TEST(UiReactorFiber, QuiesceNestsAndAssertsAllThreeHalves) {
	driven d;
	d.start();

	EXPECT_EQ(d.loop.session_phase(), phase::editing);
	d.loop.quiesce();
	EXPECT_TRUE(d.loop.quiesced());
	EXPECT_EQ(d.loop.session_phase(), phase::executing);
	d.loop.assert_quiesced();
	d.loop.quiesce();
	d.loop.assert_quiesced();

	d.loop.resume_after_execution();
	EXPECT_TRUE(d.loop.quiesced()) << "two parks need two resumes";
	EXPECT_EQ(d.loop.session_phase(), phase::executing);
	d.loop.resume_after_execution();
	EXPECT_FALSE(d.loop.quiesced());
	EXPECT_EQ(d.loop.session_phase(), phase::editing);
	EXPECT_FALSE(d.loop.reactors().group_parked(group_index(fiber_group::emitters)));
}

TEST(UiReactorFiber, AQuiescedLoopIsSafeToForkThrough) {
	// #91's requirement, and since #202 it is a much smaller claim than it was:
	// there are no helper threads left, so a lesh process is single-threaded and a
	// child born here inherits no lock at all. What is still worth executing is
	// that the child can READ everything the parent had - a fiber's stack is an
	// ordinary anonymous mapping, and the scheduler's bookkeeping is ordinary
	// memory - so a `( )` subshell reading its inherited state is not a hazard.
	driven d;
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED, 500);
	d.start();
	d.tty.type("a line");
	d.loop.turn(0);
	ASSERT_EQ(probe.started, 1u);

	d.loop.quiesce();
	d.loop.assert_quiesced();

	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0) {
		const bool as_expected =
			d.loop.quiesced()
			&& d.loop.reactors().group_parked(group_index(fiber_group::emitters))
			&& d.loop.reactor_fibers() == 1u
			&& d.loop.reactor_computes("probe") == 1u
			&& !d.loop.reactors().runnable(group_mask(fiber_group::emitters));
		::_exit(as_expected ? 0 : 1);
	}

	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	d.loop.resume_after_execution();
}

// ===========================================================================
// Ownership (ADR-0007)
// ===========================================================================

TEST(UiReactorFiber, ALoopDestroyedWithAWalkInFlightOwnsNothingAfterwards) {
	// THE LEAK GATE IS THE ASSERTION, and this test exists because the hazard is
	// real: `~scheduler` unmaps a parked fiber's stack WITHOUT unwinding it, and a
	// compute's stack owns the snapshot's buffer - `run_reactor_here` moved it into
	// the token and has not yet moved it back. `~event_loop` therefore supersedes
	// every emitter and runs it out to its next poll first, and ADR-0007's expected
	// leak count is exactly zero with no suppression.
	//
	// THE LINE IS LONGER THAN A SHORT STRING so the buffer at risk is a heap
	// block: libc++ keeps 22 bytes inside the object, and a leak of nothing would
	// be no evidence at all.
	walker probe;
	probe.units = 500;
	{
		driven d;
		ASSERT_EQ(lesh_reactor_register(&d.reg, "probe", LESH_EVENT_BUFFER_CHANGED, &walk,
		                                &probe),
		          LESH_OK);
		d.start();
		d.tty.type("git log --oneline --graph --decorate --all | head -40");
		d.loop.turn(0);
		ASSERT_EQ(probe.started, 1u);
		ASSERT_EQ(probe.finished, 0u) << "the walk must be in flight at destruction";
		ASSERT_GT(probe.buffers.front().size(), 22u);
	}
	// The drain ran it out rather than leaving it suspended: the walk gave up.
	EXPECT_EQ(probe.gave_up, 1u);
	EXPECT_EQ(probe.finished, 0u);
}

TEST(UiReactorFiber, ALoopDestroyedWhileParkedStillDrainsItsEmitters) {
	// A loop torn down mid-execution - which is what an `exit` inside a command
	// is - has its emitters group parked. The drain resumes it first, because a
	// parked group's fibers are not runnable and a tick would skip exactly the
	// fibers that need running out.
	walker probe;
	probe.units = 500;
	{
		driven d;
		ASSERT_EQ(lesh_reactor_register(&d.reg, "probe", LESH_EVENT_BUFFER_CHANGED, &walk,
		                                &probe),
		          LESH_OK);
		d.start();
		d.tty.type("a line long enough to be a heap allocation of its own");
		d.loop.turn(0);
		ASSERT_EQ(probe.started, 1u);
		d.loop.quiesce();
		ASSERT_TRUE(d.loop.reactors().group_parked(group_index(fiber_group::emitters)));
		// No resume. The destructor has to get them out anyway.
	}
	EXPECT_EQ(probe.gave_up, 1u);
}

TEST(UiReactorFiber, ALoopWithNoReactorsSpawnsNoFibersAndMapsNoStacks) {
	// The smallest loop is still a working editor, and it costs no fiber and no
	// half-megabyte mapping - which is what keeps `event_loop` cheap enough for the
	// hundred of them the unit suite builds.
	driven d;
	d.start();
	d.tty.type("hello");
	d.loop.turn(0);
	EXPECT_EQ(buffer_of(d.loop), "hello");
	EXPECT_EQ(d.loop.reactor_fibers(), 0u);
	EXPECT_EQ(d.loop.reactors().fiber_count(), 0u);
	EXPECT_FALSE(d.loop.reactors().runnable());
}

// ===========================================================================
// The drop rule is still the applier's
// ===========================================================================

TEST(UiReactorFiber, ABatchComputedAgainstAnOlderGenerationIsStillDropped) {
	// N-4, unchanged and in the same place: `apply_batch` decides, and the fiber
	// path goes through the same `take_batch` every other path does. The editor is
	// moved on underneath a walk WITHOUT a notification, which is a thing no real
	// keystroke does - and is exactly why the generation rule exists behind the
	// supersede rather than instead of it.
	driven d;
	walker& probe = d.reactor("probe", LESH_EVENT_BUFFER_CHANGED, 8);
	d.start();

	d.tty.type("a");
	d.loop.turn(0);
	ASSERT_EQ(probe.started, 1u);
	ASSERT_EQ(probe.finished, 0u);

	d.loop.editor().gen.bump();
	ASSERT_TRUE(d.turn_until([&] { return probe.finished == 1u; }));

	EXPECT_EQ(d.loop.reactor_abandoned("probe"), 0u) << "nothing superseded it";
	EXPECT_EQ(d.loop.dropped_batches(), 1u) << "the generation rule dropped it";
	EXPECT_EQ(d.loop.applied_batches(), 0u);
	EXPECT_TRUE(d.loop.editor().marks.layers().empty());
}
