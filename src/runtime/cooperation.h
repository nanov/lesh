#pragma once

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
	// Called once per command, immediately after `run_pending_traps()` at each of
	// that function's call sites - the same two places, in the same order, because
	// they are the same event: the shell has finished a command and has not yet
	// started the next one. After the traps rather than before, so a host that
	// eventually renders something sees the state a trap body left behind.
	//
	// `noexcept`, because the boundary is inside the executor's command loop and a
	// host throwing out of it would unwind the shell through code that expects to
	// return a status.
	//
	// v1 IS A NO-OP FOR EVERYONE: nothing installs an implementation yet (the
	// one-thread ticket does). Phase 2 of #145 adds `wait_child(pid)` and
	// `await_readable(fd)` here, at which point `vared`'s nested read becomes a
	// nested await. Neither is declared before it has a caller.
	virtual void on_command_boundary() noexcept = 0;
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
