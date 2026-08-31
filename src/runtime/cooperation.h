#pragma once

#include <sys/types.h>

namespace lesh::runtime {

// WHAT THE RUNTIME WANTS OF WHATEVER IS HOSTING IT (#199, #145; ADR-0011).
//
// The name is the whole point of this file. It says "cooperation" and never
// "fibers", because the runtime must not learn what is on the other side: today
// it is a scheduler slicing fibers inside `event_loop`, tomorrow it could be
// something else again.
//
// AN ABSTRACT CLASS, NOT A TEMPLATE. Whether a shell is interactive is a RUNTIME
// fact, decided in `main()` from the invocation and the terminal, and both kinds
// of shell run the same `tree_walking_executor` built from the same translation
// unit. A template parameter would have to be chosen where the type is not yet
// known, and the only way to make that work is a branch at every boundary - so
// the branch is paid once, as an indirect call, instead. The measured price is in
// tools/bench.cpp: the loop-iteration number does not move.
//
// NEVER NULL. `shell_state` starts with `&noop_cooperation()` and every setter
// takes a reference, so there is not one null check in the runtime and no site
// can forget one. The two consoles beside it (`set_binding_console`,
// `set_prompt_console`) are null in a non-interactive shell BECAUSE a null there
// is an answer - `bind` says "no line editor" rather than pretending. Here there
// is no such answer to give: a command boundary happens in every shell, and
// "nobody is waiting for it" is a behaviour, not an absence. So it is an object
// with an empty body.
//
// EVERY VERB IS `noexcept`, because every one of them is called from inside the
// executor's own control flow, which expects to return a status rather than to
// unwind.
class cooperation {
public:
	virtual ~cooperation() = default;

	// THE COMMAND BOUNDARY, and a no-op for every non-interactive shell.
	//
	// Called once per command, beside each of `run_pending_traps()`'s two call
	// sites and always AFTER it: the command loop in `run_command_list`, and
	// `interrupt_at_prompt`, where a cancelled line is a boundary with no command
	// in it. After the traps rather than before, because a trap body is itself
	// commands and what the host is told about is the state they left behind - and
	// in `interrupt_at_prompt` last rather than adjacent, for the same reason:
	// what settles `$?` there runs after the traps do.
	virtual void on_command_boundary() noexcept = 0;

	// THE FOREGROUND WAIT (#208).
	//
	// `::waitpid(pid, status, flags)`, said as a question rather than as a syscall:
	// "I have nothing to do until this child does something; whoever is hosting me
	// may use the thread until then". The executor has exactly one wait,
	// `tree_walking_executor::reap`, and every `waitpid` site goes through it.
	//
	// THE NO-OP IMPLEMENTATION *IS* `::waitpid`, which is why the verb moves no
	// behaviour: `lesh -c`, a script, a unit test and a forked child reach the
	// identical syscall with the identical arguments through one indirect call.
	// Only an interactive host answers differently, and even it only when there is
	// a fiber to park - see `ui::event_loop::await_child`.
	//
	// THE ARGUMENTS ARE `waitpid`'s, with the flags beside the pid, because at
	// every call site the flags are a property of the WAIT (`WUNTRACED` where
	// Ctrl-Z can reach it, nothing where it cannot) and the status is the
	// out-parameter. The answer is `waitpid`'s: the pid, or -1 for no such child.
	virtual pid_t wait_child(pid_t pid, int flags, int* status) noexcept = 0;

	// THE INPUT WAIT (#209).
	//
	// "I am about to block in `::read(fd, ...)`; whoever is hosting me may use the
	// thread until there is something there." Said BEFORE the read and never
	// INSTEAD of it: this verb moves no bytes, reports no error and answers
	// nothing. The read that follows is the same read it always was, which is what
	// keeps every byte of `read`'s POSIX behaviour - the escapes, the field
	// splitting, "no more of the input than the line it needs" - on one side of
	// the seam and out of the host's reach.
	//
	// THE NO-OP IMPLEMENTATION IS EMPTY, so a script, `lesh -c`, a unit test and
	// every forked child return from here immediately and block in the read exactly
	// as they always did. Only an interactive host answers differently, and even it
	// only when there is a fiber to park - see `ui::event_loop::await_readable`.
	//
	// WHAT "READABLE" HAS TO MEAN for the read after it not to block: at least one
	// byte, or end of file. A regular file is always readable, so `read x < file`
	// returns from here at once; a pipe or a tty reports POLLIN for a byte and
	// POLLHUP for a writer that has gone, and the one-byte read that follows
	// either answer completes without waiting. A descriptor the host cannot ask
	// about at all - `read x 0<&-`, whose fd 0 is closed - is treated as READY
	// rather than waited on, because the read is then the thing with the right
	// answer (EBADF, which `read` reports as end of input) and a wait would be for
	// ever.
	//
	// AND NOTHING IS PROMISED ABOUT SIGNALS. A signal arriving while the caller
	// waits here does not end the wait - exactly as `SA_RESTART` means it does not
	// end the blocking read the no-op takes. The host may well WAKE for it; what
	// it must not do is return to a caller whose read would then block.
	virtual void await_readable(int fd) noexcept = 0;
};

// The non-interactive answer, and the default every `shell_state` starts with.
// One static instance with an empty body, so `lesh -c`, a script, a unit test and
// a forked child all cooperate with nobody at the cost of one indirect call to a
// `return`.
//
// A function rather than an extern object so the instance stays private to
// cooperation.cpp: nothing outside it can name the type, which is what keeps
// "the no-op" from acquiring behaviour later.
[[nodiscard]] cooperation& noop_cooperation() noexcept;

} // namespace lesh::runtime
