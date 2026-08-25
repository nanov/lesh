#pragma once

// The wiring site: `shell_knowledge`, answered by the real `shell_state` (#135).
//
// THIS HEADER IS IN NO LIBRARY TARGET, and that is the point. `lesh_leshper`
// does not link `lesh_runtime` - CMakeLists.txt says so and spec §4.4 is why -
// so this file is not in `lesh_leshper`'s source list. It is compiled by the
// header self-containment target, which links both, and by `lesh_tests`, which
// links both; the switch-on ticket (#134) adopts it into `src/leshper/read.*`,
// the place where lesh-side and leshper-side types are allowed to meet. If some
// file inside `lesh_leshper` ever includes it, the link fails - which is the
// rule enforcing itself rather than being remembered.
//
// It is the same arrangement `history_search.h` describes for `history_source`
// and #113's store: leshper depends on a shape, the shape is filled in where
// both halves are already linked, and the tests fake it.
//
// HEADER-ONLY because there is nothing to hide: four lookups, each one line, and
// no state but a pointer. A .cpp would need a target of its own to live in, and
// the target it would need is the one #134 is about to create.

#include "leshper/shell_knowledge.h"
#include "runtime/builtins.h"
#include "runtime/shell_state.h"

#include <string_view>

namespace lesh::leshper {

// `shell_state`, read through the one window leshper is allowed.
//
// NO VERSION, NO LOCK, NO COPY - ADR-0009. The shell is the main thread and owns
// `shell_state`; a highlight, a port call that writes state and an execution are
// serialized on it, so a view into the state's own storage cannot be invalidated
// while the call that took it is running. #130's copy-on-write definitions
// version existed to make this safe across threads and was deleted with the
// second thread, not kept as insurance.
//
// The state must outlive this adapter, which must outlive every request token
// that points at it.
class shell_state_knowledge final : public shell_knowledge {
public:
	explicit shell_state_knowledge(const runtime::shell_state& state) noexcept
		: _state(&state) {}

	// Alias, function, builtin - the executor's own order, and the reason it is
	// worth reading beside `executor.cpp`'s command search rather than inventing
	// here: a highlighter that disagreed with the executor about what a name IS
	// is C-5's bug class wearing a colour.
	//
	// `external` is never answered: it costs a stat, and the token is the side
	// that memoizes those. `unknown` from here means "not in the three tables",
	// and the token walks `$PATH` next.
	[[nodiscard]] command_kind classify(std::string_view name) const override {
		std::string_view ignored;
		if (_state->lookup_alias(name, ignored))
			return command_kind::alias;
		if (_state->has_function(name))
			return command_kind::function;
		if (runtime::classify_builtin(name) != runtime::builtin_kind::none)
			return command_kind::builtin;
		return command_kind::unknown;
	}

	// The SHELL's `$PATH` - the variable, not the environment. They part company
	// the moment the line being typed is `PATH=/opt/bin` and the assignment has
	// run, which is exactly the case #124 recorded `getenv` as getting wrong.
	[[nodiscard]] bool path(std::string_view& out) const override {
		return _state->lookup(std::string_view{"PATH"}, out);
	}

private:
	const runtime::shell_state* _state;
};

} // namespace lesh::leshper
