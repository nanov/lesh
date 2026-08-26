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
// THE HARNESS IS A FAKE SCHEDULER AND NOTHING ELSE NOW. `loop_harness` runs
// reactors synchronously on the calling thread instead of on #126's pool, and
// dispatches an action by name. It no longer holds what it applied: applying is
// `apply_batch` below, the one implementation the real loop calls too, and what
// it applies lands in the state (#144). What was never fake is everything under
// that: staging, commit, generation binding, the drop rule.

#include "leshper/abi.h"
#include "leshper/effect.h"
#include "leshper/event.h"
#include "leshper/host.h"
#include "leshper/state.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::leshper {

// What a reactor emits. `decoration_span` and `virtual_text` are NOT declared
// here any more: they are `decoration.h`'s, which `state.h` includes, because
// `state::marks` holds them (#141). `proposal` went the same way for the same
// reason (#144): `state::proposals` holds the applied ones, so the type belongs
// under `state.h` rather than over it, and it is `proposal.h`'s.
//
// One reactor's whole answer, carrying both halves; defined below.
struct reactor_batch;

// The prompt engine (#157, spec §6.10). Declared, not included: this header
// hands a pointer through and never touches one, so `prompt.h` - which brings
// `<span>`, the combinator templates and a screenful of static_asserts with it -
// stays out of every translation unit that only wanted an action registry.
namespace prompt {
class engine;
}

// The memo behind lesh_request_command_kind (#135), one per request.
//
// A $PATH walk is a stat per directory, and a line repeats command names -
// `git log | git shortlog`, or the same name in a loop body. Memoizing the whole
// answer means a repeat costs a comparison against at most `capacity` short
// names instead of a second sweep of the filesystem.
//
// FIXED AND INLINE, which is a departure from #130's "memoized in the request's
// arena" and stronger than it: a token minted on the loop thread has no arena at
// all (`current_worker_arena()` is null off a worker, which is every test that
// drives a reactor through `loop_harness`), and an array inside the token
// allocates nothing anywhere rather than allocating cheaply in one of the two
// places. Its life is exactly the request's, which is what "per request" asked
// for.
//
// A name too long to store, or one arriving at a full table, is simply not
// memoized. THE MEMO CAN NEVER CHANGE AN ANSWER, only the cost of asking twice -
// which is what lets both fallbacks be "walk again" rather than a growth policy.
struct command_kind_memo {
	// Long enough for a command name that is not really a command name; a
	// pathname argument to `stat` is bounded elsewhere.
	static constexpr std::size_t name_capacity = 48;
	// Deeper than any line's distinct command names, shallow enough that a miss
	// is a handful of length-compares.
	static constexpr std::size_t capacity = 32;

	struct entry {
		std::uint32_t kind;
		std::uint16_t length;
		char name[name_capacity];
	};

	// Deliberately without a member initializer: entries at or past `used` are
	// never read, and zeroing 1.8 KB per request would be work done for nothing
	// on the path this memo exists to make cheap.
	entry entries[capacity];
	std::uint32_t used = 0;
};

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

	// THE TIMER TABLE IS THE HOST'S NOW (#168). What was here was a declaration -
	// `{id, interval_ms, action}` - which the driver walked on every turn to diff
	// against its own list of due instants, so each side held half a scheduler and
	// the arming was a table comparison rather than news. `lesh_timer_start` and
	// `lesh_timer_stop` emit `arm_timer`/`disarm_timer` effects instead, and
	// `{id, action, interval, due}` lives whole in the host.
	//
	// TWO THINGS STAY, and neither is a schedule. The id counter, because minting
	// ids is what hands the caller its handle; and the ids currently armed,
	// because the ABI promises that stopping a timer twice is LESH_ERR_NOTFOUND -
	// an id the caller has lost track of may now be somebody else's - and an
	// answer no side of the boundary can give is a promise that cannot be kept.
	// No interval, no action name, no clock.
	std::vector<std::uint64_t> armed_timers;
	std::uint64_t next_timer_id = 0;

	// The action names those timers name, interned (#168).
	//
	// THERE IS A TABLE AT ALL because an effect and an event must be trivially
	// copyable - they are values on the channel between the editor and its host,
	// and a `std::string` in one is a heap allocation per arm and another per
	// expiry, which on a `{time}` prompt is one per second forever.
	//
	// THE HANDLE IS THE INDEX, not a pointer and not a view: a
	// `std::vector<std::string>` MOVES its elements when it grows, so a view into
	// one is stable only until the next arm, while an index into one is stable for
	// the registry's life. (`styles` above is interned the same way, for the same
	// reason.)
	//
	// The name is still resolved at DISPATCH and not here: a timer armed before
	// its action is registered is legal, and re-registering the action replaces
	// what the timer runs. Interning fixes the allocation without touching that.
	std::vector<std::string> timer_actions;

	// Effects an ABI verb produced with no action dispatch to hang them on
	// (#168), drained by the host on its next turn.
	//
	// THE VERBS THAT NEED IT TAKE A REGISTRY AND NOT AN EDITOR. `lesh_timer_start`
	// is called from the wiring site - the prompt arms its own tick from
	// `src/ui/session.cpp`, outside any dispatch - so there is no `action_result`
	// in scope to return an effect through. A queue on the object the verb was
	// handed is the smallest thing that reaches the driver, and it keeps the ABI
	// signature frozen (abi.h grows additively or not at all).
	//
	// LOOP-THREAD-ORDERED, not synchronised, exactly as the timer table it
	// replaces was: the shell thread touches this only while the driver is parked
	// in `wait_on_shell`, which is ADR-0008's "the registry is the loop's" holding
	// rather than being bent.
	lesh::leshper::effects pending;

	// Interned style names. Index 0 is LESH_STYLE_NONE and is never a name.
	std::vector<std::string> styles{std::string{}};

	// Bumped per action call so a stashed handle can be told from a live one.
	std::uint64_t calls = 0;

	// THE HOST (#168 Phase B; host.h).
	//
	// ONE BORROWED POINTER WHERE THERE WERE TWO. `completion` was #94's
	// `Completer` override point and `knowledge` was #135's shell tables; both
	// were leshper-declared interfaces filled in by the ui layer, and Phase B
	// found they were one door with two names. `host` is that door: it answers
	// `lesh_request_command_kind` and it carries out `want_completion`.
	//
	// Whoever set it owns it and must outlive the registry (ADR-0007: the owner
	// takes the view away as it takes the object). Null is "no host attached" and
	// is not an error - `lesh_complete` reports LESH_ERR_NOTFOUND, the same answer
	// a leshper embedded with no completer should give, and a name classifies as
	// LESH_COMMAND_UNKNOWN.
	lesh::leshper::host* host = nullptr;

	// The prompt engine the `lesh_prompt_*` verbs configure (#157, §6.10).
	//
	// A BORROWED POINTER ON THE REGISTRY, for `host`'s reason exactly: the
	// ABI's only long-lived handle is the registry, so a registry of a second
	// kind has to hang off it. Null is "no prompt engine wired up" - a
	// non-interactive shell - and every prompt verb reports it as
	// LESH_ERR_NOTFOUND rather than pretending the configuration landed
	// somewhere.
	//
	// NOT const: configuring is the point of the door.
	// Whoever set it owns it and must outlive the registry.
	lesh::leshper::prompt::engine* prompt_engine = nullptr;
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

	// The selection, staged like everything else (#96, spec §6.3). The head is
	// `staged_cursor` - the model has no second stored position - so only the
	// anchor and the flag need a home here. Staged rather than written straight
	// through to the target, because an action that sets the selection and then
	// edits would otherwise have its anchor adjusted a second time by the marker
	// rules at commit, against a diff it had already accounted for.
	std::size_t staged_anchor = 0;
	bool staged_selection_active = false;
	bool selection_written = false;

	// Input pushed with lesh_push_input, delivered at commit so it drains
	// through the keymap after the action's edits have landed.
	std::string pushed_input;

	// WHAT THE LOOP HAS APPLIED IS REACHED THROUGH `target` (#144). There was a
	// borrowed `applied` pointer here, re-pointed on every `invoke` at a vector
	// the harness owned - and the real loop, whose applier is `take_batch`, never
	// pointed it anywhere, so `lesh_proposal_read` in the running shell walked an
	// empty view while the suggestion was on screen. The view lives in
	// `state::proposals` now, beside the decorations it is the other half of, and
	// the field that has to be set for an action to see it is the one every
	// dispatch path already sets.
	//
	// Still READ-ONLY from the ABI's side: there is no emit function on this
	// handle, so an accepting action can learn what was proposed and can reach
	// the buffer only by staging a write (A-12).

	// A dismissal the action REQUESTED (lesh_proposal_dismiss), honoured by the
	// loop after the action's writes have been committed - the same shape the
	// loop outcomes have, for the same reason: the action goes on running.
	std::uint32_t dismissed_kind = 0;
	bool dismiss_requested = false;

	// What `lesh_complete` found, this call (#139, #168 Phase B).
	//
	// A MEMBER AND NOT A RETURN VALUE, because three ABI calls read it - the
	// count, the span, and the candidates one at a time - and because the handle
	// is reused across dispatches.
	//
	// A BORROWED VIEW AND NOT A LIST. It was a `completion_result` with a
	// `std::vector<candidate>` in it, which put the shell's candidate strings
	// inside the editor handle - a second copy of a list the completer already
	// held. It is the `completion_candidates` EVENT now: a pointer into the
	// host's own storage, valid until the next `want_completion` that host
	// carries out, which is exactly as long as the action that asked.
	//
	// It does NOT become a proposal and the loop never takes it: the candidates'
	// destination is the pager (#138), which the action fills through
	// `lesh_pager_add` before it returns, and a second copy of the list living on
	// past the call would be a second answer to "what is showing".
	lesh::leshper::completion_candidates completion;
	bool completion_ran = false;

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

	// THE HOST (#168 Phase B; host.h). Never owned.
	//
	// Copied onto the token when it is minted, so a reactor on a worker asks the
	// same host the registry holds without reaching back through it. Read-only by
	// construction: ADR-0009 makes the shell thread the sole owner of
	// `shell_state`, and a highlight, a port call and an execution are serialized
	// on it.
	//
	// Null is "no host attached", and it is not an error: every name classifies as
	// LESH_COMMAND_UNKNOWN, which is what a leshper embedded in something that is
	// not this shell would see.
	//
	// CONST, where `registry::host` is not, and the difference is the thread. The
	// only verb a token asks is `classify_command`, which is const and re-entrant;
	// `carry_out` writes the host's own scratch and is the loop's alone. A token
	// that cannot name the second cannot be the route by which a worker reaches
	// it.
	const lesh::leshper::host* host = nullptr;

	// Mutable because classifying is a QUERY - `lesh_request_command_kind` takes
	// a `const lesh_request*` beside every other reader on this token - and the
	// memo is the cost of asking, not part of the answer.
	mutable lesh::leshper::command_kind_memo command_kinds;

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

// A reactor's output is copied at the emit call site: nothing in a batch points
// into the worker's arena, which is what lets #90 reset it under us.
// `decoration_span` and `virtual_text` are in `decoration.h`; `proposal` is in
// `proposal.h`, beside the store that holds the applied ones (#144). Neither
// includes abi.h, so both spell a defaulted constant as a literal, and this is
// where the spellings are checked against each other rather than commented on.
static_assert(LESH_STYLE_NONE == 0u,
              "decoration.h defaults style_id to 0 and means LESH_STYLE_NONE");
static_assert(LESH_PROPOSAL_AUTOSUGGESTION == 0u,
              "proposal.h defaults kind to 0 and means LESH_PROPOSAL_AUTOSUGGESTION");

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
// The loop's three operations. Applying is the first of them, and it is a free
// function rather than a member because there is exactly ONE of it (#144).
// ---------------------------------------------------------------------------

// Interns an action name for a timer effect, answering the handle (#168).
//
// Deduplicated, so arming the same name twice is the same handle and the table is
// bounded by the distinct names ever armed rather than by how often a cadence
// changed. The allocation is here, at arm time, and nowhere on the expiry path.
std::uint32_t intern_timer_action(registry& reg, std::string_view name);

// The name behind a handle; empty for a handle this registry never minted.
[[nodiscard]] std::string_view timer_action_name(const registry& reg,
                                                 std::uint32_t handle) noexcept;

// Applies a batch, if and only if it was computed against the generation the
// editor is still at. Answers whether it was applied.
//
// N-4, AND THE ONLY PLACE IT IS DECIDED. There is no other applier and no other
// way in, so a stale batch is not rejected here so much as it has nowhere else
// to go: `event_loop::take_batch` is this plus a log line and a repaint, and the
// harness path is this and nothing else.
//
// THERE USED TO BE TWO. `loop_harness::apply` kept whole batches in a member and
// `take_batch` wrote `state::marks`, which was survivable while the harness was
// the only thing anybody dispatched through - and stopped being survivable the
// moment the real loop ran actions, because the halves had drifted: the loop
// painted the suggestion and the accessor read the harness's empty vector (#144,
// returned by #141). #118 retired the enum path the same way, and for the same
// reason: one implementation, reached by both callers.
//
// SWAPS, never copies, on both halves. The batch is pooled storage (#126's
// message pool, or the shell channel's recycler) and moving out of it would hand
// the pool back vectors with no capacity, defeating the pooling on the very path
// it was built for. What comes back in the batch is the previous layer's
// storage, which is what the pool wants and what `release` clears.
bool apply_batch(state& target, reactor_batch& batch);

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

	// APPLYING IS NOT A MEMBER any more, and neither is the store it wrote to:
	// `apply_batch` above is the one applier and `state::proposals` is where the
	// proposals land. See the argument there.

	// Supersedes whatever is in flight. Called by the loop when the buffer moves
	// on; called by a test from inside a reactor to watch the poll notice.
	void supersede() noexcept { _superseded.store(true, std::memory_order_relaxed); }

	// THE OUTCOME LATCH IS GONE (#168). `take_outcome` parked what an action had
	// asked for - accept, cancel, exit, recursive edit - and the driver pulled it
	// off this object after `step` returned, because `step` answers with effects
	// and a latch was the only way an outcome could ride out of the keystroke
	// path. It rides out as an EFFECT now: `invoke` pushes `line_accepted`,
	// `line_cancelled`, `end_of_file` or `recursive_edit_request` onto
	// `action_result::produced`, `invoke_action` folds that into what `step`
	// returns, and the driver carries it out with every other effect. Same "last
	// wins" for two verbs on one key, now because effects are carried out in
	// order rather than because a slot was overwritten.

	// The handle the last invoke used. Exposed so a test can hold what an action
	// would have stashed and see that it went dead, without a death test having
	// to be the only evidence that handle validity is enforced.
	[[nodiscard]] const editor_handle* handle() const noexcept { return &_handle; }

	// THE HOST IS THE REGISTRY'S (#168). `set_shell_knowledge` was here, which
	// made a setter on the FAKE host the way a real shell told the editor where
	// its own tables were. It hangs off `registry::host` now - one borrowed
	// pointer beside `prompt_engine`, answering command kinds and completion
	// alike - and `react` reads it from there.

private:
	registry* _registry;
	editor_handle _handle;
	std::atomic<bool> _superseded{false};
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

// Registers the pager's actions (#138) - cycling, filtering, accepting,
// closing, and the three that fill it from a reactor's proposals.
//
// ITS OWN TRANSLATION UNIT, and that is the no-side-door rule showing where the
// boundary is. src/leshper/pager.cpp renders and decides, and to do either it
// must see `state` and `surface`; a file that has seen those cannot also be
// held to the ABI by the compiler. So the actions live in
// src/leshper/pager_actions.cpp, which includes abi.h and nothing else from
// leshper - exactly builtin_actions.cpp's shape, declared here for the same
// reason `register_builtin_actions` is. Answers how many were registered.
std::size_t register_pager_actions(registry& reg);

// THE BUILT-IN REACTORS ARE THE HOST'S (#168 Phase B). The highlighter
// (F-20/F-21/F-22) and the autosuggester (F-24 to F-26) were declared here and
// defined in `src/leshper/builtin_reactors.cpp`; they are `src/ui/reactors.h`
// and `src/ui/reactors.cpp` now, in `lesh::ui`. Nothing about how they reach the
// editor changed - each is a function pointer and a `void*` in the reactor
// table, registered through the ABI and reachable by no other route (A-11) -
// which is why the move cost this header a deletion and the ui layer one
// include. What they DO is knowledge: a parse, the shell's tables, a `$PATH`
// sweep, a history walk. The editor only colours regions.

} // namespace lesh::leshper
