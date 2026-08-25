# leshper — Architecture Decisions

**Date:** 2026-08-25
**Status:** Approved — records the decisions of [map #82](https://github.com/nanov/lesh/issues/82)'s first session
**Companion to:** [`2026-08-25-leshper-requirements.md`](2026-08-25-leshper-requirements.md) (what leshper must do) — this document records *how*, as decided. Each decision's full reasoning lives on its ticket; this is the index a session loads once.

## 1. Vocabulary

Decided in [#84](https://github.com/nanov/lesh/issues/84), recorded in `CONTEXT.md`, binding everywhere: the editor is **leshper**; the editing unit is an **action** (`widget` is reserved for future UI surfaces); an **effect** is what a turn of the state machine emits, never a reactor's output; the lexer+parser are **the syntax layer**; the interactive side is **the UI**; "front end" is retired in every sense.

## 2. The spine and the scoreboard

ADR-0001 reaffirmed ([#85](https://github.com/nanov/lesh/issues/85)): POSIX sh is the spine, the flavour is lesh's own, deviations lean zsh, admitted one at a time with divergences counted. The yash suite stays the scoreboard, dash the differential authority. Nothing leshper does may move either without a recorded decision.

## 3. The syntax layer is leshper's public API

**C-1 through C-6 are frozen** as of [#103](https://github.com/nanov/lesh/issues/103)/[#104](https://github.com/nanov/lesh/issues/104): the parser is total, spans are real input offsets, `incomplete` and defect are separate channels, word roles include `command_name` and `redirect_target`, comments are lexer tokens recorded on a tree side list, command-substitution interiors are sub-parses in a side table sealed below a top-level watermark, and keyword tokens are flagged ([#105](https://github.com/nanov/lesh/issues/105)). Syntax-layer work may refactor freely behind this contract and never break it — that is what makes the shell-core and leshper tracks parallel.

The highlight parse passes `aliases = nullptr` (the default), so spans point at typed text. Re-parse per keystroke is settled by measurement (38.7 µs at 4 KiB against N-1's 1 ms); incremental parsing is dead as a topic.

## 4. One event loop owns the terminal

[#98](https://github.com/nanov/lesh/issues/98), owner's principle: **all tty interaction goes through the event loop** — `tcsetpgrp`, mode changes, winsize, indicator writes. A tty syscall anywhere else is a defect. Signals become events on the same path. leshper consumes events and emits a render buffer (A-3/A-9).

The loop's *implementation* (libuv vs hand-rolled poll/kqueue) is deliberately open — decided by benchmark under the quiescability requirement, together with the fork discipline. fish never used libuv; the owner leans libuv on a performance hunch; the benchmark rules.

- **Terminal ownership**: the terminal follows the foreground command — `tcsetpgrp` to the child's group before waiting, reclaimed at reap (`WUNTRACED`). Job-control *plumbing* is in; `fg`/`bg`/`jobs` UI stays out (slots in later — [#98](https://github.com/nanov/lesh/issues/98) Q4's seam).
- **Ctrl-C while typing** is the rebindable `cancel-line` action ( `$?` = 130) **and the user's INT trap fires — the zsh way** (owner's call). During a command the kernel delivers to the foreground group; the shell only reaps.
- **Stopped children** without job control: reclaim terminal, report, `$?` = 128+SIGTSTP, prompt returns, process stays stopped.
- **No path leaves the terminal raw**: one registered async-signal-safe restore function on every exit path, fish/zle style; winsize re-queried at every read start.

## 5. Threads, forks, and the three lanes

[#90](https://github.com/nanov/lesh/issues/90)/[#91](https://github.com/nanov/lesh/issues/91)/[#92](https://github.com/nanov/lesh/issues/92):

- **Arena per worker, reset per request**; nothing points into a worker's arena after its request — stale results' backing memory is *gone* (N-4 structurally). Allocation counters go `thread_local`. **Latest-wins substitution slot per reactor**: one in-flight + one pending, overwritten on supersede; queue depth ≤ 1 by type.
- **Two fork regimes**: at accept every worker parks (a parked thread holds no mutex), then execution-time forks are free; while editing, the editor never forks — external execution is `posix_spawn`, exec-only (fish's answer since 2.0). No `pthread_atfork`; debug assertions that the pool is quiesced at every fork site.
- **The port** maps an action's shell code onto three lanes: builtins/functions in-process; externals (pipelines and `$(...)`-captures of externals included) via spawn; only fork-requiring forms (subshells, `&`, shell-code cmdsub bodies) refuse, loudly. fish's whole language is lanes 1+2; lesh keeps full POSIX at execution time. Recursion ceiling 64. Action writes to `$BUFFER`/cursor/selection apply **atomically on return** — one undo entry, one generation bump (A-12 intact). `exit` in an action is honored. [#11](https://github.com/nanov/lesh/issues/11)'s no-runner-for-completion guarantee stands.
- **Future door**: a dedicated executor thread would change the port's implementation (message passing), not its contract.

## 6. The registries are the seam

[#89](https://github.com/nanov/lesh/issues/89): no second shell language, ever — "other languages" are extension languages over ADR-0006's flat C capability surface, and **the ABI is bidirectional**: extensions register callables outward; registered code calls back inward (execute, variables, functions, actions). Four registries: **builtins** (one table, asserted both ways), **variables** (shell state), **functions** (moved to shell state, [#106](https://github.com/nanov/lesh/issues/106) — reachable for `lesh.myFunc(...)`), **actions** ([#93](https://github.com/nanov/lesh/issues/93), designed language-neutral per NG-4). No restructuring of `src/runtime/` until #93 forces it.

## 7. The terminal floor and the cell

[#97](https://github.com/nanov/lesh/issues/97): required — ANSI + 256 colors + bracketed paste; opportunistic — truecolor, undercurl. Assume-first detection (trivial env reads), fish-style heuristics only when a real terminal misbehaves, **never terminfo, no startup queries**. Below the floor leshper never starts: one-line refusal (exit 2), message-then-`exec /bin/sh` held open. The cell is a small POD — grapheme ref + width + two tagged colors + attribute bits — always truecolor-valued; **quantization is the blitter's job**. All runtime, one binary.

## 8. Configuration

[#101](https://github.com/nanov/lesh/issues/101): **neovim's model, staged.** Non-interactive reads no rc, ever. Interactive reads `~/.leshrc` now; when Lua lands, the nvim lookup (`~/.config/lesh/init.lua` preferred) — the owner's stated biggest win. `$ENV` honored per POSIX when set. Dot-script error semantics; state → rc → first read; **no config store** — configuration is builtins (now) and capability calls (later) mutating the registries.

## 9. Unicode

[#88](https://github.com/nanov/lesh/issues/88), accepted: **generate our own tables** — UAX #29 grapheme boundaries as a two-stage trie + 16-rule machine, cluster width (which no library solves), width as runtime policy (the helix lesson), validated by Unicode's own 766 cases. Implementation: [#108](https://github.com/nanov/lesh/issues/108).

## 10. What was deliberately not decided

Open on the map, in dependency order: the **action/reactor registration ABI** ([#93](https://github.com/nanov/lesh/issues/93)); the **provider interfaces** and their failure contracts ([#94](https://github.com/nanov/lesh/issues/94)); the **selection model** ([#96](https://github.com/nanov/lesh/issues/96)) and behind it vi depth ([#99](https://github.com/nanov/lesh/issues/99)); a `vared` entry point ([#102](https://github.com/nanov/lesh/issues/102)); the event-loop implementation (benchmark, §4); the scope-chain question inherited from map #17; job-control UI; everything in requirements §8.
