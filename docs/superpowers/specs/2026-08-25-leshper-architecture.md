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

### 6.1 The action/reactor ABI ([#93](https://github.com/nanov/lesh/issues/93), ADR-0008)

**The registry stores the C shape; built-ins are its first clients — no native side door.** An action is `int32_t fn(lesh_editor*, const lesh_invocation*, void* userdata)`; the invocation carries invoking action name, key sequence, optional numeric argument. State access is copy-in/copy-out accessors over the opaque handle — no lent pointers, ever (the WASM insurance) — writes stage and commit atomically on return per #92. Loop outcomes (`accept_line`, `cancel_line`, `exit`, `recursive_edit`) are capability calls, never return values; positions are byte offsets that clamp and snap to cluster boundaries at commit, so bindings stay ignorant of grapheme geometry.

**Reactors emit only through a generation-bound request token** — there is no apply path in the ABI, so applying a stale result is unexpressible rather than checked (N-4, the capability answer to "type-level" in C). v1 event kinds: `buffer_changed`, `cursor_moved`, `selection_changed`; the snapshot is buffer + cursor + selection + generation, nothing else. Emit calls copy at the call site; the emitting reactor is the decoration namespace; spans carry interned semantic style ids mapped by the theme at render (F-21). Cancellation is a cooperative superseded poll.

One `int32_t` status space (0 ok, negatives ABI, positives domain). Action names are `snake_case` with dot-prefixed unshadowable originals (`.accept_line`); registration replaces, so rc re-sourcing is idempotent (#101). Actions, registration and lookup on the loop thread only; handles valid only for the receiving call, debug-asserted. Growth is additive-only: the recorded doors are syntax queries on the token and provider access (#94). The Lua sketch that proves NG-4 — two trampolines, zero new entry points — is on the ticket. The header lands with [#107](https://github.com/nanov/lesh/issues/107).

### 6.2 The provider interfaces ([#94](https://github.com/nanov/lesh/issues/94))

**Four providers; one discipline; the syntax layer is sealed.** The A-5 set is the syntax layer (lex + parse — A-5's illustrative `Parser`, renamed because C-6 needs the lexer independently callable), `Completer`, `HistoryStore`, `Prompt`. The last three are override points (A-13); the syntax layer is not — a replaceable parser would repeal C-5. Jobs and variables are *not* providers: job notifications are loop events (#98), variable access goes through #92's port.

**Provider calls ride the request-token machinery** — one async discipline, two triggers: reactors receive tokens on events, providers on demand. Streaming is a token that emits N batches, each a pooled generation-tagged message applied by the loop (F-31 for free). The recorded exception: the syntax layer is synchronous on the loop thread (38.7 µs vs N-1's 1 ms; the Enter-accept decision cannot wait) — "for now, revisit if measurement ever disagrees" (owner). C-2's tristate is a **derived view** over the existing orthogonal channels — `incomplete()` wins while more input can come, else top-level defect → invalid — no third channel invented.

**The failure contract: native providers are trusted threads; user providers are killable processes.** User-supplied provider code (lesh functions) runs in a `posix_spawn`ed child in provider mode — shell state is loop-owned (#90) and editing-time forks are forbidden (#91), so in-process user code on a worker was never on the table. Slow → loses the generation race; hang → killed on supersede or deadline; crash → SIGCHLD; garbage → dropped at parse. F-18 becomes structural for providers.

**Contracts, minimally:** `Completer` = snapshot + cursor in, streamed candidates out — `(text, description, trailing slash-vs-space, needs-quoting)`, nothing richer until the engine asks additively. `HistoryStore` = dumb lesh-side storage (append, snapshot-iterate newest-first, entries preserve newlines per F-34); the *searcher* — F-32's line/prefix/token modes, F-33 — lives in leshper over C-6's lexer; v1 store is an append-only log file, the atuin-direction arrives later behind the same interface (owner's noted trajectory). `Prompt` = provider returns bytes for the four F-40 surfaces plus the transient variant; **leshper measures width itself** (one measurer: #108 tables + SGR skipping — no `%{ %}` folklore); PS1 expansion and any caching/pre-compilation are lesh-side, behind the interface.

**The hot-path rule, written:** sealed interfaces bind statically; override points may cost one indirection, and only at human-action frequency (Tab, recall, prompt draw). The keystroke path's sole indirect call is action dispatch — the price of F-13.

**vared is an ordinary client**: leshper's entry is `read(providers)`; command-line editing passes the default bundle, F-17 passes a different one (null completer/history, trivially-complete syntax). Recorded as the acceptance test [#102](https://github.com/nanov/lesh/issues/102) runs against these interfaces.

### 6.3 The selection model ([#96](https://github.com/nanov/lesh/issues/96))

**One pair, three paradigms: anchor + active flag, and the cursor is the head.** The region is derived — `active ? [min(anchor,cursor), max(anchor,cursor)) : none` — exclusive half-open, endpoints always on grapheme-cluster boundaries (debug-asserted; #93's clamp-and-snap already enforces it at the ABI). Emacs projects mark/point onto it; vi visual projects inclusiveness (+1 grapheme) in its mode's operators and rendering; helix uses it raw — a helix-mode motion writes `anchor := old head`, making noun-then-verb a keymap behavior, not a core variant. **Shape lives in the mode**: linewise and blockwise are act/render-time projections (zle stores linewise as `REGION_ACTIVE=2`; the lesh binding can synthesize that from the mode for compatibility). Blockwise is out of v1 and arrives as a projection, non-breaking.

Under every edit, the anchor follows the marker rules in `apply_edit` — before → shift, after → unchanged, inside → clamp to edit start — `active` survives, and undo records carry anchor + active. Multi-cursor is future-proofed by access discipline, not a vector of one: scalar primary, all access through state methods, ABI accessors singular now and plural additively later. Select-then-act sequencing (F-10) is recorded as a keymap-machinery constraint: operator-pending is a keymap-stack push. Owner's scope note: v1 shape, reworked in v2 if needed.

### 6.4 Keymaps as data ([#117](https://github.com/nanov/lesh/issues/117))

**Flat tables keyed by symbolic key events; the mode is the base of the stack.** A keymap maps sequences of decoder-output key events to action names — never raw bytes (zle's model reversed deliberately: the decoder owns escape sequences once, bindings stop being terminal folklore). `key_event` grows an `alt` bit (plus shift for named keys); **ctrl stays a C0 codepoint** — `<C-w>` is notation that parses to U+0017, because that is all the wire carries at the #97 floor. User-facing notation is **vim's** (`<C-w>`, `<A-Left>`, `<Up>`, literal printables).

**Dispatch**: top-down through the stack, first exact match wins; any prefix match anywhere holds the pending sequence until F-5's timeout resolves to the longest exact match. Two hatches: an **opaque** keymap stops lookup (the pager's map routes unbound printables to its filter action, F-29), and a keymap may name a **default action** for unmatched keys; the global floor is `self_insert` for printables. **Full mode switches swap the base** (`set_mode`; vi's `i`/Escape); **sub-modes are pushes** — visual (sets #96's `active` on push, clears on pop), operator-pending (a `pending_operator` state slot set by the verb, consumed after the next motion — zle's `viopp` written down), pager, history search. F-40's indicator reads the topmost keymap declaring an indicator string. Helix mode needs neither slot nor push — its verbs read the always-present selection.

**No remap problem exists**: bindings name actions, never key sequences, so vim's map-vs-noremap distinction has nothing to distinguish. The rc surface is the **`bind` builtin** (`bind -N`, `bind -m <keymap> <keys> <action>`, `bind -l`), mutating the keymap registry per #101's no-config-store rule; the neovim-shaped `lesh.keymap.set(mode, lhs, action)` arrives with the Lua binding as the second frontend to the same registry (#93's model). The registry lives in leshper beside the action registry, loop-thread mutation, ABI functions additive.

### 6.5 Vi depth at v1 ([#99](https://github.com/nanov/lesh/issues/99))

**zle's repertoire + counts + text objects; one keyed kill store; minimal `.`.** Motions `h l j k 0 ^ $ w W b B e E f F t T ; ,`, operators `d c y` with doubled-line forms, `x s r ~ D C Y p P`, mode entries `i I a A o O v`, counts multiplying the vi way (`d2w`, `3dd`). **Text objects are in** — their cost collapsed once #96/#117 made an object *one action in operator-pending mode that sets the selection to a range*: `iw aw iW aW` via the segmenter and C-6, bracket/quote pairs via one matching helper (the same helper helix's `mi(` wants later, built in neither paradigm's idiom). **One kill store**: emacs's kill ring and vi's unnamed register are the same keyed store, unnamed key as default; named registers arrive later as an addressable view, and the owner's noted trajectory is a clipboard-backed key (vim's `"+` shape, OSC 52 as the floor-compatible transport). **`.` ships minimal**: dispatch records the last change's (action, count) and replays non-inserting changes; repeating an insert-carrying change is the documented v2 step. **The boundary is a paragraph, not a surprise**: not included — named registers, marks, macros, visual-block, insert-repeat. F-11 is not bent: vi's only core ask was the `pending_operator` slot #117 already gave everyone.

### 6.6 Logging, and the replay harness as its client ([#109](https://github.com/nanov/lesh/issues/109))

**One recorder, two sinks; level × category; off by default; trace compiles out of release.** Every message carries a level (`error warn info debug trace` — neovim's axis) and a category (`loop dispatch reactor provider spawn worker render history exec parse event` — fish's `FLOG` axis); enabled by `LESH_LOG=<level>[:<cat>,<cat>]`. **Cost rule**: a disabled log is one relaxed atomic load and a predictable branch, arguments evaluate only past the check, formatting uses a fixed thread-local buffer (truncate, never allocate), and `trace` is `#if`'d out of release — N-1 never sees logging. **Destination**: `$LESH_LOG_FILE`, default `${XDG_STATE_HOME:-~/.local/state}/lesh/log`; **never stderr while leshper owns the terminal** (#98); stderr only for a non-interactive shell with an explicit level; a startup `info` line records version, pid, tty and the floor-detection result.

**The N-3 replay harness is a client**: the `event` category logs every loop input — key events, resize, signals-as-events, worker-result arrival with generation and timing — and a **structured sink** (jsonl, one object per line) writes it when `LESH_REPLAY_FILE` is set; replay feeds the file back through `editor.apply` and compares with the state struct's existing equality. A second event serialization is the bug this prevents. The owner's dashboard instinct (#94) is a third client of the same records — nothing built. **Redaction**: buffer contents and key codepoints appear only at `trace`; `debug` logs lengths and action names, so it is safe to attach to a bug report; the replay file necessarily contains full input and the docs say so. Lives in the substrate (`src/substrate/log.{h,cpp}`) — shell core and leshper both use it, and it depends on nothing above the substrate. Text format `HH:MM:SS.mmm LEVEL category thread: message`.

## 7. The terminal floor and the cell

[#97](https://github.com/nanov/lesh/issues/97): required — ANSI + 256 colors + bracketed paste; opportunistic — truecolor, undercurl. Assume-first detection (trivial env reads), fish-style heuristics only when a real terminal misbehaves, **never terminfo, no startup queries**. Below the floor leshper never starts: one-line refusal (exit 2), message-then-`exec /bin/sh` held open. The cell is a small POD — grapheme ref + width + two tagged colors + attribute bits — always truecolor-valued; **quantization is the blitter's job**. All runtime, one binary.

## 8. Configuration

[#101](https://github.com/nanov/lesh/issues/101): **neovim's model, staged.** Non-interactive reads no rc, ever. Interactive reads `~/.leshrc` now; when Lua lands, the nvim lookup (`~/.config/lesh/init.lua` preferred) — the owner's stated biggest win. `$ENV` honored per POSIX when set. Dot-script error semantics; state → rc → first read; **no config store** — configuration is builtins (now) and capability calls (later) mutating the registries.

## 9. Unicode

[#88](https://github.com/nanov/lesh/issues/88), accepted: **generate our own tables** — UAX #29 grapheme boundaries as a two-stage trie + 16-rule machine, cluster width (which no library solves), width as runtime policy (the helix lesson), validated by Unicode's own 766 cases. Implementation: [#108](https://github.com/nanov/lesh/issues/108).

## 10. What was deliberately not decided

Open on the map, in dependency order: vi depth ([#99](https://github.com/nanov/lesh/issues/99), unblocked by the selection model); a `vared` entry point ([#102](https://github.com/nanov/lesh/issues/102)); the event-loop implementation (benchmark, §4); the scope-chain question inherited from map #17; job-control UI; everything in requirements §8.
