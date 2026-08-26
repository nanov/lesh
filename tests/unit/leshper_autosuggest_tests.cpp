#include "leshper/abi.h"
#include "leshper/history_search.h"
#include "leshper/layout.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "leshper/surface.h"
#include "substrate/arena.h"
#include "substrate/measure.h"

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

	// Types through the SAME door a keystroke uses - one `self_insert` dispatch
	// per character - so the undo history is left mid-run, coalescing armed. The
	// `line()` seed above writes the buffer directly and leaves no run open,
	// which is why it cannot pin #146's defect.
	void type(lesh::leshper::state& s, std::string_view text) {
		for (const char byte : text) {
			invocation how;
			how.keys.assign(1, byte);
			EXPECT_EQ(loop.invoke(s, "self_insert", how).status, LESH_OK);
		}
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

// The glyphs of one laid-out row, concatenated - the layout suite's helper, kept
// small here because the one multi-line test below is about a PICTURE and a
// proposal read back as bytes would not have shown the continuation lines.
std::string glyphs_of(const cluster_pool& pool, const surface& painted, std::uint16_t row) {
	std::string out;
	for (std::uint16_t column = 0; column < painted.columns(); ++column) {
		const cell& one = painted.at(row, column);
		if (!one.glyph.is_continuation())
			out.append(pool.cluster_of(one.glyph));
	}
	return out;
}

// `head`, then blanks out to the width - computed rather than written out, so an
// expectation is never a run of spaces nobody can count by eye.
std::string padded(std::string_view head, int columns) {
	std::string out{head};
	for (int filled = lesh::grapheme::display_width(head); filled < columns; ++filled)
		out.push_back(' ');
	return out;
}

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

// #146: the same rule #121 gave a paste, for the same reason. An accepted
// candidate is SHAPED like a plain insertion at the cursor, so the F-4 heuristic
// would fold it into the typing run that preceded it and re-arm the run behind
// it. It breaks the run on BOTH sides instead: accept is its own undo step, and
// the `git` the user typed survives one undo.
TEST(LeshperAutosuggest, AcceptingBreaksTheTypingRunItFollows) {
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s;
	fixture.type(s, "git");   // a REAL run, coalescing armed
	ASSERT_TRUE(s.undo.coalescing());
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion", invocation{}).status, LESH_OK);
	ASSERT_EQ(s.buffer.text(), "git status --short");
	// Two steps, not one: the typing run and the acceptance.
	EXPECT_EQ(s.undo.step_count(), 2u);

	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "");
}

TEST(LeshperAutosuggest, TypingAfterAnAcceptStartsItsOwnUndoStep) {
	// The "after" break, which is the load-bearing half (#121): apply_edit's
	// record() would otherwise leave the history coalescing again, and the next
	// typed character would fold into the acceptance.
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s;
	fixture.type(s, "git");
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion", invocation{}).status, LESH_OK);
	EXPECT_FALSE(s.undo.coalescing());
	fixture.type(s, " -v");

	ASSERT_EQ(s.buffer.text(), "git status -v");
	EXPECT_EQ(s.undo.step_count(), 3u);
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git status");
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, AcceptingASingleCharacterCandidateBreaksTheRunToo) {
	// The length-1 case, and the reason the discriminator is not "how long was
	// it". Accepting `gitk` after `git` writes ONE cluster - the same shape a
	// keystroke writes - and only the fact that it is not the keystroke that
	// dispatched the action tells the two apart.
	suggest_fixture fixture{{"gitk"}};
	lesh::leshper::state s;
	fixture.type(s, "git");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion", invocation{}).status, LESH_OK);
	ASSERT_EQ(s.buffer.text(), "gitk");
	EXPECT_EQ(s.undo.step_count(), 2u);
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, AcceptingAWordBreaksTheTypingRunItFollows) {
	// The word-at-a-time twin, which stages TWO writes and still commits one
	// insertion - so it reaches the coalescing rule looking exactly like the
	// whole-line accept and has to be told apart the same way.
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s;
	fixture.type(s, "git");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_word", invocation{}).status,
	          LESH_OK);
	ASSERT_EQ(s.buffer.text(), "git status");
	EXPECT_EQ(s.undo.step_count(), 2u);
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, TypingItselfStillCoalescesThroughTheSameDoor) {
	// The other side of the discriminator, pinned here because the rule that
	// tells an acceptance from a keystroke lives in the commit path both use. A
	// run of self_insert is still ONE undo step.
	suggest_fixture fixture;
	lesh::leshper::state s;
	fixture.type(s, "echo hi");
	EXPECT_EQ(s.undo.step_count(), 1u);
	EXPECT_TRUE(s.undo.coalescing());
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "");
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

// ---------------------------------------------------------------------------
// Accepting by FALLTHROUGH (#140 decision 2, #147)
//
// One implementation, five thin names, and the fallback in the userdata beside
// each. What is asserted here is the ACTION's half of the rule; the keymap
// suite asserts the table, and the vi suite asserts that an operator cannot
// reach any of it.
// ---------------------------------------------------------------------------

TEST(LeshperAutosuggest, TheFiveWrappersAreRegisteredAndSoIsTheMotionOneOfThemNeeded) {
	suggest_fixture fixture;
	for (const char* name : {"accept_suggestion_or_forward_char",
	                         "accept_suggestion_or_end_of_line",
	                         "accept_suggestion_or_forward_word",
	                         "accept_suggestion_or_word_start_next",
	                         "accept_suggestion_or_word_end_next",
	                         // `<A-f>` had no binding before #140, so emacs's
	                         // forward-word arrives with the table that needs it.
	                         "forward_word"}) {
		int32_t exists = 0;
		EXPECT_EQ(lesh_action_exists(&fixture.reg, name, &exists), LESH_OK);
		EXPECT_EQ(exists, 1) << name << " is not registered";
	}
}

TEST(LeshperAutosuggest, AtTheEndWithASuggestionShowingTheWrapperAccepts) {
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	const action_result result =
		fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{});
	EXPECT_EQ(result.status, LESH_OK);
	EXPECT_TRUE(result.buffer_changed);
	EXPECT_EQ(s.buffer.text(), "git status --short");
	EXPECT_EQ(s.cursor.byte_offset(), 18u);
}

TEST(LeshperAutosuggest, EveryWholeLineWrapperAcceptsTheSameWayAtTheEnd) {
	// `<Right>`, `<C-f>`, `<End>` and `<C-e>` are two names, and both accept the
	// WHOLE candidate: #140's table gives command mode the partial accepts and
	// the inserting keymaps both halves.
	for (const char* name : {"accept_suggestion_or_forward_char",
	                         "accept_suggestion_or_end_of_line"}) {
		suggest_fixture fixture{{"git status --short"}};
		lesh::leshper::state s = suggest_fixture::line("git");
		fixture.show(s);
		EXPECT_EQ(fixture.loop.invoke(s, name, invocation{}).status, LESH_OK) << name;
		EXPECT_EQ(s.buffer.text(), "git status --short") << name;
	}
}

TEST(LeshperAutosuggest, TheWordWrapperTakesOneWordTheWayTheActionItComposesDoes) {
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_forward_word", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git status");
	EXPECT_EQ(s.cursor.byte_offset(), 10u);
}

TEST(LeshperAutosuggest, WithNothingSuggestedTheWrapperIsTheMotionAndNothingElse) {
	// The half that matters most: these are the keys `<Right>` and `<End>` have
	// always been, and a user with no history must not be able to tell that
	// anything was wrapped around them.
	suggest_fixture fixture{{"ls"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	s.cursor = lesh::leshper::position::from_byte_offset(1);

	const action_result moved =
		fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{});
	EXPECT_EQ(moved.status, LESH_OK);
	EXPECT_FALSE(moved.buffer_changed);
	EXPECT_EQ(s.buffer.text(), "git");
	EXPECT_EQ(s.cursor.byte_offset(), 2u);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_end_of_line", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.cursor.byte_offset(), 3u);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, MidLineTheWrapperMovesEvenWithASuggestionShowing) {
	// The cursor is not at the end, so `<Right>` moves one cluster - and it does
	// so with the batch still applied, which is the case a rule written as "is
	// anything showing" alone would have got wrong.
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	ASSERT_EQ(s.proposals.layers().size(), 1u);
	s.cursor = lesh::leshper::position::from_byte_offset(1);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
	EXPECT_EQ(s.cursor.byte_offset(), 2u);
}

TEST(LeshperAutosuggest, EndMidLineFallsThroughAndTheNextPressAccepts) {
	// FISH'S BEHAVIOUR, pinned because it looks like a bug and is not. `<End>`
	// with the cursor mid-line is not at the end, so it falls through to
	// `end_of_line` - which lands it at the end, where the suggestion is now
	// acceptable and the SECOND press takes it. Two presses, two meanings, and
	// the alternative - accepting from anywhere on the line - would make `<End>`
	// a key you could not use to go to the end.
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	s.cursor = lesh::leshper::position::from_byte_offset(0);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_end_of_line", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
	EXPECT_EQ(s.cursor.byte_offset(), 3u);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_end_of_line", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git status");
}

TEST(LeshperAutosuggest, ASuggestionNoLongerThanTheBufferFallsThroughRatherThanSwallowingTheKey) {
	// A proposal that has nothing left past what is typed is not something to
	// accept, and a key that vanished into an accept-that-did-nothing would be a
	// dead `<Right>`. The autosuggester never proposes one, so this is built by
	// hand out of the same batch shape it emits.
	suggest_fixture fixture;
	lesh::leshper::state s = suggest_fixture::line("git");

	reactor_batch batch;
	batch.reactor = "autosuggester";
	batch.computed_against = s.gen;
	batch.event_kind = LESH_EVENT_BUFFER_CHANGED;
	batch.proposals.push_back(proposal{LESH_PROPOSAL_AUTOSUGGESTION, "git"});
	ASSERT_TRUE(apply_batch(s, batch));
	s.cursor = lesh::leshper::position::from_byte_offset(1);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
	EXPECT_EQ(s.cursor.byte_offset(), 2u) << "the key was swallowed by an empty accept";
}

TEST(LeshperAutosuggest, TheWrapperIsOneUndoEntryOnBothPaths) {
	// #110's wrapper rule, which is what `lesh_action_invoke` buys: the delegate
	// stages into the caller's staging area and its undo group, so neither half
	// of this key can be two steps.
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	const std::size_t before = s.undo.step_count();
	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{}).status,
	          LESH_OK);
	ASSERT_EQ(s.buffer.text(), "git status --short");
	EXPECT_EQ(s.undo.step_count(), before + 1);
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

// #146, from both sides of the same key. The rule that landed there is about
// what a BLOCK WRITE does to the typing run; the fallthrough stages nothing at
// all, so it must leave the history exactly where the bare motion leaves it.
TEST(LeshperAutosuggest, TheFallthroughLeavesTheUndoHistoryWhereTheBareMotionLeavesIt) {
	// EQUIVALENCE IS THE ASSERTION, and it is deliberately not "the run
	// survives". F-4 ends a typing run on any cursor move - undo.h: "an edit
	// somewhere else in the buffer - ends the run, which is why the editor calls
	// break_coalescing() on every non-inserting action" - so `forward_char` has
	// always broken it, wrapped or not. What #146 asks of this path is that the
	// wrapper add NO undo step of its own and behave byte for byte like the key
	// it wraps, which is what two runs off one starting point can show.
	const auto motion_from_mid_line = [](const char* action, lesh::leshper::state& s) {
		suggest_fixture fixture{{"ls"}};
		fixture.type(s, "git");
		ASSERT_TRUE(s.undo.coalescing());
		fixture.show(s);
		s.cursor = lesh::leshper::position::from_byte_offset(1);
		ASSERT_EQ(fixture.loop.invoke(s, action, invocation{}).status, LESH_OK);
	};

	lesh::leshper::state bare;
	motion_from_mid_line("forward_char", bare);
	lesh::leshper::state wrapped;
	motion_from_mid_line("accept_suggestion_or_forward_char", wrapped);

	EXPECT_EQ(wrapped.buffer.text(), bare.buffer.text());
	EXPECT_EQ(wrapped.cursor.byte_offset(), bare.cursor.byte_offset());
	EXPECT_EQ(wrapped.undo.step_count(), bare.undo.step_count());
	EXPECT_EQ(wrapped.undo.coalescing(), bare.undo.coalescing());

	// And concretely: the typed `git` is still ONE step, because the motion
	// wrote nothing for the history to record.
	EXPECT_EQ(wrapped.buffer.text(), "git");
	EXPECT_EQ(wrapped.cursor.byte_offset(), 2u);
	EXPECT_EQ(wrapped.undo.step_count(), 1u);
}

TEST(LeshperAutosuggest, TheWrappersAcceptBreaksTheTypingRunTheWayTheBareAcceptDoes) {
	// The other side: the accept half reaches the buffer as a block write, and
	// #146's rule does not care which name dispatched it.
	suggest_fixture fixture{{"git status --short"}};
	lesh::leshper::state s;
	fixture.type(s, "git");
	ASSERT_TRUE(s.undo.coalescing());
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{}).status,
	          LESH_OK);
	ASSERT_EQ(s.buffer.text(), "git status --short");
	EXPECT_FALSE(s.undo.coalescing());
	EXPECT_EQ(s.undo.step_count(), 2u);

	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "");
}

TEST(LeshperAutosuggest, TheFallthroughPathTakesNothingFromTheHeap) {
	// A keystroke path, held to #90's instrument: the wrapper asks for the
	// proposal's LENGTH with capacity zero rather than copying it, so the key
	// that only moved the cursor copies nothing at all.
	suggest_fixture fixture{{"git status --short --branch"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	s.cursor = lesh::leshper::position::from_byte_offset(1);

	// Warm once: the first dispatch is where a lazily-grown scratch would grow.
	ASSERT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{}).status,
	          LESH_OK);
	s.cursor = lesh::leshper::position::from_byte_offset(1);

	auto& counters = lesh::metrics::allocations();
	const std::size_t heap_before = counters.heap_allocations;
	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(counters.heap_allocations, heap_before);
}


// ---------------------------------------------------------------------------
// Accepting ONE CHARACTER (#149): fish's `forward-single-char`, unbound.
//
// The unit is a grapheme CLUSTER, not a byte, and that is what most of this
// section is about - a precomposed `é`, a decomposed one, a flag and a ZWJ
// sequence each come in on one press. The keymap suite asserts that neither of
// the two new names is bound anywhere.
// ---------------------------------------------------------------------------

TEST(LeshperAutosuggest, TheTwoPerCharacterNamesAreRegistered) {
	suggest_fixture fixture;
	for (const char* name : {"accept_autosuggestion_char",
	                         "accept_suggestion_char_or_forward_char"}) {
		int32_t exists = 0;
		EXPECT_EQ(lesh_action_exists(&fixture.reg, name, &exists), LESH_OK);
		EXPECT_EQ(exists, 1) << name << " is not registered";
	}
}

TEST(LeshperAutosuggest, AcceptingACharacterTakesOneAsciiCharacterAndStops) {
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git ");
	EXPECT_EQ(s.cursor.byte_offset(), 4u);
}

TEST(LeshperAutosuggest, APrecomposedAccentComesInOnOnePressAndAsTwoBytes) {
	// U+00E9 is two bytes and one cluster. A byte rule would leave the buffer
	// holding half a code point here, which is not a string any more.
	suggest_fixture fixture{{"caf\xc3\xa9 latte"}};
	lesh::leshper::state s = suggest_fixture::line("ca");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "caf");
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "caf\xc3\xa9");
	EXPECT_EQ(s.cursor.byte_offset(), 5u) << "the accent arrived a byte at a time";
}

TEST(LeshperAutosuggest, ADecomposedAccentComesInWholeOnOnePress) {
	// `e` + U+0301 COMBINING ACUTE: three bytes, still ONE cluster, and the press
	// that takes the `e` must take the mark with it. This is the case where the
	// typed prefix can even sit INSIDE the cluster - the history holds `cafe` as
	// a byte prefix of `café` - and the boundary question is asked of the
	// editor, so the answer is the segmentation's rather than this action's.
	suggest_fixture fixture{{"cafe\xcc\x81 latte"}};
	lesh::leshper::state s = suggest_fixture::line("caf");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cafe\xcc\x81");
	EXPECT_EQ(s.cursor.byte_offset(), 6u) << "the combining mark was left behind";

	// And the press after it takes the space, not the mark that is already in.
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "cafe\xcc\x81 ");
}

TEST(LeshperAutosuggest, AZwjSequenceAndAFlagAreEachOnePress) {
	// U+1F469 ZWJ U+1F4BB is eleven bytes and one image; the DE flag is two
	// regional indicators, eight bytes and one image (GB11 and GB12/GB13). Both
	// arrive whole, because `LESH_MOTION_NEXT_CLUSTER` is the same UAX-29 walk
	// the renderer measures with.
	const std::string woman_technologist = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";
	const std::string flag = "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa";
	suggest_fixture fixture{{"echo " + woman_technologist + flag + "!"}};
	lesh::leshper::state s = suggest_fixture::line("echo ");
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "echo " + woman_technologist);
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "echo " + woman_technologist + flag)
		<< "the flag came in one regional indicator at a time";
	fixture.show(s);
	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "echo " + woman_technologist + flag + "!");
}

TEST(LeshperAutosuggest, RepeatedPressesWalkTheSuggestionToItsEndAndThenDoNothing) {
	// The whole point of a per-character accept: press it enough times and you
	// have the candidate, one cluster at a time - and the press after that is the
	// #133 no-op, because there is nothing past what is typed.
	suggest_fixture fixture{{"gitk"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	for (int press = 0; press < 4; ++press) {
		fixture.show(s);
		const action_result result =
			fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{});
		EXPECT_EQ(result.status, LESH_OK) << "press " << press;
	}
	EXPECT_EQ(s.buffer.text(), "gitk");
	EXPECT_EQ(s.cursor.byte_offset(), 4u);

	// One press past the end: LESH_OK, nothing written.
	fixture.show(s);
	const action_result spare =
		fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{});
	EXPECT_EQ(spare.status, LESH_OK);
	EXPECT_FALSE(spare.buffer_changed);
	EXPECT_EQ(s.buffer.text(), "gitk");
}

TEST(LeshperAutosuggest, AcceptingACharacterWithNothingSuggestedChangesNothing) {
	suggest_fixture fixture{{"ls"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	const action_result result =
		fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{});
	EXPECT_EQ(result.status, LESH_OK);
	EXPECT_FALSE(result.buffer_changed);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, EachAcceptedClusterIsOneUndoEntry) {
	// A-12 per press: three presses, three steps, and undo walks back one cluster
	// at a time rather than unpicking the lot.
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	const std::size_t before = s.undo.step_count();
	for (int press = 0; press < 3; ++press) {
		fixture.show(s);
		ASSERT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
		          LESH_OK);
	}
	ASSERT_EQ(s.buffer.text(), "git st");
	EXPECT_EQ(s.undo.step_count(), before + 3);

	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git s");
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git ");
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

TEST(LeshperAutosuggest, AcceptingOneCharacterBreaksTheTypingRunItFollows) {
	// #146'S RULE, APPLIED RATHER THAN REOPENED, and this is the case that makes
	// the temptation concrete: accepting the `k` of `gitk` writes exactly what
	// typing `k` would have written - one cluster, plain insertion, at the point.
	// It still breaks the run, because the third mark is the bytes being the
	// dispatching key's, and an accept's bytes come off the candidate. Two steps,
	// and the typed `git` survives the first undo.
	suggest_fixture fixture{{"gitk"}};
	lesh::leshper::state s;
	fixture.type(s, "git");
	ASSERT_TRUE(s.undo.coalescing());
	fixture.show(s);

	EXPECT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	ASSERT_EQ(s.buffer.text(), "gitk");
	EXPECT_FALSE(s.undo.coalescing()) << "the run was re-armed behind the accept";
	EXPECT_EQ(s.undo.step_count(), 2u);

	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "");
}

TEST(LeshperAutosuggest, TypingAfterAOneCharacterAcceptStartsItsOwnUndoStep) {
	// The "after" half of the both-sides break (#121, #146). Without it the next
	// typed character would fold into the acceptance.
	suggest_fixture fixture{{"gitk"}};
	lesh::leshper::state s;
	fixture.type(s, "git");
	fixture.show(s);
	ASSERT_EQ(fixture.loop.invoke(s, "accept_autosuggestion_char", invocation{}).status,
	          LESH_OK);
	fixture.type(s, " -v");

	ASSERT_EQ(s.buffer.text(), "gitk -v");
	EXPECT_EQ(s.undo.step_count(), 3u);
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "gitk");
}

TEST(LeshperAutosuggest, TheCharacterWrapperAcceptsOneClusterAtTheEnd) {
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);

	const action_result result =
		fixture.loop.invoke(s, "accept_suggestion_char_or_forward_char", invocation{});
	EXPECT_EQ(result.status, LESH_OK);
	EXPECT_TRUE(result.buffer_changed);
	EXPECT_EQ(s.buffer.text(), "git ");
	EXPECT_EQ(s.cursor.byte_offset(), 4u);
}

TEST(LeshperAutosuggest, TheCharacterWrapperWalksToTheEndAndThenFallsThrough) {
	// Repeated presses take the candidate a cluster at a time; the press after
	// the last one has nothing longer than the buffer to accept, so the wrapper
	// is `forward_char` again - and `forward_char` at the end of the buffer moves
	// nowhere, which is what makes the key harmless rather than dead.
	suggest_fixture fixture{{"gitk"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	ASSERT_EQ(
		fixture.loop.invoke(s, "accept_suggestion_char_or_forward_char", invocation{}).status,
		LESH_OK);
	ASSERT_EQ(s.buffer.text(), "gitk");

	fixture.show(s);
	const action_result spare =
		fixture.loop.invoke(s, "accept_suggestion_char_or_forward_char", invocation{});
	EXPECT_EQ(spare.status, LESH_OK);
	EXPECT_FALSE(spare.buffer_changed);
	EXPECT_EQ(s.buffer.text(), "gitk");
	EXPECT_EQ(s.cursor.byte_offset(), 4u);
}

TEST(LeshperAutosuggest, TheCharacterWrappersFallthroughIsTheBareForwardChar) {
	// #147's equivalence shape, for #149's row: buffer, cursor, step count and
	// coalescing identical to the unwrapped motion. Not "the run survives" -
	// undo.h ends a typing run on every non-inserting action, so a bare
	// `forward_char` has always broken it.
	const auto motion_from_mid_line = [](const char* action, lesh::leshper::state& s) {
		suggest_fixture fixture{{"ls"}};
		fixture.type(s, "git");
		ASSERT_TRUE(s.undo.coalescing());
		fixture.show(s);
		s.cursor = lesh::leshper::position::from_byte_offset(1);
		ASSERT_EQ(fixture.loop.invoke(s, action, invocation{}).status, LESH_OK);
	};

	lesh::leshper::state bare;
	motion_from_mid_line("forward_char", bare);
	lesh::leshper::state wrapped;
	motion_from_mid_line("accept_suggestion_char_or_forward_char", wrapped);

	EXPECT_EQ(wrapped.buffer.text(), bare.buffer.text());
	EXPECT_EQ(wrapped.cursor.byte_offset(), bare.cursor.byte_offset());
	EXPECT_EQ(wrapped.undo.step_count(), bare.undo.step_count());
	EXPECT_EQ(wrapped.undo.coalescing(), bare.undo.coalescing());

	EXPECT_EQ(wrapped.buffer.text(), "git");
	EXPECT_EQ(wrapped.cursor.byte_offset(), 2u);
	EXPECT_EQ(wrapped.undo.step_count(), 1u);
}

TEST(LeshperAutosuggest, MidLineTheCharacterWrapperMovesEvenWithASuggestionShowing) {
	// The cursor is not at the end, so it is `<Right>` and nothing more - the
	// same three questions #147 wrote, asked by the same implementation.
	suggest_fixture fixture{{"git status"}};
	lesh::leshper::state s = suggest_fixture::line("git");
	fixture.show(s);
	ASSERT_EQ(s.proposals.layers().size(), 1u);
	s.cursor = lesh::leshper::position::from_byte_offset(1);

	EXPECT_EQ(
		fixture.loop.invoke(s, "accept_suggestion_char_or_forward_char", invocation{}).status,
		LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
	EXPECT_EQ(s.cursor.byte_offset(), 2u);
}

TEST(LeshperAutosuggest, TheCharacterWrappersAcceptIsOneUndoEntryAndBreaksTheRun) {
	// Both halves of the #146 pin through the wrapper, which composes the same
	// action through `lesh_action_invoke` and so cannot be two steps (#110).
	suggest_fixture fixture{{"gitk"}};
	lesh::leshper::state s;
	fixture.type(s, "git");
	ASSERT_TRUE(s.undo.coalescing());
	fixture.show(s);

	EXPECT_EQ(
		fixture.loop.invoke(s, "accept_suggestion_char_or_forward_char", invocation{}).status,
		LESH_OK);
	ASSERT_EQ(s.buffer.text(), "gitk");
	EXPECT_FALSE(s.undo.coalescing());
	EXPECT_EQ(s.undo.step_count(), 2u);
	EXPECT_EQ(fixture.loop.invoke(s, "undo", invocation{}).status, LESH_OK);
	EXPECT_EQ(s.buffer.text(), "git");
}

// ---------------------------------------------------------------------------
// Multi-line candidates (#140 decision 3): suggested whole, no filter.
// ---------------------------------------------------------------------------

TEST(LeshperAutosuggest, AMultiLineCandidateIsSuggestedWholeAndItsContinuationLinesRender) {
	// NO CODE CHANGED FOR THIS, which is the assertion. #141 made virtual bytes
	// buffer bytes and #123 treats a line break as a cluster, so a remembered
	// `for` loop suggests all three of its lines and the layout draws the two
	// below the edit line - ghost-styled, reflowing, and acceptable whole. The
	// entries most worth recalling are the long ones; a filter is one predicate
	// later if daily use proves it noisy.
	suggest_fixture fixture{{"for f in *\ndo echo $f\ndone"}};
	lesh::leshper::state s = suggest_fixture::line("for");
	fixture.show(s);

	ASSERT_EQ(s.proposals.layers().size(), 1u);
	const proposal* whole = s.proposals.find(LESH_PROPOSAL_AUTOSUGGESTION, 0);
	ASSERT_NE(whole, nullptr);
	EXPECT_EQ(whole->bytes, "for f in *\ndo echo $f\ndone") << "the candidate was filtered";

	// And it is DRAWN: the virtual text carries the newlines, so the picture has
	// three content rows and the two continuations are on them.
	ASSERT_EQ(s.marks.texts().size(), 1u);
	EXPECT_EQ(s.marks.texts()[0].bytes, " f in *\ndo echo $f\ndone");

	s.columns = 40;
	s.rows = 10;
	cluster_pool pool;
	const layout picture = lay_out(pool, input_for(s, "$ "));
	EXPECT_EQ(picture.content_rows, 3);
	ASSERT_GE(picture.screen.rows(), 3);
	EXPECT_EQ(glyphs_of(pool, picture.screen, 0), padded("$ for f in *", 40));
	EXPECT_EQ(glyphs_of(pool, picture.screen, 1), padded("do echo $f", 40));
	EXPECT_EQ(glyphs_of(pool, picture.screen, 2), padded("done", 40));

	// Accepting takes all three lines, because the proposal IS the line.
	EXPECT_EQ(fixture.loop.invoke(s, "accept_suggestion_or_forward_char", invocation{}).status,
	          LESH_OK);
	EXPECT_EQ(s.buffer.text(), "for f in *\ndo echo $f\ndone");
}
