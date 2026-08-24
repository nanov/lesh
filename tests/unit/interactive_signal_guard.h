#pragma once

// Signal-disposition guards for the unit tests.
//
// signal_state's constructor READS the process's real dispositions (issue #37)
// and set_interactive() now WRITES three of them (issue #52), so a test that
// changes one and does not put it back changes what the next test's shell
// believes it inherited - and by #37's own rule that silently turns a later
// `trap` into a no-op with nothing to say why. Shared rather than copied,
// because four test files need it and three of them are not about signals.

#include "runtime/signals.h"

#include <array>
#include <csignal>

namespace lesh::testing {

// Restores one signal's real disposition when it goes out of scope.
//
// Needed because signal_state's constructor READS the process's current
// dispositions (issue #37), so a test that leaves one changed changes what the
// next test's constructor believes the process started with. set_ignore installs
// SIG_IGN for real, and a later set_trap on the same signal would then be a
// no-op - by the very rule these tests assert - with nothing to say why.
class saved_disposition {
public:
	explicit saved_disposition(int signo) : _signo(signo) {
		sigaction(_signo, nullptr, &_old);
	}
	~saved_disposition() { sigaction(_signo, &_old, nullptr); }
	saved_disposition(const saved_disposition&) = delete;
	saved_disposition& operator=(const saved_disposition&) = delete;

	// Puts the signal in the state a shell INHERITS when its parent ignored it.
	void ignore() const {
		struct sigaction sa{};
		sa.sa_handler = SIG_IGN;
		sigemptyset(&sa.sa_mask);
		sigaction(_signo, &sa, nullptr);
	}

	// And the other state it can inherit, written down rather than assumed: SIG_DFL
	// and SIG_IGN are the only two that survive an execve, so these two calls span
	// everything a shell can start with.
	void default_action() const {
		struct sigaction sa{};
		sa.sa_handler = SIG_DFL;
		sigemptyset(&sa.sa_mask);
		sigaction(_signo, &sa, nullptr);
	}

	[[nodiscard]] void* installed() const {
		struct sigaction current{};
		sigaction(_signo, nullptr, &current);
		return reinterpret_cast<void*>(current.sa_handler);
	}

private:
	int _signo;
	struct sigaction _old{};
};

// Restores all three signals an interactive shell's DEFAULTS touch, and starts
// them at SIG_DFL.
//
// One guard per signal was not enough and the difference was a real failure: a
// test that guards SIGINT alone and then calls set_interactive(true) leaves
// SIGQUIT and SIGTERM as SIG_IGN for the rest of the binary - and the next
// signal_state constructor reads that as "ignored on entry", which by #37's rule
// silently changes what a later trap does. Forcing SIG_DFL on the way in is the
// other half: the test binary's own inherited dispositions are not this suite's
// to assume (issue #52).
class interactive_disposition_guard {
public:
	interactive_disposition_guard() {
		for (const saved_disposition& g : _guards)
			g.default_action();
	}
	// The three, in the order POSIX names them.
	[[nodiscard]] const saved_disposition& interrupt() const { return _guards[0]; }
	[[nodiscard]] const saved_disposition& quit() const { return _guards[1]; }
	[[nodiscard]] const saved_disposition& terminate() const { return _guards[2]; }

private:
	std::array<saved_disposition, 3> _guards{
		saved_disposition{SIGINT}, saved_disposition{SIGQUIT}, saved_disposition{SIGTERM}};
};

} // namespace lesh::testing
