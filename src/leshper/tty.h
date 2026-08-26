#pragma once

// The terminal, and the only file that touches it (#98, #129).
//
// #98's principle in one sentence: ALL tty interaction goes through the event
// loop - `tcsetpgrp`, mode changes, winsize queries, the redraw itself - and a
// tty syscall anywhere else is a defect. This file is where the loop keeps
// those syscalls, so that "anywhere else" is checkable by grep: `tcsetattr`,
// `tcsetpgrp`, `tcgetpgrp` and `TIOCGWINSZ` appear in tty.cpp and nowhere else
// in the tree above the runtime.
//
// THE RULE'S SCOPE, clarified by #158 decision 1 and landed by #159: *outside
// the loop, IN THE SHELL PROCESS*, is a defect. There is one sanctioned seam
// outside it, and it is a seam rather than a second owner because both halves
// of it are pinned to a single moment.
//
//   THE FORKED CHILD, between fork and exec, hands ITSELF the terminal -
//   `tcsetpgrp(saved_tty, getpgrp())` after its own `setpgid`, then SIGTTOU,
//   SIGTTIN and SIGTSTP back to SIG_DFL. That process is no longer the shell,
//   it is microseconds from being a different program, and the handoff is only
//   legal there: it works because the SIGTTOU-ignore it inherited still stands,
//   and it races nothing because it touches the tty only after owning it. This
//   is zsh's `entersubsh` and fish's pattern, not lesh's invention. It lives in
//   `tree_walking_executor::take_terminal_in_child`.
//
//   THE RUNTIME'S POST-WAIT RECLAIM is part of the same seam. The terminal has
//   to come back the instant a foreground job's wait returns, not at the end of
//   the line, or `nvim .; read x` reads a terminal it is background on. The
//   loop's unconditional reclaim in `resume_after_execution` stays as the
//   backstop; it is not the only one. `tree_walking_executor::reclaim_terminal`.
//
// What the rule still forbids, and what the grep is still for: a mode change, a
// winsize query or a `tcsetpgrp` in the SHELL process anywhere but here and
// those two named functions. leshper itself is unchanged by all of this - it
// consumes events and emits a render buffer, and never an ioctl.
//
// SEPARATE FROM loop.cpp on purpose. The loop is a poll and a dispatch; this is
// a set of rules about a device, and eleven of the fifteen fish pitfalls
// researched for #128 live in it. Keeping them together means the rules can be
// read - and tested - without a poll loop or a pty around them, and it means
// the loop's file stays about topics.
//
// EVERY FUNCTION HERE TAKES AN FD. Nothing opens `/dev/tty`, nothing assumes
// STDIN_FILENO, nothing consults `isatty` to decide whether it is allowed to
// run. That is the test contract #129 asks for: the unit tests drive all of
// this over `openpty` pairs and pipes, never over the process's own terminal,
// so a test that gets the modes wrong cannot leave the developer's shell in
// raw mode.
//
// THE FIVE RULES, each with its citation:
//
//   RAW MODE ON READ ENTRY, and the modes a child left are kept. fish's
//   `term_steal` copies the current modes first - so a child's `stty` persists
//   into the next child - and only then forces `~ICANON ~ECHO` for the shell
//   itself. `enter_raw` does the same. ISIG stays ON: Ctrl-C must reach the
//   process as SIGINT so that #98's rebindable `cancel-line` runs off a signal
//   event rather than off a byte the driver would have eaten anyway. IEXTEN
//   goes OFF with ICANON and ECHO (#140 decision 1): the BSD driver's extended
//   `c_cc` entries - VDSUSP, VDISCARD, VLNEXT, VWERASE, VREPRINT - would
//   otherwise swallow Ctrl-Y, Ctrl-O, Ctrl-V, Ctrl-W and Ctrl-R before the
//   decoder saw them, so which keys are bindable would be a fact about the
//   platform rather than about the keymap. It rides the ordinary restore.
//
//   NEVER RE-ENABLE ECHO AROUND AN ACTION'S SHELL CODE (fish #7770). There is
//   no "donate" call in this header at all, which is the enforcement: an
//   action's shell code runs with the shell's own modes, and the only thing
//   that restores cooked modes is `leave_raw`, which the loop calls when it is
//   handing the terminal to a foreground command and not otherwise.
//
//   RECLAIM THE FOREGROUND GROUP ON EVERY READ ENTRY (fish #9181). "A command
//   we ran when job control was disabled nevertheless stole the tty from us ...
//   So just unconditionally reclaim the tty." `reclaim` is that call, and the
//   loop makes it every time it starts reading.
//
//   `tcgetpgrp` BEFORE EVERY `tcsetpgrp`, with a per-errno policy. See
//   `set_foreground_pgrp` below; the four errnos and what each means are fish's
//   `tty_transfer_t::try_transfer`, verified against 3.7.1.
//
//   SIGTTOU IGNORED FIRST, AND RESTORE AT EXIT ONLY IF THE TTY IS STILL OURS.
//   Both halves are required and neither is sufficient: without the ignore, the
//   restoring `tcsetattr` from a background process group stops us and we hang
//   on exit instead of exiting; without the ownership check we steal the
//   terminal from whoever legitimately has it (fish #7060).
//
// ALLOCATION: none. Every function here works out of its arguments and a
// file-scope POD; there is no container in this file, which is what lets the
// exit path be a `write(2)` and two ioctls from a signal handler.

#include <cstdint>
#include <string_view>
#include <termios.h>
#include <sys/types.h>

namespace lesh::leshper {

// What a `tcsetpgrp` attempt meant. fish's four cases, plus the two errnos that
// are about the descriptor rather than about the group.
//
// A value rather than a bool because every one of these has a different correct
// response and three of them are not errors: `no_tty` is an editor on a pipe,
// `not_ours` is a shell that has been backgrounded and must NOT grab the
// terminal back, and `group_gone` is the ordinary race where the process we
// were handing to has already exited.
enum class tty_transfer : std::uint8_t {
	// The terminal's foreground group is now the one asked for.
	ok,
	// There is no controlling terminal on this descriptor (ENOTTY). Job control
	// without a tty - fish #6573 - and not a failure: the editor runs, it just
	// has nobody to hand to.
	no_tty,
	// Somebody else owns the terminal and it is not our place to take it. fish
	// backgrounded; taking it would be the theft #7060 describes.
	not_ours,
	// The process group has gone (EINVAL on macOS, or EPERM that outlived the
	// group). The child exited before we could hand over, which is a race we
	// lose harmlessly.
	group_gone,
	// EBADF from `tcgetpgrp`: the descriptor is closed. glibc #3644's shape.
	stdin_closed,
	// Anything else. Reported rather than retried.
	failed,
};

// The terminal's size, as the loop reports it in a `resize_event`.
struct terminal_size {
	std::uint16_t columns = 0;
	std::uint16_t rows = 0;

	friend constexpr bool operator==(terminal_size, terminal_size) noexcept = default;
};

// What a terminal that will not say its size is assumed to be.
//
// 80x24 is the VT100's, which is what every other shell falls back to and what
// every terminal at least is. #128's trap 12: `termsize.cpp` treats a zero in
// either axis as "no answer" rather than as a zero-width screen, because a
// zero-width layout is not a picture, it is a division by zero.
inline constexpr terminal_size kFallbackTerminalSize{80, 24};

// TIOCGWINSZ, with the zero rule applied.
//
// THE COUNTER IS READ BEFORE THIS CALL, not inside it. fish's `termsize.cpp`:
// "Critical read of signal-owned variable. This must happen before the
// TIOCGWINSZ ioctl" - a resize arriving between the read and the ioctl then
// leaves the counter ahead, so the next turn re-queries, where the other order
// would swallow it. This function cannot enforce that; the loop does, and the
// test that proves it is `LeshperLoopResizeIsNotLostBetweenCounterAndIoctl`.
[[nodiscard]] terminal_size query_terminal_size(int fd) noexcept;

// SIG_IGN for SIGTTOU and SIGTTIN, which must be in force BEFORE anything here
// changes modes or the foreground group.
//
// fish's own note on the consequence, quoted because it is a real cost and not
// a free win: "fish ignores SIGTTOU which means that it has the power to
// reassign the tty even if it doesn't own it. This means that other processes
// may get SIGTTOU and become zombies." We take the same trade for the same
// reason - a shell that stops itself while restoring the terminal hangs, and a
// hang is worse than a lost race.
//
// Idempotent. Answers false only if `sigaction` itself failed.
bool ignore_background_write_signals() noexcept;

// `tcsetpgrp(fd, pgid)`, with `tcgetpgrp` asked first and the per-errno policy
// applied (fish `proc.cpp: tty_transfer_t::try_transfer`, 3.7.1).
//
// The four cases, in the order they are checked:
//
//   NO TTY. `tcgetpgrp` says ENOTTY - there is nothing to transfer.
//   ALREADY THEIRS. The owner is already `pgid`: the child won the race
//     between its own `tcsetpgrp` and ours, which is the outcome the
//     both-sides-set-it design wants and not a failure.
//   NOT OURS. Somebody who is neither us nor `pgid` owns it, and we are not
//     `pgid` ourselves: we have been backgrounded. Refuse.
//   OURS. Transfer.
//
// And the errnos, each with its published cause:
//
//   EPERM   a caching race (fish saw it under WSL). Retried while the target
//           group is still alive, bounded - fish retries "while
//           `waitpid(-pgid, WNOHANG) != -1`"; we never hand to a child (the
//           child claims the terminal itself, ADR-0009), so the liveness test
//           is `kill(-pgid, 0)` and the bound keeps a dead group from spinning.
//   EINVAL  macOS's way of saying the group is gone.
//   ENOTTY  job control without a controlling terminal (fish #6573).
//   EBADF   the descriptor is closed (glibc #3644); the caller redirects.
//   EINTR   retried, always.
tty_transfer set_foreground_pgrp(int fd, pid_t pgid) noexcept;

// True when this process's group is the terminal's foreground group.
//
// The predicate behind "restore at exit only if we still own the tty" and
// behind the loop's refusal to write to a terminal it has been backgrounded
// off. False when there is no tty at all, which is the right answer for both
// callers.
[[nodiscard]] bool owns_terminal(int fd) noexcept;

// `write(2)` until it is all out, EINTR and short writes handled.
//
// Here rather than in loop.cpp because it is the last hop of every byte the
// blitter produced and #98 says that hop belongs to the terminal layer.
// Answers false when the descriptor gave a real error - EPIPE on a terminal
// that has gone away, which the loop treats the way it treats EOF on input.
bool write_all(int fd, std::string_view bytes) noexcept;

// ---------------------------------------------------------------------------
// The modes.
// ---------------------------------------------------------------------------

// One terminal's modes, owned for the life of an editing session.
//
// NOT AN OWNER OF THE DESCRIPTOR. It is handed one and never closes it: the fd
// belongs to whoever opened it (the process, for STDIN_FILENO; the test, for an
// `openpty` pair), and an RAII type that closed the process's stdin on the way
// out would be a surprise nobody asked for.
//
// The destructor DOES restore the modes, because the alternative - a thrown
// exception or an early return leaving the developer's terminal in raw mode -
// is the failure everyone who has written this code has shipped once.
class terminal {
public:
	explicit terminal(int fd) noexcept : _fd(fd) {}
	~terminal();

	terminal(const terminal&) = delete;
	terminal& operator=(const terminal&) = delete;

	[[nodiscard]] int fd() const noexcept { return _fd; }
	[[nodiscard]] bool raw() const noexcept { return _raw; }

	// True when the descriptor is a terminal at all. An editor driven over a
	// pipe - which is what most of the loop's tests do - answers false here and
	// every mode call below becomes a no-op that answers true, so the loop needs
	// no `if (is_tty)` of its own.
	[[nodiscard]] bool is_terminal() const noexcept;

	// Saves the modes as they are and installs the shell's: `~ICANON ~ECHO
	// ~IEXTEN`, VMIN 1, VTIME 0, ISIG kept.
	//
	// THE MODES A CHILD LEFT ARE KEPT (fish's `term_copy_modes`). What is read
	// here is whatever the last foreground command left behind, so a `stty -a`
	// change a user made in one command is still there for the next one; only
	// the bits the editor needs are forced. The very first call also
	// records the ORIGINAL modes, which are what `leave_raw` and the exit path
	// put back, so a session's worth of children cannot ratchet the terminal
	// somewhere the shell never restores from.
	//
	// Idempotent: entering raw twice re-asserts the modes, which is exactly what
	// the read-entry rule wants after a command has run.
	bool enter_raw() noexcept;

	// Puts the original modes back, plus `ICANON|ECHO|OPOST|ONLCR` forced on -
	// what a foreground command expects to find. fish's `term_donate`, and the
	// only cooked-mode call in leshper.
	//
	// `tcsetattr` retried on EINTR and on EIO, which is what a terminal gives
	// after SIGHUP; fish redirects at that point and so does the caller here, by
	// treating false as "the terminal is gone".
	bool leave_raw() noexcept;

	// `tcsetpgrp(fd, getpgrp())` through `set_foreground_pgrp`. The read-entry
	// reclaim of fish #9181, unconditional by design.
	tty_transfer reclaim() noexcept;

	[[nodiscard]] terminal_size size() const noexcept { return query_terminal_size(_fd); }

	// Arms the process-wide exit restore with this terminal's original modes.
	//
	// Called once the originals are known - that is, after the first
	// `enter_raw` - because an exit path that restored modes nobody saved would
	// write whatever was in the uninitialised struct to the user's terminal.
	void arm_exit_restore() const noexcept;

private:
	bool apply(const struct termios& modes) noexcept;

	int _fd = -1;
	bool _raw = false;
	bool _saved = false;
	// The modes at the first `enter_raw`: what the world gets back.
	struct termios _original{};
};

// ---------------------------------------------------------------------------
// The exit path (#98 decision 5).
// ---------------------------------------------------------------------------

// ONE registered async-signal-safe restore, called from every exit path:
// normal exit, the assert-and-die path, a fatal-signal handler, and before
// every command.
//
// Async-signal-safe means what it says: one `tcgetpgrp`, one `tcsetattr`, one
// `write(2)` of the bracketed-paste-off sequence, and no allocation, no stdio,
// no lock. It reads a file-scope POD armed by `terminal::arm_exit_restore`.
//
// Restores ONLY IF WE STILL OWN THE TTY (fish's `restore_term_mode`:
// `if (getpgrp() == tcgetpgrp(STDIN_FILENO))`). Two things go wrong without the
// check - we steal the terminal from whoever has it (#7060), and the
// `tcsetattr` from a background group would stop us if SIGTTOU were not already
// ignored, so `ignore_background_write_signals` is a precondition and is called
// by `arm_exit_restore`.
//
// A no-op when nothing has been armed, which is the state of every process that
// never started an interactive read.
void restore_terminal_for_exit() noexcept;

// Forgets the armed terminal. The loop calls it when it gives the terminal up
// for good, so a later `_exit` does not write escape bytes to a descriptor that
// now means something else.
void disarm_exit_restore() noexcept;

// The fatal-signal half of #98 decision 5: SIGSEGV, SIGBUS, SIGILL, SIGFPE and
// SIGABRT restore the terminal, then reset to SIG_DFL and re-raise so the crash
// still produces the core dump and the sanitizer report.
//
// SIGABRT is the one that matters most in practice, because `LESH_ASSERT` dies
// through `std::abort()` - which is "the assert-and-die path" #98 names.
//
// OPT-IN, AND NEVER OVER A HANDLER SOMEBODY ELSE OWNS. A signal already handled
// is left alone: ASan installs its own SIGSEGV and SIGBUS handlers, and stealing
// them would cost the sanitized gate its reports, which is a worse trade than a
// raw terminal after a segfault. So this installs only where the disposition is
// still SIG_DFL. Called by the wiring site (#134), not by the loop, because
// process-wide dispositions belong to whoever owns the process.
//
// Answers how many it installed.
int install_fatal_restore_handlers() noexcept;

// Whether anything is armed - for the tests, which have no other way to see a
// file-scope POD.
[[nodiscard]] bool exit_restore_armed() noexcept;

// The two sequences the exit path writes, named so a test can assert the bytes
// rather than an escape literal it retyped.
inline constexpr std::string_view kBracketedPasteOn = "\x1b[?2004h";
inline constexpr std::string_view kBracketedPasteOff = "\x1b[?2004l";

} // namespace lesh::leshper
