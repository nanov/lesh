# leshper — Requirements

**Date:** 2026-08-25
**Status:** Approved (draft 7, landed by [#83](https://github.com/nanov/lesh/issues/83))
**Companion to:** [`2026-08-20-lesh-scope-design.md`](2026-08-20-lesh-scope-design.md) — this spec details §2.3 of the scope

**leshper — lesh Prompt Editing & Reading.** The interactive line editor of the lesh shell. Integrated: lives in the lesh repo, compiled into the lesh binary, privileged access to shell internals. Not a library.

Design in one sentence: **zle's programmability on fish's execution model.**

Keywords per RFC 2119: MUST / SHOULD / MAY.

---

## 1. Vocabulary

| Term | Meaning |
|---|---|
| buffer | The text being edited. May span multiple lines. |
| action | Named, invocable, rebindable unit of editor behavior. What zle calls a "widget". Built-in actions = native code; user actions = lesh functions. |
| keymap | Map: key sequence → action name. First-class, mutable at runtime, stackable. |
| mode | A named keymap set defining an editing paradigm (emacs, vi-insert, vi-command, helix-normal...). |
| reactor | Subscriber to state-change events (buffer-changed, cursor-moved, ...). Runs async on workers. Produces only derived state: decorations and proposals. Never mutates buffer/cursor/selection. Built-in reactors: highlighter, autosuggester. |
| proposal | Derived state offered to the user (an autosuggestion, a candidate list). Becomes a buffer edit only through an accepting action. |
| decoration | Namespaced annotation anchored to buffer positions: highlight span or virtual text. Survives edits. |
| surface | Grid of styled cells leshper renders into. A blitter turns surfaces into terminal output. |
| generation | Counter bumped on every buffer mutation. Async results are tagged with it; stale results are dropped. |
| worker | Background thread computing highlights / suggestions / completions off the input path. |
| hot path | Code between receiving an input event and finishing the redraw. |

Reserved words: **widget** = future UI surface (panels, pickers — §8). Never use "widget" for an action. **command** = shell command, never an editor concept.

---

## 2. Architecture

- **A-1** leshper is a state machine over one explicit state struct: buffer, cursor, selection, keymap stack, decorations, pending input, generation, pager state. No editor state in globals.
  - Ref: fish keeps this in `reader_data_t` (fish `src/reader.cpp`), with a thread-safe snapshot `commandline_state_t` for script access. zle scatters it in globals (`zsh Src/Zle/zle.h`) — do not copy that.
- **A-2** The transition function `(state, event) → (state, effects)` MUST run without a TTY. Test harness from the first commit: feed events, assert state.
- **A-3** lesh's event loop owns the terminal fds. leshper never reads/writes the terminal; it consumes decoded input events and emits render output into a buffer the loop flushes. Resize and child-status signals arrive as events on the same path.
- **A-4** Worker communication is message-passing only, tagged with generation. Workers never touch editor state.
  - Ref: fish `s_generation` atomic + `get_bg_context()` cancel checker + `debounce_t` (all in `src/reader.cpp`). Copy this pattern.
- **A-5** leshper may call lesh internals (parser, completer, history, jobs, variables) only through narrow named interfaces (`Parser`, `Completer`, `HistoryStore`, ...), so the dependency surface stays enumerable.
- **A-13** The A-5 interfaces are also override points (zsh model: compsys, prompt themes, overridable widgets). lesh's default completer / history / prompt implementations can be replaced or wrapped by user code — lesh functions in v1, Lua later (§8). Consequences for leshper: it MUST depend only on the interface contract, never on behaviors of the default implementations; and it MUST stay responsive when a user-supplied provider is slow or broken (worker + generation discipline applies to providers exactly as to built-ins).

### 2.1 Future-proofing (hard v1 requirements; enables §8 without rewrite)

- **A-6** Render into an owned surface (cell grid); a separate blitter emits terminal output. Blitter must not assume a single surface forever.
- **A-7** One decoration system from the start. Highlighting (§4) and autosuggestions (§5) MUST be implemented as its clients, not as special cases.
- **A-8** All modal input (search, pager, recursive edit) works by pushing/popping keymaps on the one stack. No second dispatch system.
- **A-9** Events are the only delivery mechanism into the editor: keys, worker results, resize, job notices, injected input. No side-channel calls.

### 2.2 Execution model: actions vs reactors

Two kinds of behavior, strictly separated:

- **A-10** Only actions mutate primary state (buffer, cursor, selection). Actions are synchronous and run only when invoked (key press, script call, accepting a proposal). Reactors never mutate primary state; they produce only derived state (decorations, proposals), asynchronously, triggered by state-change events.
  - The loop: action edits buffer → generation bumps → buffer-changed event → reactors recompute on workers → results (generation-tagged) arrive as events → derived state updates → maybe an accepting action turns a proposal into the next edit. Strict alternation; no other path exists.
  - Anti-pattern this outlaws: zsh-syntax-highlighting wraps every widget to re-highlight synchronously — highlighting implemented as an action. Never do this. fish's model is correct: `super_highlight_me_plenty()` is called from the reader's state-change path, never dispatched as an input function.
- **A-11** Reactors register through one internal subscription interface (event kinds + a worker request/result contract). Built-in reactors — highlighter (F-20), autosuggester (F-24) — MUST be implemented on this interface, not special-cased. This is the interface §8 later exposes for plugin reactors (inline git-blame, live linting); pluggability is deferred, the interface is not.
- **A-12** A proposal never auto-applies. It becomes a buffer edit only through an accepting action the user (or their script) invokes. Corollary: leshper never edits the line behind the user's back.
- Classification test for any future feature: needs to mutate buffer/cursor/selection → action (sync, invoked). Derives information from state → reactor (async, produces decorations/proposals). Pairs are normal: *producing* a suggestion is a reactor; *accepting* it is an action.

### 2.3 Contracts required from lesh

lesh (POSIX sh with a curated layer — ADR-0001) provides the lexer and parser. leshper consumes them and imposes these contracts:

- **C-1 Tolerant parsing.** The parser MUST accept arbitrary input prefixes without bailing: every keystroke state is parseable. (fish: `TOK_ACCEPT_UNFINISHED` + `parse_util`; adopt the idea.)
- **C-2 Tristate verdict.** For a given buffer the parser reports **complete** / **incomplete** / **invalid**, distinctly. Drives F-35: incomplete → newline and keep editing; complete → accept; invalid → accept and let execution report, or highlight the error (F-21) — never silently swallow. Shell-language incompleteness that MUST be detected: unterminated heredoc, open quote/`$(`/`{`, dangling pipe or `&&`/`||`, unclosed `if`/`for`/`while`/`case`.
- **C-3 Source ranges.** Lexer and parser MUST attribute every token and error to buffer ranges (offsets leshper can map to grapheme positions) — decorations (F-20/F-21) are placed by range; no ranges, no highlighting.
- **C-4 Thread-safe on snapshots.** Parser and lexer MUST be callable from workers on an immutable buffer snapshot (pure function of input, no global parser state) — otherwise A-4/F-22 collapse back to synchronous highlighting.
- **C-5 One grammar.** The parse that highlights (F-20), the parse that judges completeness (C-2), and the parse that executes are the same code. Divergence here is the bug class F-20 exists to prevent.
- **C-6 Token-level access.** The lexer is independently callable (without full parse) for cheap needs: token under cursor, token boundaries for word-wise movement and token history search (F-32).

---

## 3. Editing core

- **F-1** Standard repertoire: char/word/line insert & delete, transpose, kill ring, unlimited linear undo/redo, selection region.
  - Ref: fish `editable_line_t` (`src/reader.cpp`) — note `edit_t` records old text + cursor for undo, coalesces single-char insertions, and supports edit groups (`begin_edit_group`). Adopt all three ideas.
- **F-2** Multiline buffer is a first-class 2D text object: vertical movement, edit any line anytime, always rendered whole.
- **F-3** Cursor and word boundaries operate on grapheme clusters, not bytes/codepoints. Double-width chars occupy correct columns.
- **F-4** Undo restores text + cursor; coalesces runs of plain typing into one step (see F-1 ref).
- **F-5** Input decoding: incremental UTF-8 (a codepoint may arrive split across reads); ambiguous key-sequence prefixes resolved with a configurable timeout.
- **F-6** Bracketed paste: one buffer mutation, one undo step, one generation bump, one redraw. Never per-character.
- **F-7** lesh code can inject synthetic input and invoke actions programmatically (zle's `zle -U` / `zle action-name`).

## 3a. Keymaps and modes

- **F-8** Keymaps are first-class data: create, copy, modify, push/pop from lesh code at runtime.
- **F-9** Ship default modes: **emacs**, **vi-insert**, **vi-command**. Plus dedicated keymaps for pager and incremental search.
- **F-10** A **helix-style mode** (selection-first: select object, then act) MUST be implementable without core changes, and SHOULD ship once vi mode is stable. Consequences that are v1-hard requirements:
  - selection region is a core primitive with well-defined semantics under every edit (F-1), not a vi-visual afterthought;
  - actions can read/extend/replace the selection;
  - keymaps can require "selection then operator" sequencing.
- **F-11** Adding a new mode = defining keymaps + actions in lesh script or native code. No enum of modes hardcoded anywhere.
  - Ref: zle keymap machinery (`Src/Zle/zle_keymap.c` in zsh repo) — keymaps by name, linkable, user-creatable (`bindkey -N`). This is the model. fish hardcodes its bind modes as strings with a `fish_bind_mode` variable — weaker; don't copy.
- **F-12** Any key sequence bindable to any action (built-in or user).

## 3b. Actions (the zle inheritance)

- **F-13** Every built-in behavior is a named action. Nothing unnamed, nothing unrebindable.
- **F-14** User actions are lesh functions. During execution, editor state is exposed as shell variables: buffer text, cursor, line count, selection, last key sequence, invoking action name. Writes apply on return.
  - Ref: zle exposes `$BUFFER`, `$CURSOR`, `$LBUFFER`, `$RBUFFER`, etc. (`Src/Zle/zle_thingy.c` wires builtins; `zle.h` declares the parameter plumbing). fish equivalent is the `commandline` builtin reading `commandline_state_t`. zle's variable model is richer; adopt it.
- **F-15** User actions can call other actions synchronously.
- **F-16** Recursive edit: an action can pump a nested editing session and resume (zle `recursive-edit`).
- **F-17** Editor invokable on an arbitrary string, not just a command line (zle `vared`).
- **F-18** A blocking/crashing user action never corrupts state or wedges the terminal; recover, report error above prompt.
- **F-19** Async user actions (awaiting workers/jobs): deferred to §8. v1 user actions are synchronous.

## 4. Syntax highlighting

- **F-20** Highlight with lesh's real parser — the grammar that will execute the line. No second grammar.
- **F-21** Distinguish at minimum: valid command / unknown command / alias-function-builtin / valid path / quoted string by kind / expansion-substitution / comment / syntax error. Independently themeable. Applied via decorations (A-7).
- **F-22** The highlighter is a reactor (A-10/A-11): runs on a worker, debounced, generation-tagged, stale results dropped. Keystroke latency independent of highlight cost (path checks touch the filesystem).
  - Ref: fish `debounce_highlighting()` (500ms), `highlight_complete()`, and the in-flight request dedup (`in_flight_highlight_request`) in `src/reader.cpp`; the highlighter itself is `src/highlight.cpp`. Also note `kHighlightTimeoutForExecutionMs = 250`: before executing, fish waits max 250ms for pending highlight, else falls back to no-io highlighting. Adopt.
- **F-23** Display never blocks on colors; previous highlighting shows over new text until results arrive.

## 5. Autosuggestions

- **F-24** The autosuggester is a reactor (A-10/A-11). It proposes an inline muted continuation from history (most recent match first); SHOULD fall back to completion-derived suggestions. The proposal is rendered as virtual-text decoration (A-7) — it is never buffer content: Enter with a suggestion showing executes only what was typed.
- **F-25** Accepting actions: accept whole suggestion, accept one word, dismiss. All rebindable. These are the only path from proposal to buffer (A-12).
- **F-26** Same worker/debounce/generation discipline as F-22. Stale suggestion never shown.
  - Ref: fish `autosuggestion_t`, `debounce_autosuggestions()` (500ms), `update_autosuggestion()` / `autosuggest_completed()` in `src/reader.cpp`.
- **F-27** Validity-filter suggestions where cheap (suggested `cd` target must exist) — on the worker.

## 6. Completion UI, history, multiline

(Candidate generation, history persistence, prompt expansion are lesh subsystems, consumed via the pluggable A-5/A-13 interfaces.)

- **F-28** Pager: columns + per-candidate descriptions, scrolls, keyboard-navigable via own keymap. Renders into its own internal surface (A-6); surface API not exposed in v1.
  - Ref: fish `src/pager.cpp` + `pager_t` usage in reader. Note fish 3.4 reused this same pager for Ctrl-R history search (`fill_history_pager` in `src/reader.cpp`) — proof the component generalizes; design ours the same way.
- **F-29** Typing while pager is open filters candidates incrementally.
- **F-30** Unambiguous common prefix inserted without opening pager. Candidate metadata (trailing `/` vs space, quoting) honored.
- **F-31** Large candidate sets stream from a worker into the pager, generation-gated.
- **F-32** Incremental history search fwd/back, matches highlighted, own keymap. (fish: `reader_history_search_t`, modes line/prefix/token — token search is worth copying.)
- **F-33** Prefix-constrained history navigation (up-arrow with `git c` typed cycles matches only).
- **F-34** Recalled multiline entries reconstruct as 2D buffers, not flattened.
- **F-35** On Enter: ask lesh's parser "complete construct?" Incomplete → insert newline, keep editing. Complete → accept. Separate actions to force-newline and force-accept.
  - Ref: fish `handle_execute` / `expand_for_execute` using `parse_util` checks in `src/reader.cpp`.
- **F-36** Continuation lines render with configurable secondary prompt; whole construct stays editable until accepted.

## 7. Rendering and shell coexistence

- **F-37** Diff-based rendering: compute desired surface, blitter emits minimal update. Full repaint only on resize or explicit request.
  - Ref: fish `layout_data_t` (a value-type "everything the screen needs" snapshot) + `screen_t` diffing in `src/screen.cpp`. The layout-as-value pattern also makes N-3 (virtual screen tests) trivial. Adopt.
- **F-38** Resize reflows buffer, prompt, pager correctly, including mid-edit.
- **F-39** Async shell output during editing (job finished, notifications) prints above the prompt; edit line repaints intact beneath; no state lost.
- **F-40** Prompts (left, right, secondary, mode indicator) are rendered by lesh; leshper treats them as opaque strings with declared widths. Transient prompt repaint on accept is supported.
- **F-41** Terminal ownership contract: raw mode + screen state established when a read begins, fully restored before running a command, on SIGTSTP/SIGCONT, and on abnormal exit. No path leaves the terminal raw.
  - Ref: fish `term_fix_modes` / `term_fix_external_modes` in `src/reader.cpp` — read the comments; each line is a scar from a real bug.

---

## N. Non-functional

- **N-1 Latency** — hot path: input event → finished redraw in <1ms editor CPU for buffers ≤4KiB. Hot path never touches the filesystem, never runs user shell code (except explicitly invoked user actions), never waits on a worker. 100KiB paste: <50ms, one redraw.
- **N-2 Memory** — hot-path allocation bounded and jitter-free: amortized buffer growth, reusable scratch. General heap OK where it can't threaten N-1. Worker results in pooled messages; superseded requests coalesce (no unbounded queues).
- **N-3 Testability** — everything in §3–§7 exercisable as event-in / state-out sequences without a PTY (A-2). Renderer tested against the surface model: assert exact cell grids, never golden escape sequences. Deterministic replay: recorded event sequence (incl. worker-result timing) reproduces identical state.
- **N-4 Correctness** — arbitrary UTF-8 correct (combining marks, ZWJ emoji, wide CJK) — enforced by tests. Malformed bytes degrade gracefully. Applying a stale-generation result to a newer buffer is structurally impossible (type-level, not convention).

---

## NG. Non-goals

- **NG-1** Not a standalone library. No external API, no separate artifact.
- **NG-2** No readline compatibility (API or `.inputrc`).
- **NG-3** No editor-owned config file or format, ever. leshper is configured through lesh's configuration surface. When lesh gains an rc mechanism (zsh-style; likely Lua — deferred, §8), leshper settings ride it. leshper itself never parses config.
- **NG-4** No leshper-invented scripting language. Extension languages are lesh script (the shell language; v1, F-14) and Lua (later, §8). Hard consequence **now**: the internal action/reactor registration ABI MUST be language-neutral — an action is a callable plus a state-access contract. The lesh binding (state as shell variables, F-14) is one binding of that ABI; the Lua binding (state as an object/table API, neovim-style) is another, added later without ABI change. Do not bake shell-variable injection into the ABI itself.
- **NG-5** Candidate generation, history persistence, and prompt rendering live in lesh — as pluggable providers (A-13), not sealed built-ins. leshper consumes the interface, never the implementation.

---

## 8. Deferred: UI platform

Later extension, neovim tradition: **widgets** (panels/floating surfaces for plugins), **plugin reactors** (the A-11 subscription interface made public: inline git-blame, live linting, custom suggesters), hook table (buffer-changed, cursor-moved, pre-prompt, timers, fd-readable), async jobs delivering output as events, namespaced plugin state, async user actions (F-19), **Lua bindings** (second binding of the NG-4 language-neutral ABI: Lua actions, Lua reactors, Lua providers for A-13, and the lesh rc/config mechanism per NG-3). Core ships primitives, never features.

Enabled by A-6…A-12. When it lands: pager's internal surface API gets formalized, pager becomes its first client. Plugin-facing API freezes only then; until then all internals refactor freely.

## 9. Open questions

Each open question is a ticket on [the leshper map](https://github.com/nanov/lesh/issues/82); the ticket holds the reasoning, this section holds only the pointer. Resolved questions move to the list below; the decisions themselves are indexed in the [architecture spec](2026-08-25-leshper-architecture.md).

Still open:

- **Q-3** `vared`-equivalent entry point at v1 or later → [#102](https://github.com/nanov/lesh/issues/102) — unblocked by #94; the interfaces it tests are frozen

Resolved:

- **Q-2** Selection semantics → [#96](https://github.com/nanov/lesh/issues/96): anchor + active flag, the cursor is the head; shape is the mode's projection — architecture spec §6.3
- **Q-1** Vi depth at v1 → [#99](https://github.com/nanov/lesh/issues/99): zle's repertoire + counts + text objects, one keyed kill store, minimal `.` — architecture spec §6.5

- **Q-4** Terminal floor → [#97](https://github.com/nanov/lesh/issues/97): ANSI + 256 colors + bracketed paste required, truecolor opportunistic, never terminfo, no startup queries — architecture spec §7
- The NG-4 language-neutral action/reactor ABI → [#93](https://github.com/nanov/lesh/issues/93): one C-shaped registry, results only through a generation-bound request token — ADR-0008, architecture spec §6.1
- The A-5/A-13 provider interfaces and their failure contracts → [#94](https://github.com/nanov/lesh/issues/94): four providers, syntax layer sealed, user overrides run as killable spawned children — architecture spec §6.2

## 10. Reference index

| Topic | Where to read |
|---|---|
| Reader state, generation counter, debounce, undo groups, layout snapshot | fish 3.7.1 `src/reader.cpp` — https://github.com/fish-shell/fish-shell/blob/3.7.1/src/reader.cpp |
| Highlighting engine | fish 3.7.1 `src/highlight.cpp` |
| Pager | fish 3.7.1 `src/pager.cpp` |
| Screen diffing | fish 3.7.1 `src/screen.cpp` |
| Key decoding / bind modes | fish 3.7.1 `src/input.cpp`, `src/input_common.cpp` |
| Worker pool | fish 3.7.1 `src/iothread.cpp` |
| zle core declarations, editor state | zsh 5.9 `Src/Zle/zle.h` — https://github.com/zsh-users/zsh/blob/zsh-5.9/Src/Zle/zle.h |
| zle main loop, getbyte, zle -F fd watchers | zsh 5.9 `Src/Zle/zle_main.c` |
| Keymaps as named objects (`bindkey -N`) | zsh 5.9 `Src/Zle/zle_keymap.c` |
| Widget/builtin wiring, `$BUFFER` parameters, `zle -U` | zsh 5.9 `Src/Zle/zle_thingy.c`, `Src/Zle/zle_params.c` |
| Screen refresh | zsh 5.9 `Src/Zle/zle_refresh.c` |
| vi mode implementation | zsh 5.9 `Src/Zle/zle_vi.c` |
| Library-boundary prior art (traits: Completer/Highlighter/Validator); closest existing design to leshper, in Rust | reedline — https://github.com/nushell/reedline |
| No-dependency terminal handling, input decoding and unicode in plain C; NG-2 bounds what to take from its readline-shaped editing model | isocline — https://github.com/daanx/isocline |
