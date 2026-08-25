#include "leshper/registry.h"

#include "leshper/editor.h"
#include "substrate/assert.h"
#include "substrate/grapheme.h"

#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using lesh::leshper::decoration_span;
using lesh::leshper::loop_outcome;
using lesh::leshper::position;
using lesh::leshper::proposal;
using lesh::leshper::virtual_text;

// A thread's identity as a plain integer, so no header needs <thread> to hold
// one. Never zero, because zero means "no owner" on a dead handle.
std::uint64_t current_thread_key() noexcept {
	const std::uint64_t key =
		static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	return key == 0 ? 1 : key;
}

// ---------------------------------------------------------------------------
// Handle validity (ADR-0008: valid for the receiving call, loop thread only).
// ---------------------------------------------------------------------------

bool editor_ok(const lesh_editor* handle) noexcept {
	return lesh::leshper::handle_is_live(handle);
}

bool request_ok(const lesh_request* token) noexcept {
	return lesh::leshper::token_is_live(token);
}

// Every accessor opens with this. In release it costs nothing; in debug it
// turns "stashed the handle" from a stale read into an abort at the call that
// did it.
#define LESH_EDITOR_HANDLE(handle)                                             \
	do {                                                                       \
		LESH_ASSERT(editor_ok(handle));                                        \
		if (!editor_ok(handle))                                                \
			return LESH_ERR_INVAL;                                             \
	} while (0)

#define LESH_REQUEST_HANDLE(token)                                             \
	do {                                                                       \
		LESH_ASSERT(request_ok(token));                                        \
		if (!request_ok(token))                                                \
			return LESH_ERR_INVAL;                                             \
	} while (0)

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// snake_case: a lowercase letter, then lowercase letters, digits, underscores.
//
// Narrow on purpose. The names are what a user types into a binding and what an
// rc file re-sources idempotently; a name space that admits hyphens as well
// would make `delete-backward-word` and `delete_backward_word` two actions that
// look like one, which is the failure worth designing out rather than
// documenting around.
bool is_snake_case(std::string_view name) noexcept {
	if (name.empty() || name[0] < 'a' || name[0] > 'z')
		return false;
	for (const char c : name) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Text geometry, over the staged bytes.
//
// The editor owns this so no binding has to. #108's segmenter is the authority
// on cluster boundaries; the line and word helpers are byte scans, because
// lines and blank-separated words are byte questions and asking Unicode about
// them would be answering a question nobody posed.
// ---------------------------------------------------------------------------

std::size_t clamp_into(std::string_view text, std::size_t offset) noexcept {
	return offset < text.size() ? offset : text.size();
}

bool on_cluster_boundary(std::string_view text, std::size_t offset) noexcept {
	if (offset == 0 || offset >= text.size())
		return true;
	return lesh::grapheme::next_boundary(text, lesh::grapheme::prev_boundary(text, offset))
	    == offset;
}

// Back to the start of the cluster the offset falls inside. Where a cursor
// lands: the cursor rests ON a cluster, never in the middle of one (F-3).
std::size_t snap_back(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	if (on_cluster_boundary(text, offset))
		return offset;
	return lesh::grapheme::prev_boundary(text, offset);
}

// Forward to the end of the cluster the offset falls inside. Where the far end
// of a write range lands, so that a replacement swallows whole clusters and
// cannot leave half of one behind.
std::size_t snap_forward(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	if (on_cluster_boundary(text, offset))
		return offset;
	return lesh::grapheme::next_boundary(text, lesh::grapheme::prev_boundary(text, offset));
}

constexpr bool is_blank(char byte) noexcept {
	return byte == ' ' || byte == '\t' || byte == '\n';
}

std::size_t line_start_of(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	while (offset > 0 && text[offset - 1] != '\n')
		--offset;
	return offset;
}

std::size_t line_end_of(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	while (offset < text.size() && text[offset] != '\n')
		++offset;
	return offset;
}

// Skip trailing blanks, then take the run of non-blanks. The same rule
// editor.cpp's `backward_word_start` uses, and the same placeholder: C-6 makes
// the lexer independently callable so this becomes token-wise, at which point
// both sites change to a call into the syntax layer.
std::size_t prev_word_of(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	while (offset > 0 && is_blank(text[offset - 1]))
		--offset;
	while (offset > 0 && !is_blank(text[offset - 1]))
		--offset;
	return offset;
}

std::size_t next_word_of(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	while (offset < text.size() && is_blank(text[offset]))
		++offset;
	while (offset < text.size() && !is_blank(text[offset]))
		++offset;
	return offset;
}

// ---------------------------------------------------------------------------
// Copy-out, the one shape every reading accessor has.
//
// `*length_out` is the full length whether or not it fit, so a caller that
// asked with a zero capacity learns what to allocate, and a caller that guessed
// too small learns by how much. LESH_ERR_TOOSMALL is not a failure to answer -
// it is the answer, with the bytes withheld.
// ---------------------------------------------------------------------------

std::int32_t copy_out(std::string_view source, char* out, std::size_t capacity,
                      std::size_t* length_out) noexcept {
	if (length_out == nullptr)
		return LESH_ERR_INVAL;
	*length_out = source.size();
	if (source.size() > capacity)
		return LESH_ERR_TOOSMALL;
	if (source.empty())
		return LESH_OK;
	if (out == nullptr)
		return LESH_ERR_INVAL;
	for (std::size_t i = 0; i < source.size(); ++i)
		out[i] = source[i];
	return LESH_OK;
}

} // namespace

// ===========================================================================
// The ABI, implemented.
// ===========================================================================

extern "C" {

// --- Registration ----------------------------------------------------------

int32_t lesh_action_register(lesh_registry* registry, const char* name,
                             lesh_action_fn fn, void* userdata) {
	if (registry == nullptr || name == nullptr || fn == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view given{name};
	// A dot name is minted by the registry and by nobody else: it is the
	// original, and an original you can overwrite is not one.
	if (!given.empty() && given[0] == '.')
		return LESH_ERR_REFUSED;
	if (!is_snake_case(given))
		return LESH_ERR_INVAL;

	const lesh_registry::action_entry entry{fn, userdata};
	const auto found = registry->actions.find(given);
	if (found == registry->actions.end()) {
		// First definition: it is also the unshadowable original, forever.
		registry->actions.emplace(std::string{given}, entry);
		registry->actions.emplace("." + std::string{given}, entry);
		return LESH_OK;
	}
	found->second = entry;  // #101: re-sourcing an rc file replaces, idempotently
	return LESH_OK;
}

int32_t lesh_action_exists(lesh_registry* registry, const char* name, int32_t* out) {
	if (registry == nullptr || name == nullptr || out == nullptr)
		return LESH_ERR_INVAL;
	*out = registry->actions.find(std::string_view{name}) != registry->actions.end() ? 1 : 0;
	return LESH_OK;
}

int32_t lesh_action_invoke(lesh_editor* editor, const char* name,
                           const lesh_invocation* invocation) {
	LESH_EDITOR_HANDLE(editor);
	if (name == nullptr || invocation == nullptr || editor->registry == nullptr)
		return LESH_ERR_INVAL;
	// #92's ceiling. A wrapper that delegates to the thing it wrapped is one
	// frame; a wrapper that delegates to itself is the bug this catches.
	constexpr int ceiling = 64;
	if (editor->depth >= ceiling)
		return LESH_ERR_RECURSION;

	const auto found = editor->registry->actions.find(std::string_view{name});
	if (found == editor->registry->actions.end())
		return LESH_ERR_NOTFOUND;

	// The SAME handle, so the callee stages into the caller's staging area: a
	// wrapper delegating to `.accept_line` is one undo entry and one generation
	// bump, not two.
	++editor->depth;
	const std::int32_t status = found->second.fn(editor, invocation, found->second.userdata);
	--editor->depth;
	return status;
}

int32_t lesh_reactor_register(lesh_registry* registry, const char* name,
                              uint32_t event_mask, lesh_reactor_fn fn, void* userdata) {
	if (registry == nullptr || name == nullptr || fn == nullptr)
		return LESH_ERR_INVAL;
	constexpr std::uint32_t known = LESH_EVENT_BUFFER_CHANGED | LESH_EVENT_CURSOR_MOVED
	                              | LESH_EVENT_SELECTION_CHANGED;
	if (event_mask == 0 || (event_mask & ~known) != 0)
		return LESH_ERR_INVAL;
	const std::string_view given{name};
	if (!given.empty() && given[0] == '.')
		return LESH_ERR_REFUSED;
	if (!is_snake_case(given))
		return LESH_ERR_INVAL;

	registry->reactors[std::string{given}] =
		lesh_registry::reactor_entry{fn, userdata, event_mask};
	return LESH_OK;
}

int32_t lesh_reactor_exists(lesh_registry* registry, const char* name, int32_t* out) {
	if (registry == nullptr || name == nullptr || out == nullptr)
		return LESH_ERR_INVAL;
	*out = registry->reactors.find(std::string_view{name}) != registry->reactors.end() ? 1 : 0;
	return LESH_OK;
}

// --- Styles ----------------------------------------------------------------

int32_t lesh_style_intern(lesh_registry* registry, const char* name, uint32_t* out) {
	if (registry == nullptr || name == nullptr || out == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view given{name};
	if (given.empty())
		return LESH_ERR_INVAL;
	for (std::size_t i = 1; i < registry->styles.size(); ++i) {
		if (registry->styles[i] == given) {
			*out = static_cast<std::uint32_t>(i);
			return LESH_OK;
		}
	}
	registry->styles.emplace_back(given);
	*out = static_cast<std::uint32_t>(registry->styles.size() - 1);
	return LESH_OK;
}

int32_t lesh_style_name(lesh_registry* registry, uint32_t style_id, char* out,
                        size_t capacity, size_t* length_out) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	if (style_id == LESH_STYLE_NONE || style_id >= registry->styles.size())
		return LESH_ERR_NOTFOUND;
	return copy_out(registry->styles[style_id], out, capacity, length_out);
}

// --- Editor state ----------------------------------------------------------

int32_t lesh_buffer_length(lesh_editor* editor, size_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = editor->staged.size();
	return LESH_OK;
}

int32_t lesh_buffer_get(lesh_editor* editor, char* out, size_t capacity, size_t* length_out) {
	LESH_EDITOR_HANDLE(editor);
	return copy_out(editor->staged, out, capacity, length_out);
}

int32_t lesh_buffer_read(lesh_editor* editor, size_t from, size_t to, char* out,
                         size_t capacity, size_t* length_out) {
	LESH_EDITOR_HANDLE(editor);
	const std::string_view text{editor->staged};
	const std::size_t begin = clamp_into(text, from);
	const std::size_t end = clamp_into(text, to);
	if (end <= begin)
		return copy_out({}, out, capacity, length_out);
	return copy_out(text.substr(begin, end - begin), out, capacity, length_out);
}

int32_t lesh_buffer_replace(lesh_editor* editor, size_t from, size_t to,
                            const char* bytes, size_t length) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;

	// Clamp, then snap outward. A binding hands us byte offsets it got from
	// somewhere - a regex match, a variable a user typed into - and the ABI's
	// promise is that no such offset can leave a grapheme cluster in halves.
	const std::string_view text{editor->staged};
	std::size_t begin = clamp_into(text, from);
	std::size_t end = clamp_into(text, to);
	if (end < begin)
		std::swap(begin, end);
	begin = snap_back(text, begin);
	end = snap_forward(text, end);

	editor->staged.replace(begin, end - begin, bytes == nullptr ? "" : bytes, length);
	editor->buffer_written = true;
	// The cursor follows the edit to where the replacement ends, which is what
	// every one of the ten built-ins wants and what `apply_edit` already does on
	// the enum path. An action that wants it elsewhere says so afterwards.
	editor->staged_cursor = begin + length;
	editor->cursor_written = true;
	return LESH_OK;
}

int32_t lesh_buffer_set(lesh_editor* editor, const char* bytes, size_t length) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	editor->staged.assign(bytes == nullptr ? "" : bytes, length);
	editor->buffer_written = true;
	// Deliberately NOT moved: `$BUFFER=...` in the lesh binding replaces the
	// text and leaves `$CURSOR` where the user put it. It clamps and snaps at
	// commit like every other position.
	return LESH_OK;
}

int32_t lesh_cursor_get(lesh_editor* editor, size_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = editor->staged_cursor;
	return LESH_OK;
}

int32_t lesh_cursor_set(lesh_editor* editor, size_t offset) {
	LESH_EDITOR_HANDLE(editor);
	editor->staged_cursor = offset;
	editor->cursor_written = true;
	return LESH_OK;
}

// The selection, backed for real (#96, spec §6.3).
//
// Singular, and staying singular until multi-cursor arrives as ADDITIVE plural
// functions - #96 decision 5 and #93's growth rule, in the same breath.
//
// The region the getter reports is the derived one, `[min(anchor, head),
// max(anchor, head))`, over the STAGED anchor and the STAGED cursor: an action
// that has moved the cursor is looking at the selection its own motion made,
// which is the whole of what a helix-mode motion needs to see.
int32_t lesh_selection_get(lesh_editor* editor, size_t* start_out, size_t* end_out,
                           int32_t* active_out) {
	LESH_EDITOR_HANDLE(editor);
	if (start_out == nullptr || end_out == nullptr || active_out == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view text{editor->staged};
	const std::size_t anchor = snap_back(text, editor->staged_anchor);
	const std::size_t head = snap_back(text, editor->staged_cursor);
	// Reported whether or not the region is live, because the anchor outlives
	// deactivation (emacs's mark) and a binding asking where the mark is deserves
	// an answer. The flag is the separate question, and it is the one that says
	// whether the range means anything.
	*start_out = anchor < head ? anchor : head;
	*end_out = anchor < head ? head : anchor;
	*active_out = editor->staged_selection_active ? 1 : 0;
	return LESH_OK;
}

// Sets the region to `[start, end)` and activates it. The head lands on `end`,
// because the head IS the cursor - there is nowhere else for it to go, and a
// setter that left the cursor behind would leave the state describing a
// different region than the one it was just handed.
//
// `start > end` is not an error and is not swapped: the pair is a direction, and
// a backward selection (helix's, vi's `o`-flipped one) is the reason the model
// stores an anchor and a head rather than a sorted pair. The derived range comes
// out sorted either way.
//
// Both endpoints clamp and snap BACK to a cluster start, not outward the way
// lesh_buffer_replace snaps: a selection endpoint is a cursor-like position that
// rests on a cluster, where a replacement's range must swallow whole clusters.
int32_t lesh_selection_set(lesh_editor* editor, size_t start, size_t end) {
	LESH_EDITOR_HANDLE(editor);
	const std::string_view text{editor->staged};
	editor->staged_anchor = snap_back(text, start);
	editor->staged_cursor = snap_back(text, end);
	editor->staged_selection_active = true;
	editor->selection_written = true;
	editor->cursor_written = true;
	return LESH_OK;
}

// Deactivates the region and KEEPS the anchor, which is what state::
// drop_selection does and for the reason it gives: emacs's mark survives
// `deactivate-mark`. A binding that wants the mark moved says where.
int32_t lesh_selection_clear(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	editor->staged_selection_active = false;
	editor->selection_written = true;
	return LESH_OK;
}

int32_t lesh_generation(lesh_editor* editor, uint64_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = editor->target->gen.value();
	return LESH_OK;
}

int32_t lesh_position_move(lesh_editor* editor, size_t from, lesh_motion motion, size_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view text{editor->staged};
	const std::size_t at = clamp_into(text, from);
	switch (motion) {
	case LESH_MOTION_PREV_CLUSTER:
		*out = lesh::grapheme::prev_boundary(text, at);
		return LESH_OK;
	case LESH_MOTION_NEXT_CLUSTER:
		*out = lesh::grapheme::next_boundary(text, at);
		return LESH_OK;
	case LESH_MOTION_LINE_START:
		*out = line_start_of(text, at);
		return LESH_OK;
	case LESH_MOTION_LINE_END:
		*out = line_end_of(text, at);
		return LESH_OK;
	case LESH_MOTION_PREV_WORD:
		*out = prev_word_of(text, at);
		return LESH_OK;
	case LESH_MOTION_NEXT_WORD:
		*out = next_word_of(text, at);
		return LESH_OK;
	case LESH_MOTION_BUFFER_START:
		*out = 0;
		return LESH_OK;
	case LESH_MOTION_BUFFER_END:
		*out = text.size();
		return LESH_OK;
	}
	return LESH_ERR_INVAL;  // an enumerator from a newer header than this build
}

// --- Loop outcomes ---------------------------------------------------------

namespace {

std::int32_t request_outcome(lesh_editor* editor, loop_outcome which, std::int32_t status) {
	editor->outcome = static_cast<std::uint8_t>(which);
	editor->exit_status = status;
	return LESH_OK;
}

} // namespace

int32_t lesh_accept_line(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	return request_outcome(editor, loop_outcome::accept_line, 0);
}

int32_t lesh_cancel_line(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	return request_outcome(editor, loop_outcome::cancel_line, 0);
}

int32_t lesh_exit(lesh_editor* editor, int32_t status) {
	LESH_EDITOR_HANDLE(editor);
	return request_outcome(editor, loop_outcome::exit, status);
}

int32_t lesh_recursive_edit(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	return request_outcome(editor, loop_outcome::recursive_edit, 0);
}

int32_t lesh_undo(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	// History movement is not a staged write, and mixing the two in one call is
	// not a thing the history can mean: undo restores the buffer to a state the
	// staged edits were never applied to.
	if (editor->buffer_written)
		return LESH_ERR_REFUSED;
	lesh::leshper::state& target = *editor->target;
	if (target.undo_one()) {
		target.gen.bump();
		editor->staged.assign(target.buffer.text());
		editor->staged_cursor = target.cursor.byte_offset();
		editor->cursor_written = false;
		// The staging area is re-synced from the state history just restored,
		// selection included: an action that undoes and then reads the selection
		// must see the one that came back, not the one it started the call with.
		editor->staged_anchor = target.selection_anchor().byte_offset();
		editor->staged_selection_active = target.selection_active();
		editor->selection_written = false;
	}
	return LESH_OK;  // nothing to undo is not an error
}

int32_t lesh_redo(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	if (editor->buffer_written)
		return LESH_ERR_REFUSED;
	lesh::leshper::state& target = *editor->target;
	if (target.redo_one()) {
		target.gen.bump();
		editor->staged.assign(target.buffer.text());
		editor->staged_cursor = target.cursor.byte_offset();
		editor->cursor_written = false;
		editor->staged_anchor = target.selection_anchor().byte_offset();
		editor->staged_selection_active = target.selection_active();
		editor->selection_written = false;
	}
	return LESH_OK;
}

int32_t lesh_push_input(lesh_editor* editor, const char* bytes, size_t length) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	editor->pushed_input.append(bytes == nullptr ? "" : bytes, length);
	return LESH_OK;
}

// --- The request token -----------------------------------------------------

int32_t lesh_request_buffer_length(const lesh_request* request, size_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->buffer.size();
	return LESH_OK;
}

int32_t lesh_request_buffer(const lesh_request* request, char* out, size_t capacity,
                            size_t* length_out) {
	LESH_REQUEST_HANDLE(request);
	return copy_out(request->buffer, out, capacity, length_out);
}

int32_t lesh_request_cursor(const lesh_request* request, size_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->cursor;
	return LESH_OK;
}

int32_t lesh_request_selection(const lesh_request* request, size_t* start_out,
                               size_t* end_out, int32_t* active_out) {
	LESH_REQUEST_HANDLE(request);
	if (start_out == nullptr || end_out == nullptr || active_out == nullptr)
		return LESH_ERR_INVAL;
	*start_out = request->selection_start;
	*end_out = request->selection_end;
	*active_out = request->selection_active ? 1 : 0;
	return LESH_OK;
}

int32_t lesh_request_generation(const lesh_request* request, uint64_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->computed_against.value();
	return LESH_OK;
}

int32_t lesh_request_event_kind(const lesh_request* request, uint32_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->event_kind;
	return LESH_OK;
}

int32_t lesh_request_superseded(const lesh_request* request, int32_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->superseded != nullptr
	       && request->superseded->load(std::memory_order_relaxed) ? 1 : 0;
	return LESH_OK;
}

int32_t lesh_emit_span(lesh_request* request, size_t start, size_t end, uint32_t style_id) {
	LESH_REQUEST_HANDLE(request);
	if (request->spans == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view text{request->buffer};
	std::size_t begin = clamp_into(text, start);
	std::size_t stop = clamp_into(text, end);
	if (stop < begin)
		std::swap(begin, stop);
	// Copied at the call site, into storage the loop owns: the worker's arena
	// dies with the request (#90) and nothing it allocated is retained.
	request->spans->push_back(decoration_span{begin, stop, style_id});
	return LESH_OK;
}

int32_t lesh_emit_virtual_text(lesh_request* request, size_t at, const char* bytes,
                               size_t length) {
	LESH_REQUEST_HANDLE(request);
	if (request->texts == nullptr)
		return LESH_ERR_INVAL;
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	request->texts->push_back(
		virtual_text{clamp_into(request->buffer, at), std::string(bytes == nullptr ? "" : bytes, length)});
	return LESH_OK;
}

int32_t lesh_propose(lesh_request* request, uint32_t kind, const char* bytes, size_t length) {
	LESH_REQUEST_HANDLE(request);
	if (request->proposals == nullptr)
		return LESH_ERR_INVAL;
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	if (kind != LESH_PROPOSAL_AUTOSUGGESTION && kind != LESH_PROPOSAL_COMPLETION)
		return LESH_ERR_INVAL;
	request->proposals->push_back(
		proposal{kind, std::string(bytes == nullptr ? "" : bytes, length)});
	return LESH_OK;
}

} // extern "C"

// ===========================================================================
// The loop side.
// ===========================================================================

namespace lesh::leshper {
namespace {

// Diffs the staged bytes against the buffer and answers the ONE replacement
// that explains the difference: common prefix off the front, common suffix off
// the back, whatever is left in the middle.
//
// One replacement is the whole point. An action that edited six times, or set
// the buffer wholesale, or edited and changed its mind, all arrive here as one
// record - which is what makes "one undo entry, one generation bump" (#92,
// A-12) a property of the commit rather than of the action's manners.
struct difference {
	std::size_t from = 0;
	std::size_t to = 0;
	std::string_view inserted;
	bool any = false;
};

difference diff_of(std::string_view was, std::string_view now) noexcept {
	if (was == now)
		return difference{};
	std::size_t prefix = 0;
	const std::size_t shortest = was.size() < now.size() ? was.size() : now.size();
	while (prefix < shortest && was[prefix] == now[prefix])
		++prefix;
	std::size_t suffix = 0;
	while (suffix < shortest - prefix && was[was.size() - 1 - suffix] == now[now.size() - 1 - suffix])
		++suffix;
	difference d;
	d.from = prefix;
	d.to = was.size() - suffix;
	d.inserted = now.substr(prefix, now.size() - suffix - prefix);
	d.any = true;
	return d;
}

} // namespace

bool handle_is_live(const editor_handle* handle) noexcept {
	return handle != nullptr && handle->live() && handle->target != nullptr
	    && handle->owner_thread == current_thread_key();
}

bool token_is_live(const request_token* token) noexcept {
	return token != nullptr && token->live() && token->owner_thread == current_thread_key();
}

action_result loop_harness::invoke(state& target, std::string_view name,
                                   const invocation& how) {
	action_result result;

	const auto found = _registry->actions.find(name);
	if (found == _registry->actions.end()) {
		result.status = LESH_ERR_NOTFOUND;
		return result;
	}

	const generation before = target.gen;
	const position cursor_before = target.cursor;

	// The handle is a member, reused: dispatch happens once per keystroke and
	// N-2 wants the hot path free of allocation the loop did not have to do.
	_handle.target = &target;
	_handle.registry = _registry;
	_handle.staged.assign(target.buffer.text());
	_handle.staged_cursor = target.cursor.byte_offset();
	_handle.buffer_written = false;
	_handle.cursor_written = false;
	_handle.staged_anchor = target.selection_anchor().byte_offset();
	_handle.staged_selection_active = target.selection_active();
	_handle.selection_written = false;
	_handle.pushed_input.clear();
	_handle.outcome = static_cast<std::uint8_t>(loop_outcome::none);
	_handle.exit_status = 0;
	_handle.depth = 0;
	_handle.owner_thread = current_thread_key();
	_handle.call_token = ++_registry->calls;

	lesh_invocation crossing{};
	// The registry's own key: already NUL-terminated, stable for the call, and
	// not a copy that could disagree with the name dispatch actually matched.
	crossing.action_name = found->first.c_str();
	crossing.keys = how.keys.empty() ? nullptr : how.keys.data();
	crossing.keys_length = how.keys.size();
	crossing.numeric_argument = how.numeric_argument;
	crossing.has_numeric_argument = how.has_numeric_argument ? 1 : 0;

	result.status = found->second.fn(&_handle, &crossing, found->second.userdata);

	// The action is done, so the handle is dead from here: an accessor called on
	// it now fires the debug assertion, which is the half of "valid only for the
	// receiving call" that a check can catch. What follows is the loop reading
	// its own staging area, which it owns and which no handle guards.
	_handle.call_token = 0;
	_handle.owner_thread = 0;

	const difference change = diff_of(target.buffer.text(), _handle.staged);

	// F-4's coalescing rule, decided from what the action DID rather than from
	// which action it was. The enum path in editor.cpp knows `self_insert` by
	// name and breaks the run for everything else; the loop has no names to go
	// by and does not need any - a run of plain typing is a run of single plain
	// insertions, and anything else ends it. The two rules agree on all nine
	// built-ins and this one also covers the actions nobody has written yet.
	const bool plain_insertion =
		change.any && change.from == change.to && !change.inserted.empty();
	if (!plain_insertion)
		target.undo.break_coalescing();

	if (change.any) {
		// Where the cursor ends up, recorded on the undo entry so F-4's "undo
		// restores text AND cursor" survives the trip. `staged_cursor` is the
		// cursor the action left behind - moved by every write to the end of the
		// replacement, which is where every built-in wants it, and moved again by
		// any explicit lesh_cursor_set. An action that only replaced the whole
		// buffer moved it nowhere, and it stays where the user had it, clamped.
		const position landing =
			position::from_byte_offset(snap_back(_handle.staged, _handle.staged_cursor));
		apply_edit(target, position::from_byte_offset(change.from),
		           position::from_byte_offset(change.to), change.inserted, &landing);
		result.buffer_changed = true;
	} else if (_handle.cursor_written) {
		target.cursor = position::from_byte_offset(snap_back(target.buffer.text(),
		                                                     _handle.staged_cursor));
	}

	// The selection, committed AFTER the edit, and only when the action wrote
	// one. An action that did not touch it has had its anchor carried across the
	// edit by apply_edit's marker rules already; an action that did wrote offsets
	// against the staged text, which is the text the buffer now holds, so the
	// staged anchor is the one that means what the action meant.
	if (_handle.selection_written) {
		target.set_selection(
			position::from_byte_offset(snap_back(target.buffer.text(), _handle.staged_anchor)),
			_handle.staged_selection_active);
	}

	if (!_handle.pushed_input.empty())
		target.pending.injected += _handle.pushed_input;

	result.outcome = static_cast<loop_outcome>(_handle.outcome);
	result.exit_status = _handle.exit_status;
	result.cursor_moved = target.cursor != cursor_before;
	_handle.target = nullptr;

	// The same rule the enum path follows: an action that changed nothing asks
	// for nothing, a mutation asks the reactors to recompute, and a bare cursor
	// move asks only for a redraw (A-10).
	if (target.gen != before || result.cursor_moved) {
		result.produced.push_back(render_request{});
		if (target.gen != before)
			result.produced.push_back(worker_request{target.gen});
	}
	return result;
}

std::vector<reactor_batch> loop_harness::react(const state& target, std::uint32_t kinds) {
	std::vector<reactor_batch> batches;
	_superseded.store(false, std::memory_order_relaxed);

	for (const auto& [name, entry] : _registry->reactors) {
		if ((entry.event_mask & kinds) == 0)
			continue;

		reactor_batch batch;
		batch.reactor = name;
		batch.computed_against = target.gen;
		batch.event_kind = kinds;

		request_token token;
		token.buffer.assign(target.buffer.text());
		token.cursor = target.cursor.byte_offset();
		// The derived region, snapshotted with everything else (#96). Reported
		// even when inactive, on the same reasoning lesh_selection_get gives: the
		// anchor outlives deactivation and the flag is the separate question.
		{
			const std::size_t anchor = target.selection_anchor().byte_offset();
			const std::size_t head = token.cursor;
			token.selection_start = anchor < head ? anchor : head;
			token.selection_end = anchor < head ? head : anchor;
			token.selection_active = target.selection_active();
		}
		token.computed_against = target.gen;
		token.event_kind = kinds;
		token.superseded = &_superseded;
		token.spans = &batch.spans;
		token.texts = &batch.texts;
		token.proposals = &batch.proposals;
		token.owner_thread = current_thread_key();
		token.call_token = ++_registry->calls;

		batch.status = entry.fn(&token, entry.userdata);

		token.call_token = 0;
		batches.push_back(std::move(batch));
	}
	return batches;
}

bool loop_harness::apply(const state& target, reactor_batch batch) {
	// N-4, and the only place it is decided. There is no other applier and no
	// other way in, so a stale batch is not rejected here so much as it has
	// nowhere else to go.
	if (!(batch.computed_against == target.gen))
		return false;
	// The emitting reactor is the decoration namespace: a new batch from a
	// reactor replaces that reactor's previous one and touches nobody else's.
	for (auto it = _applied.begin(); it != _applied.end(); ++it) {
		if (it->reactor == batch.reactor) {
			*it = std::move(batch);
			return true;
		}
	}
	_applied.push_back(std::move(batch));
	return true;
}

} // namespace lesh::leshper
