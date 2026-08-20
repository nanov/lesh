#include "runtime/signals.h"

#include <array>
#include <cstdlib>

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
	std::string_view name;
	int number;
};

// POSIX's required set, plus the ones the conformance suite exercises. Named
// without the SIG prefix; signal_number accepts either form.
constexpr std::array<name_entry, 20> kSignalNames = {{
	{"EXIT", kExitTrap}, {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT},
	{"ILL", SIGILL},     {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"FPE", SIGFPE},
	{"KILL", SIGKILL},   {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
	{"TERM", SIGTERM},   {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"CHLD", SIGCHLD},
	{"CONT", SIGCONT},   {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU},
}};

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
	for (const auto& e : kSignalNames)
		if (e.name == bare)
			return e.number;
	return -1;
}

std::string_view signal_state::signal_name(int signo) {
	for (const auto& e : kSignalNames)
		if (e.number == signo)
			return e.name;
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
	for (int i = 0; i < kMaxSignal; ++i) {
		if (_entries[i].how == disposition::handler)
			reset(i);
	}
	// Pending signals do not carry into a subshell either.
	for (int i = 0; i < kMaxSignal; ++i)
		g_pending[i] = 0;
}

} // namespace lesh::runtime
