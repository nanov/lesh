#include "runtime/cooperation.h"

#include <sys/wait.h>

namespace lesh::runtime {
namespace {

// The empty body, and the only implementation of `cooperation` that ships in
// this ticket. Local to this translation unit on purpose - see the note on
// `noop_cooperation`.
class noop_cooperation_impl final : public cooperation {
public:
	void on_command_boundary() noexcept override {}

	// AND THIS ONE IS NOT EMPTY: cooperating with nobody about a child is
	// `::waitpid` itself. The verb exists so that an interactive host can park a
	// fiber instead of blocking a thread; a shell with no host blocks, which is
	// what every shell in this tree did before #208 and what every non-interactive
	// one still does, through one indirect call into the same syscall.
	pid_t wait_child(pid_t pid, int flags, int* status) noexcept override {
		return ::waitpid(pid, status, flags);
	}

	// AND THIS ONE *IS* EMPTY (#209). Cooperating with nobody about a descriptor
	// is having nothing to say before the read; the `::read` that follows blocks,
	// which is what every shell in this tree did before #209 and what every
	// non-interactive one still does. One indirect call to a `return`.
	void await_readable(int) noexcept override {}
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
