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
	LESH_MOTION_BUFFER_END = 7
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LESH_LESHPER_ABI_H */
