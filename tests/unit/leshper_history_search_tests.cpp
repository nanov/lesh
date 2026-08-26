#include "leshper/abi.h"
#include "leshper/history_search.h"
#include "leshper/registry.h"
#include "leshper/state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lesh::leshper;

// The searcher's tests feed VECTORS and never open a file (#125).
//
// That is a property of the design and not a convenience of the tests: #94 put
// the searcher on leshper's side of the provider interface precisely so it
// depends on the shape of `for_each_newest_first` and on nothing else. A test
// here that read `~/.lesh_history` would pass differently on every machine and
// would be evidence that the split had not been achieved.

namespace {

// What a consumer that KEEPS results does: copy, because the match is borrowed
// for the sink call only.
struct kept {
	std::string entry;
	std::vector<history_search::range> ranges;
};

std::vector<kept> collect(history_search& search, std::string_view query,
                          const history_source& source,
                          const history_search::cancel_poll& cancelled = {}) {
	std::vector<kept> found;
	search.run(
		query, source,
		[&](const history_search::match& one) {
			found.push_back(kept{std::string{one.entry},
			                     std::vector<history_search::range>{one.ranges.begin(),
			                                                       one.ranges.end()}});
			return true;
		},
		cancelled);
	return found;
}

history_search searcher_for(history_search::mode which) {
	history_search::options opts;
	opts.search = which;
	return history_search{opts};
}

// A source that supersedes the request part-way through its own walk, so the
// poll has something to notice. The real trigger is the user typing while a
// worker is thinking; here it is the second entry.
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

state buffer_of(std::string_view text, std::size_t cursor) {
	state s;
	s.buffer.replace(s.buffer.begin_position(), s.buffer.begin_position(), text);
	s.cursor = s.buffer.clamped(position::from_byte_offset(cursor));
	s.gen.bump();
	return s;
}

} // namespace

// ---------------------------------------------------------------------------
// The source, and the order everything comes back in
// ---------------------------------------------------------------------------

TEST(LeshperHistorySearch, TheSourceWalksNewestFirst) {
	// "Newest first" falls out of "append-only" (#113), and the vector source
	// says so by holding entries in APPEND order and walking them backwards -
	// so a test reads like a session, oldest command written first.
	const vector_history_source source{{"first", "second", "third"}};

	std::vector<std::string> seen;
	source.for_each_newest_first([&](std::string_view entry) {
		seen.emplace_back(entry);
		return true;
	});
	EXPECT_EQ(seen, (std::vector<std::string>{"third", "second", "first"}));
}

TEST(LeshperHistorySearch, TheSourceStopsWhenTheWalkSaysStop) {
	const vector_history_source source{{"a", "b", "c"}};
	std::size_t seen = 0;
	source.for_each_newest_first([&](std::string_view) {
		++seen;
		return seen < 2;
	});
	EXPECT_EQ(seen, 2u);
}

TEST(LeshperHistorySearch, AnEmptySourceIsZeroMatchesAndNotAnError) {
	const vector_history_source source;
	history_search search = searcher_for(history_search::mode::line);
	const history_search::outcome result =
		search.run("anything", source, [](const history_search::match&) { return true; });
	EXPECT_EQ(result.entries_examined, 0u);
	EXPECT_EQ(result.matches, 0u);
	EXPECT_FALSE(result.stopped);
	EXPECT_FALSE(result.cancelled);
}

TEST(LeshperHistorySearch, MatchesComeBackNewestFirst) {
	const vector_history_source source{{"git status", "ls -l", "git commit"}};
	history_search search = searcher_for(history_search::mode::line);
	const std::vector<kept> found = collect(search, "git", source);

	ASSERT_EQ(found.size(), 2u);
	EXPECT_EQ(found[0].entry, "git commit");
	EXPECT_EQ(found[1].entry, "git status");
}

// ---------------------------------------------------------------------------
// F-32, line mode: substring anywhere
// ---------------------------------------------------------------------------

TEST(LeshperHistorySearch, LineModeMatchesASubstringAnywhere) {
	history_search search = searcher_for(history_search::mode::line);
	EXPECT_TRUE(search.matches("status", "git status"));
	EXPECT_TRUE(search.matches("tat", "git status"));
	EXPECT_FALSE(search.matches("commit", "git status"));
}

TEST(LeshperHistorySearch, LineModeRangesCoverEveryOccurrence) {
	// Both `git`s are why the entry is in the list, so both are highlighted.
	history_search search = searcher_for(history_search::mode::line);
	ASSERT_TRUE(search.matches("git", "git log | git diff"));
	const std::span<const history_search::range> ranges = search.last_ranges();
	ASSERT_EQ(ranges.size(), 2u);
	EXPECT_EQ(ranges[0], (history_search::range{0, 3}));
	EXPECT_EQ(ranges[1], (history_search::range{10, 13}));
}

TEST(LeshperHistorySearch, LineModeOccurrencesDoNotOverlap) {
	history_search search = searcher_for(history_search::mode::line);
	ASSERT_TRUE(search.matches("aa", "aaaa"));
	const std::span<const history_search::range> ranges = search.last_ranges();
	ASSERT_EQ(ranges.size(), 2u);
	EXPECT_EQ(ranges[0], (history_search::range{0, 2}));
	EXPECT_EQ(ranges[1], (history_search::range{2, 4}));
}

// ---------------------------------------------------------------------------
// F-33, prefix mode: navigation constrained to what is typed
// ---------------------------------------------------------------------------

TEST(LeshperHistorySearch, PrefixModeConstrainsNavigationToTheTypedPrefix) {
	// F-33 in the shape the requirement states it: up-arrow with `git c` typed
	// cycles matches only.
	const vector_history_source source{
		{"git status", "cargo build", "git checkout main", "git commit -m x"}};
	history_search search = searcher_for(history_search::mode::prefix);
	const std::vector<kept> found = collect(search, "git c", source);

	ASSERT_EQ(found.size(), 2u);
	EXPECT_EQ(found[0].entry, "git commit -m x");
	EXPECT_EQ(found[1].entry, "git checkout main");
	ASSERT_EQ(found[0].ranges.size(), 1u);
	EXPECT_EQ(found[0].ranges[0], (history_search::range{0, 5}));
}

TEST(LeshperHistorySearch, PrefixModeIsNotSubstringSearch) {
	history_search search = searcher_for(history_search::mode::prefix);
	EXPECT_TRUE(search.matches("git", "git status"));
	EXPECT_FALSE(search.matches("status", "git status"));
	// An entry that IS the query is a prefix of itself and matches. Excluding
	// the line the user is already on is the caller's policy, not the
	// searcher's - it is the only side that knows what is in the buffer.
	EXPECT_TRUE(search.matches("git status", "git status"));
}

// ---------------------------------------------------------------------------
// F-32, token mode: whole tokens, via C-6's lexer
// ---------------------------------------------------------------------------

TEST(LeshperHistorySearch, TokenModeMatchesWholeTokensOnly) {
	history_search search = searcher_for(history_search::mode::token);
	EXPECT_TRUE(search.matches("git", "git status"));
	EXPECT_TRUE(search.matches("status", "git status"));
	// The difference from line mode, and the whole reason the lexer is here:
	// `git` occurs inside `github-cli`, but not as a token.
	EXPECT_FALSE(search.matches("git", "github-cli auth"));
	EXPECT_TRUE(searcher_for(history_search::mode::line).matches("git", "github-cli auth"));
}

TEST(LeshperHistorySearch, TokenModeUsesTheLexersBoundariesAndNotBlanks) {
	history_search search = searcher_for(history_search::mode::token);
	// `foo|bar` is three tokens even without a blank in sight.
	EXPECT_TRUE(search.matches("foo", "foo|bar"));
	EXPECT_TRUE(search.matches("bar", "foo|bar"));
	// `'foo bar'` is ONE token, so neither half of it is one.
	EXPECT_FALSE(search.matches("foo", "echo 'foo bar'"));
	EXPECT_TRUE(search.matches("'foo bar'", "echo 'foo bar'"));
}

TEST(LeshperHistorySearch, TokenModeComparesSourceBytesWithoutQuoteRemoval) {
	// The argued decision (see match_token): a token is compared AS WRITTEN.
	// The searcher has a lexer and not an expander, and a match established
	// after quote removal would cover bytes that are not in the entry, leaving
	// F-32 with nothing honest to highlight.
	history_search search = searcher_for(history_search::mode::token);
	EXPECT_FALSE(search.matches("foo", "echo 'foo'"));
	EXPECT_FALSE(search.matches("foo", "echo \"foo\""));
	EXPECT_TRUE(search.matches("'foo'", "echo 'foo'"));
	EXPECT_TRUE(search.matches("\"foo\"", "echo \"foo\""));
}

TEST(LeshperHistorySearch, TokenModeMatchesAContiguousRunOfTokens) {
	history_search search = searcher_for(history_search::mode::token);
	ASSERT_TRUE(search.matches("git commit", "git commit -m x"));
	const std::span<const history_search::range> ranges = search.last_ranges();
	// One range per run, spanning the blank between the two tokens: the user
	// typed a phrase and sees a phrase highlighted.
	ASSERT_EQ(ranges.size(), 1u);
	EXPECT_EQ(ranges[0], (history_search::range{0, 10}));

	// Order matters, and so does contiguity.
	EXPECT_FALSE(search.matches("commit git", "git commit -m x"));
	EXPECT_FALSE(search.matches("git x", "git commit -m x"));
}

TEST(LeshperHistorySearch, TokenModeRangesLandOnTheTokenAndNotOnTheBlanks) {
	history_search search = searcher_for(history_search::mode::token);
	ASSERT_TRUE(search.matches("status", "git status --short"));
	const std::span<const history_search::range> ranges = search.last_ranges();
	ASSERT_EQ(ranges.size(), 1u);
	EXPECT_EQ(ranges[0], (history_search::range{4, 10}));
}

TEST(LeshperHistorySearch, TokenModeSurvivesAHalfTypedQuery) {
	// The lexer never fails (#9): an unterminated quote is a token that says
	// so, not an error path. An incremental search sees this on every other
	// keystroke and must simply not match.
	history_search search = searcher_for(history_search::mode::token);
	EXPECT_FALSE(search.matches("'", "echo hello"));
	EXPECT_FALSE(search.matches("$(", "echo hello"));
	EXPECT_TRUE(search.matches("'", "echo '"));
}

// ---------------------------------------------------------------------------
// F-34: multi-line entries stay multi-line
// ---------------------------------------------------------------------------

TEST(LeshperHistorySearch, MultiLineEntriesComeBackWithTheirNewlinesIntact) {
	// F-34: a recalled multi-line entry reconstructs as a 2D buffer. The
	// searcher's contribution is to never flatten one on the way through.
	const std::string loop_entry = "for f in *; do\n  echo $f\ndone";
	const vector_history_source source{{"ls", loop_entry}};

	history_search search = searcher_for(history_search::mode::line);
	const std::vector<kept> found = collect(search, "echo", source);
	ASSERT_EQ(found.size(), 1u);
	EXPECT_EQ(found[0].entry, loop_entry);
	EXPECT_NE(found[0].entry.find('\n'), std::string::npos);
	EXPECT_EQ(std::count(found[0].entry.begin(), found[0].entry.end(), '\n'), 2);
}

TEST(LeshperHistorySearch, TokenModeLexesAcrossTheNewlinesOfAMultiLineEntry) {
	const std::string loop_entry = "for f in *; do\n  echo $f\ndone";
	history_search search = searcher_for(history_search::mode::token);
	ASSERT_TRUE(search.matches("echo", loop_entry));
	const std::span<const history_search::range> ranges = search.last_ranges();
	ASSERT_EQ(ranges.size(), 1u);
	EXPECT_EQ(loop_entry.substr(ranges[0].start, ranges[0].end - ranges[0].start), "echo");
	// A query may itself be multi-line, and its newline is a token like any
	// other separator.
	EXPECT_TRUE(search.matches("echo $f\ndone", loop_entry));
}

TEST(LeshperHistorySearch, PrefixModeOnAMultiLineEntryMatchesTheFirstLine) {
	// F-33 with a multi-line entry: the typed prefix is compared against the
	// whole entry, newlines and all, so a partly typed `for f in` still finds
	// the loop the user ran yesterday.
	const std::string loop_entry = "for f in *; do\n  echo $f\ndone";
	history_search search = searcher_for(history_search::mode::prefix);
	EXPECT_TRUE(search.matches("for f in", loop_entry));
	EXPECT_FALSE(search.matches("echo", loop_entry));
}

// ---------------------------------------------------------------------------
// The degenerate query, and the caps
// ---------------------------------------------------------------------------

TEST(LeshperHistorySearch, AnEmptyQueryMatchesEverythingInEveryMode) {
	// Not a guard - it is plain history navigation, and all three modes have to
	// agree on it or the search UI's first keystroke would change the list out
	// from under the user.
	const vector_history_source source{{"a", "b", "c"}};
	for (const history_search::mode which :
	     {history_search::mode::line, history_search::mode::prefix,
	      history_search::mode::token}) {
		history_search search = searcher_for(which);
		const std::vector<kept> found = collect(search, "", source);
		ASSERT_EQ(found.size(), 3u);
		EXPECT_TRUE(found[0].ranges.empty());
		EXPECT_EQ(found[0].entry, "c");
	}
}

TEST(LeshperHistorySearch, ATokenQueryOfNothingButBlanksIsTheEmptyQuery) {
	history_search search = searcher_for(history_search::mode::token);
	EXPECT_TRUE(search.matches("   ", "anything at all"));
	EXPECT_TRUE(search.last_ranges().empty());
}

TEST(LeshperHistorySearch, MaxMatchesStopsTheWalk) {
	const vector_history_source source{{"git a", "git b", "git c", "git d"}};
	history_search::options opts;
	opts.search = history_search::mode::prefix;
	opts.max_matches = 2;
	history_search search{opts};

	std::vector<kept> found;
	const history_search::outcome result = search.run(
		"git", source,
		[&](const history_search::match& one) {
			found.push_back(kept{std::string{one.entry}, {}});
			return true;
		});
	EXPECT_EQ(found.size(), 2u);
	EXPECT_EQ(result.matches, 2u);
	EXPECT_TRUE(result.stopped);
	EXPECT_FALSE(result.cancelled);
	// And it really stopped: the two older entries were never looked at.
	EXPECT_EQ(result.entries_examined, 2u);
}

TEST(LeshperHistorySearch, MaxRangesCapsHighlightsWithoutDroppingTheMatch) {
	history_search::options opts;
	opts.search = history_search::mode::line;
	opts.max_ranges = 2;
	history_search search{opts};
	ASSERT_TRUE(search.matches("x", "x x x x x"));
	EXPECT_EQ(search.last_ranges().size(), 2u);
}

TEST(LeshperHistorySearch, ASinkThatSaysStopEndsTheWalk) {
	const vector_history_source source{{"a1", "a2", "a3"}};
	history_search search = searcher_for(history_search::mode::line);
	std::size_t seen = 0;
	const history_search::outcome result =
		search.run("a", source, [&](const history_search::match&) {
			++seen;
			return false;
		});
	EXPECT_EQ(seen, 1u);
	EXPECT_TRUE(result.stopped);
	EXPECT_EQ(result.entries_examined, 1u);
}

TEST(LeshperHistorySearch, TheCancelPollRunsBetweenEntries) {
	// #94's supersede poll: it runs BEFORE an entry is examined, so a cancelled
	// walk has nothing half-done and the sink was not called for work about to
	// be thrown away.
	const vector_history_source source{{"a1", "a2", "a3", "a4"}};
	history_search search = searcher_for(history_search::mode::line);
	std::size_t polls = 0;
	std::size_t matched = 0;

	const history_search::outcome result = search.run(
		"a", source,
		[&](const history_search::match&) {
			++matched;
			return true;
		},
		[&]() { return ++polls > 2; });

	EXPECT_EQ(matched, 2u);
	EXPECT_EQ(result.entries_examined, 2u);
	EXPECT_TRUE(result.cancelled);
	EXPECT_FALSE(result.stopped);
}

TEST(LeshperHistorySearch, OneSearcherServesManySearches) {
	// The scratch is a member so an incremental search that reruns on every
	// keystroke stops allocating. What has to be true for that to be safe is
	// that consecutive runs do not contaminate each other.
	const vector_history_source source{{"git status", "cargo build"}};
	history_search search = searcher_for(history_search::mode::token);

	EXPECT_EQ(collect(search, "git", source).size(), 1u);
	EXPECT_EQ(collect(search, "cargo", source).size(), 1u);
	EXPECT_EQ(collect(search, "nothing", source).size(), 0u);
	EXPECT_TRUE(search.last_ranges().empty());
	EXPECT_EQ(collect(search, "git status", source).size(), 1u);
}

// ---------------------------------------------------------------------------
// The provider face: a request token, streaming proposals, generation gating
// ---------------------------------------------------------------------------

TEST(LeshperHistorySearch, TheProviderEmitsMatchesAsProposalsNewestFirst) {
	registry reg;
	const vector_history_source source{{"ls -l", "git status", "git commit"}};
	history_search_provider provider;
	provider.source = &source;
	provider.options.search = history_search::mode::prefix;
	ASSERT_EQ(lesh_reactor_register(&reg, "history_search", LESH_EVENT_BUFFER_CHANGED,
	                                history_search_compute, &provider), LESH_OK);

	state s = buffer_of("git ", 4);
	loop_harness loop(reg);
	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);

	ASSERT_EQ(batches.size(), 1u);
	EXPECT_EQ(batches[0].status, LESH_OK);
	ASSERT_EQ(batches[0].proposals.size(), 2u);
	EXPECT_EQ(batches[0].proposals[0].bytes, "git commit");
	EXPECT_EQ(batches[0].proposals[1].bytes, "git status");
	EXPECT_EQ(batches[0].proposals[0].kind, LESH_PROPOSAL_HISTORY_MATCH);
}

TEST(LeshperHistorySearch, TheQueryIsTheSnapshotsTypedPrefix) {
	// buffer[0, cursor), taken from the TOKEN - which is what makes the result
	// generation-bound (N-4). Text to the right of the cursor is not part of
	// what the user has typed towards a match.
	registry reg;
	const vector_history_source source{{"git status", "git commit"}};
	history_search_provider provider;
	provider.source = &source;
	provider.options.search = history_search::mode::prefix;
	ASSERT_EQ(lesh_reactor_register(&reg, "history_search", LESH_EVENT_BUFFER_CHANGED,
	                                history_search_compute, &provider), LESH_OK);

	loop_harness loop(reg);
	state early = buffer_of("git commit --amend", 4);
	std::vector<reactor_batch> batches = loop.react(early, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_EQ(batches[0].proposals.size(), 2u);

	state late = buffer_of("git commit --amend", 10);
	batches = loop.react(late, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	ASSERT_EQ(batches[0].proposals.size(), 1u);
	EXPECT_EQ(batches[0].proposals[0].bytes, "git commit");
}

TEST(LeshperHistorySearch, TheProviderCarriesTheModeAndTheProposalKind) {
	registry reg;
	const vector_history_source source{{"github-cli auth", "git status"}};
	history_search_provider provider;
	provider.source = &source;
	provider.options.search = history_search::mode::token;
	provider.proposal_kind = LESH_PROPOSAL_COMPLETION;
	ASSERT_EQ(lesh_reactor_register(&reg, "history_search", LESH_EVENT_BUFFER_CHANGED,
	                                history_search_compute, &provider), LESH_OK);

	state s = buffer_of("git", 3);
	loop_harness loop(reg);
	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);

	ASSERT_EQ(batches.size(), 1u);
	ASSERT_EQ(batches[0].proposals.size(), 1u);
	EXPECT_EQ(batches[0].proposals[0].bytes, "git status");
	EXPECT_EQ(batches[0].proposals[0].kind, LESH_PROPOSAL_COMPLETION);
}

TEST(LeshperHistorySearch, ThePollNoticesMidWalkAndTheAnswerIsDroppedAnyway) {
	registry reg;
	loop_harness loop(reg);
	const superseding_source source{loop, {"git a", "git b", "git c"}};
	history_search_provider provider;
	provider.source = &source;
	provider.options.search = history_search::mode::prefix;
	ASSERT_EQ(lesh_reactor_register(&reg, "history_search", LESH_EVENT_BUFFER_CHANGED,
	                                history_search_compute, &provider), LESH_OK);

	state s = buffer_of("git", 3);
	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);

	ASSERT_EQ(batches.size(), 1u);
	// Streaming: the first match was already emitted when the supersede landed.
	EXPECT_EQ(batches[0].proposals.size(), 1u);
	// Giving up early is a courtesy to the worker, not a correctness mechanism.
	EXPECT_EQ(batches[0].status, LESH_ERR_SUPERSEDED);
}

TEST(LeshperHistorySearch, AStaleBatchIsNotApplied) {
	// N-4, and nothing here does the checking: the loop is the only applier and
	// it refuses a batch whose generation has moved on.
	registry reg;
	const vector_history_source source{{"git status"}};
	history_search_provider provider;
	provider.source = &source;
	provider.options.search = history_search::mode::prefix;
	ASSERT_EQ(lesh_reactor_register(&reg, "history_search", LESH_EVENT_BUFFER_CHANGED,
	                                history_search_compute, &provider), LESH_OK);

	state s = buffer_of("git", 3);
	loop_harness loop(reg);
	std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);

	// The user typed while the worker was walking the history.
	s.buffer.replace(s.buffer.end_position(), s.buffer.end_position(), " ");
	s.gen.bump();
	EXPECT_FALSE(apply_batch(s, batches[0]));
	EXPECT_TRUE(s.proposals.empty());

	batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_TRUE(apply_batch(s, batches[0]));
	ASSERT_EQ(s.proposals.layers().size(), 1u);
	EXPECT_EQ(s.proposals.layers()[0].items.size(), 1u);
}

TEST(LeshperHistorySearch, AProviderWiredUpWithoutASourceSaysSoRatherThanGuessing) {
	// Not an empty history: F-17's null history is a vector source with nothing
	// in it, so a null pointer here is a wiring bug and gets a wiring bug's
	// answer.
	registry reg;
	history_search_provider provider;
	ASSERT_EQ(lesh_reactor_register(&reg, "history_search", LESH_EVENT_BUFFER_CHANGED,
	                                history_search_compute, &provider), LESH_OK);

	state s = buffer_of("git", 3);
	loop_harness loop(reg);
	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_EQ(batches[0].status, LESH_ERR_INVAL);
	EXPECT_TRUE(batches[0].proposals.empty());
}

TEST(LeshperHistorySearch, ANullHistoryProducesNothingAndIsNotAnError) {
	// F-17: vared passes a bundle with a null history, and the whole of that is
	// a default-constructed source.
	registry reg;
	const vector_history_source none;
	history_search_provider provider;
	provider.source = &none;
	ASSERT_EQ(lesh_reactor_register(&reg, "history_search", LESH_EVENT_BUFFER_CHANGED,
	                                history_search_compute, &provider), LESH_OK);

	state s = buffer_of("anything", 8);
	loop_harness loop(reg);
	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	EXPECT_EQ(batches[0].status, LESH_OK);
	EXPECT_TRUE(batches[0].proposals.empty());
}

TEST(LeshperHistorySearch, AMultiLineMatchCrossesTheAbiUnflattened) {
	// F-34 through the provider face: the proposal carries the newlines, so
	// what an accepting action puts back in the buffer is the 2D construct the
	// user originally typed.
	registry reg;
	const std::string loop_entry = "for f in *; do\n  echo $f\ndone";
	const vector_history_source source{{loop_entry}};
	history_search_provider provider;
	provider.source = &source;
	provider.options.search = history_search::mode::prefix;
	ASSERT_EQ(lesh_reactor_register(&reg, "history_search", LESH_EVENT_BUFFER_CHANGED,
	                                history_search_compute, &provider), LESH_OK);

	state s = buffer_of("for", 3);
	loop_harness loop(reg);
	const std::vector<reactor_batch> batches = loop.react(s, LESH_EVENT_BUFFER_CHANGED);
	ASSERT_EQ(batches.size(), 1u);
	ASSERT_EQ(batches[0].proposals.size(), 1u);
	EXPECT_EQ(batches[0].proposals[0].bytes, loop_entry);
}
