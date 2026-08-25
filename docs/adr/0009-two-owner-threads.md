# ADR-0009: Two owner threads — the shell owns its state, leshper only reads it

**Status:** Accepted
**Date:** 2026-08-26

## Context

leshper runs reactors on worker threads while the user types (A-10/A-11), and
some of them need to know things only the shell knows — whether `ll` is an
alias, whether `ls` is on `$PATH` (F-21). Meanwhile an action bound to a key can
run shell code that *changes* those tables (#92's port). The first design put
shell state on the loop thread and made workers read it through a
copy-on-write version held by the request token (#130) — correct, but a
mechanism whose only job was to make concurrent reads of mutable state safe.

The owner's observation dissolved the problem instead of solving it: leshper
never mutates shell state, it reads only small parts of it, and it reads only
while the shell is *not executing* — a command line is either being edited or
being run, never both. And of the reactors, only the highlighter reads shell
state at all; history search and the autosuggester need nothing but the buffer.

## Decision

**The process has two owner threads, and shell state has exactly one.**

- **The shell is the main thread.** It owns `shell_state`, forks and execs,
  reaps, and handles the terminal handoff — as `main()` always has, so the
  non-interactive shell (`lesh -c`, scripts) starts no second thread at all.
  While a command line is being edited it serves three **latest-wins slots**,
  in priority order: `execute`, `port_call` (an action's shell code), and
  `highlight`. Each slot holds at most one item; a newer highlight overwrites a
  pending one, which is the cancellation. It waits on a condition variable, not
  a poll — it has no file descriptors to watch while editing.
- **leshper's loop is a spawned thread.** It owns editor state and the tty
  while editing, dispatches keys to actions, and lays out and blits. It waits
  in its `poll(2)`; the shell thread reaches it through one wakeup pipe — one
  more topic beside `tty`, `signal`, `worker`, `timer` (#128). During execution
  it blocks in that same poll, so signals and the terminal-restore path still
  flow through it.
- **On the shell thread, everything is serialized.** A port call that writes
  state, a highlight that reads it, and an execution never overlap — there is
  no second thread that can touch shell state. The highlighter reads the alias,
  function and builtin tables and `$PATH` directly; `command_kind` is a local
  lookup. The copy-on-write definitions version of #130 is **deleted**, not
  kept as insurance.
- **Staleness has one version: the editor's generation.** Every message from
  the shell thread to the loop carries the generation it was computed against;
  the loop drops mismatches. That is the request token's existing rule.
- **State-free work stays parallel.** History search, the autosuggester's scan
  and `stat`-heavy path checks need only the buffer; they run on the stateless
  helper pool (#126), cancelled by supersede as before.
- **The fork happens on the shell thread**, which is the main thread. Quiesce
  is: helpers parked, loop blocked in its wait. The child claims the terminal
  itself between fork and exec (fish's `child_setup_process`), so the loop
  never needs the child's process group.

## Consequences

- One invariant replaces three mechanisms: no versions, no locks on shell
  state, no parked-only-mutation rule around the port. Aliases are classified
  at highlight time from the real table (one level of resolution for kind;
  never expanded — spans stay on typed text, #95).
- A port call is now a cross-thread round trip (~5 µs by #115's numbers,
  against N-1's 1 ms) instead of an in-thread call. #92's contract is
  unchanged; #92 predicted exactly this implementation change.
- Serialization on the shell thread is the cost: a `$PATH` stat storm delays
  the next highlight, never a keystroke's dispatch. The highlighter polls
  `superseded` before every lookup; `execute` and `port_call` outrank
  `highlight` in slot order.
- #129 (the loop) hosts neither the port nor the highlighter; #135 shrinks to
  the link-boundary interface, the ABI verb and the highlighter's classes;
  #134 (switch-on) starts one thread, not several. The ABI, token model, worker
  pool, highlighter code, layout, decoder and history search are untouched —
  the highlighter changes threads, not lines.
- The non-interactive shell is exactly today's program. Everything in this
  ADR is what an interactive read *adds*.
