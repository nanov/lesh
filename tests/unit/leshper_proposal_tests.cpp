#include "leshper/keymap.h"
#include "leshper/loop.h"
#include "leshper/proposal.h"
#include "leshper/registry.h"
#include "leshper/workers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace lesh::leshper;

// WHAT A REACTOR OFFERED, REACHED BY AN ACTION THE LOOP RAN (#133, #144, F-25).
//
// The trail this file walks end to end is the one #144 was opened for, and the
// break was in the middle of it: a reactor proposes, a worker answers, the loop
// applies, an action bound to a key reads index 0 and puts it in the buffer.
// #141 landed the painting half of that and returned the rest - the accessor
// walked a vector `loop_harness` owned and the running loop filled `state::marks`
// instead, so the suggestion was on screen and no action could accept it.
//
// So the loop tests here drive a REAL `event_loop` over a pipe, with a real
// worker pool, and dispatch through the keymap - never through the harness
// directly, because dispatching through the harness is exactly what hid the
// defect. The store's own rules are unit-tested first, above them.
//
// NOTHING IS BOUND BY DEFAULT. #140 decides which key accepts a suggestion; a
// test that needed one binds it in its own keymap, which is what an rc file's
// `bind` does (#118, #134).

namespace {

// --- The store --------------------------------------------------------------

std::vector<proposal> offering(std::uint32_t kind, std::initializer_list<const char*> bytes) {
	std::vector<proposal> made;
	for (const char* one : bytes)
		made.push_back(proposal{kind, std::string(one)});
	return made;
}

// --- The loop ---------------------------------------------------------------

// A pipe standing in for a terminal, as everywhere else in leshper's tests:
// never the process's own tty.
class fake_tty {
public:
	fake_tty() {
		[&] { ASSERT_EQ(::pipe(_in), 0); }();
		[&] { ASSERT_EQ(::pipe(_out), 0); }();
		::fcntl(_in[0], F_SETFL, O_NONBLOCK);
		::fcntl(_out[0], F_SETFL, O_NONBLOCK);
	}
	~fake_tty() {
		for (int fd : {_in[0], _in[1], _out[0], _out[1]})
			if (fd >= 0)
				::close(fd);
	}

	fake_tty(const fake_tty&) = delete;
	fake_tty& operator=(const fake_tty&) = delete;

	[[nodiscard]] loop_fds fds() const { return loop_fds{_in[0], _out[1]}; }

	void type(std::string_view bytes) const {
		ASSERT_EQ(::write(_in[1], bytes.data(), bytes.size()),
		          static_cast<ssize_t>(bytes.size()));
	}

	// Everything the loop has written since the last call.
	[[nodiscard]] std::string painted() const {
		std::string all;
		char chunk[4096];
		for (;;) {
			const ssize_t n = ::read(_out[0], chunk, sizeof(chunk));
			if (n <= 0)
				break;
			all.append(chunk, static_cast<std::size_t>(n));
		}
		return all;
	}

private:
	int _in[2]{-1, -1};
	int _out[2]{-1, -1};
};

loop_options pipe_options() {
	loop_options options;
	options.manage_terminal = false;
	options.prompt = "> ";
	return options;
}

template <typename Predicate>
bool turn_until(event_loop& loop, Predicate predicate, int budget = 200) {
	for (int i = 0; i < budget; ++i) {
		if (predicate())
			return true;
		loop.turn(5);
	}
	return predicate();
}

// What the autosuggester does, without a history: the continuation as virtual
// text at the end of the typed text, and the WHOLE candidate as a proposal.
struct offer {
	std::string candidate = "git status";
	std::uint32_t kind = LESH_PROPOSAL_AUTOSUGGESTION;
};

int32_t offering_reactor(lesh_request* request, void* userdata) {
	const offer& what = *static_cast<const offer*>(userdata);
	char typed[256] = {};
	std::size_t length = 0;
	if (lesh_request_buffer(request, typed, sizeof(typed), &length) != LESH_OK)
		return LESH_OK;
	const std::string_view line{typed, length};
	if (line.empty() || line.size() >= what.candidate.size()
	    || std::string_view{what.candidate}.substr(0, line.size()) != line)
		return LESH_OK;
	const std::string_view rest = std::string_view{what.candidate}.substr(line.size());
	lesh_emit_virtual_text(request, length, rest.data(), rest.size());
	return lesh_propose(request, what.kind, what.candidate.data(), what.candidate.size());
}

// What an action sees, read back through the accessor an accepting action uses.
struct probe {
	std::uint32_t kind = LESH_PROPOSAL_AUTOSUGGESTION;
	std::size_t index = 0;
	std::int32_t status = LESH_ERR_NOTFOUND;
	std::string bytes;
	int runs = 0;
};

int32_t probing_action(lesh_editor* editor, const lesh_invocation*, void* userdata) {
	probe& p = *static_cast<probe*>(userdata);
	char out[256] = {};
	std::size_t length = 0;
	p.status = lesh_proposal_read(editor, p.kind, p.index, out, sizeof(out), &length);
	p.bytes.assign(out, p.status == LESH_OK ? length : 0u);
	++p.runs;
	return LESH_OK;
}

// A loop, its helpers, and the editing context they all share.
//
// THE CONTEXT IS THE LOOP'S EDITOR'S, which is the wiring #134 does in
// `ui/session.cpp` and the only wiring that works: `editor.cpp` dispatches a key
// through `context_of(state)`, so the registry the loop attaches has to be that
// context's or a bound key reaches a different table than the reactors do.
struct looped {
	fake_tty tty;
	worker_pool helpers{1};
	event_loop loop{tty.fds(), pipe_options()};
	offer what;
	probe seen;

	looped() {
		editing_context& context = context_of(loop.editor());
		EXPECT_EQ(lesh_reactor_register(&context.actions(), "offerer",
		                                LESH_EVENT_BUFFER_CHANGED, &offering_reactor, &what),
		          LESH_OK);
		EXPECT_EQ(lesh_action_register(&context.actions(), "probe_proposal", &probing_action,
		                               &seen),
		          LESH_OK);
		loop.attach_registry(context.actions());
		loop.attach_helpers(helpers);
		// A size, so a turn paints: the pipe has no winsize to report.
		loop.editor().columns = 40;
		loop.editor().rows = 6;
		loop.enter_read();
	}

	void bind(const char* notation, std::string_view action) {
		editing_context& context = context_of(loop.editor());
		keymap* map = context.keymaps().find(keymap_registry::emacs);
		std::string encoded;
		ASSERT_NE(map, nullptr);
		ASSERT_TRUE(parse_key_notation(notation, encoded)) << notation;
		map->bind(encoded, action);
	}

	// Type, and wait for the offer to come back from the helper and be applied.
	[[nodiscard]] bool show(std::string_view typed) {
		const std::size_t before = loop.applied_batches();
		tty.type(typed);
		loop.turn(50);
		return turn_until(loop, [&] { return loop.applied_batches() > before; });
	}

	// Type a key that is bound, and let the turn run it.
	void press(std::string_view keys) {
		tty.type(keys);
		loop.turn(50);
	}

	[[nodiscard]] std::string buffer() const { return std::string{loop.editor().buffer.text()}; }
};

} // namespace

// ===========================================================================
// The store: what `lesh_proposal_read` walks
// ===========================================================================

TEST(LeshperAppliedProposals, ANewerBatchReplacesTheSameReactorsAndKeepsItsPlace) {
	// The emitting reactor is the namespace (ADR-0008), on this half exactly as
	// on the decorations. Two reactors offer; the first offers again; the list is
	// still two long and still in the order it was first applied, because an
	// index the pager handed out must not shuffle when a source answers twice.
	applied_proposals applied;
	std::vector<proposal> first = offering(LESH_PROPOSAL_HISTORY_MATCH, {"git log"});
	std::vector<proposal> second = offering(LESH_PROPOSAL_HISTORY_MATCH, {"grep -r"});
	applied.apply("searcher", first);
	applied.apply("other", second);

	std::vector<proposal> again = offering(LESH_PROPOSAL_HISTORY_MATCH, {"git status"});
	applied.apply("searcher", again);

	ASSERT_EQ(applied.layers().size(), 2u);
	EXPECT_EQ(applied.layers()[0].reactor, "searcher");
	ASSERT_EQ(applied.layers()[0].items.size(), 1u);
	EXPECT_EQ(applied.layers()[0].items[0].bytes, "git status");
	EXPECT_EQ(applied.layers()[1].reactor, "other");
}

TEST(LeshperAppliedProposals, ApplyingSwapsSoThePooledBatchKeepsItsStorage) {
	// #126's pooling, which is why this is a swap and not a move: what goes back
	// to the caller is the layer's old vector, capacity and all, and what the
	// pool clears is a vector that has one.
	applied_proposals applied;
	std::vector<proposal> first = offering(LESH_PROPOSAL_AUTOSUGGESTION, {"git log"});
	applied.apply("offerer", first);
	EXPECT_TRUE(first.empty()) << "the layer took what was handed in";

	std::vector<proposal> second = offering(LESH_PROPOSAL_AUTOSUGGESTION, {"git status"});
	applied.apply("offerer", second);
	ASSERT_EQ(second.size(), 1u);
	EXPECT_EQ(second[0].bytes, "git log") << "the previous layer's storage came back out";
}

TEST(LeshperAppliedProposals, AnEmptyBatchIsStillAnApplicationAndTakesTheOfferAway) {
	// A reactor that decides it has nothing to offer must be able to say so. If
	// an empty batch did not replace, a suggestion would outlive the buffer it
	// was about - the same wrong that the drop rule exists to prevent.
	applied_proposals applied;
	std::vector<proposal> first = offering(LESH_PROPOSAL_AUTOSUGGESTION, {"git status"});
	applied.apply("offerer", first);
	EXPECT_FALSE(applied.empty());

	std::vector<proposal> nothing;
	applied.apply("offerer", nothing);
	EXPECT_TRUE(applied.empty());
	EXPECT_EQ(applied.layers().size(), 1u) << "the layer stays; it is simply empty";
	EXPECT_EQ(applied.find(LESH_PROPOSAL_AUTOSUGGESTION, 0), nullptr);
}

TEST(LeshperAppliedProposals, TheIndexWalksOneKindInEmissionOrderAcrossTheLayers) {
	applied_proposals applied;
	std::vector<proposal> completions =
		offering(LESH_PROPOSAL_COMPLETION, {"lesh.cpp", "lesh.h"});
	std::vector<proposal> matches = offering(LESH_PROPOSAL_HISTORY_MATCH, {"git log"});
	std::vector<proposal> more = offering(LESH_PROPOSAL_COMPLETION, {"leshper.h"});
	applied.apply("completer", completions);
	applied.apply("searcher", matches);
	applied.apply("other_completer", more);

	ASSERT_NE(applied.find(LESH_PROPOSAL_COMPLETION, 0), nullptr);
	EXPECT_EQ(applied.find(LESH_PROPOSAL_COMPLETION, 0)->bytes, "lesh.cpp");
	EXPECT_EQ(applied.find(LESH_PROPOSAL_COMPLETION, 1)->bytes, "lesh.h");
	// Past the layer that carries another kind entirely, which is skipped rather
	// than counted.
	EXPECT_EQ(applied.find(LESH_PROPOSAL_COMPLETION, 2)->bytes, "leshper.h");
	EXPECT_EQ(applied.find(LESH_PROPOSAL_COMPLETION, 3), nullptr);
	EXPECT_EQ(applied.find(LESH_PROPOSAL_HISTORY_MATCH, 0)->bytes, "git log");
}

TEST(LeshperAppliedProposals, DismissingDropsTheWholeBatchAndItsPaintedLayer) {
	// #133's rule, both halves in one call: the drawn half of a suggestion is its
	// virtual text, and a dismissal that left that on screen would have dismissed
	// nothing the user can see.
	applied_proposals applied;
	decorations marks;
	std::vector<proposal> offers = offering(LESH_PROPOSAL_AUTOSUGGESTION, {"git status"});
	std::vector<decoration_span> spans;
	std::vector<virtual_text> texts{virtual_text{3, " status", 0}};
	applied.apply("offerer", offers);
	marks.apply("offerer", spans, texts);

	std::vector<proposal> matches = offering(LESH_PROPOSAL_HISTORY_MATCH, {"git log"});
	std::vector<decoration_span> other_spans{decoration_span{0, 3, 1}};
	std::vector<virtual_text> no_texts;
	applied.apply("searcher", matches);
	marks.apply("searcher", other_spans, no_texts);

	EXPECT_TRUE(applied.dismiss(LESH_PROPOSAL_AUTOSUGGESTION, marks));

	ASSERT_EQ(applied.layers().size(), 1u);
	EXPECT_EQ(applied.layers()[0].reactor, "searcher") << "another kind is untouched";
	EXPECT_TRUE(marks.texts().empty()) << "the virtual text went with the batch";
	EXPECT_EQ(marks.spans().size(), 1u) << "and nobody else's layer did";

	// Dismissing a kind nothing is offering is not an error and drops nothing,
	// which is what tells the loop it has no repaint to do.
	EXPECT_FALSE(applied.dismiss(LESH_PROPOSAL_AUTOSUGGESTION, marks));
}

// ===========================================================================
// The real loop: applied by `take_batch`, read by an action it dispatched
// ===========================================================================

TEST(LeshperLoopProposals, AKeyBoundToAcceptPutsTheAppliedProposalInTheBuffer) {
	// THE WHOLE TRAIL, and the one #144 found broken: reactor -> worker ->
	// `take_batch` -> `state::proposals` -> `lesh_proposal_read` -> a staged
	// write the loop commits. Nothing here dispatches through the harness by
	// hand; the key is bound and typed, exactly as a user's `bind` would be.
	looped driven;
	driven.bind("<C-y>", "accept_autosuggestion");

	ASSERT_TRUE(driven.show("git"));
	ASSERT_FALSE(driven.loop.editor().proposals.empty()) << "the loop applied the offer";
	EXPECT_EQ(driven.loop.editor().marks.texts().size(), 1u) << "and painted its other half";

	const std::uint64_t before = driven.loop.editor().gen.value();
	driven.press("\x19");   // Ctrl-Y

	EXPECT_EQ(driven.buffer(), "git status");
	// A-12: the proposal reached the buffer through staged writes and by no
	// other route, so the accept is ONE edit and ONE generation bump.
	EXPECT_EQ(driven.loop.editor().gen.value(), before + 1);
	EXPECT_TRUE(driven.loop.editor().undo.can_undo());
}

TEST(LeshperLoopProposals, AnActionReadsIndexZeroOfWhatTheLoopApplied) {
	looped driven;
	driven.bind("<C-y>", "probe_proposal");

	ASSERT_TRUE(driven.show("git"));
	driven.press("\x19");

	EXPECT_EQ(driven.seen.runs, 1);
	EXPECT_EQ(driven.seen.status, LESH_OK);
	EXPECT_EQ(driven.seen.bytes, "git status");
}

TEST(LeshperLoopProposals, ATimerDispatchSeesTheSameViewAKeystrokeDoes) {
	// The loop dispatches a timer expiry through its OWN harness and a keystroke
	// through the context's, and before #144 those were two objects with two
	// stores. The view is the state's now, so there is one - and this is the path
	// that would have gone quietly wrong again if it were not.
	looped driven;
	std::uint64_t id = 0;
	ASSERT_EQ(lesh_timer_start(&context_of(driven.loop.editor()).actions(), 5,
	                           "probe_proposal", &id),
	          LESH_OK);

	ASSERT_TRUE(driven.show("git"));
	ASSERT_TRUE(turn_until(driven.loop, [&] { return driven.seen.runs > 0; }));

	EXPECT_EQ(driven.seen.status, LESH_OK);
	EXPECT_EQ(driven.seen.bytes, "git status");
}

TEST(LeshperLoopProposals, ANewerBatchFromOneReactorReplacesWhatItOfferedBefore) {
	// Latest-wins, through the loop rather than through the store: the reactor
	// answers twice and the second answer is the one an action reads. Not two
	// offers stacked up, which would make index 1 a suggestion about a buffer
	// two keystrokes ago.
	looped driven;
	driven.bind("<C-y>", "probe_proposal");

	ASSERT_TRUE(driven.show("g"));
	driven.what.candidate = "git log --oneline";
	ASSERT_TRUE(driven.show("i"));

	ASSERT_EQ(driven.loop.editor().proposals.layers().size(), 1u);
	driven.press("\x19");
	EXPECT_EQ(driven.seen.bytes, "git log --oneline");

	driven.seen.index = 1;
	driven.press("\x19");
	EXPECT_EQ(driven.seen.status, LESH_ERR_NOTFOUND) << "there is one offer, not two";
}

TEST(LeshperLoopProposals, ABatchFromASupersededGenerationIsNeverReadable) {
	// N-4 from the accepting side. The batch was computed against a generation
	// the editor has left behind, so it is dropped rather than applied - and an
	// action cannot read a proposal about text the buffer no longer holds.
	looped driven;
	driven.bind("<C-y>", "probe_proposal");

	driven.helpers.submit("offerer",
	                      snapshot_of(driven.loop.editor(), LESH_EVENT_BUFFER_CHANGED),
	                      &offering_reactor, &driven.what);
	driven.loop.editor().gen.bump();
	ASSERT_TRUE(turn_until(driven.loop, [&] { return driven.loop.dropped_batches() > 0; }));

	EXPECT_TRUE(driven.loop.editor().proposals.empty());
	driven.press("\x19");
	EXPECT_EQ(driven.seen.runs, 1);
	EXPECT_EQ(driven.seen.status, LESH_ERR_NOTFOUND);
}

TEST(LeshperLoopProposals, DismissingTakesTheBatchOffTheScreenAndOutOfTheView) {
	// #133's dismissal in the running loop: the whole batch goes, virtual text
	// included, and the loop repaints - a dismissal changes neither the buffer
	// nor the cursor, so the redraw has to be asked for by the dismissal itself.
	looped driven;
	driven.bind("<C-g>", "dismiss_autosuggestion");
	driven.bind("<C-y>", "probe_proposal");

	ASSERT_TRUE(driven.show("git"));
	ASSERT_EQ(driven.loop.editor().marks.texts().size(), 1u);
	(void)driven.tty.painted();

	driven.press("\x07");   // Ctrl-G

	EXPECT_TRUE(driven.loop.editor().proposals.empty());
	EXPECT_TRUE(driven.loop.editor().marks.texts().empty())
		<< "the painted half went with the batch that carried it";
	EXPECT_EQ(driven.buffer(), "git");
	EXPECT_EQ(driven.tty.painted().find("status"), std::string::npos)
		<< "and the repaint the dismissal asked for no longer draws it";

	driven.press("\x19");
	EXPECT_EQ(driven.seen.status, LESH_ERR_NOTFOUND) << "nothing left to accept";
}

TEST(LeshperLoopProposals, TheLineBoundaryTakesTheOffersWithTheDecorations) {
	// A proposal is what THIS line would become, and this line has just run.
	looped driven;
	ASSERT_TRUE(driven.show("git"));
	ASSERT_FALSE(driven.loop.editor().proposals.empty());

	driven.loop.accept_current_line();

	EXPECT_TRUE(driven.loop.editor().proposals.empty());
	EXPECT_TRUE(driven.loop.editor().marks.empty());
	EXPECT_EQ(driven.buffer(), "");
}
