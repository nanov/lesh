#include "ui/tty.h"

#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace lesh::ui {
namespace {

// The armed exit restore (#98 decision 5).
//
// File-scope and POD, because a signal handler may reach it: no constructor, no
// destructor, no allocation, and every field written before `armed` is set so a
// handler that observes `armed` observes the rest of it too.
struct exit_restore_state {
	int fd = -1;
	struct termios modes{};
	volatile sig_atomic_t armed = 0;
};

exit_restore_state g_exit_restore;

// EPERM from `tcsetpgrp` is a caching race - fish saw it under WSL and retries
// while the target group is alive. We hand only to our OWN group (ADR-0009: the
// child claims the terminal itself between fork and exec), so the liveness test
// is a signal-zero rather than fish's `waitpid`, and the bound is what stops a
// group that died mid-retry from spinning the loop.
constexpr int kTransferRetries = 32;

// Restores, then gets out of the way. Async-signal-safe throughout: the
// restore is two ioctls and a `write`, and the re-raise is `raise`.
void fatal_restore_handler(int signo) noexcept;

bool set_modes(int fd, const struct termios& modes) noexcept {
	for (;;) {
		if (tcsetattr(fd, TCSANOW, &modes) == 0)
			return true;
		// EINTR is ordinary. EIO is what a terminal gives after SIGHUP, and fish
		// retries it once for the same reason it retries EINTR: the descriptor
		// may still come back. A second EIO means it will not.
		if (errno == EINTR)
			continue;
		return false;
	}
}

void fatal_restore_handler(int signo) noexcept {
	restore_terminal_for_exit();
	// Back to the default and re-raise, so the crash still produces the core
	// dump and the sanitizer report the developer needs. A handler that
	// swallowed the signal would turn a crash into a silent hang.
	struct sigaction dfl{};
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	sigaction(signo, &dfl, nullptr);
	raise(signo);
}

} // namespace

// ---------------------------------------------------------------------------
// Size
// ---------------------------------------------------------------------------

terminal_size query_terminal_size(int fd) noexcept {
	if (fd < 0)
		return kFallbackTerminalSize;

	struct winsize ws{};
	if (ioctl(fd, TIOCGWINSZ, &ws) != 0)
		return kFallbackTerminalSize;

	// #128's trap 12: a zero in either axis is "no answer", not a zero-sized
	// screen. Terminals report it during startup and inside `screen` sessions
	// that have not attached yet, and a layout at zero columns is not a picture.
	if (ws.ws_col == 0 || ws.ws_row == 0)
		return kFallbackTerminalSize;

	return terminal_size{static_cast<std::uint16_t>(ws.ws_col),
	                     static_cast<std::uint16_t>(ws.ws_row)};
}

// ---------------------------------------------------------------------------
// Process group
// ---------------------------------------------------------------------------

bool ignore_background_write_signals() noexcept {
	struct sigaction ignore{};
	ignore.sa_handler = SIG_IGN;
	sigemptyset(&ignore.sa_mask);
	const bool ttou = sigaction(SIGTTOU, &ignore, nullptr) == 0;
	const bool ttin = sigaction(SIGTTIN, &ignore, nullptr) == 0;
	return ttou && ttin;
}

tty_transfer set_foreground_pgrp(int fd, pid_t pgid) noexcept {
	if (fd < 0)
		return tty_transfer::no_tty;

	// ASK FIRST, ALWAYS. The answer decides which of the four cases we are in,
	// and three of them are not a transfer at all.
	const pid_t owner = tcgetpgrp(fd);
	if (owner < 0) {
		switch (errno) {
			case EBADF:
				// glibc #3644: stdin is closed. The caller redirects rather than
				// treating this as a terminal that misbehaved.
				return tty_transfer::stdin_closed;
			case ENOTTY:
			case ENXIO:
				return tty_transfer::no_tty;
			default:
				return tty_transfer::failed;
		}
	}

	// The child won the race between its own `tcsetpgrp` and ours - which is the
	// outcome the both-sides-set-it design wants, and the reason this is checked
	// before ownership rather than after.
	if (owner == pgid)
		return tty_transfer::ok;

	// `tcgetpgrp` legitimately answers 0 on FreeBSD and inside pid namespaces
	// (fish `acquire_tty_or_exit`, and `initial_fg_process_group == 0` in
	// `restore_term_foreground_process_group_for_exit`). Nobody owns it, so
	// taking it is not theft.
	const pid_t self = getpgrp();
	if (owner != self && owner != 0)
		return tty_transfer::not_ours;

	for (int attempt = 0; attempt < kTransferRetries; ++attempt) {
		if (tcsetpgrp(fd, pgid) == 0)
			return tty_transfer::ok;

		switch (errno) {
			case EINTR:
				continue;
			case EPERM:
				// The WSL caching race. Retry while the group is still there; a
				// group that has gone is `group_gone` and not an error.
				if (kill(-pgid, 0) != 0 && errno == ESRCH)
					return tty_transfer::group_gone;
				continue;
			case EINVAL:
				// macOS's spelling of "that group no longer exists".
				return tty_transfer::group_gone;
			case ENOTTY:
				return tty_transfer::no_tty;
			case EBADF:
				return tty_transfer::stdin_closed;
			default:
				return tty_transfer::failed;
		}
	}
	return tty_transfer::group_gone;
}

bool owns_terminal(int fd) noexcept {
	if (fd < 0)
		return false;
	const pid_t owner = tcgetpgrp(fd);
	return owner >= 0 && owner == getpgrp();
}

bool write_all(int fd, std::string_view bytes) noexcept {
	if (fd < 0)
		return false;
	std::size_t written = 0;
	while (written < bytes.size()) {
		const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
		if (n > 0) {
			written += static_cast<std::size_t>(n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		// EAGAIN on a non-blocking terminal: the kernel buffer is full and the
		// only honest thing is to wait for it. One byte at a time would be the
		// alternative, and it is the same wait with more syscalls.
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd wait{};
			wait.fd = fd;
			wait.events = POLLOUT;
			if (::poll(&wait, 1, -1) < 0 && errno != EINTR)
				return false;
			continue;
		}
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// terminal
// ---------------------------------------------------------------------------

terminal::~terminal() {
	if (_raw)
		leave_raw();
}

bool terminal::is_terminal() const noexcept { return _fd >= 0 && isatty(_fd) == 1; }

bool terminal::apply(const struct termios& modes) noexcept { return set_modes(_fd, modes); }

bool terminal::enter_raw() noexcept {
	if (!is_terminal()) {
		// A pipe or a pty-less test harness. There are no modes to set, and
		// answering true keeps every caller free of an `if (is_tty)` that would
		// be a second place for the rule to be got wrong.
		_raw = true;
		return true;
	}

	struct termios current{};
	if (tcgetattr(_fd, &current) != 0)
		return false;

	// THE FIRST ENTRY RECORDS THE ORIGINALS. Later entries do not, so that a
	// child's `stty` change survives into the next child (fish's
	// `term_copy_modes`) without ratcheting what we hand back at exit.
	if (!_saved) {
		_original = current;
		_saved = true;
	}

	struct termios shell = current;
	// The bits the editor needs, and no more. ISIG STAYS ON: Ctrl-C must
	// arrive as SIGINT so #98's `cancel-line` runs off a signal event, and
	// turning it off would mean re-implementing the driver's job.
	//
	// IEXTEN GOES OFF (#140 decision 1, fish's and zle's answer). It is what
	// makes the BSD driver's extended `c_cc` entries live, and on macOS those
	// are Ctrl-Y (VDSUSP), Ctrl-O (VDISCARD), Ctrl-V (VLNEXT), Ctrl-W (VWERASE)
	// and Ctrl-R (VREPRINT) - five bytes the driver eats before any of them
	// reaches the decoder, several of them the natural accept and word keys.
	// Whether a key is bindable must be a fact about the keymap, not about
	// which platform's tty driver claimed the byte first. Ctrl-W stops working
	// only by accident of WERASE, and `delete_backward_word` is bound to it.
	//
	// NOTHING RESTORES THIS SEPARATELY. `_original` is the whole termios as it
	// was before the first entry, and `leave_raw` and the exit path write it
	// back wholesale, so the bit rides the save/restore every other bit rides;
	// a parallel path for one flag would be a second place to get it wrong.
	shell.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN));
	shell.c_cc[VMIN] = 1;
	shell.c_cc[VTIME] = 0;

	if (!apply(shell))
		return false;

	_raw = true;
	arm_exit_restore();
	// #97's floor includes bracketed paste, and #111's decoder is written
	// against the markers, so turning it on is part of entering the editor's
	// modes rather than a separate feature.
	write_all(_fd, kBracketedPasteOn);
	return true;
}

bool terminal::leave_raw() noexcept {
	_raw = false;
	if (!is_terminal() || !_saved)
		return true;

	// Bracketed paste goes off BEFORE the modes go back: a foreground command
	// that reads the terminal must not be handed paste markers, and the order
	// matters because the write is what the child would otherwise race.
	write_all(_fd, kBracketedPasteOff);

	struct termios cooked = _original;
	// What a foreground command expects to find, forced on rather than merely
	// restored - fish's `term_donate` does the same, because a shell started
	// with ECHO already off would otherwise hand a child a terminal that eats
	// its prompts.
	cooked.c_lflag |= static_cast<tcflag_t>(ICANON | ECHO);
	cooked.c_oflag |= static_cast<tcflag_t>(OPOST | ONLCR);
	return apply(cooked);
}

tty_transfer terminal::reclaim() noexcept {
	// fish #9181, and it is unconditional on purpose: "a command we ran when job
	// control was disabled nevertheless stole the tty from us ... So just
	// unconditionally reclaim the tty."
	return set_foreground_pgrp(_fd, getpgrp());
}

void terminal::arm_exit_restore() const noexcept {
	if (!_saved)
		return;
	// SIGTTOU first - see the header. Without it the restoring `tcsetattr` from
	// a background group stops us and the process hangs where it meant to exit.
	ignore_background_write_signals();
	// NORMAL EXIT, registered once. `main()` returning, `exit()`, and a static
	// destructor all reach here; `_exit` deliberately does not, which is why the
	// loop also restores explicitly on its own way out and why fish's `main`
	// ends with `_exit` after an explicit restore rather than trusting this.
	static const bool once = [] { return std::atexit(&restore_terminal_for_exit) == 0; }();
	(void)once;
	g_exit_restore.fd = _fd;
	g_exit_restore.modes = _original;
	g_exit_restore.armed = 1;
}

// ---------------------------------------------------------------------------
// The exit path
// ---------------------------------------------------------------------------

void restore_terminal_for_exit() noexcept {
	if (g_exit_restore.armed == 0)
		return;
	const int fd = g_exit_restore.fd;
	if (fd < 0)
		return;

	// ONLY IF IT IS STILL OURS (fish `restore_term_mode`). Neither `tcgetpgrp`
	// nor `tcsetattr` is on POSIX's async-signal-safe list; both are thin ioctl
	// wrappers with no allocation and no lock, and fish calls exactly these two
	// from exactly this path. The alternative - not restoring - leaves a
	// terminal the user cannot type into.
	if (tcgetpgrp(fd) != getpgrp())
		return;

	tcsetattr(fd, TCSANOW, &g_exit_restore.modes);

	// A raw `write(2)`, not `write_all`: this path may be a signal handler and
	// a short write of eight bytes to a terminal is not a case worth a loop.
	ssize_t ignored = ::write(fd, kBracketedPasteOff.data(), kBracketedPasteOff.size());
	(void)ignored;
}

int install_fatal_restore_handlers() noexcept {
	// SIGABRT included, and it is the one that fires in practice: `LESH_ASSERT`
	// dies through `std::abort()`, which is #98's "assert-and-die path".
	constexpr int kFatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
	int installed = 0;
	for (int signo : kFatal) {
		struct sigaction now{};
		if (sigaction(signo, nullptr, &now) != 0)
			continue;
		// NEVER over a handler somebody else owns. ASan holds SIGSEGV and SIGBUS
		// under the sanitized gate, and its report is worth more than a restored
		// terminal after a segfault.
		if (now.sa_handler != SIG_DFL)
			continue;
		struct sigaction restore{};
		restore.sa_handler = &fatal_restore_handler;
		sigemptyset(&restore.sa_mask);
		// SA_RESETHAND as belt and braces: the handler resets the disposition
		// itself before re-raising, and a handler that somehow re-entered would
		// otherwise loop forever inside a crash.
		restore.sa_flags = SA_RESETHAND;
		if (sigaction(signo, &restore, nullptr) == 0)
			++installed;
	}
	return installed;
}

void disarm_exit_restore() noexcept {
	g_exit_restore.armed = 0;
	g_exit_restore.fd = -1;
}

bool exit_restore_armed() noexcept { return g_exit_restore.armed != 0; }

} // namespace lesh::ui
