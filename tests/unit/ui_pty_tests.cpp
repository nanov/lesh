#include "ui/history/store.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
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
// `$HOME` IS THE TEMP DIRECTORY, always - and `$XDG_DATA_HOME` is UNSET, which
// since #193 is the other half of the same rule. The shell reads `~/.leshrc` and
// writes `$XDG_DATA_HOME/lesh/`, defaulting to `~/.local/share/lesh/`; a test
// that read the developer's own rc would pass differently on every machine, and
// one that inherited their `$XDG_DATA_HOME` would append to the history they
// use even with `$HOME` redirected.
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
	// Where #193's two-tier history lands under this `$HOME`, given that the
	// child unsets `$XDG_DATA_HOME`.
	[[nodiscard]] std::string history_dir() const { return _dir.file(".local/share/lesh"); }

private:
	lesh::testing::temp_path _dir;
};

// One real shell on one pty.
class shell_on_a_pty {
public:
	// `execution` is `$LESH_EXECUTION` for the child, or null for "leave it
	// unset", which is the default and therefore the execution fiber (#208). The
	// pty tests exec the real binary, so this environment variable is the only
	// door they have onto the host's execution mode - and it is the same door a
	// user would reach for if a fiber stack ever turned out to be the wrong place
	// to fork from.
	explicit shell_on_a_pty(const scratch_home& home, const char* term = "xterm-256color",
	                        const char* execution = nullptr) {
		// A SIZE, EXPLICITLY. `openpty` with no winsize leaves the pty at 0x0 and
		// the shell falls back to tty.h's 80x24 - which was fine while every
		// expectation here was a ten-byte `lesh-test>`, and is not fine now that
		// one of them is a full filesystem path (#157's native prompt). A prompt
		// that wrapped would reach the wire with a line break through the middle of
		// it and no exact match could find it. 24 rows, which is what the fallback
		// gave, so nothing else in this file sees a different screen.
		struct winsize size{};
		size.ws_col = 120;
		size.ws_row = 24;
		if (::openpty(&_master, &_slave, nullptr, nullptr, &size) != 0)
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
			// The redirected `$HOME` only reaches the history if nothing names a
			// data directory outright (#193, ADR-0010 §Placement).
			::unsetenv("XDG_DATA_HOME");
			::unsetenv("LESH_LOG");
			::unsetenv("LESH_LOG_FILE");
			// EXPLICITLY EITHER WAY, never inherited: a developer with
			// `LESH_EXECUTION` set in their own shell must not silently flip every
			// default-mode case in this file.
			if (execution != nullptr)
				::setenv("LESH_EXECUTION", execution, 1);
			else
				::unsetenv("LESH_EXECUTION");
			// AND `$PWD`, so the shell's LOGICAL working directory is its physical
			// one. `shell_state::logical_working_directory` prefers an inherited
			// `$PWD` whenever it still names the current directory, and whatever
			// ctest put there may name it by a different spelling than `getcwd`
			// does. #157's prompt test computes its expectation with `getcwd`; this
			// is what makes the shell answer the same question the same way.
			::unsetenv("PWD");
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

	// A window-size change, which is what dragging the window edge is (#185).
	//
	// THE IOCTL AND THE SIGNAL, both. Setting the master's winsize is what makes
	// the shell's own `TIOCGWINSZ` answer the new size, and the driver raises
	// SIGWINCH on the foreground group for it; the explicit `kill` after it is
	// belt and braces, because a duplicate SIGWINCH costs one no-op resize event
	// and a lost one would make this test hang on a budget instead of failing.
	void resize(unsigned short columns, unsigned short rows = 24) const {
		struct winsize size{};
		size.ws_col = columns;
		size.ws_row = rows;
		::ioctl(_master, TIOCSWINSZ, &size);
		if (_child > 0)
			::kill(_child, SIGWINCH);
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

	// The same wait, ON THE TEXT WITH THE ESCAPE SEQUENCES TAKEN OUT (#201).
	//
	// WHY IT IS NEEDED NOW. A keystroke's highlight used to land a turn after the
	// keystroke, so the first paint of what was typed was UNCOLOURED and the typed
	// bytes appeared contiguously on the wire; the highlighter runs in place now,
	// so the first paint of a word already carries its spans and `echo $((6*`
	// reaches the terminal as `ESC[38;2;...m` `echo` `ESC[39m` ` ` `ESC[...m`
	// `$((6*`. The SCREEN is identical - one paint instead of two - and a test
	// that waits for the echo of what it typed has to say so without depending on
	// which spans the highlighter emitted.
	[[nodiscard]] bool wait_for_uncoloured(std::string_view needle, std::size_t times = 1,
	                                       std::chrono::milliseconds budget =
	                                           std::chrono::seconds{10}) {
		const auto deadline = std::chrono::steady_clock::now() + budget;
		while (count_of_uncoloured(needle) < times
		       && std::chrono::steady_clock::now() < deadline) {
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
		return count_of_uncoloured(needle) >= times;
	}

	// `_seen` with the escape sequences dropped: ESC[ ... final, ESC] ... BEL,
	// and any other two-byte ESC pair. Enough for what a frame of ours contains -
	// SGR runs, ESC[K, cursor moves and the bracketed-paste toggles - and
	// deliberately not a terminal emulator: nothing here replays the moves, so
	// this is the BYTES a user would have seen typed, not the screen.
	[[nodiscard]] std::string uncoloured() const {
		std::string out;
		out.reserve(_seen.size());
		for (std::size_t at = 0; at < _seen.size(); ++at) {
			if (_seen[at] != '\x1b') {
				out.push_back(_seen[at]);
				continue;
			}
			if (at + 1 >= _seen.size())
				break;
			const char kind = _seen[at + 1];
			if (kind == '[') {
				at += 2;
				while (at < _seen.size()
				       && !(_seen[at] >= '@' && _seen[at] <= '~'))
					++at;
			} else if (kind == ']') {
				at += 2;
				while (at < _seen.size() && _seen[at] != '\a')
					++at;
			} else {
				++at;
			}
		}
		return out;
	}

	[[nodiscard]] std::size_t count_of_uncoloured(std::string_view needle) const {
		const std::string plain = uncoloured();
		std::size_t found = 0;
		for (std::size_t at = plain.find(needle); at != std::string::npos;
		     at = plain.find(needle, at + needle.size()))
			++found;
		return found;
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

// #152: what the parent shell inherits when lesh is gone. The question is
// whether a newline followed the last thing the prompt painted - NOT whether
// the wire's last byte is one, because the armed exit restore writes
// `\x1b[?2004l` after everything else and a terminal draws nothing for it and
// moves no cursor for it. fish and zsh both leave column 0 here; a shell that
// does not is what zsh's inverse `%` marker is reporting.
// The pid out of `stopped: pid N (job control not implemented)`, or -1 (#161).
//
// PARSED OFF THE WIRE ON PURPOSE. The shell keeps no job table, so that number is
// the entire interface the user is given for a stopped process - reading it back
// the way a reader would is what makes the report a contract rather than a
// decoration. Digits only, and it refuses a marker with no digits behind it, so a
// message that ever loses its pid fails here rather than turning into `kill 0`.
[[nodiscard]] pid_t pid_in_stopped_report(std::string_view wire) {
	constexpr std::string_view marker = "stopped: pid ";
	const std::size_t at = wire.find(marker);
	if (at == std::string_view::npos)
		return -1;
	std::size_t i = at + marker.size();
	const std::size_t first_digit = i;
	pid_t pid = 0;
	for (; i < wire.size() && wire[i] >= '0' && wire[i] <= '9'; ++i)
		pid = pid * 10 + (wire[i] - '0');
	return i == first_digit ? -1 : pid;
}

// EVERY pid the wire reported stopped, in order (#160). A pipeline is one job and
// Ctrl-Z stops its whole process group, so the report is one line PER MEMBER -
// `pid_in_stopped_report` above answers about the first and would be satisfied by
// a shell that only ever stopped one.
[[nodiscard]] std::vector<pid_t> pids_in_stopped_reports(std::string_view wire) {
	constexpr std::string_view marker = "stopped: pid ";
	std::vector<pid_t> pids;
	for (std::size_t at = wire.find(marker); at != std::string_view::npos;
	     at = wire.find(marker, at + marker.size())) {
		std::size_t i = at + marker.size();
		const std::size_t first_digit = i;
		pid_t pid = 0;
		for (; i < wire.size() && wire[i] >= '0' && wire[i] <= '9'; ++i)
			pid = pid * 10 + (wire[i] - '0');
		if (i != first_digit)
			pids.push_back(pid);
	}
	return pids;
}

// How many prompts the wire carries AFTER the last erase-to-end-of-screen (#185).
//
// THE BYTE STREAM IS ALL THIS HARNESS HAS - there is no terminal emulator on the
// far side of the master, so "what is on screen" is not a question it can ask.
// The erase is the next best thing and is exactly the claim being made: from
// ESC[J onwards the shell has erased everything below the frame's top row and
// repainted, so what follows is the whole of what a terminal would be showing.
// More than one prompt in there is the defect - N resizes leaving N+1 copies.
[[nodiscard]] std::string_view after_the_last_erase(std::string_view wire) {
	constexpr std::string_view erase = "\x1b[J";
	const std::size_t last = wire.rfind(erase);
	return last == std::string_view::npos ? wire : wire.substr(last + erase.size());
}

[[nodiscard]] std::size_t occurrences(std::string_view haystack, std::string_view needle) {
	std::size_t found = 0;
	for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
	     at = haystack.find(needle, at + needle.size()))
		++found;
	return found;
}

[[nodiscard]] std::size_t prompts_after_the_last_erase(std::string_view wire,
                                                       std::string_view prompt) {
	return occurrences(after_the_last_erase(wire), prompt);
}

// The longest PREFIX of the prompt that shows up in the tail more often than the
// whole prompt does - which is to say the longest clipped copy of it (#189).
//
// A fragment is what a shrink used to leave behind: each row of the frame was a
// hard line to the terminal, so it clipped every one of them at the new width
// and `lesh-test>aaa...` became `lesh-tes` somewhere above the live prompt.
// Counting prefixes rather than searching for one particular truncation is what
// makes this indifferent to WHERE the clip fell.
[[nodiscard]] std::size_t longest_clipped_prompt_after_the_last_erase(
	std::string_view wire, std::string_view prompt) {
	const std::string_view tail = after_the_last_erase(wire);
	const std::size_t whole = occurrences(tail, prompt);
	// Four bytes is short enough to catch a clip anywhere in a ten-byte prompt
	// and long enough that `lesh` on its own - a word a session might print for
	// any number of reasons - is not mistaken for one.
	for (std::size_t length = prompt.size(); length >= 4; --length)
		if (occurrences(tail, prompt.substr(0, length)) > whole)
			return length;
	return 0;
}

[[nodiscard]] bool a_newline_follows_the_last_prompt(std::string_view wire,
                                                     std::string_view prompt) {
	const std::size_t last = wire.rfind(prompt);
	if (last == std::string_view::npos)
		return false;
	return wire.find('\n', last) != std::string_view::npos;
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

// What the shipped prompt (#157) looks like ON THE WIRE in a given directory:
// `path_t`'s `$HOME` contraction, then the arrow - and NOT the space after it,
// for `kPrompt`'s reason one screen up. The default table's literal is `"> "`,
// but the blitter erases to end of line rather than emitting a trailing space,
// so the space never reaches the terminal.
//
// RESTATED HERE RATHER THAN REACHED FOR, on purpose. This file execs the real
// binary and shares no code with it; an expectation computed by calling the
// engine would be the shell agreeing with itself, and the whole point of a pty
// test is that it does not get to do that.
[[nodiscard]] std::string contracted_path(std::string_view pwd, std::string_view home) {
	// BY COMPONENT: a sibling whose name merely starts with `$HOME`'s is not
	// inside it, which is the rule `path_t` follows and prompt.h asserts.
	if (!home.empty() && pwd.size() >= home.size() && pwd.substr(0, home.size()) == home
	    && (pwd.size() == home.size() || pwd[home.size()] == '/'))
		return "~" + std::string{pwd.substr(home.size())};
	return std::string{pwd};
}

[[nodiscard]] std::string native_prompt_in(std::string_view pwd, std::string_view home) {
	return contracted_path(pwd, home) + ">";
}

// The rc that gives a session an alias with nothing behind it.
constexpr std::string_view kAliasRc =
	"PS1='lesh-test>'\nalias zzalias='echo aliased'\n";

// A command that answers ONE question from inside whatever job it is part of: is
// my own process group the terminal's foreground group? That is the whole of
// #158 decision 1 as a job can observe it, and #160's scope decision is four
// answers to it - yes for a foreground pipeline and a foreground `( )`, no for
// `&` and for `$( )`.
//
// `ps` AND `$$` RATHER THAN A PYTHON ONE-LINER. `tools/tty_handoff_probe.py`
// asks `os.tcgetpgrp(0) == os.getpgid(0)` because it is already Python; a test
// in the sanitized gate should not acquire an interpreter dependency for two
// integers. `TPGID` is the foreground group of the process's CONTROLLING
// terminal, so it is still readable in a `&` job whose fd 0 is /dev/null, and
// `$$` inside `sh -c` is the pid of the `sh` that IS the job - a pipeline stage
// execs in place, so no fork stands between this and the group being asserted.
//
// `-n "$1"` FIRST, and it is not defensive noise: without it a `ps` that printed
// nothing would leave both words empty, `[ "" = "" ]` would be TRUE, and every
// positive assertion below would pass on a machine where the probe did not run.
// The guard makes a broken probe report `lost`, which fails the positive tests
// and passes the negative ones - the direction that cannot fake a green.
//
// THE MARKER IS COMPUTED, the same discipline as `$((1 + 1))-ran-anyway` above:
// the editor paints the line as it is keyed, so `pipe$((1 + 1))=owns` is on the
// terminal before anything runs and only execution can turn it into `pipe2=owns`.
[[nodiscard]] std::string owns_terminal_probe(std::string_view tag) {
	return std::string{R"(sh -c 'set -- $(ps -o tpgid=,pgid= -p $$); )"
	                   R"([ -n "$1" ] && [ "$1" = "$2" ] && echo )"} +
	       std::string{tag} + R"($((1 + 1))=owns || echo )" + std::string{tag} +
	       R"($((1 + 1))=lost')";
}

} // namespace

TEST(UiPty, APromptAppearsAndACommandRuns) {
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

TEST(UiPty, AShellThatSetsNoPS1PaintsTheNativePrompt) {
	// #157'S FLIP, END TO END, AND FROM THE FIRST PAINT. The owner's ruling is
	// that §6.10's supersession has arrived: a user who never set `$PS1` expressed
	// no preference - the POSIX `$ ` every `shell_state` is born holding is not a
	// choice - so what the session shows is the engine's own default table. The rc
	// file here is EMPTY, which is the case `source_rc` returns early on and the
	// reason the session constructor has to do the first refresh itself.
	//
	// The other half of this rule is every other test in this file: their rc sets
	// `PS1='lesh-test>'`, and that they still see it is what says the opt-out
	// survived.
	const scratch_home home{""};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());

	// The child inherited THIS process's working directory, and its `$HOME` is the
	// scratch directory - which the build tree is not inside - so the contraction
	// finds nothing to contract and the expectation is the whole path.
	const std::string expected =
		native_prompt_in(std::filesystem::current_path().string(), home.path());
	ASSERT_TRUE(shell.wait_for(expected))
		<< "no native prompt; expected " << expected << "; saw: " << shell.seen();
	// AND NOT THE STUB, not even once. `$ ` is what `$PS1` holds and what
	// `options_for` seeded the loop with, so a single `$` anywhere on this wire
	// would mean the stub got painted first and the native prompt only arrived
	// later - which is exactly what happens if the session constructor does not
	// refresh before `run`. The `$` alone, without its space, because the blitter
	// would erase to end of line rather than emit the space.
	EXPECT_EQ(shell.count_of("$"), 0u) << "the POSIX stub was painted; saw: " << shell.seen();

	// A prompt and not a banner: a command runs and it comes back.
	shell.type("echo native-on\r");
	EXPECT_TRUE(shell.wait_for("native-on")) << "saw: " << shell.seen();
	EXPECT_TRUE(shell.wait_for(expected, 2)) << "saw: " << shell.seen();
}

TEST(UiPty, AnRcTemplateSetsThePromptAndBarePromptPrintsItBack) {
	// THE TEMPLATE LANGUAGE END TO END (#157): an rc line, through the shell's own
	// quoting, through the `prompt` builtin, through the console, into the parser,
	// and out as painted bytes. Every layer is the real one - this file execs the
	// binary - so nothing here can pass by the shell agreeing with itself.
	const scratch_home home{"prompt '[{path}]> '\n"};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());

	// The literal runs on either side of the placement, with `path`'s contraction
	// between them - and no trailing space, for the reason `kPrompt` gives: the
	// blitter erases to end of line rather than emitting one.
	const std::string expected =
		"[" + contracted_path(std::filesystem::current_path().string(), home.path()) + "]>";
	ASSERT_TRUE(shell.wait_for(expected))
		<< "no templated prompt; expected " << expected << "; saw: " << shell.seen();

	// And bare `prompt` prints the SOURCE back - the round trip that makes
	// `prompt > f` and re-reading `f` a configuration. It arrives through
	// `printf`, not the blitter, so the trailing space is on the wire this time.
	shell.type("prompt\r");
	EXPECT_TRUE(shell.wait_for("[{path}]> "))
		<< "the template did not come back; saw: " << shell.seen();
}

TEST(UiPty, AnIncompleteLineGetsTheContinuationPromptRatherThanRunning) {
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

TEST(UiPty, AnAcceptedLineIsRecordedInTheHistory) {
	const scratch_home home{kRc};
	{
		shell_on_a_pty shell{home};
		ASSERT_TRUE(shell.alive());
		ASSERT_TRUE(shell.wait_for(kPrompt));
		shell.type("echo remembered\r");
		ASSERT_TRUE(shell.wait_for("remembered"));
		ASSERT_TRUE(shell.wait_for(kPrompt, 2));
		// A command that FAILS, because the exit status is the thing #193's split
		// between `add` and `resolve_pending` exists to capture.
		shell.type("false\r");
		// Ctrl-D at an empty prompt: `end_of_file`, which exits only when there is
		// nothing typed.
		ASSERT_TRUE(shell.wait_for(kPrompt, 2));
		shell.type("\x04");
		EXPECT_TRUE(shell.reap().has_value()) << "Ctrl-D did not end the session";
	}

	// THE WIRING, END TO END (#193). A second store opened over the same data
	// directory is a RESTART: it reads `history.new.log` the way the next shell
	// would, so what this asserts on is what the user's next session will see.
	lesh::ui::history::store storage;
	const lesh::ui::history::open_report report = storage.open(home.history_dir());
	ASSERT_FALSE(report.directory_unusable);
	// EITHER TIER WILL DO, and it has to be said this way since #194: the
	// countdown starts at a random value in `[0, 25)`, so the shell that just
	// exited had one chance in twenty-five of vacuuming these two commands out
	// of the log and into `history.data` on its way through. The walk below is
	// the assertion that matters and is indifferent to which tier answered.
	ASSERT_TRUE(report.log_frames > 0u || report.tier1_mapped)
		<< "nothing was written to either history tier";

	std::vector<std::string> entries;
	std::vector<std::int32_t> statuses;
	storage.for_each_merged_newest_first([&](const lesh::ui::history::merged_entry& one) {
		entries.emplace_back(reinterpret_cast<const char*>(one.what.cmd.data()),
		                     one.what.cmd.size());
		statuses.push_back(one.what.exit_code);
		return true;
	});
	ASSERT_FALSE(entries.empty());
	// Newest first, and the exit status came back through `resolve_pending` -
	// which is the half of the recording that only exists because `session::execute`
	// splits the add from the resolve around the wait.
	EXPECT_EQ(entries.front(), "false");
	EXPECT_EQ(statuses.front(), 1);
	ASSERT_GE(entries.size(), 2u);
	EXPECT_EQ(entries[1], "echo remembered");
	EXPECT_EQ(statuses[1], 0);
}

TEST(UiPty, ALeadingSpaceKeepsACommandOutOfTheHistoryFile) {
	// fish's privacy rule, through the real binary (#193, ADR-0010 §In memory).
	// The unit tests prove the store never writes an ephemeral item; this proves
	// the shell hands it to the store as one.
	const scratch_home home{kRc};
	{
		shell_on_a_pty shell{home};
		ASSERT_TRUE(shell.alive());
		ASSERT_TRUE(shell.wait_for(kPrompt));
		shell.type(" echo secret\r");
		ASSERT_TRUE(shell.wait_for("secret"));
		ASSERT_TRUE(shell.wait_for(kPrompt, 2));
		shell.type("echo ordinary\r");
		ASSERT_TRUE(shell.wait_for("ordinary"));
		ASSERT_TRUE(shell.wait_for(kPrompt, 2));
		shell.type("\x04");
		EXPECT_TRUE(shell.reap().has_value()) << "Ctrl-D did not end the session";
	}

	lesh::ui::history::store storage;
	ASSERT_FALSE(storage.open(home.history_dir()).directory_unusable);
	std::vector<std::string> entries;
	storage.for_each_newest_first([&](std::string_view entry) {
		entries.emplace_back(entry);
		return true;
	});
	EXPECT_EQ(entries, (std::vector<std::string>{"echo ordinary"}));
}

TEST(UiPty, ControlCCancelsTheLineAndLeavesTheStatusAt130) {
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

TEST(UiPty, TheIntTrapFiresAtThePromptTheZshWay) {
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

TEST(UiPty, AnIgnoredIntLeavesTheLineAloneAtThePrompt) {
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

TEST(UiPty, ADefaultHangupKillsTheShell) {
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

TEST(UiPty, AChildTrapFiresAfterACommand) {
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

TEST(UiPty, ControlCDuringAForegroundCommandStillYields130AndFiresTheTrap) {
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

TEST(UiPty, ControlCDuringAForegroundCommandAbandonsTheRestOfTheLine) {
	// THE OTHER HALF of the case above, and the half nothing was watching. With no
	// trap set, #52's interactive default is what answers SIGINT: the shell stops
	// running the line rather than merely surviving it. `sleep 5; echo after` must
	// print nothing after the Ctrl-C - dash, zsh and bash all agree, and lesh
	// agreed too before #159 handed the terminal to the child.
	//
	// That handoff is exactly what put this at risk: the child's group becomes the
	// terminal's foreground group, so the keyboard interrupt no longer reaches the
	// shell at all, and BOTH of the dispositions' answers - the trap body above,
	// and this unwind - stop happening unless the reap synthesizes the delivery.
	// The first version of #159's synthesis covered only the trap, `echo after`
	// ran, and no test in this file noticed. This is that test.
	//
	// THE MARKER IS COMPUTED, not typed. The editor renders the line as it is
	// keyed, so any literal in the command text is already on the terminal before
	// anything runs; `$((1 + 1))` is on screen as five characters and can only
	// become `2` by being executed.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("sleep 5; echo $((1 + 1))-ran-anyway\r");
	// Long enough that `sleep` is certainly the foreground job and the editor
	// parked, which is what makes this the during-a-command path and not the
	// at-the-prompt one the cancel-line tests cover.
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	shell.type("\x03");

	// A second prompt is the line being over, however it ended. Waiting on it
	// rather than on a timeout is what keeps the absence assertion below honest:
	// `echo` would have run long before the shell asked for input again.
	ASSERT_TRUE(shell.wait_for(kPrompt, 2)) << "no prompt after the interrupt; saw: " << shell.seen();
	EXPECT_EQ(shell.count_of("2-ran-anyway"), 0u)
		<< "the interrupt did not abandon the rest of the line; saw: " << shell.seen();

	shell.type("echo status=$?\r");
	EXPECT_TRUE(shell.wait_for("status=130")) << "saw: " << shell.seen();
}

TEST(UiPty, ControlZStopsTheForegroundCommandAndReturnsThePrompt) {
	// #161, and the whole of it: #159 reset SIGTSTP to default in the child and
	// gave it the terminal, so the suspend character now genuinely stops a
	// foreground external - and every foreground `waitpid(pid, &st, 0)` in the
	// executor blocked forever on a child that was never going to exit. WITHOUT
	// THE FIX THIS TEST HANGS, which is why every assertion below is a budgeted
	// `wait_for` and not a sleep: the harness turns the hang into a failed
	// expectation with the transcript attached, rather than leaving it for ctest's
	// global timeout to notice.
	//
	// `\x1a` IS THE TTY DRIVER'S VSUSP, not a key the editor binds. It only means
	// SIGTSTP while the terminal is cooked and ISIG is on, which is exactly the
	// state the loop restores before running a command - so this reaches the
	// foreground process group the same way the Ctrl-C tests above reach it, and
	// tests the during-a-command path rather than the at-the-prompt one.
	//
	// `sleep 30` and not `sleep 5`: the assertions below must run against a
	// process that is still there, and a 5-second command could exit on its own
	// under a loaded machine and pass this test for the wrong reason.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("sleep 30\r");
	// Long enough that `sleep` is certainly the foreground job and the editor
	// parked - the same wait the two Ctrl-C tests above take, for the same reason.
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	shell.type("\x1a");

	ASSERT_TRUE(shell.wait_for("stopped: pid "))
		<< "no stopped report - the shell is still waiting on a stopped child; saw: " << shell.seen();
	ASSERT_TRUE(shell.wait_for(kPrompt, 2))
		<< "no prompt after the stop; saw: " << shell.seen();

	// 128 + WSTOPSIG, which for the suspend character is SIGTSTP. Computed rather
	// than typed: `status=` is on the wire the moment it is keyed, so only the
	// digits can distinguish a shell that ran the command from one that echoed it.
	shell.type("echo status=$?\r");
	EXPECT_TRUE(shell.wait_for("status=" + std::to_string(128 + SIGTSTP)))
		<< "$? is not 128+SIGTSTP after the stop; saw: " << shell.seen();

	// THE PID IN THE REPORT IS THE ONLY HANDLE THE USER IS GIVEN - there is no job
	// table and no `fg`, which is what the parenthesis in the message says - so
	// reading it back off the wire is the test standing exactly where the user
	// stands. `kill -CONT` of it is the documented way out, and it can only work if
	// the process is still alive and stopped rather than reaped or killed.
	const pid_t stopped = pid_in_stopped_report(shell.seen());
	ASSERT_GT(stopped, 0) << "no pid in the stopped report; saw: " << shell.seen();
	EXPECT_EQ(::kill(stopped, 0), 0)
		<< "the reported process is gone, so it was never stopped; saw: " << shell.seen();

	shell.type("kill -CONT " + std::to_string(stopped) + "; echo cont=$?-$((1 + 1))\r");
	EXPECT_TRUE(shell.wait_for("cont=0-2"))
		<< "`kill -CONT` of the stopped pid did not succeed from a usable prompt; saw: "
		<< shell.seen();

	// Cleanup, and the reason the command above is `sleep 30`: continued, it would
	// outlive this test by half a minute. Through the shell first, so the prompt is
	// asserted usable a second time, and then directly - the shell keeps no job
	// table, so nothing else is going to.
	shell.type("kill -KILL " + std::to_string(stopped) + "\r");
	EXPECT_TRUE(shell.wait_for(kPrompt, 5)) << "saw: " << shell.seen();
	::kill(stopped, SIGKILL);
}

TEST(UiPty, AForegroundPipelineHandsTheTerminalToTheWholeJob) {
	// #160, and the case the acceptance criteria call `ls | less`: a pipeline is ONE
	// foreground job, so the terminal goes to its process group and every stage is
	// in that group. Only a pager can show a human that it worked; what a test can
	// assert is the mechanism underneath it, which is that the LAST stage - the one
	// that never made a handoff call and only joined the leader's group - finds
	// itself the terminal's foreground group.
	//
	// THE LAST STAGE ON PURPOSE. The leader owning the terminal proves only that
	// `tcsetpgrp` was called; the last stage owning it proves the thing #160
	// actually decided, that one handoff to the group leader covers the job. A
	// per-stage handoff and a single one are indistinguishable from the leader.
	//
	// `sleep 1` FIRST so the leader is still alive while the probe runs. With
	// `echo x |` the leader would usually be a zombie by then and the test would be
	// asserting against a group whose leader had already gone - true here, but for
	// a reason that has nothing to do with what is being tested.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("sleep 1 | " + owns_terminal_probe("pipe") + "\r");
	EXPECT_TRUE(shell.wait_for("pipe2=owns"))
		<< "the last stage of a foreground pipeline is not the terminal's foreground group; saw: "
		<< shell.seen();
}

TEST(UiPty, AForegroundSubshellHandsTheTerminalToTheCommandInside) {
	// #160's other half, and the acceptance criterion `(nvim .)` takes the screen.
	//
	// WHAT MAKES THIS DIFFERENT FROM THE PIPELINE is why `enter_subshell` needed a
	// role at all. A subshell does not exec: the command inside it forks AGAIN,
	// into a process group of its own, so no handoff the subshell could make would
	// reach it. Only the saved tty fd SURVIVING into the subshell lets that inner
	// fork hand the terminal over, and this assertion is what says it survived -
	// against a shell that cleared it, the probe reports `lost` and nvim would be
	// running blind in a background group, which is the whole #158 defect.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("(" + owns_terminal_probe("sub") + ")\r");
	EXPECT_TRUE(shell.wait_for("sub2=owns"))
		<< "the command inside a foreground ( ) is not the terminal's foreground group; saw: "
		<< shell.seen();
}

TEST(UiPty, ABackgroundJobNeverBecomesTheTerminalsForegroundGroup) {
	// THE NEGATIVE SPACE, ASSERTED RATHER THAN ASSUMED (#158 decision 3, #160).
	// This is the half a widening change can silently break: `&` reaches the very
	// same `run_simple_command` a foreground command does, one fork further down,
	// and the only thing standing between it and the terminal is `enter_subshell`
	// clearing the saved fd. #160 turned that clearing into a decision with two
	// answers, so "the default is still clear" needs a test rather than a comment.
	//
	// A background job's fd 0 is /dev/null by POSIX, which is why the probe reads
	// TPGID from the CONTROLLING terminal instead of from a descriptor - the
	// question is whether the group was made foreground, not what it can read.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type(owns_terminal_probe("bg") + " &\r");
	ASSERT_TRUE(shell.wait_for("bg2="))
		<< "the background probe never reported at all; saw: " << shell.seen();
	EXPECT_TRUE(shell.wait_for("bg2=lost"))
		<< "a background job took the terminal; saw: " << shell.seen();
	EXPECT_EQ(shell.count_of("bg2=owns"), 0u)
		<< "a background job took the terminal; saw: " << shell.seen();
}

TEST(UiPty, ACommandSubstitutionNeverBecomesTheTerminalsForegroundGroup) {
	// The other negative, and the one with a reader on the other end of it: the
	// LINE EDITOR is reading that terminal while a substitution runs, so a
	// `$(read x)` that became the foreground group would take the user's keystrokes
	// away from the prompt they were typed at. #158 decision 3 names it as its own
	// bug, and it stays one.
	//
	// The probe's output is CAPTURED by the substitution rather than written to the
	// terminal, so it is echoed back out - which also proves the substitution ran
	// at all, making the absence of `owns` an assertion about the handoff rather
	// than about a command that never happened.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("echo captured-$(" + owns_terminal_probe("cs") + ")\r");
	ASSERT_TRUE(shell.wait_for("captured-cs2="))
		<< "the substitution never ran; saw: " << shell.seen();
	EXPECT_TRUE(shell.wait_for("captured-cs2=lost"))
		<< "a command substitution took the terminal from the editor; saw: " << shell.seen();
	EXPECT_EQ(shell.count_of("cs2=owns"), 0u)
		<< "a command substitution took the terminal from the editor; saw: " << shell.seen();
}

TEST(UiPty, ControlZStopsEveryStageOfAForegroundPipeline) {
	// THE REVIEW DEFECT IN #160's FIRST CUT, as a test. The terminal handoff is one
	// per process GROUP - the pipeline's leader makes it for the whole job - but
	// #158 decision 4's signal reset is one per PROCESS THAT EXECS, and while both
	// lived in one function only the leader got either. So stages 2..N exec'd with
	// SIGTTOU, SIGTTIN and SIGTSTP still SIG_IGN, inherited from the editing loop:
	// `ls | less` handed `less` a SIGTSTP it could not act on, and this case stopped
	// the first `sleep` while the second ignored the stop and the shell blocked in
	// its wait for the remaining thirty seconds.
	//
	// WITHOUT THE FIX THIS TEST FAILS BY BLOCKING into the harness budget rather
	// than by asserting something false, which is why every step is a budgeted
	// `wait_for`: the second stage never stops, so the second report never comes and
	// no prompt does either.
	//
	// TWO REPORTS IS THE ASSERTION, not one. A shell that stopped only the leader
	// would satisfy `pid_in_stopped_report` and the #161 test next door, which is
	// exactly how this survived review of the first cut.
	//
	// `sleep 30` twice, for #161's reason: the assertions below run against
	// processes that must still be there, and a short command could exit on its own
	// under load and pass this for the wrong reason.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("sleep 30 | sleep 30\r");
	// Long enough that both stages are certainly running and the editor parked - the
	// same wait every during-a-command test in this file takes.
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	shell.type("\x1a");

	// `times = 2`: the harness counts occurrences rather than searching, so this
	// waits for a SECOND report instead of being satisfied by the first.
	ASSERT_TRUE(shell.wait_for("stopped: pid ", 2))
		<< "only one stage of the pipeline stopped - the others exec'd with the shell's "
		   "ignored SIGTSTP and the shell is still waiting on them; saw: " << shell.seen();
	ASSERT_TRUE(shell.wait_for(kPrompt, 2))
		<< "no prompt after the stop; saw: " << shell.seen();

	// 128 + WSTOPSIG for the suspend character, composed from the members the same
	// way any pipeline status is - here every member stopped, so the last one decides
	// and it decides the same as the rest.
	shell.type("echo status=$?\r");
	EXPECT_TRUE(shell.wait_for("status=" + std::to_string(128 + SIGTSTP)))
		<< "$? is not 128+SIGTSTP after the pipeline stopped; saw: " << shell.seen();

	// BOTH PIDS ARE REAL AND BOTH ARE STOPPED. The pids are the only handles the
	// user is given - no job table, which is what the report's parenthesis says - so
	// reading them back off the wire is the test standing where the user stands.
	const std::vector<pid_t> stopped = pids_in_stopped_reports(shell.seen());
	ASSERT_GE(stopped.size(), 2u)
		<< "fewer than two pids in the stopped reports; saw: " << shell.seen();
	EXPECT_NE(stopped[0], stopped[1]) << "the same pid was reported twice";
	for (const pid_t pid : stopped) {
		EXPECT_GT(pid, 0);
		EXPECT_EQ(::kill(pid, 0), 0)
			<< "reported pid " << pid << " is gone, so it was never stopped; saw: "
			<< shell.seen();
	}

	// Cleanup, and the reason the commands are `sleep 30`: continued, they would
	// outlive this test by half a minute. `kill -CONT` first, so the prompt is
	// asserted usable afterwards and the documented way out is exercised on a
	// pipeline as well as on a simple command; then SIGKILL, because the shell keeps
	// no job table and nothing else is going to.
	//
	// ONE LINE, one `wait_for`, exactly as the simple-command case next door does
	// it - `kill` takes every pid at once. Typing a second line before the prompt
	// for the first has come back is what this file's budgeted waits exist to
	// avoid, and staying prompt-synchronized is this file's convention.
	std::string cleanup = "kill -CONT";
	for (const pid_t pid : stopped)
		cleanup += " " + std::to_string(pid);
	shell.type(cleanup + "; echo cont=$?-$((1 + 1))\r");
	EXPECT_TRUE(shell.wait_for("cont=0-2"))
		<< "`kill -CONT` of the stopped pids did not succeed from a usable prompt; saw: "
		<< shell.seen();
	for (const pid_t pid : stopped)
		::kill(pid, SIGKILL);
}

TEST(UiPty, ControlCDuringAForegroundPipelineYields130AndAbandonsTheLine) {
	// `sleep 5 | cat` STILL DIES TO A SINGLE CTRL-C, which is an acceptance
	// criterion precisely because #160 is what puts it at risk. Handing the
	// pipeline's group the terminal excludes the shell from the keyboard interrupt,
	// so the reap has to synthesize the delivery the way #159's does for a simple
	// command - and if it does not, the shell reports 130 and then calmly runs the
	// rest of the line, which dash, zsh and bash all refuse to do (measured on this
	// machine) and which lesh refused to do before the pipeline had a handoff.
	//
	// THE STATUS AND THE ABANDON ARE ONE CONTRACT, not two assertions that happen
	// to sit together: the member whose status became the pipeline's is the member
	// the interrupt note asks about, so `$?` being 130 from a kill is exactly what
	// abandons the line. Asserting only one of them would let the pair drift.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("sleep 5 | cat; echo $((1 + 1))-pipe-ran-anyway\r");
	// Long enough that the pipeline is certainly the foreground job and the editor
	// parked - the same wait the simple-command Ctrl-C tests above take.
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	shell.type("\x03");

	// A second prompt is the line being over, however it ended, and waiting on it
	// rather than on a timeout is what keeps the absence assertion honest.
	ASSERT_TRUE(shell.wait_for(kPrompt, 2))
		<< "no prompt after the interrupt - the shell is still waiting on the pipeline; saw: "
		<< shell.seen();
	EXPECT_EQ(shell.count_of("2-pipe-ran-anyway"), 0u)
		<< "the interrupt did not abandon the rest of the line; saw: " << shell.seen();

	shell.type("echo status=$?\r");
	EXPECT_TRUE(shell.wait_for("status=130")) << "saw: " << shell.seen();
}

TEST(UiPty, ControlCDuringAForegroundPipelineFiresTheIntTrap) {
	// The sibling of the case above and of #159's own trap test, for the job shape
	// #160 adds. Measured on this machine with `trap 'echo T' INT; sleep 5 | cat`
	// and Ctrl-C on a pty: dash fires the trap, zsh fires it, bash does not. That
	// is the same three-shell split #159 resolved for the simple command, so it
	// gets the same answer - ADR-0001 makes dash the POSIX floor and #98 decision 3
	// is the owner's override adopting zsh's INT-trap visibility, and bash's
	// silence is the divergence that decision declined by name.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("trap 'echo caught-int' INT\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 2));

	shell.type("sleep 5 | cat\r");
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	shell.type("\x03");

	EXPECT_TRUE(shell.wait_for("caught-int"))
		<< "the INT trap did not fire for a foreground pipeline; saw: " << shell.seen();
}

TEST(UiPty, ControlCDuringAForegroundSubshellYields130AndAbandonsTheLine) {
	// The same contract for the same reason one construct over - and the one that
	// needed more than a call to `note_interrupt_after_handoff` to keep.
	//
	// A `( )` puts TWO processes between the keyboard and the shell. The interrupt
	// reaches the command inside the subshell, which dies of it; the SUBSHELL is
	// excluded too, and it has no interactive default left - `enter_subshell` drops
	// #52's, because a subshell is not the process that reads commands - so the
	// pending flag it records for itself would be dropped on the floor, the
	// subshell would exit 130 of its own accord, and the parent's note keys on
	// WIFSIGNALED and would see an ordinary exit. `echo after` would run.
	//
	// So the subshell takes SIGINT's REAL DEFAULT ACTION and dies of it, which is
	// what the kernel would have done had the terminal not moved, and the only
	// thing the waiting parent can tell apart from `exit 130`. dash, zsh and bash
	// all abandon the line here (measured on this machine), and so did lesh before
	// the subshell had a handoff to be excluded by.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("(sleep 5); echo $((1 + 1))-sub-ran-anyway\r");
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	shell.type("\x03");

	ASSERT_TRUE(shell.wait_for(kPrompt, 2))
		<< "no prompt after the interrupt - the shell is still waiting on the subshell; saw: "
		<< shell.seen();
	EXPECT_EQ(shell.count_of("2-sub-ran-anyway"), 0u)
		<< "the interrupt did not abandon the rest of the line; saw: " << shell.seen();

	shell.type("echo status=$?\r");
	EXPECT_TRUE(shell.wait_for("status=130")) << "saw: " << shell.seen();
}

TEST(UiPty, ControlDAtAnEmptyPromptExitsAndRestoresTheTerminal) {
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

TEST(UiPty, TheExitBuiltinEndsTheSessionWithItsStatus) {
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

TEST(UiPty, AnAcceptedExitPaintsNoSecondPromptAndLeavesAFreshLine) {
	// #152, both halves at once. `exit` is honoured by the SHELL thread, which
	// answers the `execute` slot and only then says the session is over - so the
	// loop used to unpark, repaint a prompt for a line that will never be typed,
	// and exit out from under it, leaving the parent's cursor parked after
	// `lesh-test>`. ONE prompt for the whole session is the assertion: the
	// blitter diffs, so typing `exit` re-emits no prompt, and a second
	// occurrence can only be a repaint that should not have happened.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("exit\r");
	const std::optional<int> status = shell.reap();
	ASSERT_TRUE(status.has_value()) << "`exit` did not end the session; saw: " << shell.seen();
	ASSERT_TRUE(WIFEXITED(*status));
	EXPECT_EQ(WEXITSTATUS(*status), 0);

	EXPECT_EQ(shell.count_of(kPrompt), 1u)
		<< "a prompt was painted after the accepted `exit`; saw: " << shell.seen();
	EXPECT_TRUE(a_newline_follows_the_last_prompt(shell.seen(), kPrompt))
		<< "`exit` left the cursor mid-line; saw: " << shell.seen();
}

TEST(UiPty, AnExitWithAStatusAlsoLeavesAFreshLine) {
	// The same path with a status on it: `exit N` is still an accepted line, and
	// the status is the shell's answer, not a different exit sequence.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("exit 3\r");
	const std::optional<int> status = shell.reap();
	ASSERT_TRUE(status.has_value()) << "`exit 3` did not end the session; saw: " << shell.seen();
	ASSERT_TRUE(WIFEXITED(*status));
	EXPECT_EQ(WEXITSTATUS(*status), 3);

	EXPECT_EQ(shell.count_of(kPrompt), 1u)
		<< "a prompt was painted after the accepted `exit 3`; saw: " << shell.seen();
	EXPECT_TRUE(a_newline_follows_the_last_prompt(shell.seen(), kPrompt))
		<< "`exit 3` left the cursor mid-line; saw: " << shell.seen();
}

TEST(UiPty, ControlDAtAnEmptyPromptLeavesAFreshLine) {
	// The EOF half. Nothing is accepted here and nothing runs, so no `\r\n` was
	// ever written: the cursor is sitting just after `lesh-test>` when the loop
	// tears down, and the teardown is the only side that can move it.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("\x04");
	const std::optional<int> status = shell.reap();
	ASSERT_TRUE(status.has_value()) << "Ctrl-D did not end the session; saw: " << shell.seen();
	ASSERT_TRUE(WIFEXITED(*status));

	EXPECT_EQ(shell.count_of(kPrompt), 1u)
		<< "a prompt was painted after the EOF; saw: " << shell.seen();
	EXPECT_TRUE(a_newline_follows_the_last_prompt(shell.seen(), kPrompt))
		<< "Ctrl-D left the cursor mid-line; saw: " << shell.seen();
}

TEST(UiPty, DyingOnTheAssertPathStillRestoresTheTerminal) {
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

TEST(UiPty, ATerminalBelowTheFloorIsRefusedInOneLine) {
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

TEST(UiPty, ABoundKeyAcceptsTheSuggestionTheLoopApplied) {
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
	// THE ECHO OF WHAT WAS TYPED, UNCOLOURED (#201). Still the first occurrence -
	// the run above typed its whole line and its Enter in one go, so that line was
	// accepted inside the same turn and never painted - and uncoloured because the
	// highlighter's spans are in the first paint of a word now, which puts an SGR
	// run between `echo` and `$((6*`. See `wait_for_uncoloured`.
	ASSERT_TRUE(shell.wait_for_uncoloured("echo $((6*")) << "saw: " << shell.seen();
	shell.type("\x07\r");

	EXPECT_TRUE(shell.wait_for("42", 2)) << "the accept never reached the buffer; saw: "
	                                     << shell.seen();
}

TEST(UiPty, RawModeClearsIEXTENAndTheExitPutsItBack) {
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

TEST(UiPty, AControlVByteReachesTheShellNowThatIEXTENIsOff) {
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
	// THE ECHO OF WHAT WAS TYPED, UNCOLOURED (#201). Still the first occurrence -
	// the run above typed its whole line and its Enter in one go, so that line was
	// accepted inside the same turn and never painted - and uncoloured because the
	// highlighter's spans are in the first paint of a word now, which puts an SGR
	// run between `echo` and `$((6*`. See `wait_for_uncoloured`.
	ASSERT_TRUE(shell.wait_for_uncoloured("echo $((6*")) << "saw: " << shell.seen();
	shell.type("\x16\r");

	EXPECT_TRUE(shell.wait_for("42", 2))
		<< "the Ctrl-V byte never reached the shell; saw: " << shell.seen();
}

TEST(UiPty, TheDefaultRightArrowAcceptsTheSuggestionAndTheLineRuns) {
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
	// THE ECHO OF WHAT WAS TYPED, UNCOLOURED (#201). Still the first occurrence -
	// the run above typed its whole line and its Enter in one go, so that line was
	// accepted inside the same turn and never painted - and uncoloured because the
	// highlighter's spans are in the first paint of a word now, which puts an SGR
	// run between `echo` and `$((6*`. See `wait_for_uncoloured`.
	ASSERT_TRUE(shell.wait_for_uncoloured("echo $((6*")) << "saw: " << shell.seen();
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

TEST(UiPty, ABuiltinWithNoBinaryBehindItPaintsAsRunnable) {
	// THE DEFECT #151 FIXED, and the only shape of test that could see it. The
	// highlighter runs where shell state is owned (ADR-0009) and its token was
	// built by `shell_actor`, on the far side of a thread; that build copied every
	// field of the snapshot except
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

TEST(UiPty, AnAliasFromTheRcPaintsAsRunnable) {
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

TEST(UiPty, TheShellsOwnPathDecidesWhetherAnExternalIsKnown) {
	// #124's case, end to end: it is the SHELL's `PATH` variable that classifies,
	// not the process environment `getenv` would answer with. The assignment is
	// RUN, not merely typed, because a typed prefix assignment has not happened
	// yet - which is the distinction the highlighter is required to respect.
	//
	// `grep` AND NOT `ls`, since #165. This case needs a name that is an EXTERNAL
	// COMMAND AND NOTHING ELSE, and `ls` stopped being one interactively the day
	// leshnici's extension builtins arrived: `set -o leshnici` defaults on when
	// the shell is interactive, and a builtin is runnable whatever `PATH` says -
	// which is the correct answer and a useless subject for this test. `grep` is
	// on PATH, in no builtin table, and not something this tree plans to ship.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("grep");
	ASSERT_TRUE(shell.wait_for(kRunnable))
		<< "`grep` was not found on the inherited PATH; saw: " << shell.seen();

	shell.type("\x03");
	ASSERT_TRUE(shell.wait_for("^C"));
	const std::size_t runnable = shell.count_of(kRunnable);
	const std::size_t unknown = shell.count_of(kUnknown);

	shell.type("PATH=/nonexistent\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 3));

	shell.type("grep");
	EXPECT_TRUE(shell.wait_for(kUnknown, unknown + 1))
		<< "`grep` did not go unknown under an empty PATH; saw: " << shell.seen();
	EXPECT_EQ(shell.count_of(kRunnable), runnable)
		<< "nothing on this line can be runnable any more; saw: " << shell.seen();
}

TEST(UiPty, AnExtensionBuiltinIsRunnableInAnInteractiveShellWithNoPath) {
	// THE OTHER HALF OF THE CASE ABOVE, and #165's default asserted where it is
	// actually decided: `src/main.cpp` turns `leshnici` on iff the shell is
	// interactive, so a shell reached over a pty has `ls` in its command search
	// and a `lesh -c` does not. `PATH=/nonexistent` is what makes this an
	// assertion about the builtin table rather than about what is in /bin.
	const scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type("PATH=/nonexistent\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 2));

	shell.type("ls");
	EXPECT_TRUE(shell.wait_for(kRunnable))
		<< "`ls` did not paint as a builtin with the option on by default; saw: "
		<< shell.seen();
}

// #189: growing the window after shrinking it. The wire is the only thing this
// harness has, so what these two assert is the pair of shapes the defect made -
// a second whole prompt (the frame appended rather than replaced, #185) and a
// clipped one (each row painted as a hard line, so the terminal truncated them
// separately on the way down and never rejoined them on the way up).

TEST(UiPty, GrowingAfterShrinkingLeavesOneCopyOfThePrompt) {
	// THE CASE #185's PTY TEST DID NOT END ON. It resized down and then part of
	// the way back up, and stopped there; the screenshots on #189 are of the
	// last leg - back to the width the line was typed at, where the previous
	// frame's top row stayed behind because the terminal had never joined it to
	// the row below it.
	scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type(std::string(130, 'a'));
	ASSERT_TRUE(shell.wait_for("aaaaaaaaaa"));

	const std::size_t before = shell.count_of(kPrompt);
	shell.resize(60);
	ASSERT_TRUE(shell.wait_for(kPrompt, before + 1)) << shell.seen();
	shell.resize(120);
	ASSERT_TRUE(shell.wait_for(kPrompt, before + 2)) << shell.seen();

	EXPECT_EQ(prompts_after_the_last_erase(shell.seen(), kPrompt), 1u)
		<< "the grow left a second copy of the prompt on screen";
	EXPECT_EQ(longest_clipped_prompt_after_the_last_erase(shell.seen(), kPrompt), 0u)
		<< "a clipped fragment of the prompt survived the shrink";
	// AND THE SHAPE OF THE PAINT, which is the part of #189 a byte stream CAN
	// answer. A hundred and forty cells at a hundred and twenty columns is two
	// rows and the second is a soft wrap of the first, so a vertical move in the
	// repaint is a soft row painted as a hard line - the defect itself, before
	// the terminal has had a chance to show it.
	EXPECT_EQ(occurrences(after_the_last_erase(shell.seen()), "\x1b[1B"), 0u)
		<< "the repaint moved between two soft-wrapped rows";
}

TEST(UiPty, ADragOfTenSizesLeavesOneCopyOfThePrompt) {
	// A DRAG, which is what a user actually does: a burst of SIGWINCHes, most of
	// them landing while the repaint for the previous one is still going out.
	// Every intermediate width is a different wrap of the same line, so this is
	// the write-through and the walk-up being asked to agree fourteen times in a
	// row rather than once.
	scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	shell.type(std::string(130, 'a'));
	ASSERT_TRUE(shell.wait_for("aaaaaaaaaa"));

	// Down and back up. The master is drained between steps - `wait_for` on a
	// needle that cannot occur reads for its whole budget and gives up - because
	// nothing else is reading it and a full pty buffer would stall the shell
	// mid-drag, which is a hang and not a failure.
	constexpr std::string_view never = "\x01\x02";
	for (const unsigned short columns :
	     {110, 100, 90, 80, 70, 60, 50, 60, 70, 80, 90, 100, 110}) {
		shell.resize(columns);
		(void)shell.wait_for(never, 1, std::chrono::milliseconds{20});
	}

	// And the last step on its own, so there is something to wait FOR: every
	// repaint re-emits the prompt, so the count going up is the drag having
	// been rendered rather than a sleep hoping it was.
	const std::size_t before = shell.count_of(kPrompt);
	shell.resize(120);
	ASSERT_TRUE(shell.wait_for(kPrompt, before + 1)) << shell.seen();

	EXPECT_EQ(prompts_after_the_last_erase(shell.seen(), kPrompt), 1u)
		<< "the drag left more than one copy of the prompt on screen";
	EXPECT_EQ(longest_clipped_prompt_after_the_last_erase(shell.seen(), kPrompt), 0u)
		<< "a clipped fragment of the prompt survived the drag";
	EXPECT_EQ(occurrences(after_the_last_erase(shell.seen()), "\x1b[1B"), 0u)
		<< "the repaint moved between two soft-wrapped rows";
}

TEST(UiPty, ResizingTwiceMidLineLeavesOneCopyOfThePromptAndNotThree) {
	// #185, END TO END. The bug was a copy of the prompt and the buffer left
	// behind by every resize: the repaint started from where the cursor was -
	// the end of the buffer - instead of from the top of the frame the terminal
	// was showing, and nothing erased the old rows. Two resizes made three
	// copies, which is what the screenshots on the ticket show.
	scratch_home home{kRc};
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt));

	// LONGER THAN THE PTY IS WIDE (120), so the frame is more than one row and
	// the walk back up to its top is a real one rather than a no-op. Typed as
	// one write: the editor coalesces it into one edit and one repaint.
	shell.type(std::string(130, 'a'));
	ASSERT_TRUE(shell.wait_for("aaaaaaaaaa"));

	// NARROWER, THEN WIDER. Each repaint re-emits the prompt, so the count on
	// the wire going up by one is how this waits for the resize to have been
	// rendered rather than sleeping for it.
	const std::size_t before = shell.count_of(kPrompt);
	shell.resize(60);
	ASSERT_TRUE(shell.wait_for(kPrompt, before + 1)) << shell.seen();
	shell.resize(100);
	ASSERT_TRUE(shell.wait_for(kPrompt, before + 2)) << shell.seen();

	EXPECT_EQ(prompts_after_the_last_erase(shell.seen(), kPrompt), 1u)
		<< "the resize repaint appended a frame instead of replacing one";
}

// ===========================================================================
// EXECUTION MODES, ON THE REAL BINARY (#208)
// ===========================================================================
//
// Every case in this file above runs the DEFAULT mode, which is the execution
// fiber; these run each case both ways, because the owner's requirement on this
// ticket is that the inline path stays first-class and a path that is not
// exercised is a path that does not work.
//
// AND THIS IS WHERE FORKING FROM A FIBER STACK IS ON TRIAL. In the default mode
// `execute` runs on an 8 MB fiber stack, so every fork below - a subshell, a
// command substitution, both stages of a pipeline, an `&` child, a function
// calling an external - is taken from that stack, under ASan, in the real
// binary. #202 named this the first-contact risk of phase 2; the finding is that
// there is nothing to report beyond these tests passing.

namespace {

// The two modes, spelled as the child's `$LESH_EXECUTION` - null being "unset",
// which is the fiber.
constexpr const char* kExecutionModes[] = {nullptr, "inline"};

[[nodiscard]] const char* mode_name(const char* execution) {
	return execution == nullptr ? "on_a_fiber" : execution;
}

} // namespace

TEST(UiPtyExecution, EveryForkLaneRunsFromTheInteractiveShellInBothModes) {
	// Point 7's list, in one shell per mode. Each marker is COMPUTED rather than
	// typed - `$((1 + 1))` is five characters on the wire the moment it is keyed
	// and can only become `2` by being executed - so a shell that merely echoed
	// the line cannot pass any of them.
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);
		std::size_t prompts = 1;

		const auto run = [&](const std::string& line, const std::string& expect) {
			shell.type(line + "\r");
			++prompts;
			EXPECT_TRUE(shell.wait_for(expect))
				<< mode_name(execution) << ": `" << line << "`; saw: " << shell.seen();
			EXPECT_TRUE(shell.wait_for(kPrompt, prompts))
				<< mode_name(execution) << ": no prompt after `" << line << "`; saw: "
				<< shell.seen();
		};

		// A subshell: a fork that goes on running SHELL code, which is the lane
		// `enter_subshell` resets the cooperation for.
		run("( echo a$((1 + 1)); echo b$((1 + 1)) )", "b2");
		// A command substitution: the third fork that runs shell code without
		// exec'ing, and the one that never receives the terminal.
		run("x=$(echo hi); echo got-$x-$((1 + 1))", "got-hi-2");
		// A pipeline: two children, both foreground, both awaited with WUNTRACED.
		run("echo pipe-$((1 + 1)) | cat", "pipe-2");
		// An `&` child and then `wait` for it - the one wait with no WUNTRACED and
		// the one child the sweep must never reap on its own.
		run("sleep 0.1 & wait; echo after-$((1 + 1))", "after-2");
		// A function calling an external: shell code on the fiber stack forking to
		// exec.
		run("f() { /bin/echo fn-$((1 + 1)); }; f", "fn-2");
		// `exit` INSIDE a subshell, which ends the subshell and not the session -
		// and whose status the parent reads off the wait.
		run("( exit 3 ); echo st=$?-$((1 + 1))", "st=3-2");
		// And the session is still there afterwards.
		run("echo alive-$((1 + 1))", "alive-2");
	}
}

TEST(UiPtyExecution, ControlCAtThePromptYields130InBothModes) {
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);

		shell.type("some words nobody will run");
		ASSERT_TRUE(shell.wait_for("nobody will run")) << mode_name(execution);
		shell.type("\x03");
		ASSERT_TRUE(shell.wait_for(kPrompt, 2))
			<< mode_name(execution) << ": no fresh prompt after Ctrl-C; saw: " << shell.seen();

		shell.type("echo status=$?\r");
		EXPECT_TRUE(shell.wait_for("status=130"))
			<< mode_name(execution) << ": saw: " << shell.seen();
	}
}

TEST(UiPtyExecution, ControlCMidCommandAbandonsTheLineInBothModes) {
	// The during-a-command path in both modes. On a fiber the SIGINT lands while
	// the host is blocked in a `poll` over the signal topic and the execution
	// fiber is parked in its wait; inline it lands while the host is blocked in
	// `waitpid`. Either way the child is the terminal's foreground group and gets
	// the interrupt, the reap synthesizes the delivery the shell was excluded
	// from, and #52's interactive default abandons the rest of the line.
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);

		shell.type("sleep 5; echo $((1 + 1))-ran-anyway\r");
		// Long enough that `sleep` is certainly the foreground job and the editor
		// has parked - the same wait the older Ctrl-C cases take.
		std::this_thread::sleep_for(std::chrono::milliseconds{300});
		shell.type("\x03");

		ASSERT_TRUE(shell.wait_for(kPrompt, 2))
			<< mode_name(execution) << ": no prompt after the interrupt; saw: " << shell.seen();
		EXPECT_EQ(shell.count_of("2-ran-anyway"), 0u)
			<< mode_name(execution) << ": the interrupt did not abandon the line; saw: "
			<< shell.seen();

		shell.type("echo status=$?\r");
		EXPECT_TRUE(shell.wait_for("status=130"))
			<< mode_name(execution) << ": saw: " << shell.seen();
	}
}

TEST(UiPtyExecution, ControlZStopsTheForegroundCommandInBothModes) {
	// #161's case in both modes, which is what makes `WUNTRACED` on the awaited
	// wait load-bearing rather than inherited: `waitpid(pid, &st, WUNTRACED |
	// WNOHANG)` in the loop's sweep has to report a STOP exactly as the blocking
	// call did, or the fiber never wakes and the shell hangs on a process that is
	// never going to exit.
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);

		shell.type("sleep 30\r");
		std::this_thread::sleep_for(std::chrono::milliseconds{300});
		shell.type("\x1a");

		ASSERT_TRUE(shell.wait_for("stopped: pid "))
			<< mode_name(execution)
			<< ": no stopped report - the shell is still waiting on a stopped child; saw: "
			<< shell.seen();
		ASSERT_TRUE(shell.wait_for(kPrompt, 2))
			<< mode_name(execution) << ": no prompt after the stop; saw: " << shell.seen();

		shell.type("echo status=$?\r");
		EXPECT_TRUE(shell.wait_for("status=" + std::to_string(128 + SIGTSTP)))
			<< mode_name(execution) << ": saw: " << shell.seen();

		// And the prompt is usable, which is the whole of what "returns the
		// prompt" has to mean.
		shell.type("echo alive-$((1 + 1))\r");
		EXPECT_TRUE(shell.wait_for("alive-2"))
			<< mode_name(execution) << ": saw: " << shell.seen();

		const pid_t stopped = pid_in_stopped_report(shell.seen());
		ASSERT_GT(stopped, 0) << mode_name(execution) << ": saw: " << shell.seen();
		::kill(stopped, SIGCONT);
		::kill(stopped, SIGKILL);
	}
}

TEST(UiPtyExecution, TheExitBuiltinEndsTheSessionInBothModes) {
	// Point 8: `exit` is a status the shell reports from inside the `execute` the
	// loop is waiting on, and the execution fiber must be left in a state the
	// process can die in - parked on its inbox owning nothing, which is what the
	// leak gate judges. On a fiber the `request_stop` happens one stack away from
	// where it is read, and the flag is still settled by the statement after
	// `run_the_line` returns.
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);

		// A command first, so the fiber exists and has parked on its inbox before
		// the session ends - which is the shutdown shape point 8 is about.
		shell.type("echo before-$((1 + 1))\r");
		ASSERT_TRUE(shell.wait_for("before-2")) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt, 2)) << mode_name(execution);

		shell.type("exit 5\r");
		const std::optional<int> status = shell.reap();
		ASSERT_TRUE(status.has_value())
			<< mode_name(execution) << ": the session did not end; saw: " << shell.seen();
		ASSERT_TRUE(WIFEXITED(*status)) << mode_name(execution);
		EXPECT_EQ(WEXITSTATUS(*status), 5) << mode_name(execution);

		struct termios after{};
		ASSERT_TRUE(shell.modes(after)) << mode_name(execution);
		EXPECT_TRUE(is_cooked(after)) << mode_name(execution) << ": `exit` left the terminal raw";
	}
}

// ===========================================================================
// `read` ON THE REAL BINARY, IN BOTH MODES (#209)
// ===========================================================================
//
// The interactive `read` is the one shape where the shell blocks for as long as
// a user is willing to think, and until this ticket it did it on the thread that
// is also the line editor. What these assert is that it now does it through
// `await_readable` in the default mode and through a blocking `::read` in the
// inline one, and that a user cannot tell the two apart.
//
// EVERY MARKER IS COMPUTED, as in the block above: `$((1 + 1))` is five
// characters on the wire the moment it is keyed, so a shell that only echoed
// could not produce one.

TEST(UiPtyExecution, ReadTakesATypedLineInBothModes) {
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);

		shell.type("read x\r");
		// THE PROMPT DOES NOT COME BACK HERE, and that is the point: `read` is
		// still running, on a parked fiber in one mode and on a blocked thread in
		// the other. The line is typed into the COMMAND, not into the editor.
		std::this_thread::sleep_for(std::chrono::milliseconds{300});
		shell.type("hello world\r");

		shell.type("echo got=[$x]-$((1 + 1))\r");
		EXPECT_TRUE(shell.wait_for("got=[hello world]-2"))
			<< mode_name(execution) << ": saw: " << shell.seen();
	}
}

TEST(UiPtyExecution, AWhileReadLoopTakesThreeLinesInBothModes) {
	// The shape the ticket names beside `read x`, and the one that would have
	// found a table entry left behind: three awaits per line, one after another,
	// each on the same descriptor, with the entry removed before the fiber runs
	// again. A leak of one entry would put a stale pointer into the poll set on
	// the second line.
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);

		shell.type("while read l; do echo \"[$l]\"; done\r");
		std::this_thread::sleep_for(std::chrono::milliseconds{300});
		for (const char* line : {"one", "two", "three"}) {
			shell.type(std::string{line} + "\r");
			EXPECT_TRUE(shell.wait_for(std::string{"["} + line + "]"))
				<< mode_name(execution) << ": saw: " << shell.seen();
		}
		// Ctrl-D ends the loop's input, which is the only way out of a `while read`
		// that has no more lines.
		shell.type("\x04");
		ASSERT_TRUE(shell.wait_for(kPrompt, 2))
			<< mode_name(execution) << ": the loop did not end; saw: " << shell.seen();

		shell.type("echo alive-$((1 + 1))\r");
		EXPECT_TRUE(shell.wait_for("alive-2"))
			<< mode_name(execution) << ": saw: " << shell.seen();
	}
}

TEST(UiPtyExecution, ControlCDuringAReadBehavesExactlyAsItAlwaysHasInBothModes) {
	// THE DECISION THIS TICKET HAD TO MAKE, WRITTEN AS A TEST. The wait is NOT
	// completed with an EINTR-shaped answer: the shell's handlers carry
	// `SA_RESTART`, so a SIGINT has never interrupted this read, and the byte it
	// rings the self-pipe with wakes the HOST - which drains it, defers the number
	// past the command (#208) and goes back to polling - while the fiber stays
	// parked on the descriptor.
	//
	// So `read` still finishes with the line the user goes on to type, `$x` is
	// assigned, and the deferred SIGINT settles `$?` at 130 at the command
	// boundary. Byte for byte what the shell did before this ticket, in both
	// modes, and the reason the alternative was rejected: waking the fiber early
	// would return it to a `::read` that then blocks the whole loop.
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);

		shell.type("read x\r");
		std::this_thread::sleep_for(std::chrono::milliseconds{300});
		shell.type("\x03");
		std::this_thread::sleep_for(std::chrono::milliseconds{400});

		// NO SECOND PROMPT YET: the interrupt did not end the read.
		EXPECT_EQ(shell.count_of(kPrompt), 1u)
			<< mode_name(execution) << ": Ctrl-C ended the read; saw: " << shell.seen();

		shell.type("after-the-interrupt\r");
		shell.type("echo got=[$x] st=$?\r");
		EXPECT_TRUE(shell.wait_for("got=[after-the-interrupt] st=130"))
			<< mode_name(execution) << ": saw: " << shell.seen();

		// And the session is usable, which is what "behaves as today" has to mean
		// on the far side of the interrupt.
		shell.type("echo alive-$((1 + 1))\r");
		EXPECT_TRUE(shell.wait_for("alive-2"))
			<< mode_name(execution) << ": saw: " << shell.seen();
	}
}

TEST(UiPtyExecution, ReadFromAPipeAndFromAFileAreUnchangedInBothModes) {
	// The two non-tty descriptors the ticket names, and neither reaches an
	// interactive host at all: the pipeline stage is a forked child, whose
	// `enter_subshell` put the no-op cooperation back, and the redirected `read`
	// is a regular file, which is readable the instant it is asked about.
	//
	// `echo hi | read y` therefore leaves `$y` UNCHANGED in the parent - POSIX's
	// subshell rule, and the assertion that this ticket did not accidentally move
	// the read into the shell process.
	for (const char* execution : kExecutionModes) {
		const scratch_home home{kRc};
		shell_on_a_pty shell{home, "xterm-256color", execution};
		ASSERT_TRUE(shell.alive()) << mode_name(execution);
		ASSERT_TRUE(shell.wait_for(kPrompt)) << mode_name(execution);

		shell.type("y=kept; echo hi | read y; echo pipe=[$y]-$((1 + 1))\r");
		EXPECT_TRUE(shell.wait_for("pipe=[kept]-2"))
			<< mode_name(execution) << ": saw: " << shell.seen();

		shell.type("printf 'from-a-file\\n' > \"$HOME/in\"; read z < \"$HOME/in\";"
		           " echo file=[$z]-$((1 + 1))\r");
		EXPECT_TRUE(shell.wait_for("file=[from-a-file]-2"))
			<< mode_name(execution) << ": saw: " << shell.seen();

		// AND `read` DOES NOT OVER-READ, which is read-p.tst's own assertion made
		// interactively: the `cat` after it gets the second line.
		shell.type("printf 'a\\nb\\n' > \"$HOME/two\"; { read p; cat; } < \"$HOME/two\";"
		           " echo p=[$p]-$((1 + 1))\r");
		EXPECT_TRUE(shell.wait_for("p=[a]-2"))
			<< mode_name(execution) << ": saw: " << shell.seen();
	}
}

// #214: Tab with several matches must PAINT the menu. The pager state machine
// always worked - keys cycled, Enter accepted - but no repaint was requested,
// so the menu was invisible and Tab read as a hang. On the real binary, over
// the pty, because every unit suite passed while the screen showed nothing.
TEST(UiPtyCompletion, TabPaintsTheCompletionMenu) {
	const scratch_home home{kRc};
	{
		std::ofstream a{home.path() + "/file_a.txt"};
		std::ofstream b{home.path() + "/file_b.txt"};
	}
	shell_on_a_pty shell{home};
	ASSERT_TRUE(shell.alive());
	ASSERT_TRUE(shell.wait_for(kPrompt)) << "no prompt; saw: " << shell.seen();

	shell.type("cd \"$HOME\"\r");
	ASSERT_TRUE(shell.wait_for(kPrompt, 2)) << "saw: " << shell.seen();
	shell.type("cat ./file_");
	ASSERT_TRUE(shell.wait_for_uncoloured("./file_")) << "saw: " << shell.seen();

	shell.type("\t");
	EXPECT_TRUE(shell.wait_for_uncoloured("file_a.txt"))
		<< "the menu never painted; saw: " << shell.seen();
	EXPECT_TRUE(shell.wait_for_uncoloured("file_b.txt"))
		<< "half a menu; saw: " << shell.seen();
}
