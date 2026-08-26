#include "leshper/proposal.h"
#include "leshper/registry.h"
#include "leshper/state.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace lesh::leshper;

// WHAT A REACTOR OFFERED, AS THE STORE HOLDS IT (#133, #144, F-25).
//
// The store's own rules, and nothing that needs a driver to observe: a batch
// replaces the same reactor's previous one and keeps its place, an empty batch
// is still an application, the index walks one kind in emission order, and a
// dismissal takes the painted layer with the offers.
//
// The other half of the trail - a reactor proposes, a worker answers, the host
// applies, an action bound to a key reads index 0 - is `ui_proposal_tests.cpp`,
// where the driver is (#168). It moved there rather than being deleted here: the
// break #144 fixed was BETWEEN the two halves, and a test that never runs a real
// loop cannot see it.

namespace {

// --- The store --------------------------------------------------------------

std::vector<proposal> offering(std::uint32_t kind, std::initializer_list<const char*> bytes) {
	std::vector<proposal> made;
	for (const char* one : bytes)
		made.push_back(proposal{kind, std::string(one)});
	return made;
}
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
