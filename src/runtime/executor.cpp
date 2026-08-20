#include "runtime/executor.h"

#include "runtime/builtins.h"
#include "runtime/pattern.h"
#include "substrate/assert.h"
#include "syntax/parser.h"

#include <cerrno>
#include <fcntl.h>
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
using syntax::token;
using syntax::token_kind;
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
		t.source().substr(t.token_at(self.last_token).offset,
		                  t.token_at(self.last_token).length);

	// The target is a word and gets expanded: `> $out` has to work. Field
	// splitting must not apply - POSIX makes a redirection target expanding to
	// more than one field an error, and treating it as one word is the behaviour
	// dash has.
	expander ex{_pool, _state, &_runner, !_state.opts().no_glob, &_state, &_state};
	arena_array<std::string_view> fields{_pool, 2};
	{
		// Reparse the target as a standalone word so the expander sees it whole.
		const std::string_view expanded = ex.expand_assignment_value(target_text);
		fields.push(expanded);
	}
	const std::string_view target = fields.empty() ? std::string_view{} : fields[0];

	// NUL-terminate for open(2).
	char* path = nullptr;
	_pool.allocate(target.size() + 1, path, 1);
	std::memcpy(path, target.data(), target.size());
	path[target.size()] = '\0';

	const int fd = self.aux == 0xFFFFFFFFu ? default_fd_for(op_token.kind)
	                                       : static_cast<int>(self.aux);

	int opened = -1;
	switch (op_token.kind) {
		case token_kind::less:
			opened = open(path, O_RDONLY);
			break;
		case token_kind::great:
		case token_kind::clobber:
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
				if (restore != nullptr) {
					const int saved = dup(fd);
					if (saved != -1)
						restore->push({fd, saved});
				}
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
			if (restore != nullptr) {
				const int saved = dup(fd);
				if (saved != -1)
					restore->push({fd, saved});
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
	if (restore != nullptr) {
		const int saved = dup(fd);
		if (saved != -1)
			restore->push({fd, saved});
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
		expander ex{_pool, _state, &_runner, false, &_state, &_state};
		const std::string_view result = ex.expand_assignment_value(text);
		expanded.assign(result);
		text = expanded;
	}

	int pipe_fds[2];
	if (pipe(pipe_fds) == -1) {
		std::fprintf(stderr, "lesh: pipe: %s\n", std::strerror(errno));
		return false;
	}

	if (restore != nullptr) {
		const int saved = dup(STDIN_FILENO);
		if (saved != -1)
			restore->push({STDIN_FILENO, saved});
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

	dup2(pipe_fds[0], STDIN_FILENO);
	close(pipe_fds[0]);
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

void tree_walking_executor::restore_fds(arena_array<saved_fd>& saved) {
	// In reverse, so nested saves of the same fd unwind correctly.
	for (size_t i = saved.size(); i > 0; --i) {
		dup2(saved[i - 1].saved, saved[i - 1].original);
		close(saved[i - 1].saved);
	}
}

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
		case node_kind::compound_list: return run_compound_list(t, n);
		case node_kind::if_clause:     return run_if(t, n);
		case node_kind::while_loop:    return run_loop(t, n, /*until=*/false);
		case node_kind::until_loop:    return run_loop(t, n, /*until=*/true);
		case node_kind::for_loop:      return run_for(t, n);
		case node_kind::case_clause:   return run_case(t, n);
		case node_kind::brace_group:   return run_compound_list(t, t.child_of(t[n], 0));
		case node_kind::subshell:      return run_subshell(t, n);
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

int tree_walking_executor::run_compound_list(const tree& t, node_index n) {
	const node& self = t[n];
	int status = 0;
	for (uint32_t i = 0; i < self.children_count; ++i) {
		status = run_node(t, t.child_of(self, i));
		_state.set_last_status(status);
		// A break, continue or return unwinds through here rather than being
		// swallowed: the enclosing loop or function is what decides to stop.
		if (_flow != control_flow::normal || _exit_requested)
			break;
		if (status != 0 && _state.opts().exit_on_error)
			break;
	}
	return status;
}

int tree_walking_executor::run_if(const tree& t, node_index n) {
	const node& self = t[n];
	const uint32_t pairs = self.aux;

	for (uint32_t i = 0; i < pairs; ++i) {
		const int cond = run_node(t, t.child_of(self, i * 2));
		if (_flow != control_flow::normal || _exit_requested)
			return cond;
		// POSIX: a condition's status is TESTED, so `set -e` must not fire on it.
		// That is why conditions run through run_node directly rather than through
		// the exit_on_error path in run_compound_list's caller.
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
		const int cond = run_node(t, t.child_of(self, 0));
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
	const std::string_view var = t.source().substr(t.token_at(self.aux).offset,
	                                               t.token_at(self.aux).length);

	// Every child but the last is a word to iterate; the last is the body.
	const uint32_t word_count = self.children_count - 1;
	expander ex{_pool, _state, &_runner, !_state.opts().no_glob, &_state, &_state};
	arena_array<std::string_view> items{_pool, 8};
	for (uint32_t i = 0; i < word_count; ++i)
		ex.expand_word(t, t.child_of(self, i), items);

	int status = 0;
	for (const auto& item : items) {
		_state.set(var, item);
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

int tree_walking_executor::run_subshell(const tree& t, node_index n) {
	// A separate execution environment whose changes do not escape. Forking gives
	// that for free: the child mutates its own copy of shell state.
	std::fflush(nullptr);
	const pid_t pid = fork();
	if (pid == -1)
		return 1;
	if (pid == 0) {
		setpgid(0, 0);
		const int status = t[n].children_count > 0
		                   ? run_node(t, t.child_of(t[n], 0))
		                   : 0;
		std::fflush(nullptr);
		_exit(status);
	}
	setpgid(pid, pid);
	int wait_status = 0;
	waitpid(pid, &wait_status, 0);
	return status_from_wait(wait_status);
}

bool tree_walking_executor::build_argv(const tree& t, node_index n,
                                       arena_array<char*>& argv,
                                       arena_array<std::string_view>* assignments) {
	const node& self = t[n];

	// The runner is passed here, so a command substitution inside a word reaches
	// this same executor. That is what makes `echo $(echo $(echo x))` work.
	expander ex{_pool, _state, &_runner, !_state.opts().no_glob, &_state, &_state};
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
			_exit(1);

		// `x=1 cmd` exports x to cmd only. Applying it in the CHILD is what keeps it
		// out of the shell - the parent's state is untouched by construction rather
		// than by remembering to undo it.
		if (assignments != nullptr) {
			expander child_ex{_pool, _state, nullptr, true, &_state, &_state};
			for (const auto& a : *assignments) {
				const size_t eq = a.find('=');
				if (eq != std::string_view::npos)
					_state.set_exported(a.substr(0, eq),
					                    child_ex.expand_assignment_value(a.substr(eq + 1)));
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
	// The value is expanded, not stored raw: `x="a b"` assigns `a b` without the
	// quotes, and `x=$y` assigns y's value. Storing the source text meant a later
	// `echo $x` printed the quotes.
	expander ex{_pool, _state, &_runner, !_state.opts().no_glob, &_state, &_state};
	_state.set(text.substr(0, eq), ex.expand_assignment_value(text.substr(eq + 1)));
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
		const bool ok = apply_redirections(t, n, &saved);
		builtin_result result{};
		if (ok)
			(void)try_run_builtin(_state, argv.data(), result);
		else
			result.status = 1;
		std::fflush(nullptr);
		restore_fds(saved);
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

		arena_array<char*> argv{_pool, 8};
		arena_array<std::string_view> assignments{_pool, 4};
		if (build_argv(t, t.child_of(self, i), argv, &assignments)) {
			// Every stage runs in its own process, so a builtin in a pipeline stage
			// affects only that process - which is why dispatch here would be wrong
			// and `echo a | read x` cannot set x in the shell. That is POSIX's
			// behaviour, not a limitation.
			const pid_t pid = spawn(argv, {input_fd, is_last ? STDOUT_FILENO : pipe_fds[1], group},
			                        &assignments, &t, t.child_of(self, i));
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
