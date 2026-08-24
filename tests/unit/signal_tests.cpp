#include "runtime/signals.h"

#include "interactive_signal_guard.h"

#include <gtest/gtest.h>

#include <string>

using namespace lesh::runtime;
using lesh::testing::interactive_disposition_guard;
using lesh::testing::saved_disposition;


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

TEST(Signals, ANumberPastTheCeilingIsRefusedRatherThanOverflowed) {
	// ISSUE #62. The accumulator was an unbounded `int`, so
	// `trap - 99999999999999999999` overflowed it - undefined behaviour, and an
	// abort under the debug preset's UBSan - on the way to an answer the ceiling
	// below already had. kMaxSignal bounds the whole valid set and the accumulator
	// only grows, so a number that has passed the ceiling can never come back under
	// it and the loop can stop there.
	EXPECT_EQ(signal_state::signal_number("99999999999999999999"), -1);
	EXPECT_EQ(signal_state::signal_number("9223372036854775808"), -1);
	EXPECT_EQ(signal_state::signal_number(std::to_string(kMaxSignal)), -1);

	// THE WRAP WAS NOT MERELY UNDEFINED, it landed on real signals. 2^32 wraps an
	// int to 0, which is kExitTrap, so `trap - 4294967296` used to CLEAR THE EXIT
	// TRAP; 2^32 + 2 wraps to SIGINT. A saturating answer would have had the same
	// defect in a tidier form - clamping to the ceiling names whatever signal sits
	// there - which is why this site refuses instead of clamping.
	EXPECT_EQ(signal_state::signal_number("4294967296"), -1)
		<< "wrapping an int landed on the EXIT condition";
	EXPECT_EQ(signal_state::signal_number("4294967298"), -1)
		<< "wrapping an int landed on SIGINT";

	// The ceiling is the only thing that moved: every number below it still
	// resolves, leading zeros included.
	EXPECT_EQ(signal_state::signal_number(std::to_string(kMaxSignal - 1)), kMaxSignal - 1);
	EXPECT_EQ(signal_state::signal_number("0000000002"), 2);
	EXPECT_EQ(signal_state::signal_number("00"), kExitTrap);
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

// ISSUE #52. POSIX XCU 2.11's third rule: an interactive shell IGNORES SIGQUIT and
// SIGTERM and CATCHES SIGINT, so a keyboard interrupt abandons the command being
// run instead of ending the session.
//
// Every case below guards the real dispositions, because set_interactive now
// INSTALLS three of them for real - and SIGTERM left as SIG_IGN would make the
// test binary itself unkillable for the rest of the run.

TEST(Signals, AnInteractiveShellIgnoresSIGQUITAndSIGTERM) {
	const interactive_disposition_guard guard;
	signal_state s;
	s.set_interactive(true);

	EXPECT_EQ(guard.quit().installed(), reinterpret_cast<void*>(SIG_IGN));
	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_IGN));
	// The KERNEL disposition changed and the shell's own view did not: `trap` must
	// keep reporting the default action, because that is what the user asked for.
	// Recording it as disposition::ignore would have made `trap` list an ignore
	// nobody set, and an explicit `trap` would then have had to fight it out.
	EXPECT_EQ(s.disposition_of(SIGQUIT), disposition::default_action);
	EXPECT_EQ(s.trap_command(SIGTERM), "");
}

TEST(Signals, AnInteractiveShellCatchesSIGINTRatherThanIgnoringIt) {
	const interactive_disposition_guard guard;
	signal_state s;
	s.set_interactive(true);

	// SIG_IGN would have been the easy answer and it is the WRONG one: POSIX has the
	// shell abandon the current command and prompt again, so the interrupt has to be
	// delivered somewhere the executor can see it. A shell that merely survived would
	// spin in `while :; do :; done` forever, where bash abandons the loop.
	EXPECT_NE(guard.interrupt().installed(), reinterpret_cast<void*>(SIG_IGN));
	EXPECT_NE(guard.interrupt().installed(), reinterpret_cast<void*>(SIG_DFL));
	EXPECT_TRUE(s.interrupts_command(SIGINT));
	// And only SIGINT: the other two never arrive, so nothing may ask the executor
	// to abandon a command on their behalf.
	EXPECT_FALSE(s.interrupts_command(SIGQUIT));
	EXPECT_FALSE(s.interrupts_command(SIGTERM));
}

TEST(Signals, TheInteractiveDefaultReachesOnlyTheThreeSignalsPOSIXNames) {
	// The over-reach canary in unit form. sighup5/6-p.tst and sigurg5/6-p.tst were
	// already 180/180 before this rule existed, because SIGHUP does terminate an
	// interactive shell and SIGURG is discarded whatever the mode. A rule that
	// sprayed across all signals would have cost those four files.
	const interactive_disposition_guard touched;
	const saved_disposition hup{SIGHUP};
	const saved_disposition urg{SIGURG};
	const saved_disposition usr1{SIGUSR1};
	// What the process held BEFORE, rather than SIG_DFL written down: the assertion
	// is that set_interactive did not touch these, and comparing against a constant
	// would instead assert whatever the test binary happens to start with.
	void* const was_hup = hup.installed();
	void* const was_urg = urg.installed();
	void* const was_usr1 = usr1.installed();
	signal_state s;
	s.set_interactive(true);

	EXPECT_EQ(hup.installed(), was_hup);
	EXPECT_EQ(urg.installed(), was_urg);
	EXPECT_EQ(usr1.installed(), was_usr1);
	EXPECT_FALSE(s.interrupts_command(SIGHUP));
}

TEST(Signals, ANonInteractiveShellIsLeftAtTheDefaultAction) {
	const interactive_disposition_guard guard;
	signal_state s;
	s.set_interactive(false);

	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_DFL));
	EXPECT_FALSE(s.interrupts_command(SIGINT));
}

TEST(Signals, AnExplicitTrapWinsOverTheInteractiveDefault) {
	// The suite's `command -> command` cases passed before this rule existed and had
	// to keep passing: a trap the user set must run, and an interrupt must not
	// abandon the command out from under it.
	const interactive_disposition_guard guard;
	signal_state s;
	s.set_interactive(true);

	s.set_trap(SIGINT, "echo trapped");
	EXPECT_EQ(s.disposition_of(SIGINT), disposition::handler);
	EXPECT_FALSE(s.interrupts_command(SIGINT))
		<< "the trap body answers the signal, not the interactive default";

	// `trap '' INT` on an interactive shell is a real ignore, not the default one,
	// and `trap` must be able to say so.
	s.set_ignore(SIGINT);
	EXPECT_EQ(s.disposition_of(SIGINT), disposition::ignore);
	EXPECT_EQ(guard.interrupt().installed(), reinterpret_cast<void*>(SIG_IGN));

	// And `trap - INT` goes back to the interactive default rather than to SIG_DFL,
	// which is the whole of sigint5-p.tst's `clear -> clear` case.
	s.reset(SIGINT);
	EXPECT_EQ(s.disposition_of(SIGINT), disposition::default_action);
	EXPECT_TRUE(s.interrupts_command(SIGINT));
}

TEST(Signals, ASubshellTakesTheDefaultActionBackButKeepsTheRightToTrap) {
	// The split that makes this a SEPARATE axis from _interactive. A subshell of an
	// interactive shell is not the process with a prompt to return to, so SIGTERM
	// kills it again - while it is still an interactive SHELL, so #37's rule stays
	// lifted for it. sigint5-p.tst needs both halves in the one file: the same
	// `kill` spares the shell in `main` context and kills it in `subshell`.
	const interactive_disposition_guard guard;
	const saved_disposition usr1{SIGUSR1};
	usr1.ignore();
	signal_state s;
	s.set_interactive(true);
	ASSERT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_IGN));

	s.reset_for_subshell();

	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_DFL));
	EXPECT_FALSE(s.interrupts_command(SIGINT));
	EXPECT_FALSE(s.cannot_be_trapped(SIGUSR1))
		<< "#37's interactive exemption is a different axis and must survive";
}

TEST(Signals, AnInheritedIgnoreIsNotOverwrittenByTheInteractiveDefault) {
	// The case that would have cost more than the rule bought. A signal IGNORED on
	// entry is already spared, so the interactive default has nothing to add - and
	// must not overwrite it, because SIG_IGN is the one disposition a child inherits
	// across fork and execve. Writing SIG_DFL over it when the subshell drops the
	// defaults again would kill the subshell and the child shell in sigterm6-p.tst's
	// `keep -> keep`, which #37 made survive.
	const interactive_disposition_guard guard;
	guard.terminate().ignore();
	signal_state s;
	s.set_interactive(true);
	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_IGN));

	s.reset_for_subshell();
	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_IGN))
		<< "the inherited ignore has to reach the subshell";
}

TEST(Signals, AnExplicitResetGivesUpTheInheritedIgnoreForGood) {
	// The other half of the pair above, and why one bool cannot serve for both.
	// `trap - TERM` on an INTERACTIVE shell whose SIGTERM arrived ignored genuinely
	// reverts to the default action - #37 lifts its rule for an interactive shell -
	// so a subshell of THAT shell must die where a subshell of an untouched one
	// survives. disposition_of() is default_action in both cases and cannot tell
	// them apart.
	const interactive_disposition_guard guard;
	guard.terminate().ignore();
	signal_state s;
	s.set_interactive(true);

	s.reset(SIGTERM);
	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_IGN))
		<< "the interactive default still spares the shell itself";

	s.reset_for_subshell();
	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_DFL));
}

TEST(Signals, DroppingTheDefaultsTwiceIsHarmlessAndRestoringPutsThemBack) {
	// `exec` is the one caller that can drop them and then need them back: it
	// reports and carries on when there is nothing to become, and the shell it
	// carries on as must still be the one POSIX describes.
	const interactive_disposition_guard guard;
	signal_state s;
	s.set_interactive(true);

	s.drop_interactive_defaults();
	s.drop_interactive_defaults();
	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_DFL));

	s.restore_interactive_defaults();
	EXPECT_EQ(guard.terminate().installed(), reinterpret_cast<void*>(SIG_IGN));
}
