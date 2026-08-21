#include "runtime/signals.h"

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

signal_state::signal_state() : _entries(kMaxSignal) {}

int signal_state::signal_number(std::string_view name) {
	if (name.empty())
		return -1;

	// A bare number is accepted, which is how `trap - 2` works.
	if (name[0] >= '0' && name[0] <= '9') {
		int value = 0;
		for (const char c : name) {
			if (c < '0' || c > '9')
				return -1;
			value = value * 10 + (c - '0');
		}
		return value < kMaxSignal ? value : -1;
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

	struct sigaction sa{};
	switch (_entries[signo].how) {
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

void signal_state::set_trap(int signo, std::string command) {
	if (signo < 0 || signo >= kMaxSignal)
		return;
	_entries[signo].how = disposition::handler;
	_entries[signo].command = std::move(command);
	install(signo);
}

void signal_state::set_ignore(int signo) {
	if (signo < 0 || signo >= kMaxSignal)
		return;
	_entries[signo].how = disposition::ignore;
	_entries[signo].command.clear();
	install(signo);
}

void signal_state::reset(int signo) {
	if (signo < 0 || signo >= kMaxSignal)
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
