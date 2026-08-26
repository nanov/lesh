#pragma once

// The vi repertoire (#99, #119, architecture spec §6.5).
//
// WHAT IS IN, and it is zle's vi mode with counts and text objects:
//
//   motions      h l j k 0 ^ $ w W b B e E f F t T ; ,
//   operators    d c y, with the doubled line forms dd cc yy
//   edits        x s r ~ D C Y p P
//   mode entry   i I a A o O v, Escape back
//   text objects iw aw iW aW, and i/a over ( [ { " ' `
//   counts       that multiply the way vi's do - d2w, 3dd
//   repeat       `.`, over non-inserting changes
//
// THE BOUNDARY, stated rather than discovered (#99 answer 4). NOT included, and
// not by oversight:
//
//   NAMED REGISTERS (`"ayy`). The store underneath is already keyed and the
//   unnamed key is one of its keys, so `"a` arrives as an addressable view over
//   the same table - additively, with no second read path in `p`.
//
//   MARKS (`m` and the backtick jumps). A prompt has no places to come back to.
//
//   MACROS (`q`, `@`). Recording invocations properly needs N-3's replay harness
//   to test against, and a macro facility that cannot be tested is a facility
//   that will be wrong.
//
//   VISUAL-BLOCK. #96 kept blockwise out of v1 deliberately: a rectangle is
//   derivable from the two positions plus the line geometry, both of which
//   exist, so it arrives as a projection and breaks nothing.
//
//   INSERT-REPEAT (`.` after `ciw`foo). The record is there and the replay door
//   is there; what is missing is that replaying an insert means replaying the
//   text that was typed into it, which is invocation recording rather than key
//   recording. `.` says so by doing nothing rather than by doing half of it.
//
// WHY THIS FILE LOOKS LIKE builtin_actions.cpp. vi.cpp includes `leshper/abi.h`
// and this header, and this header includes nothing from leshper at all - so
// the forty-odd actions below reach the editor through exactly the surface a
// Lua binding gets, held there by the compiler rather than by care. ADR-0008's
// "no native side door" is not a rule vi is exempt from because vi is builtin;
// it is the reason the mode-entry capabilities became ABI functions instead of
// C++ calls on `state`.

#include <cstddef>

struct lesh_registry;

namespace lesh::leshper {

// The vi mode's own memory: what `;` repeats, which `f`-family key is waiting
// for its target character, and whether the object that just ran told the
// operator its span was a whole line.
//
// REGISTRATION-TIME CONTEXT, which is where ADR-0008 already puts a built-in's
// state - the `void* userdata` in the action signature, the same door the
// highlighter and the autosuggester use for their arenas. It is deliberately
// NOT editor state: none of it survives a replay comparison (N-3), and none of
// it should - vim's own last-find is per-session, not per-buffer.
struct vi_context;

[[nodiscard]] vi_context* vi_context_create();
void vi_context_destroy(vi_context* self) noexcept;

// Registers the vi actions through the ABI and by no other route (A-11).
// `self` must outlive `reg`. Answers how many were registered.
std::size_t register_vi_actions(::lesh_registry& reg, vi_context& self);

// The owner ADR-0007 asks for, so no caller has to remember the pair above.
class owned_vi_context {
public:
	owned_vi_context() : _self(vi_context_create()) {}
	~owned_vi_context() { vi_context_destroy(_self); }

	owned_vi_context(const owned_vi_context&) = delete;
	owned_vi_context& operator=(const owned_vi_context&) = delete;

	[[nodiscard]] vi_context& get() const noexcept { return *_self; }

private:
	vi_context* _self;
};

} // namespace lesh::leshper
