#include "leshnici/leshnici.h"

#include "leshnici/coreutils/builtins.h"
#include "runtime/builtins.h"
#include "runtime/diagnostic.h"
#include "runtime/shell_state.h"

#include <array>
#include <span>

namespace lesh::leshnici {

namespace {

// THE EXTENSION TABLE. `constexpr` with static storage duration, which is what
// makes handing the shell a borrowed view of it correct: there is no owner to
// free and no lifetime to outlive, so ADR-0007's rule is answered by there being
// nothing to answer.
//
// ONE LIST, NOT TWO. The runtime's core set is a registry plus a handler table
// cross-checked by a `static_assert` (#35), because there the two carry
// different facts - a kind, a home, a `command -v` spelling. Here every row
// answers the same way: regular, implemented in this library, reported by name.
// A second table would be a second thing to keep in step for no fact it could
// hold, so the name and the function are one row and `coreutils/builtins.h` is
// the human-readable index.
//
// ORDER IS ALPHABETICAL AND MEANS NOTHING ELSE. Lookup is a linear scan over
// four rows, which is cheaper than any structure that would make the order
// matter; when this list is long enough for that to be false it wants a sorted
// table and a binary search, not a rearrangement.
constexpr std::array<runtime::extension_builtin, 4> kExtensionBuiltins = {{
	{"cat", &coreutils::builtin_cat},
	{"head", &coreutils::builtin_head},
	{"ls", &coreutils::builtin_ls},
	{"tail", &coreutils::builtin_tail},
}};

} // namespace

void install_builtins(runtime::shell_state& state) {
	// The refusal is `set_extension_builtins`': it knows `kBuiltinRegistry` and
	// this side does not, which is the whole reason the check lives there. It has
	// already reported by the time it answers false, so there is nothing to add -
	// and nothing to abort, either. A shell that could not install the extension
	// set is a POSIX shell, which is a working shell.
	(void)state.set_extension_builtins(std::span<const runtime::extension_builtin>{
		kExtensionBuiltins});
}

} // namespace lesh::leshnici
