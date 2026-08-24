#include "runtime/executor.h"

#include "runtime/builtins.h"
#include "runtime/pattern.h"
#include "runtime/signals.h"
#include "substrate/assert.h"
#include "syntax/parser.h"

#include <cerrno>
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

// A variable's value before an assignment prefix overwrote it, so a function call
// can put it back. Owns its strings: the state's own storage is what is being
// overwritten, so a view into it would dangle.
struct saved_variable {
	std::string name;
	std::string value;
	bool was_set = false;
};

// Turns a wait(2) status into the value POSIX defines for `$?`.
int status_from_wait(int wait_status) noexcept {
	if (WIFEXITED(wait_status))
		return WEXITSTATUS(wait_status);
	if (WIFSIGNALED(wait_status))
		return 128 + WTERMSIG(wait_status);
	return 0;
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
	std::fprintf(stderr, "lesh: %s: %s\n", argv[0], std::strerror(failure));
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
		std::fprintf(stderr, "lesh: %d: %s\n", fd, std::strerror(errno));
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
			int source = 0;
			for (const char c : target) {
				if (c < '0' || c > '9') {
					std::fprintf(stderr, "lesh: %s: bad file descriptor\n", path);
					return false;
				}
				source = source * 10 + (c - '0');
			}
			// POSIX 2.7.5/2.7.6: `>&n` is an error unless n is open for OUTPUT and
			// `<&n` unless n is open for INPUT, so the access mode is checked before
			// anything is displaced. dup2 alone cannot tell - it happily aims a
			// read-only fd at stdout, which is why `3</dev/null >&3` succeeded here
			// and in dash, and why both fail redir-p.tst's 'output duplication,
			// failure (unreadable file descriptor)'.
			const int flags = fcntl(source, F_GETFL);
			if (flags == -1) {
				std::fprintf(stderr, "lesh: %d: %s\n", source, std::strerror(errno));
				return false;
			}
			const int access = flags & O_ACCMODE;
			const bool writable = access == O_WRONLY || access == O_RDWR;
			const bool readable = access == O_RDONLY || access == O_RDWR;
			if (op_token.kind == token_kind::great_and ? !writable : !readable) {
				std::fprintf(stderr, "lesh: %d: %s\n", source,
				             op_token.kind == token_kind::great_and
				                 ? "not open for output" : "not open for input");
				return false;
			}
			if (dup2(source, fd) == -1) {
				std::fprintf(stderr, "lesh: %d: %s\n", source, std::strerror(errno));
				return false;
			}
			return true;
		}
		default:
			return true;
	}

	if (opened == -1) {
		std::fprintf(stderr, "lesh: %s: %s\n", path, std::strerror(errno));
		return false;
	}
	if (opened != fd) {
		dup2(opened, fd);
		close(opened);
	}
	return true;
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
		std::fprintf(stderr, "lesh: pipe: %s\n", std::strerror(errno));
		return false;
	}

	// A body that fits the pipe buffer is written directly; a larger one needs a
	// writer process, or the write blocks before the reader has been started.
	long buffer_capacity = fpathconf(pipe_fds[1], _PC_PIPE_BUF);
	if (buffer_capacity <= 0)
		buffer_capacity = 512;
	if (text.size() <= static_cast<size_t>(buffer_capacity)) {
		if (!text.empty())
			(void)!write(pipe_fds[1], text.data(), text.size());
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
int tree_walking_executor::run_redirections_only(const tree& t, node_index n) {
	std::fflush(nullptr);
	const pid_t pid = fork();
	if (pid == -1) {
		std::fprintf(stderr, "lesh: fork: %s\n", std::strerror(errno));
		return kRedirectionError;
	}
	if (pid == 0) {
		setpgid(0, 0);
		const bool ok = apply_redirections(t, n, nullptr);
		std::fflush(nullptr);
		// _exit, not exit: no EXIT trap and no buffers, because this subshell is
		// implicit and never ran a command of its own.
		_exit(ok ? 0 : kRedirectionError);
	}
	setpgid(pid, pid);
	int wait_status = 0;
	waitpid(pid, &wait_status, 0);
	return status_from_wait(wait_status);
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
	std::fprintf(stderr, "lesh: %s: I/O error\n", name);
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
	const int status = run_parsed(t);
	// The EXIT trap runs on the way out, whether that is the end of input or an
	// explicit `exit` - which is why it cannot live at the `exit` builtin.
	run_exit_trap();
	return status;
}

int tree_walking_executor::run_input(std::string_view source, bool echo_when_verbose) {
	int status = _state.last_status();
	size_t at = 0;
	while (at < source.size()) {
		const size_t from = at;
		// The trees are kept, not dropped: a function defined by one command is a
		// node in the tree that command was parsed from, and the next command may
		// call it. Reading a command at a time is what makes that lifetime visible -
		// with one whole-input parse it held for free.
		_input_trees.push_back(syntax::parse_next_command(_pool, source, at, &_state));
		echo_if_verbose(source.substr(from, at - from), echo_when_verbose);
		status = run_parsed(_input_trees.back());
		if (_exit_requested)
			break;
	}
	run_exit_trap();
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
static void report_syntax_error(const tree& t) {
	const syntax::node_index at = t.first_error();
	const char* detail = at == syntax::no_node ? nullptr : t.error_detail(t[at]);
	if (detail != nullptr)
		std::fprintf(stderr, "lesh: syntax error: %s\n", detail);
	else
		std::fprintf(stderr, "lesh: syntax error\n");
}

int tree_walking_executor::run_parsed(const tree& t) {
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
		report_syntax_error(t);
		_exit_requested = true;
		return 2;
	}

	const node& program = t[t.root()];
	int status = _state.last_status();
	for (uint32_t i = 0; i < program.children_count; ++i) {
		// `set -n`: read and parse, execute nothing. POSIX says the option is
		// ignored by an INTERACTIVE shell, or a typo would end the session.
		// Checked per command rather than once, so `set -n` partway through a
		// script stops the rest of it as well as a `sh -n` invocation stopping
		// everything.
		if (_state.opts().no_exec && !_state.interactive())
			break;
		status = run_node(t, t.child_of(program, i));
		_state.set_last_status(status);
		run_pending_traps();
		if (_exit_requested)
			break;
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
	while (_state.signals().take_pending(signo)) {
		if (_state.signals().disposition_of(signo) != disposition::handler)
			continue;
		const std::string command{_state.signals().trap_command(signo)};
		if (command.empty())
			continue;
		// The status around a trap is preserved: a trap must not clobber `$?` for
		// the command that follows it.
		const int saved = _state.last_status();
		_in_trap = true;
		(void)run_source(command);
		_in_trap = false;
		_state.set_last_status(saved);
	}
}

// Runs the EXIT trap, once, on the way out.
void tree_walking_executor::run_exit_trap() {
	if (_exit_trap_ran)
		return;
	_exit_trap_ran = true;
	if (_state.signals().disposition_of(kExitTrap) != disposition::handler)
		return;
	const std::string command{_state.signals().trap_command(kExitTrap)};
	if (!command.empty())
		(void)run_source(command);
}

int tree_walking_executor::run_node(const tree& t, node_index n) {
	// Cleared on the way IN so that whatever runs deepest decides. Only a
	// short-circuited and-or list and a `!` pipeline set it, on the way out.
	_status_tested = false;
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
			std::fprintf(stderr, "lesh: syntax error near '%.*s'\n",
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
	const node& self = t[n];
	int status = 0;
	for (uint32_t i = 0; i < self.children_count; ++i) {
		status = run_node(t, t.child_of(self, i));
		_state.set_last_status(status);
		run_pending_traps();
		// A break, continue or return unwinds through here rather than being
		// swallowed: the enclosing loop or function is what decides to stop.
		if (_flow != control_flow::normal || _exit_requested)
			break;
		// POSIX: `set -e` EXITS the shell. Merely breaking out of this list left the
		// enclosing loop free to iterate again, so `set -e; while true; do false;
		// done` ran forever.
		if (errexit_fires(status)) {
			_exit_requested = true;
			break;
		}
	}
	return status;
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
		if (--_flow_level <= 0) {
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
	const std::string_view var = t.text_of_token(t.token_at(name_token));

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
	}

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
	expander ex{_pool, _state, &_runner, false, &_state, &_state};
	arena_array<std::string_view> subject{_pool, 2};
	ex.expand_word(t, t.child_of(self, 0), subject);
	const std::string_view text = subject.empty() ? std::string_view{} : subject[0];

	for (uint32_t i = 1; i < self.children_count; ++i) {
		const node& item = t[t.child_of(self, i)];
		for (uint32_t p = 0; p < item.aux; ++p) {
			arena_array<std::string_view> pattern{_pool, 2};
			ex.expand_word(t, t.child_of(item, p), pattern);
			if (pattern.empty())
				continue;
			// The shared matcher from #23. period_is_special is false here: the
			// filename rule does not apply to `case`.
			if (pattern_match(pattern[0], text, /*period_is_special=*/false)) {
				if (item.children_count > item.aux)
					return run_node(t, t.child_of(item, item.aux));
				return 0;
			}
		}
	}
	return 0;  // POSIX: no matching pattern is status zero
}

// Parses and runs source in THIS environment. `eval` and `.` both need it, and
// both must see the shell's own state rather than a copy - which is what makes
// `. ./config` able to set variables the caller then reads.
int tree_walking_executor::run_source(std::string_view source) {
	// A nested pool: the trees live only as long as this call. Functions defined
	// here would not outlive it, which is the same limitation #25 recorded. A deque
	// of them rather than one, because POSIX says `eval` and `.` READ their
	// argument as shell input - and a shell reads one command at a time, so
	// `eval 'alias e=echo<newline>e hi'` prints hi in dash. The trees must all
	// stand while any of them runs: a function defined by one is a node in it.
	buffer_pool nested{BUFFER_POOL_SIZE};
	std::deque<tree> trees;
	int status = _state.last_status();
	size_t at = 0;
	while (at < source.size()) {
		trees.push_back(syntax::parse_next_command(nested, source, at, &_state));
		const tree& parsed = trees.back();
		if (parsed.has_errors() && !_state.interactive()) {
			// POSIX: a syntax error in `eval` or `.` kills a non-interactive shell,
			// exactly as one at the top level does. Returning a status and carrying on
			// let `eval fi; echo not reached` reach the echo.
			report_syntax_error(parsed);
			_exit_requested = true;
			return 2;
		}
		if (parsed.root() == syntax::no_node)
			continue;
		const node& program = parsed[parsed.root()];
		for (uint32_t i = 0; i < program.children_count; ++i) {
			status = run_node(parsed, parsed.child_of(program, i));
			_state.set_last_status(status);
			if (_flow != control_flow::normal || _exit_requested)
				return status;
		}
	}
	return status;
}

int tree_walking_executor::run_file(std::string_view path) {
	std::string name{path};
	std::FILE* f = std::fopen(name.c_str(), "rb");
	if (f == nullptr) {
		std::fprintf(stderr, "lesh: %s: %s\n", name.c_str(), std::strerror(errno));
		return 127;
	}
	std::string source;
	char buffer[4096];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
		source.append(buffer, got);
	std::fclose(f);
	return run_source(source);
}

int tree_walking_executor::run_function_definition(const tree& t, node_index n) {
	const node& self = t[n];
	if (self.children_count == 0)
		return 0;
	const token& name_token = t.token_at(self.aux);
	const std::string name{t.text_of_token(name_token)};
	// A redefinition replaces the previous body, which is what POSIX requires and
	// what makes reloading an rc file work.
	_functions[name] = {&t, t.child_of(self, 0)};
	return 0;
}

bool tree_walking_executor::try_run_function(const tree&, arena_array<char*>& argv,
                                             int& status) {
	const auto it = _functions.find(argv[0]);
	if (it == _functions.end())
		return false;

	if (_function_depth >= kMaxFunctionDepth) {
		std::fprintf(stderr, "lesh: %s: recursion too deep\n", argv[0]);
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
	status = run_node(*it->second.tree, it->second.body);
	--_function_depth;

	// `return` unwinds to here and no further. The control_flow machinery has been
	// wired since #24 with nothing to unwind; this is what it was waiting for.
	if (_flow == control_flow::return_from)
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
		std::fprintf(stderr, "lesh: fork: %s\n", std::strerror(errno));
		return 1;
	}
	if (pid == 0) {
		setpgid(0, 0);
		_state.signals().reset_for_subshell();
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
		const int status = run_node(t, t.child_of(t[n], 0));
		run_exit_trap();
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
		_state.signals().reset_for_subshell();
		// The subshell gets its own EXIT trap, and the flag must be cleared for it:
		// a subshell forked from INSIDE the parent's EXIT trap inherits a raised
		// flag and would skip its own. `trap '(trap "echo x" EXIT)' EXIT` is exactly
		// that shape.
		_exit_trap_ran = false;
		const int status = t[n].children_count > 0
		                   ? run_node(t, t.child_of(t[n], 0))
		                   : 0;
		run_exit_trap();
		std::fflush(nullptr);
		_exit(status);
	}
	setpgid(pid, pid);
	int wait_status = 0;
	waitpid(pid, &wait_status, 0);
	// A subshell is a command in its own right: `set -e; (false && echo a)` exits
	// even though the same list would not inside a brace group.
	_status_tested = false;
	return status_from_wait(wait_status);
}

bool tree_walking_executor::build_argv(const tree& t, node_index n,
                                       arena_array<char*>& argv,
                                       arena_array<std::string_view>* assignments) {
	const node& self = t[n];

	// The runner is passed here, so a command substitution inside a word reaches
	// this same executor. That is what makes `echo $(echo $(echo x))` work.
	expander ex = make_expander();
	arena_array<std::string_view> fields{_pool, 8};

	for (uint32_t i = 0; i < self.children_count; ++i) {
		const node_index child = t.child_of(self, i);
		if (t[child].kind == node_kind::assignment) {
			if (assignments != nullptr)
				assignments->push(t.text_of(t[child]));
			continue;
		}
		if (t[child].kind != node_kind::word)
			continue;
		(void)ex.expand_word(t, child, fields);
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
                                   const tree* t, node_index command) {
	// Built-ins run in this process, so their output sits in our stdout buffer.
	// Flush before forking or the child inherits a copy and prints it again.
	std::fflush(nullptr);

	const pid_t pid = fork();
	if (pid == -1) {
		std::fprintf(stderr, "lesh: fork: %s\n", std::strerror(errno));
		return -1;
	}

	if (pid == 0) {
		// setpgid in the child AND in the parent below: whichever runs first wins,
		// and neither ordering can leave a child outside a group. This is the
		// documented way to avoid the race.
		setpgid(0, ctx.group);

		if (ctx.input_fd != STDIN_FILENO) {
			dup2(ctx.input_fd, STDIN_FILENO);
			close(ctx.input_fd);
		}
		if (ctx.output_fd != STDOUT_FILENO) {
			dup2(ctx.output_fd, STDOUT_FILENO);
			close(ctx.output_fd);
		}

		// Redirections are applied AFTER the pipeline's fds, so an explicit
		// `> file` on a pipeline stage overrides the pipe - which is what POSIX
		// requires and what `a | b > out` means.
		if (t != nullptr && !apply_redirections(*t, command, nullptr))
			_exit(kRedirectionError);

		// `x=1 cmd` exports x to cmd only. Applying it in the CHILD is what keeps it
		// out of the shell - the parent's state is untouched by construction rather
		// than by remembering to undo it.
		if (assignments != nullptr) {
			expander child_ex{_pool, _state, nullptr, true, &_state, &_state};
			for (const auto& a : *assignments) {
				const size_t eq = a.find('=');
				// A readonly name was refused before the fork - see run_simple_command -
				// so the refusal cannot happen here, and a child could not exit the
				// shell over it anyway.
				if (eq != std::string_view::npos)
					std::ignore = _state.set_exported(
						a.substr(0, eq),
						child_ex.expand_value(a.substr(eq + 1), value_context::assignment));
			}
		}

		std::string_view path_value;
		if (!_state.lookup("PATH", path_value))
			path_value = "/usr/bin:/bin";
		exec_or_die(argv.data(), _state.environment_block(), path_value,
		            _state.own_path());
	}

	setpgid(pid, ctx.group == 0 ? pid : ctx.group);
	return pid;
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
	const std::string_view name = text.substr(0, eq);
	char* joined = nullptr;
	_pool.allocate(name.size() + 1 + value.size(), joined, 1);
	std::memcpy(joined, name.data(), name.size());
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
		for (const auto& a : assignments)
			if (!apply_assignment(a))
				return assignment_error();
		return 0;
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
	const int failure = search_and_exec(argv.data(), _state.environment_block(),
	                                   path_value, _state.own_path());

	// dash's shape, minus the line number lesh does not track:
	// `dash: 1: exec: ./_no_such_command_: not found`. exec-p.tst only requires
	// stderr to be non-empty (test_O -d), so this is not compared anywhere - but a
	// message that names the builtin is what makes the failure findable, and
	// `not found` rather than strerror's `No such file or directory` is what every
	// other shell prints for a command search that came up empty.
	std::fprintf(stderr, "lesh: exec: %s: %s\n", argv[0],
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

// `unset -f name...` removes functions. It lives here rather than in builtins.cpp
// for the same reason `eval` does: the function table is the executor's, and
// try_run_builtin is handed nothing but shell state.
//
// POSIX: unsetting a name that is not a function is NOT an error, which is why
// there is no diagnostic and no status but zero.
builtin_result tree_walking_executor::run_unset_functions(char** argv) {
	for (size_t i = 1; argv[i] != nullptr; ++i) {
		const std::string_view arg{argv[i]};
		if (arg == "--") {
			++i;
			for (; argv[i] != nullptr; ++i)
				_functions.erase(std::string{argv[i]});
			break;
		}
		if (arg.size() >= 2 && arg[0] == '-')
			continue;  // an option word; unset_selects_functions has read them
		_functions.erase(std::string{arg});
	}
	return {0};
}

// The builtins the EXECUTOR implements instead of builtins.cpp, which is what
// `builtin_home::executor` marks in the registry.
//
// One function rather than three blocks inside run_simple_command, because a
// PIPELINE STAGE needs the same four: `echo hi | eval cat` went to
// try_run_builtin, which has no entry for `eval`, and the false return was
// discarded - the stage printed nothing and reported success, which is the same
// defect as the unimplemented `test` in #35. Returns false when the name is none
// of them.
bool tree_walking_executor::try_run_executor_builtin(
		const tree& t, node_index n, arena_array<char*>& argv,
		const arena_array<std::string_view>& assignments, bool bypass_functions,
		int& status) {
	const std::string_view name{argv[0]};

	// `exec` replaces this process, so no table of functions could hold it. Until it
	// landed it was classified with no handler anywhere, so try_run_builtin found
	// nothing and returned success: `exec echo hi; echo notreached` printed only
	// `notreached`. A stub that silently succeeds is worse than an absent builtin -
	// the same mistake `command` made earlier on #31, which cost 19 of 49
	// assertions in command-p.tst.
	if (name == "exec") {
		status = run_exec(t, n, argv, assignments, bypass_functions);
		return true;
	}

	// `wait` needs the executor's record of background jobs, so it lives here
	// alongside eval and . rather than in builtins.cpp.
	if (name == "wait") {
		if (argv[1] == nullptr) {
			// POSIX: with no operands, `wait` waits for ALL known children and its
			// status is ZERO - not the last child's. Reporting the last one made
			// `false & wait` fail, and under `set -e` that would exit the shell.
			for (const pid_t pid : _background) {
				int wait_status = 0;
				(void)waitpid(pid, &wait_status, 0);
			}
			_background.clear();
			status = 0;
			return true;
		}
		// With operands the status is the LAST operand's, and a child killed by a
		// signal reports 128 + the signal number.
		status = 0;
		for (size_t i = 1; argv[i] != nullptr; ++i) {
			const pid_t target = static_cast<pid_t>(std::atoi(argv[i]));
			int wait_status = 0;
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
		return true;
	}

	// `eval` and `.` re-enter the FRONT END from inside execution, so they live
	// here too - giving every builtin a back-reference to the executor to serve
	// two of them would be the wrong trade. This is the cycle the ports in #11
	// were designed to survive: parsing inside execution, with the parse seeing
	// the same aliases and the execution seeing the same state.
	if (name == "eval" || name == ".") {
		arena_array<saved_fd> saved{_pool, 4};
		std::fflush(nullptr);
		const bool ok = apply_redirections(t, n, &saved);
		if (!ok) {
			// `eval` and `.` are SPECIAL builtins, and POSIX 2.8.1 makes a
			// redirection error on a special builtin fatal to a non-interactive
			// shell. dash agrees: `dash -c 'eval : </missing; echo reached'` exits 2
			// without printing `reached`, while a regular builtin in the same
			// position reports 2 and carries on. `command eval ...` demotes it, which
			// is what bypass_functions records.
			status = kRedirectionError;
			if (!bypass_functions && !_state.interactive())
				_exit_requested = true;
		} else if (name == "eval") {
			// POSIX: the arguments are joined with spaces and read as shell input.
			std::string joined;
			for (size_t i = 1; argv[i] != nullptr; ++i) {
				if (i > 1)
					joined += ' ';
				joined += argv[i];
			}
			status = joined.empty() ? 0 : run_source(joined);
		} else {
			if (argv[1] == nullptr) {
				std::fprintf(stderr, "lesh: .: filename argument required\n");
				status = 2;
			} else {
				status = run_file(argv[1]);
			}
		}
		std::fflush(nullptr);
		restore_fds(saved);
		return true;
	}

	return false;
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
	// the prefix here and setting a flag is simpler than threading a mode through
	// the search, and it is the executor that owns the search order anyway.
	//
	// The `-v` reporting form stays in builtins.cpp. Implementing only that form
	// and letting every other use silently succeed made `command echo hi` do
	// nothing - a stub that succeeds is worse than an absent builtin, which is
	// the mistake #24 explicitly warned about and I made anyway.
	bool bypass_functions = false;
	while (argv.size() > 1 && std::string_view{argv[0]} == "command" &&
	       argv[1] != nullptr && std::string_view{argv[1]} != "-v") {
		bypass_functions = true;
		// Drop argv[0] by shifting the rest down; the arena owns the strings.
		for (size_t i = 0; i + 1 < argv.size(); ++i)
			argv[i] = argv[i + 1];
		if (argv.size() > 0)
			argv.truncate(argv.size() - 1);
		if (argv.empty() || argv[0] == nullptr)
			return 0;
	}

	// A readonly name in an assignment PREFIX is a variable assignment error, and
	// POSIX makes it fatal BEFORE the command runs: `readonly a=1; a=2 echo prefix`
	// prints nothing at all in dash. Asked here, ahead of every dispatch path,
	// because each applies the prefix somewhere else - a builtin's is applied in
	// this process, an external command's in the CHILD, where a refusal could no
	// longer exit the shell, and a regular builtin's not at all yet.
	for (const auto& a : assignments) {
		const size_t eq = a.find('=');
		if (eq != std::string_view::npos && _state.is_readonly(a.substr(0, eq))) {
			shell_state::report_readonly({}, a.substr(0, eq));
			return assignment_error();
		}
	}

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
	if (!bypass_functions && classify_builtin(argv[0]) != builtin_kind::special &&
	    _functions.find(argv[0]) != _functions.end()) {
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
		if (ok) {
			restore_vars.reserve(assignments.size());
			for (const auto& a : assignments) {
				const size_t eq = a.find('=');
				if (eq == std::string_view::npos)
					continue;
				saved_variable sv;
				sv.name.assign(a.substr(0, eq));
				std::string_view previous;
				sv.was_set = _state.lookup(sv.name, previous);
				if (sv.was_set)
					sv.value.assign(previous);
				restore_vars.push_back(std::move(sv));
				if (!apply_assignment(a)) {
					refused = true;
					break;
				}
			}
		}
		bool ran = false;
		if (ok && !refused)
			ran = try_run_function(t, argv, status);
		for (const auto& sv : restore_vars) {
			// A name the function made readonly cannot be put back, and POSIX says a
			// readonly variable stays readonly - so the refusal is the correct
			// outcome here rather than an error to report twice.
			if (sv.was_set)
				std::ignore = _state.set(sv.name, sv.value);
			else
				std::ignore = _state.unset(sv.name);
		}
		std::fflush(nullptr);
		restore_fds(saved);
		if (!ok)
			return kRedirectionError;
		if (refused)
			return assignment_error();
		if (ran)
			return status;
	}

	// `eval`, `.`, `exec` and `wait` live in the executor rather than in
	// builtins.cpp; see try_run_executor_builtin.
	{
		int status = 0;
		if (try_run_executor_builtin(t, n, argv, assignments, bypass_functions, status))
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
		const bool persist = classify_builtin(argv[0]) == builtin_kind::special;
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
		if (ok) {
			if (!persist)
				restore_vars.reserve(assignments.size());
			for (const auto& a : assignments) {
				const size_t eq = a.find('=');
				if (eq == std::string_view::npos)
					continue;
				if (!persist) {
					saved_variable sv;
					sv.name.assign(a.substr(0, eq));
					std::string_view previous;
					sv.was_set = _state.lookup(sv.name, previous);
					if (sv.was_set)
						sv.value.assign(previous);
					restore_vars.push_back(std::move(sv));
				}
				if (!apply_assignment(a)) {
					refused = true;
					break;
				}
			}
		}
		if (ok && !refused) {
			// `unset -f` removes a FUNCTION, and the function table lives here rather
			// than in shell state. Only this FORM is intercepted - the variable form
			// is builtins.cpp's - and it is done inside this block so the
			// redirections around it are still applied and restored.
			if (std::string_view{argv[0]} == "unset" &&
			    unset_selects_functions(argv.data())) {
				result = run_unset_functions(argv.data());
			} else if (!try_run_builtin(_state, argv.data(), result)) {
				// A CLASSIFIED name with no implementation. The registry guard in
				// builtins.cpp makes this a compile error, and this branch is what
				// happens if the guard is ever removed: 127 and a diagnostic rather
				// than the silent success that made `test 1 = 2` report 0 (#35). The
				// return value used to be discarded with a `(void)`, which is what let
				// that through.
				std::fprintf(stderr, "lesh: %s: not implemented\n", argv[0]);
				result.status = 127;
			}
		} else if (!ok) {
			result.status = kRedirectionError;
			// POSIX 2.8.1: a redirection error on a SPECIAL builtin exits a
			// non-interactive shell; on a regular one it does not. Verified against
			// dash, which exits 2 for `: </missing` and reports 2 and continues for
			// `echo x </missing`. `command : </missing` is demoted to regular and
			// survives, which is what bypass_functions records.
			if (!bypass_functions && !_state.interactive() &&
			    classify_builtin(argv[0]) == builtin_kind::special)
				_exit_requested = true;
		}
		std::fflush(nullptr);
		if (ok && drop_unwritable_output(argv[0]))
			result.status = 1;
		restore_fds(saved);
		for (const auto& sv : restore_vars) {
			// A name the builtin itself made readonly cannot be put back, and POSIX
			// says a readonly variable stays readonly - so the refusal is the correct
			// outcome here rather than an error to report twice.
			if (sv.was_set)
				std::ignore = _state.set(sv.name, sv.value);
			else
				std::ignore = _state.unset(sv.name);
		}
		if (refused)
			result.status = assignment_error();

		if (result.flow == control_flow::exit_shell) {
			_exit_requested = true;
		} else if (result.flow != control_flow::normal) {
			// break, continue and return unwind through the enclosing construct.
			_flow = result.flow;
			_flow_level = result.level;
		}
		return result.status;
	}

	const pid_t pid = spawn(argv, {}, &assignments, &t, n);
	if (pid == -1)
		return 1;

	int wait_status = 0;
	waitpid(pid, &wait_status, 0);
	return status_from_wait(wait_status);
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
			std::fprintf(stderr, "lesh: pipe: %s\n", std::strerror(errno));
			break;
		}

		const node_index stage = t.child_of(self, i);

		// A stage may be a COMPOUND command - `echo x | { read v; ... }` and
		// `... | while read l; do ...; done` are both ordinary shell. build_argv
		// only understands simple commands, so a compound stage found no words and
		// was silently skipped, producing no output at all.
		if (t[stage].kind != node_kind::simple_command) {
			std::fflush(nullptr);
			const pid_t pid = fork();
			if (pid == 0) {
				setpgid(0, group);
				if (input_fd != STDIN_FILENO) { dup2(input_fd, STDIN_FILENO); close(input_fd); }
				if (!is_last) { dup2(pipe_fds[1], STDOUT_FILENO); close(pipe_fds[1]); }
				const int status = run_node(t, stage);
				std::fflush(nullptr);
				_exit(status);
			}
			if (pid > 0) {
				setpgid(pid, group == 0 ? pid : group);
				pids.push(pid);
				if (group == 0)
					group = pid;
			}
			if (input_fd != STDIN_FILENO) close(input_fd);
			if (!is_last) { close(pipe_fds[1]); input_fd = pipe_fds[0]; }
			continue;
		}

		arena_array<char*> argv{_pool, 8};
		arena_array<std::string_view> assignments{_pool, 4};
		if (build_argv(t, stage, argv, &assignments)) {
			// A function or builtin in a pipeline stage runs in ITS OWN process, so
			// it forks like anything else and its effects do not reach the shell.
			// POSIX allows either, and running it in a subshell is what dash does -
			// which is why `f | cat` cannot set a variable in the parent.
			const bool in_process = _functions.contains(argv[0]) ||
			                        classify_builtin(argv[0]) != builtin_kind::none;
			if (in_process) {
				std::fflush(nullptr);
				const pid_t pid = fork();
				if (pid == 0) {
					setpgid(0, group);
					if (input_fd != STDIN_FILENO) { dup2(input_fd, STDIN_FILENO); close(input_fd); }
					if (!is_last) { dup2(pipe_fds[1], STDOUT_FILENO); close(pipe_fds[1]); }
					// The executor's own builtins are dispatched here rather than through
					// try_run_builtin, which has no entry for any of them: `exec echo foo |
					// cat` printed nothing, and so did `echo hi | eval cat`. They come
					// before the apply_redirections below because each applies the stage's
					// redirections itself where it needs them.
					int status = 0;
					if (try_run_executor_builtin(t, stage, argv, assignments, false, status)) {
						std::fflush(nullptr);
						_exit(status);
					}
					// nullptr for `restore`: this process exists only for this stage, so
					// there is nothing to put the fds back for. Its status matters
					// though - a failed redirection has to make the STAGE fail, and
					// discarding the result ran the builtin anyway, on the unredirected
					// fds.
					if (!apply_redirections(t, stage, nullptr)) {
						std::fflush(nullptr);
						_exit(kRedirectionError);
					}
					if (!try_run_function(t, argv, status)) {
						builtin_result r{};
						(void)try_run_builtin(_state, argv.data(), r);
						status = r.status;
					}
					std::fflush(nullptr);
					_exit(status);
				}
				if (pid > 0) {
					setpgid(pid, group == 0 ? pid : group);
					pids.push(pid);
					if (group == 0)
						group = pid;
				}
				if (input_fd != STDIN_FILENO) close(input_fd);
				if (!is_last) { close(pipe_fds[1]); input_fd = pipe_fds[0]; }
				continue;
			}

			// Every stage runs in its own process, so a builtin in a pipeline stage
			// affects only that process - which is why dispatch here would be wrong
			// and `echo a | read x` cannot set x in the shell. That is POSIX's
			// behaviour, not a limitation.
			const pid_t pid = spawn(argv, {input_fd, is_last ? STDOUT_FILENO : pipe_fds[1], group},
			                        &assignments, &t, stage);
			if (pid > 0) {
				pids.push(pid);
				if (group == 0)
					group = pid;
			}
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
	int last_status = 0;
	int rightmost_failure = 0;
	for (size_t i = 0; i < pids.size(); ++i) {
		int wait_status = 0;
		waitpid(pids[i], &wait_status, 0);
		const int status = status_from_wait(wait_status);
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

bool tree_walking_executor::capture(std::string_view code, arena_array<char>& out) {
	int pipe_fds[2];
	if (pipe(pipe_fds) == -1)
		return false;

	std::fflush(nullptr);
	const pid_t pid = fork();
	if (pid == -1) {
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		return false;
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
		_state.signals().reset_for_subshell();
		_exit_trap_ran = false;
		// The substitution's contents are a fresh parse. Nesting works because this
		// is the same code path, reached recursively.
		buffer_pool inner_pool{BUFFER_POOL_SIZE};
		const tree inner = syntax::parse(inner_pool, code, &_state);
		const int status = run(inner);
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
	waitpid(pid, &wait_status, 0);
	_state.set_last_status(status_from_wait(wait_status));
	++_substitutions;
	return true;
}

} // namespace lesh::runtime
