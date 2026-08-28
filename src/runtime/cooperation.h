#pragma once

#include <sys/types.h>

namespace lesh::runtime {

// WHAT THE RUNTIME WANTS OF WHATEVER IS HOSTING IT (#199, step 1a of #145).
//
// The name is the whole point of this file. It says "cooperation" and never
// "fibers", because the runtime must not learn what is on the other side: today
// nothing is, tomorrow it is a scheduler slicing reactor fibers inside
// `event_loop`, and the day after it could be something else again. The executor
// knows one sentence - "a command just finished, whoever is hosting me may run" -
// and that sentence is this interface.
//
// AN ABSTRACT CLASS, NOT A TEMPLATE. Whether a shell is interactive is a RUNTIME
// fact, decided in `main()` from the invocation and the terminal, and both kinds
// of shell run the same `tree_walking_executor` built from the same translation
// unit. A template parameter would have to be chosen where the type is not yet
// known, and the only way to make that work is a branch at every boundary - so
// the branch is paid once, as an indirect call, instead. The measured price is in
// tools/bench.cpp: the loop-iteration number does not move.
//
// NEVER NULL, which is the second decision worth stating. `shell_state` starts
// with `&noop_cooperation()` and every setter takes a reference, so there is not
// one null check in the runtime and no site can forget one. The two consoles
// beside it (`set_binding_console`, `set_prompt_console`) are null in a
// non-interactive shell BECAUSE a null there is an answer - `bind` says "no line
// editor" rather than pretending. Here there is no such answer to give: a command
// boundary happens in every shell, and "nobody is waiting for it" is a behaviour,
// not an absence. So it is an object with an empty body.
class cooperation {
public:
	virtual ~cooperation() = default;

	// THE COMMAND BOUNDARY, and in v1 the only verb.
	//
	// Called once per command, beside each of `run_pending_traps()`'s two call
	// sites and always after it: the command loop in `run_command_list`, and
	// `interrupt_at_prompt`, where a cancelled line is a boundary with no command
	// in it. AFTER the traps rather than before, because a trap body is itself
	// commands and what the host is told about is the state they left behind - and
	// in `interrupt_at_prompt` last rather than adjacent, for the same reason:
	// what settles `$?` there runs after the traps do.
	//
	// `noexcept`, because the boundary is inside the executor's command loop and a
	// host throwing out of it would unwind the shell through code that expects to
	// return a status.
	//
	// v1 WAS A NO-OP FOR EVERYONE and still is for every non-interactive shell.
	// Phase 2 of #145 adds `await_readable(fd)` beside `wait_child` below, at
	// which point `vared`'s nested read becomes a nested await. Neither was
	// declared before it had a caller.
	virtual void on_command_boundary() noexcept = 0;

	// THE FOREGROUND WAIT, AND THE SECOND VERB (#208, phase 2a of #145).
	//
	// `::waitpid(pid, status, flags)`, said as a question rather than as a
	// syscall. The executor has exactly one wait - `tree_walking_executor::reap`,
	// which every one of its seven former `waitpid` sites now goes through - and
	// what it means is "I have nothing to do until this child does something;
	// whoever is hosting me may use the thread until then".
	//
	// THE NO-OP IMPLEMENTATION *IS* `::waitpid`, which is the whole reason this
	// verb can be introduced without moving a single behaviour: `lesh -c`, a
	// script, a unit test and a forked child call through one indirect call into
	// the identical syscall with the identical arguments. Only an interactive
	// host answers differently, and even it answers differently only when there
	// is a fiber to park - see `ui::event_loop::await_child`.
	//
	// THE ARGUMENTS ARE `waitpid`'s, IN `waitpid`'s ORDER OF MEANING but with the
	// flags beside the pid, because at every call site the flags are a property of
	// the WAIT (`WUNTRACED` where Ctrl-Z can reach it, nothing where it cannot)
	// and the status is the out-parameter. The answer is `waitpid`'s answer: the
	// pid, or -1 when there is no such child.
	//
	// `noexcept`, for the reason `on_command_boundary` is: this is called from
	// inside the executor, which expects to return a status rather than to unwind.
	virtual pid_t wait_child(pid_t pid, int flags, int* status) noexcept = 0;
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
