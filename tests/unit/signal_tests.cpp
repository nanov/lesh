#include "runtime/signals.h"

#include <gtest/gtest.h>

using namespace lesh::runtime;

namespace {

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

	[[nodiscard]] void* installed() const {
		struct sigaction current{};
		sigaction(_signo, nullptr, &current);
		return reinterpret_cast<void*>(current.sa_handler);
	}

private:
	int _signo;
	struct sigaction _old{};
};

} // namespace

TEST(Signals, NamesResolveWithAndWithoutPrefix) {
	EXPECT_EQ(signal_state::signal_number("INT"), SIGINT);
	EXPECT_EQ(signal_state::signal_number("SIGINT"), SIGINT);
	EXPECT_EQ(signal_state::signal_number("TERM"), SIGTERM);
	EXPECT_EQ(signal_state::signal_number("EXIT"), kExitTrap);
}

TEST(Signals, NumbersAreAccepted) {
	// `trap - 2` is legal, so a bare number must resolve.
	EXPECT_EQ(signal_state::signal_number("2"), 2);
	EXPECT_EQ(signal_state::signal_number("0"), kExitTrap);
}

TEST(Signals, UnknownNamesAreRejectedRatherThanGuessed) {
	EXPECT_EQ(signal_state::signal_number("NOSUCH"), -1);
	EXPECT_EQ(signal_state::signal_number(""), -1);
	EXPECT_EQ(signal_state::signal_number("12x"), -1);
}

TEST(Signals, EverySignalThePlatformHasIsReachable) {
	// ISSUE #38. The table was a hand-typed array of 20 names and everything
	// outside it was unreachable, not merely unnamed: `kill -s URG` and
	// `trap '' URG` both said "bad signal", which cost 336 assertions in
	// sigurg1/2-p.tst. This asserts the property, not a list - a name is required
	// for every number the platform can actually deliver, so a longer hand-typed
	// list cannot pass it by accident.
	for (int signo = 1; signo < kMaxSignal; ++signo) {
#if defined(SIGRTMIN)
		// glibc reserves a few numbers below SIGRTMIN for its own threading use and
		// gives them no name at all; nothing can send them, so nothing needs one.
		if (signo >= 32 && signo < SIGRTMIN)
			continue;
#endif
		const std::string_view name = signal_state::signal_name(signo);
		ASSERT_FALSE(name.empty()) << "signal " << signo << " has no name";
		EXPECT_EQ(signal_state::signal_number(name), signo)
			<< "the name of signal " << signo << " does not resolve back to it";
	}
}

TEST(Signals, TheNamesIssue38WasAboutResolve) {
	// The exact names dash lists and lesh did not. SIGURG is the one the ticket is
	// named for; the rest were missing the same way and for the same reason.
	EXPECT_EQ(signal_state::signal_number("URG"), SIGURG);
	EXPECT_EQ(signal_state::signal_number("SIGURG"), SIGURG);
	EXPECT_EQ(signal_state::signal_number("BUS"), SIGBUS);
	EXPECT_EQ(signal_state::signal_number("SYS"), SIGSYS);
	EXPECT_EQ(signal_state::signal_number("XCPU"), SIGXCPU);
	EXPECT_EQ(signal_state::signal_number("XFSZ"), SIGXFSZ);
	EXPECT_EQ(signal_state::signal_number("VTALRM"), SIGVTALRM);
	EXPECT_EQ(signal_state::signal_number("PROF"), SIGPROF);
	EXPECT_EQ(signal_state::signal_number("WINCH"), SIGWINCH);
	EXPECT_EQ(signal_state::signal_number("IO"), SIGIO);
}

TEST(Signals, TheStopSignalsAreNamedEvenThoughJobControlIsNot) {
	// ADR-0001 puts job control out of scope and tools/conformance.py excludes the
	// files that test it. NAMING these is still required: `kill -l` must list them
	// and `trap` must accept them, and kill3-p.tst sends STOP to the shell and
	// expects the kernel's default action, no job control involved.
	EXPECT_EQ(signal_state::signal_number("STOP"), SIGSTOP);
	EXPECT_EQ(signal_state::signal_number("TSTP"), SIGTSTP);
	EXPECT_EQ(signal_state::signal_number("TTIN"), SIGTTIN);
	EXPECT_EQ(signal_state::signal_number("TTOU"), SIGTTOU);
	EXPECT_EQ(signal_state::signal_name(SIGSTOP), "STOP");
}

TEST(Signals, RealTimeSignalsExistOnlyWhereThePlatformHasThem) {
	// signal.sh probes `trap : RTMAX RTMIN` and SKIPS its real-time cases when the
	// shell rejects them, so "unknown" has to stay a real answer. Answering yes on
	// macOS would convert those skips into wrong passes.
#if defined(SIGRTMIN) && defined(SIGRTMAX)
	EXPECT_EQ(signal_state::signal_number("RTMIN"), SIGRTMIN);
	EXPECT_EQ(signal_state::signal_number("RTMAX"), SIGRTMAX);
	EXPECT_LT(SIGRTMAX, kMaxSignal) << "the pending array must cover RTMAX";
#else
	EXPECT_EQ(signal_state::signal_number("RTMIN"), -1);
	EXPECT_EQ(signal_state::signal_number("RTMAX"), -1);
#endif
}

TEST(Signals, AliasesResolveButAreNotThePrintedName) {
	// SIGABRT and SIGIOT are one number on every platform that has both. `kill -l`
	// has to name it once, and POSIX spells it ABRT.
	EXPECT_EQ(signal_state::signal_name(SIGABRT), "ABRT");
#ifdef SIGIOT
	EXPECT_EQ(signal_state::signal_number("IOT"), SIGIOT);
#endif
#ifdef SIGCLD
	EXPECT_EQ(signal_state::signal_number("CLD"), SIGCHLD);
#endif
}

TEST(Signals, ZeroIsTheExitConditionAndNotASignal) {
	// `trap` must be able to print it, so signal_name(0) has to answer - but it is
	// a condition, not something `kill` can send, which is why `kill -l` starts at
	// 1 and prints a bare `0` for the null signal instead of this name.
	EXPECT_EQ(signal_state::signal_name(kExitTrap), "EXIT");
}

TEST(Signals, PendingCoversTheWholeSignalRange) {
	// kMaxSignal sizes g_pending, which the handler writes. If the ceiling were
	// below the platform's highest signal, a trap on that signal would install a
	// handler that then dropped every delivery on the floor - which is what the
	// literal 32 did to SIGRTMIN..SIGRTMAX on glibc.
	signal_state s;
	int signo = 0;
	g_pending[kMaxSignal - 1] = 1;
	ASSERT_TRUE(s.take_pending(signo));
	EXPECT_EQ(signo, kMaxSignal - 1);
	EXPECT_FALSE(s.any_pending());
}

TEST(Signals, DispositionsRoundTrip) {
	const saved_disposition guard{SIGUSR1};
	signal_state s;
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action);

	s.set_trap(SIGUSR1, "echo hi");
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::handler);
	EXPECT_EQ(s.trap_command(SIGUSR1), "echo hi");

	s.set_ignore(SIGUSR1);
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::ignore);

	s.reset(SIGUSR1);
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action);
}

TEST(Signals, SubshellResetsHandlersButKeepsIgnores) {
	// POSIX's asymmetry, and the thing several conformance cases test: a subshell
	// resets traps to default EXCEPT those set to ignore, so `trap '' INT`
	// protects a whole subtree while a handler belongs to the shell that set it.
	const saved_disposition usr1{SIGUSR1};
	const saved_disposition usr2{SIGUSR2};
	signal_state s;
	s.set_trap(SIGUSR1, "echo handler");
	s.set_ignore(SIGUSR2);

	s.reset_for_subshell();

	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action)
		<< "a handler must not survive into a subshell";
	EXPECT_EQ(s.disposition_of(SIGUSR2), disposition::ignore)
		<< "an ignore must survive";
}

TEST(Signals, PendingIsEmptyUntilSomethingArrives) {
	signal_state s;
	int signo = 0;
	EXPECT_FALSE(s.any_pending());
	EXPECT_FALSE(s.take_pending(signo));
}

TEST(Signals, PendingIsConsumedOnce) {
	signal_state s;
	// Simulating what the handler does - the only thing it is allowed to do.
	g_pending[SIGUSR1] = 1;

	EXPECT_TRUE(s.any_pending());
	int signo = 0;
	ASSERT_TRUE(s.take_pending(signo));
	EXPECT_EQ(signo, SIGUSR1);

	EXPECT_FALSE(s.take_pending(signo)) << "taking must clear the flag";
	EXPECT_FALSE(s.any_pending());
}

TEST(Signals, ASignalIgnoredOnEntryCannotBeTrappedOrReset) {
	// ISSUE #37, POSIX XCU 2.11. This single rule is the whole difference between
	// sigint1-p.tst at 177/180 and sigint2-p.tst at 74/180: the sig*2 files are the
	// sig*1 files with the signal already ignored when the shell was invoked, and
	// all 180 of their cases require the shell to survive it.
	const saved_disposition guard{SIGUSR1};
	guard.ignore();
	signal_state s;  // its constructor is what reads "how the process started"

	ASSERT_TRUE(s.cannot_be_trapped(SIGUSR1));

	s.set_trap(SIGUSR1, "echo handler");
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action);
	EXPECT_EQ(s.trap_command(SIGUSR1), "")
		<< "nothing may be recorded, because nothing may be reported by `trap`";

	s.set_ignore(SIGUSR1);
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action);

	s.reset(SIGUSR1);
	// The assertion that actually matters. The kernel disposition is what a CHILD
	// shell inherits across fork and execve, and it is how that child discovers the
	// same fact about itself - so if reset() had reached sigaction here, every
	// `target=child` and `target=exec` case in the sig*2 files would still fail.
	EXPECT_EQ(guard.installed(), reinterpret_cast<void*>(SIG_IGN));
}

TEST(Signals, AnInteractiveShellMayStillTrapASignalIgnoredOnEntry) {
	// POSIX scopes the rule to a NON-interactive shell, and the suite holds it to
	// that: sigurg6-p.tst runs the testee as `sh -i` with SIGURG already ignored and
	// requires the trap to fire. dash and bash both apply the rule interactively
	// anyway; following them there would have traded the sig*2 files for the sig*6.
	const saved_disposition guard{SIGUSR1};
	guard.ignore();
	signal_state s;
	s.set_interactive(true);

	EXPECT_FALSE(s.cannot_be_trapped(SIGUSR1));
	s.set_trap(SIGUSR1, "echo handler");
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::handler);
}

TEST(Signals, ASignalAtItsDEFAULTDispositionIsUntouchedByTheRule) {
	// The regression canary in unit form. sigint1-p.tst - the same 180 combinations
	// with an inherited DEFAULT disposition - was already at 177/180, so a rule that
	// leaked into this case would have cost more than it bought.
	const saved_disposition guard{SIGUSR1};
	signal_state s;

	ASSERT_FALSE(s.cannot_be_trapped(SIGUSR1));
	s.set_trap(SIGUSR1, "echo handler");
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::handler);
	s.reset(SIGUSR1);
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action);
}

TEST(Signals, IgnoredOnEntrySurvivesASubshell) {
	// A subshell reverts handlers and keeps ignores; this fact is neither, and is
	// immutable for the life of the PROCESS rather than of the shell environment.
	const saved_disposition guard{SIGUSR1};
	guard.ignore();
	signal_state s;

	s.reset_for_subshell();

	EXPECT_TRUE(s.cannot_be_trapped(SIGUSR1));
	s.set_trap(SIGUSR1, "echo handler");
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action);
	EXPECT_EQ(guard.installed(), reinterpret_cast<void*>(SIG_IGN));
}

TEST(Signals, TheEXITConditionIsNeverLockedByTheRule) {
	// EXIT is condition 0, not a signal, so it has no inherited disposition to be
	// ignored - and `trap 'cleanup' EXIT` in a script whose parent ignored something
	// must keep working. An off-by-one that let 0 test as "ignored on entry" would
	// disable every EXIT trap in the shell.
	signal_state s;
	EXPECT_FALSE(s.cannot_be_trapped(kExitTrap));
	s.set_trap(kExitTrap, "echo bye");
	EXPECT_EQ(s.disposition_of(kExitTrap), disposition::handler);
}
