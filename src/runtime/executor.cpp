#include "runtime/executor.h"

#include "substrate/assert.h"

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

} // namespace

int tree_walking_executor::run(const tree& t) {
	if (t.root() == syntax::no_node)
		return _state.last_status();

	const node& program = t[t.root()];
	int status = _state.last_status();
	for (uint32_t i = 0; i < program.children_count; ++i) {
		status = run_node(t, t.child_of(program, i));
		_state.set_last_status(status);

		// set -e: a non-zero status ends the shell. POSIX exempts commands whose
		// status is being tested, which the and_or path handles by not routing
		// through here.
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
			// POSIX: a syntax error in a non-interactive shell exits with a status
			// greater than zero; 2 is the conventional choice.
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

int tree_walking_executor::run_simple_command(const tree& t, node_index n) {
	const node& self = t[n];

	expander ex{_pool, _state, nullptr};
	arena_array<std::string_view> fields{_pool, 8};

	for (uint32_t i = 0; i < self.children_count; ++i) {
		const node_index child = t.child_of(self, i);
		if (t[child].kind != node_kind::word)
			continue;
		ex.expand_word(t, child, fields);
	}

	if (fields.empty())
		return _state.last_status();

	// argv must be NUL-terminated C strings; fields are views into the arena and
	// the source. Copy into an argv block the arena owns.
	arena_array<char*> argv{_pool, fields.size() + 1};
	for (const auto& f : fields) {
		char* buf = nullptr;
		_pool.allocate(f.size() + 1, buf, 1);
		std::memcpy(buf, f.data(), f.size());
		buf[f.size()] = '\0';
		argv.push(buf);
	}
	argv.push(nullptr);

	// Built-ins above run in this process, so their output sits in our stdout
	// buffer. Flush before forking or the child inherits a copy and prints it
	// again when it exits.
	std::fflush(nullptr);

	const pid_t pid = fork();
	if (pid == -1) {
		std::fprintf(stderr, "lesh: fork: %s\n", std::strerror(errno));
		return 1;
	}

	if (pid == 0) {
		// Own process group, so a hung command can be killed as a group and cannot
		// orphan its children to init. See the note on tree_walking_executor.
		setpgid(0, 0);

		// PATH search. execve takes a path, not a command name, and macOS has no
		// execvpe - so either the environment goes through `environ` and we use
		// execvp, or the search is explicit. Explicit, because the child's
		// environment is built from shell state and assigning to environ to smuggle
		// it into execvp is the kind of hidden coupling this design avoids.
		char** child_env = _state.environment_block();
		if (std::strchr(argv[0], '/') != nullptr) {
			execve(argv[0], argv.data(), child_env);
		} else {
			std::string_view path_value;
			if (!_state.lookup("PATH", path_value))
				path_value = "/usr/bin:/bin";
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
				execve(candidate.c_str(), argv.data(), child_env);
				// Only ENOENT means "keep looking"; anything else is the real answer.
				if (errno != ENOENT)
					last_errno = errno;
				if (colon == std::string_view::npos)
					break;
				at = colon + 1;
			}
			errno = last_errno;
		}
		// POSIX: 127 when the command was not found, 126 when it was found but
		// could not be executed.
		std::fprintf(stderr, "lesh: %s: %s\n", argv[0], std::strerror(errno));
		// _exit, not exit: never flush stdio buffers inherited from the parent.
		_exit(errno == ENOENT ? 127 : 126);
	}

	// Set it in the parent too: whichever runs first wins, and neither ordering
	// can leave the child outside a group.
	setpgid(pid, pid);

	int wait_status = 0;
	waitpid(pid, &wait_status, 0);
	return status_from_wait(wait_status);
}

int tree_walking_executor::run_pipeline(const tree& t, node_index n) {
	const node& self = t[n];
	LESH_ASSERT(self.children_count >= 2);

	// Not implemented yet: the pipeline path needs fd plumbing plus a shared
	// process group for the whole pipeline, which is the job-control-shaped part
	// of #12. Reported rather than silently wrong.
	std::fprintf(stderr, "lesh: pipelines are not implemented in the new executor yet\n");
	return 1;
}

namespace {

// Adapts the executor to the expander's port. Defined here rather than in the
// header so the expander never sees the executor's type.
class executor_runner final : public command_runner {
public:
	bool run_and_capture(std::string_view, arena_array<char>&) override {
		// Command substitution needs a captured subshell, which needs the pipeline
		// plumbing above. Refusing is correct until then: the alternative is
		// silently expanding to nothing, which looks like a working shell producing
		// wrong answers.
		return false;
	}
};

executor_runner g_runner;

} // namespace

command_runner& tree_walking_executor::as_command_runner() noexcept { return g_runner; }

} // namespace lesh::runtime
