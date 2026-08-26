#pragma once

// THE BUILT-IN REACTORS, AND THEY ARE THE HOST'S (#168 Phase B; #124, #133).
//
// The highlighter (F-20/F-21/F-22) and the autosuggester (F-24 to F-26) were
// declared in `src/leshper/registry.h` and defined in
// `src/leshper/builtin_reactors.cpp`. They are here because of what they DO, not
// because of how they are reached: a highlight is a re-parse plus the shell's
// alias/function/builtin tables plus a `$PATH` sweep, and a suggestion is a walk
// of the history. All of that is KNOWLEDGE - runtime, syntax, history - and the
// editor's half of both is colouring a region and drawing virtual text.
//
// HOW THEY REACH THE EDITOR DID NOT CHANGE, which is the whole reason the move
// was a file move. Each is a `lesh_reactor_fn` and a `void*`, registered through
// `lesh_reactor_register` and reachable by no other route (A-11, ADR-0008), so
// `reactors.cpp` still includes `leshper/abi.h` and nothing else from leshper -
// it cannot read the buffer except by copying it out of a request token and
// cannot emit except through one. A reactor written in Lua would come in the
// same door; these two now come in from the same SIDE as well.
//
// The registration call sites are `session.cpp`'s, unchanged.

#include <cstddef>

// The registry handle, declared and not included. `lesh_registry` is a C
// typedef of an incomplete type in `leshper/abi.h`; these two functions take a
// reference to it and never touch one, so a caller that already has a registry
// has already included the header that gave it one.
extern "C" {
typedef struct lesh_registry lesh_registry;
}

namespace lesh::ui {

class history_source;

// The highlighter's registration-time context: its per-request arena and the
// style ids it interned (#124). OPAQUE - defined in reactors.cpp, which like
// `leshper/builtin_actions.cpp` includes `abi.h` and no other leshper header, so
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
std::size_t register_builtin_reactors(::lesh_registry& reg, highlighter& self);

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

// Where the entries come from (#125) is `history_search.h`'s `history_source`,
// forward-declared at the top of this header rather than included: this file
// hands a pointer through and never touches one, so the wiring site chooses
// between the store's adapter and a `vector_history_source` without every
// includer of this header learning what either is.

// The autosuggester's registration-time context: its per-request arena, the
// history it searches, and the one style id it interns. OPAQUE, exactly as
// `highlighter` is and for the same reason - reactors.cpp defines it and reaches
// the editor through `abi.h` alone, so a built-in reactor that wanted a shortcut
// would not compile.
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
std::size_t register_autosuggester(::lesh_registry& reg, autosuggester& self);

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

} // namespace lesh::ui
