#include "runtime/cooperation.h"

namespace lesh::runtime {
namespace {

// The empty body, and the only implementation of `cooperation` that ships in
// this ticket. Local to this translation unit on purpose - see the note on
// `noop_cooperation`.
class noop_cooperation_impl final : public cooperation {
public:
	void on_command_boundary() noexcept override {}
};

} // namespace

cooperation& noop_cooperation() noexcept {
	// FUNCTION-LOCAL AND STATIC: no static initialisation order to reason about,
	// and nothing to free at exit, which is ADR-0007's "everything has an owner"
	// answered by there being nothing to own. The thread-safe-init guard costs an
	// atomic load per CALL to this function, and the only caller is
	// `shell_state`'s default member initialiser - once per shell state, never at
	// a command boundary.
	static noop_cooperation_impl instance;
	return instance;
}

} // namespace lesh::runtime
