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
#include "leshper/shell_knowledge.h"
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

	// An armed timer (#128 decision 3, #129's `timer` topic).
	//
	// The INTERVAL lives here and the next DUE INSTANT does not: the registry
	// has never heard of a clock, and the loop that polls is the only thing that
	// knows what "now" is. So the loop keeps a due instant beside each id and
	// this table stays the declaration rather than the schedule.
	struct timer_entry {
		std::uint64_t id = 0;
		std::uint64_t interval_ms = 0;
		std::string action;
	};

	// A vector rather than a map: a session has a handful of timers, they are
	// walked in full on every turn to find the minimum deadline, and a walk is
	// what a vector is for.
	std::vector<timer_entry> timers;
	std::uint64_t next_timer_id = 0;

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

	// What the shell knows (#135). Never owned, and read-only by construction:
	// ADR-0009 makes the shell thread the sole owner of `shell_state`, and a
	// highlight, a port call and an execution are serialized on it.
	//
	// Null is "no shell attached", and it is not an error: the tables read empty
	// and `$PATH` comes from the process environment, which is exactly what the
	// highlighter did before this door existed. The wiring site fills it in.
	const lesh::leshper::shell_knowledge* knowledge = nullptr;

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

	// The loop outcome the last dispatched action REQUESTED, latched here and
	// taken by the loop (#134).
	//
	// WHY A LATCH AND NOT THE RETURN VALUE. `invoke` answers with the whole
	// `action_result`, and the loop's own dispatch - a timer expiry - reads the
	// outcome straight off it. The KEYSTROKE path does not: a key goes through
	// editor.cpp's `step`, which is a pure function of state and events and has
	// no loop to hand an outcome to, so before this existed `accept_line`,
	// `cancel_line` and `exit` requested from a bound key went nowhere at all.
	// The harness is the one object both paths already share, so it holds the
	// request until the loop takes it after `step` returns.
	//
	// LAST WINS within one turn. A turn that dispatched two actions asking for
	// two different outcomes is a keymap that bound two verbs to one key, and
	// the second is the one that ran.
	struct requested_outcome {
		loop_outcome what = loop_outcome::none;
		std::int32_t exit_status = 0;
	};

	[[nodiscard]] requested_outcome take_outcome() noexcept {
		const requested_outcome taken = _requested;
		_requested = requested_outcome{};
		return taken;
	}

	// The handle the last invoke used. Exposed so a test can hold what an action
	// would have stashed and see that it went dead, without a death test having
	// to be the only evidence that handle validity is enforced.
	[[nodiscard]] const editor_handle* handle() const noexcept { return &_handle; }

	// The shell's tables, put on every token this mints (#135). Null - the
	// default - is "no shell attached"; see the field on the token. `knowledge`
	// must outlive this harness.
	void set_shell_knowledge(const shell_knowledge* knowledge) noexcept {
		_knowledge = knowledge;
	}

private:
	registry* _registry;
	editor_handle _handle;
	std::atomic<bool> _superseded{false};
	const shell_knowledge* _knowledge = nullptr;
	requested_outcome _requested;
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

// ---------------------------------------------------------------------------
// The autosuggester (#133: F-24 to F-26), the second built-in reactor.
// ---------------------------------------------------------------------------

// Where the entries come from (#125). Declared, not included: this header hands
// a pointer through and never touches one, so the wiring site chooses between
// the store's adapter and a `vector_history_source` without registry.h knowing
// either exists.
class history_source;

// The autosuggester's registration-time context: its per-request arena, the
// history it searches, and the one style id it interns. OPAQUE, exactly as
// `highlighter` is and for the same reason - builtin_reactors.cpp defines it and
// includes nothing from this module, so a built-in reactor that wanted a
// shortcut would not compile.
struct autosuggester;

// `source` is BORROWED and must outlive the returned context. Null is accepted
// here and refused per request as LESH_ERR_INVAL, on #125's reasoning: a
// provider wired up wrong should say so rather than look like an empty history.
[[nodiscard]] autosuggester* autosuggester_create(const history_source* source);
void autosuggester_destroy(autosuggester* self) noexcept;

// Registers the autosuggester through the ABI and by no other route (A-11).
// `self` must outlive `reg`. Answers how many were registered.
//
// Its own function rather than a second argument to register_builtin_reactors,
// because that would be a signature change on a call site the loop already
// makes - the same additive-growth rule the ABI is held to, applied one layer
// up.
std::size_t register_autosuggester(registry& reg, autosuggester& self);

// The owner ADR-0007 asks for. `source` is borrowed and must outlive this.
class owned_autosuggester {
public:
	explicit owned_autosuggester(const history_source* source)
		: _self(autosuggester_create(source)) {}
	~owned_autosuggester() { autosuggester_destroy(_self); }

	owned_autosuggester(const owned_autosuggester&) = delete;
	owned_autosuggester& operator=(const owned_autosuggester&) = delete;

	[[nodiscard]] autosuggester& get() const noexcept { return *_self; }

private:
	autosuggester* _self;
};

} // namespace lesh::leshper
