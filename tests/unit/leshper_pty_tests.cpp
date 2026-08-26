#include "runtime/history_store.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

// SWITCH-ON, END TO END (#134): a real pty, the real binary, a real session.
//
// IT EXECS `lesh` RATHER THAN RUNNING A SESSION IN-PROCESS, and that is not
// squeamishness about threads. `loop.cpp` captures the process's pid at static
// initialization, because fish's rule is that a handler must tell a forked child
// from the shell by `getpid()` and never by an atfork flag - so a session forked
// out of an already-running gtest binary sees every signal as delivered to a
// child, resets the disposition and re-raises it. Ctrl-C would kill the session
// instead of cancelling the line, which is not the shell's behaviour but the
// harness's. Exec'ing is also the more honest test: main.cpp's floor check, its
// rc lookup and its logging setup are all on the path this drives.
//
// `$HOME` IS THE TEMP DIRECTORY, always. The shell reads `~/.leshrc` and writes
// `~/.lesh_history`, and a test that read the developer's own rc would pass
// differently on every machine and append to a history they use.
//
// WHAT #151 ADDED: the highlighter, through the real seam. A unit test can only
// assert that a token WITH an adapter answers correctly; the defect #151 fixed
// was that the real session's token never got one, so the only test that could
// have caught it is one that reads colours off a real pty. The words chosen are
// the discriminating ones - `exit` and `bind` have no binary anywhere on the
// machine, so nothing but the shell's own builtin table can make them green.
//
// F-41 IS WHAT THIS IS FOR: every exit path restores the terminal. Three are
// exercised - Ctrl-D at an empty prompt, `exit`, and death by SIGABRT, which is
// LESH_ASSERT's route and #98 decision 5's "assert-and-die path". A fourth,
// SIGKILL, is deliberately NOT asserted on: nothing can restore a terminal after
// a signal nothing can catch, and a test that claimed otherwise would be lying.

namespace {

// The prompt the rc file sets, distinctive enough that nothing else in this file
// could produce it by accident - and with NO trailing space, because the blitter
// erases to end of line rather than emitting one, so a trailing space never
// reaches the wire.
constexpr std::string_view kPrompt = "lesh-test>";

// A `$HOME` of its own, with an rc file in it.
class scratch_home {
public:
	explicit scratch_home(std::string_view rc_body) {
		std::ofstream rc{_dir.file(".leshrc")};
		rc << rc_body;
	}

	[[nodiscard]] const std::string& path() const { return _dir.dir(); }
	[[nodiscard]] std::string history_path() const { return _dir.file(".lesh_history"); }

private:
	lesh::testing::temp_path _dir;
};

// One real shell on one pty.
class shell_on_a_pty {
public:
	explicit shell_on_a_pty(const scratch_home& home, const char* term = "xterm-256color") {
		if (::openpty(&_master, &_slave, nullptr, nullptr, nullptr) != 0)
			return;
		const std::string home_path = home.path();
		_child = ::fork();
		if (_child == 0) {
			// `login_tty` and not setsid-plus-ioctl by hand: it makes this process
			// a session leader, makes the pty its CONTROLLING terminal and dups it
			// onto the three standard descriptors, in the one order that works.
			// Without the controlling terminal the driver has no foreground
			// process group to send SIGINT to, and Ctrl-C would silently do
			// nothing - which is exactly the case this file is here to test.
			if (::login_tty(_slave) != 0)
				::_exit(120);
			::setenv("HOME", home_path.c_str(), 1);
			::setenv("TERM", term, 1);
			// SET, NOT INHERITED. The theme's colours are RGB and the blitter
			// quantizes them to the 256-colour cube unless the terminal says it
			// can take them literally - so without this, whether an assertion
			// about `38;2;95;175;95` passes would depend on the developer's own
			// `$COLORTERM`. Nothing here asserts on the quantized form; that is
			// blit's own test.
			::setenv("COLORTERM", "truecolor", 1);
			::unsetenv("ENV");
			::unsetenv("LESH_LOG");
			::unsetenv("LESH_LOG_FILE");
			::execl(LESH_BINARY, "lesh", static_cast<char*>(nullptr));
			::_exit(121);
		}
		::close(_slave);
		_slave = -1;
		::fcntl(_master, F_SETFL, O_NONBLOCK);
	}

	~shell_on_a_pty() {
		if (_child > 0) {
			// Whatever the test did or did not do, this process does not outlive
			// it. SIGKILL because by here we no longer care how it dies.
			::kill(_child, SIGKILL);
			int ignored = 0;
			::waitpid(_child, &ignored, 0);
		}
		if (_master >= 0)
			::close(_master);
	}

	shell_on_a_pty(const shell_on_a_pty&) = delete;
	shell_on_a_pty& operator=(const shell_on_a_pty&) = delete;

	[[nodiscard]] bool alive() const noexcept { return _master >= 0 && _child > 0; }
	[[nodiscard]] pid_t child() const noexcept { return _child; }

	void type(std::string_view bytes) const {
		[[maybe_unused]] const ssize_t wrote = ::write(_master, bytes.data(), bytes.size());
	}

	// Reads until `needle` has shown up `times` times or the budget runs out. A
	// budget rather than a blocking read, because a test that hangs is worse than
	// a test that fails and there is a whole shell on the other end of this
	// descriptor. A COUNT rather than a search, because everything the session
	// ever wrote is still in `_seen` - the second prompt has to be told from the
	// first, or every wait after the first would answer instantly.
	[[nodiscard]] bool wait_for(std::string_view needle, std::size_t times = 1,
	                            std::chrono::milliseconds budget = std::chrono::seconds{10}) {
		const auto deadline = std::chrono::steady_clock::now() + budget;
		while (count_of(needle) < times && std::chrono::steady_clock::now() < deadline) {
			char chunk[512];
			const ssize_t n = ::read(_master, chunk, sizeof(chunk));
			if (n > 0) {
				_seen.append(chunk, static_cast<std::size_t>(n));
				continue;
			}
			if (n == 0)
				break;  // the slave side is gone
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds{5});
		}
		return count_of(needle) >= times;
	}

	[[nodiscard]] std::size_t count_of(std::string_view needle) const {
		std::size_t found = 0;
		for (std::size_t at = _seen.find(needle); at != std::string::npos;
		     at = _seen.find(needle, at + needle.size()))
			++found;
		return found;
	}

	// Waits for the child to leave and answers its wait status, or nothing when
	// it outstayed the budget.
	[[nodiscard]] std::optional<int> reap(
		std::chrono::milliseconds budget = std::chrono::seconds{10}) {
		const auto deadline = std::chrono::steady_clock::now() + budget;
		while (std::chrono::steady_clock::now() < deadline) {
			int status = 0;
			const pid_t done = ::waitpid(_child, &status, WNOHANG);
			if (done == _child) {
				_child = -1;
				return status;
			}
			// Keep draining, or a child writing into a full pty buffer never
			// reaches its own exit - and KEEP what was drained, because the last
			// thing a shell writes on the way out is usually the reason it went.
			char chunk[512];
			for (ssize_t n = ::read(_master, chunk, sizeof(chunk)); n > 0;
			     n = ::read(_master, chunk, sizeof(chunk)))
				_seen.append(chunk, static_cast<std::size_t>(n));
			std::this_thread::sleep_for(std::chrono::milliseconds{5});
		}
		return std::nullopt;
	}

	[[nodiscard]] const std::string& seen() const noexcept { return _seen; }

	// The slave's line discipline, which is what "the terminal was restored"
	// means. A pty master and its slave share one termios, so the parent can ask
	// this without holding the slave open.
	[[nodiscard]] bool modes(struct termios& out) const {
		return ::tcgetattr(_master, &out) == 0;
	}

private:
	int _master = -1;
	int _slave = -1;
	pid_t _child = -1;
	std::string _seen;
};

[[nodiscard]] bool is_cooked(const struct termios& modes) noexcept {
	// What a foreground command expects to find, and what the editor turns off:
	// canonical input and echo. fish's `term_donate` forces exactly these back.
	return (modes.c_lflag & ICANON) != 0 && (modes.c_lflag & ECHO) != 0;
}

// The driver's extended input processing (#140 decision 1). While the editor
// holds the terminal this is OFF, so VDSUSP, VDISCARD, VLNEXT, VWERASE and
// VREPRINT are inert and their keys reach the decoder as bytes.
[[nodiscard]] bool extended_input(const struct termios& modes) noexcept {
	return (modes.c_lflag & IEXTEN) != 0;
}

// Every test starts the same way, and the rc file proves #101's ordering as a
// side effect: the prompt only says `lesh-test>` because `~/.leshrc` ran before
// the first read.
constexpr std::string_view kRc = "PS1='lesh-test>'\n";

// The theme's two verdicts about a command name, as the wire sees them (#124's
// vocabulary, theme.h's colours, `COLORTERM=truecolor` above).
//
// ALL FOUR RUNNABLE KINDS SHARE ONE GREEN - external, builtin, function, alias -
// and #141's pty smoke test could not tell them apart because it used
// `echo`, which is all four's colour AND has a binary. So the discrimination
// here is not in the colour: it is in the WORD. `exit` and `bind` are green only
// if the shell's builtin table was consulted, because there is nothing else on
// the machine they could be.
constexpr std::string_view kRunnable = "38;2;95;175;95";
constexpr std::string_view kUnknown = "38;2;215;95;95";

// The rc that gives a session an alias with nothing behind it.
constexpr std::string_view kAliasRc =
	"PS1='lesh-test>'\nalias zzalias='echo aliased'\n";

} // namespace

TEST(LeshperPty, APromptAppearsAndACommandRuns) {
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());

	// `$PS1`, painted by the loop's first render - before the first poll, because
	// a prompt that appeared only once you had typed is a prompt that arrived too
	// late to be one. That it says `lesh-test>` at all is #101's ordering: the rc
	// ran before the first read.
	ASSERT_TRUE(shell.wait_for(kPrompt)) << "no prompt; saw: " << shell.seen();

	// `\r` is Enter, which is what a terminal sends: U+000D.
	shell.type("echo switched-on\r");
	EXPECT_TRUE(shell.wait_for("switched-on")) << "saw: " << shell.seen();
	// And the prompt comes back, which is what makes it a session and not a
	// one-shot.
	EXPECT_TRUE(shell.wait_for(kPrompt, 2)) << "saw: " << shell.seen();
}

TEST(LeshperPty, AnIncompleteLineGetsTheContinuationPromptRatherThanRunning) {
	// F-35, end to end: Enter asks the syntax layer, and an unterminated quote is
	// a continuation and not a syntax error.
	const scratch_home home{"PS1='lesh-test>'\nPS2='more>'\n"};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("echo \"one\r");
	ASSERT_TRUE(shell.wait_for("more>")) << "no continuation; saw: " << shell.seen();
	shell.type("two\"\r");
	// `\r\n`, because ONLCR is on: the shell writes `\n` and the driver expands it.
	EXPECT_TRUE(shell.wait_for("one\r\ntwo")) << "saw: " << shell.seen();
}

TEST(LeshperPty, AnAcceptedLineIsRecordedInTheHistory) {
	const scratch_home home{kRc};
	{
		shell_on_a_pty shell{home};
		ASSERT_TRUE(shell.alive());
		ASSERT_TRUE(shell.wait_for(kPrompt));
		shell.type("echo remembered\r");
		ASSERT_TRUE(shell.wait_for("remembered"));
		// Ctrl-D at an empty prompt: `end_of_file`, which exits only when there is
		// nothing typed.
		ASSERT_TRUE(shell.wait_for(kPrompt, 2));
		shell.type("\x04");
		EXPECT_TRUE(shell.reap().has_value()) << "Ctrl-D did not end the session";
	}

	lesh::runtime::history_store store{home.history_path()};
	std::vector<std::string> entries;
	store.for_each_newest_first([&](std::string_view entry) { entries.emplace_back(entry); });
	ASSERT_FALSE(entries.empty()) << "nothing was written to ~/.lesh_history";
	EXPECT_EQ(entries.front(), "echo remembered");
}

TEST(LeshperPty, ControlCCancelsTheLineAndLeavesTheStatusAt130) {
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("echo never-run");
	ASSERT_TRUE(shell.wait_for("never-run"));
	// ISIG is on by design (tty.h's first rule), so this byte becomes SIGINT in
	// the driver and never reaches a keymap. The loop dispatches `cancel_line`
	// off the signal event; the shell sets `$?` and fires the INT trap.
	shell.type("\x03");
	ASSERT_TRUE(shell.wait_for("^C")) << "no cancel indicator; saw: " << shell.seen();

	shell.type("echo status=$?\r");
	EXPECT_TRUE(shell.wait_for("status=130")) << "saw: " << shell.seen();
}

TEST(LeshperPty, TheIntTrapFiresAtThePromptTheZshWay) {
	// #98 decision 3, the owner's override of the recommendation: Ctrl-C while
	// editing runs `cancel-line` AND fires the user's INT trap. It is also the
	// end-to-end proof of the signal CHAIN - `g_pending` is set by a handler the
	// hub replaced, and only chaining keeps it being set.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("trap 'echo caught-int' INT\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 2));
	shell.type("\x03");
	EXPECT_TRUE(shell.wait_for("caught-int")) << "saw: " << shell.seen();
}

TEST(LeshperPty, AnIgnoredIntLeavesTheLineAloneAtThePrompt) {
	// #142 rule 3, end to end: the newest ignore stands. `trap '' INT` means in
	// lesh what it means in bash - Ctrl-C inert at the prompt - because the hub's
	// reassert now asks the kernel first and declines to take a SIG_IGN back.
	// Before this, reassert stomped the ignore and the line was cancelled anyway.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("trap '' INT\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 2));

	shell.type("echo survives");
	ASSERT_TRUE(shell.wait_for("survives"));
	shell.type("\x03");

	// Nothing to wait FOR when the assertion is that nothing happens, so the
	// proof is positive: the line is still there and still runs. Enter after the
	// Ctrl-C echoes the command and its output, which a cancelled line could not
	// produce.
	shell.type("\r");
	EXPECT_TRUE(shell.wait_for("survives", 2)) << "the ignored SIGINT cancelled the line anyway";
	EXPECT_EQ(shell.count_of("^C"), 0u) << "an ignored SIGINT still printed a cancel indicator";
}

TEST(LeshperPty, ADefaultHangupKillsTheShell) {
	// #142 rule 5 - SIGHUP left the hub entirely. The hub used to CATCH a
	// SIG_DFL SIGHUP, and the editor's signal entrance is deliberately unbound,
	// so `kill -HUP $$` on a shell with no HUP trap was simply eaten. `trap - HUP`
	// is written out explicitly rather than relied upon, because the point is the
	// disposition the shell itself asks for being the one that runs.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("trap - HUP\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 2));

	ASSERT_EQ(::kill(shell.child(), SIGHUP), 0);
	const std::optional<int> status = shell.reap();
	ASSERT_TRUE(status.has_value()) << "the shell swallowed a default-fatal SIGHUP; saw: "
	                                << shell.seen();
	EXPECT_TRUE(WIFSIGNALED(*status) && WTERMSIG(*status) == SIGHUP)
		<< "SIGHUP did not kill the shell the way its own disposition says it should";
}

TEST(LeshperPty, AChildTrapFiresAfterACommand) {
	// #142 rule 4a, end to end, and the defect that motivated the whole ticket.
	// `trap 'cmd' CHLD` installs `record_signal`; the old save-once hub stomped
	// it at the next reassert and went on chaining to the disposition from before
	// the trap existed, so `g_pending[CHLD]` was never set and the body never ran.
	// `/bin/echo` and not the builtin, because a builtin forks no child and there
	// would be no SIGCHLD to catch.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("trap 'echo caught-chld' CHLD\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 2));

	shell.type("/bin/echo ran\r");
	EXPECT_TRUE(shell.wait_for("caught-chld")) << "the CHLD trap never fired; saw: " << shell.seen();
}

TEST(LeshperPty, ControlCDuringAForegroundCommandStillYields130AndFiresTheTrap) {
	// #142's second amendment: delivery is pinned to the shell thread now - the
	// loop thread and every helper block the caught set at spawn - so a Ctrl-C
	// during EXECUTION reaches the one thread that owns `g_pending` every time
	// rather than whichever thread the kernel happened to pick. The behaviour
	// asserted is #134's and unchanged; what is new is that it is deterministic.
	//
	// It is also where the accepted consequence shows: SIGINT carries no
	// SA_RESTART, so it now reliably interrupts the shell thread's syscalls
	// mid-command. Anything that surfaces here is a real latent bug on the
	// executor's EINTR path rather than a flake.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("trap 'echo caught-int' INT\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 2));

	shell.type("sleep 5\r");
	// Long enough that the command is certainly running and the editor parked.
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	shell.type("\x03");

	EXPECT_TRUE(shell.wait_for("caught-int")) << "the INT trap did not fire; saw: " << shell.seen();
	shell.type("echo status=$?\r");
	EXPECT_TRUE(shell.wait_for("status=130")) << "saw: " << shell.seen();
}

TEST(LeshperPty, ControlDAtAnEmptyPromptExitsAndRestoresTheTerminal) {
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	// While it is editing, the terminal is the editor's.
	struct termios editing{};
	ASSERT_TRUE(shell.modes(editing));
	EXPECT_FALSE(is_cooked(editing)) << "the editor never took the terminal";

	shell.type("\x04");
	const std::optional<int> status = shell.reap();
	ASSERT_TRUE(status.has_value()) << "Ctrl-D did not end the session";
	EXPECT_TRUE(WIFEXITED(*status)) << "saw: " << shell.seen();

	struct termios after{};
	ASSERT_TRUE(shell.modes(after));
	EXPECT_TRUE(is_cooked(after)) << "a normal exit left the terminal raw";
}

TEST(LeshperPty, TheExitBuiltinEndsTheSessionWithItsStatus) {
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("exit 3\r");
	const std::optional<int> status = shell.reap();
	ASSERT_TRUE(status.has_value()) << "`exit` did not end the session; saw: " << shell.seen();
	ASSERT_TRUE(WIFEXITED(*status));
	EXPECT_EQ(WEXITSTATUS(*status), 3);

	struct termios after{};
	ASSERT_TRUE(shell.modes(after));
	EXPECT_TRUE(is_cooked(after)) << "`exit` left the terminal raw";
}

TEST(LeshperPty, DyingOnTheAssertPathStillRestoresTheTerminal) {
	// #98 decision 5, and the one that is easiest to get wrong: `LESH_ASSERT`
	// dies through `std::abort()`, so SIGABRT is the assert-and-die path. The
	// registered restore is async-signal-safe - one `tcgetpgrp`, one `tcsetattr`,
	// one `write` - and it re-raises afterwards, so the crash still produces its
	// core and its sanitizer report.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	struct termios editing{};
	ASSERT_TRUE(shell.modes(editing));
	ASSERT_FALSE(is_cooked(editing));

	ASSERT_EQ(::kill(shell.child(), SIGABRT), 0);
	const std::optional<int> status = shell.reap();
	ASSERT_TRUE(status.has_value()) << "the child outlived SIGABRT";

	struct termios after{};
	ASSERT_TRUE(shell.modes(after));
	EXPECT_TRUE(is_cooked(after)) << "the fatal-signal path left the terminal raw";
}

TEST(LeshperPty, ATerminalBelowTheFloorIsRefusedInOneLine) {
	// #97 decision 3: below the floor, leshper never starts. Exit 2, one line -
	// and NOT a degraded renderer, which is the half of the decision that is
	// invisible in the code and would be easy to add later by accident.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home, "dumb"};
	ASSERT_TRUE(shell.alive());

	const std::optional<int> status = shell.reap();
	ASSERT_TRUE(status.has_value()) << "a below-floor terminal did not end the session";
	ASSERT_TRUE(WIFEXITED(*status));
	EXPECT_EQ(WEXITSTATUS(*status), 2);
	EXPECT_NE(shell.seen().find("below the terminal floor"), std::string::npos)
		<< "saw: " << shell.seen();
	EXPECT_EQ(shell.count_of(kPrompt), 0u) << "it started anyway";
}

TEST(LeshperPty, ABoundKeyAcceptsTheSuggestionTheLoopApplied) {
	// F-25 ON A REAL TERMINAL (#144): the autosuggester proposes off the session's
	// own history, the loop applies the batch, and a key the rc BOUND runs an
	// action that reads the proposal back through `lesh_proposal_read` and stages
	// it. Every seam in that sentence is a different ticket's, and this is the
	// only test that has all of them at once.
	//
	// NO DEFAULT BINDING IS ASSERTED, and none exists: #140 decides which key
	// accepts, and fish's answer overloads a MOTION at end of buffer, which is not
	// a decision to make from here. The rc binds `<C-y>`, which is what a user
	// does today.
	//
	// THE ARITHMETIC IS THE ASSERTION. `42` is nowhere in what is typed and
	// nowhere in what is painted - not in the suggestion's virtual text either -
	// so a second `42` on the wire can only be a second RUN of the remembered
	// line. Had the accept read nothing, `echo $((6*` would have gone to the
	// continuation prompt instead, which is what this test saw while #144 was
	// open.
	//
	// `<C-g>` AND NOT `<C-y>`, which is what fish's accept-ish keys are near:
	// `enter_raw` clears ICANON and ECHO and nothing else, so IEXTEN is still on
	// and the BSD driver eats Ctrl-Y as VDSUSP before any byte reaches the shell.
	// Ctrl-G is no `c_cc` entry on either platform. Whether the editor should
	// clear IEXTEN is #98's question and not this test's.
	const scratch_home home{"PS1='lesh-test>'\nbind '<C-g>' accept_autosuggestion\n"};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("echo $((6*7))\r");
	ASSERT_TRUE(shell.wait_for("42")) << "saw: " << shell.seen();
	ASSERT_TRUE(shell.wait_for(kPrompt, 2)) << "saw: " << shell.seen();

	// A prefix of the remembered line, then the bound key, then Enter.
	shell.type("echo $((6*");
	ASSERT_TRUE(shell.wait_for("echo $((6*")) << "saw: " << shell.seen();
	shell.type("\x07\r");

	EXPECT_TRUE(shell.wait_for("42", 2)) << "the accept never reached the buffer; saw: "
	                                     << shell.seen();
}

TEST(LeshperPty, RawModeClearsIEXTENAndTheExitPutsItBack) {
	// #140 decision 1, and #98's raw mode made complete. On macOS and the BSDs
	// IEXTEN is what makes the driver's extended `c_cc` entries live: Ctrl-Y is
	// VDSUSP, Ctrl-O is VDISCARD, Ctrl-V is VLNEXT, Ctrl-W is VWERASE and Ctrl-R
	// is VREPRINT, and every one of those bytes is eaten before the decoder sees
	// it. fish and zle both clear the bit; so does `enter_raw` now, and which
	// keys a user can bind stops being a fact about the platform.
	//
	// IT RIDES THE ORDINARY RESTORE. There is no second save and no second
	// restore path: `_original` is the whole termios from before the first
	// `enter_raw`, and `leave_raw` and the armed exit restore write it back
	// wholesale - which is why the "after" half of this test is an assertion
	// about a bit nobody wrote code for.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());

	struct termios before{};
	ASSERT_TRUE(shell.modes(before));
	ASSERT_TRUE(extended_input(before)) << "the pty did not start with IEXTEN on";

	ASSERT_TRUE(shell.wait_for(kPrompt));
	struct termios editing{};
	ASSERT_TRUE(shell.modes(editing));
	EXPECT_FALSE(extended_input(editing)) << "the editor left IEXTEN on";
	// And nothing else moved: ISIG is still the driver's job (tty.h's first rule).
	EXPECT_NE(editing.c_lflag & ISIG, 0u);

	shell.type("\x04");
	ASSERT_TRUE(shell.reap().has_value()) << "Ctrl-D did not end the session";

	struct termios after{};
	ASSERT_TRUE(shell.modes(after));
	EXPECT_TRUE(extended_input(after)) << "the exit path did not restore IEXTEN";
	EXPECT_TRUE(is_cooked(after));
}

TEST(LeshperPty, AControlVByteReachesTheShellNowThatIEXTENIsOff) {
	// The bit above, spent. `<C-v>` is VLNEXT on this platform, so before #147
	// the driver swallowed the 0x16 and quoted the byte after it - and the `\r`
	// that follows here would have been inserted as a literal carriage return,
	// sending `echo $((6*` to the continuation prompt instead of running
	// anything. Ctrl-V is the sharpest of the five to test with because its
	// failure mode is not "nothing happened" but "the next key was stolen too".
	//
	// THE ARITHMETIC IS THE ASSERTION, the same way #144's is: `42` is nowhere in
	// what is typed and nowhere in what is painted, so a second `42` on the wire
	// can only be a second RUN of the remembered line - which means the byte
	// reached the keymap, dispatched the action the rc bound, and the action read
	// the proposal back.
	const scratch_home home{"PS1='lesh-test>'\nbind '<C-v>' accept_autosuggestion\n"};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("echo $((6*7))\r");
	ASSERT_TRUE(shell.wait_for("42")) << "saw: " << shell.seen();
	ASSERT_TRUE(shell.wait_for(kPrompt, 2)) << "saw: " << shell.seen();

	shell.type("echo $((6*");
	ASSERT_TRUE(shell.wait_for("echo $((6*")) << "saw: " << shell.seen();
	shell.type("\x16\r");

	EXPECT_TRUE(shell.wait_for("42", 2))
		<< "the Ctrl-V byte never reached the shell; saw: " << shell.seen();
}

TEST(LeshperPty, TheDefaultRightArrowAcceptsTheSuggestionAndTheLineRuns) {
	// #140's table on a real terminal, with NOTHING bound by the rc: `<Right>`
	// is `accept_suggestion_or_forward_char` out of the box, the cursor is at
	// the end of the buffer and a suggestion is showing, so the key accepts and
	// Enter runs what it accepted. This is the switch-on test for the whole
	// ticket - the keymap default, the wrapper, the composed accept and the
	// loop's own apply, all at once.
	//
	// `\x1b[C` is what a terminal sends for the right arrow, and it is written as
	// the bytes rather than as a name because that is what arrives on the wire.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("echo $((6*7))\r");
	ASSERT_TRUE(shell.wait_for("42")) << "saw: " << shell.seen();
	ASSERT_TRUE(shell.wait_for(kPrompt, 2)) << "saw: " << shell.seen();

	shell.type("echo $((6*");
	ASSERT_TRUE(shell.wait_for("echo $((6*")) << "saw: " << shell.seen();
	// The suggestion is computed on a worker, so the ghost text lands a moment
	// after the echo of what was typed. Long enough that it certainly has.
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	shell.type("\x1b[C\r");

	EXPECT_TRUE(shell.wait_for("42", 2))
		<< "the default Right did not accept; saw: " << shell.seen();
}

// ===========================================================================
// What the shell knows, painted (#151, F-21)
// ===========================================================================

TEST(LeshperPty, ABuiltinWithNoBinaryBehindItPaintsAsRunnable) {
	// THE DEFECT #151 FIXED, and the only shape of test that could see it. The
	// highlighter runs on the shell thread (ADR-0009) and its token is built by
	// `shell_actor`; that build copied every field of the snapshot except
	// `knowledge`, so the verb fell back to `environment_knowledge` - empty
	// tables, `getenv("PATH")` - and every name that is ONLY a builtin resolved
	// unknown. `cd` hid it on macOS, which ships `/usr/bin/cd`. `exit` and `bind`
	// have no binary anywhere, so green here means the builtin table was read.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("exit");
	ASSERT_TRUE(shell.wait_for(kRunnable))
		<< "`exit` did not paint as runnable; saw: " << shell.seen();

	// A second word, on a fresh line, so that one lucky repaint is not the whole
	// evidence. Ctrl-C rather than Enter, because Enter would run `exit`.
	shell.type("\x03");
	ASSERT_TRUE(shell.wait_for("^C"));
	const std::size_t runnable = shell.count_of(kRunnable);
	shell.type("bind");
	EXPECT_TRUE(shell.wait_for(kRunnable, runnable + 1))
		<< "`bind` did not paint as runnable; saw: " << shell.seen();
}

TEST(LeshperPty, AnAliasFromTheRcPaintsAsRunnable) {
	// The alias table, reached the same way - and the rc is what puts it there,
	// so this is #101's ordering and #135's door in one line. `zzalias` is not a
	// builtin, not a function and not on anybody's `$PATH`.
	const scratch_home home{kAliasRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("zzalias");
	EXPECT_TRUE(shell.wait_for(kRunnable))
		<< "the rc's alias did not paint as runnable; saw: " << shell.seen();
}

TEST(LeshperPty, TheShellsOwnPathDecidesWhetherAnExternalIsKnown) {
	// #124's case, end to end: it is the SHELL's `PATH` variable that classifies,
	// not the process environment `getenv` would answer with. The assignment is
	// RUN, not merely typed, because a typed prefix assignment has not happened
	// yet - which is the distinction the highlighter is required to respect.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("ls");
	ASSERT_TRUE(shell.wait_for(kRunnable))
		<< "`ls` was not found on the inherited PATH; saw: " << shell.seen();

	shell.type("\x03");
	ASSERT_TRUE(shell.wait_for("^C"));
	const std::size_t runnable = shell.count_of(kRunnable);
	const std::size_t unknown = shell.count_of(kUnknown);

	shell.type("PATH=/nonexistent\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 3));

	shell.type("ls");
	EXPECT_TRUE(shell.wait_for(kUnknown, unknown + 1))
		<< "`ls` did not go unknown under an empty PATH; saw: " << shell.seen();
	EXPECT_EQ(shell.count_of(kRunnable), runnable)
		<< "nothing on this line can be runnable any more; saw: " << shell.seen();
}
