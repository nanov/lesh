#include "ui/reactor_call.h"

#include "substrate/assert.h"

#include <functional>
#include <thread>
#include <utility>

namespace lesh::ui {
namespace {

// The third copy of registry.cpp's thread key, and the comment workers.cpp
// carries applies verbatim: the two must agree or every accessor on the token
// would refuse, so `run_reactor_here` asserts `token_is_live` on a token it has
// just built rather than trusting that they still do.
std::uint64_t this_thread_key() noexcept {
	const std::uint64_t key =
		static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	return key == 0 ? 1 : key;
}

} // namespace

void run_reactor_here(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
                      request_snapshot& snapshot, const std::atomic<bool>& superseded,
                      leshper::reactor_batch& into) {
	LESH_ASSERT(fn != nullptr);

	into.reactor.assign(reactor);
	into.computed_against = snapshot.computed_against;
	into.event_kind = snapshot.event_kind;
	into.spans.clear();
	into.texts.clear();
	into.proposals.clear();

	leshper::request_token token;
	// Moved, not copied: the snapshot has no second reader. It goes BACK at the
	// end of this function, so a caller serving out of a long-lived member keeps
	// the storage.
	token.buffer = std::move(snapshot.buffer);
	token.cursor = snapshot.cursor;
	token.selection_start = snapshot.selection_start;
	token.selection_end = snapshot.selection_end;
	token.selection_active = snapshot.selection_active;
	token.computed_against = snapshot.computed_against;
	token.event_kind = snapshot.event_kind;
	// #151: THE FIELD THAT WAS NOT COPIED. Every other member of the snapshot
	// was transcribed here and this one was not, so a highlight ran with a null
	// adapter and `exit`, `bind`, every alias and every function resolved
	// `unknown` while `cd` passed only because macOS ships `/usr/bin/cd`. The
	// stamp itself is the caller's - it knows whose shell this is; what belongs
	// here is that the copy is now COMPLETE.
	token.host = snapshot.host;
	token.superseded = &superseded;
	token.spans = &into.spans;
	token.texts = &into.texts;
	token.proposals = &into.proposals;
	token.owner_thread = this_thread_key();
	// Any non-zero value satisfies the ABI's only requirement of a live token.
	// A counter local to this thread is enough and reaches into nothing that is
	// loop-thread-only by ADR-0008.
	static thread_local std::uint64_t calls = 0;
	token.call_token = ++calls;

	LESH_ASSERT(leshper::token_is_live(&token));

	into.status = fn(&token, userdata);

	token.call_token = 0;
	token.owner_thread = 0;
	// AND THE STORAGE GOES BACK, exactly as `worker_pool::compute` hands it back
	// to its scratch task. Without this the token's destructor would free a
	// buffer the caller is about to want again.
	snapshot.buffer = std::move(token.buffer);
}

} // namespace lesh::ui
