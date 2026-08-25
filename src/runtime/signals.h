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

// One PAST the highest signal number the platform can deliver, taken from the C
// library rather than written down: 32 on macOS, 65 on glibc, where the real-time
// signals live above 32. It was the literal 32 that made SIGRTMIN..SIGRTMAX
// untrappable on Linux no matter what the name table said (issue #38).
//
// It must stay a compile-time CONSTANT, because it sizes g_pending, which the
// installed handler writes. A signal handler may not allocate, so that array can
// never become a container and can never be sized at run time. Every state added
// per signal from here on inherits the same rule.
#if defined(NSIG)
inline constexpr int kMaxSignal = NSIG;
#elif defined(_NSIG)
inline constexpr int kMaxSignal = _NSIG;
#else
// No supported platform lacks NSIG; 65 spans glibc's real-time range so that a
// missing macro can never be the reason a signal is unreachable.
inline constexpr int kMaxSignal = 65;
#endif

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
	// The recorded command. Non-empty even after a subshell reset the disposition to
	// default, because `trap` must still REPORT what it inherited - see
	// reset_for_subshell.
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

	// POSIX XCU 2.11, the other half: a signal that was ALREADY IGNORED when a
	// NON-INTERACTIVE shell was invoked cannot be trapped or reset, for the whole
	// life of that shell. True for exactly those signals, and set_trap, set_ignore
	// and reset are all no-ops for them (issue #37).
	//
	// This is a THIRD axis, independent of the two an entry already carries. The
	// DISPOSITION is what this shell has asked for, and a subshell changes it. The
	// recorded COMMAND TEXT outlives a disposition that has reverted. This fact is
	// about how the PROCESS started: it is fixed in the constructor and nothing can
	// change it afterwards, which is why it is not a fourth disposition value.
	[[nodiscard]] bool cannot_be_trapped(int signo) const;

	// The rule above is scoped to a non-interactive shell, so this mirrors
	// shell_state's flag. A mirror rather than a parameter on set_trap, because the
	// constructor captures the entry dispositions BEFORE main() has decided whether
	// the shell is interactive; and not a back-pointer, because signal_state is a
	// member of the very state it would point at.
	//
	// It also turns the interactive DEFAULTS on, which is why it is no longer a
	// one-liner: the two are separate axes and the difference is load bearing, see
	// drop_interactive_defaults.
	void set_interactive(bool v);

	// POSIX XCU 2.11, the third part: an interactive shell IGNORES SIGQUIT and
	// SIGTERM and CATCHES SIGINT, so that a keyboard interrupt or a stray `kill`
	// abandons the command being run instead of ending the session. It applies only
	// where no trap has been set - an explicit `trap` wins - and only to those three
	// signals, which is why sighup5/6-p.tst and sigurg5/6-p.tst are the canaries
	// that say the rule did not spread (issue #52).
	//
	// A SEPARATE AXIS from _interactive, and not a term in cannot_be_trapped's
	// predicate, because a subshell keeps one and loses the other. A subshell of an
	// interactive shell is still allowed to trap a signal ignored on entry - #37's
	// rule stays lifted, and sigint6-p.tst's `ignore -> clear` subshell case needs
	// it - while the DEFAULT ACTION belongs to the process that reads commands and
	// has a prompt to return to. sigint5-p.tst requires exactly that split: the same
	// `kill` spares the shell in `main` context and kills it in `subshell`.
	void drop_interactive_defaults();
	// Puts them back. The one caller is `exec` FAILING on an interactive shell: it
	// reports and carries on, so the shell it carries on as must still be the one
	// POSIX describes.
	void restore_interactive_defaults();

	// True when `signo` has arrived and the interactive default is all there is to
	// answer it, so the executor must abandon the command it is running rather than
	// run a trap or die. Only SIGINT ever gets here: SIGQUIT and SIGTERM are
	// SIG_IGN under the same rule, so they never arrive at all.
	[[nodiscard]] bool interrupts_command(int signo) const;

	// Name to number, accepting `INT`, `SIGINT`, `2`, and `EXIT`. Returns -1 when
	// the name is not recognised - and "not recognised" is a real answer, not a
	// gap: the conformance suite probes `trap : RTMAX RTMIN` and SKIPS its
	// real-time cases when the shell rejects them, so claiming a signal the
	// platform does not have would turn skips into wrong passes.
	[[nodiscard]] static int signal_number(std::string_view name);
	// Number to name, without the SIG prefix. Empty for a number the platform has
	// no name for. The table behind both is built from what the PLATFORM defines
	// rather than from a list typed by hand - see signals.cpp, which is where the
	// bug was that made SIGURG unreachable.
	[[nodiscard]] static std::string_view signal_name(int signo);

private:
	void install(int signo);

	struct entry {
		disposition how = disposition::default_action;
		std::string command;
		// Ignored when this PROCESS started, read from the kernel in the constructor.
		// A plain bool and not volatile sig_atomic_t: nothing in signal context ever
		// looks at it - the readers are this class and the `trap` builtin, both of
		// which run between commands like every other part of the shell.
		bool ignored_on_entry = false;
		// The SIG_IGN this process was GIVEN is still what the kernel holds, because
		// nothing this shell installed has replaced it. Starts equal to
		// ignored_on_entry and only ever goes false, in install().
		//
		// It is not a second spelling of that flag: `trap - INT` in an INTERACTIVE
		// shell whose SIGINT arrived ignored genuinely reverts to the default action
		// (#37 lifts its rule for an interactive shell), so a subshell of that shell
		// must die - while a subshell of one that never touched SIGINT must keep the
		// SIG_IGN it inherited and survive. `how` is default_action in both cases and
		// cannot tell them apart; this is what does.
		bool inherited_ignore_stands = false;
	};
	// True when the interactive defaults are installed: see
	// drop_interactive_defaults for why this is not _interactive.
	void set_interactive_defaults(bool on);
	// True when `signo` is one of the three POSIX names and the interactive default
	// is in force for this process. Says nothing about whether a trap has been set,
	// which is the caller's business.
	[[nodiscard]] bool interactive_default(int signo) const;

	std::vector<entry> _entries;
	bool _interactive = false;
	bool _interactive_defaults = false;
};

// Set by the installed handler, checked by the executor. Free functions with
// internal linkage in the .cpp would be cleaner, but a signal handler cannot
// reach a member, so this is the one place a global is the right answer.
extern volatile sig_atomic_t g_pending[kMaxSignal];

} // namespace lesh::runtime
