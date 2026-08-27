#include "leshnici/prompt/git_head.h"

#include "substrate/fork_guard.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#else
// At file scope, and outside the namespace: a block-scope `extern` inside a
// namespaced function would name `lesh::leshnici::environ`, which does
// not exist and would not link.
extern "C" char** environ;
#endif

namespace lesh::leshnici::prompt {

namespace {

// ---------------------------------------------------------------------------
// The budget
// ---------------------------------------------------------------------------

// One deadline, taken at entry, consulted everywhere. Monotonic, because a
// prompt drawn across an NTP step must not wait an hour or give up instantly.
//
// `expired()` is `now >= deadline`, and that comparison is what makes
// `budget_ms == 0` exact rather than a race: the deadline is the instant of
// entry, `steady_clock` never runs backwards, so the first check after
// construction is guaranteed to fire and no `stat` is issued. A zero budget is
// therefore a supported way to ask "answer without touching the disk", not a
// degenerate input to be special-cased.
class budget {
public:
	explicit budget(std::uint32_t ms) noexcept
		: _deadline(clock::now() + std::chrono::milliseconds{ms}) {}

	[[nodiscard]] bool expired() const noexcept { return clock::now() >= _deadline; }

	// What is left, clamped into the shape `poll(2)` wants. Zero means "do not
	// wait" and is never negative, because a negative timeout is `poll`'s
	// spelling of "block forever" - the one answer this file must never give.
	[[nodiscard]] int remaining_ms() const noexcept {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			_deadline - clock::now()).count();
		if (left <= 0)
			return 0;
		return static_cast<int>(std::min<long long>(
			left, std::numeric_limits<int>::max()));
	}

private:
	using clock = std::chrono::steady_clock;
	clock::time_point _deadline;
};

// ---------------------------------------------------------------------------
// Paths and bytes
// ---------------------------------------------------------------------------

// `<dir>/<name>`, with the one case that would otherwise produce `//`.
void join_into(std::string& out, std::string_view dir, std::string_view name) {
	out.assign(dir);
	if (out != "/")
		out += '/';
	out.append(name);
}

// Trailing whitespace off, in place. Git writes a newline after HEAD and after
// a loose ref; some tools write CRLF; a hand-edited file may have trailing
// spaces. All three are the same file to git and are the same file here.
void trim_trailing(std::string& text) {
	std::size_t end = text.size();
	while (end > 0 && static_cast<unsigned char>(text[end - 1]) <= ' ')
		--end;
	text.resize(end);
}

std::string_view trim_leading_blanks(std::string_view text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
		text.remove_prefix(1);
	return text;
}

// A whole object name: hex, and at least SHA-1's 40 digits. The lower bound is
// the point - `1234567` in HEAD is not a detached head, it is a file we do not
// understand, and treating a short string of hex digits as an object name is
// exactly the kind of plausible-but-wrong answer the fallback exists to avoid.
// The upper end is deliberately open so SHA-256's 64 needs no second rule.
[[nodiscard]] bool is_object_name(std::string_view text) noexcept {
	if (text.size() < 40)
		return false;
	for (const char c : text) {
		const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
		              || (c >= 'A' && c <= 'F');
		if (!hex)
			return false;
	}
	return true;
}

// A refname we are willing to turn into a path under the common dir.
//
// HEAD is a file, and a file can say anything. `ref: /etc/passwd` or
// `ref: ../../../../elsewhere` would otherwise have us `open` a path of the
// file's choosing; nothing leaks (only 7 hex digits ever escape, and only from
// content that passes `is_object_name`) but a reader that follows an arbitrary
// path out of its own repository is not one whose behaviour can be described in
// a sentence. Git's own `check_refname_format` rejects both of these, so this
// is not a restriction on real repositories - it is agreement with git.
[[nodiscard]] bool is_safe_refname(std::string_view name) noexcept {
	if (name.empty() || name.front() == '/')
		return false;
	if (name.find('\\') != std::string_view::npos)
		return false;
	std::size_t start = 0;
	for (;;) {
		const std::size_t slash = name.find('/', start);
		const std::string_view part = name.substr(
			start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
		if (part.empty() || part == "." || part == "..")
			return false;
		if (slash == std::string_view::npos)
			return true;
		start = slash + 1;
	}
}

enum class read_result : std::uint8_t {
	ok,
	// ENOENT / ENOTDIR: the file is simply not there. A first-class answer -
	// an absent loose ref means "look in packed-refs", an absent `commondir`
	// means "this gitdir is its own common dir".
	absent,
	// Anything else, including a file over the cap. Never distinguished
	// further, because every one of them means the same thing to the caller:
	// this layout was not read, so do not answer from it.
	failed,
};

// Reads at most `cap` bytes of `path` into `out`. A file LARGER than the cap is
// `failed`, not truncated: HEAD is 41 bytes and a 4 KiB HEAD is a file we do
// not understand, so silently reading its first kilobyte would be inventing an
// answer. `O_CLOEXEC` throughout - a prompt draw can be concurrent with the
// shell's own exec, and an fd of ours in somebody else's child is a leak with a
// long fuse.
read_result read_file_capped(const std::string& path, std::size_t cap, std::string& out) {
	out.clear();
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (errno == ENOENT || errno == ENOTDIR) ? read_result::absent : read_result::failed;

	std::array<char, 1024> chunk{};
	read_result status = read_result::ok;
	for (;;) {
		const ssize_t n = ::read(fd, chunk.data(), chunk.size());
		if (n < 0) {
			if (errno == EINTR)
				continue;
			status = read_result::failed;   // EISDIR lands here, which is right
			break;
		}
		if (n == 0)
			break;
		if (out.size() + static_cast<std::size_t>(n) > cap) {
			status = read_result::failed;
			break;
		}
		out.append(chunk.data(), static_cast<std::size_t>(n));
	}
	::close(fd);
	if (status != read_result::ok)
		out.clear();
	return status;
}

[[nodiscard]] bool path_exists(const std::string& path) noexcept {
	struct ::stat st{};
	return ::stat(path.c_str(), &st) == 0;
}

// ---------------------------------------------------------------------------
// packed-refs
// ---------------------------------------------------------------------------

enum class packed_result : std::uint8_t { found, absent, expired, failed };

// Finds `refname`'s object name in `packed-refs`, streamed.
//
// Read in fixed chunks with one carried partial line rather than slurped: a
// busy repository's `packed-refs` is measured in megabytes (one line per remote
// branch and per tag), this runs on every prompt, and the answer is usually in
// the first few kilobytes. The carry is a `std::string` that settles at the
// longest line seen and is reused for the rest of the file.
//
// THE MATCH IS EXACT, and that is the whole difficulty of this file. The format
// is `<sha> <refname>`, so a prefix comparison makes `refs/heads/main` match the
// line for `refs/heads/main2` - and the failure is silent, because the sha it
// reads is a perfectly good sha for the wrong branch. Two other line kinds are
// skipped for the same reason: `#` is the header (`# pack-refs with: peeled ...`)
// and `^<sha>` is the PEELED target of the tag on the line before it, whose sha
// is the commit an annotated tag points AT. Attributing a peeled line's sha to
// the preceding refname would be wrong by one indirection.
packed_result packed_ref_lookup(const std::string& path, std::string_view refname,
                                const budget& deadline, std::string& sha_out) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (errno == ENOENT || errno == ENOTDIR) ? packed_result::absent : packed_result::failed;

	std::array<char, 8192> chunk{};
	std::string line;
	packed_result result = packed_result::absent;

	const auto examine = [&](std::string_view text) {
		if (text.empty() || text.front() == '#' || text.front() == '^')
			return false;
		const std::size_t space = text.find(' ');
		if (space == std::string_view::npos)
			return false;
		const std::string_view sha = text.substr(0, space);
		std::string_view name = text.substr(space + 1);
		while (!name.empty() && static_cast<unsigned char>(name.back()) <= ' ')
			name.remove_suffix(1);
		if (name != refname || !is_object_name(sha))
			return false;
		sha_out.assign(sha);
		return true;
	};

	for (;;) {
		if (deadline.expired()) {
			result = packed_result::expired;
			break;
		}
		const ssize_t n = ::read(fd, chunk.data(), chunk.size());
		if (n < 0) {
			if (errno == EINTR)
				continue;
			result = packed_result::failed;
			break;
		}
		if (n == 0) {
			// A final line with no newline after it is still a line.
			if (examine(line))
				result = packed_result::found;
			break;
		}
		std::string_view rest{chunk.data(), static_cast<std::size_t>(n)};
		bool done = false;
		while (!rest.empty()) {
			const std::size_t nl = rest.find('\n');
			if (nl == std::string_view::npos) {
				line.append(rest);
				break;
			}
			line.append(rest.substr(0, nl));
			rest.remove_prefix(nl + 1);
			if (examine(line)) {
				result = packed_result::found;
				done = true;
				break;
			}
			line.clear();
		}
		if (done)
			break;
	}
	::close(fd);
	return result;
}

// ---------------------------------------------------------------------------
// The layout walk
// ---------------------------------------------------------------------------

enum class layout_outcome : std::uint8_t {
	// `out` is the answer.
	answered,
	// No `.git` anywhere up to the root. NOT unrecognized: there is nothing
	// here for `git` to tell us either, so this never reaches the fallback.
	not_a_repo,
	// Something on the path was not what this reader knows how to read. The
	// only outcome that exits to the spawn.
	unrecognized,
	// The deadline passed mid-walk. Answers not-found WITHOUT the fallback: a
	// probe that has already spent its budget on a slow mount must not then
	// spend a process on it.
	expired,
};

layout_outcome resolve_head(std::string_view directory, const budget& deadline, git_head& out) {
	if (directory.empty())
		return layout_outcome::not_a_repo;

	// One scratch string for every path this function builds, and one for every
	// file's contents. At prompt frequency the steady state is zero
	// allocations after the first draw's growth.
	std::string scan{directory};
	while (scan.size() > 1 && scan.back() == '/')
		scan.pop_back();
	std::string path;
	std::string content;

	// 1. DISCOVERY: climb until a `.git` turns up.
	//
	// No `$GIT_DIR` and no `$GIT_CEILING_DIRECTORIES`, deliberately. Both are
	// real git behaviour and both are DEFERRED until something asks for them:
	// the repo's rule is that an override point arrives with its consumer, and
	// a prompt module that honoured `$GIT_DIR` would be honouring it on behalf
	// of a user nobody has met. `$GIT_DIR` in particular is a layout the
	// fallback handles correctly today - `git -C <dir>` reads the caller's
	// environment - so what is missing is the fast path for it, not the answer.
	std::string gitdir;
	for (;;) {
		if (deadline.expired())
			return layout_outcome::expired;

		join_into(path, scan, ".git");
		struct ::stat st{};
		// stat, not lstat: a `.git` SYMLINK to a directory elsewhere is a
		// layout git follows, so following it is agreement, not laxity.
		if (::stat(path.c_str(), &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				gitdir = path;
				break;
			}
			if (!S_ISREG(st.st_mode))
				return layout_outcome::unrecognized;

			// 2. THE GITFILE, which is how a linked worktree and a submodule
			// say "my real gitdir is over there". One line, `gitdir: <path>`.
			if (read_file_capped(path, 4096, content) != read_result::ok)
				return layout_outcome::unrecognized;
			trim_trailing(content);
			constexpr std::string_view kMarker = "gitdir:";
			if (!std::string_view{content}.starts_with(kMarker))
				return layout_outcome::unrecognized;
			std::string_view target = trim_leading_blanks(
				std::string_view{content}.substr(kMarker.size()));
			// A second line means a file with more in it than the one thing
			// this format holds, so it is not this format.
			if (target.empty() || target.find('\n') != std::string_view::npos)
				return layout_outcome::unrecognized;
			if (target.front() == '/') {
				gitdir.assign(target);
			} else {
				// Relative to the directory HOLDING the `.git` file, which is
				// `scan` - not to the process's cwd, which this file does not
				// know and does not want to.
				join_into(gitdir, scan, target);
			}
			break;
		}

		if (scan == "/")
			return layout_outcome::not_a_repo;
		const std::size_t slash = scan.rfind('/');
		if (slash == std::string::npos)
			return layout_outcome::not_a_repo;   // a relative path, run out of components
		scan.resize(slash == 0 ? 1 : slash);
	}

	// 3. THE COMMON DIR. A linked worktree's gitdir holds that worktree's own
	// HEAD and index, and a `commondir` file pointing back at the main `.git`,
	// where the refs actually live. Getting this wrong in either direction is a
	// wrong branch name rather than a missing one: refs from the private dir
	// find nothing, HEAD from the common dir reports the MAIN worktree's branch
	// while standing in a linked one. So: HEAD from the gitdir, refs from the
	// common dir, and no third rule.
	if (deadline.expired())
		return layout_outcome::expired;
	std::string commondir = gitdir;
	join_into(path, gitdir, "commondir");
	{
		const read_result status = read_file_capped(path, 4096, content);
		if (status == read_result::failed)
			return layout_outcome::unrecognized;
		if (status == read_result::ok) {
			trim_trailing(content);
			std::string_view target = trim_leading_blanks(content);
			if (target.empty() || target.find('\n') != std::string_view::npos)
				return layout_outcome::unrecognized;
			if (target.front() == '/')
				commondir.assign(target);
			else
				join_into(commondir, gitdir, target);   // git writes `../..`
		}
	}

	// 4. REFTABLE, the ref store this reader does not speak. It is a binary
	// format with its own tables and its own compaction, and no amount of
	// squinting at `refs/heads/main` will produce a branch name from it. Its
	// mere presence is therefore a hand-off to the fallback - the one case
	// where we know in advance that a plausible answer is unavailable, rather
	// than discovering it after parsing something.
	join_into(path, commondir, "reftable");
	if (path_exists(path))
		return layout_outcome::unrecognized;

	// 5. HEAD.
	if (deadline.expired())
		return layout_outcome::expired;
	join_into(path, gitdir, "HEAD");
	if (read_file_capped(path, 1024, content) != read_result::ok)
		return layout_outcome::unrecognized;
	trim_trailing(content);

	constexpr std::string_view kRef = "ref:";
	if (!std::string_view{content}.starts_with(kRef)) {
		// A DETACHED HEAD is the object name itself, and nothing else in this
		// file is bare hex, so no ambiguity with the symbolic form.
		if (!is_object_name(content))
			return layout_outcome::unrecognized;
		out.found = true;
		out.detached = true;
		out.short_sha = content.substr(0, 7);
		return layout_outcome::answered;
	}

	const std::string refname{trim_leading_blanks(
		std::string_view{content}.substr(kRef.size()))};
	if (!is_safe_refname(refname))
		return layout_outcome::unrecognized;

	out.found = true;
	constexpr std::string_view kHeads = "refs/heads/";
	// A refname outside `refs/heads/` is reported WHOLE. It happens during a
	// rebase and a bisect (`refs/rebase-merge/...`), and shortening it by
	// dropping the namespace would print something indistinguishable from a
	// branch of that name. The full refname is longer and true.
	out.branch = std::string_view{refname}.starts_with(kHeads)
		? refname.substr(kHeads.size())
		: refname;

	// 5a. THE LOOSE REF, `<commondir>/refs/heads/<name>`.
	if (deadline.expired())
		return layout_outcome::expired;
	join_into(path, commondir, refname);
	{
		const read_result status = read_file_capped(path, 1024, content);
		if (status == read_result::failed)
			return layout_outcome::unrecognized;
		if (status == read_result::ok) {
			trim_trailing(content);
			// Present but not an object name: git can write a symref here
			// (`ref: refs/heads/other`), and chasing it is a loop this reader
			// does not implement. The fallback does.
			if (!is_object_name(content))
				return layout_outcome::unrecognized;
			out.short_sha = content.substr(0, 7);
			return layout_outcome::answered;
		}
	}

	// 5b. PACKED REFS. Only reached when the loose file is absent, which is the
	// order git resolves in and matters: after `git pack-refs`, a branch that
	// then moves gets a loose file again and the packed line goes stale.
	if (deadline.expired())
		return layout_outcome::expired;
	join_into(path, commondir, "packed-refs");
	switch (packed_ref_lookup(path, refname, deadline, content)) {
	case packed_result::found:
		out.short_sha = content.substr(0, 7);
		return layout_outcome::answered;
	case packed_result::absent:
		// 5c. AN UNBORN BRANCH: HEAD names a branch that has no commit yet, so
		// `git init` then `cd` shows `main` with no sha. Correct, not an error,
		// and NOT a hand-off to the fallback - `git branch --show-current`
		// would print exactly the branch already in hand.
		return layout_outcome::answered;
	case packed_result::expired:
		return layout_outcome::expired;
	case packed_result::failed:
		break;
	}
	return layout_outcome::unrecognized;
}

// ---------------------------------------------------------------------------
// The fallback: git itself, on a leash
// ---------------------------------------------------------------------------

char** current_environ() noexcept {
#ifdef __APPLE__
	return *::_NSGetEnviron();
#else
	return ::environ;
#endif
}

void set_cloexec(int fd) noexcept {
	const int flags = ::fcntl(fd, F_GETFD);
	if (flags >= 0)
		::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// Runs `argv` and returns its first line of stdout, or false.
//
// FALSE COVERS EVERYTHING THAT IS NOT A CLEAN EXIT: spawn failure, a signal, a
// deadline. The caller has one thing to do with all of them - answer not-found
// - so collapsing them here keeps that from being four branches at the call
// site that must all be got right.
//
// posix_spawn, NEVER fork. `fork_guard.h` is explicit that the exec lanes are
// the ones held to "raw libc only between fork and exec"; posix_spawn is that
// discipline implemented once, by libc, with none of it in our hands. This is
// also the only reason it is legal at all on the loop thread: the prompt draws
// with workers running, and a bare `fork()` there would need #91's parking.
//
// THE CHILD IS ITS OWN PROCESS GROUP. Two things fall out of that and both are
// wanted: the user's Ctrl-C at a prompt does not reach it, and the SIGKILL on
// the deadline goes to the whole group, so a `git` that spawned a credential
// helper or a pager takes its children with it. Nothing else can be relied on
// to clean those up - we are not the session leader here.
bool run_command(const char* command, char* const argv[], const budget& deadline,
                 std::string& first_line, int& exit_code) {
	first_line.clear();
	// Defense in depth, debug only, and the reason it is here rather than
	// assumed: this runs from the editor's loop thread, and #129's rule is that
	// the exec lanes assert their lane.
	assert_not_in_forked_child();

	if (command == nullptr || *command == '\0')
		return false;
	if (deadline.remaining_ms() <= 0)
		return false;

	int out_pipe[2] = {-1, -1};
	if (::pipe(out_pipe) != 0)
		return false;
	// Both ends close-on-exec. The write end still reaches the child, because
	// `adddup2` onto fd 1 clears FD_CLOEXEC on the copy - so the child gets
	// exactly one fd of ours and it is the one we meant.
	set_cloexec(out_pipe[0]);
	set_cloexec(out_pipe[1]);

	const int devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
	if (devnull < 0) {
		::close(out_pipe[0]);
		::close(out_pipe[1]);
		return false;
	}

	posix_spawn_file_actions_t actions;
	::posix_spawn_file_actions_init(&actions);
	::posix_spawn_file_actions_adddup2(&actions, devnull, STDIN_FILENO);
	::posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
	// stderr to /dev/null, not to the terminal: `git` is entitled to complain
	// about a layout we already decided we did not understand, and the host owns
	// the screen (#98). Its complaint would land in the middle of a prompt.
	::posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);

	posix_spawnattr_t attr;
	::posix_spawnattr_init(&attr);
	// Unqualified: on macOS `sigemptyset` and `sigfillset` are function-like
	// MACROS, and `::sigemptyset` expands into something that does not parse.
	sigset_t none;
	sigemptyset(&none);
	sigset_t all;
	sigfillset(&all);
	::posix_spawnattr_setsigmask(&attr, &none);
	::posix_spawnattr_setsigdefault(&attr, &all);
	::posix_spawnattr_setpgroup(&attr, 0);
	::posix_spawnattr_setflags(&attr, static_cast<short>(
		POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));

	pid_t pid = -1;
	// A command with a slash is a path and a bare name is a PATH search, which
	// is the shell's own rule and the one a caller pointing `git_command` at a
	// stub is relying on.
	const bool has_slash = std::strchr(command, '/') != nullptr;
	const int rc = has_slash
		? ::posix_spawn(&pid, command, &actions, &attr, argv, current_environ())
		: ::posix_spawnp(&pid, command, &actions, &attr, argv, current_environ());

	::posix_spawn_file_actions_destroy(&actions);
	::posix_spawnattr_destroy(&attr);
	::close(devnull);
	::close(out_pipe[1]);   // the parent must not hold it, or EOF never comes

	if (rc != 0) {
		::close(out_pipe[0]);
		return false;   // no child was created, so nothing to reap
	}

	// Drain, bounded. The cap is generous next to a refname and exists only so
	// that a `git_command` pointed at something that prints forever cannot
	// grow this string without limit; past the cap we keep reading and discard,
	// because a reader that stops would leave the child blocked in write() and
	// turn a bounded wait into a deadline.
	constexpr std::size_t kOutputCap = 64 * 1024;
	std::string output;
	bool timed_out = false;
	for (;;) {
		const int wait_ms = deadline.remaining_ms();
		if (wait_ms <= 0) {
			timed_out = true;
			break;
		}
		pollfd watch{out_pipe[0], POLLIN, 0};
		const int ready = ::poll(&watch, 1, wait_ms);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			timed_out = true;
			break;
		}
		if (ready == 0) {
			timed_out = true;
			break;
		}
		std::array<char, 4096> chunk{};
		const ssize_t n = ::read(out_pipe[0], chunk.data(), chunk.size());
		if (n < 0) {
			if (errno == EINTR)
				continue;
			timed_out = true;
			break;
		}
		if (n == 0)
			break;   // EOF: the child closed stdout or exited
		if (output.size() < kOutputCap)
			output.append(chunk.data(), std::min(static_cast<std::size_t>(n),
			                                     kOutputCap - output.size()));
	}
	::close(out_pipe[0]);

	// THE CHILD IS REAPED ON EVERY PATH, and that is not a tidiness point: this
	// runs once per prompt, so a leaked zombie per unrecognized repo is a pid
	// table filling up over an afternoon of `cd`.
	//
	// EOF is not exit. Between the two there is a window - short, but a `git`
	// that closed stdout and is still writing its index would sit in it - so
	// the post-EOF wait is polled against the same deadline rather than
	// blocking, and falls through to the kill when it runs out.
	int status = 0;
	bool reaped = false;
	bool no_child = false;
	if (!timed_out) {
		for (;;) {
			const pid_t done = ::waitpid(pid, &status, WNOHANG);
			if (done == pid) {
				reaped = true;
				break;
			}
			if (done < 0) {
				if (errno == EINTR)
					continue;
				no_child = true;   // ECHILD: somebody else reaped it
				break;
			}
			if (deadline.remaining_ms() <= 0)
				break;
			::poll(nullptr, 0, 1);
		}
	}
	if (!reaped && !no_child) {
		// Negative pid: the group, because the child leads its own.
		if (::kill(-pid, SIGKILL) != 0)
			::kill(pid, SIGKILL);
		for (;;) {
			const pid_t done = ::waitpid(pid, &status, 0);
			if (done == pid) {
				reaped = true;
				break;
			}
			if (done < 0 && errno == EINTR)
				continue;
			break;
		}
		timed_out = true;   // killed is not answered
	}

	if (timed_out || !reaped || !WIFEXITED(status))
		return false;
	exit_code = WEXITSTATUS(status);

	const std::size_t newline = output.find('\n');
	first_line.assign(newline == std::string::npos ? output : output.substr(0, newline));
	trim_trailing(first_line);
	return true;
}

git_head spawn_fallback(std::string_view directory, const git_probe_options& options,
                        const budget& deadline) {
	git_head head;

	// `-C <directory>`, not the gitdir the walk may have found: the walk is the
	// half we do not trust here, so handing git one of its intermediate results
	// would carry the doubt into the fallback. A directory is the input the
	// caller gave us and the input git is built to take.
	// Not const: `posix_spawn` wants `char* const argv[]`, so the one genuinely
	// mutable byte string in the argv has to be one we own.
	std::string dir{directory};
	std::string line;
	int exit_code = 0;

	// `branch --show-current` rather than `rev-parse --abbrev-ref HEAD`,
	// because the latter prints the literal `HEAD` for a detached head - a
	// string indistinguishable from a branch actually named HEAD - while
	// `--show-current` prints nothing, which is unambiguous and is the signal
	// the second command keys off. It also prints an unborn branch's name,
	// so the fast path's 5c has a matching answer here.
	std::array<char*, 6> branch_argv{
		const_cast<char*>(options.git_command),
		const_cast<char*>("-C"),
		dir.data(),
		const_cast<char*>("branch"),
		const_cast<char*>("--show-current"),
		nullptr,
	};
	if (!run_command(options.git_command, branch_argv.data(), deadline, line, exit_code))
		return head;
	// Non-zero is "not a repository", a broken repository, or a git too old for
	// the flag. All three are not-found, and none of them is a second guess.
	if (exit_code != 0)
		return head;
	if (!line.empty()) {
		head.found = true;
		head.branch = line;
		return head;
	}

	// Empty output with a clean exit: detached. One more child, under what is
	// left of the same deadline.
	std::array<char*, 7> sha_argv{
		const_cast<char*>(options.git_command),
		const_cast<char*>("-C"),
		dir.data(),
		const_cast<char*>("rev-parse"),
		const_cast<char*>("--short"),
		const_cast<char*>("HEAD"),
		nullptr,
	};
	if (!run_command(options.git_command, sha_argv.data(), deadline, line, exit_code))
		return head;
	if (exit_code != 0 || line.empty())
		return head;

	// IF THE SECOND CHILD FAILS THE ANSWER IS NOT-FOUND, not `found + detached`
	// with an empty sha. The two are different prompts: the latter renders the
	// segment's literals and its style around nothing at all - ` on ` with no
	// branch after it - which reads as a bug in the prompt rather than as the
	// absence of information. Half an answer is worse than none. (The fast
	// path's empty `short_sha` is not this case: there it means "unborn", which
	// is a fact, and `branch` carries the answer.)
	head.found = true;
	head.detached = true;
	head.short_sha = line;
	return head;
}

} // namespace

git_head read_git_head(std::string_view directory, const git_probe_options& options) {
	const budget deadline{options.budget_ms};

	// The one `catch`, and it is a policy rather than a safety net. Everything
	// below reports failure by returning; the only throw that can reach here is
	// an allocation failure from one of the scratch strings. A prompt is not
	// the place to propagate that - the shell is drawing, not computing - so it
	// degrades to the same answer every other failure degrades to.
	try {
		git_head head;
		switch (resolve_head(directory, deadline, head)) {
		case layout_outcome::answered:
			return head;
		case layout_outcome::not_a_repo:
		case layout_outcome::expired:
			return {};
		case layout_outcome::unrecognized:
			break;
		}

		if (!options.allow_spawn)
			return {};
		return spawn_fallback(directory, options, deadline);
	} catch (...) {
		return {};
	}
}

} // namespace lesh::leshnici::prompt
