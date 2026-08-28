# ADR-0011: One thread, cooperative fibers

**Status:** Accepted
**Date:** 2026-08-28

Supersedes [ADR-0009](0009-two-owner-threads.md). Written when step 3 of #145
landed (#203), against the design recorded on #145 — the grilling record, the
architecture review, the tick statement and the later notes — and implemented by
#198, #199, #200, #201 and #202. Amended by #206 (the yield's price) and #208
(phase 2a: execution as a fiber). The library evidence is the research note,
[2026-08-26 stackful fibers: Tarantool and minicoro](../superpowers/research/2026-08-26-stackful-fibers-tarantool-minicoro.md).

## Context

ADR-0009 gave the process two owner threads because *worker threads read shell
state*: the highlighter needed the alias, function and builtin tables while the
loop could be mutating them. #151 made the shell-state reactor the one reader and
that premise expired. What was left was a pile of machinery whose entire job was
to make two threads look like one: a condition variable, three latest-wins slots,
a reply pool, a `shell` topic, a `worker` topic, a four-thread helper pool with
per-worker arenas and a completion pipe, a park-before-fork handshake (#91), and
a signal mask on every spawned thread so Ctrl-C would land where it was wanted.

The mechanisms were correct and each was cheap. Together they were the price of
concurrency nothing needed: a shell's editing-time computes are microseconds,
already latest-wins-cancellable, and none of them may run while a command runs.

## Decision

**One OS thread hosts a scheduler of stackful fibers. Nothing in the interactive
shell runs in parallel.** The library is minicoro, vendored, with a `PROT_NONE`
guard page added to its virtual-memory reserve; `src/fiber/` is the scheduler,
the stack allocator and the one channel type. A scheduler is **instantiable,
never global** — the cord door stays open.

**Two roles survive, and they are all ADR-0009 ever bought.** The shell owns
`shell_state`; the loop owns editor state and the terminal. They take turns
instead of taking locks, and the turn-taking is a call: `event_loop` calls
`shell_side::execute` and `shell_side::port_call` directly. `shell_writing_flag`
stays as the tripwire that says the premise still holds.

**The host is the loop, not a fiber.** `event_loop::run` is the tick. The UI part
never parks, and a fiber that never parks is the loop with extra ceremony.

**EXECUTION *IS* A FIBER, and that is what phase 2 bought (#208).** One fiber in a
lane of its own — `execution`, never parked by phase, because it is what the phase
is ABOUT — with an 8 MB reserve stack (16 under ASan) given per spawn, committed on
touch, spawned on the first accepted line and parked on its inbox between
commands. Its body is `for(;;){ line = inbox.recv();
done.send(shell.execute(line)); }`; the accept path sends and then KEEPS TURNING
until the answer is there, and the foreground wait at the bottom of the
interpreter is a park rather than a blocked thread. **Phase is still written at
exactly the two host places.** While `executing`, the tty and timer topics are out
of the poll set — the terminal is the child's, and a timer would dispatch an action
into an editor that is not on screen — render is suppressed, and the signal
topic's byte is drained without producing an editor event, because a SIGINT turned
into `cancel_line` there would call `execute` from inside `execute`. The signal
NUMBERS are held and replayed on the first turn after the command, which is the
delivery the self-pipe byte used to make by simply staying in the pipe.

**AND EXECUTION IS ALSO RUNNABLE INLINE, first-class, chosen per `execute` by the
host** (`loop_options::execution`, `LESH_EXECUTION=inline` at the wiring site). On
that path `shell_side::execute` runs on the host's own stack, `current()` is null
throughout, and every wait is a blocking `::waitpid` — the shell exactly as it ran
before #208. It is not a degraded mode: it is what an action's `port_call` and the
EXIT trap after `run()` has returned take anyway, and it is the recorded way out if
a fiber stack ever turns out to be the wrong place to fork from. Both modes are
covered by `UiLoop*` and `UiPty*`, because a path that is not exercised is a path
that does not work.

**The fiber-or-inline decision is ONE SCHEDULER PRIMITIVE, and nothing above it
branches.** `scheduler::block_or_park(slot, run_blocking, enlist)`:
`current() == nullptr` runs the blocking thing on the stack it is already on and
answers with its result; on a fiber it `enlist`s the caller's `await_slot` with
whoever will complete it, parks, and answers with what `complete` stored.
Completing from inside `enlist` is legal and means the fiber never parks — which is
how a caller says "this has already happened", and how `sleep 0.1 & wait` finds a
zombie whose SIGCHLD was drained turns ago. `src/fiber/` therefore never sees a pid
or a file descriptor: `await_slot` is one word of answer and the fiber to wake, and
the waiter TABLE belongs to the host, which is the only side that knows what it is
waiting for. `cooperation::wait_child` is a five-line adapter over it, and #209's
`await_readable` will be another.

**Phase is where the session is**, written at exactly two places — `quiesce()`
(→`executing`) and `resume_after_execution()` (→`boundary`→`editing`). **Groups
are scheduler tags**, one bit each, and a group's bit is derived FROM the phase,
never set beside it. Parking a set is one store: under cooperative scheduling
suspended is every fiber's default state, so there is no handshake and no race.
That is what made #91's quiesce structural rather than negotiated.

**Emitters and observers are the two reactor kinds, and they differ in channel
policy and lifetime, not in mechanism.**

| | emitter | observer |
|---|---|---|
| input | a buffer state, per keystroke | an event: a line was accepted |
| may touch the line | yes — that is its purpose | never |
| **lifetime** | **the LINE's**: superseded on every edit, cancelled at accept | **the SESSION's**: outlives every line |
| channel | `slot<T>`, conflating — overwrite IS cancellation | ordered `queue<T,N>`, never conflated |
| examples | highlighter, autosuggester | history persistence, telemetry |

The highlighter and the autosuggester are emitters and are fibers. `observers`
is a named, reserved, empty group: naming it is what keeps `emitters` from
quietly meaning "all fibers".

**The tick, in the owner's words: reactor slices before and after the UI part.**
A turn gives every runnable emitter one slice, polls — timeout 0 while any of
them is runnable, the ordinary deadline otherwise — drains the topics, dispatches
what arrived, gives every runnable emitter another slice, and renders. A buffer
change bumps the generation and SENDS into each emitter's slot; a cursor move
cancels nothing (#90's rule, unchanged).

**`slot<T>` is the cancellation.** Every `send` supersedes every outstanding
token — not only a send that overwrites an unconsumed value, because the case
that matters is the reactor that already consumed line 1 and is computing when
line 2 arrives. The reactor notices at its next cancellation poll, and **that
poll is also its yield** (`lesh_request::cooperate`): a `$PATH` stat storm is
spread over turns that each read the terminal first. How OFTEN a reactor polls is
the reactor's own, and is a statement about the size of its unit of work — per
`$PATH` lookup, per 256 tokens, per 256 history entries (#206, in Consequences). **The paint therefore lands
one turn after the keystroke, and that is the semantics, not a regression** — a
reactor that polls has yielded, so its compute finishes in a leading slice of the
following turn, and that turn is immediate because a runnable emitter makes the
poll timeout 0.

**The runtime's seam is `runtime::cooperation`, and it is never null.** The
executor knows two sentences — "a command just finished, whoever is hosting me may
run" and "I have nothing to do until this child does something" — and says them
through an abstract class, not a template: whether a shell is
interactive is a runtime fact decided in `main()`. `shell_state` starts with a
static no-op, so there is not one null check in the runtime and no site can
forget one. `lesh -c`, a script, a unit test and a forked child all cooperate
with nobody at the cost of an indirect call to a `return`; `enter_subshell` puts
the no-op back, for every role, because nothing a child can do makes its parent's
host the right thing to talk to.

**THE SECOND VERB IS `wait_child`, AND ITS NO-OP IMPLEMENTATION *IS* `::waitpid`
(#208)** — which is why it could be introduced without moving one behaviour.
`tree_walking_executor::reap` is the runtime's ONE wait: all seven former direct
`waitpid` calls go through it, `WUNTRACED` staying exactly where the file already
had it (the two foreground waits Ctrl-Z can reach), and `run_async`'s `_background`
bookkeeping untouched. A script, `lesh -c`, a unit test and every forked child
reach the identical syscall with the identical arguments through one indirect call.
**The host reaps ONLY AWAITED PIDS, never `-1`**, so a background child stays a
zombie until `wait` asks for it exactly as before and no job-control semantics
moved in that ticket. The "notices mid-command" upgrade is a later ticket built on
this seam, not a side effect of it.

**There is no fiber call stack.** Fibers are long-lived and wait for messages;
they are never called. The host is the sole resumer, every yield returns to the
host, and fibers reach each other only through a `slot` and a wake. minicoro's
`prev_co` resume chain is present and unused; `scheduler::run_one_slice` asserts
that slices do not nest. Phase 2's nested cases are park-and-wake, not nested
resumes.

**Background threads stay possible, and only as independent ones.** A thread is
admitted when blocking I/O is the job itself — the history's append and fsync are
the candidate, because `O_NONBLOCK` does not apply to regular files and poll
cannot express them. Such a thread owns its inputs by copy, touches no shell or
editor state, reports through one ordered channel and an fd the loop polls
("indistinguishable from a spawned child, and cheaper"), **blocks the caught
signal set** and **never forks** — main is the only forking thread, and a mask
survives `execve`. `BS::thread_pool` (single header, MIT) is the recorded
candidate for the plain-pool branch; a Tarantool-style **cord** — a thread
carrying its own scheduler — is the other branch, wanted only if observer
*plugins* must run off-thread. Neither is built: the branch is decided by the
first customer.

**The accepted ledger line.** A long-running builtin blocks everything, and under
threads that dormancy was POLICY — the loop thread existed and was parked — while
under fibers it is PHYSICS. Behaviourally identical today, because the policy
parked everything anyway; what moves is the optionality. Accepted as-is, with the
door priced: the executor's command boundary is a yield point that already exists
(`on_command_boundary`, beside the `g_pending` poll), and turning it on makes the
shell strictly MORE live during execution than the parked-thread design ever was.
**Detection, not preemption**, is the ceiling a C++ runtime can reach — no
compiler safepoints, and an async stack switch at an arbitrary instruction is UB
— so the debug watchdog is the mechanism: a slice that runs 50 ms without
yielding logs in a shell and aborts under `lesh_tests`.

**Interactive-only, and it is measured.** A non-interactive shell constructs no
scheduler, no stack and no channel; the proof obligation carried from #134 is
per-file conformance, byte-identical across every landing of this work, and it
has held for all five steps.

## Consequences

- Deleted: `shell_actor` and its slots, the condvar, the reply pool, the `shell`
  and `worker` topics, `worker_pool` with its threads, arenas, pooled messages,
  `parked_scope` and `current_worker_arena`, the loop's `std::thread`,
  `block_caught_signals_on_this_thread`, and #91's park-depth counter. Roughly
  2,100 lines out for ~500 in.
- **A green `lesh_tests` is not a green gate.** minicoro hands ASan's stack
  bounds back only on a coroutine-to-coroutine switch, which under the
  no-call-stack rule is a switch lesh never takes — so the thread's recorded
  bounds stayed a fiber's from its first yield, `__asan_handle_no_return`
  declined before every `_exit` in a forked child, and #198's LeakSanitizer
  negative control had been passing because LSan was scanning the fiber stack as
  if it were the thread's. `run_one_slice` restores the host bounds after every
  resume and `spawn` registers every live fiber stack with
  `__lsan_register_root_region` — tracing rather than suppressing. LSan runs at
  process exit, so the leak a fiber's stack holds appears after the last
  `[ PASSED ]` line and only `ctest` turns it into a red case (#202: 1915/1915
  green, 512 KB leaked).
- A fiber's stack is not free: 512 KB reserved per fiber, 1 MB under ASan,
  committed on touch, with a guard page below. Two fibers was the v1 inventory;
  #208 makes it three, the third being the execution fiber's 8 MB reserve (16
  under ASan) through `spawn`'s per-spawn size override. Reserve is not memory —
  the pages a `run_input` actually walks are what get committed — and the size is
  what a thread gets on Linux by default, so a deeply nested script survives to
  the same depth interactively as it does in a script.
- **FORKING FROM A FIBER STACK UNDER ASan IS FINE, and that was the ticket's
  first-contact risk** (#202 flagged it, #208 measured it). Every fork lane the
  interactive shell has — a subshell, a command substitution, both stages of a
  pipeline, an `&` child, a function calling an external — is taken from the
  execution fiber's stack in `UiPtyExecution.EveryForkLaneRuns...` and in
  `UiLoopExecution`'s own forking shell, under ASan/UBSan/LSan, on the real
  binary, and there is nothing to report. The reason it works is #202's own fix:
  minicoro announces the switch INTO a coroutine, so ASan's record of the current
  stack is correct for the whole of a slice, which is exactly when a fork happens;
  `run_one_slice` restores the host bounds after the resume, and
  `__asan_handle_no_return` before the child's `_exit` therefore has the right
  bounds on both sides. No leak is reported for the execution fiber parked on its
  inbox at shutdown either: its frame owns nothing, and `spawn` registered the
  stack as an LSan root region in any case.
- **The park needs a wake, and the wake is SIGCHLD — so the host asks the kernel
  whether it still has it** (`signal_hub::catches`). `reassert`'s rule 3 leaves an
  inherited `SIG_IGN` and a user's `trap '' CHLD` alone, both legitimate, and both
  mean nothing will ever ring the self-pipe again; with SIGCHLD ignored the kernel
  reaps children itself, so even a poll woken for another reason would find
  nothing. `await_child` therefore takes the wait inline when the answer is no,
  which is one `sigaction` query per foreground command and the difference between
  a shell that runs `trap '' CHLD; sleep 1` and a shell that hangs on it.
- **`turn` is re-entrant now, and it is safe by exclusion rather than by design.**
  The accept path's turns run from inside the outer turn's event walk. Nothing is
  pushed onto `_events` while `executing` — the signal drain defers instead, and no
  other topic is polled — so the swap that would move the outer walk's storage out
  from under it never happens. Written down because the next verb to park inside a
  command inherits this and not a rule.
- A keystroke costs no more than it did: the reactors' contribution to 100
  keystrokes is **0 mallocs**, measured against a loop with no reactors at all.
- Serialization is the cost and is bounded the same way it always was: a stat
  storm delays the next highlight, never a keystroke's dispatch.
- **A yield's price is one readiness check, and the platform charges for it**
  (#206). The switch is 12 ns and the tick is 20; `turn(0)` was 6.7 µs, of which
  ~6.5 was a `poll(2)` that found nothing — XNU takes a "wait for zero
  nanoseconds" path through the scheduler for that case, and charges 8 µs for it
  whether or not there are any descriptors, where `select` with a zero `timeval`
  charges 0.2. The zero-timeout case now asks `select` (`ready_now` in
  `loop.cpp`); the blocking case keeps `poll`, where the cost is the wait.
- **A poll site strides to the size of its unit, and the walk was the one that
  did not.** A history entry is 4 ns of work and a yield is ~155, so the
  autosuggester's 5000-entry walk paid 760 µs to protect 20 — 25x its own
  no-yield time even with the turn made cheap. `history_search::run` therefore
  polls every `history_search::poll_every` (256) entries, exactly as the
  highlighter's segment sweep already polls every 256 tokens; the walk is then
  1.2x its no-yield time, ~1 µs of latency before a keystroke is looked at, and
  ~1 µs of work done past a supersede nobody noticed. **The stride is on the WALK
  and not on the yield**, because `classify_command` polls immediately before
  each `$PATH` stat precisely so that a stat storm yields between stats — a
  stride on `lesh_request::cooperate` would have been a stride over syscalls.

## Deferred — phase 2, priced and not built

`wait_child(pid)` LANDED IN #208 and is struck from this list; what is left of the
line is `await_readable(fd)` on the `cooperation` seam (the fd verb is coost's
shape: `io_event(fd, ev).wait(ms)` — register interest for the current fiber, park,
resume on readiness or timeout, deregister), and it is a five-line adapter over
`block_or_park` like its sibling. Then: `read` and `tail -f` as awaits on it —
`wait` is already one; **`vared`'s nested read as a plain nested await**, which is
what retires its slot-refusal plumbing; `request<Req,Rep>` for the port, WHICH #208
DECLINED TO BUILD because it would not have removed code (the waiter table is
pointers into the awaiting fibers' own frames, ten lines, and a request/reply type
over it would have been more machinery for the same shape); **`queue<T,N>` with
`close()`** — a receiver
parked on a channel whose producer is destroyed must wake with "closed", not
sleep for ever (coost `chan::close`) — as the observers group's first customer;
prompt modules as fibers (`git_head` is I/O, so cord material); Lua plugins,
whose capability surface supplies the I/O verbs because stock LuaJIT has none;
reactors running during execution (do not flip the bit); the `select`-shaped
composite waiter, which arrives with phase 2's second customer.

**Rejected from coost, with the reasons**: shared copy-out stacks — a pointer
into a suspended stack is exactly the mid-parse reactor case, and copying is
worse than owning; and libc syscall hooking — process-global state in a forking,
exec'ing, job-controlling shell.

**Asio was considered and declined.** Asio has no stackful coroutines of its own,
so "Asio instead of minicoro" means Asio plus C++20 stackless coroutines, and
three things lesh must suspend live under frames it will not rewrite: reactors
behind the flat-C `lesh_reactor_fn` pointer, Lua inside `lua_pcall`, and the
tree-walking executor beneath phase 2's awaits. Its `io_context` would also own
`run()`, which is to say it would own the deterministic tick, the phase and the
group park that this ADR is mostly about.

## Open

- **What a stopped foreground job does to the loop is unchanged and still
  unresolved** (#161's ledger line, restated because #208 is where it now lives).
  `WUNTRACED | WNOHANG` in the sweep reports a stop exactly as the blocking wait
  did, so Ctrl-Z returns the prompt and `$?` is 128+SIGTSTP; a pipeline whose
  MIDDLE stage stops still hangs the shell, because there is no job table. That is
  the same floor as before, now reached through a park.
- **LSan on Linux is unverified** — no environment. Darwin needed the explicit
  root-region registration; the registration is unconditional, so it should be
  right either way, and the fix if it is not is `__lsan_ignore_object` on parked
  stacks rather than a redesign.
- The cold-cache `$PATH` sweep's contribution to keystroke latency has not been
  measured once, as ADR-0009's third amendment asked.

## References

- #145 (the design record), #82 (the umbrella), #198–#203 (phase 1), #206 (the
  yield stride), #208 (phase 2a: execution as a fiber).
- Research note: `docs/superpowers/research/2026-08-26-stackful-fibers-tarantool-minicoro.md`.
- ADR-0007 (one owner per message in flight), ADR-0008 (the token capability
  surface, whose `superseded` poll is now also the yield), ADR-0009 (superseded).
