#ifndef LESH_LESHPER_ABI_H
#define LESH_LESHPER_ABI_H

/* The action/reactor ABI (#93, ADR-0008, architecture spec 6.1).
 *
 * This file is C, and that is the whole point. ADR-0006 fixed the outer
 * boundary as flat C - no C++ type crosses it, no arena pointer is handed out -
 * and NG-4 requires that adding the second binding (Lua, later) need no change
 * here. The lesh binding and the ten built-in actions are its first clients,
 * registered through the identical signatures a binding would use. There is no
 * native side door: src/leshper/builtin_actions.cpp includes this header and
 * nothing else from leshper, so a built-in that wanted a shortcut would not
 * compile.
 *
 * THE TWO PROPERTIES C HAS NO TYPE FOR, and how they are expressed anyway:
 *
 *   Applying a stale reactor result must be structurally impossible (N-4).
 *   There is no apply function anywhere below. Emission functions exist only on
 *   `lesh_request*`, a token you are handed and cannot mint, and the loop - the
 *   only applier - drops a completed batch whose generation has moved on.
 *   Staleness is unexpressible rather than checked. Capability, where C has no
 *   affine type.
 *
 *   No lent pointers, ever (the WASM insurance). Every accessor copies in or
 *   copies out at the call site. A reactor's arena dies with its request (#90)
 *   and nothing it allocated is retained, so an emit call copies too.
 *
 * GROWTH IS ADDITIVE ONLY. New capability functions, new `lesh_motion`
 * enumerators, new event-mask bits, new emit kinds. Signature changes are
 * forbidden - which is why an action returns status and nothing else, and a new
 * loop outcome is a new function rather than a new return value. The two
 * recorded future doors are both new functions: syntax queries on the request
 * token (deliberately absent in v1 - the only clients are native and call the
 * syntax layer directly), and provider access (#94).
 *
 * THREADING, and it is not advisory. Registration, lookup, and every action run
 * on the loop thread. A reactor's compute call and its token live on a worker.
 * Handles are valid only for the duration of the call that received them;
 * stashing one is undefined behaviour, asserted in debug builds.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Status                                                                     */
/*                                                                            */
/* One int32_t space. Zero is ok, negatives are owned by this file, positives  */
/* pass through as the binding's domain status - a user action's exit status   */
/* crosses without the ABI knowing that shells have exit statuses.             */
/* ------------------------------------------------------------------------- */

#define LESH_OK 0

/* A malformed argument: a null pointer, an out-of-range enumerator, a name
 * that is not snake_case. */
#define LESH_ERR_INVAL (-1)

/* An out buffer too small for the answer. The length out-parameter is still
 * filled in with what would have been needed, so the caller can retry. */
#define LESH_ERR_TOOSMALL (-2)

/* The request this token belongs to has been superseded. Returned by the
 * cooperative poll, and the conventional status for a reactor that noticed and
 * gave up. */
#define LESH_ERR_SUPERSEDED (-3)

/* The action recursion ceiling (#92: 64) was reached. */
#define LESH_ERR_RECURSION (-4)

/* Structurally refused: shadowing an unshadowable original, a capability that
 * does not exist yet (selection writes until #96), an operation whose
 * combination is not meaningful (history movement with staged edits pending). */
#define LESH_ERR_REFUSED (-5)

/* No action or reactor of that name is registered. */
#define LESH_ERR_NOTFOUND (-6)

/* ------------------------------------------------------------------------- */
/* Handles                                                                    */
/* ------------------------------------------------------------------------- */

/* The registries: actions, reactors, and the style intern table. Loop-thread
 * only, and long-lived - it is the one handle a binding keeps.
 *
 * ARGUED ADDITION, and the only place this header departs from the letter of
 * #93, which writes registration as the tuple `(name, mask, fn, userdata)`.
 * Something has to own the tables. ADR-0007 requires every allocation to have
 * an owner that frees it before main returns, and a file-scope registry has
 * none; a process-global one would also make "one loop, one registry" a comment
 * rather than a type. So the tuple is carried, as #93 says, and the registry it
 * is carried into is named - the same opaque-handle pattern as `lesh_editor`
 * and `lesh_request`, which is the pattern the rest of the design already is. */
typedef struct lesh_registry lesh_registry;

/* The editor, during one action call. Everything an action may read or write
 * goes through this and copies at the boundary. */
typedef struct lesh_editor lesh_editor;

/* The request token: the generation-bound snapshot a reactor computes against,
 * and the only mint for results. Valid only during the compute call. */
typedef struct lesh_request lesh_request;

/* ------------------------------------------------------------------------- */
/* Actions                                                                    */
/* ------------------------------------------------------------------------- */

/* What invoked an action.
 *
 * `action_name` is the name dispatch went through, which is not always the
 * action's own: a wrapper registered over `accept_line` and delegating to
 * `.accept_line` sees the name the user's key was bound to. `keys` is a byte
 * sequence, not a string - it is what the terminal sent - and is NULL with
 * length zero when the invocation did not come from a key. */
typedef struct lesh_invocation {
	const char* action_name;
	const char* keys;
	size_t keys_length;
	int64_t numeric_argument;
	int32_t has_numeric_argument;
} lesh_invocation;

/* An action.
 *
 * The signature #93 resolved, unchanged and unchangeable: opaque editor handle,
 * invocation, registration-time context pointer. The return is STATUS ONLY.
 * Loop outcomes - accept, cancel, exit, recursive edit - are capability calls
 * on the handle, so a new outcome is a new function and never a change here. */
typedef int32_t (*lesh_action_fn)(lesh_editor* editor,
                                  const lesh_invocation* invocation,
                                  void* userdata);

/* Registers an action under `name`, replacing any existing registration.
 *
 * Names are snake_case: a lowercase letter, then lowercase letters, digits and
 * underscores. Anything else is LESH_ERR_INVAL.
 *
 * REGISTRATION REPLACES, so re-sourcing an rc file is idempotent (#101) rather
 * than an error or a duplicate.
 *
 * THE FIRST registration of a name also mints `.name` - the unshadowable
 * original. A later registration of `name` replaces it; `.name` keeps pointing
 * at the first definition forever, and registering a dot-prefixed name directly
 * is LESH_ERR_REFUSED. That is what lets a wrapper delegate to the behaviour it
 * wrapped, and what keeps F-18's recovery path from dispatching through a name
 * the user has replaced. Minting it for every action rather than only for
 * built-ins is deliberate: a wrapper over a plugin's action needs the same
 * escape hatch a wrapper over a built-in does, and one rule is cheaper to
 * remember than two. */
int32_t lesh_action_register(lesh_registry* registry,
                             const char* name,
                             lesh_action_fn fn,
                             void* userdata);

/* True (1) when an action of that name is registered. */
int32_t lesh_action_exists(lesh_registry* registry, const char* name, int32_t* out);

/* Invokes another action from inside one (F-15).
 *
 * The invoked action shares the caller's staging area and its undo group, which
 * is what makes a wrapper delegating to `.accept_line` one undo entry and one
 * generation bump rather than two. The recursion ceiling is 64 (#92);
 * LESH_ERR_RECURSION past it. */
int32_t lesh_action_invoke(lesh_editor* editor,
                           const char* name,
                           const lesh_invocation* invocation);

/* ------------------------------------------------------------------------- */
/* Editor state: copy-in, copy-out                                            */
/*                                                                            */
/* No accessor lends a pointer into editor state. Native built-ins pay a       */
/* line-sized copy per access at keystroke rate; ADR-0008 measured that as     */
/* noise against N-1's millisecond and took it, so that the ABI cannot rot     */
/* unexercised.                                                               */
/*                                                                            */
/* Writes STAGE. The loop commits them atomically when the action returns -    */
/* one undo entry, one generation bump (#92, A-12) - so an action that edits   */
/* six times is one step of undo, and an action that edits and then changes    */
/* its mind can leave the buffer as it found it.                              */
/*                                                                            */
/* Positions are byte offsets into the buffer. They clamp into range, and a    */
/* write range snaps outward to grapheme cluster boundaries, so no binding     */
/* needs grapheme geometry and no binding can split a cluster (#108 stays      */
/* inside leshper). The cursor clamps and snaps at commit.                     */
/* ------------------------------------------------------------------------- */

/* The buffer's length in bytes. */
int32_t lesh_buffer_length(lesh_editor* editor, size_t* out);

/* Copies the buffer out. `*length_out` is always set to the buffer's full
 * length, whether or not it fit; LESH_ERR_TOOSMALL when it did not. `out` may
 * be NULL with `capacity` zero, which is how a caller asks for the length
 * before allocating. No NUL is appended - the buffer is bytes. */
int32_t lesh_buffer_get(lesh_editor* editor, char* out, size_t capacity, size_t* length_out);

/* Copies out the bytes in [from, to). The range clamps; `*length_out` is the
 * slice's full length. */
int32_t lesh_buffer_read(lesh_editor* editor, size_t from, size_t to,
                         char* out, size_t capacity, size_t* length_out);

/* Replaces [from, to) with `length` bytes. `bytes` may be NULL with `length`
 * zero, which deletes. */
int32_t lesh_buffer_replace(lesh_editor* editor, size_t from, size_t to,
                            const char* bytes, size_t length);

/* Replaces the whole buffer. `$BUFFER=...` in the lesh binding. */
int32_t lesh_buffer_set(lesh_editor* editor, const char* bytes, size_t length);

int32_t lesh_cursor_get(lesh_editor* editor, size_t* out);
int32_t lesh_cursor_set(lesh_editor* editor, size_t offset);

/* The selection (F-10, #96).
 *
 * An anchor and a flag, with the CURSOR as the head - so setting a region moves
 * the cursor to `end`, and moving the cursor moves the region. The pair the
 * getter reports is the derived one, `[min, max)`, exclusive half-open and on
 * cluster boundaries; `*active_out` is the separate question of whether it
 * means anything, and the anchor outlives a clear (emacs's mark). `start > end`
 * in the setter is a direction, not an error.
 *
 * Singular by decision, not by omission: multi-cursor arrives as ADDITIONAL
 * plural functions and leaves these three alone. */
int32_t lesh_selection_get(lesh_editor* editor,
                           size_t* start_out, size_t* end_out, int32_t* active_out);
int32_t lesh_selection_set(lesh_editor* editor, size_t start, size_t end);
int32_t lesh_selection_clear(lesh_editor* editor);

/* The generation the editor is at (A-10, N-4). Bumped by buffer mutation and
 * by nothing else. */
int32_t lesh_generation(lesh_editor* editor, uint64_t* out);

/* ------------------------------------------------------------------------- */
/* Text geometry                                                              */
/*                                                                            */
/* The editor owns it and answers questions about it. This is the other half   */
/* of "no binding needs grapheme geometry": a binding does not need it because */
/* it can ASK, not because it never wants to know. `backward_char` written any */
/* other way is a binding reimplementing UAX #29, which is the C-5 bug class   */
/* one layer down.                                                            */
/*                                                                            */
/* Additive: a new motion is a new enumerator, and an unknown one is           */
/* LESH_ERR_INVAL rather than a guess.                                        */
/* ------------------------------------------------------------------------- */

typedef enum lesh_motion {
	/* The grapheme cluster boundary before / after `from` (F-3). */
	LESH_MOTION_PREV_CLUSTER = 0,
	LESH_MOTION_NEXT_CLUSTER = 1,
	/* The line `from` is on - the buffer is a 2D text object (F-2), so these
	 * stop at newlines and not at the ends of the buffer. */
	LESH_MOTION_LINE_START = 2,
	LESH_MOTION_LINE_END = 3,
	/* Word-wise. Blank-separated today; C-6 makes the lexer independently
	 * callable so this can become token-wise without the ABI noticing. */
	LESH_MOTION_PREV_WORD = 4,
	LESH_MOTION_NEXT_WORD = 5,
	LESH_MOTION_BUFFER_START = 6,
	LESH_MOTION_BUFFER_END = 7,

	/* --- Appended by #119, never reordered --------------------------------
	 *
	 * The vi repertoire needed geometry the first eight do not have, and every
	 * one of them is a question about TEXT rather than about vi, so they are
	 * spelled in neither paradigm's vocabulary and helix mode will reuse them
	 * unchanged. A binding asks; it does not reimplement - the same rule that
	 * put cluster stepping here in the first place.
	 *
	 * The two WORD FAMILIES are the one thing worth naming carefully.
	 * LESH_MOTION_*_WORD_* (4, 5) are blank-separated, which is what emacs's
	 * word motions and vi's `W B E` both mean. The `WORD_*` family below is
	 * CLASS-AWARE: a word is a run of identifier bytes or a run of punctuation,
	 * which is what vi's `w b e` mean and what a shell user wants when a path
	 * and its slashes are one blank-separated blob. C-6 makes the lexer
	 * independently callable and both families become token-wise without this
	 * enum noticing. */

	/* The first non-blank byte on the line `from` is on (vi's `^`). */
	LESH_MOTION_LINE_FIRST_NONBLANK = 8,

	/* The same byte column on the previous / next line, clamped to that line's
	 * end. The 2D buffer (F-2) is what makes these mean anything at a prompt. */
	LESH_MOTION_LINE_UP = 9,
	LESH_MOTION_LINE_DOWN = 10,

	/* Class-aware word geometry (vi's `w`, `b`, `e`). */
	LESH_MOTION_WORD_START_NEXT = 11,
	LESH_MOTION_WORD_START_PREV = 12,
	LESH_MOTION_WORD_END_NEXT = 13,

	/* Blank-separated word geometry (vi's `W`, `B`, `E`). START_NEXT is not
	 * LESH_MOTION_NEXT_WORD: that one lands past the END of the word, which is
	 * what emacs's `forward-word` means and is a different place. */
	LESH_MOTION_BLANK_WORD_START_NEXT = 14,
	LESH_MOTION_BLANK_WORD_START_PREV = 15,
	LESH_MOTION_BLANK_WORD_END_NEXT = 16
} lesh_motion;

int32_t lesh_position_move(lesh_editor* editor, size_t from, lesh_motion motion, size_t* out);

/* ------------------------------------------------------------------------- */
/* Loop outcomes, as capabilities                                             */
/*                                                                            */
/* Requested, never performed, and never returned. The action goes on running  */
/* after one of these; the loop reads the request once the action's writes     */
/* have been committed. Two requests in one action: the last wins.             */
/* ------------------------------------------------------------------------- */

int32_t lesh_accept_line(lesh_editor* editor);
int32_t lesh_cancel_line(lesh_editor* editor);
int32_t lesh_exit(lesh_editor* editor, int32_t status);
int32_t lesh_recursive_edit(lesh_editor* editor);

/* Undo and redo (F-1, F-4).
 *
 * History movement, not a staged write: it restores text AND cursor and bumps
 * the generation on its own. LESH_ERR_REFUSED when the action has staged buffer
 * writes that have not been committed - an action that edits and then undoes in
 * the same call is not asking for anything the history can mean. Answers
 * LESH_OK when there was nothing to undo, having changed nothing, because
 * pressing undo once too often is not an error. */
int32_t lesh_undo(lesh_editor* editor);
int32_t lesh_redo(lesh_editor* editor);

/* Pushes bytes onto the input stack, read back as though typed (F-7, zle's
 * `zle -U`) - through the keymap, not around it. */
int32_t lesh_push_input(lesh_editor* editor, const char* bytes, size_t length);

/* ------------------------------------------------------------------------- */
/* Reactors                                                                   */
/* ------------------------------------------------------------------------- */

/* v1 event kinds. A mask, reserved for additive growth. */
#define LESH_EVENT_BUFFER_CHANGED 0x1u
#define LESH_EVENT_CURSOR_MOVED 0x2u
#define LESH_EVENT_SELECTION_CHANGED 0x4u

/* A reactor's compute, run on a worker against a token it did not mint.
 *
 * The token carries the snapshot - buffer, cursor, selection, generation - and
 * NOTHING ELSE. It is the only place emission exists. */
typedef int32_t (*lesh_reactor_fn)(lesh_request* request, void* userdata);

/* Registers a reactor. Names are snake_case, and registration replaces, as for
 * actions. `event_mask` is a bitwise or of LESH_EVENT_*; zero, or any bit
 * outside the defined set, is LESH_ERR_INVAL.
 *
 * No dot-prefixed original is minted, and that is not an oversight: the dot
 * name exists so a wrapper can delegate to what it replaced along a dispatch
 * path, and a reactor has no dispatch path - it is pushed to, and the emitting
 * reactor IS the decoration namespace, so replacing one replaces its
 * decorations too, which is the whole of what a user wants. */
int32_t lesh_reactor_register(lesh_registry* registry,
                              const char* name,
                              uint32_t event_mask,
                              lesh_reactor_fn fn,
                              void* userdata);

int32_t lesh_reactor_exists(lesh_registry* registry, const char* name, int32_t* out);

/* --- The request token ---------------------------------------------------- */

int32_t lesh_request_buffer_length(const lesh_request* request, size_t* out);
int32_t lesh_request_buffer(const lesh_request* request,
                            char* out, size_t capacity, size_t* length_out);
int32_t lesh_request_cursor(const lesh_request* request, size_t* out);
int32_t lesh_request_selection(const lesh_request* request,
                               size_t* start_out, size_t* end_out, int32_t* active_out);
int32_t lesh_request_generation(const lesh_request* request, uint64_t* out);
int32_t lesh_request_event_kind(const lesh_request* request, uint32_t* out);

/* The cooperative cancellation poll. `*out` is 1 once a newer request has
 * superseded this one; a long compute checks it and returns
 * LESH_ERR_SUPERSEDED. Not checking is safe - the loop drops the batch anyway -
 * it just wastes the worker. */
int32_t lesh_request_superseded(const lesh_request* request, int32_t* out);

/* --- What the shell knows (#130, #135) ------------------------------------ */

/* What a command name is. Additive, like every other enumerated space here: a
 * new kind is a new number, and a consumer that does not know it reads it as
 * something it cannot classify rather than as a wrong class. LESH_COMMAND_
 * UNKNOWN is zero so an ignored failure reads as the harmless answer. */
#define LESH_COMMAND_UNKNOWN 0u
#define LESH_COMMAND_EXTERNAL 1u
#define LESH_COMMAND_BUILTIN 2u
#define LESH_COMMAND_FUNCTION 3u
#define LESH_COMMAND_ALIAS 4u

/* Classifies a command name against the shell's own tables and $PATH.
 *
 * `name` is BYTES, `length` bytes of them, as they appear in the buffer - no
 * NUL, no quote removal, no expansion. The caller decides whether the bytes name
 * a command at all; the highlighter asks only for words the lexer marked
 * literal, because `$cmd` names a command only after expansion.
 *
 * Resolution is the shell's: alias, then function, then builtin, then a $PATH
 * walk with a stat per directory, then LESH_COMMAND_UNKNOWN. A name containing a
 * slash is a pathname and is not looked up in any table (POSIX 2.9.1.1). An
 * alias is resolved ONE level - the answer is `alias` and the body is neither
 * re-resolved nor expanded (#95: spans stay on the typed bytes).
 *
 * THE FILESYSTEM IS WHY THIS IS ON THE TOKEN and not on the editor handle. The
 * walk is the one cost F-22 moved off the keystroke path, so it belongs where a
 * cooperative poll can abandon it: a caller polls lesh_request_superseded before
 * each call, and a stat storm then delays the next highlight rather than a
 * keypress. The answers are memoized for the life of the request, so a name
 * repeated on one line is walked once.
 *
 * With no shell attached to the token the tables are empty and $PATH is the
 * process environment's - what a leshper embedded in something that is not this
 * shell sees. */
int32_t lesh_request_command_kind(const lesh_request* request,
                                  const char* name, size_t length, uint32_t* out);

/* --- Emission: only here, and only outward ------------------------------- */

/* A highlight span over [start, end) carrying an interned semantic style id
 * (F-21). Positions clamp into the snapshot. */
int32_t lesh_emit_span(lesh_request* request, size_t start, size_t end, uint32_t style_id);

/* Virtual text shown at `at` without being in the buffer - what the
 * autosuggester (F-24) draws. */
int32_t lesh_emit_virtual_text(lesh_request* request, size_t at,
                               const char* bytes, size_t length);

/* The same, carrying an interned semantic style id (F-21).
 *
 * ADDITIVE, and a new function rather than a fifth parameter, because #93 wrote
 * the emit as `emit_virtual_text(pos, bytes)` and a signature change is the one
 * kind of growth this header forbids. The unstyled call is exactly this one
 * with LESH_STYLE_NONE, which is what "no style, the renderer decides" already
 * meant.
 *
 * The autosuggester (#133) is the consumer that asked: its continuation is
 * muted, and "muted" is the THEME's word for the id it interns as `suggestion`
 * - a reactor that named a colour here would be the thing F-21 exists to
 * prevent. Without an id to carry, interning one would be ceremony. */
int32_t lesh_emit_virtual_text_styled(lesh_request* request, size_t at,
                                      const char* bytes, size_t length,
                                      uint32_t style_id);

/* Proposal kinds (a proposal becomes a buffer edit only through an accepting
 * action; it never auto-applies). */
#define LESH_PROPOSAL_AUTOSUGGESTION 0u
#define LESH_PROPOSAL_COMPLETION 1u
/* A history entry matching the query (F-32); the search UI's kind. */
#define LESH_PROPOSAL_HISTORY_MATCH 2u

int32_t lesh_propose(lesh_request* request, uint32_t kind, const char* bytes, size_t length);

/* ------------------------------------------------------------------------- */
/* Styles                                                                     */
/*                                                                            */
/* A span carries a SEMANTIC id - "comment.todo" - and the theme maps ids to   */
/* attributes at render (F-21). A plugin that insists on a literal colour      */
/* interns a fixed-attribute id and gets one; a plugin that does not stays     */
/* themeable for free.                                                        */
/*                                                                            */
/* Interning is loop-thread only, like everything else on the registry: a      */
/* binding interns the ids it will use when it registers, and the id is a      */
/* plain integer thereafter, which is what a worker needs. Id 0 is reserved    */
/* and means "no style".                                                      */
/* ------------------------------------------------------------------------- */

#define LESH_STYLE_NONE 0u

int32_t lesh_style_intern(lesh_registry* registry, const char* name, uint32_t* out);

/* The name an id was interned under, copied out. LESH_ERR_NOTFOUND for an id
 * nobody interned. */
int32_t lesh_style_name(lesh_registry* registry, uint32_t style_id,
                        char* out, size_t capacity, size_t* length_out);

/* ------------------------------------------------------------------------- */
/* Timers (#128 decision 3, #129)                                             */
/*                                                                            */
/* THE LOOP'S TIMER TOPIC, MADE PUBLIC. There is no timer file descriptor and  */
/* no `EVFILT_TIMER`: the loop's poll timeout IS `min(deadlines) - now` on a   */
/* monotonic clock, and these two calls are how a binding puts a deadline into */
/* that minimum. Spinners and plugin ticks, and §8's hook-table timers         */
/* arriving early. Unarmed, the mechanism costs nothing - the timeout is -1    */
/* and the loop blocks.                                                       */
/*                                                                            */
/* ADDITIVE, as growth here always is: two new functions, no signature         */
/* changed, and a binding that never calls them cannot tell they exist.        */
/*                                                                            */
/* REPEATING, and rearmed FROM THE MOMENT IT FIRES rather than from the        */
/* instant it was due. A loop that spent a minute inside a command must not    */
/* then dispatch a one-second timer sixty times; catch-up is a behaviour a     */
/* caller would have to ask for and nobody has.                                */
/*                                                                            */
/* Loop-thread only, like everything else on the registry (ADR-0008).          */
/* ------------------------------------------------------------------------- */

/* Arms a repeating timer that dispatches the action named `action` every
 * `interval_ms` milliseconds. `*id_out` receives the handle `lesh_timer_stop`
 * takes.
 *
 * The action is looked up BY NAME AT EXPIRY, not resolved here: a timer armed
 * before its action is registered is legal, and re-registering the action
 * replaces what the timer runs - which is the same rule dispatch follows for a
 * key. LESH_ERR_INVAL for a zero interval or a name that is not snake_case. */
int32_t lesh_timer_start(lesh_registry* registry, uint64_t interval_ms,
                         const char* action, uint64_t* id_out);

/* Disarms it. LESH_ERR_NOTFOUND for an id that is not armed, so stopping twice
 * is reported rather than silently accepted. */
int32_t lesh_timer_stop(lesh_registry* registry, uint64_t id);

/* ------------------------------------------------------------------------- */
/* Proposals, from the ACTION side (#133, F-25)                               */
/*                                                                            */
/* A reactor proposes; an accepting action decides. The two halves are         */
/* deliberately not symmetric: `lesh_propose` exists only on the request       */
/* token, and there is no way to read a proposal back from there, because a    */
/* reactor reading its own stale output is the loop's job to prevent and not   */
/* a thing to be careful about. These two exist only on the EDITOR handle,     */
/* which an action holds and a reactor never does.                            */
/*                                                                            */
/* There is still no apply path. An accepting action reads the bytes and       */
/* stages a buffer write like any other action (A-12), so accepting a          */
/* suggestion is one undo entry and one generation bump, and nothing about it  */
/* is privileged. That is why this is an ACCESSOR and not `lesh_accept`.       */
/* ------------------------------------------------------------------------- */

/* Copies out the `index`-th proposal of `kind` that is currently on screen.
 *
 * Currently on screen, not "ever emitted": the loop applies only batches
 * computed against the generation the editor is still at (N-4), so what this
 * reads is by construction a proposal about the text the buffer holds now.
 *
 * `index` walks the proposals of that kind in emission order across the applied
 * batches, so an autosuggester's single candidate is index 0 and the search
 * UI's list (#118) is 0, 1, 2, ... LESH_ERR_NOTFOUND once they run out, which
 * is also the answer when nothing has been proposed - pressing the accept key
 * with no suggestion showing is not an error.
 *
 * Copy-out, like every other reading accessor: `*length_out` is the proposal's
 * full length whether or not it fit, and `out` may be NULL with `capacity`
 * zero to ask the length first. No NUL is appended. */
int32_t lesh_proposal_read(lesh_editor* editor, uint32_t kind, size_t index,
                           char* out, size_t capacity, size_t* length_out);

/* Dismisses what is on screen for `kind` (F-25's third action).
 *
 * REQUESTED, NEVER PERFORMED, the same shape the loop outcomes have: the action
 * goes on running and the loop drops the batches when it returns. Two dismisses
 * in one action are one dismissal, and the last kind wins.
 *
 * It drops the whole batch that carried the proposal, virtual text and spans
 * included, because a suggestion the user dismissed must stop being drawn and
 * the drawn half is the virtual text. The reactor is free to propose again on
 * the next event - dismissal is about what is showing, not a mute. */
int32_t lesh_proposal_dismiss(lesh_editor* editor, uint32_t kind);

/* ------------------------------------------------------------------------- */
/* Completion, from the ACTION side (#139, F-28/F-30, 6.9)                    */
/*                                                                            */
/* THE `Completer` PROVIDER, PULLED. #94 gave leshper four providers and two   */
/* trigger shapes: reactors receive tokens on EVENTS, providers on DEMAND.     */
/* Tab is the demand, and these three are the door abi.h recorded as a future  */
/* one ("provider access (#94)") arriving with its first consumer.             */
/*                                                                            */
/* WHY NOT A REACTOR. A reactor runs when the buffer changed; completion runs  */
/* when a key was pressed, and it must have finished before the action that    */
/* pressed it returns. There is no event to subscribe to and no batch to wait  */
/* for - which is also why the work is synchronous on the loop thread and why  */
/* 6.9 records that as the deviation from F-31 rather than hiding it.          */
/*                                                                            */
/* WHAT THEY DO NOT DO. They do not decide anything and they do not edit. F-30 */
/* lives in `lesh_pager_commit` so that every client of the pager gets one     */
/* rule (see the pager block below), and the one buffer write is the pager's   */
/* staged insertion. A completion action reads these three, feeds the pager,   */
/* and commits - which is the whole of it.                                     */
/* ------------------------------------------------------------------------- */

/* Runs the completer over the buffer as it stands and answers how many
 * candidates it found.
 *
 * LESH_ERR_NOTFOUND when no completer is wired up, which is not an error a
 * binding should report: a leshper embedded with no completer answers no
 * candidates. Calling it twice in one action recomputes; the previous set is
 * discarded. */
int32_t lesh_complete(lesh_editor* editor, size_t* count_out);

/* The half-open byte range of the buffer the candidates replace - what
 * `lesh_pager_open` wants.
 *
 * It is the COMPONENT under the cursor, not the whole word: for `~/Doc` it
 * starts at the `D` so the `~` stays in the buffer (6.9), and for `$HOM` it
 * starts at the `H`. LESH_ERR_NOTFOUND before `lesh_complete` has run in this
 * call. */
int32_t lesh_completion_range(lesh_editor* editor, size_t* from_out, size_t* to_out);

/* Copies out the `index`-th candidate and its LESH_PAGER_* kind - the two
 * arguments `lesh_pager_add` takes, in the order it takes them.
 *
 * The bytes are the candidate as it goes INTO the buffer, quoted where the
 * shell would not read the name back literally (6.9's "needs-quoting applied to
 * the inserted part only" - the range above is the inserted part, so a `~` or a
 * directory prefix outside it is never re-quoted). They are bare of the kind's
 * trailer, because that is the pager's to append.
 *
 * Copy-out like every other reader: `*length_out` is the full length whether or
 * not it fit, and `out` may be NULL with `capacity` zero to ask the length
 * first. `kind_out` may be NULL. LESH_ERR_NOTFOUND past the last candidate. */
int32_t lesh_completion_candidate(lesh_editor* editor, size_t index, char* out,
                                  size_t capacity, size_t* length_out, uint32_t* kind_out);

/* ------------------------------------------------------------------------- */
/* Modes and the keymap stack (#117 decision 5, #119)                         */
/*                                                                            */
/* THE DECISION #118 LEFT OPEN, and it is decided here. The actions that enter */
/* and leave insert, command, visual and operator-pending modes could have     */
/* been native C++ over `state` - `set_mode`, `push` and `pop` are already     */
/* there, one call each. They are not, and the reason is ADR-0008's first      */
/* sentence: there is no native side door. An `i` implemented in C++ over the  */
/* state would be a capability the built-in vi mode has and the Lua binding    */
/* cannot have, and NG-4 says the second binding needs NO ABI change - which   */
/* it would need, on the day someone writes a plugin that enters a mode. Mode  */
/* entry is the single most-bound behaviour in a modal editor; a surface that  */
/* cannot express it is not the surface a binding writes against.              */
/*                                                                            */
/* So the ABI grows, additively, by the six functions below - exactly the      */
/* growth the ADR sanctions ("new capability functions"). src/leshper/vi.cpp   */
/* includes abi.h and nothing else from leshper, the same as                   */
/* builtin_actions.cpp, and so is held to this surface by the compiler.        */
/*                                                                            */
/* WRITTEN THROUGH, NOT STAGED, and that is deliberate. Staging exists so an   */
/* action's buffer writes commit as one undo entry; a mode is not in the undo  */
/* history and never was (zle's `i` is not undoable either), and dispatch has  */
/* to READ the stack back the instant the action returns to decide whether an  */
/* operator is now pending. A staged mode would be a mode dispatch could not   */
/* see.                                                                        */
/* ------------------------------------------------------------------------- */

/* The name of the keymap at the BASE of the stack - the mode. Copy-out, like
 * every other reader. */
int32_t lesh_mode_get(lesh_editor* editor, char* out, size_t capacity, size_t* length_out);

/* A full mode switch: the base is replaced and every sub-mode pushed above it
 * goes with it (state.h gives the argument). LESH_ERR_INVAL for a null or empty
 * name; a name no keymap has is accepted, because `bind` may create it later
 * and a mode naming a table that does not exist yet dispatches to nothing
 * rather than crashing. */
int32_t lesh_mode_set(lesh_editor* editor, const char* name);

/* A sub-mode arrives: visual, operator-pending, the pager, history search. */
int32_t lesh_keymap_push(lesh_editor* editor, const char* name);

/* Pops the topmost sub-mode. LESH_ERR_REFUSED at the base: a mode is not
 * something one can pop out of, only something one swaps. */
int32_t lesh_keymap_pop(lesh_editor* editor);

/* zle's `viopp` slot (#117 decision 6): the action name the verb parked here,
 * empty when none. Dispatch consumes it after the next motion or text object,
 * runs it on the selection region, and clears it - so an action setting this
 * is asking dispatch for the NEXT key's span, not performing anything.
 *
 * Not vi's alone by construction: helix mode never sets it, because its verbs
 * read the always-present selection, and nothing here knows the difference. */
int32_t lesh_pending_operator_get(lesh_editor* editor, char* out, size_t capacity,
                                  size_t* length_out);

/* Sets it; NULL or "" clears it, which is how an operator aborts itself. */
int32_t lesh_pending_operator_set(lesh_editor* editor, const char* action);

/* ------------------------------------------------------------------------- */
/* The numeric argument, pending (#99: counts that multiply)                  */
/*                                                                            */
/* `lesh_invocation::numeric_argument` is what an action RECEIVES. This is how */
/* an action leaves one for the NEXT dispatch, which is the half a digit key   */
/* needs and the half that did not exist. vi's `3dd` and emacs's `M-3 C-f` are */
/* the same mechanism; the name is neither one's.                              */
/*                                                                            */
/* Dispatch reads the pending argument into the invocation and clears it        */
/* BEFORE calling, so an action that sets one is always setting the next key's  */
/* and never re-reading its own.                                               */
/* ------------------------------------------------------------------------- */

int32_t lesh_numeric_argument_set(lesh_editor* editor, int64_t value);
int32_t lesh_numeric_argument_clear(lesh_editor* editor);

/* ------------------------------------------------------------------------- */
/* The kill store (#99 answer 3, spec §6.5)                                   */
/*                                                                            */
/* One keyed store, the unnamed key as default: F-1's kill ring and vi's       */
/* unnamed register are the same object under two paradigms' names. Named      */
/* registers and a clipboard-backed key are both OUT of v1 and both arrive as  */
/* new KEYS rather than as new functions, which is the whole reason the door   */
/* takes a key at all.                                                         */
/*                                                                            */
/* `key` may be NULL, which is the unnamed key - so an action that has never   */
/* heard of registers passes NULL and reaches the one entry v1 has.            */
/* ------------------------------------------------------------------------- */

/* How the text was taken, because `p` has to put it back the same way. A mask,
 * reserved for additive growth: blockwise is a bit, not a third enumerator. */
#define LESH_KILL_CHARWISE 0x0u
#define LESH_KILL_LINEWISE 0x1u

int32_t lesh_kill_set(lesh_editor* editor, const char* key,
                      const char* bytes, size_t length, uint32_t flags);

/* LESH_ERR_NOTFOUND when nothing has ever been put under `key` - which is what
 * `p` with an empty register reads, and is not an error. `flags_out` may be
 * NULL. Copy-out otherwise: `*length_out` is the full length whether or not it
 * fit, and `out` may be NULL with capacity zero to ask the length first. */
int32_t lesh_kill_get(lesh_editor* editor, const char* key,
                      char* out, size_t capacity, size_t* length_out, uint32_t* flags_out);

/* ------------------------------------------------------------------------- */
/* Matching a delimiter pair (#99 answer 2: ONE helper, in neither paradigm's  */
/* idiom)                                                                     */
/*                                                                            */
/* The innermost pair of `open`/`close` that ENCLOSES `at`, as the offsets of  */
/* the opener and one past the closer. Nesting counts when the two differ;     */
/* when they are equal - a quote - the run is paired in order along the line,  */
/* because `'` has no nesting and pretending it does finds the wrong pair.     */
/*                                                                            */
/* `at` sitting ON a delimiter counts as inside its own pair, which is what    */
/* makes `di(` work with the cursor on either parenthesis.                     */
/*                                                                            */
/* LESH_ERR_NOTFOUND when `at` is not enclosed. Both codepoints must be ASCII  */
/* delimiters; anything else is LESH_ERR_INVAL, because a multi-byte pair is a */
/* question nobody has asked and guessing at it would fix the answer.          */
/*                                                                            */
/* Written for vi's `i(`/`a(` and consumed unchanged by helix's `mi(` later -  */
/* which is why it is a geometry question on the editor rather than a piece of */
/* either mode.                                                                */
/* ------------------------------------------------------------------------- */
int32_t lesh_match_pair(lesh_editor* editor, size_t at, uint32_t open, uint32_t close,
                        size_t* start_out, size_t* end_out);

/* ------------------------------------------------------------------------- */
/* The run at a position (#119)                                              */
/*                                                                            */
/* The other half of what a text object needs, and the half a MOTION cannot   */
/* answer: `iw` is not "move somewhere", it is "what is the extent of the     */
/* thing under the cursor". Asked of the editor for the same reason cluster   */
/* stepping is - a binding that worked it out itself would be a second        */
/* word-boundary rule, and the second one is always the one that is wrong.    */
/*                                                                            */
/* The two spans are the two word families the motions already distinguish.   */
/* A run of BLANKS is a run like any other, which is what makes `iw` on a     */
/* space select the whitespace, exactly as vim does, with no special case.    */
/*                                                                            */
/* `[start, end)`, on cluster boundaries, both equal to the buffer's length    */
/* when `at` is past the end. Additive: a new span is a new enumerator.        */
/* ------------------------------------------------------------------------- */
typedef enum lesh_span {
	/* Class-aware: identifier bytes, punctuation, or blanks (vi's `iw`). */
	LESH_SPAN_WORD = 0,
	/* Blank-separated (vi's `iW`). */
	LESH_SPAN_BLANK_WORD = 1
} lesh_span;

int32_t lesh_span_at(lesh_editor* editor, size_t at, lesh_span which,
                     size_t* start_out, size_t* end_out);

/* ------------------------------------------------------------------------- */
/* The last change, for `.` (#99 answer 4)                                    */
/*                                                                            */
/* Dispatch records every buffer change as the KEY SEQUENCE that made it, plus */
/* the mode it was made in and whether it left that mode. See state.h's        */
/* `change_replay` for why the record is keys rather than (action, count) -    */
/* the short version is that `dw` is three dispatches and has no single action */
/* name, and that F-7's `lesh_push_input` already replays keys.                */
/*                                                                            */
/* An action reads the two below and pushes the keys back. Nothing about that  */
/* is vi's: helix's `.` is the same two calls.                                 */
/* ------------------------------------------------------------------------- */

/* The typed text of the last change's key sequence. LESH_ERR_NOTFOUND when
 * nothing has changed the buffer yet. */
int32_t lesh_last_change_keys(lesh_editor* editor, char* out, size_t capacity,
                              size_t* length_out);

/* Whether replaying that sequence HERE would mean what it meant THERE: it was
 * made in the mode the editor is in now, it did not leave that mode, and every
 * key in it is text a push-back can re-type. `*out` is 0 or 1; a 0 is the
 * answer, not an error - pressing `.` after `ciwfoo` is the documented no-op. */
int32_t lesh_last_change_replayable(lesh_editor* editor, int32_t* out);

/* ------------------------------------------------------------------------- */
/* The pager (#138, F-28 to F-30, spec 6.9)                                   */
/*                                                                            */
/* ONE PAGER, THREE CLIENTS: tab completion, history search (F-32) and the     */
/* autosuggestion candidate view are one surface with one keymap, and the      */
/* doors below are how any of them fills it. Nothing here names a client, and  */
/* nothing here generates a candidate - a completer (native or bound) decides  */
/* WHAT to offer and calls these to offer it.                                  */
/*                                                                            */
/* THE PROTOCOL IS THREE CALLS: open a span, add candidates, commit.           */
/*                                                                            */
/*     lesh_pager_open(editor, from, to);                                      */
/*     for each candidate: lesh_pager_add(editor, bytes, length, kind);        */
/*     lesh_pager_commit(editor, &outcome);                                    */
/*                                                                            */
/* COMMIT IS WHERE F-30 LIVES, and that is the decision worth naming. The      */
/* client does not choose between inserting a common prefix and opening a      */
/* list: it hands over what it found and the editor answers, so that every     */
/* client - the completer, the search UI, a plugin - gets the same rule and    */
/* nobody re-implements "is this unambiguous" three times. An unambiguous      */
/* extension INSERTS and the pager never opens; an ambiguous set opens and     */
/* pushes the pager's keymap.                                                  */
/*                                                                            */
/* WRITTEN THROUGH, NOT STAGED, like the mode and the keymap stack and for the */
/* same two reasons: the pager is not in the undo history, and dispatch has to */
/* see the pushed keymap the instant the action returns. The ONE thing that is */
/* staged is the only thing that touches the buffer - the insertion            */
/* `lesh_pager_commit` and `lesh_pager_accept` perform, which stages a write    */
/* exactly as `lesh_buffer_replace` does, so A-12 holds: a candidate becomes   */
/* buffer text through an accepting action, as one undo entry, and by no other */
/* route.                                                                      */
/* ------------------------------------------------------------------------- */

/* What a candidate IS, in `ls -F`'s vocabulary - the marker drawn after it and
 * what follows it into the buffer. The only description v1 has (#137).
 *
 * PLAIN and WORD are both marker-less and differ in the trailer: a history line
 * is PLAIN and nothing follows it, a command name or a plain file is WORD and a
 * space does, because an argument comes next. Appended to, never reordered. */
#define LESH_PAGER_PLAIN 0u
#define LESH_PAGER_WORD 1u
#define LESH_PAGER_DIRECTORY 2u   /* `/`, and a `/` follows it */
#define LESH_PAGER_EXECUTABLE 3u  /* `*` */
#define LESH_PAGER_SYMLINK 4u     /* `@` */

/* What `lesh_pager_commit` did. */
#define LESH_PAGER_NOTHING 0u   /* no candidates; nothing changed */
#define LESH_PAGER_INSERTED 1u  /* F-30: the span grew, the pager stayed closed */
#define LESH_PAGER_OPENED 2u    /* the list is showing and its keymap is pushed */

/* Which axis `lesh_pager_move` walks. A row is the grid's width at the current
 * terminal size, which is why this is a question for the editor and not
 * arithmetic a binding could do. */
#define LESH_PAGER_BY_CANDIDATE 0u
#define LESH_PAGER_BY_ROW 1u

/* Begins a candidate set that will replace `[from, to)`. Discards whatever the
 * pager held, closing it if it was open.
 *
 * The span is the client's statement of what it is completing: the token under
 * the cursor, the whole buffer for a history search, `[cursor, cursor)` for a
 * suggestion. Offsets clamp and snap to cluster boundaries like every other
 * offset the ABI takes. */
int32_t lesh_pager_open(lesh_editor* editor, size_t from, size_t to);

/* Offers one candidate. `bytes` is the BARE text - `bin`, not `bin/` - and
 * `kind` says the rest, so that what is filtered, what is shown and what is
 * inserted are one string. LESH_ERR_INVAL for a kind this ABI does not define
 * or for a null pointer with a non-zero length. */
int32_t lesh_pager_add(lesh_editor* editor, const char* bytes, size_t length,
                       uint32_t kind);

/* F-30's decision point. `*outcome_out` receives one of the LESH_PAGER_NOTHING
 * / _INSERTED / _OPENED constants; it may be NULL if the caller does not care.
 *
 * Committing an empty set closes the pager and answers NOTHING, which is what a
 * completer with no candidates should do and is not an error. */
int32_t lesh_pager_commit(lesh_editor* editor, uint32_t* outcome_out);

/* Stages the selected candidate over the span the pager was opened on and
 * closes it (A-12). LESH_ERR_NOTFOUND when nothing is selected, which is the
 * answer for a closed pager rather than an error. */
int32_t lesh_pager_accept(lesh_editor* editor);

/* Closes it: the keymap it pushed is popped and the candidates are dropped.
 * LESH_OK on a pager that was not open - closing a closed pager is what Escape
 * pressed twice means. */
int32_t lesh_pager_close(lesh_editor* editor);

/* Whether it is showing, how many candidates the filter admits, and which one is
 * selected. Any out pointer may be NULL. */
int32_t lesh_pager_status(lesh_editor* editor, int32_t* open_out, size_t* count_out,
                          size_t* selected_out);

/* The span an accepted candidate would replace. LESH_ERR_NOTFOUND when the
 * pager is not open. */
int32_t lesh_pager_range(lesh_editor* editor, size_t* from_out, size_t* to_out);

/* The selected candidate's text, copied out, and its kind. Copy-out like every
 * other reader: `*length_out` is the full length whether or not it fit, and
 * `out` may be NULL with `capacity` zero. `kind_out` may be NULL.
 * LESH_ERR_NOTFOUND when nothing is selected. */
int32_t lesh_pager_selected(lesh_editor* editor, char* out, size_t capacity,
                            size_t* length_out, uint32_t* kind_out);

/* Moves the selection by `by`, wrapping at both ends so that Tab is a cycle.
 * `axis` is LESH_PAGER_BY_CANDIDATE or LESH_PAGER_BY_ROW. LESH_ERR_NOTFOUND
 * when the pager is not showing. */
int32_t lesh_pager_move(lesh_editor* editor, int64_t by, uint32_t axis);

/* F-29: appends to the filter and narrows the list. The bytes are text the user
 * typed, routed here by the pager keymap's default action; they never reach the
 * buffer. */
int32_t lesh_pager_filter_push(lesh_editor* editor, const char* bytes, size_t length);

/* Drops the filter's last grapheme cluster. LESH_ERR_NOTFOUND - having changed
 * nothing - when the filter was already empty, which is what lets a binding
 * close the pager on the backspace that would have emptied it. */
int32_t lesh_pager_filter_pop(lesh_editor* editor);

/* ------------------------------------------------------------------------- */
/* The prompt (#157, spec 6.10)                                               */
/*                                                                            */
/* `bind`'s SHAPE, A DIFFERENT REGISTRY. The prompt is leshper state, and the  */
/* runtime configures it the way it configures keymaps: across this surface.   */
/* Nothing below is C++-shaped - no element type, no status enum, no template  */
/* - because NG-4 says the Lua binding reuses these verbs unchanged, and a     */
/* verb that took a C++ value would be the one place it could not.            */
/*                                                                            */
/* TWO HALVES, AND THEY ARE ADDRESSED DIFFERENTLY. Registering a module and    */
/* placing elements are REGISTRY operations, long-lived, on `lesh_registry*`.  */
/* Everything a module does while it runs is on `lesh_prompt_context*`, valid  */
/* only for the receiving call - the same handle discipline the editor has,    */
/* and for the same reason.                                                    */
/*                                                                            */
/* EVERY VERB HERE ANSWERS LESH_ERR_NOTFOUND WHEN NO ENGINE IS WIRED UP. A     */
/* non-interactive shell has no prompt engine at all, and a binding that       */
/* configures one should learn that the way it learns there is no completer -  */
/* by being told, not by crashing and not by silently succeeding.              */
/* ------------------------------------------------------------------------- */

/* What a module's return value MEANS - spec 6.10's element status. A module    */
/* answers one of these; a literal and a style answer NEUTRAL, and only a       */
/* module may answer the first three.                                           */
/*                                                                             */
/* A NEGATIVE RETURN - any of the LESH_ERR_* above - reads as OMITTED. A module */
/* that failed contributes nothing and the prompt still draws; there is no      */
/* error channel out of a render, because there is nowhere for an error to go   */
/* that is not the prompt itself. */
#define LESH_PROMPT_OMITTED 0
#define LESH_PROMPT_READY 1
/* Reserved: v1's loop-integration surface is timers only, so nothing resolves
 * a pending element yet and a group does not count one as ready. The
 * completion path arrives with #156. */
#define LESH_PROMPT_PENDING 2
#define LESH_PROMPT_NEUTRAL 3

/* The surfaces v1 has. The right prompt and the transient prompt are #156's and
 * arrive as new constants. */
#define LESH_PROMPT_LEFT 0u
#define LESH_PROMPT_CONTINUATION 1u

/* One module call. Valid only for the duration of that call; stashing it is
 * undefined behaviour, asserted in debug builds. */
typedef struct lesh_prompt_context lesh_prompt_context;

/* A module: reads shell facts through the context, writes its bytes, and
 * answers a LESH_PROMPT_* status.
 *
 * `userdata` is the registration-time context, exactly as an action's is. The
 * PLACEMENT's argument is not a parameter - `lesh_prompt_arg` reads it - because
 * a module is a singleton with free placement (6.10) and the argument belongs to
 * the placement rather than to the registration. */
typedef int32_t (*lesh_prompt_module_fn)(lesh_prompt_context* context, void* userdata);

/* Registers a module under `name`, replacing any existing registration - #101's
 * rule again, so re-sourcing an rc file is idempotent. Names are snake_case;
 * anything else, or a null function, is LESH_ERR_INVAL.
 *
 * Registering does not place anything. `lesh_prompt_add_module` does that, and
 * may do it more than once. */
int32_t lesh_prompt_module_register(lesh_registry* registry,
                                    const char* name,
                                    lesh_prompt_module_fn fn,
                                    void* userdata);

/* True (1) when a module of that name is registered, built-in ones included. */
int32_t lesh_prompt_module_exists(lesh_registry* registry, const char* name, int32_t* out);

/* ---- Inside a module call ---- */

/* Appends bytes to the prompt. They are BYTES, not a string: no NUL is looked
 * for and none is written. */
int32_t lesh_prompt_write(lesh_prompt_context* context, const char* bytes, size_t length);

/* The placement's argument, copied out - `lesh_buffer_get`'s convention exactly.
 * `*length_out` is the full length whether or not it fit, LESH_ERR_TOOSMALL when
 * it did not, and `out` may be NULL with `capacity` zero to ask the length
 * first. An unargued placement answers zero bytes, which is not an error. */
int32_t lesh_prompt_arg(const lesh_prompt_context* context, char* out, size_t capacity,
                        size_t* length_out);

/* The virtual clock, in 10 ms ticks (6.10). A module derives its animation frame
 * from this - `frame = tick / cadence % n` - and stores nothing, which is what
 * makes two spinners spin in phase and #109's replay reproduce a frame
 * sequence. */
int32_t lesh_prompt_tick(const lesh_prompt_context* context, uint64_t* out);

/* Asks to be re-invoked in `ticks` ticks. Zero means the next tick, not "never".
 *
 * The composer keeps ONE deadline list and arms the loop's single prompt timer
 * for the earliest, so nothing runs every 10 ms and a static prompt causes zero
 * idle wakeups. Asking twice in one call keeps the SMALLEST request. */
int32_t lesh_prompt_wake_in(lesh_prompt_context* context, uint64_t ticks);

/* A shell variable's value, copied out like every other reader.
 * LESH_ERR_NOTFOUND when it is unset or when no variable lookup is wired up -
 * which a module should treat the way the built-in `env` does, by omitting. */
int32_t lesh_prompt_variable(const lesh_prompt_context* context, const char* name,
                             char* out, size_t capacity, size_t* length_out);

/* `$?` - the status of the command before this prompt. */
int32_t lesh_prompt_last_status(const lesh_prompt_context* context, int32_t* out);

/* ---- Placing elements ---- */

/* Empties a surface. A cleared surface renders nothing at all, which is a
 * legitimate configuration (an empty continuation prompt) and not a mistake to
 * be corrected into the default. */
int32_t lesh_prompt_clear(lesh_registry* registry, uint32_t surface);

/* Puts a surface back on the built-in default table. */
int32_t lesh_prompt_use_default(lesh_registry* registry, uint32_t surface);

/* Appends a module placement, with `arg` as its argument (NULL for none).
 * LESH_ERR_NOTFOUND when no module of that name is registered.
 *
 * The placement lands INSIDE the open group when there is one, and at top level
 * otherwise. The bytes of `arg` are copied. */
int32_t lesh_prompt_add_module(lesh_registry* registry, uint32_t surface,
                               const char* name, const char* arg);

/* Appends a literal. Bytes, copied; NULL with length zero is a literal that says
 * nothing, which is pointless but not wrong.
 *
 * A TOP-LEVEL LITERAL IS UNCONDITIONAL and a literal inside a group vanishes
 * with the group. That is the whole of the binding rule: explicit grouping,
 * never inferred from adjacency (6.10). */
int32_t lesh_prompt_add_literal(lesh_registry* registry, uint32_t surface,
                                const char* bytes, size_t length);

/* Opens a group: everything added until the matching close is its child, and the
 * group is shown only when a module inside it is ready.
 *
 * LESH_ERR_REFUSED when one is already open. Groups do not nest in v1 across
 * this surface, and refusing is the honest answer - a caller that lost track of
 * its own nesting should hear so rather than have the verbs guess. Nesting
 * arrives with the template language, which has the structure to express it. */
int32_t lesh_prompt_group_open(lesh_registry* registry, uint32_t surface);

/* Closes it. LESH_ERR_REFUSED when none is open. */
int32_t lesh_prompt_group_close(lesh_registry* registry, uint32_t surface);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LESH_LESHPER_ABI_H */
