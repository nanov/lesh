#include "leshper/abi.h"
#include "leshper/history_search.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "substrate/arena.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lesh::leshper;

// The autosuggester reactor and F-25's accepting actions (#133).
//
// Driven the way the loop will drive them, and by no other route: a state, the
// harness fake, a batch read back, an action dispatched by NAME. Nothing here
// calls into builtin_reactors.cpp or builtin_actions.cpp directly, because there
// is no other entry point - both are a function pointer in a registry with a
// `void*` beside it, exactly as a Lua reactor would be (A-11).
//
// The history is a VECTOR, never `~/.lesh_history` (#125): a unit test that read
// the developer's own history would pass differently on every machine.

namespace {

// One registry, one autosuggester, one loop - and the history outlives the
// autosuggester, which outlives the registry it registered into. That is the
// lifetime the ABI requires of any reactor's context pointer, and the
// declaration order here is what enforces it.
struct suggest_fixture {
	vector_history_source history;
	registry reg;
	owned_autosuggester self{&history};
	loop_harness loop{reg};

	explicit suggest_fixture(std::vector<std::string> entries = {})
		: history(std::move(entries)) {
		register_autosuggester(reg, self.get());
		register_builtin_actions(reg);
	}

	// Entries are in APPEND order, so the last one is the newest.
	[[nodiscard]] static lesh::leshper::state line(std::string_view text) {
		lesh::leshper::state s;
		if (!text.empty())
			s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), text);
		s.cursor = s.buffer.end_position();
		s.gen.bump();
		return s;
	}

	[[nodiscard]] reactor_batch suggest(const lesh::leshper::state& s,
	                                    std::uint32_t kinds = LESH_EVENT_BUFFER_CHANGED) {
		std::vector<reactor_batch> batches = loop.react(s, kinds);
		EXPECT_EQ(batches.size(), 1u);
		if (batches.empty())
			return reactor_batch{};
		return std::move(batches[0]);
	}

	// React and apply, which is the state the accepting actions run against.
	// `apply_batch` is the loop's own applier (#144), so what these tests read
	// back through the ABI is what the running shell would have applied.
	void show(lesh::leshper::state& s) {
		for (reactor_batch& one : loop.react(s, LESH_EVENT_BUFFER_CHANGED))
			apply_batch(s, one);
	}

	[[nodiscard]] std::string style_name(std::uint32_t id) {
		char out[64] = {};
		std::size_t length = 0;
		if (lesh_style_name(&reg, id, out, sizeof(out), &length) != LESH_OK)
			return "<none>";
		return std::string(out, length);
	}
};

// A history that supersedes the request part-way through its own walk, so the
// cooperative poll has something to notice. The real trigger is the user typing
// while a worker is thinking; here it is the second entry.
class superseding_source final : public history_source {
public:
	superseding_source(loop_harness& loop, std::vector<std::string> entries)
		: _loop(&loop), _entries(std::move(entries)) {}

	void for_each_newest_first(
		const std::function<bool(std::string_view)>& fn) const override {
		std::size_t seen = 0;
		for (auto it = _entries.rbegin(); it != _entries.rend(); ++it) {
			if (++seen == 2)
				_loop->supersede();
			if (!fn(*it))
				return;
		}
	}

private:
	loop_harness* _loop;
	std::vector<std::string> _entries;
};

// What an accepting action sees, read back through the ABI accessor it uses.
struct probe {
	std::uint32_t kind = LESH_PROPOSAL_AUTOSUGGESTION;
	std::size_t index = 0;
	std::int32_t status = LESH_OK;
	std::size_t length = 0;
	std::string bytes;
};

int32_t probe_action(lesh_editor* editor, const lesh_invocation*, void* userdata) {
	probe* p = static_cast<probe*>(userdata);
	char out[256] = {};
	p->status = lesh_proposal_read(editor, p->kind, p->index, out, sizeof(out), &p->length);
	p->bytes.assign(out, p->status == LESH_OK ? p->length : 0u);
	return LESH_OK;
}

} // namespace

// ---------------------------------------------------------------------------
// The reactor
// ---------------------------------------------------------------------------

TEST(LeshperAutosuggest, TheNewestPrefixMatchBecomesVirtualTextAndAProposal) {
	suggest_fixture fixture{{"ls -l", "git status"}};
	const lesh::leshper::state s = suggest_fixture::line("git");
	const reactor_batch batch = fixture.suggest(s);

	EXPECT_EQ(batch.status, LESH_OK);
	EXPECT_EQ(batch.reactor, "autosuggester");

	// The CONTINUATION, at the end of the buffer - what a renderer draws after
	// the cursor, and nothing of what the user already typed.
	ASSERT_EQ(batch.texts.size(), 1u);
	EXPECT_EQ(batch.texts[0].bytes, " status");
	EXPECT_EQ(batch.texts[0].at, 3u);
	EXPECT_EQ(fixture.style_name(batch.texts[0].style_id), "suggestion");

	// The WHOLE candidate, for the accepting action.
	ASSERT_EQ(batch.proposals.size(), 1u);
	EXPECT_EQ(batch.proposals[0].kind, LESH_PROPOSAL_AUTOSUGGESTION);
	EXPECT_EQ(batch.proposals[0].bytes, "git status");

	// It paints nothing: highlighting is the highlighter's, and the emitting
	// reactor is the decoration namespace.
	EXPECT_TRUE(batch.spans.empty());
}

TEST(LeshperAutosuggest, TheNEWESTMatchWins) {
	suggest_fixture fixture{{"git status", "git commit", "ls"}};
	const reactor_batch batch = fixture.suggest(suggest_fixture::line("git "));
	ASSERT_EQ(batch.proposals.size(), 1u);
	EXPECT_EQ(batch.proposals[0].bytes, "git commit");
}

TEST(LeshperAutosuggest, AnEmptyBufferSuggestsNothing) {
	// An empty query matches every entry in every mode (#125), so without the
	// guard an empty line would suggest the last command - which is what the
	// up-arrow is for.
	suggest_fixture fixture{{"git status"}};
	const reactor_batch batch = fixture.suggest(suggest_fixture::line(""));
	EXPECT_EQ(batch.status, LESH_OK);
	EXPECT_TRUE(batch.texts.empty());
	EXPECT_TRUE(batch.proposals.empty());
}

TEST(LeshperAutosuggest, ACursorAwayFromTheEndSuggestsNothing) {
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	s.cursor = s.buffer.begin_position();
	const reactor_batch batch = fixture.suggest(s);
	EXPECT_EQ(batch.status, LESH_OK);
	EXPECT_TRUE(batch.texts.empty());
	EXPECT_TRUE(batch.proposals.empty());
}

TEST(LeshperAutosuggest, AnEntryEqualToTheBufferIsNotSuggested) {
	suggest_fixture fixture{{"git"}};
	const reactor_batch batch = fixture.suggest(suggest_fixture::line("git"));
	EXPECT_TRUE(batch.texts.empty());
	EXPECT_TRUE(batch.proposals.empty());
}

TEST(LeshperAutosuggest, AnEntryEqualToTheBufferIsWalkedPastRatherThanGivenUpOn) {
	// The commonest case in a history the user is retyping from. Stopping on the
	// identical entry would mean a line you have run before can never be
	// suggested past.
	suggest_fixture fixture{{"git status", "git"}};
	const reactor_batch batch = fixture.suggest(suggest_fixture::line("git"));
	ASSERT_EQ(batch.proposals.size(), 1u);
	EXPECT_EQ(batch.proposals[0].bytes, "git status");
}

TEST(LeshperAutosuggest, AnEntryThatOnlyCONTAINSTheQueryIsNotAPrefixMatch) {
	suggest_fixture fixture{{"sudo git status"}};
	const reactor_batch batch = fixture.suggest(suggest_fixture::line("git"));
	EXPECT_TRUE(batch.proposals.empty());
}

TEST(LeshperAutosuggest, AnEmptyHistorySuggestsNothingAndDoesNotFail) {
	suggest_fixture fixture;
	const reactor_batch batch = fixture.suggest(suggest_fixture::line("git"));
	EXPECT_EQ(batch.status, LESH_OK);
	EXPECT_TRUE(batch.proposals.empty());
}

TEST(LeshperAutosuggest, ItRecomputesWhenTheCursorMoves) {
	// Fish's rule, and the one place this reactor differs from the highlighter:
	// a suggestion is shown only at the end of the buffer, so moving off the end
	// must retract it and moving back must bring it back. A highlight is a
	// function of the text alone and asks for no such thing (A-10).
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");

	reactor_batch batch = fixture.suggest(s, LESH_EVENT_CURSOR_MOVED);
	EXPECT_EQ(batch.proposals.size(), 1u);

	s.cursor = s.buffer.begin_position();
	batch = fixture.suggest(s, LESH_EVENT_CURSOR_MOVED);
	EXPECT_TRUE(batch.proposals.empty());

	s.cursor = s.buffer.end_position();
	batch = fixture.suggest(s, LESH_EVENT_CURSOR_MOVED);
	EXPECT_EQ(batch.proposals.size(), 1u);
}

TEST(LeshperAutosuggest, ASupersededWalkGivesUpAndSaysSo) {
	suggest_fixture fixture;
	const superseding_source source{fixture.loop, {"git status", "git log", "git diff"}};
	owned_autosuggester self{&source};
	// Registration REPLACES (#93), so this is the fixture's autosuggester with a
	// different history behind it - and the flag the source raises is the one the
	// reactor polls, because the token carries a pointer to the loop that ran it.
	ASSERT_EQ(register_autosuggester(fixture.reg, self.get()), 1u);

	const lesh::leshper::state s = suggest_fixture::line("zzz");
	const reactor_batch batch = fixture.suggest(s);
	EXPECT_EQ(batch.status, LESH_ERR_SUPERSEDED);
	EXPECT_TRUE(batch.proposals.empty());
}

TEST(LeshperAutosuggest, AReactorWithNoHistoryWiredInRefusesRatherThanLooksEmpty) {
	// #125's rule: a provider wired up wrong should say so. F-17's null history
	// is a `vector_history_source` with nothing in it, which is a different
	// thing and answers LESH_OK.
	registry reg;
	loop_harness loop{reg};
	owned_autosuggester self{nullptr};
	ASSERT_EQ(register_autosuggester(reg, self.get()), 1u);

	const lesh::leshper::state s = suggest_fixture::line("git");
	std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_EQ(batches[0].status, LESH_ERR_INVAL);
}

TEST(LeshperAutosuggest, TheComputePathTakesNothingFromTheHeap) {
	// #90's rule, on the same instrument the highlighter is held to:
	// `heap_allocations` counts ONLY the arena's malloc fallback, so a non-zero
	// reading means the snapshot outgrew the pool. The searcher itself is asked
	// for no ranges, which is what keeps its scratch vector from allocating.
	std::vector<std::string> entries;
	for (int i = 0; i < 64; ++i)
		entries.push_back("git status --short --branch " + std::to_string(i));
	suggest_fixture fixture{entries};

	std::string line;
	while (line.size() < 4096)
		line += "git status --short --branch ";
	line.resize(4096);
	lesh::leshper::state s = suggest_fixture::line(line);

	// Warm once: the first walk is where a lazily-grown arena would grow.
	std::vector<reactor_batch> warm = fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(warm.size(), 1u);

	auto& counters = lesh::metrics::allocations();
	const std::size_t heap_before = counters.heap_allocations;
	const std::vector<reactor_batch> again =
		fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	EXPECT_EQ(counters.heap_allocations, heap_before);
	EXPECT_EQ(again.size(), 1u);
}

// ---------------------------------------------------------------------------
// lesh_proposal_read
// ---------------------------------------------------------------------------

TEST(LeshperAutosuggest, AnActionReadsTheProposalThatIsOnScreen) {
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	probe seen;
	ASSERT_EQ(lesh_action_register(&fixture.reg, "probe", probe_action, &seen), LESH_OK);
	EXPECT_EQ(fixture.loop.invoke(s, "probe", invocation{}).status, LESH_OK);
	EXPECT_EQ(seen.status, LESH_OK);
	EXPECT_EQ(seen.bytes, "git status");
	EXPECT_EQ(seen.length, 10u);
}

TEST(LeshperAutosuggest, NothingOnScreenReadsAsNotFoundRatherThanAsEmpty) {
	suggest_fixture fixture{{"ls"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	probe seen;
	ASSERT_EQ(lesh_action_register(&fixture.reg, "probe", probe_action, &seen), LESH_OK);
	EXPECT_EQ(fixture.loop.invoke(s, "probe", invocation{}).status, LESH_OK);
	EXPECT_EQ(seen.status, LESH_ERR_NOTFOUND);
	EXPECT_EQ(seen.length, 0u);
}

TEST(LeshperAutosuggest, AProposalOfAnotherKindIsNotThisOne) {
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	probe seen;
	seen.kind = LESH_PROPOSAL_HISTORY_MATCH;
	ASSERT_EQ(lesh_action_register(&fixture.reg, "probe", probe_action, &seen), LESH_OK);
	EXPECT_EQ(fixture.loop.invoke(s, "probe", invocation{}).status, LESH_OK);
	EXPECT_EQ(seen.status, LESH_ERR_NOTFOUND);
}

TEST(LeshperAutosuggest, PastTheLastProposalOfAKindIsNotFound) {
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	probe seen;
	seen.index = 1;
	ASSERT_EQ(lesh_action_register(&fixture.reg, "probe", probe_action, &seen), LESH_OK);
	EXPECT_EQ(fixture.loop.invoke(s, "probe", invocation{}).status, LESH_OK);
	EXPECT_EQ(seen.status, LESH_ERR_NOTFOUND);
}

// ---------------------------------------------------------------------------
// The accepting actions (F-25)
// ---------------------------------------------------------------------------

TEST(LeshperAutosuggest, AcceptingTheWholeSuggestionMakesItTheLine) {
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	const action_result result =
		fixture.loop.invoke(s, "accept_autosuggestion", invocation{});
	EXPECT_EQ(result.status, LESH_OK);
	EXPECT_TRUE(result.buffer_changed);
	EXPECT_EQ(s.buffer.text(), "git status --short");
	EXPECT_EQ(s.cursor.byte_offset(), 18u);
}

TEST(LeshperAutosuggest, AcceptingIsOneUndoEntry) {
	// A-12: a proposal reaches the buffer through staged writes and by no other
	// route, so accepting is one edit and undo puts back exactly what was typed.
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion", invocation{}).status, LESH_OK);
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, AcceptingAWordTakesOneWordAndStops) {
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_word", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git status");
	EXPECT_EQ(s.cursor.byte_offset(), 10u);
}

TEST(LeshperAutosuggest, AcceptingAWordTwiceWalksTheSuggestion) {
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_word", invocation{}).status,
	          LESH_OK);
	// The buffer moved, so the batch computed against the old generation is
	// stale (N-4) and a new one has to be computed before the second word can be
	// accepted - which is exactly what the loop does on a buffer change.
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_word", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git status --short");
}

TEST(LeshperAutosuggest, AcceptingAWordWithOneWordLeftTakesTheRest) {
	suggest_fixture fixture{{"gitk"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_word", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "gitk");
}

TEST(LeshperAutosuggest, AcceptingWithNothingSuggestedChangesNothing) {
	// Pressing the key with no suggestion showing is the ordinary case, not an
	// error - the rule undo already follows for pressing undo once too often.
	suggest_fixture fixture{{"ls"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	const action_result result =
		fixture.loop.invoke(s, "accept_autosuggestion", invocation{});
	EXPECT_EQ(result.status, LESH_OK);
	EXPECT_FALSE(result.buffer_changed);
	EXPECT_EQ(s.buffer.text(), "git");

	const action_result word =
		fixture.loop.invoke(s, "accept_autosuggestion_word", invocation{});
	EXPECT_EQ(word.status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, DismissingTakesTheWholeBatchOffTheScreen) {
	// The drawn half of a suggestion is its virtual text, so a dismissal that
	// left that showing would have dismissed nothing the user can see.
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	ASSERT_EQ(s.proposals.layers().size(), 1u);
	ASSERT_EQ(s.marks.texts().size(), 1u) << "the drawn half is the virtual text";

	EXPECT_EQ(fixture.loop.invoke(s, "dismiss_autosuggestion", invocation{}).status,
	          LESH_OK);
	EXPECT_TRUE(s.proposals.empty());
	EXPECT_TRUE(s.marks.texts().empty()) << "and it goes with the batch that carried it";
	EXPECT_EQ(s.buffer.text(), "git");

	// And accepting afterwards has nothing to accept.
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, DismissingWithNothingShowingIsNotAnError) {
	suggest_fixture fixture{{"ls"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "dismiss_autosuggestion", invocation{}).status,
	          LESH_OK);
	// The reactor's batch is still applied - it is simply empty. Dismissal drops
	// the batches that CARRY a proposal of the kind, and a batch that proposed
	// nothing was never showing anything to dismiss.
	for (const applied_proposals::layer& one : s.proposals.layers())
		EXPECT_TRUE(one.items.empty());
}

TEST(LeshperAutosuggest, ADismissedSuggestionComesBackOnTheNextEvent) {
	// Dismissal is about what is showing, not a mute: the reactor is free to
	// propose again, and the loop is free to apply it.
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "dismiss_autosuggestion", invocation{}).status,
	          LESH_OK);
	ASSERT_TRUE(s.proposals.empty());

	fixture.show(s);
	ASSERT_EQ(s.proposals.layers().size(), 1u);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git status");
}

TEST(LeshperAutosuggest, TheThreeActionsAreRegisteredUnderTheirNames) {
	suggest_fixture fixture;
	for (const char* name : {"accept_autosuggestion", "accept_autosuggestion_word",
	                         "dismiss_autosuggestion"}) {
		int32_t exists = 0;
		EXPECT_EQ(lesh_action_exists(&fixture.reg, name, &exists), LESH_OK);
		EXPECT_EQ(exists, 1) << name << " is not registered";
	}
}

TEST(LeshperAutosuggest, AStaleBatchIsNeverAcceptable) {
	// N-4, from the accepting side: the loop applies only what was computed
	// against the generation the editor is still at, so there is no way for an
	// action to read a proposal about text the buffer no longer holds.
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	std::vector<reactor_batch> batches = fixture.loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);

	// The user types on before the batch comes back.
	s.buffer.replace(s.buffer.end_position(), s.buffer.end_position(), std::string_view{"x"});
	s.cursor = s.buffer.end_position();
	s.gen.bump();
	EXPECT_FALSE(apply_batch(s, batches[0]));

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "gitx");
}
