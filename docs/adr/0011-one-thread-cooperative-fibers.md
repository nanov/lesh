# ADR-0011: One thread, cooperative fibers

**Status:** Accepted
**Date:** 2026-08-28

Supersedes [ADR-0009](0009-two-owner-threads.md). Written when step 3 of #145
landed (#203), against the design recorded on #145 — the grilling record, the
architecture review, the tick statement and the later notes — and implemented by
#198, #199, #200, #201 and #202. The library evidence is the research note,
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
spread over turns that each read the terminal first. **The paint therefore lands
one turn after the keystroke, and that is the semantics, not a regression** — a
reactor that polls has yielded, so its compute finishes in a leading slice of the
following turn, and that turn is immediate because a runnable emitter makes the
poll timeout 0.

**The runtime's seam is `runtime::cooperation`, and it is never null.** The
executor knows one sentence — "a command just finished, whoever is hosting me may
run" — and says it through an abstract class, not a template: whether a shell is
interactive is a runtime fact decided in `main()`. `shell_state` starts with a
static no-op, so there is not one null check in the runtime and no site can
forget one. `lesh -c`, a script, a unit test and a forked child all cooperate
with nobody at the cost of an indirect call to a `return`; `enter_subshell` puts
the no-op back, for every role, because nothing a child can do makes its parent's
host the right thing to talk to.

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
  committed on touch, with a guard page below. Two fibers is the v1 inventory.
- A keystroke costs no more than it did: the reactors' contribution to 100
  keystrokes is **0 mallocs**, measured against a loop with no reactors at all.
- Serialization is the cost and is bounded the same way it always was: a stat
  storm delays the next highlight, never a keystroke's dispatch.

## Deferred — phase 2, priced and not built

`wait_child(pid)` and `await_readable(fd)` on the `cooperation` seam (the fd verb
is coost's shape: `io_event(fd, ev).wait(ms)` — register interest for the current
fiber, park, resume on readiness or timeout, deregister); `read`, `wait` and
`tail -f` as awaits on those; **`vared`'s nested read as a plain nested await**,
which is what retires its slot-refusal plumbing; `request<Req,Rep>` for the port
when execution becomes a fiber; **`queue<T,N>` with `close()`** — a receiver
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

- **LSan on Linux is unverified** — no environment. Darwin needed the explicit
  root-region registration; the registration is unconditional, so it should be
  right either way, and the fix if it is not is `__lsan_ignore_object` on parked
  stacks rather than a redesign.
- **The yield stride** (#206). Yield-at-every-poll costs the autosuggester's walk
  ~6x and buys ~400x on keystroke latency; the autosuggester polls per entry
  while the highlighter polls per 256 tokens, and a stride or a clock gate would
  decouple cancellation granularity from latency at the price of the
  deterministic slice count N-3's replay wants.
- The cold-cache `$PATH` sweep's contribution to keystroke latency has not been
  measured once, as ADR-0009's third amendment asked.

## References

- #145 (the design record), #82 (the umbrella), #198–#203 (the steps).
- Research note: `docs/superpowers/research/2026-08-26-stackful-fibers-tarantool-minicoro.md`.
- ADR-0007 (one owner per message in flight), ADR-0008 (the token capability
  surface, whose `superseded` poll is now also the yield), ADR-0009 (superseded).
