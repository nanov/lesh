#include "runtime/signals.h"

#include "substrate/numeric.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

// Each platform publishes its own signal names somewhere different, and neither
// spelling is portable, so the probe is here rather than inline: glibc 2.32 added
// sigabbrev_np, and the BSDs have had sys_signame[] forever.
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 32)
#include <cstring>  // sigabbrev_np
#define LESH_LIBRARY_SIGNAL_NAMES 1
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
      defined(__OpenBSD__) || defined(__DragonFly__)
#define LESH_LIBRARY_SIGNAL_NAMES 2
#endif
#ifndef LESH_LIBRARY_SIGNAL_NAMES
// No library source. The roster below still covers everything such a platform
// defines; what is lost is only the safety net for a name nobody has listed.
#define LESH_LIBRARY_SIGNAL_NAMES 0
#endif

namespace lesh::runtime {

volatile sig_atomic_t g_pending[kMaxSignal] = {};

namespace {

// The ONLY thing that runs in signal context. Writing a sig_atomic_t is the one
// operation the standard guarantees here; anything else - allocating, printing,
// taking a lock - is undefined and will eventually deadlock or corrupt.
extern "C" void record_signal(int signo) {
	if (signo > 0 && signo < kMaxSignal)
		g_pending[signo] = 1;
}

struct name_entry {
	std::string name;
	int number;
	// False for a spelling that resolves as INPUT but is never printed back:
	// SIGIOT and SIGABRT are one number, and `kill -l` must name it once.
	bool canonical;
};

// The name the C library itself has for a signal number, uppercased and without
// the SIG prefix, or empty when it has none.
//
// This is the LAST resort in the table below, and the reason the table cannot go
// stale again: a signal this file has never heard of is still nameable, because
// the platform is asked directly.
std::string library_signal_name(int signo) {
#if LESH_LIBRARY_SIGNAL_NAMES == 1
	// glibc hands back "HUP" - already uppercase and unprefixed.
	if (const char* abbrev = ::sigabbrev_np(signo))
		return abbrev;
#elif LESH_LIBRARY_SIGNAL_NAMES == 2
	// The BSDs, macOS among them, publish sys_signame[] - lowercase, unprefixed.
	if (signo > 0 && signo < kMaxSignal)
		if (const char* lower = sys_signame[signo]) {
			std::string name{lower};
			for (char& c : name)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return name;
		}
#endif
	(void)signo;
	return {};
}

// ISSUE #38. This table used to be a hand-typed array of 20 names, and anything
// missing from it was not merely unnamed but UNREACHABLE: `kill -s URG $$` and
// `trap '' URG` both answered "bad signal", which cost 336 conformance
// assertions across sigurg1/2-p.tst. Typing a longer array would have set the
// same trap for the next name nobody thought of, so the table is now derived
// from three sources, in descending order of authority:
//
//   1. the #ifdef-guarded roster below - every signal name POSIX, XSI, or a live
//      BSD or Linux platform is known to define, compiled in only where the macro
//      actually exists. This fixes the SPELLING of the POSIX set, so `kill -l`
//      says ABRT and not IOT wherever those share a number;
//   2. SIGRTMIN..SIGRTMAX, which on glibc are function calls rather than
//      constants and so can only be enumerated at run time;
//   3. library_signal_name() for every number the first two left unnamed.
//
// What is deliberately NOT here is any attempt to invent a name for a signal the
// platform lacks. macOS has no real-time signals, and `trap : RTMAX RTMIN` must
// keep failing there, because signal.sh in the suite probes exactly that and
// skips its real-time cases on rejection.
//
// ON THE STOP SIGNALS. STOP, TSTP, TTIN and TTOU are named here and accepted by
// `trap` and `kill`, and that is the whole of it: their JOB-CONTROL behaviour is
// out of scope per ADR-0001, and tools/conformance.py excludes the files that
// test it. Naming them is not a promise to implement it. The stop is the
// kernel's default action, which is why kill3-p.tst's `kill -s STOP $$` passes
// with no job-control code anywhere: the suite's own subshell sends the SIGCONT.
std::vector<name_entry> build_signal_table() {
	std::vector<name_entry> table;
	table.reserve(kMaxSignal + 8);
	// Which numbers already have a printable name, so the later sources only fill
	// gaps instead of shadowing the POSIX spelling.
	std::vector<char> named(static_cast<size_t>(kMaxSignal), 0);

	// EXIT is condition 0, not a signal. It resolves as input for `trap EXIT` and
	// never appears in `kill -l`, which is why it is not canonical.
	table.push_back({"EXIT", kExitTrap, false});

	const auto primary = [&](std::string name, int number) {
		if (number <= 0 || number >= kMaxSignal || named[static_cast<size_t>(number)])
			return;
		named[static_cast<size_t>(number)] = 1;
		table.push_back({std::move(name), number, true});
	};
	const auto alias = [&](std::string name, int number) {
		if (number <= 0 || number >= kMaxSignal)
			return;
		table.push_back({std::move(name), number, false});
	};

	// The spelling and the macro come from one token, so `LESH_SIGNAL(URG)` cannot
	// register the name of one signal against the number of another.
#define LESH_SIGNAL(NAME) primary(#NAME, SIG##NAME)

	// POSIX.1-2024's required set first, in numeric order on every platform seen,
	// so `kill -l` reads the way dash's does.
#ifdef SIGHUP
	LESH_SIGNAL(HUP);
#endif
#ifdef SIGINT
	LESH_SIGNAL(INT);
#endif
#ifdef SIGQUIT
	LESH_SIGNAL(QUIT);
#endif
#ifdef SIGILL
	LESH_SIGNAL(ILL);
#endif
#ifdef SIGTRAP
	LESH_SIGNAL(TRAP);
#endif
#ifdef SIGABRT
	LESH_SIGNAL(ABRT);
#endif
#ifdef SIGEMT
	LESH_SIGNAL(EMT);
#endif
#ifdef SIGFPE
	LESH_SIGNAL(FPE);
#endif
#ifdef SIGKILL
	LESH_SIGNAL(KILL);
#endif
#ifdef SIGBUS
	LESH_SIGNAL(BUS);
#endif
#ifdef SIGSEGV
	LESH_SIGNAL(SEGV);
#endif
#ifdef SIGSYS
	LESH_SIGNAL(SYS);
#endif
#ifdef SIGPIPE
	LESH_SIGNAL(PIPE);
#endif
#ifdef SIGALRM
	LESH_SIGNAL(ALRM);
#endif
#ifdef SIGTERM
	LESH_SIGNAL(TERM);
#endif
#ifdef SIGURG
	LESH_SIGNAL(URG);
#endif
#ifdef SIGSTOP
	LESH_SIGNAL(STOP);
#endif
#ifdef SIGTSTP
	LESH_SIGNAL(TSTP);
#endif
#ifdef SIGCONT
	LESH_SIGNAL(CONT);
#endif
#ifdef SIGCHLD
	LESH_SIGNAL(CHLD);
#endif
#ifdef SIGTTIN
	LESH_SIGNAL(TTIN);
#endif
#ifdef SIGTTOU
	LESH_SIGNAL(TTOU);
#endif
#ifdef SIGIO
	LESH_SIGNAL(IO);
#endif
#ifdef SIGXCPU
	LESH_SIGNAL(XCPU);
#endif
#ifdef SIGXFSZ
	LESH_SIGNAL(XFSZ);
#endif
#ifdef SIGVTALRM
	LESH_SIGNAL(VTALRM);
#endif
#ifdef SIGPROF
	LESH_SIGNAL(PROF);
#endif
#ifdef SIGWINCH
	LESH_SIGNAL(WINCH);
#endif
#ifdef SIGINFO
	LESH_SIGNAL(INFO);
#endif
#ifdef SIGUSR1
	LESH_SIGNAL(USR1);
#endif
#ifdef SIGUSR2
	LESH_SIGNAL(USR2);
#endif
	// Platform-specific, past the POSIX set.
#ifdef SIGSTKFLT
	LESH_SIGNAL(STKFLT);
#endif
#ifdef SIGPWR
	LESH_SIGNAL(PWR);
#endif
#ifdef SIGTHR
	LESH_SIGNAL(THR);
#endif
#ifdef SIGLIBRT
	LESH_SIGNAL(LIBRT);
#endif

#undef LESH_SIGNAL

	// Historical spellings that share a number with one of the above. They must
	// resolve as input - `trap : SIGCLD` is real code on old systems - without
	// competing for the printed name.
#ifdef SIGIOT
	alias("IOT", SIGIOT);
#endif
#ifdef SIGCLD
	alias("CLD", SIGCLD);
#endif
#ifdef SIGPOLL
	alias("POLL", SIGPOLL);
#endif
#ifdef SIGUNUSED
	alias("UNUSED", SIGUNUSED);
#endif

#if defined(SIGRTMIN) && defined(SIGRTMAX)
	// Real-time signals. glibc spells SIGRTMIN as __libc_current_sigrtmin(), a
	// call, so these cannot live in a constant table at all - which is half of why
	// the table is built rather than declared. Both spellings of the interior
	// signals are accepted because that is what every shell that has them accepts.
	{
		const int rtmin = SIGRTMIN;
		const int rtmax = SIGRTMAX;
		primary("RTMIN", rtmin);
		primary("RTMAX", rtmax);
		for (int n = rtmin + 1; n < rtmax; ++n) {
			primary("RTMIN+" + std::to_string(n - rtmin), n);
			alias("RTMAX-" + std::to_string(rtmax - n), n);
		}
	}
#endif

	for (int n = 1; n < kMaxSignal; ++n)
		if (!named[static_cast<size_t>(n)])
			if (std::string name = library_signal_name(n); !name.empty())
				primary(std::move(name), n);

	return table;
}

// Built once, on first use, and never mutated afterwards. It is read only from
// ordinary shell context - never from the signal handler, which touches nothing
// but g_pending.
const std::vector<name_entry>& signal_table() {
	static const std::vector<name_entry> table = build_signal_table();
	return table;
}

} // namespace

signal_state::signal_state() : _entries(kMaxSignal) {
	// ISSUE #37. Which signals arrived already ignored has to be captured HERE, and
	// nowhere later: install() is the only thing that changes a disposition, and it
	// is reachable only through set_trap / set_ignore / reset, which need a running
	// `trap` - so this constructor is provably earlier than any of them. shell_state
	// holds this as a member, so it is also earlier than the executor exists.
	//
	// sigaction with a null ACT only reports, so this asks the kernel what it
	// inherited without disturbing anything. Signals the platform will not admit to
	// (the numbers glibc reserves for its threading library) fail the call and stay
	// false, which is right: nothing can send them.
	for (int signo = 1; signo < kMaxSignal; ++signo) {
		struct sigaction current{};
		if (sigaction(signo, nullptr, &current) == 0) {
			_entries[signo].ignored_on_entry = current.sa_handler == SIG_IGN;
			// Nothing has replaced it yet, by construction: install() is the only thing
			// that writes a disposition and it has not run.
			_entries[signo].inherited_ignore_stands = _entries[signo].ignored_on_entry;
		}
	}
}

// The seam between the two halves above, asserted rather than assumed: the policy
// table admits everything an `int` can hold, and this is the check that a platform
// whose NSIG outgrew that would not pass silently.
static_assert(kMaxSignal <= policy_for(numeric_site::trap_signal_number).high,
              "the signal policy range must cover every signal number NSIG allows");

int signal_state::signal_number(std::string_view name) {
	if (name.empty())
		return -1;

	// A bare number is accepted, which is how `trap - 2` works.
	if (name[0] >= '0' && name[0] <= '9') {
		// THE MECHANISM STOPS THE OVERFLOW; THE MEANING STAYS HERE (#63). The policy
		// table's range for this site is what an `int` can hold, because NSIG is a
		// platform constant the substrate layer must not include; kMaxSignal is the
		// real ceiling and it is applied right here, where the shell knows it.
		//
		// Both failures answer the same way, and that is the decision rather than a
		// shortcut: `trap - 99999999999999999999` gets exactly what `trap - 99`
		// already gets - `bad signal`, status 1, which is dash's `bad trap` at
		// status 1 - instead of overflowing an int on the way to it (#62).
		//
		// NOT SATURATED, unlike an arithmetic literal (#59): a clamped signal number
		// would name a REAL SIGNAL, so `trap '' 99999999999999999999` would install
		// a handler for whatever sits at the ceiling. There is no value here that
		// "too large" can safely become.
		const numeric_result parsed = parse_integer(name, numeric_site::trap_signal_number);
		if (parsed.status != numeric_parse::ok || parsed.value >= kMaxSignal)
			return -1;
		return static_cast<int>(parsed.value);
	}

	std::string_view bare = name;
	if (bare.size() > 3 && bare.substr(0, 3) == "SIG")
		bare.remove_prefix(3);
	for (const auto& e : signal_table())
		if (e.name == bare)
			return e.number;
	return -1;
}

std::string_view signal_state::signal_name(int signo) {
	for (const auto& e : signal_table())
		if (e.canonical && e.number == signo)
			return e.name;
	// Number 0 is the one non-signal condition, and `trap` has to print it.
	if (signo == kExitTrap)
		return "EXIT";
	return {};
}

void signal_state::install(int signo) {
	if (signo == kExitTrap || signo <= 0 || signo >= kMaxSignal)
		return;  // EXIT is not a real signal; there is nothing to install

	// Whatever the kernel was given on entry is gone from here on, and the
	// interactive default has to know: see entry::inherited_ignore_stands.
	_entries[signo].inherited_ignore_stands = false;

	// ISSUE #52. The interactive default is COMPUTED rather than stored, so `trap`
	// keeps reporting the disposition this shell actually asked for - which for
	// these three is the default action, and prints nothing. Storing it as a
	// disposition would have made `trap` list an ignore the user never set, and an
	// explicit `trap` would have had to fight it back out.
	disposition effective = _entries[signo].how;
	if (effective == disposition::default_action && interactive_default(signo))
		// SIGQUIT and SIGTERM are ignored outright; SIGINT is CAUGHT, because POSIX
		// has the shell abandon the current command and prompt again rather than
		// discard the interrupt. What "prompt again" means with a script on stdin is
		// tree_walking_executor::run_pending_traps.
		effective = signo == SIGINT ? disposition::handler : disposition::ignore;

	struct sigaction sa{};
	switch (effective) {
		case disposition::default_action: sa.sa_handler = SIG_DFL; break;
		case disposition::ignore:         sa.sa_handler = SIG_IGN; break;
		case disposition::handler:        sa.sa_handler = record_signal; break;
	}
	sigemptyset(&sa.sa_mask);
	// SA_RESTART so a signal with a handler does not turn every blocking read
	// into EINTR that callers would have to retry by hand.
	sa.sa_flags = SA_RESTART;
	sigaction(signo, &sa, nullptr);
}

bool signal_state::interactive_default(int signo) const {
	if (!_interactive_defaults)
		return false;
	// The three POSIX names and nothing else. SIGHUP does terminate an interactive
	// shell and SIGURG is discarded whatever the mode, which is why sighup5/6-p.tst
	// and sigurg5/6-p.tst were already 180/180 and are the canaries for over-reach.
	return signo == SIGINT || signo == SIGQUIT || signo == SIGTERM;
}

void signal_state::set_interactive_defaults(bool on) {
	if (_interactive_defaults == on)
		return;  // nothing installed, so nothing to undo
	_interactive_defaults = on;
	for (const int signo : {SIGINT, SIGQUIT, SIGTERM}) {
		// The one case that must be left alone. An inherited SIG_IGN this shell has
		// never replaced already spares it, so the interactive default has nothing to
		// add - and must not overwrite it, because SIG_IGN is the ONE disposition a
		// child inherits across fork and execve. Writing SIG_DFL here would kill the
		// subshell and the child shell in sigterm6-p.tst's `keep -> keep`, which
		// POSIX says survive, and it is #37's rule that makes them survive.
		if (_entries[signo].inherited_ignore_stands)
			continue;
		install(signo);
	}
}

void signal_state::set_interactive(bool v) {
	_interactive = v;
	set_interactive_defaults(v);
}

void signal_state::drop_interactive_defaults() { set_interactive_defaults(false); }

void signal_state::restore_interactive_defaults() { set_interactive_defaults(_interactive); }

bool signal_state::interrupts_command(int signo) const {
	// A trap WINS: the disposition has to still be the default action for the
	// interactive default to be what answers the signal. The suite's `command ->
	// command` cases assert exactly that, and they passed before this rule existed.
	return signo == SIGINT && _entries[signo].how == disposition::default_action &&
	       interactive_default(signo);
}

bool signal_state::cannot_be_trapped(int signo) const {
	if (signo <= 0 || signo >= kMaxSignal)
		return false;  // EXIT is a condition, not an inherited disposition
	// The rule is scoped to a NON-interactive shell, and that scoping is load
	// bearing rather than pedantry. dash and bash both apply the rule to an
	// interactive shell as well, and the conformance suite says they are wrong to:
	// sigurg6-p.tst runs the testee as `sh -i` with SIGURG already ignored and
	// requires `trap 'echo trapped' URG` to FIRE. Dropping this term would trade
	// the four sig*2 files for the four sig*6 ones.
	return _entries[signo].ignored_on_entry && !_interactive;
}

void signal_state::set_trap(int signo, std::string command) {
	if (signo < 0 || signo >= kMaxSignal || cannot_be_trapped(signo))
		return;
	_entries[signo].how = disposition::handler;
	_entries[signo].command = std::move(command);
	install(signo);
}

void signal_state::set_ignore(int signo) {
	// `trap '' SIG` asks for the state such a signal is already in, so refusing it
	// changes no behaviour - only what `trap` lists afterwards, which is the point:
	// the listing must not name a disposition this shell set, because it did not.
	if (signo < 0 || signo >= kMaxSignal || cannot_be_trapped(signo))
		return;
	_entries[signo].how = disposition::ignore;
	_entries[signo].command.clear();
	install(signo);
}

void signal_state::reset(int signo) {
	// The half of the rule that KILLS a shell when it is missing: `trap - INT` in a
	// script whose parent ignored SIGINT restored the default action, so the next
	// `kill -s INT $$` - or a keyboard interrupt aimed at the parent's whole
	// process group - terminated a shell POSIX says must survive.
	if (signo < 0 || signo >= kMaxSignal || cannot_be_trapped(signo))
		return;
	_entries[signo].how = disposition::default_action;
	_entries[signo].command.clear();
	install(signo);
}

disposition signal_state::disposition_of(int signo) const {
	if (signo < 0 || signo >= kMaxSignal)
		return disposition::default_action;
	return _entries[signo].how;
}

std::string_view signal_state::trap_command(int signo) const {
	if (signo < 0 || signo >= kMaxSignal)
		return {};
	return _entries[signo].command;
}

bool signal_state::any_pending() const {
	for (int i = 1; i < kMaxSignal; ++i)
		if (g_pending[i] != 0)
			return true;
	return false;
}

void signal_state::note_pending(int signo) {
	if (signo > 0 && signo < kMaxSignal)
		g_pending[signo] = 1;
}

bool signal_state::take_pending(int& signo) {
	for (int i = 1; i < kMaxSignal; ++i) {
		if (g_pending[i] != 0) {
			g_pending[i] = 0;
			signo = i;
			return true;
		}
	}
	return false;
}

void signal_state::reset_for_subshell() {
	// POSIX: a subshell resets traps to default, EXCEPT those set to ignore, which
	// remain ignored. The asymmetry exists so `trap '' INT` genuinely protects a
	// whole subtree, while a handler belongs to the shell that set it.
	//
	// The command TEXT is kept even though the disposition goes back to default.
	// Those are two different things, and conflating them made both halves wrong:
	// `trap` in a subshell must still report the traps it INHERITED - that is the
	// only portable way to save and restore them, `saved=$(trap)`, and POSIX.1-2024
	// requires it - while the actions themselves must no longer be taken. dash
	// reports nothing here and is behind the standard on it.
	//
	// A signal ignored on entry needs nothing here and gets nothing: set_trap can
	// never have made it a handler, so the loop below cannot reach it, and the
	// kernel disposition it arrived with is still SIG_IGN. That is also how a CHILD
	// shell learns the same fact - SIG_IGN survives execve, so the child's own
	// constructor rediscovers it rather than being told (issue #37).
	//
	// ISSUE #52, and it is the whole reason the interactive default is a separate
	// axis. A subshell is not the process that reads commands and has a prompt to
	// return to, so it takes the DEFAULT ACTION for SIGINT, SIGQUIT and SIGTERM
	// again - while it keeps being allowed to trap a signal ignored on entry,
	// because that permission is about being an interactive SHELL. sigint5-p.tst
	// requires both halves of that split in the one file: the same `kill` spares the
	// shell in `main` context and kills it in `subshell`.
	//
	// Before the loop below, so a handler being reverted here is installed once, as
	// the plain default action rather than as the interactive one.
	drop_interactive_defaults();
	for (int i = 0; i < kMaxSignal; ++i) {
		if (_entries[i].how == disposition::handler) {
			_entries[i].how = disposition::default_action;
			install(i);  // the text stays; only the action reverts
		}
	}
	// Pending signals do not carry into a subshell either.
	for (int i = 0; i < kMaxSignal; ++i)
		g_pending[i] = 0;
}

void signal_state::ignore_interrupts_for_async() {
	set_ignore(SIGINT);
	set_ignore(SIGQUIT);
}

} // namespace lesh::runtime
