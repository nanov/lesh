#pragma once

// What a reactor OFFERS the buffer, and the applied offers an accepting action
// reads back (A-7, F-25; #133, #141, #144).
//
// THE OTHER HALF OF `decoration.h`. One applied batch has two halves: the spans
// and virtual text that get PAINTED, which `decorations` holds, and the
// proposals, which are what an action would ACCEPT and which nothing paints. The
// autosuggester emits both for one suggestion - the continuation as virtual text
// and the whole candidate as a proposal - so the two halves are written by one
// application and dropped by one dismissal.
//
// `proposal` lived in `registry.h` until this ticket, on the argument that "no
// state field carries one". One does now, and the reason is the drift #144 was
// opened for: the applied proposals lived beside the harness, `state::marks`
// lived in the state, and the real loop filled only the second - so the
// suggestion was on screen and `lesh_proposal_read` found nothing. Two halves of
// one batch with two owners is exactly how that happens twice. They have one
// owner now, the state, and one applier (`apply_batch` in registry.h).
//
// WHY THE STATE and not the loop. Every dispatch path already carries a
// `state&` - the keystroke path through `editor.cpp`'s `step`, the loop's own
// timer dispatch, the interrupt binding - and `lesh_editor` already points at
// one. A view owned anywhere else needs a pointer threaded to every one of those
// call sites, and a pointer somebody forgot to thread is the defect this file
// exists to remove. Held out of `state::operator==` for the same reason `marks`
// is; the argument is written there.

#include "leshper/decoration.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::leshper {

// One offer: a kind, and the bytes the buffer would become if it were accepted.
//
// WHOLE, not a tail. The autosuggester proposes `git status` and not ` status`,
// because the accepting action replaces a span rather than appending to what is
// there - which is what lets one accept action serve a completion, a history
// match and a suggestion without knowing which it has.
//
// `kind` defaults to 0 spelled as 0, the way `decoration.h` spells
// LESH_STYLE_NONE: this header does not include abi.h either, and registry.h
// static_asserts that 0 is still LESH_PROPOSAL_AUTOSUGGESTION.
struct proposal {
	std::uint32_t kind = 0;
	std::string bytes;

	friend bool operator==(const proposal&, const proposal&) noexcept = default;
};

// Everything the reactors have offered, applied - what `lesh_proposal_read`
// walks (#133).
//
// NAMESPACED BY REACTOR, exactly as `decorations` is, and for the same reason:
// ADR-0008 makes the emitting reactor the namespace, so a new batch from the
// autosuggester replaces the autosuggester's offer and leaves the completer's
// alone. The name travels with the batch, so no second mechanism is needed.
//
// EMISSION ORDER is layer order, and layer order is order of FIRST application -
// a replacement keeps its place. That is what makes `index` 0, 1, 2 stable for
// the pager (#138) while a reactor keeps recomputing: the list does not shuffle
// because one of its sources answered again.
//
// AN EMPTY BATCH IS STILL AN APPLICATION. A reactor that decides it has nothing
// to offer emits no proposals, and its layer must become empty rather than keep
// the last thing it said - otherwise a suggestion outlives the buffer it was
// about, which is the same bug the generation drop rule exists to prevent.
class applied_proposals {
public:
	// One reactor's last word.
	struct layer {
		std::string reactor;
		std::vector<proposal> items;

		friend bool operator==(const layer&, const layer&) noexcept = default;
	};

	// Replaces what `reactor` offered, or adds it if this is its first batch.
	//
	// SWAPPED IN, not copied or moved out of, for #126's reason and in
	// `decorations::apply`'s words: the batch is pooled storage, and moving out
	// of it would hand the pool back a vector with no capacity, defeating the
	// pooling on the very path it was built for. The layer's old items go back
	// out in their place.
	void apply(std::string_view reactor, std::vector<proposal>& items);

	// #133's dismissal, BOTH HALVES. Every layer carrying a proposal of `kind`
	// goes, and each one takes its decoration layer with it - the drawn half of a
	// suggestion is its virtual text, and a dismissal that left that on screen
	// would have dismissed nothing the user can see.
	//
	// One call taking both stores rather than two calls a caller must remember to
	// pair: the two halves were written by one application and there is no state
	// of the world in which dropping one of them is right.
	//
	// Answers whether anything went, which is what tells the loop to repaint.
	bool dismiss(std::uint32_t kind, decorations& marks);

	// Every layer goes. The loop's line boundary, beside `decorations::clear`: an
	// accepted or cancelled line takes its offers with it, because a proposal is
	// about a buffer that is going away.
	void clear() noexcept;

	// Nothing is on offer. A layer with no proposals in it does not count -
	// a reactor that answered with nothing is showing nothing.
	[[nodiscard]] bool empty() const noexcept;

	// The record: what each reactor last offered, in application order.
	[[nodiscard]] const std::vector<layer>& layers() const noexcept { return _layers; }

	// The `index`-th proposal of `kind` in emission order across the layers, or
	// null. THE ONE WALK: `lesh_proposal_read` is this plus a copy-out, so the
	// ordering rule the ABI documents has one implementation.
	[[nodiscard]] const proposal* find(std::uint32_t kind, std::size_t index) const noexcept;

	friend bool operator==(const applied_proposals&, const applied_proposals&) noexcept
		= default;

private:
	std::vector<layer> _layers;
};

} // namespace lesh::leshper
