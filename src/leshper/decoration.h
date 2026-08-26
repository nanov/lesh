#pragma once

// What the reactors say about the buffer, and the form the renderer reads it in
// (A-7, F-20, F-21, F-24; #93, #133, #141).
//
// These three types were #93's emit vocabulary and lived in `registry.h`, which
// is where the ABI's side of them still is: `lesh_emit_span` and
// `lesh_emit_virtual_text_styled` fill vectors of them at the call site (#90 -
// nothing points into a worker's arena). They are here now because `state` HOLDS
// them: `state::decorations` was a named placeholder with no fields and this is
// the type that fills it, so the vocabulary has to sit under `state.h` rather
// than over it. registry.h includes this file by including state.h.
//
// A HEADER OF ITS OWN rather than more of `state.h`, for two reasons that are
// the same reason: `rebuild` below is a forty-line sweep that wants `<algorithm>`
// and a translation unit, and `state.h` is included by nearly every file in
// leshper. The state keeps the field; the vocabulary and the normalization keep
// their own file.
//
// NOTHING HERE KNOWS WHAT A COLOUR IS. A span carries an interned semantic id -
// what it MEANS - and `theme.h` is the seam that says what an id LOOKS like.
// That split is #124's, and it is why a plugin can emit `command.builtin`
// without agreeing with anybody about green.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::leshper {

// One highlight span: a half-open byte range over the buffer, and the interned
// style id (#93, #124) that says what the range means.
//
// EXCLUSIVE at the end, which is `region`'s convention in state.h and the
// parser's in syntax/: a selection can be handed to either without a fence-post
// translation. `style_id` 0 is abi.h's LESH_STYLE_NONE - "no style, the renderer
// decides" - and this header does not include abi.h to say so; registry.h
// static_asserts that the two spellings agree.
struct decoration_span {
	std::size_t start = 0;
	std::size_t end = 0;
	std::uint32_t style_id = 0;

	friend bool operator==(const decoration_span&, const decoration_span&) noexcept = default;
};

// Text that is DRAWN at a buffer offset and is not IN the buffer (#133, F-24).
//
// The autosuggester's whole visible half: it proposes the rest of a history
// line and emits it here at the end of the typed text, so the user sees the
// completion without a single byte of it being in what `accept_line` would run.
// The cursor never lands in one - layout.cpp places the cursor at an offset
// BEFORE painting what is virtual there, which is the one rule that keeps
// "the suggestion is never in the buffer" true on screen as well as in the model.
struct virtual_text {
	std::size_t at = 0;
	std::string bytes;
	// The interned semantic id `lesh_emit_virtual_text_styled` carried, or 0
	// from the unstyled emit (#133). Additive, and defaulted, so the two emit
	// functions differ in exactly this field.
	std::uint32_t style_id = 0;

	friend bool operator==(const virtual_text&, const virtual_text&) noexcept = default;
};

// Everything the reactors have said, applied (A-7, spec §6.1).
//
// NAMESPACED BY REACTOR, because ADR-0008 makes the emitting reactor the
// decoration namespace: a batch from `highlighter` replaces the highlighter's
// spans and touches the autosuggester's not at all. The name travels with the
// batch, so no second namespacing mechanism is needed and none is here.
//
// TWO REPRESENTATIONS, and the second is why this is a class and not a struct of
// vectors. `layers()` is the RECORD - what each reactor last said, verbatim -
// and it is what a namespace replacement acts on. `spans()` and `texts()` are
// the same information NORMALIZED for the renderer: one list, sorted, and in the
// spans' case non-overlapping, so that `lay_out` answers "what pen is this byte"
// with a cursor that only ever moves forward.
//
// THE NORMALIZATION HAPPENS HERE, at application time, and that is the decision.
// A highlighter's spans NEST by construction - builtin_reactors.cpp's
// `paint_segments` emits a double-quoted segment and then recurses into it for
// the `$x` inside - so somebody has to decide that the inner span wins. The two
// places to decide it are once per applied batch and once per cluster per
// repaint, and the second one would put a scan over every span inside the render
// walk, where `allocation_tests.cpp` pins the per-repaint cost to a constant and
// layout.h promises a walk that carries nothing.
//
// LATER WINS, in both directions: a later layer over an earlier one, and within
// a layer a later span over an earlier one. That is the order
// `lesh_proposal_read` already reads applied batches in, and it is what makes
// nesting come out right without an emitter having to sort anything.
class decorations {
public:
	// One reactor's last word.
	struct layer {
		std::string reactor;
		std::vector<decoration_span> spans;
		std::vector<virtual_text> texts;

		friend bool operator==(const layer&, const layer&) noexcept = default;
	};

	// Replaces what `reactor` said, or adds it if this is its first batch.
	//
	// The vectors are SWAPPED IN, not copied, and the layer's old ones go back
	// out in their place. The batch a caller hands us is pooled storage (#126's
	// message pool, or the shell channel's recycler) and moving out of it would
	// hand the pool back an empty vector with no capacity, defeating the pooling
	// on the very path it was built for. Answers nothing: an application always
	// happens, the drop rule having been decided before we were called.
	void apply(std::string_view reactor, std::vector<decoration_span>& spans,
	           std::vector<virtual_text>& texts);

	// Drops one reactor's layer, and answers whether there was one. #133's
	// dismissal, from the state's side: the WHOLE layer goes, because the drawn
	// half of a suggestion is its virtual text and a dismissal that left that on
	// screen would have dismissed nothing the user can see.
	bool forget(std::string_view reactor);

	// Every layer goes. The loop's line boundary: an accepted or cancelled line
	// takes its decorations with it, or the next line would open painted in the
	// colours of the one before it.
	void clear() noexcept;

	[[nodiscard]] bool empty() const noexcept { return _painted.empty() && _texts.empty(); }

	[[nodiscard]] const std::vector<layer>& layers() const noexcept { return _layers; }

	// The renderer's two lists. `spans()` is sorted by start and non-overlapping;
	// `texts()` is sorted by offset, stably, so two texts at one offset paint in
	// the order they were emitted.
	[[nodiscard]] const std::vector<decoration_span>& spans() const noexcept {
		return _painted;
	}
	[[nodiscard]] const std::vector<virtual_text>& texts() const noexcept { return _texts; }

	// The record is the value; the two normalized lists are a function of it, so
	// comparing them as well would be comparing the same thing twice.
	friend bool operator==(const decorations& a, const decorations& b) noexcept {
		return a._layers == b._layers;
	}

private:
	// One end of one span, in `rebuild`'s sweep.
	//
	// IN THE HEADER because the sweep's buffer is a member now (below) and a
	// member needs a complete type. It was a file-local struct in decoration.cpp
	// until #168 Phase B, which is where the sweep still is - nothing outside
	// `rebuild` constructs one, and `private` is what says so.
	struct edge {
		std::size_t at = 0;
		std::uint32_t which = 0;   // the span's index in emission order
		bool opening = false;
	};

	// THE SWEEP'S WORKING MEMORY, HELD RATHER THAN BUILT (#168 Phase B).
	//
	// `rebuild` runs on every applied batch, which is once per reactor per
	// keystroke: a highlight and a suggestion on every character typed. It used
	// to declare these three as locals, so a warm line cost three mallocs and
	// three frees per keystroke forever - N-2's own rule, broken on the one path
	// N-2 was written for. They are members now, cleared at the top of the sweep
	// and grown only when a line has more spans in it than any line before it, so
	// the steady state reaches the heap zero times
	// (`AllocationTest.ApplyingAWarmHighlightBatchCostsNoHeap`).
	//
	// A COPY STARTS EMPTY, and that is the whole reason this is a type rather
	// than three more fields. `decorations` is a member of `state`, `state` is
	// copyable, and F-38's replay compares copies - so scratch that travelled
	// would hand a copy capacity it never asked for and make an equality
	// question ("are these two states the same?") depend on how much work each
	// one happened to have done. Equality is `_layers` and only `_layers`, which
	// this leaves untouched; the copy is a fresh empty scratch that fills itself
	// the first time the copy is applied to.
	struct rebuild_scratch {
		std::vector<decoration_span> flat;
		std::vector<edge> edges;
		// The set open at the current offset. A small vector scanned for its
		// maximum rather than a heap: the set is the NESTING DEPTH of a command
		// line's highlights, which is two or three.
		std::vector<std::uint32_t> active;

		rebuild_scratch() = default;
		~rebuild_scratch() = default;

		rebuild_scratch(const rebuild_scratch&) noexcept {}
		rebuild_scratch& operator=(const rebuild_scratch&) noexcept { return *this; }

		// Moving DOES carry it: a moved-from state is not going to be applied to
		// again, and taking the buffers with it is the point of a move.
		rebuild_scratch(rebuild_scratch&&) noexcept = default;
		rebuild_scratch& operator=(rebuild_scratch&&) noexcept = default;
	};

	void rebuild();

	std::vector<layer> _layers;
	std::vector<decoration_span> _painted;
	std::vector<virtual_text> _texts;
	rebuild_scratch _scratch;
};

} // namespace lesh::leshper
