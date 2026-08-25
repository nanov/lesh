#pragma once

// WHAT THE SHELL KNOWS, as an interface leshper owns (#130, #135; spec §6.7,
// narrowed by ADR-0009).
//
// The highlighter has to say whether `ll` is an alias, `cd` a builtin, `deploy`
// a function and `ls` a thing on `$PATH` (F-21). All four answers live in
// `shell_state`, and `lesh_leshper` does not link `lesh_runtime` - that is the
// CMake rule spec §4.4 made enforceable rather than reviewable. So the shape of
// the question is declared here and the answer is supplied at the wiring site,
// which is exactly the A-5 arrangement `history_source` already has: leshper
// depends on a shape, `src/leshper/shell_state_knowledge.h` implements it over
// the real state, and a test fakes it with a map.
//
// ONE OWNER, NO VERSION. #130 resolved this over a copy-on-write definitions
// version held by the request token, because the highlighter then ran on a
// worker while the loop mutated the tables. ADR-0009 dissolved that: the shell
// is the main thread, it owns `shell_state`, and a highlight, a port call and an
// execution are serialized on it. So the implementation may hand back views into
// the state's own storage - there is no second thread that could invalidate one
// mid-call - and this interface is a plain const reference, not a refcounted
// snapshot. The only version left is the editor's generation, already on the
// token.
//
// WHY TWO METHODS AND NOT ONE. #130 wrote "one method: command_kind(name)". The
// `$PATH` walk is a stat per directory and the token memoizes it per request
// (F-22 keeps the filesystem off the keystroke path), and the memo lives with
// the token - so the walk has to run on leshper's side of this boundary, where
// the token is. The shell's contribution to it is the VALUE of `$PATH` and
// nothing else. Folding the walk behind `classify` would put a filesystem sweep
// where the memo cannot see it and re-stat every candidate for every repeat of a
// name on the line. Both methods below are pure lookups; neither touches disk.

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace lesh::leshper {

// What a command name IS, in the shell's own resolution order.
//
// The numbers are the ABI's `LESH_COMMAND_*` and registry.cpp static_asserts
// that they still agree. `unknown` is zero so that a caller who ignored a
// failed status reads the harmless answer rather than a confident wrong one.
enum class command_kind : std::uint8_t {
	unknown = 0,   // no table holds it and no $PATH directory has it
	external = 1,  // a regular executable file, found by the $PATH walk or named directly
	builtin = 2,   // the static builtin registry
	function = 3,  // a shell function
	alias = 4,     // an alias
};

// The shell's tables, asked one name at a time.
//
// Const throughout: leshper never mutates shell state, which is the whole of
// what makes ADR-0009's single owner work.
class shell_knowledge {
public:
	shell_knowledge() = default;
	virtual ~shell_knowledge() = default;

	shell_knowledge(const shell_knowledge&) = delete;
	shell_knowledge& operator=(const shell_knowledge&) = delete;

	// Alias, then function, then builtin - POSIX's order (2.3.1 for the alias,
	// 2.9.1.1 for the rest), and the order the executor searches in.
	//
	// `unknown` means "none of the three tables", NOT "no such command": the
	// caller walks `$PATH` next. An implementation that already knows a name is
	// external may answer `external` and skip the walk; the `shell_state` adapter
	// does not, because knowing costs a stat and the token is the side that
	// memoizes those.
	//
	// ONE LEVEL OF RESOLUTION FOR AN ALIAS, and never an expansion. `ll` in
	// `alias ll='ls -l'` is an alias and the answer stops there - the body is not
	// re-resolved to see what `ls` is. #95 is why: the highlighter's spans are
	// over the bytes the user typed, and an expanded alias's tokens live in a
	// text region that no position in the line can name.
	[[nodiscard]] virtual command_kind classify(std::string_view name) const = 0;

	// The shell's `$PATH`, borrowed. False when the variable is unset, which is
	// not the same as empty: POSIX gives an empty PATH one empty element, and an
	// empty element means the current directory.
	[[nodiscard]] virtual bool path(std::string_view& out) const = 0;
};

// The answer when no shell has been attached to the token.
//
// Tables empty, `$PATH` from the process environment - which is precisely what
// the highlighter did before this door existed, and what a leshper embedded in
// something that is not this shell would want. Named rather than left as a
// silent `getenv` inside the ABI, so that "where did that PATH come from" has an
// object to point at.
//
// The environment belongs to the shell thread, which is the only thread that
// writes it; a reader on any other thread is reading a table that could be
// changing under it. That is a pre-existing property of `getenv`, unchanged
// here, and it is one more reason the wired-up path passes a real
// `shell_knowledge` instead.
class environment_knowledge final : public shell_knowledge {
public:
	[[nodiscard]] command_kind classify(std::string_view) const override {
		return command_kind::unknown;
	}

	[[nodiscard]] bool path(std::string_view& out) const override {
		const char* value = std::getenv("PATH");
		if (value == nullptr)
			return false;
		out = std::string_view{value};
		return true;
	}
};

} // namespace lesh::leshper
