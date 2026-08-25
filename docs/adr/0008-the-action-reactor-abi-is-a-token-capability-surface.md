# ADR-0008: The action/reactor ABI is a token-capability surface

**Status:** Accepted
**Date:** 2026-08-25

## Context

NG-4 requires the action/reactor registration ABI to be language-neutral from
v1: the lesh binding (editor state as shell variables, F-14) and the later Lua
binding (state as a table API) are both frontends to one ABI, and adding the
second must need no ABI change. ADR-0006 fixed the outer boundary as flat C —
no C++ types cross, no arena pointers are handed out.

Two problems have no C type to lean on:

1. N-4 demands that applying a stale-generation reactor result to a newer
   buffer be **structurally impossible** — but C has no affine types or borrows
   to express "this result belongs to that snapshot".
2. Reactors produce decorations and proposals as ranges into a buffer they do
   not own, across a boundary that forbids sharing pointers.

The registry could also have been an internal C++ interface with a C shim added
when extensions land. That shim would be a second path with zero clients until
Lua arrives — the drift A-11 exists to prevent, restated at the registry.

## Decision

**The registry stores the language-neutral C shape, and results enter only
through a generation-bound request token.**

- One registry, C-shaped entries — name, function pointer, context pointer.
  Built-in actions and reactors are its first clients, registered through the
  identical signatures a binding would use. There is no native side door.
- An action is `int32_t fn(lesh_editor*, const lesh_invocation*, void* userdata)`.
  State access is pure copy-in/copy-out accessors over the opaque handle;
  writes stage and the loop commits them atomically on return (one undo entry,
  one generation bump, per #92). Loop outcomes — accept, cancel, exit,
  recursive edit — are requested by capability call, never performed; the
  return value is status only.
- A reactor computes on a worker against an opaque `lesh_request*` token
  carrying the snapshot (buffer, cursor, selection) and its generation.
  **Emission functions exist only on the token; no function anywhere in the
  ABI writes decorations or proposals against the editor.** The loop applies a
  completed token's batch only if its generation is still current. Staleness is
  therefore unexpressible: "apply" is not a thing anyone but the loop can do,
  and you cannot submit except through a token you were handed and cannot mint.
- Emit calls copy plain structs at the call site — the reactor's arena dies
  with its request (per #90) and nothing it allocated is retained. The emitting
  reactor is the decoration namespace. Spans carry interned semantic style ids;
  themes map ids to attributes at render.
- One `int32_t` status space: 0 ok, negatives owned by the ABI spec, positives
  pass through as the binding's domain status (a user action's exit status
  crosses without the ABI knowing shell semantics).

The acceptance test for the design was written before any code: the Lua
binding, sketched against this surface, needs two C trampolines and zero new
entry points. It is recorded on
[#93](https://github.com/nanov/lesh/issues/93).

## Consequences

- The NG-4 property is structural. A binding that would need an ABI change
  cannot be written, because there is no second path to diverge from.
- N-4's "type-level, not convention" lands as capability, not type: the token
  is the only mint for results, and the loop is the only applier. C never had
  to grow a type for it.
- Native built-ins pay copy-accessor friction — a line-sized copy per access at
  keystroke rate, measured as noise against N-1's 1 ms budget. In exchange the
  ABI cannot rot unexercised.
- Growth is additive only: new capability functions, new event-mask bits, new
  emit kinds. Signature changes are forbidden; the two recorded future doors
  (syntax queries on the token, provider access per #94) are both new
  functions.
- Handles are valid only for the duration of the call that received them —
  stashing one is undefined behavior, asserted in debug builds. Actions,
  registration, and lookup run on the loop thread only; reactor compute and its
  token live on the worker's call, cooperatively cancelled by a superseded
  poll.
