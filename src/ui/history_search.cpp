#include "ui/history_search.h"

#include "syntax/lexer.h"

#include <string>
#include <utility>

namespace lesh::ui {

namespace {

// The bytes a token spans, borrowed from the text it was lexed out of.
//
// The lexer's own `text()` wants the lexer instance; this wants only the
// source, because the tokens outlive the lexer that produced them here - they
// are collected into a vector and compared afterwards.
[[nodiscard]] std::string_view text_of(std::string_view source,
                                       const syntax::token& one) noexcept {
	return source.substr(one.offset, one.length);
}

} // namespace

void vector_history_source::for_each_newest_first(
	const std::function<bool(std::string_view)>& fn) const {
	if (!fn)
		return;
	for (auto it = _entries.rbegin(); it != _entries.rend(); ++it)
		if (!fn(*it))
			return;
}

void history_search::lex_into(std::string_view text, std::vector<syntax::token>& out) {
	out.clear();
	syntax::lexer lex{text};

	// The lexer never fails - a malformed construct is a token that says so
	// (#9) - so an unterminated quote in a half-typed query is not an error
	// path here, it is a token that will not compare equal to anything. The
	// only thing worth defending against is a token that consumed nothing,
	// which would spin this loop forever; `previous` is that guard and nothing
	// more. It has never fired, and if it ever does the bug is in the lexer.
	std::uint32_t previous = 0;
	for (;;) {
		const syntax::token one = lex.next(syntax::lex_mode::command);
		if (one.kind == syntax::token_kind::end)
			break;
		out.push_back(one);
		const std::uint32_t now = lex.position();
		if (now <= previous)
			break;
		previous = now;
	}
}

bool history_search::match_line(std::string_view query, std::string_view entry) {
	_ranges.clear();

	// EVERY occurrence, not just the first. The entry `git log | git diff`
	// matches `git` because of both of them, and highlighting only the first
	// would show the user a match that does not explain why the entry is in
	// the list. Non-overlapping and leftmost: after a hit the scan resumes past
	// it, so `aa` in `aaaa` is two matches and not three.
	bool found = false;
	std::size_t from = 0;
	for (;;) {
		const std::size_t at = entry.find(query, from);
		if (at == std::string_view::npos)
			break;
		found = true;
		if (_ranges.size() >= _options.max_ranges)
			break;
		_ranges.push_back(range{at, at + query.size()});
		from = at + query.size();
	}
	return found;
}

bool history_search::match_prefix(std::string_view query, std::string_view entry) {
	_ranges.clear();
	if (!entry.starts_with(query))
		return false;
	if (_options.max_ranges != 0)
		_ranges.push_back(range{0, query.size()});
	return true;
}

bool history_search::match_token(std::string_view query, std::string_view entry) {
	_ranges.clear();
	lex_into(entry, _entry_tokens);

	const std::size_t need = _query_tokens.size();
	if (need == 0 || _entry_tokens.size() < need)
		return false;

	// WHAT A TOKEN IS COMPARED AS, which is the one decision in this file that
	// could have gone another way: its SOURCE BYTES, exactly as written, with
	// no quote removal. `foo` therefore does not match `'foo'`, and `"a b"` in
	// the query matches `"a b"` in the entry and not `a b`.
	//
	// Three reasons, in the order they are load-bearing.
	//
	// The searcher has a LEXER, not an expander. Quote removal is the
	// expander's, it needs an arena to write the removed-quote bytes into
	// (#75), and half of what it would produce is not knowable at search time
	// anyway: the `$HOME` in an entry had a value when the command ran, not
	// now. A comparison that unquoted but could not expand would be
	// arbitrary - equal for one kind of quoting and unequal for another.
	//
	// The RANGES have to be honest. A match established after quote removal
	// covers bytes that are not in the entry, so there would be nothing to
	// highlight; F-32 highlights what the user is looking at, and the user is
	// looking at the raw text.
	//
	// And it is what the lexer is actually FOR here. Its contribution is not a
	// notion of equality, it is where tokens BEGIN AND END: `'foo bar'` is one
	// token so `foo` does not match inside it, `foo|bar` is three so `foo`
	// does, and `github-cli` is one so `git` does not. That is exactly the
	// difference between token mode and line mode, and it is entirely a
	// question of boundaries.
	bool found = false;
	std::size_t at = 0;
	while (at + need <= _entry_tokens.size()) {
		bool same = true;
		for (std::size_t k = 0; k < need; ++k) {
			if (text_of(entry, _entry_tokens[at + k]) != text_of(query, _query_tokens[k])) {
				same = false;
				break;
			}
		}
		if (!same) {
			++at;
			continue;
		}

		found = true;
		if (_ranges.size() >= _options.max_ranges)
			break;
		// One range per RUN, spanning from the first matched token's start to
		// the last one's end - blanks between them included. A multi-token
		// query is a phrase the user typed, and a highlight broken into pieces
		// at every space reads as several matches rather than the one match it
		// is.
		_ranges.push_back(range{_entry_tokens[at].offset,
		                        _entry_tokens[at + need - 1].end_offset()});
		at += need;
	}
	return found;
}

bool history_search::match_entry(std::string_view query, std::string_view entry) {
	// An empty query matches everything, with nothing to highlight - see the
	// header. Checked once, before the modes, so all three agree by
	// construction rather than by three matching guards.
	if (query.empty()) {
		_ranges.clear();
		return true;
	}

	switch (_options.search) {
		case mode::line:
			return match_line(query, entry);
		case mode::prefix:
			return match_prefix(query, entry);
		case mode::token:
			break;
	}

	// A query that is non-empty but lexes to no tokens at all - blanks, or a
	// line continuation and nothing else - is the empty query in token mode's
	// own terms, and answers the same way. Typing a space into an empty search
	// box must not empty the result list.
	if (_query_tokens.empty()) {
		_ranges.clear();
		return true;
	}
	return match_token(query, entry);
}

bool history_search::matches(std::string_view query, std::string_view entry) {
	if (_options.search == mode::token)
		lex_into(query, _query_tokens);
	return match_entry(query, entry);
}

history_search::outcome history_search::run(std::string_view query,
                                            const history_source& source,
                                            const match_sink& on_match,
                                            const cancel_poll& cancelled) {
	outcome result;

	// Once for the whole walk. The query does not change under us - a keystroke
	// makes a NEW request against a new generation rather than mutating this
	// one (N-4) - so lexing it per entry would be the same work a thousand
	// times.
	if (_options.search == mode::token)
		lex_into(query, _query_tokens);

	source.for_each_newest_first([&](std::string_view entry) -> bool {
		// #94's supersede poll, between entries. Before the entry rather than
		// after it, so `cancelled` means nothing was examined half-way and the
		// sink was not called for work that is about to be thrown away.
		if (cancelled && cancelled()) {
			result.cancelled = true;
			return false;
		}

		++result.entries_examined;
		if (!match_entry(query, entry))
			return true;
		++result.matches;

		if (on_match) {
			const match one{entry, std::span<const range>{_ranges}};
			if (!on_match(one)) {
				result.stopped = true;
				return false;
			}
		}
		if (_options.max_matches != 0 && result.matches >= _options.max_matches) {
			result.stopped = true;
			return false;
		}
		return true;
	});

	return result;
}

std::int32_t history_search_compute(lesh_request* request, void* userdata) {
	auto* const provider = static_cast<history_search_provider*>(userdata);
	if (request == nullptr || provider == nullptr || provider->source == nullptr)
		return LESH_ERR_INVAL;

	// The snapshot, copied out. No accessor lends a pointer into the token
	// (ADR-0006's WASM insurance), so the query has to live somewhere and this
	// frame is where.
	std::size_t length = 0;
	if (lesh_request_buffer_length(request, &length) != LESH_OK)
		return LESH_ERR_INVAL;

	std::string snapshot(length, '\0');
	std::size_t copied = 0;
	if (length != 0
	    && lesh_request_buffer(request, snapshot.data(), snapshot.size(), &copied) != LESH_OK)
		return LESH_ERR_INVAL;
	snapshot.resize(copied);

	std::size_t cursor = 0;
	if (lesh_request_cursor(request, &cursor) != LESH_OK)
		return LESH_ERR_INVAL;
	if (cursor > snapshot.size())
		cursor = snapshot.size();

	const std::string_view query{snapshot.data(), cursor};

	// On the worker's stack: see `history_search_provider`. Two requests in
	// flight against the same provider must not share scratch.
	history_search searcher{provider->options};

	std::int32_t emit_status = LESH_OK;
	const history_search::outcome result = searcher.run(
		query, *provider->source,
		[&](const history_search::match& one) {
			// Streaming (F-31): one emit per match, as it is found, rather than
			// a vector collected and handed over at the end. The loop applies
			// batches as they arrive and a superseded stream dies mid-flight.
			const std::int32_t status =
				lesh_propose(request, provider->proposal_kind,
				             one.entry.empty() ? nullptr : one.entry.data(),
				             one.entry.size());
			if (status != LESH_OK) {
				// A refused emit is not something to keep walking past: the
				// only ways it fails are a malformed proposal kind and a token
				// with nowhere to emit to, and both are true of every
				// subsequent match too.
				emit_status = status;
				return false;
			}
			return true;
		},
		[request]() {
			std::int32_t superseded = 0;
			lesh_request_superseded(request, &superseded);
			return superseded != 0;
		});

	if (result.cancelled)
		return LESH_ERR_SUPERSEDED;
	return emit_status;
}

} // namespace lesh::ui
