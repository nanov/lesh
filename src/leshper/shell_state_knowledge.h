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
#include "substrate/assert.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

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
//
// AND IT IS WHERE ADR-0009 IS CHECKED (#151). Every method below opens with
// `assert_readable`, which fails if the shell thread is inside `execute` or
// `port_call` - the two verbs that can WRITE the state this reads. The check
// belongs here rather than in the base class because this is the adapter over
// the REAL state: a fake over a map has nothing a running command could change,
// and a check it could never fail is a check nobody would trust. Debug-only:
// `LESH_ASSERT` compiles out in release.
class shell_state_knowledge final : public shell_knowledge {
public:
	// `writing` is the flag `shell_actor` raises around the two writers. Null -
	// the default - means "unchecked", which is what every adapter built over a
	// state that no actor is serving gets, and what the tests that own their own
	// `shell_state` want.
	explicit shell_state_knowledge(const runtime::shell_state& state,
	                               const shell_writing_flag* writing = nullptr) noexcept
		: _state(&state), _writing(writing) {}

	// Alias, function, builtin - the executor's own order, and the reason it is
	// worth reading beside `executor.cpp`'s command search rather than inventing
	// here: a highlighter that disagreed with the executor about what a name IS
	// is C-5's bug class wearing a colour.
	//
	// `external` is never answered: it costs a stat, and the token is the side
	// that memoizes those. `unknown` from here means "not in the three tables",
	// and the token walks `$PATH` next.
	[[nodiscard]] command_kind classify(std::string_view name) const override {
		assert_readable();
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
		assert_readable();
		return _state->lookup(std::string_view{"PATH"}, out);
	}

	// The enumeration read (#139, spec 6.9). Copies, because the answer is about
	// to cross to the loop thread - see the comment on the base class.
	//
	// ONE TABLE PER DOMAIN AND NO MERGING, so a candidate keeps the fact that
	// decides its marker. Builtins come from the same `kBuiltinRegistry`
	// `classify_builtin` reads, so the completer cannot offer a builtin the
	// classifier would then call unknown. Nothing here touches the filesystem.
	void enumerate(name_domain which, std::vector<std::string>& into) const override {
		assert_readable();
		switch (which) {
			case name_domain::builtin:
				for (const runtime::builtin_descriptor& one : runtime::kBuiltinRegistry)
					into.emplace_back(one.name);
				break;
			case name_domain::function:
				for (std::string_view name : _state->function_names())
					into.emplace_back(name);
				break;
			case name_domain::alias:
				for (const runtime::shell_state::alias_row& row : _state->aliases())
					into.emplace_back(row.name);
				break;
			case name_domain::variable:
				for (const runtime::shell_state::variable_row& row : _state->variables()) {
					// A name that was marked (`export x`) but never assigned is
					// still a name the user can complete to: `$x` expands to
					// nothing, which is what an unset variable does anyway, and
					// hiding it would make `export FOO; echo $F<Tab>` offer less
					// than `set` lists.
					into.emplace_back(row.name);
				}
				break;
			case name_domain::path_directory:
				split_path(into);
				break;
		}
	}

private:
	// ADR-0009 in one line, and the reason the rest of this file may borrow.
	//
	// NOT A GUARD ON THE CALLER'S THREAD - it says nothing about WHO is reading,
	// only that the one writer is not writing. That is the whole invariant: the
	// highlighter reads on the shell thread between slots, the completer reads on
	// the loop thread while the loop is not blocked on an execution, and either
	// is safe exactly when this is false.
	void assert_readable() const noexcept {
		// The discard is for RELEASE, where `LESH_ASSERT` expands to nothing and
		// the flag is a member nobody reads. Keeping the member in both builds
		// rather than compiling it out keeps this class one layout, which is what
		// lets the same header be compiled into `lesh` and into `lesh_tests`.
		(void)_writing;
		LESH_ASSERT(_writing == nullptr || !_writing->writing());
	}

	// `$PATH`, cut on `:`, with POSIX 2.6's rule applied HERE and nowhere else:
	// an empty element means the current directory. An UNSET `PATH` yields no
	// directories at all, which is not the same as an empty one - an empty
	// `PATH` has exactly one element and that element is `.`, which is why the
	// distinction survives this far rather than being flattened at `lookup`.
	void split_path(std::vector<std::string>& into) const {
		std::string_view value;
		if (!path(value))
			return;
		for (std::size_t at = 0;;) {
			const std::size_t colon = value.find(':', at);
			const std::string_view element = colon == std::string_view::npos
				? value.substr(at)
				: value.substr(at, colon - at);
			into.emplace_back(element.empty() ? std::string_view{"."} : element);
			if (colon == std::string_view::npos)
				break;
			at = colon + 1;
		}
	}

	const runtime::shell_state* _state;
	const shell_writing_flag* _writing;
};

} // namespace lesh::leshper
