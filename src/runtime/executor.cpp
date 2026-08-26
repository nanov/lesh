#include "runtime/executor.h"

#include "substrate/numeric.h"

#include "runtime/diagnostic.h"
#include "runtime/builtins.h"
#include "runtime/pattern.h"
#include "runtime/signals.h"
#include "substrate/assert.h"
#include "syntax/parser.h"

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace lesh::runtime {

using syntax::node;
using syntax::node_index;
using syntax::node_kind;
using syntax::token;
using syntax::token_kind;
using syntax::tree;

namespace {

// Puts the shell's idea of WHERE IT IS back when a nested source finishes (#76).
//
// An `eval`, a dot script and a trap body each read a source of their own, so
// `$LINENO` and any diagnostic inside one count that source's lines - which is
// what dash, bash and zsh all do for a dot script. What must not survive is the
// pointer: the nested trees die with run_source, and an offset left pointing into
// one would be measured against a buffer that is gone.
//
// RAII rather than a restore at the end of run_source, because that function
// returns from inside its loop for every unwind an `eval` can take.
class origin_guard {
public:
	explicit origin_guard(shell_state& state) noexcept
		: _state(state), _saved(state.command_origin_now()) {}
	~origin_guard() { _state.restore_command_origin(_saved); }
	origin_guard(const origin_guard&) = delete;
	origin_guard& operator=(const origin_guard&) = delete;

private:
	shell_state& _state;
	shell_state::command_origin _saved;
};

// Turns a wait(2) status into the value POSIX defines for `$?`.
int status_from_wait(int wait_status) noexcept {
	if (WIFEXITED(wait_status))
		return WEXITSTATUS(wait_status);
	if (WIFSIGNALED(wait_status))
		return 128 + WTERMSIG(wait_status);
	return 0;
}

// The same question one answer wider, for a FOREGROUND job: exited, killed, or
// STOPPED (#161, and #98 decision 4 which specified it).
//
// THE ONE PLACE THE THIRD ANSWER LIVES, and it is one rather than three on
// purpose. Three foreground waits reach it - a simple command, a `( )` subshell,
// and each member of a pipeline - and they read their status through here so
// that the seam job control eventually slots into is a single function rather
// than three copies to find and keep in step. #98 draws that line by name: the
// plumbing (process groups, the terminal handoff, `WUNTRACED` reaping) lands and
// the UI (`fg`, `bg`, `jobs`) stays out, and this is where the UI would attach.
//
// 128 + WSTOPSIG, NOT 128 + SIGTSTP, AND THAT IS A DELIBERATE GENERALIZATION OF
// THE SPEC. #98 decision 4 says "128+SIGTSTP" because Ctrl-Z is the case it was
// written for, but SIGTSTP is not the only signal that stops a child: SIGSTOP
// (which cannot be caught or ignored), SIGTTIN and SIGTTOU all do, and after
// #159 a foreground child carries the default disposition for every one of them.
// Answering 128+SIGTSTP for a child stopped by SIGSTOP would be a lie the user
// can check - `kill -l $?` names the signal - so the child's own stopping signal
// is reported. This is exactly the generalization `status_from_wait` above
// already makes for a KILLED child, where POSIX says 128+the signal rather than
// 128+SIGINT, and it is what every shell with job control answers.
//
// The wait status of a stop is safe to keep carrying afterwards:
// `note_interrupt_after_handoff` reads it through WIFSIGNALED, which is false
// for a stop, so a stopped child cannot synthesize an interrupt.
int foreground_status(pid_t pid, int wait_status) {
	if (!WIFSTOPPED(wait_status))
		return status_from_wait(wait_status);
	// #98 decision 4's wording, kept verbatim, and the parenthesis is the load
	// bearing half: it is the shell saying why nothing more will come back for
	// this pid on its own. There is no job table to name the process by - that is
	// the half of job control deliberately left out - so the PID is the handle,
	// which is why the pid is in the text and not just in the shell's head.
	// `kill -CONT` of it is the way out, for the determined.
	//
	// Through `report` and not a `fprintf` of its own: this is a shell diagnostic
	// like every other, and it carries the same position prefix they do.
	report("stopped: pid %d (job control not implemented)", static_cast<int>(pid));
	return 128 + WSTOPSIG(wait_status);
}

// The path `command -p` searches: the one the system guarantees finds the standard
// utilities, whatever $PATH holds. That is the whole point of the option -
// `PATH= command -p cat` must still run cat, which is command-p.tst's 'executing
// with standard path'.
std::string_view standard_path() {
	static const std::string value = [] {
		const size_t needed = confstr(_CS_PATH, nullptr, 0);
		if (needed == 0)
			return std::string{"/usr/bin:/bin"};
		std::string out(needed, '\0');
		confstr(_CS_PATH, out.data(), needed);
		// confstr counts the terminating NUL; a string_view over it would carry the
		// NUL into the middle of a candidate pathname.
		out.resize(needed - 1);
		return out;
	}();
	return value;
}

// A pathname made absolute against the LOGICAL working directory - $PWD when it
// can be believed, and the real one otherwise, which is the rule `cd` and `pwd`
// already follow.
//
// POSIX requires `command -v` to write an ABSOLUTE pathname, and both a relative
// operand (`command -v ./foo`) and a relative PATH entry (`PATH=. command -v foo`)
// reach here.
std::string absolute_pathname(const shell_state& state, std::string_view path) {
	if (!path.empty() && path[0] == '/')
		return std::string{path};
	// The FOURTH copy of this rule, and the comment above was already claiming to
	// follow it while doing something weaker: `$PWD` was taken on the strength of
	// being absolute alone, so a stale or inherited-and-wrong one was believed here
	// after #51 had stopped believing it in `cd` and `pwd`. One caller of one rule.
	std::string base = state.logical_working_directory();
	if (base.empty())
		return std::string{path};
	if (base.empty() || base.back() != '/')
		base += '/';
	base.append(path);
	return base;
}

// Searches PATH and execs. Never returns on success.
//
// execve takes a path, not a command name, and macOS has no execvpe - so either
// the environment goes through `environ` and we use execvp, or the search is
// explicit. Explicit, because the child's environment is built from shell state,
// and assigning to environ to smuggle it into execvp is the hidden coupling this
// design avoids.
// POSIX 2.9.1.1: when execve rejects a file with ENOEXEC - no shebang, no magic
// number - the shell runs it as a SHELL SCRIPT rather than reporting a format
// error. `self` is this shell's own executable, resolved at startup, because
// argv[0] is not a usable path when the shell was itself found on PATH.
//
// Returns only if the re-exec fails too.
void exec_as_script(std::string_view self, const char* file, char** argv, char** env) {
	if (self.empty())
		return;
	// argv becomes: <self> <file> <the original arguments after argv[0]>.
	std::vector<char*> rewritten;
	std::string self_owned{self};
	std::string file_owned{file};
	rewritten.push_back(self_owned.data());
	rewritten.push_back(file_owned.data());
	for (size_t i = 1; argv[i] != nullptr; ++i)
		rewritten.push_back(argv[i]);
	rewritten.push_back(nullptr);
	execve(self_owned.c_str(), rewritten.data(), env);
}

// Searches PATH and execs, returning only on failure - and then the errno that
// says why, rather than acting on it.
//
// Split out of exec_or_die because the `exec` builtin must be able to SURVIVE a
// failure: POSIX lets an interactive shell report and carry on, and a process
// that has already _exit()ed cannot. A forked child still wants exec_or_die.
int search_and_exec(char** argv, char** env, std::string_view path_value,
                    std::string_view self_path) {
	if (std::strchr(argv[0], '/') != nullptr) {
		execve(argv[0], argv, env);
		if (errno == ENOEXEC)
			exec_as_script(self_path, argv[0], argv, env);
	} else {
		std::string candidate;
		size_t at = 0;
		int last_errno = ENOENT;
		while (at <= path_value.size()) {
			const size_t colon = path_value.find(':', at);
			const std::string_view dir = path_value.substr(
				at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
			candidate.assign(dir.empty() ? std::string_view{"."} : dir);
			candidate += '/';
			candidate += argv[0];
			execve(candidate.c_str(), argv, env);
			if (errno == ENOEXEC) {
				// Found it, and it is a script without a shebang. This is the one
				// "failure" that is not a failure.
				exec_as_script(self_path, candidate.c_str(), argv, env);
			}
			// Only ENOENT means "keep looking"; anything else is the real answer.
			if (errno != ENOENT)
				last_errno = errno;
			if (colon == std::string_view::npos)
				break;
			at = colon + 1;
		}
		errno = last_errno;
	}
	return errno;
}

[[noreturn]] void exec_or_die(char** argv, char** env, std::string_view path_value,
                              std::string_view self_path) {
	const int failure = search_and_exec(argv, env, path_value, self_path);
	// `not found`, dash's wording, rather than strerror's `No such file or
	// directory`. The search looked in every directory on $PATH; naming the errno
	// of the last attempt describes one of them and reads as a complaint about a
	// file the user never mentioned. Every other failure - a permission, a
	// directory, an unreadable format - IS about the one file that was found, so
	// there strerror is exactly right.
	report("%s: %s", argv[0],
	       failure == ENOENT ? "not found" : std::strerror(failure));
	// POSIX: 127 when the command was not found, 126 when it was found but could
	// not be executed. _exit, not exit: never flush buffers inherited from the
	// parent.
	_exit(failure == ENOENT ? 127 : 126);
}

// The status a command reports when one of its redirections failed.
//
// POSIX only requires "greater than zero", so this follows dash, which answers 2
// for every shape and every position: a missing file, a never-opened fd, a fd
// that `>&-` closed; on a regular builtin, an external command, a compound
// command, a function and a pipeline stage. lesh answered 1, which is also what a
// command that RAN and failed reports - and a redirection error means the command
// never ran at all, so the two deserve different numbers.
constexpr int kRedirectionError = 2;
// The status dash reports for `set -u` on an unset parameter and for `${x?}`.
constexpr int kExpansionError = 2;
// The status `.` reports when the script cannot be found or read. dash answers 2,
// and the operand of `.` is not a command name - so 127, which lesh answered
// before, was the status of a command search that never happened. dot-p.tst only
// requires non-zero, so this follows the reference shell.
constexpr int kDotNotFound = 2;

// What run_redirections_only's forked child reports back about the ONE thing its
// exit status cannot carry alone (#70): whether it performed a command
// substitution while expanding a redirection operand, and if so, what that
// substitution's status was. The exit status already means "did the redirection
// itself succeed" (0 or kRedirectionError), and a substitution can legitimately
// report the very same 2 - `>file$(exit 2)` - so the two questions need separate
// channels, and this is the one a pipe carries.
struct redirection_substitution {
	bool happened;
	int status;
};

// A redirection's default fd when none was written: `>` means 1, `<` means 0.
int default_fd_for(token_kind op) noexcept {
	switch (op) {
		case token_kind::less: case token_kind::dless: case token_kind::dless_dash:
		case token_kind::less_and: case token_kind::less_great:
			return STDIN_FILENO;
		default:
			return STDOUT_FILENO;
	}
}

} // namespace

// Remembers what fd `fd` holds, so restore_fds can put it back.
//
// The copy is made at fd 10 or above, not at the lowest free fd. dup(2) hands
// back the lowest, which a LATER redirection on the same command then targets:
// with fd 3 open, `{ ...; } 3>&2 4>&3` saved fd 3 into fd 4 and the second
// redirection overwrote the copy, so the restore put back the wrong file. dash
// saves from 10 for the same reason.
//
// CLOEXEC because the saved copy is the shell's bookkeeping: an external command
// run inside the construct - `{ ls; } 3>&2` - must not inherit it.
bool tree_walking_executor::save_fd(int fd, arena_array<saved_fd>* restore) {
	if (restore == nullptr)
		return true;
	const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	if (copy == -1) {
		// EBADF is not a failure: it is the answer. The fd was CLOSED, so putting
		// it back means closing it, and that is what the sentinel records. Pushing
		// nothing here is what leaked fds past their construct (#34).
		if (errno == EBADF) {
			restore->push({fd, saved_fd::closed});
			return true;
		}
		report("%d: %s", fd, std::strerror(errno));
		return false;
	}
	restore->push({fd, copy});
	return true;
}

// Applies one redirection. Returns false on failure, having reported why.
//
// Called after fork and before exec for external commands, and around the call
// for builtins - which run in this process and therefore need their fds put back.
bool tree_walking_executor::apply_redirection(const tree& t, node_index n,
                                              arena_array<saved_fd>* restore) {
	const node& self = t[n];

	// The operator sits immediately before the target word. `aux` holds the
	// explicit fd, or the sentinel when none was written.
	const token& op_token = t.token_at(self.last_token - 1);
	const std::string_view target_text =
		t.text_of_token(t.token_at(self.last_token));

	// The target is a word and gets expanded: `> $out` has to work. Field
	// splitting must not apply - POSIX makes a redirection target expanding to
	// more than one field an error, and treating it as one word is the behaviour
	// dash has.
	expander ex = make_expander();
	arena_array<std::string_view> fields{_pool, 2};
	{
		// Reparse the target as a standalone word so the expander sees it whole.
		const std::string_view expanded =
			ex.expand_value(target_text, value_context::redirection_operand);
		fields.push(expanded);
	}
	// `set -u` in a redirection target is as fatal as anywhere else: dash exits 2
	// for `dash -u -c '> $x'` rather than creating a file called the empty string.
	if (expansion_failed(ex))
		return false;
	const std::string_view target = fields.empty() ? std::string_view{} : fields[0];

	// NUL-terminate for open(2).
	char* path = nullptr;
	_pool.allocate(target.size() + 1, path, 1);
	std::memcpy(path, target.data(), target.size());
	path[target.size()] = '\0';

	const int fd = self.aux == 0xFFFFFFFFu ? default_fd_for(op_token.kind)
	                                       : static_cast<int>(self.aux);

	// Saved BEFORE anything is opened, because open(2) hands back the LOWEST free
	// descriptor - which is `fd` itself whenever `fd` is closed. Saving afterwards
	// therefore recorded a copy of the file the redirection had just opened as if
	// it were the fd's previous contents, and the restore reinstated that instead
	// of closing: `{ :; } 3>/dev/null` left fd 3 pointing at /dev/null forever.
	if (!save_fd(fd, restore))
		return false;

	int opened = -1;
	switch (op_token.kind) {
		case token_kind::less:
			opened = open(path, O_RDONLY);
			break;
		case token_kind::great:
			// `set -C`: `>` refuses to truncate a file that already exists. O_EXCL is
			// how that is done without a window between the test and the open.
			//
			// An existing file that is NOT REGULAR is still fair game - `>/dev/null`
			// has to keep working, and redir-p.tst checks exactly that - so EEXIST is
			// retried without O_EXCL once stat says the path is not a regular file.
			if (_state.opts().no_clobber) {
				opened = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
				if (opened == -1 && errno == EEXIST) {
					struct stat st{};
					if (stat(path, &st) == 0 && !S_ISREG(st.st_mode))
						opened = open(path, O_WRONLY);
					else
						errno = EEXIST;
				}
				break;
			}
			[[fallthrough]];
		case token_kind::clobber:
			// `>|` overrides noclobber, which is the whole reason the operator exists.
			opened = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			break;
		case token_kind::dgreat:
			opened = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
			break;
		case token_kind::less_great:
			opened = open(path, O_RDWR | O_CREAT, 0666);
			break;
		case token_kind::great_and:
		case token_kind::less_and: {
			// `2>&1` duplicates a descriptor rather than opening a file, and
			// `2>&-` closes one.
			if (target == "-") {
				close(fd);
				return true;
			}
			// A FILE DESCRIPTOR IS AN `int`, so a number too large to be one is not
			// invalid in some new way - it is invalid in the way `>&99` already is,
			// and it takes the SAME diagnostic and the same status rather than a
			// second one invented here (#62). The accumulator used to overflow on the
			// way to that answer, which is undefined behaviour.
			//
			// Saturating, which is what #59 chose for an over-large arithmetic
			// literal, would be meaningless at this site: a literal that will not fit
			// still has a nearest representable value to stand in for it, whereas
			// there is no "largest file descriptor" for a clamp to land on - every
			// candidate is either a descriptor the script never wrote or one it did
			// not mean. So the number is refused, and the diagnostic names the
			// descriptor the script actually asked for.
			//
			// THE TWO FAILURES KEEP THEIR OWN WORDINGS because they are different
			// complaints: `>&x` names no descriptor at all, and `>&<huge>` names one
			// the system cannot have. Both end the redirection at 2.
			const numeric_result parsed =
				parse_integer(target, numeric_site::redirection_target_fd);
			if (parsed.status == numeric_parse::not_a_number) {
				report("%s: bad file descriptor", path);
				return false;
			}
			if (parsed.status == numeric_parse::out_of_range) {
				report("%s: %s", path, std::strerror(EBADF));
				return false;
			}
			const int source = static_cast<int>(parsed.value);
			// POSIX 2.7.5/2.7.6: `>&n` is an error unless n is open for OUTPUT and
			// `<&n` unless n is open for INPUT, so the access mode is checked before
			// anything is displaced. dup2 alone cannot tell - it happily aims a
			// read-only fd at stdout, which is why `3</dev/null >&3` succeeded here
			// and in dash, and why both fail redir-p.tst's 'output duplication,
			// failure (unreadable file descriptor)'.
			const int flags = fcntl(source, F_GETFL);
			if (flags == -1) {
				report("%d: %s", source, std::strerror(errno));
				return false;
			}
			const int access = flags & O_ACCMODE;
			const bool writable = access == O_WRONLY || access == O_RDWR;
			const bool readable = access == O_RDONLY || access == O_RDWR;
			if (op_token.kind == token_kind::great_and ? !writable : !readable) {
				report("%d: %s", source,
				       op_token.kind == token_kind::great_and
				           ? "not open for output" : "not open for input");
				return false;
			}
			if (dup2(source, fd) == -1) {
				report("%d: %s", source, std::strerror(errno));
				return false;
			}
			return true;
		}
		default:
			return true;
	}

	if (opened == -1) {
		report("%s: %s", path, std::strerror(errno));
		return false;
	}
	if (opened != fd) {
		dup2(opened, fd);
		close(opened);
	}
	return true;
}

// A token's text with its LINE CONTINUATIONS removed, in arena storage.
//
// POSIX 2.2.1 removes `\<newline>` before the input is tokenised, but the lexer
// records the extent a token SPANS rather than the text it means - it owns no
// memory and cannot rewrite the input (#9) - so every reader of a token's text as
// a NAME has to do the removal itself. Returns the original view when there is
// nothing to remove, which is almost always, and costs nothing then.
std::string_view tree_walking_executor::joined_text(std::string_view text) {
	size_t at = text.find('\\');
	if (at == std::string_view::npos)
		return text;
	char* block = nullptr;
	_pool.allocate(text.size() == 0 ? 1 : text.size(), block, 1);
	size_t written = 0;
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n') {
			++i;
			continue;
		}
		block[written++] = text[i];
	}
	return {block, written};
}

// Applies every redirection on a command, LEFT TO RIGHT. The order is
// observable: `>a >b` leaves the fd pointing at b while creating both files.
// Feeds a here-document body to stdin.
//
// Written into a pipe rather than a temporary file: no filesystem, no cleanup,
// and no window where a signal leaves a stray file behind. The cost is the pipe
// buffer - a body larger than it would block, so this forks a writer for exactly
// that case rather than assuming bodies are small.
bool tree_walking_executor::apply_here_doc(const tree& t, node_index n,
                                           arena_array<saved_fd>* restore) {
	const syntax::here_doc_body& body = t.here_doc_at(t[n].aux);
	std::string_view text = t.here_doc_text(t[n].aux);

	// `<<-` strips leading tabs from every line of the body.
	std::string stripped;
	if (body.strip_tabs) {
		stripped.reserve(text.size());
		bool at_line_start = true;
		for (const char c : text) {
			if (at_line_start && c == '\t')
				continue;
			stripped += c;
			at_line_start = c == '\n';
		}
		text = stripped;
	}

	// An unquoted delimiter means the body is expanded: parameters, command
	// substitution and arithmetic, but NOT field splitting or pathname expansion
	// - the body is one blob of text, not a word list.
	std::string expanded;
	if (body.expand) {
		// make_expander, not a hand-built one: `set -u` was recorded and inert here
		// while every other expansion site honoured it, which is exactly the reason
		// there is one factory (see the comment on it).
		expander ex = make_expander();
		const std::string_view result =
			ex.expand_value(text, value_context::here_document_body);
		// A fatal expansion error in the BODY fails the redirection rather than the
		// shell. `${a?}` in a body was reported and then ignored, so
		// `echo not printed <<END` printed it (redir-p.tst:436) - the site #35 left
		// out of the fatal-error wiring. Deliberately not expansion_failed(), which
		// also arranges for a non-interactive shell to exit: dash prints the
		// diagnostic, skips the command, and carries on to the next one, and it is a
		// REDIRECTION error, which POSIX 2.8.1 does not make fatal for a command
		// other than a special builtin.
		if (ex.fatal_error())
			return false;
		expanded.assign(result);
		text = expanded;
	}

	// `3<<END` feeds the body to fd 3, not to stdin. Hardcoding STDIN_FILENO here
	// made `cat 3<<END <&3` read the terminal, and made
	// `{ cat <&5; ...; } <<A 3<<B 4<<C 5<<D` see only the last body - redir-p.tst's
	// 'here-document with non-default file descriptor' and 'multiple
	// here-documents on single command'.
	const int fd = body.fd == 0 ? STDIN_FILENO : static_cast<int>(body.fd);

	// Saved before the pipe is made, for the same reason apply_redirection saves
	// before it opens: pipe(2) returns the lowest free descriptors, so with `fd`
	// closed the pipe lands ON it and a save taken afterwards would record the
	// pipe as the fd's previous contents.
	if (!save_fd(fd, restore))
		return false;

	int pipe_fds[2];
	if (pipe(pipe_fds) == -1) {
		report("pipe: %s", std::strerror(errno));
		return false;
	}

	// A body that fits the pipe buffer is written directly; a larger one needs a
	// writer process, or the write blocks before the reader has been started.
	long buffer_capacity = fpathconf(pipe_fds[1], _PC_PIPE_BUF);
	if (buffer_capacity <= 0)
		buffer_capacity = 512;
	if (text.size() <= static_cast<size_t>(buffer_capacity)) {
		if (!text.empty()) {
			// NO SHORT WRITE IS POSSIBLE HERE, and that is why the result was being
			// discarded - but it was discarded without saying so, which is the same
			// shape as the bugs #39 was opened for. POSIX XSH 2.9.1: a write of at
			// most PIPE_BUF bytes to a pipe is atomic, and this branch is taken only
			// when the body fits, so the call either transfers every byte or fails
			// outright. What it can still do is FAIL - and a here-document whose body
			// never reached the command is not something to carry on from silently.
			const ssize_t written = write(pipe_fds[1], text.data(), text.size());
			if (written != static_cast<ssize_t>(text.size())) {
				report("here-document: %s", std::strerror(errno));
				close(pipe_fds[0]);
				close(pipe_fds[1]);
				return false;
			}
		}
		close(pipe_fds[1]);
	} else {
		std::fflush(nullptr);
		const pid_t writer = fork();
		if (writer == 0) {
			setpgid(0, 0);
			close(pipe_fds[0]);
			size_t written = 0;
			while (written < text.size()) {
				const ssize_t n = write(pipe_fds[1], text.data() + written, text.size() - written);
				if (n <= 0)
					break;
				written += static_cast<size_t>(n);
			}
			close(pipe_fds[1]);
			_exit(0);
		}
		close(pipe_fds[1]);
	}

	// The read end may already BE `fd` - pipe(2) hands back the lowest free
	// descriptors and `fd` is free whenever it was closed. Closing it then would
	// throw away the body.
	if (pipe_fds[0] != fd) {
		dup2(pipe_fds[0], fd);
		close(pipe_fds[0]);
	}
	return true;
}

bool tree_walking_executor::apply_redirections(const tree& t, node_index command,
                                               arena_array<saved_fd>* restore) {
	const node& self = t[command];
	for (uint32_t i = 0; i < self.children_count; ++i) {
		const node_index child = t.child_of(self, i);
		if (t[child].kind == node_kind::here_doc) {
			if (!apply_here_doc(t, child, restore))
				return false;
			continue;
		}
		if (t[child].kind != node_kind::redirect)
			continue;
		if (!apply_redirection(t, child, restore))
			return false;
	}
	return true;
}

bool tree_walking_executor::has_redirections(const tree& t, node_index command) noexcept {
	const node& self = t[command];
	for (uint32_t i = 0; i < self.children_count; ++i) {
		const node_kind kind = t[t.child_of(self, i)].kind;
		if (kind == node_kind::redirect || kind == node_kind::here_doc)
			return true;
	}
	return false;
}

// A command that is nothing but redirections - `>file`, `<&3`, `<<END`.
//
// POSIX 2.9.1: with no command name the redirections are still performed, and
// they are performed in a SUBSHELL environment. That is why this forks rather
// than applying and restoring in place: the redirection OPERANDS are expanded
// too, and `< ${x=no/such/file}` must not leave x set in the shell afterwards.
// dash performs them in the current environment and is the only assertion it
// fails in redir-p.tst ('redirection without command name runs in subshell'), so
// the divergence from dash here is deliberate and POSIX is followed instead.
//
// Doing nothing at all - which is what lesh did - meant `>file` never created the
// file, `<missing` never failed, and `1>&- 2>&1` never closed anything.
//
// POSIX 2.9.1 also makes this command's exit status the LAST command
// substitution it performed (#39, for the assignment prefix; #50, for the same
// case on a pipeline stage) - and a substitution in the redirection OPERAND runs
// inside the fork above, so `_substitutions` and `_state.last_status()` are
// updated in a process whose memory never rejoins the caller's. The child reports
// what it saw over `result_pipe`, and the caller folds it into ITS OWN
// `_substitutions`/`_state` exactly as if the substitution had run here - which
// is what lets the existing `_substitutions != substitutions_before` check in
// run_simple_command see it, and what lets a LATER assignment's substitution
// still win over it, the way #50 already lets a later one win inside a stage.
int tree_walking_executor::run_redirections_only(const tree& t, node_index n) {
	int result_pipe[2];
	if (pipe(result_pipe) == -1) {
		report("pipe: %s", std::strerror(errno));
		return kRedirectionError;
	}
	// Moved to 10 or above, same as save_fd and for the same reason: pipe(2)
	// hands back the LOWEST free descriptors, and the command's OWN redirections
	// - applied below, in the child - can name any fd explicitly. `4>&2
	// >/dev/null$(exit 7)` landed the write end on fd 4 here and `>&2` duped
	// stdout onto it, so the result never reached this function and `>&2`
	// silently wrote binary noise to stderr instead.
	for (int* end : {&result_pipe[0], &result_pipe[1]}) {
		const int moved = fcntl(*end, F_DUPFD_CLOEXEC, 10);
		if (moved != -1) {
			close(*end);
			*end = moved;
		}
	}
	const uint64_t substitutions_before = _substitutions;
	std::fflush(nullptr);
	const pid_t pid = fork();
	if (pid == -1) {
		report("fork: %s", std::strerror(errno));
		close(result_pipe[0]);
		close(result_pipe[1]);
		return kRedirectionError;
	}
	if (pid == 0) {
		close(result_pipe[0]);
		setpgid(0, 0);
		const bool ok = apply_redirections(t, n, nullptr);
		if (ok) {
			// A struct this small is far under PIPE_BUF, so POSIX makes the write
			// atomic and it cannot short-write; a failed write only means the parent
			// falls back to "no substitution", the same default it starts from.
			const redirection_substitution result{
				_substitutions != substitutions_before, _state.last_status()};
			(void)write(result_pipe[1], &result, sizeof(result));
		}
		close(result_pipe[1]);
		std::fflush(nullptr);
		// _exit, not exit: no EXIT trap and no buffers, because this subshell is
		// implicit and never ran a command of its own.
		_exit(ok ? 0 : kRedirectionError);
	}
	close(result_pipe[1]);
	setpgid(pid, pid);
	redirection_substitution result{false, 0};
	const ssize_t got = read(result_pipe[0], &result, sizeof(result));
	close(result_pipe[0]);
	int wait_status = 0;
	// NO WUNTRACED, deliberately (#161). This helper child is not a job: it opens
	// the redirections, writes one struct back and `_exit`s without ever running a
	// command or reaching an exec, so nothing the user typed is in it to stop and
	// there is no pid they could have named. Reaping a stop here would report a
	// "stopped: pid N" the shell then had no way to explain.
	waitpid(pid, &wait_status, 0);
	const int status = status_from_wait(wait_status);
	if (status != 0)
		return status;
	if (got == static_cast<ssize_t>(sizeof(result)) && result.happened) {
		_state.set_last_status(result.status);
		++_substitutions;
	}
	return 0;
}

// A builtin runs in this process and writes through stdio, so a fd that a
// redirection closed is not noticed until the flush - and dash turns that into
// `echo: I/O error` and status 1, which is redir-p.tst's 'effect of output
// closing'.
//
// Reporting is only half of it. On a failed flush stdio KEEPS the bytes it could
// not write, so the next flush - by then after restore_fds has put the shell's
// own fd 1 back - printed them on the terminal: `echo a >&-` printed `a` and
// reported success. Draining into /dev/null is the portable way to throw the
// buffer away, since fpurge is BSD-only. fd 1 is put back afterwards so that
// `{ echo x; echo y; } >&-` fails twice, the way dash does, rather than quietly
// succeeding once fd 1 has become /dev/null.
bool tree_walking_executor::drop_unwritable_output(const char* name) {
	if (std::ferror(stdout) == 0)
		return false;
	report("%s: I/O error", name);
	std::clearerr(stdout);
	arena_array<saved_fd> displaced{_pool, 1};
	if (!save_fd(STDOUT_FILENO, &displaced))
		return true;
	const int null_fd = open("/dev/null", O_WRONLY);
	if (null_fd != -1) {
		// open(2) hands back the lowest free descriptor, which IS fd 1 when the
		// redirection closed it - closing the copy would then close the drain and
		// the buffered bytes would survive to reach the restored fd anyway.
		if (null_fd != STDOUT_FILENO) {
			dup2(null_fd, STDOUT_FILENO);
			close(null_fd);
		}
		std::fflush(stdout);
		std::clearerr(stdout);
	}
	restore_fds(displaced);
	return true;
}

void tree_walking_executor::restore_fds(arena_array<saved_fd>& saved) {
	// In reverse, so nested saves of the same fd unwind correctly.
	for (size_t i = saved.size(); i > 0; --i) {
		const saved_fd& entry = saved[i - 1];
		if (entry.saved == saved_fd::closed) {
			// Putting back "not open" means closing. Without this the fd a
			// redirection opened survived the construct that opened it (#34).
			close(entry.original);
			continue;
		}
		dup2(entry.saved, entry.original);
		close(entry.saved);
	}
	// Emptied because this is now the only thing that CLOSES fds: a second call on
	// the same array would close descriptors that have since been handed out to
	// something else.
	saved.truncate(0);
}

int tree_walking_executor::run(const tree& t) {
	int status = run_parsed(t);
	// The EXIT trap runs on the way out, whether that is the end of input or an
	// explicit `exit` - which is why it cannot live at the `exit` builtin. An
	// `exit` INSIDE the body replaces the status the shell was leaving with.
	//
	// An interactive session defers it: for that caller "the way out" is the end
	// of the SESSION and not the end of this line (#134, see defer_exit_trap).
	if (_defer_exit_trap)
		return status;
	int from_trap = 0;
	if (run_exit_trap(from_trap))
		status = from_trap;
	return status;
}

int tree_walking_executor::finish(int status) {
	_defer_exit_trap = false;
	int from_trap = 0;
	if (run_exit_trap(from_trap))
		return from_trap;
	return status;
}

void tree_walking_executor::interrupt_at_prompt() {
	// BEFORE the traps, so a trap body reads the 130 the cancel produced - and
	// after them too, because `run_pending_traps` restores `$?` around a body it
	// ran and the interactive default sets 130 only where no trap exists. Both
	// paths land on the same number, which is what #98 decision 3 asks for.
	_state.set_last_status(128 + SIGINT);
	run_pending_traps();
	if (_flow == control_flow::interrupted)
		_flow = control_flow::normal;
	_state.set_last_status(128 + SIGINT);
}

// A REGULAR file and nothing else. POSIX confines the rule to a seekable input,
// and a regular file is the only thing lseek can be trusted to put back: a
// character device may be seekable and un-rewindable at once, and a shell that
// tried would lose input rather than hand it back.
script_input::script_input(int fd) noexcept {
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
		return;
	const off_t here = lseek(fd, 0, SEEK_CUR);
	if (here < 0)
		return;
	_fd = fd;
	_origin = here;
	_device = st.st_dev;
	_inode = st.st_ino;
}

bool script_input::still_the_script() noexcept {
	if (_fd < 0)
		return false;
	struct stat st;
	if (fstat(_fd, &st) == 0 && st.st_dev == _device && st.st_ino == _inode)
		return true;
	// `exec < other` displaced the script, or the descriptor was closed. Either
	// way the shell has no input file to hand anything back to, so it reads on
	// from the bytes it already holds - which is what it did before #67.
	_fd = -1;
	return false;
}

void script_input::hand_back(size_t consumed) noexcept {
	if (!still_the_script())
		return;
	if (lseek(_fd, _origin + static_cast<off_t>(consumed), SEEK_SET) < 0)
		_fd = -1;
}

size_t script_input::resume_at(size_t fallback, size_t limit) noexcept {
	if (!still_the_script())
		return fallback;
	const off_t now = lseek(_fd, 0, SEEK_CUR);
	if (now < 0) {
		_fd = -1;
		return fallback;
	}
	// Before the origin means the command rewound past where the shell started
	// reading; the shell can only re-run what it holds, so it starts over. Past
	// the end means the file grew after it was read: the shell runs the script it
	// was GIVEN, so that is end of input either way.
	if (now < _origin)
		return 0;
	const size_t moved = static_cast<size_t>(now - _origin);
	return moved > limit ? limit : moved;
}

int tree_walking_executor::run_input(std::string_view source, bool echo_when_verbose,
                                     script_input* input) {
	int status = _state.last_status();
	size_t at = 0;
	while (at < source.size()) {
		const size_t from = at;
		// The tree is handed to SHELL STATE, not dropped: a function defined by one
		// command is a node in the tree that command was parsed from, and the next
		// command may call it. Reading a command at a time is what makes that
		// lifetime visible - with one whole-input parse it held for free. The state
		// owns it because the state owns the function (#106, ADR-0007); the executor
		// is a replaceable back end and outlives nothing.
		const tree& parsed =
			_state.retain_tree(syntax::parse_next_command(_pool, source, at, &_state));
		echo_if_verbose(source.substr(from, at - from), echo_when_verbose);
		// BEFORE the command runs, not after: the whole point is that a command
		// reading fd 0 finds it positioned at the byte after itself (#67).
		if (input != nullptr)
			input->hand_back(at);
		status = run_parsed(parsed);
		// THIS is the prompt an interrupted command returns to (#52). Clearing the
		// unwind here and not in run_parsed is what makes the interrupt travel out of
		// a function, an `eval` and a `.` script the same way it travels out of a
		// loop - and clearing it before the exit test below is what leaves the EXIT
		// trap free to run its whole body afterwards.
		if (_flow == control_flow::interrupted)
			_flow = control_flow::normal;
		// `_input_ended` is a `return` outside any function or dot script, which
		// dash and zsh both answer by ending the input. Not folded into
		// `_exit_requested`: that flag is `exit`, and every construct that stops
		// early for it would then stop for a `return` too, whose whole point is that
		// the function or script it was in decides. Only THIS loop reads it.
		if (_exit_requested || _input_ended)
			break;
		// And the command may have read some of what it was handed: `read a` takes
		// the next line, `cat` takes the rest of the file. Where it left the
		// descriptor is where the shell reads on from - which is how a pipeline is
		// fed its own following lines instead of executing them.
		if (input != nullptr)
			at = input->resume_at(at, source.size());
	}
	if (_defer_exit_trap)
		return status;
	int from_trap = 0;
	if (run_exit_trap(from_trap))
		status = from_trap;
	return status;
}

// `set -v`: the shell writes its input to standard error AS IT READS IT.
//
// Per command rather than once for the whole input, which is both what dash does
// and what makes `set -v` partway through a script echo only the rest of it. The
// bytes are the input's own, comments and blank lines included, so the echo of a
// whole script is byte-identical to the script.
void tree_walking_executor::echo_if_verbose(std::string_view unit, bool enabled) {
	if (enabled && _state.opts().verbose)
		std::fwrite(unit.data(), 1, unit.size(), stderr);
}

// Says what is wrong with a tree that will not be run.
//
// Named rather than generic: dash prints `Syntax error: Unterminated quoted
// string`, and a bare `lesh: syntax error` for `echo it's` reads as a complaint
// about the script rather than about the apostrophe that caused it. Nothing in
// either test suite compares the text - the yash suite's `-d` only requires
// stderr to be non-empty, and the differential harness compares stderr for
// emptiness alone - so the wording is free to be useful.
//
// POSITIONED AT THE DEFECT, which it has to set for itself: a syntax error is
// reported before any node of this command has RUN, so run_node has not named a
// place and the origin still holds the previous command's. `echo one` then
// `echo ;;` reported line 1, which is the one line in the file that is fine.
//
// The offending TOKEN's error_offset when there is one - the lexer records where
// the quote was opened, not merely which node ended up defective - and the node's
// own start otherwise. Same scan error_detail does, and for the same reason: a
// redirect's first token is the operator and its unterminated target is the one
// worth pointing at.
static void report_syntax_error(shell_state& state, const tree& t) {
	const syntax::node_index at = t.first_error();
	if (at != syntax::no_node) {
		const syntax::node& n = t[at];
		uint32_t where = t.span_of(n).offset;
		for (uint32_t i = n.first_token; i <= n.last_token && i < t.token_count(); ++i)
			if (t.token_at(i).is_error()) {
				where = t.token_at(i).error_offset;
				break;
			}
		state.set_command_origin(t, where);
	}
	const char* detail = at == syntax::no_node ? nullptr : t.error_detail(t[at]);
	if (detail != nullptr)
		report("syntax error: %s", detail);
	else
		report("syntax error");
}

int tree_walking_executor::run_parsed(const tree& t, source_kind kind) {
	if (t.root() == syntax::no_node)
		return _state.last_status();

	// POSIX: a syntax error in a non-interactive shell exits without executing
	// anything. The parser deliberately recovers and returns a tree - that is what
	// the line editor needs (#10) - but recovery is for INSPECTION, not execution.
	// Running the parts that parsed is what made `echo a;; echo b` print both.
	//
	// The shell EXITS, rather than this one command merely reporting: input read
	// one command at a time would otherwise carry on to the next line, where dash
	// stops. The EXIT trap still runs, as it does in dash.
	//
	// The test is has_errors() and never incomplete(): a caller holding the whole
	// input has nothing left to continue, so an unterminated construct is a defect
	// here even though an interactive reader would answer it with a prompt. See
	// tree::incomplete() for why the two are separate questions.
	if (!_state.interactive() && t.has_errors()) {
		report_syntax_error(_state, t);
		_exit_requested = true;
		return 2;
	}

	// Everything per command is run_command_list's, which every compound body
	// shares. The starting status is the one row the two cannot agree on: the
	// shell's own input answers a unit holding no command with `$?`, and a body
	// answers zero. See run_command_list.
	return run_command_list(t, t.root(), kind, _state.last_status());
}

// THE COMMAND LOOP, and after #77 the only one in the shell.
//
// Reached by the shell's own input, by an `eval`, a `.`, a trap body and a
// command substitution (through run_parsed), and by every compound body in the
// language (through run_compound_list) - a brace group, a function body, a loop
// body, an `if` branch, a `case` item and a subshell.
//
// It is one function because the two it replaced were near-copies that had both
// drifted. #74 merged run_source's copy after finding it had gone without
// `run_pending_traps` for its whole life, which cost fifteen signal files three
// assertions each the moment #67 routed a command substitution through it. This
// third copy, in run_compound_list, was missing `set -n` - so `{ eval "set -n";
// echo after; }` printed `after` - and was ALSO failing to re-read the status a
// trap body exited with, which nobody had noticed at all.
//
// `list` is the node whose children are the commands, `kind` says whether an
// escaping unwind is consumed here or travels outward, and `status` is what an
// EMPTY list answers with. Three parameters rather than three loops: a copy is
// how a guard added to one reader silently fails to reach the others.
int tree_walking_executor::run_command_list(const tree& t, node_index list,
                                            source_kind kind, int status) {
	const node& self = t[list];
	for (uint32_t i = 0; i < self.children_count; ++i) {
		// `set -n`: read and parse, execute nothing. POSIX says the option is
		// ignored by an INTERACTIVE shell, or a typo would end the session.
		// Checked per command rather than once, so `set -n` partway through a
		// script stops the rest of it as well as a `sh -n` invocation stopping
		// everything - and, since #77, a body it is set inside stops there too
		// rather than running on to the next command in that body.
		if (_state.opts().no_exec && !_state.interactive())
			break;
		status = run_node(t, t.child_of(self, i));
		_state.set_last_status(status);
		run_pending_traps();
		// The status is re-read rather than kept: when a TRAP BODY exited, the
		// status the shell leaves with is the body's and not this command's.
		if (_exit_requested) {
			status = _state.last_status();
			break;
		}
		// Before the `set -e` test, and deliberately: an interrupt is not a command
		// that failed, and 130 must not exit the shell of someone who ran `set -e`.
		if (_flow == control_flow::interrupted) {
			status = _state.last_status();
			break;
		}
		// A `return` that reached the SHELL'S OWN INPUT was inside no function and no
		// dot script, and POSIX leaves that unspecified. dash and zsh both END THE
		// INPUT with the status the return asked for - `return; echo x` prints
		// nothing in either - and lesh went on to the next command, so `return 7` was
		// a no-op that reported 7. Ending the input is also the only reading that
		// keeps `.` consistent: a dot script's `return` ends the script, and the
		// shell's own input is the outermost script there is.
		//
		// The flow is CLEARED here rather than left for run_input, so a `return` at
		// the top of a script leaves the EXIT trap free to run its whole body.
		if (_flow == control_flow::return_from) {
			// EVERY OTHER READER lets it through, and that is the whole of what `kind`
			// decides. `eval return` inside a function returns from the FUNCTION
			// (return-p.tst's 'returning out of eval'), so the unwind has to survive
			// this loop and be consumed by whatever invoked the source - try_run_function
			// for a function, the `.` builtin for a dot script. Consuming it here would
			// end the eval and carry on with the line after it.
			//
			// A COMPOUND BODY is the same case and the reason #77 could share this loop
			// at all: `f() { { return 7; }; echo no; }` has to return from `f`, so the
			// unwind travels out of the brace group to the construct that owns it. A
			// body that consumed it would break every loop and every function in the
			// language, which is why it is this one row that keeps run_compound_list
			// from simply being run_parsed.
			if (kind == source_kind::nested) {
				status = _state.last_status();
				break;
			}
			_flow = control_flow::normal;
			_input_ended = true;
			break;
		}
		// A `break` or `continue` cannot reach here in the shell's own process: with
		// no loop around it the builtin does nothing, and the outermost loop consumes
		// whatever level is left. It reaches here in a SUBSHELL, whose enclosing loop
		// is in the parent - and then it unwinds out of the subshell, which is what
		// dash does: `for i in 1; do echo "[$(break; echo insub)]"; done` prints `[]`.
		if (_flow == control_flow::break_loop || _flow == control_flow::continue_loop) {
			// Nested, and for the same reason: `for i in 1 2; do eval break; done` has to
			// break the loop the eval is INSIDE, so the level travels out to run_loop -
			// as it does out of a loop's own body, which is the ordinary spelling of
			// `break` and the case run_loop's consume_loop_flow is waiting for.
			if (kind == source_kind::nested) {
				status = _state.last_status();
				break;
			}
			_flow = control_flow::normal;
			_input_ended = true;
			break;
		}
		if (errexit_fires(status)) {
			_exit_requested = true;
			break;
		}
	}
	return status;
}

// Runs any traps whose signals have arrived.
//
// This is the safe half of signal handling: the installed handler only sets a
// flag, and the body runs HERE, between commands, where allocating and forking
// are legal. POSIX requires the same timing, so the safety constraint and the
// specification agree.
void tree_walking_executor::run_pending_traps() {
	if (!_state.signals().any_pending())
		return;  // the common case, kept cheap

	int signo = 0;
	int interrupted_by = 0;
	while (_state.signals().take_pending(signo)) {
		// ISSUE #52. SIGINT with no trap, in an interactive shell. Recorded and acted
		// on AFTER the loop rather than here, so a trap on some other pending signal
		// still gets to run its body instead of unwinding out of it on the first
		// command.
		if (_state.signals().interrupts_command(signo)) {
			interrupted_by = signo;
			continue;
		}
		if (_state.signals().disposition_of(signo) != disposition::handler)
			continue;
		const std::string command{_state.signals().trap_command(signo)};
		if (command.empty())
			continue;
		// The status around a trap is preserved: a trap must not clobber `$?` for
		// the command that follows it.
		const int saved = _state.last_status();
		// The status the trap action was ENTERED with, which is what `exit` and
		// `return` with no operand report from inside it. See shell_state.
		const std::optional<int> outer = _state.trap_entry_status();
		_state.set_trap_entry_status(saved);
		const int in_trap = run_source(command);
		_state.set_trap_entry_status(outer);
		// An `exit` in the trap BODY decides the shell's status, and lesh threw that
		// status away: `trap '(exit 2); exit 3' INT; kill -INT $$` exited 0, the
		// status of the `kill` the trap interrupted (exit-p.tst:83). Only when the
		// body exited - otherwise the status around the trap is preserved, which is
		// what makes `trap '(exit 2)' EXIT; (exit 1); exit` still exit 1.
		if (_exit_requested)
			_state.set_last_status(in_trap);
		else
			_state.set_last_status(saved);
	}
	if (interrupted_by == 0)
		return;

	// POSIX XCU 2.11 has an interactive shell CATCH SIGINT, and catching it means
	// ABANDONING the command being run and going back to reading input - not merely
	// surviving. The difference is visible: an interrupted `while :; do :; done` has
	// to stop, and a shell that only stayed alive would spin in it forever. bash
	// abandons the loop and prints the command after it; dash dies instead, which is
	// why dash fails these cases too.
	//
	// "Back to reading input" is run_input's loop, which is where a prompt would be
	// written when there is a terminal to write it to (Phase 4) and which already
	// reads one complete command at a time. So the unwind stops there and the next
	// command runs, whether it comes from a terminal or from the script on stdin.
	_flow = control_flow::interrupted;
	// `$?` afterwards. zsh reports 130 and dash exits 130; bash reports 1 and is the
	// odd one out. Nothing in the conformance suite asserts it - every case runs
	// `echo ok` next - so the two shells that agree win.
	_state.set_last_status(128 + interrupted_by);
}

// Runs the EXIT trap, once, on the way out. True when the BODY itself exited, in
// which case `status` is the status the shell must exit with.
bool tree_walking_executor::run_exit_trap(int& status) {
	if (_exit_trap_ran)
		return false;
	_exit_trap_ran = true;
	if (_state.signals().disposition_of(kExitTrap) != disposition::handler)
		return false;
	const std::string command{_state.signals().trap_command(kExitTrap)};
	if (command.empty())
		return false;

	// The shell is ALREADY on its way out when this runs after an `exit`, and
	// run_source stops at the first command whenever `_exit_requested` stands - so
	// `trap 'echo A; echo B' EXIT; exit 1` printed A and not B. The EXIT trap is
	// where a script's cleanup lives; running one command of it is worse than
	// running none, because the half that did not run is the half that removes the
	// temporary files.
	const bool already_exiting = _exit_requested;
	_exit_requested = false;
	// The trap action's entry status, for an `exit` with no operand inside it:
	// `trap exit EXIT; (exit 2); exit` exits 2 (exit-p.tst).
	const std::optional<int> outer = _state.trap_entry_status();
	_state.set_trap_entry_status(_state.last_status());
	const int in_trap = run_source(command);
	_state.set_trap_entry_status(outer);
	const bool exited_here = _exit_requested;
	_exit_requested = already_exiting || exited_here;
	if (!exited_here)
		return false;
	// `trap 'exit 7' EXIT; exit 1` exits 7: an `exit` in the trap REPLACES the
	// status the shell was leaving with (exit-p.tst:50 and :55). A body that merely
	// ran commands does not - which is the distinction this return value carries,
	// and why it is not simply "the status run_source reported".
	_state.set_last_status(in_trap);
	status = in_trap;
	return true;
}

int tree_walking_executor::run_node(const tree& t, node_index n) {
	// Cleared on the way IN so that whatever runs deepest decides. Only a
	// short-circuited and-or list and a `!` pipeline set it, on the way out.
	_status_tested = false;
	// WHERE THE SHELL IS, for `$LINENO` and for every runtime diagnostic (#76).
	//
	// Set on the way IN and never restored, so the innermost command that started
	// wins: `while ...; do echo $LINENO; done` reports the body's line, not the
	// loop's, because the body ran later. That is what POSIX asks for - the line of
	// the command CURRENTLY executing - and it is also why this is here rather than
	// in run_simple_command: a compound command that fails in its own machinery,
	// before any simple command inside it runs, still has a line to name.
	//
	// Two stores. The line itself is a scan, and it happens only when something
	// asks - see shell_state::set_command_origin.
	_state.set_command_origin(t, t.span_of(t[n]).offset);
	switch (t[n].kind) {
		case node_kind::simple_command: return run_simple_command(t, n);
		case node_kind::pipeline:       return run_pipeline(t, n);
		case node_kind::negation:       return run_negation(t, n);
		case node_kind::and_or:         return run_and_or(t, n);
		case node_kind::compound_list: return run_compound_list(t, n);
		case node_kind::if_clause:     return run_if(t, n);
		case node_kind::while_loop:    return run_loop(t, n, /*until=*/false);
		case node_kind::until_loop:    return run_loop(t, n, /*until=*/true);
		case node_kind::for_loop:      return run_for(t, n);
		case node_kind::case_clause:   return run_case(t, n);
		case node_kind::brace_group: {
			// A brace group may carry redirections when it was produced by
			// attach_trailing_redirects for `if ...; fi > file`. Applying them here
			// covers both the written `{ ...; } > file` and the synthesised case.
			arena_array<saved_fd> saved{_pool, 4};
			std::fflush(nullptr);
			if (!apply_redirections(t, n, &saved)) {
				restore_fds(saved);
				// POSIX: a redirection error on a COMPOUND command does not exit a
				// non-interactive shell, unlike one on a special builtin.
				return kRedirectionError;
			}
			// run_node, not run_compound_list: a written brace group's child IS a
			// compound_list, but a synthesised one wraps whatever construct carried
			// the redirection. Calling run_compound_list directly iterated a for
			// loop's WORDS as if they were commands.
			const int status = run_node(t, t.child_of(t[n], 0));
			std::fflush(nullptr);
			restore_fds(saved);
			return status;
		}
		case node_kind::subshell:      return run_subshell(t, n);
		case node_kind::function_definition: return run_function_definition(t, n);
		case node_kind::async_list:          return run_async(t, n);
		case node_kind::error:
			report("syntax error near '%.*s'",
			       static_cast<int>(t.text_of(t[n]).size()), t.text_of(t[n]).data());
			return 2;
		default:
			return 0;
	}
}

// `! pipeline`. POSIX: status zero becomes one, anything non-zero becomes zero.
//
// `set -e` never fires on the negated pipeline itself - its status is being tested,
// which is the whole point of writing `!`.
int tree_walking_executor::run_negation(const tree& t, node_index n) {
	if (t[n].children_count == 0)
		return 1;  // `!` with nothing after it inverts the status of nothing
	int inner = 0;
	{
		const errexit_suppression quiet{*this};
		inner = run_node(t, t.child_of(t[n], 0));
	}
	_status_tested = true;  // POSIX exempts a `!` pipeline from `set -e`
	return inner == 0 ? 1 : 0;
}

int tree_walking_executor::run_and_or(const tree& t, node_index n) {
	const node& self = t[n];
	LESH_ASSERT(self.children_count == 2);

	// The left operand's status is TESTED, so `set -e` must not fire on it. Only
	// the last command of an and-or list can exit the shell. Nesting is left-deep,
	// so suppressing the left child covers every operand but the last.
	int left = 0;
	{
		const errexit_suppression quiet{*this};
		left = run_node(t, t.child_of(self, 0));
	}
	_state.set_last_status(left);

	// A `break`, `continue`, `return` or `exit` in the LEFT operand unwinds PAST
	// the operator rather than being something the list gets to test: POSIX makes
	// the unwind immediate, so `break && echo x` never reaches the echo.
	//
	// Reading `left` alone ran the right-hand side of every one of them, because
	// `break`, `continue` and a bare `return` all report 0 and 0 is exactly what
	// `&&` continues on - break-p.tst's 'breaking before &&', continue-p.tst's
	// 'continuing before &&' and return-p.tst's 'returning before &&'. The `||`
	// cases passed only because 0 is also what `||` stops on.
	//
	// `_status_tested` is deliberately left alone: this list did not short-circuit
	// on a STATUS, and the unwind reaches the enclosing construct before any
	// `set -e` test does.
	if (_flow != control_flow::normal || _exit_requested)
		return left;

	const bool is_and = t.token_at(self.aux).kind == syntax::token_kind::and_if;
	if (is_and ? (left != 0) : (left == 0)) {
		// Short-circuit: the list's LAST command never ran, so this status was
		// tested rather than acted on and `set -e` must not fire on it.
		_status_tested = true;
		return left;
	}

	return run_node(t, t.child_of(self, 1));
}

int tree_walking_executor::run_compound_list(const tree& t, node_index n) {
	// THE SAME LOOP THE TOP LEVEL READS THROUGH (#77). This carried a third copy of
	// it, and a copy drifts: it had no `set -n` test, so `{ eval "set -n"; echo
	// after; }` printed `after` where dash prints nothing, and it kept the status of
	// the command a signal interrupted where run_parsed re-reads the one the TRAP
	// BODY exited with - which lost the 5 from `trap "exit 5" USR1; { kill -USR1 $$;
	// }` in every compound body in the language. Only the first of the two was known;
	// the second came out of reading the loops side by side, which is exactly how #74
	// found the `run_pending_traps` gap that had cost fifteen signal files.
	//
	// Two arguments carry everything the two readers disagree about:
	//
	//   `nested` - an escaping `break`, `continue` or `return` TRAVELS OUT of a body
	//   to the construct that owns it, where the shell's own input consumes it and
	//   ends there. Sharing the loop is only possible because #74 had already made
	//   that row a parameter instead of a reason to write a second one.
	//
	//   0 - an EMPTY body answers zero, where run_parsed answers `$?`. The mirror of
	//   the empty-unit trap #74 hit from the other side. `case a in a) ;; esac` is
	//   the shape that reaches it: POSIX makes a case item's list the one compound
	//   list that may be empty, and require_list makes every other spelling of an
	//   empty body a syntax error. `(exit 9); case a in a) ;; esac` is 0 in dash, so
	//   inheriting `$?` here would report 9.
	return run_command_list(t, n, source_kind::nested, 0);
}

int tree_walking_executor::run_if(const tree& t, node_index n) {
	const node& self = t[n];
	const uint32_t pairs = self.aux;

	for (uint32_t i = 0; i < pairs; ++i) {
		// POSIX: a condition's status is TESTED, so `set -e` must not fire anywhere
		// inside it - not on the last command of the condition list, and not on any
		// command nested within it. A depth counter covers the whole subtree.
		int cond = 0;
		{
			const errexit_suppression quiet{*this};
			cond = run_node(t, t.child_of(self, i * 2));
		}
		if (_flow != control_flow::normal || _exit_requested)
			return cond;
		if (cond == 0)
			return run_node(t, t.child_of(self, i * 2 + 1));
	}

	// Whatever follows the (condition, body) pairs is the else branch.
	if (self.children_count > pairs * 2)
		return run_node(t, t.child_of(self, pairs * 2));
	// POSIX: an if with no matching branch has status zero, not the condition's.
	return 0;
}

// True when a break or continue should stop this loop, decrementing the level so
// `break 2` passes through the inner loop and stops the outer one.
bool tree_walking_executor::consume_loop_flow(bool& should_break) {
	if (_flow == control_flow::break_loop || _flow == control_flow::continue_loop) {
		should_break = _flow == control_flow::break_loop;
		// The OUTERMOST loop of this process consumes the unwind whatever level is
		// left on it: POSIX says `break n` breaks the n-th enclosing loop or, when
		// there are fewer than n, the outermost one. Leaving the flow standing here
		// handed the commands AFTER the loop an unwind nothing would consume - which
		// is break-p.tst's 'breaking one more than actual nest level two', where the
		// echo between the two loops must not run and the shell must go on afterwards.
		if (--_flow_level <= 0 || _loop_depth <= 1) {
			_flow = control_flow::normal;
			return true;  // handled here
		}
		return true;  // still unwinding, but this loop stops either way
	}
	return false;
}

int tree_walking_executor::run_loop(const tree& t, node_index n, bool until) {
	const node& self = t[n];
	if (self.children_count < 2)
		return 0;

	// Raised for the CONDITION too, not only the body: `while break; do ...; done`
	// leaves the loop in dash, so the condition is inside it.
	const loop_scope inside{*this};
	int status = 0;
	// A guard against a runaway loop taking the machine down, which is exactly
	// what an unbounded `while true` in a test harness would do.
	for (uint64_t iteration = 0; iteration < kMaxLoopIterations; ++iteration) {
		// The CONDITION is tested, so `set -e` is suppressed there. The BODY is not:
		// `set -e; while true; do false; done` must exit the shell, and breaking out
		// of the body's list without exiting is what made it loop forever.
		int cond = 0;
		{
			const errexit_suppression quiet{*this};
			cond = run_node(t, t.child_of(self, 0));
		}
		if (_exit_requested)
			return status;
		if (_flow != control_flow::normal) {
			bool should_break = false;
			if (consume_loop_flow(should_break))
				return status;
			// An interrupt is neither a break nor a continue, so consume_loop_flow
			// leaves it standing - and the loop must stop rather than test the
			// condition it never finished evaluating. Without this the body ran one
			// more time before the check after it noticed (#52).
			if (_flow == control_flow::interrupted)
				return status;
		}
		const bool keep_going = until ? (cond != 0) : (cond == 0);
		if (!keep_going)
			break;

		status = run_node(t, t.child_of(self, 1));
		if (_exit_requested)
			return status;
		if (_flow != control_flow::normal) {
			bool should_break = false;
			if (consume_loop_flow(should_break)) {
				if (should_break || _flow != control_flow::normal)
					return status;
				continue;  // `continue`: next iteration
			}
			return status;  // `return` unwinds past the loop
		}
	}
	return status;
}

int tree_walking_executor::run_for(const tree& t, node_index n) {
	const node& self = t[n];
	if (self.children_count == 0)
		return 0;

	const std::string_view name = t.text_of(t[t.child_of(self, 0)]).empty()
	                              ? std::string_view{}
	                              : std::string_view{};
	(void)name;
	const bool has_in = (self.aux & 0x80000000u) != 0;
	const uint32_t name_token = self.aux & 0x7FFFFFFFu;
	// Line continuations removed: `fo\<newline>r i\<newline>x in ...` names the
	// variable `ix`, and the raw text created one whose name held a backslash and a
	// newline, so the body saw nothing (quote-p.tst's `for in do done` case).
	const std::string_view var = joined_text(t.text_of_token(t.token_at(name_token)));

	// Every child but the last is a word to iterate; the last is the body.
	const uint32_t word_count = self.children_count - 1;
	expander ex = make_expander();
	arena_array<std::string_view> items{_pool, 8};

	if (!has_in) {
		// `for x do ...` iterates the POSITIONAL PARAMETERS. Without this the loop
		// found no words and its body never ran at all - which is silently doing
		// nothing, and it is the form yash's own test helpers use throughout, so it
		// made a third of the conformance suite produce no result.
		for (size_t i = 1; i <= _state.positional_count(); ++i) {
			std::string_view arg;
			if (_state.positional_at(i, arg))
				items.push(arg);
		}
	} else {
		for (uint32_t i = 0; i < word_count; ++i)
			ex.expand_word(t, t.child_of(self, i), items);
		// The word list is the loop's whole input, so a fatal expansion here means
		// there is nothing to iterate and nothing to decide from. It was REPORTED
		// and then ignored: `sh -u -c 'for i in $nope; do echo x; done; echo reached'`
		// printed the diagnostic and then `reached`, where dash stops at 2 (#39).
		if (expansion_failed(ex))
			return kExpansionError;
	}

	// After the word list is expanded, not before: the words are evaluated once,
	// ahead of the first iteration, so a `break` in a command substitution there is
	// not inside this loop.
	const loop_scope inside{*this};
	int status = 0;
	for (const auto& item : items) {
		// A readonly loop variable stops the loop: dash exits the shell over
		// `readonly i; for i in 1 2; do echo $i; done`, because assigning the
		// variable is how the loop advances and POSIX makes the failure fatal.
		if (!_state.set(var, item)) {
			shell_state::report_readonly({}, var);
			return assignment_error();
		}
		status = run_node(t, t.child_of(self, word_count));
		if (_exit_requested)
			return status;
		if (_flow != control_flow::normal) {
			bool should_break = false;
			if (consume_loop_flow(should_break)) {
				if (should_break || _flow != control_flow::normal)
					return status;
				continue;
			}
			return status;
		}
	}
	return status;
}

int tree_walking_executor::run_case(const tree& t, node_index n) {
	const node& self = t[n];
	if (self.children_count == 0)
		return 0;

	// Patterns are expanded with PATHNAME EXPANSION DISABLED. POSIX subjects a
	// case pattern to tilde, parameter, command and arithmetic expansion but NOT
	// to field splitting or pathname expansion - otherwise `*)` expands to the
	// files in the current directory and matches nothing, which is exactly what
	// happened before this flag was passed.
	expander ex = make_expander(false);
	arena_array<std::string_view> subject{_pool, 2};
	ex.expand_word(t, t.child_of(self, 0), subject);
	// The subject is expanded before any pattern is looked at, so a `set -u`
	// failure here stops the construct rather than matching against the empty
	// string: dash exits 2 for `sh -u -c 'case $nope in *) echo x;; esac'` and
	// lesh ran the `*)` item, printing `x` and reporting success.
	if (expansion_failed(ex))
		return kExpansionError;
	const std::string_view text = subject.empty() ? std::string_view{} : subject[0];

	for (uint32_t i = 1; i < self.children_count; ++i) {
		const node& item = t[t.child_of(self, i)];
		// Low 31 bits: pattern count. High bit: the item was closed by `;&`
		// (POSIX.1-2024) rather than `;;` or `esac` - see case_item in ast.h.
		const uint32_t pattern_count = item.aux & 0x7FFFFFFFu;
		bool matched = false;
		for (uint32_t p = 0; p < pattern_count; ++p) {
			arena_array<std::string_view> pattern{_pool, 2};
			ex.expand_word(t, t.child_of(item, p), pattern);
			// pattern is never empty here: since #54 a pattern word is ONE VALUE
			// rather than a field list (word_role::pattern in expand_word), so an
			// empty pattern pushes an empty string rather than nothing at all. The
			// old `continue` on an empty pattern skipped the item entirely for
			// having what it read as NO pattern, which hid case-p.tst:369 - a
			// subject and pattern that both expand to empty via command
			// substitution - behind an executor bug rather than the redirection
			// triage first blamed it on.
			if (pattern_match(pattern[0], text, /*period_is_special=*/false)) {
				matched = true;
				break;
			}
		}
		if (!matched)
			continue;

		// Matched: run this item's body, then keep running successive items'
		// bodies WITHOUT re-testing their patterns for as long as `;&` chains
		// them together - POSIX.1-2024 fallthrough (case-p.tst:203). Only the
		// LAST body run in the chain becomes the case command's exit status; an
		// intermediate one is discarded rather than returned, which is what
		// keeps an EMPTY `;&` item from resetting $?: run_node on a body with no
		// commands returns 0 locally without ever calling set_last_status, so
		// discarding that 0 here leaves $? exactly where the command before the
		// whole `case` left it, for the NEXT item's body to read
		// (case-p.tst:214 - there is no shell oracle for this, so the choice is
		// the yash test file's).
		for (uint32_t cur = i;; ++cur) {
			const node& cur_item = t[t.child_of(self, cur)];
			const uint32_t cur_patterns = cur_item.aux & 0x7FFFFFFFu;
			const int status = cur_item.children_count > cur_patterns
				? run_node(t, t.child_of(cur_item, cur_patterns))
				: 0;
			const bool falls_through = (cur_item.aux & 0x80000000u) != 0;
			if (!falls_through || cur + 1 >= self.children_count)
				return status;
		}
	}
	return 0;  // POSIX: no matching pattern is status zero
}

// Parses and runs source in THIS environment. `eval` and `.` both need it, and
// both must see the shell's own state rather than a copy - which is what makes
// `. ./config` able to set variables the caller then reads.
int tree_walking_executor::run_source(std::string_view source, bool echo_as_read,
                                      std::string_view file) {
	// A nested pool: the trees live only as long as this call. Functions defined
	// here would not outlive it, which is the same limitation #25 recorded. A deque
	// of them rather than one, because POSIX says `eval` and `.` READ their
	// argument as shell input - and a shell reads one command at a time, so
	// `eval 'alias e=echo<newline>e hi'` prints hi in dash. The trees must all
	// stand while any of them runs: a function defined by one is a node in it.
	buffer_pool nested{BUFFER_POOL_SIZE};
	std::deque<tree> trees;
	// The trees below die with this call, so where the shell says it is has to go
	// back to the caller's source before they do. See origin_guard.
	const origin_guard origin{_state};
	// A dot script names ITSELF in a diagnostic; an `eval` and a trap body are not
	// files and keep `$0`. Set after the guard, so that the caller's answer is what
	// gets put back (#61).
	if (!file.empty())
		_state.set_origin_file(file);
	// ZERO, not `$?`. POSIX gives both `eval` and `.` an exit status of zero when
	// no command is executed, and starting from the caller's status reported the
	// status of whatever ran BEFORE instead: `false; eval '' '' ''` reported 1 and
	// `(exit 1); . /dev/null` reported 1, where dash reports 0 for both
	// (eval-p.tst's 'evaluating null operands', dot-p.tst's 'empty dot script').
	//
	// `eval` with NO operands answered 0 already, through a `joined.empty()` test
	// at the call site - so the two spellings of "nothing to run" disagreed, which
	// is why the test is here and that special case is gone.
	int status = 0;
	size_t at = 0;
	while (at < source.size()) {
		const size_t from = at;
		trees.push_back(syntax::parse_next_command(nested, source, at, &_state));
		// `set -v` writes input to standard error AS IT IS READ, and a dot script is
		// input: dash echoes each of its commands, in the script's own bytes, right
		// where this reads them. lesh echoed the `. ./script` line and then nothing,
		// so the option that exists to show what the shell is reading went silent
		// exactly where the reading was least visible (dot-p.tst's 'with verbose
		// option'). After the parse and not before it, so the unit echoed is the one
		// about to run - the same order run_input uses.
		echo_if_verbose(source.substr(from, at - from), echo_as_read);
		const tree& parsed = trees.back();
		// An empty unit is SKIPPED rather than handed to run_parsed, which answers
		// one with `$?` - right for the shell's own input, wrong here. `eval` and `.`
		// report ZERO when no command is executed, and `$?` is the status of whatever
		// ran BEFORE: it made `false; eval '' '' ''` report 1 (eval-p.tst's
		// 'evaluating null operands'). This is the only thing run_source still decides
		// for itself, and it decides it OUTSIDE the loop rather than inside a copy of
		// one.
		//
		// Errors first, because a unit can be BOTH malformed and empty of commands,
		// and skipping it would swallow the diagnostic that ends the shell.
		if (!parsed.has_errors() && parsed.holds_no_command())
			continue;
		// And everything else is run_parsed's, which is the point of #74. The
		// syntax-error exit, `set -n`, pending traps, the interrupt unwind and
		// `set -e` all live there and are now reached by both readers instead of by
		// whichever copy last remembered to grow them.
		status = run_parsed(parsed, source_kind::nested);
		// `status` and not `_state.last_status()`: run_parsed already re-reads the
		// state for every unwind it can take - so the two agree wherever the old loop
		// re-read - and it returns 2 for a syntax error, which is BEFORE any command
		// set a status. Re-reading here would have made `eval 'if'` exit with the
		// status of the command before it.
		if (_exit_requested || _flow != control_flow::normal)
			return status;
	}
	return status;
}

// The pathname $PATH gives for a dot script, or false when no directory on it
// holds a READABLE regular file of that name.
//
// Separate from search_path_for, which tests X_OK: POSIX says a dot script need
// not be executable, and dash sources a mode-644 file happily. Sharing the
// executable test would make `. lib.sh` fail on every library anyone ever wrote.
//
// A directory of the right name is SKIPPED rather than opened - dash reports
// `not found` for it - and the search does NOT fall back to the working
// directory when the path runs out. POSIX allows that fallback as an extension;
// dash does not do it, and doing it silently would make `. config` source a
// different file than the reference shell.
bool tree_walking_executor::search_path_for_dot(std::string_view name,
                                                std::string& out) const {
	std::string_view path_value;
	if (!_state.lookup("PATH", path_value))
		path_value = "/usr/bin:/bin";
	size_t at = 0;
	while (at <= path_value.size()) {
		const size_t colon = path_value.find(':', at);
		const std::string_view dir = path_value.substr(
			at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
		// An EMPTY entry means the current directory, the same rule the command
		// search itself follows.
		std::string candidate{dir.empty() ? std::string_view{"."} : dir};
		candidate += '/';
		candidate.append(name);
		struct stat info {};
		if (stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
		    access(candidate.c_str(), R_OK) == 0) {
			out = std::move(candidate);
			return true;
		}
		if (colon == std::string_view::npos)
			break;
		at = colon + 1;
	}
	return false;
}

// Finds and runs a dot script. False when it could not be found or read, having
// reported it - the caller owns what that costs a non-interactive shell.
//
// POSIX XCU `.`: an operand CONTAINING A SLASH names the file directly, and one
// without a slash is searched for on $PATH. This simply fopen()ed the operand,
// which searched the working directory instead. dot-p.tst's 'dot script in $PATH'
// passed anyway, because the case sets `PATH=$PWD` and the two answers coincide
// there - a false pass, and with any other PATH lesh sourced a file dash refuses
// to find.
bool tree_walking_executor::run_dot_script(std::string_view operand, int& status) {
	std::string path;
	if (operand.find('/') != std::string_view::npos) {
		path.assign(operand);
	} else if (!search_path_for_dot(operand, path)) {
		report(".: %.*s: not found",
		       static_cast<int>(operand.size()), operand.data());
		status = kDotNotFound;
		return false;
	}
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		report(".: %s: %s", path.c_str(), std::strerror(errno));
		status = kDotNotFound;
		return false;
	}
	std::string source;
	char buffer[4096];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
		source.append(buffer, got);
	std::fclose(f);
	// A diagnostic inside the script names the SCRIPT, not `$0`, because the line
	// it carries is counted in the script (#61). bash and zsh both do this; dash
	// names $0 and then appends the path, which says the same thing at more length.
	// Handed to run_source rather than set here, so that the guard putting the
	// caller's position back puts its file back with it - see origin_guard.
	status = run_source(source, /*echo_as_read=*/true, path);
	// A dot script is a RETURN BOUNDARY. POSIX XCU `return`: it returns from the
	// function OR the dot script that invoked it, whichever is innermost - so the
	// unwind stops at this one call and whatever invoked it carries on. dash and
	// zsh agree, and the two cases that distinguish it are return-p.tst's
	// 'returning from dot script, nested in another dot script' (the OUTER script
	// must go on to its last line) and 'returning from dot script, nested in
	// function' (the calling function must go on to its).
	//
	// Consumed HERE and not in run_source, which `eval` shares: `eval return`
	// inside a function returns from the FUNCTION - return-p.tst's 'returning out
	// of eval' - so eval must let the unwind through and only `.` may stop it.
	//
	// The status is left alone. `. ./script` where the script ends in `return 17`
	// reports 17, which run_source already returned as the last command's status.
	if (_flow == control_flow::return_from)
		_flow = control_flow::normal;
	return true;
}

int tree_walking_executor::run_function_definition(const tree& t, node_index n) {
	const node& self = t[n];
	if (self.children_count == 0)
		return 0;
	const token& name_token = t.token_at(self.aux);
	// Line continuations removed, like every other name: `f\<newline>unc () { ... }`
	// defines `func`, and the raw text registered a function whose name held a
	// backslash and a newline, so calling `func` reported "No such file or
	// directory" (quote-p.tst's 'line continuation in function definition').
	const std::string name{joined_text(t.text_of_token(name_token))};
	// Recorded in SHELL STATE, which owns the tree this body is a node in - see
	// shell_state::retain_tree. A redefinition replaces the previous body there.
	_state.define_function(name, t, t.child_of(self, 0));
	return 0;
}

bool tree_walking_executor::try_run_function(const tree&, arena_array<char*>& argv,
                                             int& status) {
	const shell_state::function_definition* found = _state.lookup_function(argv[0]);
	if (found == nullptr)
		return false;
	// A COPY, taken before the body runs: the body may define a function, and an
	// insertion rehashes the table the pointer above points into. `f() { g() { :; }; }`
	// is that case, and the tree pointer read afterwards would be a dead one.
	const shell_state::function_definition target = *found;

	if (_function_depth >= kMaxFunctionDepth) {
		report("%s: recursion too deep", argv[0]);
		status = 1;
		return true;
	}

	// POSIX: the positional parameters are REPLACED for the duration of the call
	// and restored afterwards. $0 is not replaced - it stays the shell's name.
	std::vector<std::string> saved = _state.positional();
	std::vector<std::string> arguments;
	for (size_t i = 1; argv[i] != nullptr; ++i)
		arguments.emplace_back(argv[i]);
	_state.set_positional(std::move(arguments));

	++_function_depth;
	// A function call is a BOUNDARY for `break` and `continue`: the loops the
	// CALLER is inside are not loops this body is inside. dash says so twice over -
	// `while true; do f() { break; }; f; echo in; break; done` prints `in`, so the
	// callee's break does not break the caller's loop, and `f() { for i in 1 2; do
	// break 3; done; }` called from a loop does not either, so the level does not
	// travel out with it. lesh broke the caller's loop in both, which is zsh's
	// dynamic answer rather than the POSIX floor's (ADR-0001).
	//
	// Saved and restored rather than just zeroed: a function called FROM a loop
	// body must leave the caller's own count intact, or the loop it returns into
	// would think it was outermost.
	const int caller_loops = _loop_depth;
	_loop_depth = 0;
	status = run_node(*target.tree, target.body);
	_loop_depth = caller_loops;
	--_function_depth;

	// `return` unwinds to here and no further. The control_flow machinery has been
	// wired since #24 with nothing to unwind; this is what it was waiting for.
	if (_flow == control_flow::return_from)
		_flow = control_flow::normal;
	// And so does a `break n` whose level outlived the loops inside the body: with
	// no loop here to consume it, it would break the CALLER's loop.
	if (_flow == control_flow::break_loop || _flow == control_flow::continue_loop)
		_flow = control_flow::normal;

	_state.set_positional(std::move(saved));
	// A function CALL is a command in its own right, so `set -e; f` exits when the
	// body's last list short-circuited. The exemption stops at the call boundary.
	_status_tested = false;
	return true;
}

// Runs a list in the background. POSIX: the shell does NOT wait, and the status
// is zero regardless of what the command eventually does.
int tree_walking_executor::run_async(const tree& t, node_index n) {
	if (t[n].children_count == 0)
		return 0;

	std::fflush(nullptr);
	const pid_t pid = fork();
	if (pid == -1) {
		report("fork: %s", std::strerror(errno));
		return 1;
	}
	if (pid == 0) {
		setpgid(0, 0);
		_state.enter_subshell();
		// POSIX XCU 2.11: with job control disabled, an asynchronous command has
		// SIGINT and SIGQUIT ignored. lesh has no job control (ADR-0001 puts it in
		// the User Portability option), so this is the ordinary case rather than the
		// exception - and SIG_IGN survives exec, which is what carries the rule into
		// a background job that replaces itself.
		if (!_state.opts().monitor)
			_state.signals().ignore_interrupts_for_async();
		// POSIX: an asynchronous command's stdin is /dev/null unless redirected,
		// so a background job cannot steal the terminal's input.
		const int devnull = open("/dev/null", O_RDONLY);
		if (devnull != -1) {
			dup2(devnull, STDIN_FILENO);
			close(devnull);
		}
		_exit_trap_ran = false;
		int status = run_node(t, t.child_of(t[n], 0));
		int from_trap = 0;
		if (run_exit_trap(from_trap))
			status = from_trap;
		std::fflush(nullptr);
		_exit(status);
	}

	setpgid(pid, pid);
	// Remembered so `wait` can reap it, and so it is not left as a zombie.
	_background.push_back(pid);
	// `!` is not a NAME, so nothing can have made it readonly and the refusal path
	// is unreachable here.
	std::ignore = _state.set("!", std::to_string(pid));
	return 0;
}

int tree_walking_executor::run_subshell(const tree& t, node_index n) {
	// A separate execution environment whose changes do not escape. Forking gives
	// that for free: the child mutates its own copy of shell state.
	std::fflush(nullptr);
	const pid_t pid = fork();
	if (pid == -1)
		return 1;
	if (pid == 0) {
		setpgid(0, 0);
		// POSIX: a subshell resets traps to default, except those set to ignore.
		_state.enter_subshell();
		// The subshell gets its own EXIT trap, and the flag must be cleared for it:
		// a subshell forked from INSIDE the parent's EXIT trap inherits a raised
		// flag and would skip its own. `trap '(trap "echo x" EXIT)' EXIT` is exactly
		// that shape.
		_exit_trap_ran = false;
		int status = t[n].children_count > 0
		             ? run_node(t, t.child_of(t[n], 0))
		             : 0;
		int from_trap = 0;
		if (run_exit_trap(from_trap))
			status = from_trap;
		std::fflush(nullptr);
		_exit(status);
	}
	setpgid(pid, pid);
	int wait_status = 0;
	// A `( )` IS A FOREGROUND JOB, so it reaps like one (#158 decision 5, which
	// names "the foreground subshell waits" beside the simple command and the
	// pipeline). One process, waited on by a shell with nothing else to do, and a
	// stop here hangs the shell exactly as it did at the simple-command wait -
	// today only from a deliberate `kill -STOP`, and from Ctrl-Z once #160 hands
	// this fork the terminal the way #159 hands it to a simple command.
	waitpid(pid, &wait_status, WUNTRACED);
	// A subshell is a command in its own right: `set -e; (false && echo a)` exits
	// even though the same list would not inside a brace group.
	_status_tested = false;
	return foreground_status(pid, wait_status);
}

bool tree_walking_executor::build_argv(const tree& t, node_index n,
                                       arena_array<char*>& argv,
                                       arena_array<std::string_view>* assignments) {
	const node& self = t[n];

	// The runner is passed here, so a command substitution inside a word reaches
	// this same executor. That is what makes `echo $(echo $(echo x))` work.
	expander ex = make_expander();
	arena_array<std::string_view> fields{_pool, 8};

	// POSIX 2.9.1's declaration utility: an `export`/`readonly` operand in
	// `NAME=value` form is an assignment and is expanded as one. Which operands
	// those are is not knowable at parse time - `v=export; $v A=~:~` is one in
	// dash - so the scan runs here, where the words arrive in order and their
	// fields arrive with them. See declaration_scan (#55).
	declaration_scan declaration;

	for (uint32_t i = 0; i < self.children_count; ++i) {
		const node_index child = t.child_of(self, i);
		if (t[child].kind == node_kind::assignment) {
			if (assignments != nullptr)
				assignments->push(t.text_of(t[child]));
			continue;
		}
		if (t[child].kind != node_kind::word)
			continue;
		const std::string_view word = t.text_of(t[child]);
		if (declaration.operand_is_assignment(word)) {
			// The same entry point a plain `A=$a` takes, which is the whole of the
			// fix: one value, no field splitting, no pathname expansion, and a tilde
			// eligible after an unquoted colon. Going through expand_word instead
			// made `a='x y'; export A=$a` export `x` and hand `y` to export as a
			// second operand, and left `export A=~:~` holding both tildes.
			fields.push(expand_assignment(word));
			continue;
		}
		const size_t before = fields.size();
		(void)ex.expand_word(t, child, fields);
		// A word that expanded to no fields at all names no utility, so it leaves
		// the scan where it was rather than settling it on nothing.
		if (fields.size() > before)
			declaration.note_expanded_word(fields[before]);
	}

	// `set -u` on an unset parameter, or `${x?}`, must stop the command rather
	// than run it with the parameter expanded to nothing. Until this was checked
	// the expander printed the message and the command ran anyway, so
	// `sh -u -c 'echo ${x}'` printed a blank line and reported success.
	if (expansion_failed(ex))
		return false;

	if (fields.empty())
		return false;

	for (const auto& f : fields) {
		char* buf = nullptr;
		_pool.allocate(f.size() + 1, buf, 1);
		std::memcpy(buf, f.data(), f.size());
		buf[f.size()] = '\0';
		argv.push(buf);
	}
	argv.push(nullptr);
	return true;
}

pid_t tree_walking_executor::spawn(arena_array<char*>& argv, const spawn_context& ctx,
                                   const arena_array<std::string_view>* assignments,
                                   const tree* t, node_index command,
                                   bool standard_path) {
	// Built-ins run in this process, so their output sits in our stdout buffer.
	// Flush before forking or the child inherits a copy and prints it again.
	std::fflush(nullptr);

	const pid_t pid = fork();
	if (pid == -1) {
		report("fork: %s", std::strerror(errno));
		return -1;
	}

	if (pid == 0) {
		// setpgid in the child AND in the parent below: whichever runs first wins,
		// and neither ordering can leave a child outside a group. This is the
		// documented way to avoid the race.
		setpgid(0, ctx.group);

		// FIRST THING AFTER THE GROUP EXISTS, and before the dup2s below (#158
		// decision 1). The group has to be settled - the child hands the terminal to
		// the group it is in - and the fd has to still be the terminal, which is why
		// this reads the saved tty rather than fd 0: the dup2s underneath may be
		// about to make fd 0 a pipe. See take_terminal_in_child for why the handoff
		// comes before the signal reset it also does.
		if (ctx.foreground)
			take_terminal_in_child(_state.tty_fd());

		if (ctx.input_fd != STDIN_FILENO) {
			dup2(ctx.input_fd, STDIN_FILENO);
			close(ctx.input_fd);
		}
		if (ctx.output_fd != STDOUT_FILENO) {
			dup2(ctx.output_fd, STDOUT_FILENO);
			close(ctx.output_fd);
		}

		become_command(argv, assignments, t, command, standard_path);
	}

	setpgid(pid, ctx.group == 0 ? pid : ctx.group);
	return pid;
}

void tree_walking_executor::take_terminal_in_child(int tty_fd) noexcept {
	// No terminal means no handoff AND no reset - see the header. The two are one
	// decision, and splitting them would make a non-interactive shell's children
	// lose an ignored-on-entry SIGTSTP that #37 says they keep.
	if (tty_fd < 0)
		return;

	// The whole of decision 1. No error path: the four outcomes tty.h's
	// `set_foreground_pgrp` distinguishes are the SHELL's problem, where "somebody
	// else owns the terminal" is worth a diagnostic and a retry. Here the only
	// answers are "we have it" and "we do not", the child is about to exec either
	// way, and there is no place to report from - the shell's stderr is the very
	// surface a message would corrupt.
	(void)tcsetpgrp(tty_fd, getpgrp());

	// Then, and only then, the dispositions this shell's editing loop imposed and
	// which execve would otherwise carry into the command.
	struct sigaction dfl{};
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	dfl.sa_flags = 0;
	for (const int signo : {SIGTTOU, SIGTTIN, SIGTSTP})
		(void)sigaction(signo, &dfl, nullptr);
}

void tree_walking_executor::reclaim_terminal() noexcept {
	const int tty_fd = _state.tty_fd();
	if (tty_fd < 0)
		return;
	// SIGTTOU is ignored in the shell for the whole interactive session
	// (`ignore_background_write_signals`, re-asserted at every read entry), so
	// this cannot stop the process it is trying to give the terminal back to.
	(void)tcsetpgrp(tty_fd, getpgrp());
}

void tree_walking_executor::note_interrupt_after_handoff(int wait_status) {
	// The gate, in the order that costs least: no terminal means no handoff means
	// the shell was signalled itself and owes nothing here.
	if (_state.tty_fd() < 0)
		return;
	// THE KEY IS "KILLED BY", not "reported 130". A child that CATCHES SIGINT and
	// exits normally has handled its own interrupt, and the line it was part of
	// carries on - the cooperative-exit discipline every shell keeps. WIFSIGNALED
	// is what tells the two apart, and it is the whole of the distinction.
	if (!WIFSIGNALED(wait_status) || WTERMSIG(wait_status) != SIGINT)
		return;
	// AND THEN NOTHING ELSE IS ASKED. The disposition is `run_pending_traps`'s
	// business, exactly as it is for a signal that really arrived: a user trap
	// runs its body and the line continues, and the interactive default takes
	// #52's route instead - `interrupts_command` sets `control_flow::interrupted`
	// and the list loop abandons the rest of the line with 130.
	//
	// Deciding it here was the bug this replaced. A trap-only synthesis left
	// `sleep 5; echo after` printing `after` after a Ctrl-C, which no shell does:
	// dash, zsh AND bash all abandon the line, and so did lesh before the handoff
	// excluded it from the signal. Unanimity, and lesh's own prior behaviour, on
	// the same answer - so the narrow gate was a regression of the handoff rather
	// than a conservative reading of it.
	//
	// The two dispositions that are neither are unreachable and harmless. `trap ''
	// INT` is inherited as SIG_IGN across the exec, so the child cannot die of a
	// signal it ignores and this line is never reached; were it reached, the flag
	// would be dropped by run_pending_traps' own disposition test.
	_state.signals().note_pending(SIGINT);
}

void tree_walking_executor::become_command(arena_array<char*>& argv,
                                           const arena_array<std::string_view>* assignments,
                                           const tree* t, node_index command,
                                           bool standard_path_wanted) {
	// The interactive defaults are this SHELL's, and SIG_IGN is the one disposition
	// that survives execve - so an interactive shell that ignores SIGTERM would hand
	// every command it runs a disposition it cannot be killed by, and a lesh child
	// would then read that as "ignored on entry" and refuse to trap it (#37). That
	// is every `target=child` case in sigquit5/sigterm5-p.tst, 120 assertions per
	// file. Dropped in the child, after the fork, so the shell itself keeps them.
	_state.signals().drop_interactive_defaults();

	// Redirections are applied AFTER the pipeline's fds, so an explicit
	// `> file` on a pipeline stage overrides the pipe - which is what POSIX
	// requires and what `a | b > out` means.
	if (t != nullptr && !apply_redirections(*t, command, nullptr))
		_exit(kRedirectionError);

	// `x=1 cmd` exports x to cmd only. Applying it in the CHILD is what keeps it
	// out of the shell - the parent's state is untouched by construction rather
	// than by remembering to undo it.
	//
	// The VALUES were expanded by the caller, before the fork. Expanding them here
	// meant building an expander with no command runner, and a command substitution
	// in a prefix value therefore had nothing to run it: `x=$(echo z) sh -c 'echo
	// $x'` exported x empty (#31). POSIX 2.9.1 performs every expansion before the
	// command runs, so the shell that read the command is the one that owes them.
	if (assignments != nullptr) {
		for (const auto& a : *assignments) {
			const size_t eq = a.find('=');
			// A readonly name was refused before the fork - see run_simple_command -
			// so the refusal cannot happen here, and a child could not exit the
			// shell over it anyway.
			if (eq != std::string_view::npos)
				std::ignore = _state.set_exported(a.substr(0, eq), a.substr(eq + 1));
		}
	}

	// `command -p` searches the path POSIX guarantees finds the standard utilities
	// instead of $PATH, which is what makes `PATH= command -p cat` run cat.
	std::string_view path_value = standard_path();
	if (!standard_path_wanted && !_state.lookup("PATH", path_value))
		path_value = "/usr/bin:/bin";
	exec_or_die(argv.data(), _state.environment_block(), path_value,
	            _state.own_path());
}

// Expands the value of a NAME=value assignment, returning the whole thing with
// the value expanded and the name untouched.
//
// The value is expanded, not stored raw: `x="a b"` assigns `a b` without the
// quotes, and `x=$y` assigns y's value. Storing the source text meant a later
// `echo $x` printed the quotes.
std::string_view tree_walking_executor::expand_assignment(std::string_view text) {
	const size_t eq = text.find('=');
	if (eq == std::string_view::npos)
		return text;
	expander ex = make_expander();
	const std::string_view value =
		ex.expand_value(text.substr(eq + 1), value_context::assignment);
	(void)expansion_failed(ex);
	// The NAME's line continuations are removed too, and here rather than in the
	// expander: the expander is handed the value alone. `fo\<newline>o=bar` assigns
	// to `foo`, and copying the name verbatim created a variable whose name held a
	// backslash and a newline (quote-p.tst's 'line continuation in assignment').
	const std::string_view raw_name = text.substr(0, eq);
	char* joined = nullptr;
	_pool.allocate(raw_name.size() + 1 + value.size(), joined, 1);
	size_t written = 0;
	for (size_t i = 0; i < raw_name.size(); ++i) {
		if (raw_name[i] == '\\' && i + 1 < raw_name.size() && raw_name[i + 1] == '\n') {
			++i;
			continue;
		}
		joined[written++] = raw_name[i];
	}
	const std::string_view name{joined, written};
	joined[name.size()] = '=';
	std::memcpy(joined + name.size() + 1, value.data(), value.size());
	return {joined, name.size() + 1 + value.size()};
}

// False when the assignment was REFUSED because the name is readonly, having
// reported it. POSIX 2.8.1 calls that a variable assignment error and makes it
// fatal to a non-interactive shell, which is why the result travels rather than
// being dropped here: `readonly a=1; a=2; echo not reached` must print nothing.
bool tree_walking_executor::apply_expanded_assignment(std::string_view expanded) {
	const size_t eq = expanded.find('=');
	if (eq == std::string_view::npos)
		return true;
	const std::string_view name = expanded.substr(0, eq);
	if (_state.set(name, expanded.substr(eq + 1)))
		return true;
	shell_state::report_readonly({}, name);
	return false;
}

// Splits NAME=value, expands the value, and applies it.
bool tree_walking_executor::apply_assignment(std::string_view text) {
	return apply_expanded_assignment(expand_assignment(text));
}

bool tree_walking_executor::apply_prefix(const arena_array<std::string_view>& assignments,
                                         bool persist,
                                         std::vector<saved_variable>& restore) {
	if (!persist)
		restore.reserve(assignments.size());
	for (const auto& a : assignments) {
		const size_t eq = a.find('=');
		if (eq == std::string_view::npos)
			continue;
		// The old value is read BEFORE the new one lands, and only when it will be
		// needed: a persisting prefix has nothing to put back by definition.
		if (!persist) {
			saved_variable sv;
			sv.name.assign(a.substr(0, eq));
			std::string_view previous;
			sv.was_set = _state.lookup(sv.name, previous);
			if (sv.was_set)
				sv.value.assign(previous);
			restore.push_back(std::move(sv));
		}
		// Applied one at a time, in order, because `a=1 b=$a cmd` must see a's new
		// value - dash does the same.
		if (!apply_assignment(a))
			return false;
	}
	return true;
}

void tree_walking_executor::restore_prefix(const std::vector<saved_variable>& restore) {
	for (const auto& sv : restore) {
		// A name the command itself made readonly cannot be put back, and POSIX says
		// a readonly variable stays readonly - so the refusal is the correct outcome
		// here rather than an error to report twice.
		if (sv.was_set)
			std::ignore = _state.set(sv.name, sv.value);
		else
			std::ignore = _state.unset(sv.name);
	}
}

bool tree_walking_executor::expand_prefix(const arena_array<std::string_view>& assignments,
                                          arena_array<std::string_view>& expanded) {
	for (const auto& a : assignments) {
		expanded.push(expand_assignment(a));
		// A fatal expansion error - `set -u` on an unset parameter, or `${x?}` - stops
		// the command rather than exporting the empty string it could not expand.
		if (_expansion_error)
			return false;
	}
	return true;
}

// A variable assignment error, applied where POSIX 2.8.1 puts it: the shell exits
// when it is not interactive, and reports and carries on when it is. One function
// because every assignment site - a bare `x=1`, a command prefix, a `for` variable,
// `exec`'s prefix, a function call's prefix - must not decide it separately.
int tree_walking_executor::assignment_error() {
	if (!_state.interactive())
		_exit_requested = true;
	return kExpansionError;
}

// One `set -x` trace line.
//
// PS4 is expanded on every line - option-p.tst's `$PS4` case sets it to
// `${foo#X} ` and requires the expansion, not the literal - and defaults to
// `+ `, whose trailing space is part of the default rather than added here.
//
// `prefix` holds ALREADY-EXPANDED `NAME=value` assignments; `argv` the expanded
// words. Tracing is what the expansion is for: `+ echo bar` is the point, not
// `+ echo $foo`.
void tree_walking_executor::trace_command(const arena_array<std::string_view>& prefix,
                                          char* const* argv) {
	std::string_view ps4 = "+ ";
	std::string_view raw;
	std::string expanded_ps4;
	if (_state.lookup("PS4", raw)) {
		// A separate expander with NO runner: a command substitution in PS4 would
		// otherwise recurse into tracing itself.
		expander ex{_pool, _state, nullptr, false, &_state, &_state};
		expanded_ps4.assign(ex.expand_value(raw, value_context::assignment));
		ps4 = expanded_ps4;
	}
	std::string line{ps4};
	bool first = true;
	for (const auto& p : prefix) {
		if (!first)
			line += ' ';
		line.append(p);
		first = false;
	}
	for (size_t i = 0; argv != nullptr && argv[i] != nullptr; ++i) {
		if (!first)
			line += ' ';
		line += argv[i];
		first = false;
	}
	line += '\n';
	// stderr, per POSIX, and written whole so a trace line cannot be interleaved
	// with a child's output half-way through.
	std::fwrite(line.data(), 1, line.size(), stderr);
}

// `exec [--] [command [argument...]]`.
//
// This cannot be a builtins.cpp builtin. A builtin is handed argv and returns a
// status, and exec's whole contract is that on success it NEVER returns - the
// shell is gone. It also needs its redirections applied WITHOUT saving them,
// which is the one place that distinction is observable: `exec >log` is how a
// script redirects itself for the rest of its life. So it lives here beside
// `eval`, `.` and `wait`, which are in the executor for the same kind of reason.
//
// `demoted` is true when the command was written `command exec ...`, which POSIX
// says makes a special builtin behave like a regular one - so its errors stop
// being fatal to the shell. exec-p.tst's 'redirection error on exec' case checks
// exactly that, by requiring the shell to survive and report 1..125.
//
// Returns only when there was no command to become, or the exec failed.
int tree_walking_executor::run_exec(const tree& t, node_index n,
                                    arena_array<char*>& argv,
                                    const arena_array<std::string_view>& assignments,
                                    bool demoted) {
	const bool fatal = !demoted && !_state.interactive();

	// Flush BEFORE redirecting. A builtin writes through stdio, so bytes queued
	// earlier still sit in the FILE* buffer and would otherwise land in the file
	// `exec` has just opened - the same trap the builtin path documents.
	std::fflush(nullptr);
	// nullptr for `restore`: exec's redirections OUTLIVE the command, whether or
	// not a command follows. That is the whole difference from every other
	// builtin, all of which save and put their fds back.
	if (!apply_redirections(t, n, nullptr)) {
		// POSIX: a redirection error on a SPECIAL builtin is fatal to a
		// non-interactive shell. Verified against dash, which exits 2 for both
		// `exec <missing` and `exec >&3` on a closed fd, and survives with status 2
		// under `command exec`. The EXIT trap still runs, which is why this sets
		// the flag rather than calling _exit.
		if (fatal)
			_exit_requested = true;
		return 2;
	}

	// POSIX XBD 12.2 guideline 10: `--` ends the options, and exec has none, so it
	// is simply skipped. dash does NOT do this - `dash -c 'exec -- echo hi'` says
	// `exec: --: not found` - and fails both `-- separator` cases in exec-p.tst.
	// Divergence recorded rather than copied: dash is behind the standard here.
	size_t first = 1;
	if (argv[1] != nullptr && std::string_view{argv[1]} == "--")
		first = 2;

	if (argv[first] == nullptr) {
		// No command: the redirections were the whole point. The assignments persist
		// because exec is a special builtin, and they persist UNEXPORTED - dash
		// leaves `FOO=bar exec` visible to the shell but absent from `env`.
		//
		// Unless `command exec` demoted it, and then they are restored like any
		// regular builtin's: `x=1 command exec; echo $x` prints nothing in dash
		// where `x=1 exec; echo $x` prints 1.
		std::vector<saved_variable> restore_vars;
		const bool refused = !apply_prefix(assignments, !demoted, restore_vars);
		restore_prefix(restore_vars);
		return refused ? assignment_error() : 0;
	}

	// Drop `exec` and any `--` by shifting the rest down; the arena owns the
	// strings and the trailing nullptr moves with them.
	for (size_t i = 0; i + first < argv.size(); ++i)
		argv[i] = argv[i + first];
	argv.truncate(argv.size() - first);

	// An assignment prefixing exec belongs to the new image's ENVIRONMENT, not
	// just to shell state: `FOO=bar exec env` prints FOO=bar in dash. There is no
	// "afterwards" to restore it for, so this is exported outright.
	{
		expander ex{_pool, _state, &_runner, true, &_state, &_state};
		for (const auto& a : assignments) {
			const size_t eq = a.find('=');
			if (eq == std::string_view::npos)
				continue;
			const std::string_view name = a.substr(0, eq);
			if (!_state.set_exported(
					name, ex.expand_value(a.substr(eq + 1), value_context::assignment))) {
				shell_state::report_readonly({}, name);
				return assignment_error();
			}
		}
	}

	std::string_view path_value;
	if (!_state.lookup("PATH", path_value))
		path_value = "/usr/bin:/bin";
	// Nothing after this point runs on success: execve replaces the image. Flush
	// first or anything still buffered dies with it.
	std::fflush(nullptr);
	// Same reason as in become_command, and here it matters even more: there is no
	// fork, so this process IS the one being replaced. `exec "$TESTEE"` from an
	// interactive shell must hand over a killable SIGTERM (#52).
	_state.signals().drop_interactive_defaults();
	const int failure = search_and_exec(argv.data(), _state.environment_block(),
	                                   path_value, _state.own_path());
	// The exec did not happen, and an interactive shell reports and carries on - so
	// the dispositions dropped for an image that never arrived have to come back.
	_state.signals().restore_interactive_defaults();

	// dash's shape, minus the line number lesh does not track:
	// `dash: 1: exec: ./_no_such_command_: not found`. exec-p.tst only requires
	// stderr to be non-empty (test_O -d), so this is not compared anywhere - but a
	// message that names the builtin is what makes the failure findable, and
	// `not found` rather than strerror's `No such file or directory` is what every
	// other shell prints for a command search that came up empty.
	report("exec: %s: %s", argv[0],
	       failure == ENOENT ? "not found" : std::strerror(failure));
	// POSIX: 127 when the command was not found, 126 when it was found but could
	// not be executed. Confirmed against dash: `exec ./_no_such_command_` is 127,
	// `exec ./mode-000-file` and `exec /tmp` are both 126 with `Permission denied`.
	const int status = failure == ENOENT ? 127 : 126;
	// The failure exits a NON-INTERACTIVE shell. An interactive one reports and
	// survives, which is exec-p.tst's 'executing non-existing command (relative,
	// interactive)' case - dash fails that one by exiting anyway.
	if (fatal)
		_exit_requested = true;
	return status;
}

tree_walking_executor::command_prefix
tree_walking_executor::read_command_options(char* const* argv) noexcept {
	command_prefix opts;
	size_t i = 1;
	for (; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		// POSIX XBD 12.2 guideline 10: `--` ends the options, so `command -- -v`
		// runs a command called `-v` rather than describing one.
		if (arg == "--") {
			++i;
			break;
		}
		if (arg.size() < 2 || arg[0] != '-')
			break;
		for (const char c : arg.substr(1)) {
			if (c == 'p') {
				opts.standard_path = true;
			} else if (c == 'v' || c == 'V') {
				// -V wins over -v whatever order they arrive in, which is what dash
				// answers for BOTH `command -v -V cat` and `command -V -v cat`. "Last
				// one wins" would disagree with dash on the first of those.
				if (opts.describe != 'V')
					opts.describe = c;
			} else if (opts.bad_option == '\0') {
				opts.bad_option = c;
			}
		}
	}
	opts.operand = i;
	return opts;
}

tree_walking_executor::command_prefix
tree_walking_executor::take_command_prefix(arena_array<char*>& argv) {
	command_prefix opts;
	// A loop, not one test: `command command echo hi` is two prefixes, and each
	// one demotes what follows it.
	while (argv.size() > 1 && std::string_view{argv[0]} == "command") {
		// A FUNCTION named `command` shadows the builtin: POSIX's search order puts
		// functions ahead of regular builtins, and `command` is regular. Stripping
		// the prefix regardless made `command() { echo F; }; command XXX` try to run
		// XXX, which is builtins-p.tst's 'function overrides non-special command
		// command'.
		//
		// Only the FIRST prefix is subject to it. Once one has been stripped,
		// function lookup is bypassed for everything after it - `command` included.
		if (!opts.present && _state.has_function("command"))
			return opts;
		const command_prefix here = read_command_options(argv.data());
		opts.standard_path = opts.standard_path || here.standard_path;
		if (here.bad_option != '\0') {
			opts.bad_option = here.bad_option;
			return opts;
		}
		// The describing form reports on its operand rather than running it, so argv
		// is left intact and `present` stays false: nothing was stripped, and a
		// function named `command` still shadows the builtin the way it would for
		// any other regular builtin.
		if (here.describe != '\0') {
			opts.describe = here.describe;
			opts.operand = here.operand;
			return opts;
		}
		// Drop `command` and its options by shifting the rest down; the arena owns
		// the strings and the trailing nullptr moves with them. When there was
		// nothing after the options this leaves argv holding only that nullptr,
		// which is the caller's signal that there is no command to run.
		for (size_t i = 0; i + here.operand < argv.size(); ++i)
			argv[i] = argv[i + here.operand];
		argv.truncate(argv.size() - here.operand);
		opts.present = true;
		if (argv.empty() || argv[0] == nullptr)
			return opts;
	}
	return opts;
}

bool tree_walking_executor::search_path_for(std::string_view name, bool standard,
                                           std::string& out) const {
	std::string_view path_value = standard_path();
	if (!standard && !_state.lookup("PATH", path_value))
		path_value = "/usr/bin:/bin";
	size_t at = 0;
	while (at <= path_value.size()) {
		const size_t colon = path_value.find(':', at);
		const std::string_view dir = path_value.substr(
			at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
		// An EMPTY entry means the current directory, the same rule the command
		// search itself follows.
		std::string candidate{dir.empty() ? std::string_view{"."} : dir};
		candidate += '/';
		candidate.append(name);
		if (access(candidate.c_str(), X_OK) == 0) {
			out = absolute_pathname(_state, candidate);
			return true;
		}
		if (colon == std::string_view::npos)
			break;
		at = colon + 1;
	}
	return false;
}

// `command -v name` and `command -V name`: how the shell would resolve a name,
// without running it.
//
// In the executor rather than in builtins.cpp because the answer needs the
// RESERVED WORDS, which live in the parser and are reached through
// syntax::is_reserved_word, and because `command` is already the executor's for
// its running form. The functions it consults are shell state's now (#106), which
// is the one ingredient this no longer has to be here for.
//
// THE ORDER is the one dash reports: a reserved word beats everything, then an
// alias, then a function, then a builtin, then PATH. Verified - `alias if=xyz;
// command -v if` prints `if`, and `alias cat=xyz; command -v cat` prints the
// alias rather than /bin/cat.
//
// Implementing only `-v` and letting `-V` and `-p` be taken for command names was
// the state this file's 14/49 came from, and one of those 14 passed only because
// `lesh: -V: No such file or directory` happens to be a non-zero status
// ('describing non-existent command (-V)' checks nothing else).
int tree_walking_executor::describe_command(const command_prefix& opts,
                                           char* const* argv) {
	// No operand at all: nothing to report, and nothing wrong either. dash answers
	// 0 for a bare `command -v`.
	if (argv[opts.operand] == nullptr)
		return 0;
	// Only the FIRST operand is described - POSIX gives the option one
	// command_name, and dash ignores the rest of `command -v cat ls`.
	const std::string_view name{argv[opts.operand]};
	const int width = static_cast<int>(name.size());
	const bool verbose = opts.describe == 'V';

	if (syntax::is_reserved_word(name)) {
		std::printf(verbose ? "%.*s is a shell keyword\n" : "%.*s\n", width, name.data());
		return 0;
	}
	if (std::string_view value; _state.lookup_alias(name, value)) {
		if (verbose) {
			std::printf("%.*s is an alias for %.*s\n", width, name.data(),
			            static_cast<int>(value.size()), value.data());
		} else {
			// POSIX writes an alias as a command line that represents its definition,
			// which is what command-p.tst's 'describing alias (-v)' relies on: it
			// unaliases the name and `eval`s this line to get the alias back. The
			// `alias` keyword is what makes it that command line - without it the
			// text is `abc='xyz'`, which on re-input assigns a VARIABLE. The listing
			// form omits the keyword because the `alias` builtin is the context
			// there; dash prints both exactly this way.
			std::fputs("alias ", stdout);
			print_alias(name, value);
		}
		return 0;
	}
	if (_state.has_function(name)) {
		std::printf(verbose ? "%.*s is a shell function\n" : "%.*s\n", width, name.data());
		return 0;
	}
	std::string found;
	if (const builtin_kind kind = classify_builtin(name); kind != builtin_kind::none) {
		// A regular built-in UTILITY is written as the pathname the search finds for
		// it and everything else as its own name; see builtin_report in builtins.h.
		// The name is the fallback when the search comes up empty, because the name
		// is still the truth about what would run.
		const bool as_path = builtin_report_of(name) == builtin_report::pathname &&
		                     search_path_for(name, opts.standard_path, found);
		if (!verbose) {
			if (as_path)
				std::printf("%s\n", found.c_str());
			else
				std::printf("%.*s\n", width, name.data());
			return 0;
		}
		if (kind == builtin_kind::special)
			std::printf("%.*s is a special shell builtin\n", width, name.data());
		else if (as_path)
			// The pathname is repeated inside the -V line because command-p.tst's
			// 'output of describing non-special built-in (-V)' greps the -V output for
			// the whole of the -v output.
			std::printf("%.*s is a shell builtin (%s)\n", width, name.data(), found.c_str());
		else
			std::printf("%.*s is a shell builtin\n", width, name.data());
		return 0;
	}

	if (name.find('/') != std::string_view::npos) {
		// A name containing a slash is used as given rather than searched for - but
		// POSIX still requires it WRITTEN as an absolute pathname, which is the one
		// place dash prints the operand back unchanged and fails command-p.tst's
		// 'output of describing external command (-v, with slash)'.
		const std::string as_given{name};
		if (access(as_given.c_str(), X_OK) == 0)
			found = absolute_pathname(_state, name);
	} else {
		(void)search_path_for(name, opts.standard_path, found);
	}
	if (found.empty()) {
		// The -v form is SILENT about a name it cannot find: command-p.tst's
		// 'describing non-existent command (-v)' requires stdout AND stderr empty.
		//
		// The -V form reports on STANDARD OUTPUT, with no `lesh:` in front of it,
		// which is where dash puts it and why: "not found" is one of the ANSWERS to
		// the question -V asks, not a diagnostic about failing to answer. dash
		// prefixes its real diagnostics with `dash: <line>:` and prints this one
		// bare. POSIX leaves the format unspecified either way.
		if (verbose)
			std::printf("%.*s: not found\n", width, name.data());
		// 127, dash's answer, not 1: the question was what would run, and nothing
		// would - which is the status a command search that came up empty reports.
		return 127;
	}
	if (verbose)
		std::printf("%.*s is %s\n", width, name.data(), found.c_str());
	else
		std::printf("%s\n", found.c_str());
	return 0;
}

// The builtins the EXECUTOR implements instead of builtins.cpp, which is what
// `builtin_home::executor` marks in the registry.
//
// One function rather than five blocks inside run_simple_command, because a
// PIPELINE STAGE needs the same ones: `echo hi | eval cat` went to
// try_run_builtin, which has no entry for `eval`, and the false return was
// discarded - the stage printed nothing and reported success, which is the same
// defect as the unimplemented `test` in #35. `echo hi | command cat` printed
// nothing for the same reason until `command` arrived here. Returns false when
// the name is none of them.
bool tree_walking_executor::try_run_executor_builtin(
		const tree& t, node_index n, arena_array<char*>& argv,
		const arena_array<std::string_view>& assignments, const command_prefix& cmd,
		int& status) {
	const std::string_view name{argv[0]};

	// `exec` replaces this process, so no table of functions could hold it. Until it
	// landed it was classified with no handler anywhere, so try_run_builtin found
	// nothing and returned success: `exec echo hi; echo notreached` printed only
	// `notreached`. A stub that silently succeeds is worse than an absent builtin -
	// the same mistake `command` made earlier on #31, which cost 19 of 49
	// assertions in command-p.tst.
	//
	// It comes before the redirection scaffolding below because it is the one
	// builtin whose redirections must NOT be put back: `exec >log` redirects the
	// shell for the rest of its life.
	if (name == "exec") {
		status = run_exec(t, n, argv, assignments, cmd.present);
		return true;
	}

	// `command` in its DESCRIBING form, `eval`, `.` and `wait`. Only the running
	// form of `command` is stripped from argv before this point, so a `command`
	// arriving here is asking about a name rather than to run one.
	if (name != "command" && name != "eval" && name != "." && name != "wait")
		return false;

	// POSIX 2.9.1 order: the redirections, then the assignments, then the command -
	// so `x=1 eval 'echo $x'` can read x, and `command -v foo >out` writes to the
	// file. Shared by all four rather than written out per builtin, which is how
	// `wait` came to perform no redirections at all.
	//
	// THE PREFIX REACHED NONE OF THEM. try_run_executor_builtin was handed the
	// assignments and passed them on to `exec` alone, so `x=1 eval 'echo $x'`
	// printed a blank line and `x=1 . /dev/null` left x unset - dash prints 1 for
	// both. `eval` and `.` are SPECIAL builtins, so their prefix PERSISTS rather
	// than being restored, which is the difference from the regular-builtin path
	// and from a function call; `command eval ...` demotes them and the prefix is
	// restored after all.
	const bool special = classify_builtin(name) == builtin_kind::special;
	arena_array<saved_fd> saved{_pool, 4};
	std::fflush(nullptr);
	const bool ok = apply_redirections(t, n, &saved);
	std::vector<saved_variable> restore_vars;
	bool refused = false;
	if (ok)
		refused = !apply_prefix(assignments, special && !cmd.present, restore_vars);
	if (!ok) {
		// POSIX 2.8.1 makes a redirection error on a SPECIAL builtin fatal to a
		// non-interactive shell. dash agrees: `dash -c 'eval : </missing; echo
		// reached'` exits 2 without printing `reached`, while a regular builtin in
		// the same position reports 2 and carries on. `command eval ...` demotes it,
		// which is what cmd.present records.
		status = kRedirectionError;
		if (special && !cmd.present && !_state.interactive())
			_exit_requested = true;
	} else if (refused) {
		// Reported by apply_prefix; the status and the shell's fate are decided
		// after the restores below, so a refusal cannot skip them.
	} else if (name == "eval") {
		// `eval` and `.` re-enter the FRONT END from inside execution, which is why
		// they live here at all - giving every builtin a back-reference to the
		// executor to serve two of them would be the wrong trade. This is the cycle
		// the ports in #11 were designed to survive: parsing inside execution, with
		// the parse seeing the same aliases and the execution seeing the same state.
		//
		// POSIX: the arguments are joined with spaces and read as shell input.
		const size_t operand = first_operand(argv.data());
		std::string joined;
		for (size_t i = operand; argv[i] != nullptr; ++i) {
			if (i > operand)
				joined += ' ';
			joined += argv[i];
		}
		status = run_source(joined);
	} else if (name == ".") {
		const size_t operand = first_operand(argv.data());
		if (argv[operand] == nullptr) {
			report(".: filename argument required");
			status = kDotNotFound;
			if (!cmd.present && !_state.interactive())
				_exit_requested = true;
		} else if (!run_dot_script(argv[operand], status)) {
			// `.` is a SPECIAL builtin, so failing to FIND or READ the script is fatal
			// to a non-interactive shell - the rule #34 established for a redirection
			// failure and #35 extended to `readonly`. `. _no_such_file_; echo not
			// reached` printed `not reached`. Only the search failure, never a non-zero
			// status the script itself reported: `. ./exits3` answering 3 must leave
			// the caller running. `command .` demotes it, which cmd.present records.
			if (!cmd.present && !_state.interactive())
				_exit_requested = true;
		}
	} else if (name == "command") {
		// Reported HERE and not where the prefix was read, so it goes through the
		// command's own redirections: `command -z cat 2>/dev/null` is silent in dash,
		// and reporting it before apply_redirections above let the message out.
		if (cmd.bad_option != '\0') {
			report("command: illegal option -- %c", cmd.bad_option);
			// 2, as for any other builtin's usage error, and what dash answers.
			status = 2;
		} else {
			status = describe_command(cmd, argv.data());
		}
	} else {
		status = run_wait(argv.data());
	}
	std::fflush(nullptr);
	restore_fds(saved);
	restore_prefix(restore_vars);
	if (refused)
		status = assignment_error();
	return true;
}

// `wait` needs the executor's record of background jobs, so it lives here
// alongside eval and . rather than in builtins.cpp.
int tree_walking_executor::run_wait(char* const* argv) {
	// `--` ends the options here too, and `wait` is the seventh utility of that
	// shape: POSIX gives it operands and no options, so `wait -- 1` waits for pid 1
	// and `wait --` waits for everything (#63). Reading argv[1] directly sent the
	// separator itself to std::atoi.
	const size_t first = first_operand(argv);
	if (argv[first] == nullptr) {
		// POSIX: with no operands, `wait` waits for ALL known children and its
		// status is ZERO - not the last child's. Reporting the last one made
		// `false & wait` fail, and under `set -e` that would exit the shell.
		//
		// NO WUNTRACED IN EITHER FORM OF `wait`, and that is POSIX rather than
		// caution (#161). XCU `wait` waits for TERMINATION; a stopped background job
		// has not terminated, and returning at its stop would both report a status
		// for a process that has not got one yet and drop the pid from
		// `_background`, so it could never be waited for again. `&` children are
		// also the one child set that never receives the terminal (#158 decision 3),
		// so nothing here can be the stop-and-return-to-a-prompt case #161 is about.
		for (const pid_t pid : _background) {
			int wait_status = 0;
			(void)waitpid(pid, &wait_status, 0);
		}
		_background.clear();
		return 0;
	}
	// With operands the status is the LAST operand's, and a child killed by a
	// signal reports 128 + the signal number.
	int status = 0;
	for (size_t i = first; argv[i] != nullptr; ++i) {
		// THE WRONG-SYSCALL HALF OF #45, VERBATIM, IN THE PATH THAT TICKET DID NOT
		// FIX. std::atoi answers 0 for `notanumber`, for `--` and for `%1`, and
		// waitpid(0, ...) waits for ANY CHILD IN THE PROCESS GROUP - so
		// `wait notanumber` reaped a job the script never named and reported its
		// status as though it had. #45 found exactly this in `kill`, where atoi's 0
		// became kill(0, sig) and signalled the whole group; it fixed the pid path in
		// `kill` and left the identical one here.
		//
		// Refused before the call, as `kill` refuses it, at dash's status 2. A pid is
		// not clamped for the same reason a signal number is not: every value a
		// clamp could land on names a REAL PROCESS, and waiting on the wrong one is
		// the defect rather than the diagnosis.
		const numeric_result parsed = parse_integer(argv[i], numeric_site::wait_pid_operand);
		if (parsed.status != numeric_parse::ok) {
			status = report_bad_number("wait", argv[i], parsed.status);
			continue;
		}
		const pid_t target = static_cast<pid_t>(parsed.value);
		int wait_status = 0;
		// No WUNTRACED, for the reason given at the no-operand form above.
		if (waitpid(target, &wait_status, 0) > 0) {
			status = status_from_wait(wait_status);
		} else {
			// POSIX DEFINES the answer for a pid that is not a child: status 127.
			// No diagnostic, because this is a specified result rather than a
			// failure - dash is silent here too.
			status = 127;
		}
		std::erase(_background, target);
	}
	return status;
}

int tree_walking_executor::run_simple_command(const tree& t, node_index n) {
	// A command with NO WORDS AT ALL is not a command: it is what is left when an
	// alias substituted to nothing but blanks, and POSIX leaves `$?` alone for it.
	// `set +e; false; b` with `alias b=' '` reports 1 in dash, where returning 0
	// here reported success for a line that never ran anything. Distinct from a word
	// that EXPANDS to nothing - `$unset` alone is a command that ran and succeeded.
	if (t[n].children_count == 0)
		return _state.last_status();

	// POSIX 2.9.1: a command with no command name completes with the status of the
	// LAST command substitution it performed, and only zero when it performed none.
	// So `x=$(exit 3); echo $?` prints 3. Counting substitutions is how "performed
	// none" is told apart from "performed one that happened to succeed" - reading
	// $? alone cannot distinguish them.
	const uint64_t substitutions_before = _substitutions;

	arena_array<char*> argv{_pool, 8};
	arena_array<std::string_view> assignments{_pool, 4};
	_expansion_error = false;
	const bool has_command = build_argv(t, n, argv, &assignments);
	// An expansion that failed fatally - `set -u` on an unset parameter, or
	// `${x?}` - must not go on to run anything. build_argv has already arranged
	// for a non-interactive shell to stop; all that is left is the status, which
	// dash reports as 2.
	if (_expansion_error)
		return kExpansionError;

	if (!has_command) {
		// The redirections come first, and only their success lets the assignments
		// through: dash leaves x UNSET after `x=1 </missing` and sets it after
		// `x=1 </dev/null`. Forking is skipped when there is nothing to redirect,
		// because a bare `x=1` is the most common command in any script.
		if (has_redirections(t, n)) {
			const int status = run_redirections_only(t, n);
			if (status != 0)
				return status;
		}
		// Assignments with no command persist in the shell: `x=1` is how a variable
		// gets set. Previously these were parsed and silently dropped.
		//
		// The values are expanded HERE, not in build_argv, so a substitution inside
		// one is counted by the check below.
		//
		// Expanded and applied one at a time, in order, because `a=1 b=$a` must see
		// a's new value - dash does the same - and the expanded text is kept so
		// `set -x` can print the values the assignments took without expanding a
		// second time, which would run a command substitution in one of them twice.
		arena_array<std::string_view> traced{_pool, 4};
		for (const auto& a : assignments) {
			const std::string_view expanded = expand_assignment(a);
			// Checked BEFORE the assignment lands: dash leaves x unset after
			// `sh -u -c 'x=${y}'` rather than assigning the empty string it never
			// managed to expand.
			if (_expansion_error)
				return kExpansionError;
			if (!apply_expanded_assignment(expanded))
				return assignment_error();
			traced.push(expanded);
		}
		if (_state.opts().trace)
			trace_command(traced, nullptr);
		return _substitutions != substitutions_before ? _state.last_status() : 0;
	}

	// `command name args...` runs a command bypassing FUNCTION lookup. Stripping
	// the prefix here and carrying a flag is simpler than threading a mode through
	// the search, and it is the executor that owns the search order anyway.
	//
	// Implementing only `-v` and letting every other use silently succeed made
	// `command echo hi` do nothing - a stub that succeeds is worse than an absent
	// builtin, which is the mistake #24 explicitly warned about and I made anyway.
	// `-V` and `-p` were then taken for command NAMES, which is the same failure
	// one step further out: `command -p ls` reported `-p: No such file or
	// directory`.
	// An option `command` does not have leaves argv ALONE, `command` still at the
	// front, so the diagnostic is written by try_run_executor_builtin with this
	// command's redirections applied. dash is silent for `command -z cat
	// 2>/dev/null`.
	const command_prefix cmd = take_command_prefix(argv);
	// A readonly name in an assignment PREFIX is a variable assignment error, and
	// POSIX makes it fatal BEFORE the command runs: `readonly a=1; a=2 echo prefix`
	// prints nothing at all in dash. Asked here, ahead of every dispatch path,
	// because each applies the prefix somewhere else - a builtin's is applied in
	// this process, an external command's in the CHILD, where a refusal could no
	// longer exit the shell, and a regular builtin's not at all yet.
	for (const auto& a : assignments) {
		const size_t eq = a.find('=');
		if (eq == std::string_view::npos || !_state.is_readonly(a.substr(0, eq)))
			continue;
		// REPORTED THROUGH THIS COMMAND'S OWN REDIRECTIONS, which is the rule
		// `command`'s bad-option path states thirty lines up: reported where the
		// command runs, not where the prefix was read. `readonly r=1;
		// r=2 : 2>/dev/null` leaked `r: is read only` to the terminal, where dash is
		// silent because the message goes through the `2>/dev/null` (#73). One rule,
		// two places, and only one of them had it.
		//
		// The redirections are PERFORMED and not merely consulted - dash creates and
		// truncates the file before it reports - and then put straight back, because
		// this command is not going to run. The speculative-open hazard the function
		// lookup below documents does not apply: there is no second, real open
		// afterwards for a FIFO's reader to have gone away before.
		arena_array<saved_fd> saved{_pool, 4};
		const bool redirected = apply_redirections(t, n, &saved);
		// A redirection that FAILED has already said so, and dash reports that and
		// never mentions the variable. Reporting both would invent a second
		// diagnostic the reference shell does not write - and fd 2 is in an unknown
		// state at that point anyway.
		if (redirected)
			shell_state::report_readonly({}, a.substr(0, eq));
		// Before restore_fds, or the message sits in the FILE* buffer and is flushed
		// after the fds are back - onto the terminal, which is the leak this fixes.
		std::fflush(nullptr);
		restore_fds(saved);
		// Either way the shell's fate is the same and so is the status: an
		// assignment error and a redirection error are both 2, and both are fatal
		// to a non-interactive shell.
		return assignment_error();
	}

	// `command`, `command -p` and `command --` with nothing after them are commands
	// that do nothing and succeed; dash reports 0 for all three. AFTER the readonly
	// check above, not before it: `readonly a=a; a=b command` is still an assignment
	// error that kills a non-interactive shell, which is three of error-p.tst's
	// assertions and the regression that returning early here caused.
	if (argv.empty() || argv[0] == nullptr)
		return 0;

	// `set -x` traces the command once, HERE - before the search order decides
	// whether this is a function, a builtin or a PATH lookup, so every one of them
	// is traced exactly once and none of the paths below has to remember to.
	//
	// A `NAME=value` prefix is NOT shown. dash prints it, but printing it here
	// would mean expanding the value in this process purely to trace it, and the
	// value is expanded in the CHILD for an external command - so a command
	// substitution in the prefix would run twice under `-x` and once without it.
	// A trace that changes what the command does is worse than a trace that omits
	// a field. Moving that expansion into the parent is its own change.
	if (_state.opts().trace) {
		const arena_array<std::string_view> no_prefix{_pool, 1};
		trace_command(no_prefix, argv.data());
	}

	// A function shadows an external command but NOT a special builtin, per
	// POSIX's command search order: special builtins, then functions, then regular
	// builtins, then PATH.
	//
	// The function must be looked up BEFORE the redirections are applied. Applying
	// them to find out whether one exists, then undoing them, is not free: opening
	// a file creates it and truncates it, and opening a FIFO BLOCKS until the other
	// end appears and then hands that reader an immediate EOF when the speculative
	// fd is closed. The re-open on the real path then waits for a reader that has
	// already gone, so `echo foo >fifo & cat fifo` lost its output and
	// `cat fifo & echo foo >fifo` hung outright - a deadlock built out of a lookup.
	if (!cmd.present && classify_builtin(argv[0]) != builtin_kind::special &&
	    _state.has_function(argv[0])) {
		arena_array<saved_fd> saved{_pool, 4};
		int status = 0;
		std::fflush(nullptr);
		const bool ok = apply_redirections(t, n, &saved);
		// POSIX: an assignment prefixing a FUNCTION call affects the current
		// environment for the duration of the call, and whether it persists
		// afterwards is unspecified. dash and bash both restore, and so does the
		// conformance suite's expectation: `foo=2; foo=3 f` shows 3 inside f and 2
		// after. Applying them AFTER the call - which is what this did - both hid
		// them from the function and made them permanent.
		std::vector<saved_variable> restore_vars;
		// A refused prefix assignment must not skip the restores below - an early
		// return here would leave the redirections applied to the shell's own fds.
		// The pre-check above makes this unreachable; leaving the path correct is
		// cheaper than relying on that.
		bool refused = false;
		if (ok)
			refused = !apply_prefix(assignments, /*persist=*/false, restore_vars);
		bool ran = false;
		if (ok && !refused)
			ran = try_run_function(t, argv, status);
		restore_prefix(restore_vars);
		std::fflush(nullptr);
		restore_fds(saved);
		if (!ok)
			return kRedirectionError;
		if (refused)
			return assignment_error();
		if (ran)
			return status;
	}

	// `command`, `eval`, `.`, `exec` and `wait` live in the executor rather than in
	// builtins.cpp; see try_run_executor_builtin.
	{
		int status = 0;
		if (try_run_executor_builtin(t, n, argv, assignments, cmd, status))
			return status;
	}

	// A builtin runs in THIS process - `cd` in a forked child would change the
	// child's directory and exit, achieving nothing. So dispatch happens before
	// the fork, not after.
	if (classify_builtin(argv[0]) != builtin_kind::none) {
		// A builtin runs in THIS process, so its redirections must be undone
		// afterwards or they would leak into the shell's own fds. dash does the
		// same save-and-restore; the alternative is forking, which would defeat
		// the point of a builtin.
		arena_array<saved_fd> saved{_pool, 4};
		// Flush BEFORE redirecting and again before restoring. A builtin writes
		// through stdio, so its bytes sit in the FILE* buffer: without the first
		// flush, output queued earlier lands in the redirected file, and without
		// the second, the builtin's own output is flushed after the fds are put
		// back and appears on the terminal instead of in the file. The redirection
		// was correct all along; the buffering was not.
		std::fflush(nullptr);
		// Cleared so the check after the call reports THIS builtin's write failure
		// and not one inherited from something earlier.
		std::clearerr(stdout);
		const bool ok = apply_redirections(t, n, &saved);
		builtin_result result{};
		// POSIX: assignments preceding a SPECIAL builtin persist afterwards; before a
		// regular one they apply only for its duration. Not cosmetic - it is why
		// `x=1 export y` leaves x set and `x=1 cd /tmp` does not.
		//
		// A `command` prefix DEMOTES the special builtin to a regular one, and the
		// assignment follows the demotion: `a=a; a=b command :; echo $a` prints `a`
		// in dash and printed `b` here, which is command-p.tst's 'assignment on
		// special built-in is temporary'.
		const bool persist =
			classify_builtin(argv[0]) == builtin_kind::special && !cmd.present;
		// POSIX 2.9.1 performs the assignments AFTER the redirections and BEFORE the
		// command, so the builtin can READ them. They used to be applied after the
		// call, and only in the persisting case, under a note saying no builtin read a
		// variable set that way. `read` does: every field-splitting case in
		// read-p.tst is `IFS=' -' read a b c`, and with the prefix invisible twelve of
		// them split on the default IFS and joined the whole line into one variable.
		std::vector<saved_variable> restore_vars;
		// A refused prefix assignment must not skip the restores below, so it is a
		// flag rather than an early return. The readonly pre-check above makes it
		// unreachable; leaving the path correct is cheaper than relying on that.
		bool refused = false;
		if (ok)
			refused = !apply_prefix(assignments, persist, restore_vars);
		if (ok && !refused) {
			if (!try_run_builtin(_state, argv.data(), result, cmd.present)) {
				// A CLASSIFIED name with no implementation. The registry guard in
				// builtins.cpp makes this a compile error, and this branch is what
				// happens if the guard is ever removed: 127 and a diagnostic rather
				// than the silent success that made `test 1 = 2` report 0 (#35). The
				// return value used to be discarded with a `(void)`, which is what let
				// that through.
				report("%s: not implemented", argv[0]);
				result.status = 127;
			}
		} else if (!ok) {
			result.status = kRedirectionError;
			// POSIX 2.8.1: a redirection error on a SPECIAL builtin exits a
			// non-interactive shell; on a regular one it does not. Verified against
			// dash, which exits 2 for `: </missing` and reports 2 and continues for
			// `echo x </missing`. `command : </missing` is demoted to regular and
			// survives, which is what bypass_functions records.
			if (!cmd.present && !_state.interactive() &&
			    classify_builtin(argv[0]) == builtin_kind::special)
				_exit_requested = true;
		}
		std::fflush(nullptr);
		if (ok && drop_unwritable_output(argv[0]))
			result.status = 1;
		restore_fds(saved);
		restore_prefix(restore_vars);
		if (refused)
			result.status = assignment_error();

		if (result.flow == control_flow::exit_shell) {
			_exit_requested = true;
		} else if ((result.flow == control_flow::break_loop ||
		            result.flow == control_flow::continue_loop) &&
		           _loop_depth == 0) {
			// A `break` or `continue` with NO ENCLOSING LOOP. POSIX leaves it
			// unspecified, and dash makes it a silent no-op that the commands after it
			// carry on from: `break; echo x` prints x, and so does `{ break; echo x; }`
			// - where lesh printed nothing at all, having unwound a brace group that
			// no loop was waiting behind. zsh diagnoses it and stops the input
			// instead; ADR-0001 makes dash authoritative for the POSIX floor, and
			// nothing in the conformance suite asserts either answer.
			//
			// Deliberately silent, and that is worth stating because this project's
			// repeated lesson is the opposite - `command`, `test`, `set -o pipefail`,
			// `kill -l EXITSTATUS`, `kill` with no operand and `unalias -a` each
			// silently succeeded and each cost a debugging session. The difference is
			// that those did nothing where POSIX required something; this one does
			// nothing where POSIX requires nothing and the reference shell does
			// nothing either, and a diagnostic here would be lesh inventing an error
			// the floor does not have.
		} else if (result.flow != control_flow::normal) {
			// break, continue and return unwind through the enclosing construct.
			_flow = result.flow;
			_flow_level = result.level;
		}
		return result.status;
	}

	// The prefix values are expanded HERE, in the shell that read the command, and
	// not in the child that will be replaced by it. POSIX 2.9.1 performs every
	// expansion before the command runs, and the child had no command runner to
	// reach - so a command substitution in a prefix value expanded to nothing and
	// `x=$(echo z) sh -c 'echo $x'` exported x empty (#31).
	arena_array<std::string_view> expanded{_pool, 4};
	if (!expand_prefix(assignments, expanded))
		return kExpansionError;

	// `foreground`: this is the one call site that has the shell waiting on the
	// child with nothing between them, so it is the one that hands over the
	// terminal (#158 decision 3). A background job forks through run_async and a
	// substitution through run_substitution, and both cleared the saved tty on the
	// way past `enter_subshell`, so neither can reach this even though both end up
	// in this function one process further down.
	const pid_t pid = spawn(argv, {.foreground = true}, &expanded, &t, n,
	                        cmd.standard_path);
	if (pid == -1)
		return 1;

	int wait_status = 0;
	// WUNTRACED, AND WITHOUT IT THIS WAIT NEVER RETURNS (#161). #159 handed the
	// terminal to this child and reset its SIGTSTP to default, which is what makes
	// Ctrl-Z genuinely stop a foreground command - and a plain `waitpid` does not
	// report a stop, so the shell blocked here forever on a process that was never
	// going to exit. This is the one wait in the file that Ctrl-Z can reach.
	waitpid(pid, &wait_status, WUNTRACED);
	// IMMEDIATELY, not at end of line (#158 decision 2). `nvim .; read x` is the
	// case: the loop's reclaim in resume_after_execution runs once the whole line
	// is done, and by then `read` has already met EIO on a terminal it was not the
	// foreground group of.
	//
	// ON THE STOPPED PATH TOO, and that is why it stays here rather than moving
	// beside the status. A stopped child still owns the terminal it was handed, so
	// without this the prompt we are about to return to would be reading from a
	// terminal whose foreground group is a process that is not running - every
	// keystroke lost, which is the same defect as `nvim .; read x` wearing a
	// different hat. Before the diagnostic below, so the diagnostic is not itself
	// a background write.
	reclaim_terminal();
	// And the interrupt the shell was excluded from by the very handoff above. The
	// flag only; the between-commands boundary decides what it means, with `$?`
	// already this command's 130. A stopped status passes through harmlessly -
	// WIFSIGNALED is false for a stop.
	note_interrupt_after_handoff(wait_status);
	return foreground_status(pid, wait_status);
}

// One simple-command pipeline stage, in the process forked for it, with the pipe
// already on its fds.
//
// The words are expanded HERE and not in the shell before the fork, which is the
// whole point: `echo a | echo $(cat)` expanded in the shell ran `cat` on the
// SHELL's fd 0, so it printed nothing under /dev/null and hung outright on a
// terminal (#50). POSIX 2.9.1 then performs the redirections, so `echo $(cat)
// <file` still substitutes from the pipe and only afterwards moves fd 0.
int tree_walking_executor::run_pipeline_stage(const tree& t, node_index stage) {
	// A stage with NO WORDS AT ALL - what is left when an alias substituted to
	// blanks - is not a command, and POSIX leaves `$?` alone for it. Same rule as
	// run_simple_command; `echo foo | e` with `alias e=' '` is the shape that
	// stalled a probe on #40.
	if (t[stage].children_count == 0)
		return _state.last_status();

	// POSIX 2.9.1: a command with no command name completes with the status of the
	// LAST command substitution it performed, and zero when it performed none.
	const uint64_t substitutions_before = _substitutions;

	arena_array<char*> argv{_pool, 8};
	arena_array<std::string_view> assignments{_pool, 4};
	_expansion_error = false;
	const bool has_command = build_argv(t, stage, argv, &assignments);
	// A fatal expansion error - `${x?}`, or `set -u` on an unset parameter - stops
	// the shell it happens in, and that shell is THIS one: the stage's subshell.
	// Expanding in the parent made `echo a | echo ${x?bad}` exit the whole shell,
	// so the command after the pipeline never ran; dash reports 2 for the stage and
	// carries on.
	if (_expansion_error)
		return kExpansionError;

	if (!has_command) {
		// No command name still performs the redirections and the assignments, in a
		// subshell - which this process already is, so they are applied directly
		// rather than through run_redirections_only, which forks one of its own.
		// The stage used to be skipped entirely, so `echo a | >out` created no file.
		if (has_redirections(t, stage) && !apply_redirections(t, stage, nullptr))
			return kRedirectionError;
		for (const auto& a : assignments) {
			const std::string_view expanded = expand_assignment(a);
			if (_expansion_error)
				return kExpansionError;
			if (!apply_expanded_assignment(expanded))
				return assignment_error();
		}
		return _substitutions != substitutions_before ? _state.last_status() : 0;
	}

	// A `command` prefix has to be stripped HERE too, and not only in
	// run_simple_command. Without it argv[0] stayed `command`, the registry said
	// that was a builtin, and the handler table had no entry for it - so
	// `echo hi | command cat` ran nothing at all and reported success, which is the
	// stub-that-succeeds failure once more.
	const command_prefix cmd = take_command_prefix(argv);
	if (argv.empty() || argv[0] == nullptr)
		return 0;

	// A function or builtin in a pipeline stage runs in ITS OWN process - this one -
	// so its effects do not reach the shell. POSIX allows either, and running it in
	// a subshell is what dash does, which is why `f | cat` cannot set a variable in
	// the parent and `echo a | read x` cannot either.
	//
	// A `command` prefix takes the FUNCTION table out of the question, so a stage
	// written `command f` where f is only a function is an external command that
	// will not be found - which is what dash reports for it.
	const bool in_process = (!cmd.present && _state.has_function(argv[0])) ||
	                        classify_builtin(argv[0]) != builtin_kind::none;
	if (in_process) {
		// The executor's own builtins are dispatched here rather than through
		// try_run_builtin, which has no entry for any of them: `exec echo foo | cat`
		// printed nothing, and so did `echo hi | eval cat`. They come before the
		// apply_redirections below because each applies the stage's redirections
		// itself where it needs them.
		int status = 0;
		if (try_run_executor_builtin(t, stage, argv, assignments, cmd, status))
			return status;
		// nullptr for `restore`: this process exists only for this stage, so there
		// is nothing to put the fds back for. Its status matters though - a failed
		// redirection has to make the STAGE fail, and discarding the result ran the
		// builtin anyway, on the unredirected fds.
		if (!apply_redirections(t, stage, nullptr))
			return kRedirectionError;
		if (cmd.present || !try_run_function(t, argv, status)) {
			builtin_result r{};
			// The discard is safe HERE and nowhere by default, so the invariant is
			// written down rather than left to be re-derived: this branch is reached
			// only when `in_process` above found classify_builtin() != none, and #35
			// made the registry static_assert both ways, so a classified name always
			// has a handler. Discarding this same return where that did NOT hold is
			// what made `test 1 = 2` return 0.
			// `cmd.present` for the same reason the branch above passes it, though this
			// stage discards `r.flow` either way: a stage is its own process and has no
			// shell left to end. Passing it keeps the two sites saying the same thing.
			const bool handled = try_run_builtin(_state, argv.data(), r, cmd.present);
			LESH_ASSERT(handled);
			(void)handled;
			status = r.status;
		}
		return status;
	}

	// An external command: this process becomes it. No second fork - the stage was
	// forked before its words were expanded, so the process that expanded them is
	// the one that execs - and the prefix is expanded here for the same reason the
	// words are, beside them rather than inside become_command.
	arena_array<std::string_view> expanded{_pool, 4};
	if (!expand_prefix(assignments, expanded))
		return kExpansionError;
	become_command(argv, &expanded, &t, stage, cmd.standard_path);
}

int tree_walking_executor::run_pipeline(const tree& t, node_index n) {
	const node& self = t[n];
	LESH_ASSERT(self.children_count >= 2);

	arena_array<pid_t> pids{_pool, self.children_count};
	pid_t group = 0;  // the first child becomes the leader; the rest join it
	int input_fd = STDIN_FILENO;

	for (uint32_t i = 0; i < self.children_count; ++i) {
		const bool is_last = (i + 1 == self.children_count);

		int pipe_fds[2] = {-1, -1};
		if (!is_last && pipe(pipe_fds) == -1) {
			report("pipe: %s", std::strerror(errno));
			break;
		}

		const node_index stage = t.child_of(self, i);

		// The fork comes FIRST, before anything in the stage is evaluated. POSIX
		// 2.9.2 puts each stage in a subshell environment with its input already
		// connected, and expanding the words is part of running the command - so a
		// command substitution in them must see the PIPE on fd 0. Expanding in the
		// shell first, which is what this did, made `echo a | echo $(cat)` read the
		// shell's own stdin: empty against /dev/null and a HANG on a terminal. See
		// issue #50.
		std::fflush(nullptr);
		const pid_t pid = fork();
		if (pid == 0) {
			setpgid(0, group);
			// A stage is a SUBSHELL ENVIRONMENT, and this is the one place per stage
			// that is already inside it - after the fork, before anything in the
			// stage is evaluated. So the traps reset here, exactly as they do for
			// `( ... )`, for `&` and for `$( ... )`: the handlers this shell set
			// belong to it, and only an IGNORE carries in. Without it a stage RAN THE
			// PARENT'S HANDLERS - `trap "echo TRAP" USR1; { "$TESTEE" -c 'kill -s
			// USR1 $PPID'; echo body; } | cat` printed TRAP and carried on where dash
			// and bash are killed by the signal (#53).
			//
			// enter_subshell rather than reset_for_subshell alone, because the trap
			// action's entry status goes with it - see shell_state. It is also what
			// drops #52's interactive defaults, which belong to the process that
			// reads commands and has a prompt to return to and not to a stage; and it
			// leaves #37's ignored-on-entry rule alone, which is why a signal the
			// shell was invoked ignoring is still untrappable in a stage.
			_state.enter_subshell();
			// The stage gets its OWN EXIT trap, and the flag must be cleared for it -
			// the same reason run_subshell clears it, for a stage forked from inside
			// the parent's EXIT trap.
			_exit_trap_ran = false;
			if (input_fd != STDIN_FILENO) { dup2(input_fd, STDIN_FILENO); close(input_fd); }
			if (!is_last) { dup2(pipe_fds[1], STDOUT_FILENO); close(pipe_fds[1]); }
			// A stage may be a COMPOUND command - `echo x | { read v; ... }` and
			// `... | while read l; do ...; done` are both ordinary shell. Only a
			// simple command has words to expand and a name to dispatch on, which is
			// all run_pipeline_stage adds over run_node.
			int status = t[stage].kind == node_kind::simple_command
			                 ? run_pipeline_stage(t, stage)
			                 : run_node(t, stage);
			// And it RUNS that trap on the way out, which is the answer run_subshell
			// gave in 7a52868 and the answer a stage has to match: `{ trap "echo S"
			// EXIT; echo body; } | cat` prints body and then S in dash, and printed
			// only body here. An EXTERNAL command never reaches this - the stage
			// EXECS and the trap goes with the process it replaced, which is what
			// POSIX says and what dash does.
			int from_trap = 0;
			if (run_exit_trap(from_trap))
				status = from_trap;
			std::fflush(nullptr);
			_exit(status);
		}
		if (pid == -1)
			report("fork: %s", std::strerror(errno));
		if (pid > 0) {
			setpgid(pid, group == 0 ? pid : group);
			pids.push(pid);
			if (group == 0)
				group = pid;
		}

		// Close our copies. Every stage must close the ends it does not use, or a
		// reader waits forever on a writer that is still open in a process that
		// will never write - the classic pipeline hang.
		if (input_fd != STDIN_FILENO)
			close(input_fd);
		if (!is_last) {
			close(pipe_fds[1]);
			input_fd = pipe_fds[0];
		}
	}

	// Every member must be waited on or it becomes a zombie. POSIX defines the
	// pipeline's status as its LAST command's - unless `set -o pipefail`, which
	// makes it the status of the RIGHTMOST member that failed, and zero when none
	// did. `exit 1 | exit 2 | exit 0` is 0 without the option and 2 with it.
	//
	// WUNTRACED HERE TOO, AND THE LOOP KEEPS GOING (#161). A pipeline is a
	// foreground job, and a member of it can stop: today only by a deliberate
	// `kill -STOP`/`-TSTP` from elsewhere, because the stages inherit this shell's
	// ignored SIGTSTP and are not the terminal's foreground group, and once #160
	// hands the group leader the terminal Ctrl-Z will reach them the way it
	// reaches a simple command. Either way, a plain wait on a stopped member is a
	// permanent hang.
	//
	// THE DECISION IS THAT A STOPPED MEMBER DOES NOT END THE LOOP. `break`ing out
	// would leave the other stages unwaited, and the first of them to exit becomes
	// a zombie the shell never reaps - the leak this ticket's acceptance criteria
	// forbid by name, and one no job table exists to clean up after. So every pid
	// is still waited exactly once, a stopped one contributes 128+WSTOPSIG as its
	// member status, and the pipeline's status is composed from those as usual.
	//
	// It does NOT follow that the prompt comes back: if an EARLY stage stops, the
	// stage after it blocks on a pipe that will never close, and the wait below
	// blocks with it. That is the floor and not a regression - measured on this
	// machine, `sleep 30 | cat` with either stage sent SIGSTOP hangs dash, zsh AND
	// bash outright, none of them reporting anything. lesh at least names the pid
	// that stopped before it waits, and where the LAST stage is the one that stops
	// (the earlier ones already reaped) it returns to the prompt where the other
	// three still do not. Getting further than that needs the job table #98 keeps
	// out of scope, which is the seam `foreground_status` marks.
	int last_status = 0;
	int rightmost_failure = 0;
	for (size_t i = 0; i < pids.size(); ++i) {
		int wait_status = 0;
		waitpid(pids[i], &wait_status, WUNTRACED);
		const int status = foreground_status(pids[i], wait_status);
		if (status != 0)
			rightmost_failure = status;
		if (i + 1 == pids.size())
			last_status = status;
	}
	// The option is read AFTER the pipeline ran, so a stage that turns it on cannot
	// change the status of the pipeline it belongs to - pipeline-p.tst's 'pipeline
	// enabling pipefail does not affect itself'. Every stage forks, so it could not
	// anyway, but the read order is what says so rather than the process model.
	return _state.opts().pipefail ? rightmost_failure : last_status;
}

substitution_result tree_walking_executor::capture(std::string_view code,
                                                  arena_array<char>& out) {
	// PARSED ON THIS SIDE OF THE FORK, and that is the whole of #57.
	//
	// The parse used to happen in the child, which refused correctly and `_exit(2)`,
	// and nothing could read that: an exit status of 2 from a body that will not
	// parse is byte-identical to one from a body that ran `exit 2`, so `echo
	// $(if true)` printed its diagnostic and then reported success. A syntax error
	// is a property of the INPUT, and the only place it can be told apart from a
	// status is where the parser runs.
	//
	// dash reaches the same answer from the other direction - it parses a
	// substitution while parsing the command around it - and refuses at 2 without
	// running anything. Parsing here costs nothing and saves a fork: a body that
	// will not parse no longer forks at all.
	//
	// This parse is now a CHECK and nothing else: the child reads the body a
	// command at a time and parses it again as it goes (#67). Two parses of the
	// same text, deliberately, because they answer different questions and only
	// one of them can be asked here. "Will this parse" has to be answered before
	// the fork, since a status cannot carry it back. "What does the next command
	// mean" cannot be, because the answer depends on aliases the EARLIER commands
	// of this same body define, and defining them means running them - in the
	// child. `x=$(alias false=:<newline>false)` is that difference, and yash is
	// the shell that gets it right.
	buffer_pool inner_pool{BUFFER_POOL_SIZE};
	const tree inner = syntax::parse(inner_pool, code, &_state);
	// has_errors() and never incomplete(): the body arrived whole, so there is no
	// more input to continue it with - the same reading run_parsed gives the top
	// level, and the reason `$(if true` and `$(if true)` answer alike.
	if (inner.has_errors()) {
		// Guarded, because `inner` is a LOCAL tree and report_syntax_error points the
		// shell's position at the defect inside it. Left there, the pointer would
		// outlive the tree by the width of an EXIT trap, which can read `$LINENO`.
		const origin_guard origin{_state};
		report_syntax_error(_state, inner);
		return substitution_result::malformed;
	}

	int pipe_fds[2];
	if (pipe(pipe_fds) == -1) {
		report("pipe: %s", std::strerror(errno));
		return substitution_result::unavailable;
	}

	std::fflush(nullptr);
	const pid_t pid = fork();
	if (pid == -1) {
		report("fork: %s", std::strerror(errno));
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		return substitution_result::unavailable;
	}

	if (pid == 0) {
		// A subshell: a separate environment whose changes do not escape. Forking
		// gives that for free - the child mutates its own copy of shell state.
		setpgid(0, 0);
		close(pipe_fds[0]);
		dup2(pipe_fds[1], STDOUT_FILENO);
		close(pipe_fds[1]);

		// A subshell environment, so the traps reset here too. Without this the
		// PARENT's EXIT trap ran inside every command substitution - `trap 'echo T'
		// EXIT; x=$(echo body)` put T into x.
		_state.enter_subshell();
		_exit_trap_ran = false;
		// ONE COMPLETE COMMAND AT A TIME, the way every other shell input is read.
		// A substituted body is shell input like any other: POSIX 2.3.1 has the
		// shell read a command, run it, and only then read the next, which is why an
		// alias defined on one of its lines is in effect on the following one.
		// Running the whole-body tree parsed above instead made the body the one
		// place in the shell where that was not true (input-p.tst's 'shell input is
		// line-wise (command substitution)').
		//
		// run_source, not run_input: `eval` and `.` already read exactly this way,
		// into a nested pool whose trees live only as long as the call, and a third
		// spelling of the same loop is the shape #34, #35, #49 and #63 each cost
		// this project once already.
		int status = run_source(code, /*echo_as_read=*/false);
		// The EXIT trap is the child's own - enter_subshell() cleared the parent's
		// above - and it runs on the way out of the substitution: `x=$(trap "echo T"
		// EXIT; echo body)` puts both lines in x. run_source does not run it,
		// because `eval` and `.` are not the end of anything.
		int from_trap = 0;
		if (run_exit_trap(from_trap))
			status = from_trap;
		std::fflush(nullptr);
		_exit(status);
	}

	setpgid(pid, pid);
	close(pipe_fds[1]);

	// Drain before waiting. Waiting first would deadlock the moment the output
	// exceeds the pipe buffer: the child blocks writing, we block waiting.
	char buffer[4096];
	for (;;) {
		const ssize_t got = read(pipe_fds[0], buffer, sizeof(buffer));
		if (got <= 0)
			break;
		for (ssize_t i = 0; i < got; ++i)
			out.push(buffer[i]);
	}
	close(pipe_fds[0]);

	int wait_status = 0;
	// NO WUNTRACED (#161). A command substitution never receives the terminal -
	// #158 decision 3 forbids it by name, since a `$(read x)` stealing the tty from
	// the line editor would be its own bug - so Ctrl-Z cannot reach this child, and
	// there is no prompt to return to either: the substitution's output is a word
	// the shell is still in the middle of expanding, and a stop has no status the
	// enclosing command could be finished with. The read above has already drained
	// the pipe to EOF, so by here the child is on its way out regardless.
	waitpid(pid, &wait_status, 0);
	_state.set_last_status(status_from_wait(wait_status));
	++_substitutions;
	return substitution_result::ok;
}

} // namespace lesh::runtime
