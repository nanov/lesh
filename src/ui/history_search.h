#pragma once

// The history SEARCHER (#94, #125): F-32's line/prefix/token modes and F-33's
// prefix-constrained navigation, as filters over one newest-first iteration.
//
// #94 split the history in two. The store is dumb - append, snapshot-iterate
// newest-first, entries preserve newlines - behind the `HistoryStore` override
// point A-13 names. Everything that knows what a SEARCH is lives here, and uses
// C-6's independently callable lexer for token mode.
//
// BOTH HALVES ARE THE HOST'S (#168 Phase B). This file was `src/leshper/`'s, on
// the argument that a searcher depends on the shape of `for_each_newest_first`
// and on nothing else about the store - which is true, and was never the
// question. What a search IS is HISTORY KNOWLEDGE, and it needs a LEXER to be
// what it is; the editor is the side that neither knows what a shell command
// looks like nor links `lesh_syntax`. So the split that matters runs between the
// store and the editor, not between the store and the searcher, and both halves
// sit on this side of it. What is unchanged: this file still depends on the shape
// and not on the file, so a user-supplied history provider serves it exactly as
// the built-in one does (A-13), and the tests feed vectors.
//
// TWO FACES, and only one of them is C.
//
//   `history_search` is a pure type. Query in, matches out through a sink,
//   newest first, with the byte ranges the match covered. It touches no
//   filesystem, no thread, no clock, and no editor state; it is the half a
//   test can drive with a vector and the half a native consumer calls
//   directly when it wants the ranges (see below).
//
//   `history_search_compute` is the PROVIDER face - a `lesh_reactor_fn`,
//   running inside a reactor's slice against a request token it did not mint, emitting
//   through `lesh_propose` and polling the supersede flag between entries.
//   It includes `abi.h` and nothing else from leshper, the same discipline
//   `builtin_actions.cpp` is held to, so the searcher reaches the loop by
//   exactly the route a Lua-registered searcher would (#93, ADR-0008).
//
// WHY THE RANGES DO NOT CROSS THE ABI. `lesh_propose` carries bytes and a
// kind; `lesh_emit_span` carries positions in the SNAPSHOT's buffer. A match
// range is an offset into a HISTORY ENTRY, which is in neither space, so
// there is no honest way to emit one on this token today - and there does not
// need to be, because F-32 highlights matches in the search UI's own rendered
// list, whose coordinates are that surface's and belong to #118. The ranges
// are therefore the pure type's product, handed to the sink while the match
// is on the table. Growth is additive-only (ADR-0008): if a range ever does
// need to cross, it is a new emit function, not a reshaping of this one.

#include "leshper/abi.h"
#include "syntax/token.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::ui {

// Where the entries come from: the one thing the searcher needs of a history.
//
// Deliberately the narrowest possible restatement of #113's
// `history_store::for_each_newest_first`, with ONE difference: the callback
// answers whether to keep going. The searcher must be able to stop - on the
// match cap, on a sink that has seen enough, and above all on the supersede
// poll - and a walk that cannot be stopped would make the poll a formality.
// The store's own callback returns void, so an adapter over it drops the rest
// of the walk on the floor rather than truly stopping; that costs a loop over
// spans already in memory (the v1 store reads the whole file before it calls
// anybody) and no further I/O. The adapter belongs at the wiring site, which
// is where lesh-side and leshper-side types are allowed to meet.
class history_source {
public:
	history_source() = default;
	virtual ~history_source() = default;

	history_source(const history_source&) = delete;
	history_source& operator=(const history_source&) = delete;

	// Calls `fn(entry)` once per stored entry, NEWEST FIRST, until `fn` answers
	// false or the entries run out. `entry` is the original text, newlines and
	// all (F-34), borrowed for the duration of the call.
	virtual void for_each_newest_first(
		const std::function<bool(std::string_view)>& fn) const = 0;
};

// A history that is a vector, which is what a test has and what F-17 wants.
//
// Two clients, and neither is a convenience. The tests feed entries and never
// touch `~/.lesh_history` - a unit test that reads the developer's own history
// is a test that passes differently on every machine. And `vared` (F-17) is an
// ordinary client of the provider bundle (#94), passing a NULL history: that is
// this type, default-constructed, rather than a null pointer every call site
// then has to remember to check.
//
// `entries` is in APPEND order - oldest first, as the file is - and the walk
// runs it back to front, because "newest first" falls out of "append-only"
// exactly as it does in the store.
class vector_history_source final : public history_source {
public:
	vector_history_source() = default;
	explicit vector_history_source(std::vector<std::string> entries) noexcept
		: _entries(std::move(entries)) {}

	void for_each_newest_first(
		const std::function<bool(std::string_view)>& fn) const override;

	void append(std::string entry) { _entries.push_back(std::move(entry)); }
	[[nodiscard]] std::size_t size() const noexcept { return _entries.size(); }

private:
	std::vector<std::string> _entries;
};

// The searcher.
//
// One object, reused across searches: the scratch it needs - the query's
// tokens, the entry's tokens, the ranges of the match in hand - are members,
// so an incremental search that reruns on every keystroke settles into zero
// further heap allocation instead of allocating three vectors per character.
// It is NOT thread-safe and is not meant to be: one search, one searcher, on
// the stack of whatever is running it (see `history_search_compute`).
//
// ADR-0007: every member is a self-freeing standard container. Nothing here
// needs an explicit teardown for the leak gate to expect zero.
class history_search {
public:
	// F-32's three, which is fish's `reader_history_search_t` set.
	enum class mode : std::uint8_t {
		// Substring anywhere in the entry. Every occurrence is a range.
		line,
		// The entry begins with the query. This is also F-33: up-arrow with
		// `git c` typed cycles only entries that start `git c`, and the mode is
		// the whole of that constraint - navigation is the caller's loop over
		// the matches this yields.
		prefix,
		// The query's tokens appear as a contiguous run of WHOLE tokens of the
		// entry, lexed by C-6's lexer. `git` matches `git status` and does not
		// match `github-cli`; `foo` does not match `'foo bar'`, because that is
		// one token and not two. See `matches_token` in the .cpp for what a
		// token is compared AS, which is the one real decision in this file.
		token,
	};

	// Half-open byte offsets into the ENTRY the match was found in - never into
	// the editor buffer, and never into the query. Half-open and named
	// start/end to match `decoration_span`, which is where a consumer that
	// renders them will end up putting them.
	struct range {
		std::size_t start = 0;
		std::size_t end = 0;

		friend bool operator==(const range&, const range&) noexcept = default;
	};

	// One match, BORROWED for the duration of the sink call.
	//
	// `entry` points into whatever the source lent us and `ranges` into the
	// searcher's own scratch; both are invalid the moment the sink returns.
	// The same rule the store's callback already imposes, kept rather than
	// softened: a copy per match would allocate once per entry on a path that
	// runs on every keystroke of an incremental search, and a consumer that
	// keeps results knows better than we do what it wants to keep them in.
	// Callers copy what they intend to keep.
	struct match {
		std::string_view entry;
		std::span<const range> ranges;
	};

	struct options {
		mode search = mode::line;
		// Stop after this many matches. Zero means no limit. A search UI shows
		// a screenful; a walk that keeps going past it is work nobody asked
		// for, and on a long history it is the difference between a keystroke
		// that feels instant and one that does not (N-1).
		std::size_t max_matches = 0;
		// Ranges recorded per match. A match with more occurrences than this is
		// still a match - the extra occurrences are simply not highlighted -
		// because a pathological entry must cost a bounded amount of memory,
		// and an unhighlighted third occurrence is a strictly better failure
		// than dropping the entry from the results.
		std::size_t max_ranges = 64;
	};

	// What the walk did. Returned rather than reported through the sink,
	// because `cancelled` is the caller's cue to answer LESH_ERR_SUPERSEDED
	// and the sink is not called at all on the entry that noticed.
	struct outcome {
		std::size_t entries_examined = 0;
		std::size_t matches = 0;
		// The sink said stop, or `max_matches` was reached.
		bool stopped = false;
		// The cancel poll said give up. Mutually exclusive with `stopped`:
		// the poll runs before an entry is examined, so nothing was mid-flight.
		bool cancelled = false;
	};

	// Answers false to end the walk.
	using match_sink = std::function<bool(const match&)>;
	// Answers true when the work has been superseded and should be abandoned.
	// May be empty, which means "never cancelled".
	using cancel_poll = std::function<bool()>;

	// HOW MANY ENTRIES THE WALK EXAMINES BETWEEN CANCELLATION POLLS (#206).
	//
	// An entry is FOUR NANOSECONDS of work (measured: a 5000-entry prefix walk is
	// 20.1 us in release), and since #202 a cancellation poll is also a YIELD -
	// the reactor hands the thread back to the host, which asks the terminal what
	// arrived before it resumes the walk. That round trip is ~155 ns even with
	// the host's turn made as cheap as it can be, so a poll per entry was 5000
	// yields costing 760 us to protect 20 us of work: 25x, and every bit of it
	// the poll rather than the walk.
	//
	// So the walk strides its poll to the size of its unit, which is what the
	// highlighter's segment sweep already does over TOKENS with the same number
	// (`kPollEvery` in reactors.cpp). The arithmetic that picks 256, release,
	// this machine:
	//
	//   walk       20.1 us + (5000/256) x 0.155 us = 23.1 us, against 31.2 us for
	//              the same walk polling every entry and never yielding - so the
	//              interruptible walk is now FASTER than the uninterruptible one,
	//              because the polls it no longer makes cost more than the yields
	//   latency    256 x 4 ns = ~1 us before a keystroke is looked at, against a
	//              100 us budget
	//   waste      the same ~1 us of work done after a supersede nobody noticed
	//
	// A poll site whose unit is a SYSCALL must not stride: `classify_command`
	// polls immediately before each command NAME, whose `$PATH` walk is a stat
	// per directory (ADR-0009), and that is why this constant lives here, on the
	// walk that knows its unit is nanoseconds, rather than on the yield itself.
	// #212 measured that walk - 27-78 us over a 35-entry `$PATH`, worst keystroke
	// wait 114 us against a 50 ms watchdog - and left it un-yielded inside.
	//
	// A power of two so the test is a mask.
	static constexpr std::size_t poll_every = 256;
	static_assert((poll_every & (poll_every - 1)) == 0, "the stride test is a mask");

	history_search() = default;
	explicit history_search(options opts) noexcept : _options(opts) {}

	// Walks `source` newest first, calling `on_match` for each entry that
	// matches `query` under the current mode.
	//
	// The cancel poll runs before the first entry and every `poll_every` entries
	// after it - #94's "superseded poll between entries" on the stride #206
	// measured - so a stale search dies a microsecond into the history rather
	// than after the whole of it.
	//
	// An EMPTY query matches every entry, with no ranges. That is not a
	// degenerate case to guard against, it is plain history navigation: F-33
	// with nothing typed yet is up-arrow walking the whole history, and every
	// mode agreeing on it is what makes the search UI's transition from "no
	// query" to "one character typed" continuous.
	outcome run(std::string_view query, const history_source& source,
	            const match_sink& on_match, const cancel_poll& cancelled = {});

	// Does `entry` match `query`, and where? The single-entry half of `run`,
	// exposed because it is the whole of the mode semantics and a test that
	// asserts on it does not need a source at all.
	//
	// The ranges are valid until the next call on this searcher.
	[[nodiscard]] bool matches(std::string_view query, std::string_view entry);

	[[nodiscard]] std::span<const range> last_ranges() const noexcept { return _ranges; }

private:
	// One entry, with `_query_tokens` ALREADY lexed for the current query. The
	// split exists so that `run` lexes the query once for a whole walk rather
	// than once per entry, without a cache anybody has to invalidate.
	bool match_entry(std::string_view query, std::string_view entry);

	bool match_line(std::string_view query, std::string_view entry);
	bool match_prefix(std::string_view query, std::string_view entry);
	bool match_token(std::string_view query, std::string_view entry);

	// Lexes `text` into `out`, one entry per token, stopping at end of input.
	//
	// `lex_mode::command` throughout, and that is a decision: this is a LEX and
	// not a parse (C-6), so there is no parser feeding context back, and
	// command mode is the one that treats operators as operators and words as
	// words - which is what "whole token" has to mean for a search. The cost is
	// that a here-document body inside a recalled multi-line entry lexes as
	// ordinary tokens rather than as a body. For a search that is the right
	// trade: the user is looking for text they typed, and the text they typed
	// is what command mode shows them.
	static void lex_into(std::string_view text, std::vector<syntax::token>& out);

	options _options{};
	// Token mode only. Offsets into the query and the entry respectively; both
	// are reused across calls, which is the whole reason this is a type and not
	// a free function.
	std::vector<syntax::token> _query_tokens;
	std::vector<syntax::token> _entry_tokens;
	std::vector<range> _ranges;
};

// ---------------------------------------------------------------------------
// The provider face (#94: providers ride the request-token machinery)
// ---------------------------------------------------------------------------

// The registration-time context `history_search_compute` reads.
//
// IMMUTABLE for the duration of a request, and deliberately holding no scratch:
// the wiring site fills this in once and a compute only reads it, so the searcher
// itself is built on the computing stack per request. That costs the searcher's
// first allocations once per request - at recall frequency, which is what
// §6.2's hot-path rule permits an override point - and buys that two requests
// in flight cannot share a scratch buffer.
struct history_search_provider {
	// Never owned. Null is LESH_ERR_INVAL rather than an empty history: a
	// provider wired up wrong should say so, and F-17's null history is a
	// `vector_history_source` with nothing in it.
	const history_source* source = nullptr;
	history_search::options options{};

	// The kind each match is proposed under. A PARAMETER rather than a fixed
	// value because the same computation serves two consumers: the search UI
	// (#118) wants history matches as themselves, and the autosuggester (F-24)
	// draws the newest match as greyed-out virtual text and asks for it under
	// its own kind.
	std::uint32_t proposal_kind = LESH_PROPOSAL_HISTORY_MATCH;
};

// A `lesh_reactor_fn`. `userdata` is a `history_search_provider*`.
//
// THE QUERY IS THE SNAPSHOT'S TYPED PREFIX - `buffer[0, cursor)` - and taking
// it from the token rather than from the userdata is the point. A query passed
// beside the token would not be generation-bound, and N-4's guarantee is only
// worth anything if everything the result depends on came from the snapshot the
// result is tagged with. For F-33 this is exact: the typed prefix IS the
// constraint. For F-32's incremental search the query is whatever the search
// sub-mode has in its buffer, which #117 makes a keymap push over the same
// editor state; if #118 needs a different rule it changes there, on the loop
// side, and this function does not move.
//
// Emits one `lesh_propose` per match as it is found - streaming (F-31), so the
// loop can show the first screenful while the walk is still running and a stale
// stream dies mid-flight - and polls `lesh_request_superseded` between entries.
// Answers LESH_ERR_SUPERSEDED when the poll noticed, which is a courtesy and not
// a correctness mechanism: the loop drops a stale batch anyway.
std::int32_t history_search_compute(lesh_request* request, void* userdata);

} // namespace lesh::ui
