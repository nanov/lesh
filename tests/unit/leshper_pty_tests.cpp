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

// Every test starts the same way, and the rc file proves #101's ordering as a
// side effect: the prompt only says `lesh-test>` because `~/.leshrc` ran before
// the first read.
constexpr std::string_view kRc = "PS1='lesh-test>'\n";

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
