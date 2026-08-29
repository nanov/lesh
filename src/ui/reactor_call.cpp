#include "ui/reactor_call.h"

#include "substrate/assert.h"

#include <utility>

namespace lesh::ui {
void take_snapshot(request_snapshot& into, const leshper::state& target,
                   std::uint32_t event_kind) {
	// `assign` AND NOT `=`: the string keeps whatever capacity it arrived with,
	// which is the whole point of the in-place form.
	into.buffer.assign(target.buffer.text());
	into.cursor = target.cursor.byte_offset();
	// The derived region, exactly as `loop_harness::react` takes it (#116 landed
	// the model after this function was written, and a snapshot that reported
	// every selection as inactive would make #129's `selection_changed` fan-out
	// wake reactors with nothing to look at). Reported even when inactive, on
	// the reasoning `lesh_selection_get` gives: the anchor outlives
	// deactivation and the flag is the separate question.
	{
		const std::size_t anchor = target.selection_anchor().byte_offset();
		const std::size_t head = into.cursor;
		into.selection_start = anchor < head ? anchor : head;
		into.selection_end = anchor < head ? head : anchor;
		into.selection_active = target.selection_active();
	}
	into.computed_against = target.gen;
	into.event_kind = event_kind;
	// `host` IS RESET, so that this and `snapshot_of` produce the same value into
	// a reused object as into a fresh one. Without it a reactor's snapshot could
	// keep a host pointer a previous notification had set and hand it to a caller
	// that meant null, which is the one field where "left over from last time" is
	// not obviously wrong at the point of use. A caller that wants one sets it
	// after (see the field's note, and `event_loop::notify_reactors`).
	into.host = nullptr;
}

request_snapshot snapshot_of(const leshper::state& target, std::uint32_t event_kind) {
	request_snapshot taken;
	take_snapshot(taken, target, event_kind);
	return taken;
}

void run_reactor_here(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
                      request_snapshot& snapshot, const std::atomic<bool>& superseded,
                      leshper::reactor_batch& into, reactor_cooperation cooperate) {
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
	// WHO THE CANCELLATION POLL YIELDS TO (#202). Empty for every caller that is
	// not a fiber, which is what makes the poll cost exactly the atomic load it
	// always cost off a fiber.
	token.cooperate = cooperate.yield;
	token.cooperate_userdata = cooperate.userdata;
	token.spans = &into.spans;
	token.texts = &into.texts;
	token.proposals = &into.proposals;
	// Any non-zero value satisfies the ABI's only requirement of a live token,
	// and since #211 §1.3 it is the whole requirement: the thread key this used to
	// stamp beside it was a second copy of registry.cpp's, kept so that the two
	// could agree about a thread that no longer has a rival.
	static std::uint64_t calls = 0;
	token.call_token = ++calls;

	LESH_ASSERT(leshper::token_is_live(&token));

	into.status = fn(&token, userdata);

	token.call_token = 0;
	// AND THE STORAGE GOES BACK. Without this the token's destructor would free a
	// buffer the caller is about to want again - which is the whole reason the
	// snapshot is borrowed rather than consumed, and the reason a warm keystroke
	// reaches the heap not at all.
	snapshot.buffer = std::move(token.buffer);
}

} // namespace lesh::ui
