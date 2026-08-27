#ifndef LESH_UI_PROMPT_ABI_H
#define LESH_UI_PROMPT_ABI_H

/* The prompt ABI (#157, ADR-0008, architecture spec 6.10).
 *
 * SPLIT OUT OF `leshper/abi.h` BY #170, unchanged in form. The verbs below are
 * byte-for-byte the ones that header used to declare - same names, same
 * signatures, same constants - and a binding that included `leshper/abi.h` for
 * them now includes this file beside it. What moved is not the boundary but the
 * side of it the engine lives on: the prompt renders SHELL FACTS (cwd, exit
 * status, jobs, time, the git branch), which is host knowledge, so the engine is
 * `src/ui/prompt/`'s and the editor takes prompt BYTES only. `leshper/abi.h`
 * declares the editor's verbs; this file declares the prompt's; they are two
 * headers over one C boundary and the status codes are shared, which is why this
 * one includes that one rather than restating it.
 *
 * This file is C, and that is the whole point - ADR-0006's outer boundary is
 * flat C, no C++ type crosses it, and NG-4 requires that adding the second
 * binding (Lua, later) need no change here.
 *
 * THE REGISTRY IS STILL THE HANDLE. The configuration verbs take
 * `lesh_registry*`, exactly as they did, because the ABI's one long-lived handle
 * is the registry and a second kind of registry has to hang off it. What changed
 * underneath is that the slot they resolve through is OPAQUE to the editor: the
 * registry carries a `void* host_prompt` the host fills in, and the verbs here
 * are the only code that knows what it points at. leshper does not name
 * `ui::prompt::engine` anywhere.
 *
 * GROWTH IS ADDITIVE ONLY, the same rule the editor half has. New verbs, new
 * LESH_PROMPT_* constants, new surfaces. Signature changes are forbidden.
 *
 * REQUIRED ARGUMENTS ARE POSITIONAL; OPTIONAL ONES TRAVEL IN A STRUCT, passed BY
 * VALUE, when a verb has more than one - `lesh_prompt_module_register_with
 * (registry, name, fn, userdata, options)` is the pattern: `name` and `fn`
 * cannot be forgotten because they have no default and no field to leave unset,
 * and `options` zero-initialized (`{0}` in C, `{}` in C++) means every optional
 * at its default, so a call reads as the configuration it stands for rather
 * than a run of positional NULLs. Each such struct is FROZEN AT BIRTH, the same
 * rule as above applied to structs rather than functions: a field is never
 * appended, because growth already has its one mechanism - a new verb taking a
 * new struct - and a second one, a `sizeof`-tagged version an old caller's
 * struct might be too small for, would tax every call for a case the first
 * already covers for free. This is not the copy-out convention
 * (`char* out, size_t capacity, size_t* length_out`) `leshper/abi.h` documents,
 * which is a different contract for a different problem and is untouched by it.
 *
 * LISTS ARE A POINTER AND A COUNT, NEVER `...`. `lesh_prompt_set_placements
 * (registry, surface, items, count)` is C's variadic - the only one either
 * header uses, because a real one is not `printf`-portable across a binding and
 * would put the calling convention back in the caller's hands. `count == 0` is
 * the empty list, and `items` is unread when it is; otherwise a NULL `items` is
 * LESH_ERR_INVAL, the same rule a required struct pointer would get.
 *
 * THREADING, and it is not advisory. Registration and every configuration verb
 * run in the loop, on the one thread the interactive shell has. A module's call
 * and its context live wherever the render does. Handles are valid only for the duration of the call that received
 * them; stashing one is undefined behaviour, asserted in debug builds.
 */

/* For LESH_OK / LESH_ERR_*, `lesh_registry`, and the copy-out and handle
 * conventions every verb here follows. */
#include "leshper/abi.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* The prompt (#157, spec 6.10)                                               */
/*                                                                            */
/* `bind`'s SHAPE, A DIFFERENT REGISTRY. The prompt is HOST state (#170), and  */
/* the runtime configures it the way it configures keymaps: across this        */
/* surface, through the registry handle it already holds.                      */
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
 * PLACEMENT's type slot is not a parameter - `lesh_prompt_arg` reads it - because
 * a module is a singleton with free placement (6.10) and the argument belongs to
 * the placement rather than to the registration. */
typedef int32_t (*lesh_prompt_module_fn)(lesh_prompt_context* context, void* userdata);

/* A module's TYPE-SLOT GRAMMAR, checked once at set time (6.10).
 *
 * Every built-in owns one - `path` knows its five variants, `env` knows that a
 * variable name is required - and this is the same door for a module that came
 * from a binding. Zero accepts. A POSITIVE value refuses, and `error_out` may
 * carry one short human phrase saying why; `*length_out` is its length whether or
 * not it fit, `lesh_buffer_get`'s convention as everywhere else here.
 *
 * WHY IT IS WORTH A VERB. Without one, a mistyped type slot is not an error at
 * all: the module gets bytes it does not understand at RENDER time, once a
 * prompt, with nowhere to report them - so the user sees a segment silently doing
 * the wrong thing rather than a message naming the byte they got wrong. A
 * validator moves that discovery to the moment the template was written. */
typedef int32_t (*lesh_prompt_validate_fn)(const char* type, size_t length, void* userdata,
                                           char* error_out, size_t capacity, size_t* length_out);

/* Registers a module under `name`, replacing any existing registration - #101's
 * rule again, so re-sourcing an rc file is idempotent. Names are snake_case;
 * anything else, or a null function, is LESH_ERR_INVAL.
 *
 * A module registered this way ACCEPTS ANY TYPE SLOT: it has no grammar this side
 * can check, so nothing is refused at set time and the bytes arrive at
 * `lesh_prompt_arg` as written. Use `lesh_prompt_module_register_with` below to
 * own a grammar.
 *
 * KEPT AS FOUR PLAIN ARGUMENTS, deliberately, alongside the options-taking form
 * below: this is the common case, every one of its arguments is required, and an
 * options value for zero optionals is ceremony a caller should not have to
 * write. Registering does not place anything. `lesh_prompt_set_placements` does
 * that, and may place the same module more than once. */
int32_t lesh_prompt_module_register(lesh_registry* registry,
                                    const char* name,
                                    lesh_prompt_module_fn fn,
                                    void* userdata);

/* The same registration, its one optional in a struct passed BY VALUE - the
 * required-positional/optional-struct convention the top of this file records,
 * applied here: `registry`, `name`, `fn` and `userdata` are required and stay
 * positional; `validate` is the only optional, so it alone moves into
 * `lesh_prompt_module_options`, frozen the same way every such struct is - see
 * the top block for why there is no size field and none is coming.
 *
 * `options.validate` may be NULL, which makes this exactly the plain form above -
 * it exists for the validator and for whatever optional the future adds as a new
 * verb rather than a new field. */
typedef struct lesh_prompt_module_options {
	lesh_prompt_validate_fn validate;   /* NULL = accepts any type slot */
} lesh_prompt_module_options;

int32_t lesh_prompt_module_register_with(lesh_registry* registry,
                                         const char* name,
                                         lesh_prompt_module_fn fn,
                                         void* userdata,
                                         lesh_prompt_module_options options);

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

/* AN OPTIONS STRUCT, per `lesh_prompt_module_options` above: frozen, by value,
 * every field optional. NULL or "" on any field means its default - unstyled,
 * the module's own type, no affix. */
typedef struct lesh_prompt_options {
	const char* style;    /* NULL or "" = unstyled */
	const char* type;     /* NULL or "" = the module's default */
	const char* prefix;   /* NULL or "" = none */
	const char* postfix;  /* NULL or "" = none */
} lesh_prompt_options;

/* ONE ELEMENT OF A SURFACE - a module placement or a group, the same two
 * shapes `(…)` and `{…}` are in a template, chosen by which of `module` and
 * `children` is set. NULL and "" answer alike for `module`, the convention
 * every optional string field in this ABI already keeps:
 *
 *   MODULE  no `children` (NULL, or a zero `child_count`), `module` set - a
 *           placement. `module` is the module's name, or the keyword "literal"
 *           for a standalone styled literal - see `lesh_prompt_set_placements`
 *           below for its rules.
 *   GROUP   no `module` (NULL or ""), `children` set to a non-empty array -
 *           everything in it is this group's child, shown only when one of
 *           them is (`lesh_prompt_set`'s `(…)`, exactly). Groups NEST to any
 *           depth: a child's own `children` makes it a group in turn.
 *   NEITHER OR BOTH set is LESH_ERR_INVAL - see `lesh_prompt_set_placements`.
 *
 * `options` is unused on a group - a group paints nothing of its own, only its
 * children do, the same as `(…)` in a template. */
typedef struct lesh_prompt_placement {
	const char* module;
	lesh_prompt_options options;
	const struct lesh_prompt_placement* children;
	size_t child_count;
} lesh_prompt_placement;

/* THE WHOLE SURFACE, IN ONE CALL - atomic, like `lesh_prompt_set`, and built
 * from the same items the template's elements are: this replaces the single
 * placement verb and the two group verbs that used to stand here. A verb
 * stream could not say which group a close belonged to and so could not nest;
 * a TREE says it directly, which is why groups here nest to any depth where
 * the old `_group_open`/`_group_close` pair refused a second one.
 *
 *   lesh_prompt_placement branch[] = {
 *       { "literal", { .style = "magenta", .prefix = " on " } },
 *       { "git",     { .style = "magenta" } },
 *   };
 *   lesh_prompt_placement left[] = {
 *       { "path",    { .style = "cyan", .type = "s" } },
 *       { .children = branch, .child_count = 2 },
 *       { "literal", { .prefix = "> " } },
 *   };
 *   lesh_prompt_set_placements(reg, LESH_PROMPT_LEFT, left, 3);   // C++
 *
 *   lesh_prompt_set_placements(reg, LESH_PROMPT_LEFT,
 *       (lesh_prompt_placement[]){
 *           { .module = "path", .options = { .style = "cyan", .type = "s" } },
 *       }, 1);   // C99 compound literal
 *
 * builds `{path:cyan:s}( on {git:magenta})> ` without a byte of template text
 * ever existing - `lesh_prompt_text` on a surface built this way answers "",
 * the same rule a `place`-built surface always followed.
 *
 * A Lua binding (LuaJIT FFI) needs nothing this header does not already say:
 *
 *     ffi.cdef[[ ...the four typedefs/declarations above, verbatim... ]]
 *     local branch = ffi.new("lesh_prompt_placement[2]", {
 *       { "literal", { style = "magenta", prefix = " on " } },
 *       { "git",     { style = "magenta" } },
 *     })
 *     local left = ffi.new("lesh_prompt_placement[3]", {
 *       { "path",    { style = "cyan", type = "s" } },
 *       { children = branch, child_count = 2 },
 *       { "literal", { prefix = "> " } },
 *     })
 *     assert(ffi.C.lesh_prompt_set_placements(reg, LESH_PROMPT_LEFT, left, 3) == 0)
 *
 * builds `{path:cyan:s}( on {git:magenta})> `. Everything is copied during the
 * call, so no Lua string or cdata has to outlive it (the copy-in rule above) -
 * NG-4's promise kept without one binding-specific entry point.
 *
 * `module` RESOLVES THROUGH THE IDENTICAL RULE THE TEMPLATE PARSER USES for a
 * placement's name - "literal" checked, and shadowing a registered module of
 * that name, before the module table; a non-empty `type` on it refused; refused
 * again with neither affix set. ONE BUILDER, TWO FRONT DOORS: this verb and
 * `lesh_prompt_set` both finish by resolving a name, a style and a type and
 * handing them to the identical engine builder `lesh_prompt_set` uses, so a
 * tree that says what a template says builds the identical program.
 *
 * EVERYTHING IS VALIDATED BEFORE ANYTHING IS APPLIED, recursively, depth first:
 * every module resolves, every style and every type parses, every group has at
 * least one child and no item has both a module and children. The first item
 * that fails is the whole answer; nothing built past it is kept, and the
 * surface that was standing is still standing - `lesh_prompt_set`'s promise,
 * scaled up from one string to a tree.
 *
 * `count == 0` clears the surface, `items` unread - the same surface
 * `lesh_prompt_clear` leaves. Otherwise `items == NULL` is LESH_ERR_INVAL.
 *
 * FOUR ANSWERS:
 *   LESH_OK            - set.
 *   LESH_ERR_INVAL     - a malformed call, or a structural error in the tree:
 *                        an item with neither `module` nor `children`, an item
 *                        with BOTH, or a group with a zero `child_count`.
 *   LESH_ERR_NOTFOUND  - the first unresolved module name (and no engine on the
 *                        registry).
 *   1                  - the first style, type or "literal" refusal - one
 *                        status for all three, because the caller already knows
 *                        which item it passed and what was in it; there is no
 *                        message channel here, unlike `lesh_prompt_set`, whose
 *                        text a human typed and needs one. */
int32_t lesh_prompt_set_placements(lesh_registry* registry, uint32_t surface,
                                   const lesh_prompt_placement* items, size_t count);

/* Sets a surface from a TEMPLATE - `{module:style:type:prefix:postfix}`, `(…)`
 * groups, `\:`-family escapes - replacing everything on it.
 *
 * PARSED ONCE, HERE, AND SWAPPED WHOLE. A template that will not parse changes
 * nothing at all: the prompt that was standing is still standing, and there is no
 * half-applied configuration to undo. That is the one thing this verb has that
 * the element verbs above cannot give a caller holding a single string.
 *
 * THREE ANSWERS, AND THE MIDDLE ONE IS THE POINT:
 *   LESH_OK             - set. `*error_length_out` is zero.
 *   1                   - REFUSED. `error_out` holds one human sentence naming
 *                         the byte that was wrong ("unknown module 'gti' at byte
 *                         7"), and `*error_length_out` its length.
 *   LESH_ERR_TOOSMALL   - refused, and the sentence did not fit.
 *                         `*error_length_out` is its full length; ask again with
 *                         room. `error_out` may be NULL with a zero capacity to
 *                         ask the length first, exactly as every other reader
 *                         here works.
 * Anything negative other than TOOSMALL is a malformed call or no engine.
 * `error_length_out` may not be NULL - a caller that does not want the message
 * still has to be told there was one. */
int32_t lesh_prompt_set(lesh_registry* registry, uint32_t surface,
                        const char* text, size_t length,
                        char* error_out, size_t error_capacity, size_t* error_length_out);

/* The SOURCE the surface was last set from, copied out - the template text
 * itself, never a rendering of it and never a walk of the elements back into a
 * spelling.
 *
 * A surface on the shipped default answers the default's own template; one built
 * by `lesh_prompt_set_placements` answers zero bytes, because a prompt assembled
 * from a tree has no template string and inventing one would put the element
 * vocabulary on the caller's side of this boundary. */
int32_t lesh_prompt_text(lesh_registry* registry, uint32_t surface,
                         char* out, size_t capacity, size_t* length_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LESH_UI_PROMPT_ABI_H */
