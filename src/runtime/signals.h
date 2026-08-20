#pragma once

#include <csignal>
#include <string>
#include <string_view>
#include <vector>

namespace lesh::runtime {

// Signal dispositions and pending-signal tracking. See issue #33.
//
// THE CENTRAL CONSTRAINT: a signal handler cannot run shell code. It arrives
// asynchronously - possibly inside malloc, inside printf, anywhere - and almost
// nothing is async-signal-safe. So the installed handler does exactly one thing:
// sets a flag. The EXECUTOR checks those flags between commands and runs the
// trap body there, where it is safe.
//
// That is also what POSIX requires: a trap fires after the current command
// completes, not during it. The safety constraint and the specification happen
// to agree, which is convenient but not a coincidence - the specification was
// written by people who knew this.

// POSIX's `EXIT` trap is signal number 0, which is not a real signal.
inline constexpr int kExitTrap = 0;
inline constexpr int kMaxSignal = 32;

enum class disposition {
	default_action,  // whatever the signal normally does
	ignore,          // trap '' SIG
	handler,         // trap 'command' SIG
};

class signal_state {
public:
	signal_state();

	// Records `command` as the trap for `signo`. An empty command means ignore;
	// resetting to default is reset().
	void set_trap(int signo, std::string command);
	void set_ignore(int signo);
	void reset(int signo);

	[[nodiscard]] disposition disposition_of(int signo) const;
	[[nodiscard]] std::string_view trap_command(int signo) const;

	// Signals that arrived since the last check. Consumed by the executor between
	// commands; returns false when there is nothing pending, which is the common
	// case and must stay cheap.
	[[nodiscard]] bool take_pending(int& signo);
	[[nodiscard]] bool any_pending() const;

	// A subshell resets traps to default EXCEPT those set to ignore, which stay
	// ignored. That asymmetry is easy to miss and is exactly what several
	// conformance cases test.
	void reset_for_subshell();

	// POSIX XCU 2.11: when job control is DISABLED, a command started
	// asynchronously has SIGINT and SIGQUIT set to ignore. Without job control
	// there is no other way to keep a keyboard interrupt aimed at the foreground
	// job from also killing everything running in the background.
	//
	// It applies at subshell entry, so a `trap` inside the async list still wins -
	// which is the order the conformance suite checks.
	void ignore_interrupts_for_async();

	// Name to number, accepting `INT`, `SIGINT`, `2`, and `EXIT`. Returns -1 when
	// the name is not recognised.
	[[nodiscard]] static int signal_number(std::string_view name);
	[[nodiscard]] static std::string_view signal_name(int signo);

private:
	void install(int signo);

	struct entry {
		disposition how = disposition::default_action;
		std::string command;
	};
	std::vector<entry> _entries;
};

// Set by the installed handler, checked by the executor. Free functions with
// internal linkage in the .cpp would be cleaner, but a signal handler cannot
// reach a member, so this is the one place a global is the right answer.
extern volatile sig_atomic_t g_pending[kMaxSignal];

} // namespace lesh::runtime
