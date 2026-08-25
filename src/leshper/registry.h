#pragma once

// The C++ side of the action/reactor ABI (#93, ADR-0008): the registries, the
// two handle types, and the loop-side operations that mint them.
//
// abi.h is what a binding sees. This is what leshper sees - the same objects
// from the inside, with the parts C must not be shown. The split is load
// bearing: src/leshper/builtin_actions.cpp includes abi.h and NOT this file, so
// the ten built-in actions are held to the same surface a Lua binding will be,
// by the compiler rather than by review.
//
// WHAT THE LOOP OWNS, and nothing else does:
//
//   Committing an action's staged writes. There is no commit function in
//   abi.h - the action returns and the loop commits, which is why "one undo
//   entry, one generation bump" is a property of the code path rather than of
//   an action author's discipline (#92, A-12).
//
//   Applying a reactor's batch. There is no apply function in abi.h either.
//   The token is the only mint for results, the loop is the only applier, and
//   it applies only what is still current - so N-4's "a stale result cannot be
//   applied" is structural (ADR-0008).
//
// THE LOOP HERE IS A HARNESS FAKE. `loop_harness` runs reactors synchronously
// on the calling thread and keeps applied batches in a member. The real loop -
// worker pool, arenas, latest-wins slots (#90) - is later work, and the seam it
// will arrive at is the three operations below. What is NOT fake is everything
// under them: staging, commit, generation binding, the drop rule.

#include "leshper/abi.h"
#include "leshper/effect.h"
#include "leshper/state.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::leshper {

// What a reactor emits, declared here because the token below points at
// vectors of them and defined below, where the loop-side vocabulary lives.
struct decoration_span;
struct virtual_text;
struct proposal;

} // namespace lesh::leshper

// ---------------------------------------------------------------------------
// The three handles abi.h declares, defined.
//
// At global scope, because that is where the C typedefs put them, and with
// ordinary C++ members, because nothing outside this translation unit and its
// tests may see the definitions anyway. No reinterpret_cast in the whole
// implementation as a result.
// ---------------------------------------------------------------------------

// The registries and the style intern table.
//
// Owns everything it hands out, and frees it in its destructor (ADR-0007). The
// entries hold a function pointer and an opaque context pointer whose lifetime
// belongs to whoever registered it - the binding, which outlives the registry
// or takes it down with it.
struct lesh_registry {
	struct action_entry {
		lesh_action_fn fn = nullptr;
		void* userdata = nullptr;
	};

	struct reactor_entry {
		lesh_reactor_fn fn = nullptr;
		void* userdata = nullptr;
		std::uint32_t event_mask = 0;
	};

	// std::less<> so a std::string_view looks up without building a std::string:
	// dispatch happens once per keystroke and has no business allocating.
	std::map<std::string, action_entry, std::less<>> actions;
	std::map<std::string, reactor_entry, std::less<>> reactors;

	// Interned style names. Index 0 is LESH_STYLE_NONE and is never a name.
	std::vector<std::string> styles{std::string{}};

	// Bumped per action call so a stashed handle can be told from a live one.
	std::uint64_t calls = 0;
};

// The editor, for the duration of one action call.
//
// The staged buffer is a copy, not a view: no accessor lends a pointer into
// editor state, and that has to be true of the implementation and not only of
// the signatures, or the WASM insurance ADR-0006 bought is void.
struct lesh_editor {
	lesh::leshper::state* target = nullptr;
	lesh_registry* registry = nullptr;

	// Staging. `staged` starts as a copy of the buffer and the action edits it;
	// the loop diffs it against the real buffer at commit and applies the one
	// replacement that explains the difference.
	std::string staged;
	std::size_t staged_cursor = 0;
	bool buffer_written = false;
	bool cursor_written = false;

	// Input pushed with lesh_push_input, delivered at commit so it drains
	// through the keymap after the action's edits have landed.
	std::string pushed_input;

	std::uint8_t outcome = 0;  // lesh::leshper::loop_outcome
	std::int32_t exit_status = 0;

	// Validity, debug-asserted. Zero between calls, so the common bug - an
	// action stashing the handle and using it later - fires an assertion instead
	// of reading a stale buffer. A stashed handle used re-entrantly DURING
	// another call is indistinguishable from the real one and is not caught;
	// catching it would need the token inside the pointer, which C does not
	// give us, and the ABI says "valid for the receiving call" rather than
	// pretending otherwise.
	std::uint64_t call_token = 0;
	std::uint64_t owner_thread = 0;

	// Action recursion depth (#92's ceiling of 64).
	int depth = 0;

	[[nodiscard]] bool live() const noexcept { return call_token != 0; }
};

// The request token: one snapshot, one generation, and the only mint for
// results.
struct lesh_request {
	std::string buffer;
	std::size_t cursor = 0;
	std::size_t selection_start = 0;
	std::size_t selection_end = 0;
	bool selection_active = false;
	lesh::leshper::generation computed_against;
	std::uint32_t event_kind = 0;

	// Cooperative cancellation. Points at the loop's flag; never owned.
	const std::atomic<bool>* superseded = nullptr;

	// The batch under construction. Emit-only from C's side - abi.h has no
	// function that reads any of this back.
	std::vector<lesh::leshper::decoration_span>* spans = nullptr;
	std::vector<lesh::leshper::virtual_text>* texts = nullptr;
	std::vector<lesh::leshper::proposal>* proposals = nullptr;

	std::uint64_t call_token = 0;
	std::uint64_t owner_thread = 0;

	[[nodiscard]] bool live() const noexcept { return call_token != 0; }
};

namespace lesh::leshper {

using registry = ::lesh_registry;
using editor_handle = ::lesh_editor;
using request_token = ::lesh_request;

// ---------------------------------------------------------------------------
// What crosses back into the loop.
// ---------------------------------------------------------------------------

// A loop outcome an action REQUESTED (ADR-0008: requested, never performed).
enum class loop_outcome : std::uint8_t {
	none,
	accept_line,
	cancel_line,
	exit,
	recursive_edit,
};

// One invocation, C++-side. Copied into a lesh_invocation at the call.
//
// No name in it: the loop dispatched, so the loop knows the name, and it hands
// the registry's own key across rather than a second copy that could disagree
// with it - or allocate, on a path that runs once per keystroke.
struct invocation {
	std::string keys;
	std::int64_t numeric_argument = 0;
	bool has_numeric_argument = false;
};

// What the loop learns from running one action.
struct action_result {
	std::int32_t status = LESH_OK;
	loop_outcome outcome = loop_outcome::none;
	std::int32_t exit_status = 0;
	bool buffer_changed = false;
	bool cursor_moved = false;
	effects produced;
};

// A reactor's output, copied at the emit call site: nothing here points into
// the worker's arena, which is what lets #90 reset it under us.
struct decoration_span {
	std::size_t start = 0;
	std::size_t end = 0;
	std::uint32_t style_id = LESH_STYLE_NONE;

	friend bool operator==(const decoration_span&, const decoration_span&) noexcept = default;
};

struct virtual_text {
	std::size_t at = 0;
	std::string bytes;

	friend bool operator==(const virtual_text&, const virtual_text&) noexcept = default;
};

struct proposal {
	std::uint32_t kind = LESH_PROPOSAL_AUTOSUGGESTION;
	std::string bytes;

	friend bool operator==(const proposal&, const proposal&) noexcept = default;
};

// One reactor's answer to one event.
//
// The emitting reactor is the decoration namespace (ADR-0008), so the name
// travels with the batch: replacing a reactor replaces its decorations, and no
// second namespacing mechanism is needed.
struct reactor_batch {
	std::string reactor;
	generation computed_against;
	std::uint32_t event_kind = 0;
	std::int32_t status = LESH_OK;
	std::vector<decoration_span> spans;
	std::vector<virtual_text> texts;
	std::vector<proposal> proposals;
};

// ---------------------------------------------------------------------------
// The loop's three operations.
// ---------------------------------------------------------------------------

// A stand-in for the event loop, sufficient to exercise every rule the ABI
// makes and honest about being a stand-in.
//
// One handle, reused for every call: an action dispatch must not allocate, and
// a per-call handle would be a heap allocation on the keystroke path.
class loop_harness {
public:
	explicit loop_harness(registry& reg) noexcept : _registry(&reg) {}

	loop_harness(const loop_harness&) = delete;
	loop_harness& operator=(const loop_harness&) = delete;

	// Dispatches an action by name and commits what it staged.
	//
	// LESH_ERR_NOTFOUND when nothing is registered under that name - dispatch
	// through a name the user has not bound is a miss, not a crash.
	action_result invoke(state& target, std::string_view name, const invocation& how);

	// Runs every reactor subscribed to `kinds`, against a snapshot of `target`.
	//
	// Synchronous, on this thread, which is the fake part. The token each
	// reactor receives is the real one, generation-bound and emit-only.
	[[nodiscard]] std::vector<reactor_batch> react(const state& target, std::uint32_t kinds);

	// Applies a batch, if and only if it was computed against the generation the
	// editor is still at. Answers whether it was applied.
	bool apply(const state& target, reactor_batch batch);

	[[nodiscard]] const std::vector<reactor_batch>& applied() const noexcept {
		return _applied;
	}
	void clear_applied() noexcept { _applied.clear(); }

	// Supersedes whatever is in flight. Called by the loop when the buffer moves
	// on; called by a test from inside a reactor to watch the poll notice.
	void supersede() noexcept { _superseded.store(true, std::memory_order_relaxed); }

	// The handle the last invoke used. Exposed so a test can hold what an action
	// would have stashed and see that it went dead, without a death test having
	// to be the only evidence that handle validity is enforced.
	[[nodiscard]] const editor_handle* handle() const noexcept { return &_handle; }

private:
	registry* _registry;
	editor_handle _handle;
	std::atomic<bool> _superseded{false};
	std::vector<reactor_batch> _applied;
};

// True when the handle is one a call is currently allowed to use. The predicate
// behind the debug assertion in every accessor, exposed so it can be asserted
// on directly rather than only by dying.
[[nodiscard]] bool handle_is_live(const editor_handle* handle) noexcept;
[[nodiscard]] bool token_is_live(const request_token* token) noexcept;

// Registers the built-in actions (F-13), through the ABI and by no other route.
//
// Defined in builtin_actions.cpp, which includes abi.h and nothing else from
// this module. Answers how many were registered.
std::size_t register_builtin_actions(registry& reg);

// The highlighter's registration-time context: its per-request arena and the
// style ids it interned (#124). OPAQUE - defined in builtin_reactors.cpp, which
// like builtin_actions.cpp includes abi.h and nothing else from this module, so
// a built-in reactor that wanted a shortcut would not compile.
//
// It exists as a named thing at all because a reactor, unlike an action, has
// state: an arena it rewinds per request (#90) and a dozen interned ids. That
// state needs an owner that frees it before main returns (ADR-0007), and the
// registry cannot be it - the registry stores a `void*` it never dereferences,
// which is exactly the language-neutral shape #93 asked for.
struct highlighter;

[[nodiscard]] highlighter* highlighter_create();
void highlighter_destroy(highlighter* self) noexcept;

// Registers the built-in reactors (F-20/F-22) through the ABI and by no other
// route (A-11). `self` must outlive `reg`. Answers how many were registered.
std::size_t register_builtin_reactors(registry& reg, highlighter& self);

// The owner ADR-0007 asks for, so no caller has to remember the pair above.
class owned_highlighter {
public:
	owned_highlighter() : _self(highlighter_create()) {}
	~owned_highlighter() { highlighter_destroy(_self); }

	owned_highlighter(const owned_highlighter&) = delete;
	owned_highlighter& operator=(const owned_highlighter&) = delete;

	[[nodiscard]] highlighter& get() const noexcept { return *_self; }

private:
	highlighter* _self;
};

} // namespace lesh::leshper
