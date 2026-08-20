#include "runtime/executor.h"

#include "runtime/builtins.h"
#include "substrate/assert.h"
#include "syntax/parser.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/wait.h>

namespace lesh::runtime {

using syntax::node;
using syntax::node_index;
using syntax::node_kind;
using syntax::tree;

namespace {

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
[[noreturn]] void exec_or_die(char** argv, char** env, std::string_view path_value) {
	if (std::strchr(argv[0], '/') != nullptr) {
		execve(argv[0], argv, env);
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
			// Only ENOENT means "keep looking"; anything else is the real answer.
			if (errno != ENOENT)
				last_errno = errno;
			if (colon == std::string_view::npos)
				break;
			at = colon + 1;
		}
		errno = last_errno;
	}
	std::fprintf(stderr, "lesh: %s: %s\n", argv[0], std::strerror(errno));
	// POSIX: 127 when the command was not found, 126 when it was found but could
	// not be executed. _exit, not exit: never flush buffers inherited from the
	// parent.
	_exit(errno == ENOENT ? 127 : 126);
}

} // namespace

int tree_walking_executor::run(const tree& t) {
	if (t.root() == syntax::no_node)
		return _state.last_status();

	const node& program = t[t.root()];
	int status = _state.last_status();
	for (uint32_t i = 0; i < program.children_count; ++i) {
		status = run_node(t, t.child_of(program, i));
		_state.set_last_status(status);
		if (_exit_requested)
			break;
		if (status != 0 && _state.opts().exit_on_error)
			break;
	}
	return status;
}

int tree_walking_executor::run_node(const tree& t, node_index n) {
	switch (t[n].kind) {
		case node_kind::simple_command: return run_simple_command(t, n);
		case node_kind::pipeline:       return run_pipeline(t, n);
		case node_kind::and_or:         return run_and_or(t, n);
		case node_kind::error:
			std::fprintf(stderr, "lesh: syntax error near '%.*s'\n",
			             static_cast<int>(t.text_of(t[n]).size()), t.text_of(t[n]).data());
			return 2;
		default:
			return 0;
	}
}

int tree_walking_executor::run_and_or(const tree& t, node_index n) {
	const node& self = t[n];
	LESH_ASSERT(self.children_count == 2);

	const int left = run_node(t, t.child_of(self, 0));
	_state.set_last_status(left);

	const bool is_and = t.token_at(self.aux).kind == syntax::token_kind::and_if;
	if (is_and ? (left != 0) : (left == 0))
		return left;  // short-circuit

	return run_node(t, t.child_of(self, 1));
}

bool tree_walking_executor::build_argv(const tree& t, node_index n,
                                       arena_array<char*>& argv,
                                       arena_array<std::string_view>* assignments) {
	const node& self = t[n];

	// The runner is passed here, so a command substitution inside a word reaches
	// this same executor. That is what makes `echo $(echo $(echo x))` work.
	expander ex{_pool, _state, &_runner};
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
		ex.expand_word(t, child, fields);
	}

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
                                   const arena_array<std::string_view>* assignments) {
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

		// `x=1 cmd` exports x to cmd only. Applying it in the CHILD is what keeps it
		// out of the shell - the parent's state is untouched by construction rather
		// than by remembering to undo it.
		if (assignments != nullptr) {
			for (const auto& a : *assignments) {
				const size_t eq = a.find('=');
				if (eq != std::string_view::npos)
					_state.set_exported(a.substr(0, eq), a.substr(eq + 1));
			}
		}

		std::string_view path_value;
		if (!_state.lookup("PATH", path_value))
			path_value = "/usr/bin:/bin";
		exec_or_die(argv.data(), _state.environment_block(), path_value);
	}

	setpgid(pid, ctx.group == 0 ? pid : ctx.group);
	return pid;
}

// Splits NAME=value and applies it. Returns the name so the caller can restore.
void tree_walking_executor::apply_assignment(std::string_view text) {
	const size_t eq = text.find('=');
	if (eq == std::string_view::npos)
		return;
	_state.set(text.substr(0, eq), text.substr(eq + 1));
}

int tree_walking_executor::run_simple_command(const tree& t, node_index n) {
	arena_array<char*> argv{_pool, 8};
	arena_array<std::string_view> assignments{_pool, 4};
	const bool has_command = build_argv(t, n, argv, &assignments);

	if (!has_command) {
		// Assignments with no command persist in the shell: `x=1` is how a variable
		// gets set. Previously these were parsed and silently dropped.
		for (const auto& a : assignments)
			apply_assignment(a);
		return 0;
	}

	// A builtin runs in THIS process - `cd` in a forked child would change the
	// child's directory and exit, achieving nothing. So dispatch happens before
	// the fork, not after.
	if (builtin_result result; try_run_builtin(_state, argv.data(), result)) {
		// POSIX: assignments preceding a SPECIAL builtin persist afterwards; before
		// a regular one they apply only for its duration. Not cosmetic - it is why
		// `x=1 export y` leaves x set and `x=1 cd /tmp` does not.
		const bool persist =
			classify_builtin(argv[0]) == builtin_kind::special;
		if (persist) {
			for (const auto& a : assignments)
				apply_assignment(a);
		}
		// A regular builtin's temporary assignments are not implemented yet: they
		// need save-and-restore around the call, and no builtin here reads a
		// variable set that way. Recorded rather than pretended.

		if (result.flow == control_flow::exit_shell)
			_exit_requested = true;
		return result.status;
	}

	const pid_t pid = spawn(argv, {}, &assignments);
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

		arena_array<char*> argv{_pool, 8};
		arena_array<std::string_view> assignments{_pool, 4};
		if (build_argv(t, t.child_of(self, i), argv, &assignments)) {
			// Every stage runs in its own process, so a builtin in a pipeline stage
			// affects only that process - which is why dispatch here would be wrong
			// and `echo a | read x` cannot set x in the shell. That is POSIX's
			// behaviour, not a limitation.
			const pid_t pid = spawn(argv, {input_fd, is_last ? STDOUT_FILENO : pipe_fds[1], group},
			                        &assignments);
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

	// Every member must be waited on or it becomes a zombie, but POSIX defines the
	// pipeline's status as its LAST command's.
	int last_status = 0;
	for (size_t i = 0; i < pids.size(); ++i) {
		int wait_status = 0;
		waitpid(pids[i], &wait_status, 0);
		if (i + 1 == pids.size())
			last_status = status_from_wait(wait_status);
	}
	return last_status;
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

		// The substitution's contents are a fresh parse. Nesting works because this
		// is the same code path, reached recursively.
		buffer_pool inner_pool{BUFFER_POOL_SIZE};
		const tree inner = syntax::parse(inner_pool, code);
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
	return true;
}

} // namespace lesh::runtime
