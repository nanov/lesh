# lesh

A POSIX-conformant shell built as a systems-craft exercise. This glossary fixes the
words the project argues in. Where POSIX defines a term, POSIX's meaning wins.

Terms are marked **[floor]** when they belong to the POSIX language lesh conforms to,
**[curated]** when they belong to the zsh-inspired layer on top, and **[lesh]** when
they are the project's own vocabulary.

## The shell language

### Lexical

**Token** _[floor]_:
The unit the lexer emits. Every token is either an operator or a word.
_Avoid_: using "word" for this. The codebase currently has no token concept at all.

**Word** _[floor]_:
A token that is not an operator. A word exists **before** expansion.
_Avoid_: using "word" for an argument, a field, a fragment of a word, a parameter
name, an array element, or the unsplit body of a brace list. All six of those appear
in the current code.

**Operator** _[floor]_:
A token of shell punctuation. **Control operators** (`|`, `;`, `&`, `&&`, `||`,
`(`, `)`, newline) separate and combine commands; **redirection operators**
(`<`, `>`, `>>`, `<<`, and friends) introduce redirections.
_Avoid_: separator, special char.

**Name** _[floor]_:
An identifier: `[A-Za-z_][A-Za-z0-9_]*`. What a variable is denoted by.
_Avoid_: var name, key.

**Blank** _[floor]_:
A space or tab. What separates words in the absence of an operator.

### Parameters

**Parameter** _[floor]_:
An entity that stores a value. Covers variables, positional parameters (`$1`), and
special parameters (`$?`, `$#`, `$@`).
_Avoid_: using "parameter" for an argument of a command. The current code does this.

**Variable** _[floor]_:
A parameter denoted by a name. Scalars are variables too, not only maps and arrays.
_Avoid_: env, which names only the exported subset.

**Readonly variable** _[floor]_:
A variable that assignment and `unset` both refuse. The flag belongs to the
variable rather than to a separate list, so a name can be readonly while **unset**:
`readonly x` then `x=1` fails, and `${x-unset}` still says unset.
_Avoid_: constant, immutable, frozen.

**Subscript** _[curated]_:
What sits between the brackets in `$var[key]` — the selector into a map or an array.
_Avoid_: key, index. The code currently uses both, for opposite halves of the same
call.

**Map** _[curated]_:
A variable whose values are addressed by string subscript. zsh and bash call this an
associative array; POSIX has no equivalent. lesh keeps the shorter name.
_Avoid_: associative array, dictionary, hash.

**Array** _[curated]_:
A variable whose values are addressed by integer subscript.

### Expansion

**Expansion** _[floor]_:
The transformation of a word into zero or more fields. POSIX defines the sequence:
tilde expansion, parameter expansion, command substitution, arithmetic expansion,
field splitting, pathname expansion, quote removal.
_Avoid_: using "expansion" for alias substitution, which POSIX explicitly excludes.
The current code uses the word exclusively for aliases.

**Field** _[floor]_:
A unit produced by field splitting. One word yields zero, one, or many fields.
_Avoid_: word, argument.

**Field splitting** _[floor]_:
Splitting the result of an unquoted expansion on the characters in `IFS`. A
**separator** has a SHAPE rather than being one byte: IFS white space, then at most
one non-white-space IFS character, then more IFS white space. Two separators in a
row therefore leave an EMPTY field between them, which is the difference between
this and dropping IFS bytes.
_Avoid_: word splitting, tokenizing. Also avoid calling the run between two fields
a "delimiter" when it may be several characters — separator is the POSIX word.

**Parameter expansion** _[floor]_:
The `${...}` family, including the bare `$name` form.

**Command substitution** _[floor]_:
`$(...)` — replacing a command with its standard output, trailing newlines removed.
_Avoid_: subshell, which names an execution environment, not this.

**Pathname expansion** _[floor]_:
Replacing a word containing `*`, `?`, or `[...]` with matching pathnames.
_Avoid_: globbing in prose, though "glob pattern" is fine for the pattern itself.

**Brace expansion** _[curated]_:
`{a,b,c}` producing one word per alternative. Not a POSIX feature.
_Avoid_: list expansion, which is what `README.md` currently calls it.

**Alias substitution** _[floor]_:
Replacing a command word with an alias's definition. Happens at read time and is
**not** an expansion.
_Avoid_: alias expansion.

**Lazy alias** _[curated]_:
An alias whose chain is resolved at use rather than pre-flattened at definition.

### Commands

**Simple command** _[floor]_:
A command name with its arguments, optionally preceded by assignments and mixed with
redirections.

**Command name** _[floor]_:
The word that names what to run. Currently unnamed in the code, where it is only
`children[0]`.

**Argument** _[floor]_:
A field passed to a command after the command name.
_Avoid_: word, child, parameter, command part — all four are used for this today.

**Compound command** _[floor]_:
`if`, `while`, `until`, `for`, `case`, a subshell `(...)`, or a brace group `{...}`.

**Pipeline** _[floor]_:
Commands joined by `|`, output to input.
_Avoid_: using "pipeline" for lesh's own lexer/parser/executor architecture — see
**Syntax layer** below. Also avoid "pipe", which names the `|` operator and the syscall.

**List** _[floor]_:
Pipelines joined by `;`, `&`, `&&`, or `||`. An **AND-OR list** is the `&&`/`||` case.

**Subshell** _[floor]_:
A separate execution environment whose changes do not affect its parent.
_Avoid_: using this for command substitution or for its output buffer.

**Special builtin** _[floor]_:
One of the POSIX-designated builtins (`break`, `:`, `continue`, `.`, `eval`, `exec`,
`exit`, `export`, `readonly`, `return`, `set`, `shift`, `times`, `trap`, `unset`)
whose failure exits the shell and whose assignments persist. Distinct from a
**regular builtin**, where neither is true.

**Extension builtin** _[lesh]_:
A builtin lesh ships that POSIX does not name, contributed by **leshnici** rather
than by the runtime and installed at startup instead of compiled into the
registry. Always a **regular builtin**, never a special one, and always reported
by `command -v` as its own bare name. It lives in a SECOND table — `runtime`
declares the row (`extension_builtin`) and holds a borrowed view of it on shell
state; `src/main.cpp` installs leshnici's; the core's two `constexpr` tables and
the `static_assert` between them are untouched. The command search reaches it
after special builtins, functions and core regular builtins, and only when
`set -o leshnici` is on — which defaults to on interactively and off in every
script, so a script sees the POSIX search unless it asks otherwise. A name that
collides with a core builtin is refused at install time, whole and reported once;
core always wins.
_Avoid_: calling one a regular builtin without qualification when the point is
where it came from; expecting one in a script that has not set the option.

**Exit status** _[floor]_:
The value a command reports on completion, readable as `$?`.

**Here-document** _[floor]_:
A redirection whose input is taken from the lines following the command.

## The implementation

**Layering** _[lesh]_:
One directory, one namespace, one target: `src/X/Y.cpp` holds `lesh::X::Y` and is
compiled into `lesh_X`. `src/substrate/` is the one flat exception — it is
`lesh::` with no second component, because it is what everything else is built
out of. The order is
`substrate ← {syntax ← runtime, leshper} ← ui ← leshnici ← lesh`, and each target
names every dependency it uses rather than inheriting one: a layer violation is a
link failure, not a review comment. Two rules are load-bearing and worth saying
out loud — **leshper never includes `ui/` or `runtime/`** (the editor declares
shapes; the host fills them in over shell state), and `lesh_ui` is the ONE
library that links both halves. The editor/host line runs between the last two:
`loop`, `tty`, `workers` and `shell_actor` are `src/ui/` (#168 Phase A), so
nothing under `src/leshper/` holds a thread, a descriptor or a clock; the
highlighter, the autosuggester, the history search and the completion sources are
`src/ui/` too (#168 Phase B), so **`lesh_leshper` links `lesh_substrate` and
nothing else** — not `lesh_syntax` either, because the editor neither parses nor
lexes. Highlighting and suggesting are runtime, syntax and history KNOWLEDGE; the
editor only colours regions. The prompt engine went the same way (#170) — it
renders shell facts, so it is `src/ui/prompt/` — and the C ABI split at that
seam: `leshper/abi.h` declares the editor's verbs, `ui/prompt/abi.h` the
`lesh_prompt_*` ones, and the registry carries the engine as an opaque `void*`
so that no header under `src/leshper/` names `ui::prompt::engine`.
_Avoid_: reaching for a symbol across a link the target does not declare; adding
a dependency by relying on transitivity.

**Substrate** _[lesh]_:
The allocation and container layer everything else is built on: the arena and the
stack-first hybrid containers. Depends on nothing.

**Builtin registry** _[lesh]_:
The one table naming every builtin, its kind, and where its implementation lives.
Classification and dispatch both read it, so a name cannot be classified without
being implemented — the disagreement that had `test 1 = 2` reporting success.
_Avoid_: builtin table, which named either of the two tables that disagreed.

**Arena** _[lesh]_:
A pool that hands out memory and reclaims it in one shot, rather than per object.

**Lexer** _[lesh]_:
Turns input into tokens. Owns no memory and mutates nothing, so it can run on every
keystroke over a buffer it does not own.

**Parser** _[lesh]_:
Turns tokens into a tree. Always returns a tree; invalid input yields error nodes.

**Expander** _[lesh]_:
Turns one word plus the shell state into fields. Called by the executor per command,
at execution time. Re-entrant — a parameter default, an assignment value and
arithmetic's inner text all re-enter it — and therefore depth-limited.

Still the last line of defence against a **defect** the syntax layer could not see,
but a narrower one than #48 recorded: the word scan followed nothing but braces
then, so an unterminated construct inside `${x-...}` was only lexed when the
default was expanded. The scan now follows quoting and nesting (#42), so the word
itself carries the defect and the parser refuses it. What is left to the expander is
text no word scan ever saw: `eval`, `.`, an assignment value and arithmetic's inner
text.

**Expansion context** _[lesh]_:
The properties that decide how a piece of text expands, held together rather than
inferred from one flag. POSIX applies its treatments independently and one bool
standing for several of them has been wrong three times: field splitting, the
double-quote backslash rules, whether a FIELD LIST is being produced (so `"$@"` may
make more than one), whether the text is itself the RESULT of an expansion (so its
literal blanks separate), whether it is a PATTERN (so quoting becomes escapes rather
than nothing), and whether it is the interior of `${...}` (so `\}` escapes). The
lexing **mode** is a sixth, and separate again.
_Avoid_: "quoted", which named two of these at once and then three.

**Executor** _[lesh]_:
Runs a tree. An interface, so the back end stays replaceable.

**Syntax layer** _[lesh]_:
The lexer and parser together — what `src/syntax/` holds. Use this, or name
the components individually, when you would otherwise reach for "pipeline" —
that word belongs to POSIX.
_Avoid_: front end.

**Language** _[lesh]_:
What the syntax layer reads — today the shell language. The axis along which
a second language would someday be added.
_Avoid_: front end, dialect.

**UI** _[lesh]_:
Two senses, and both are in use. The layer as a whole is everything an
interactive shell has that `lesh -c` does not — leshper, prompts, rendering.
`src/ui/` is the narrower thing: the **host**. It owns the poll loop, the
terminal, the timers, the workers, the shell handoff, and the session that runs
an interactive shell to its end; it drives leshper by sending **events** and
performing **effects**, and by nothing else. It also holds every piece of
KNOWLEDGE the editing experience needs (#168 Phase B): the highlighter and the
autosuggester reactors (`ui/reactors.h`), the history searcher and its store
adapters (`ui/history_search.h`, `history_store_source`), the completion sources
and the token-finding lex behind Tab (`ui/completion.h`), the shell's own tables
(`ui/shell_knowledge.h`, `shell_state_knowledge` over shell state), and the one
object that fills leshper's one door (`ui/editor_host.h` — `leshper::host`). The
**prompt** engine is its state too (#170): `src/ui/prompt/` is the engine, the
seven built-in modules, the template language and the `lesh_prompt_*` ABI
(`ui/prompt/abi.h`), because what a prompt renders is shell facts. The prompt and
binding consoles run in the other direction. leshper is the editor BENEATH it;
leshnici the extension set ABOVE it. The shell proper is everything else.

**Front end** _[retired]_:
A word that carried three meanings at once and now carries none. What it
named: the lexer and parser together is the **syntax layer**; the interactive
side is the **UI**; the legacy-or-next axis is gone entirely - #28 deleted
`src/legacy/`, the `LESH_FRONTEND` variable and `spec_run.py --frontend` with
it, so there is one shell and nothing to select between.
_Avoid_: the word itself, in every sense.

**Shell state** _[lesh]_:
Variables, scopes, functions, aliases, options, working directory, and `$?`. Read by
both the expander and the executor.

**Logical working directory** _[floor]_:
Where the shell BELIEVES it is: `$PWD`, maintained by `cd` out of the pathnames it
was given, with `.` and `..` resolved lexically. A symlink followed on the way in is
unfollowed on the way out, so `cd link` then `cd ..` returns to the link's parent
rather than the target's. What `cd -L` - the default - maintains, and what `pwd`
prints.
_Avoid_: calling it a cache of the physical one, or stale. It is not an optimisation
of anything: the two name different directories deliberately, and #24 chose this one.

**Physical working directory** _[floor]_:
The directory the KERNEL says the process is in, every symlink resolved - what
`getcwd` answers and what any pathname handed to another process resolves against.
What `cd -P` reads back into `$PWD`. On macOS `/tmp` is a symlink to `/private/tmp`,
which is why the difference is visible in one line there and invisible on a system
where it is not.

**Span** _[lesh]_:
A token or node's location in the source, carried so errors and highlighting can
point at it.

**Error node** _[lesh]_:
A tree node standing in for input the parser could not make sense of. What makes the
parser total. Not the only way a tree records a problem — see **Defect**.

**Defect** _[lesh]_:
Something wrong with a node, recorded in that node's own error field. Distinct from
an error node, which is a defect that has no other shape: a word whose quote was
never closed is still a word, so the defect travels on the word and the kind is left
alone. "Does this tree hold a defect" is the question an executor asks before running
it, and asking it of the kind alone is how an unterminated quote came to execute.

**Incomplete** _[lesh]_:
Said of input the lexer ran out of mid-construct, so more of it would help.
Orthogonal to a defect, not a milder form of one: an unterminated quote is both, and
a trailing backslash is incomplete without being defective. A reader answers
incomplete with a continuation prompt; an executor holding the whole input has
nothing to continue and answers the defect with a diagnostic.

**leshper** _[lesh]_:
lesh Prompt Editing & Reading — the shell's own line editor, the largest piece
of the interactive layer and the one BENEATH `src/ui/`. A pure editor:
`step(state, event, now) -> effects`, with **no thread, fd, poll, timer or
mailbox** anywhere in it, driven by a **host**. Integrated: linked into the lesh
binary, not shipped as a library anyone else builds against, and it never sees
the shell — `lesh_leshper` links `lesh_substrate` and nothing else, so it declares
shapes that `src/ui/` fills in. **It knows nothing**: not the shell, not syntax,
not history, not git (#168 Phase B), and not the prompt (#170) — it takes prompt
BYTES and a pen and never learns what they say. What is left is the buffer, the
keymap and actions, vi, undo and kill, decoding, layout and blit, the pager and
selection UI, the **decoration**/**proposal** vocabulary, the style grammar and
the theme, and the ABI that registers actions and reactors (`leshper/abi.h`; the
`lesh_prompt_*` verbs are `ui/prompt/abi.h`'s). One door out to what it cannot
know: `leshper::host` (`host.h`).
_Avoid_: LLE, the retired earlier name; the reader; readline; calling the
session, the loop, the reactors, the adapters or the prompt engine "leshper" —
those are `src/ui/`.

**leshnici** _[lesh]_:
The shipped extension set: what lesh ships on top of the host and the runtime but
does not owe either — **prompt modules** and **extension builtins**. A layer ABOVE
both (`lesh_leshnici` links `lesh_ui` and `lesh_runtime`, never the other
way; what a prompt module installs into is `ui::prompt::engine`), and each half arrives by being INSTALLED rather than by being compiled in:
`src/main.cpp` hands `install_prompt_modules` to the session as its extension
hook and calls `install_builtins` on shell state before either mode runs, so an
engine or a shell built anywhere else — a test's, a tool's — is the bare one.
`git` is the first prompt module, there rather than among leshper's built-ins
because it reads a filesystem where every built-in is a pure function of the
facts struct. `ls`, `cat`, `head` and `tail` are the first builtins, behind
`set -o leshnici` — on by default when the shell is interactive, off in every
script — so the POSIX command search a script sees is unchanged.
`coreutils/` and `prompt/` are its two halves (`lesh::leshnici::coreutils`,
`::prompt`); `leshnici.h` is the visible list.
_Avoid_: calling a leshnici prompt module a built-in; a template naming one on an
engine without leshnici is refused as an unknown module, like any other name.
Assuming a leshnici builtin runs in a script: it does not until the option is on.

**Prompt** _[lesh]_:
What the shell draws before the line it is about to read, and the engine that
draws it (spec §6.10). HOST STATE, in `src/ui/prompt/` (#170): a prompt is a
function of shell facts — cwd, exit status, job count, elapsed time, the git
branch — and those are the host's to know, so `ui::prompt::engine` lives beside
the session that owns them. leshper takes the rendered BYTES and a pen
(`loop_options::prompt` and `.continuation`) and nothing more. A surface (`left`,
`continuation`) is a list of placements, each a literal, a style or a **module**;
modules are singletons with free placement, and the seven built-in ones are pure
functions of the facts struct while `git` is **leshnici**'s. Configuration is the
`prompt` builtin's template language, or the `lesh_prompt_*` verbs across
`ui/prompt/abi.h`, which resolve the engine through an opaque slot on the
registry. Recalculation is BY CAUSE: a tick re-renders only the item whose
deadline came up.
_Avoid_: calling the engine leshper's; calling `git` a built-in; "PS1" for a
surface.

**Widget** _[lesh]_:
Reserved for a future UI surface a plugin can draw into — panels, pickers,
floating surfaces. Nothing ships under this name yet.
_Avoid_: using "widget" for an **action**, which is zsh's usage and was this
glossary's until #84.

**Action** _[lesh]_:
A named, invocable, rebindable unit of editing behaviour. Built-in actions are
native code; user actions are lesh functions. The only thing that mutates
buffer, cursor or selection, and it runs synchronously, only when invoked.
_Avoid_: widget, zsh's word for this — reserved here for future UI surfaces.

**Mode** _[lesh]_:
The named keymap at the base of the stack — emacs, vi_insert, vi_command.
Switching mode swaps the base; **sub-modes** (visual, operator-pending, the
pager) are pushes above it and never change the mode. Not an enum anywhere:
adding a mode is registering keymaps and actions.

**Reactor** _[lesh]_:
A subscriber to editor state-change events that computes derived state —
decorations and proposals — asynchronously on a worker. Never mutates buffer,
cursor or selection. The highlighter and the autosuggester are reactors, and both
are the **host**'s (`src/ui/reactors.cpp`, #168 Phase B) because what they compute
is knowledge; they reach the editor through the ABI, the way a Lua reactor would.
What leshper owns is the mechanism — the reactor table, `apply_batch`, the
generation drop rule — and the vocabulary the answers arrive in.
_Avoid_: calling a reactor's work an effect; an **effect** is synchronous and
the loop's own.

**Binding** _[lesh]_:
A language's frontend to the action/reactor ABI: the lesh binding exposes
editor state as shell variables, the Lua binding (later) as a table API. A
binding wraps the one C surface; the ABI itself belongs to no binding.

**Request token** _[lesh]_:
The opaque, generation-bound handle a reactor or provider computes against.
The only mint for results: decorations, proposals and candidates are emitted
through it or not at all, which is what makes applying a stale result
impossible rather than checked.

**Provider** _[lesh]_:
A named lesh subsystem the interactive layer pulls from on demand — the syntax
layer, completer, history store, prompt. The syntax layer is sealed; the other
three are **override points**. All four are the **host**'s (#168 Phase B): they
are named in `ui::provider_bundle` and assembled by the session, and the editor
does not know any of them by name. What the editor knows is one door,
`leshper::host`, which the session fills with an object holding the completer and
the shell's tables; the history store and the prompt never cross that door at all
— the reactors and the prompt engine reach them on the host's own side.
_Avoid_: calling a provider a reactor; reactors are pushed events, providers
are asked. Saying leshper "pulls" a provider — it asks its host, or nothing.

**Override point** _[lesh]_:
A provider whose default implementation user code may replace or wrap, by
supplying it in the bundle the session is built with. Native implementations run
as trusted threads; user-supplied ones run as killable spawned processes, so a
broken override cannot wedge the editor. The replacement happens entirely on the
host's side of the boundary — the editor sees the same one door whichever
implementation is behind it.

**Selection** _[lesh]_:
An anchor position plus an active flag; **the cursor is the head**. The
region is derived — exclusive half-open between anchor and cursor, endpoints
on grapheme boundaries — and shape (charwise, linewise, blockwise) belongs
to the acting mode's projection, never to the type. Emacs, vi visual and
helix all project onto this one pair.
_Avoid_: region as a stored range; mark as a separate synchronized position.

**Proposal** _[lesh]_:
Derived state offered to the user — an autosuggestion, a candidate list. It
becomes a buffer edit only through an accepting action; it never auto-applies.

**Decoration** _[lesh]_:
A namespaced annotation anchored to buffer positions: a highlight span or
virtual text. Survives edits. The one system highlighting and autosuggestions
both render through. Normalized at APPLICATION time, not at render time — nested
spans are resolved into one sorted, disjoint list so the layout walk carries no
scratch — and the normalization itself allocates nothing in the steady state
(#168 Phase B): its sweep buffers are held, not built, and a copy of them starts
empty so a copied **state** compares and costs the same as the original.

**Host** _[lesh]_:
Whatever drives the editor — `src/ui/` in the shell, a fake in a test. It owns
the loop, the terminal, the timers, the workers and the shell handoff; it owns
**everything the editor is not allowed to know** — the shell's tables, the
history, the syntax, the completion sources, the reactors that use them; and it
sends **events** in and performs the **effects** that come back. The editor never
calls it except through the one interface it declares for exactly that,
`leshper::host` (`host.h`): a borrowed pointer on the registry, answering
`lesh_request_command_kind` and carrying out `want_completion`. A question raised
there is an **effect** value and its answer is an **event** value, the same
vocabulary the deferred half uses, so a question that moves to a worker later is
a change on the host's side and nowhere else.
_Avoid_: calling the host "leshper", or a driver "the editor"; a second interface
between the two — there is one door and it is `leshper::host`.

**Event** _[lesh]_:
The only way into the editor: a closed variant the host constructs and hands to
`step`. A key, a resize, a worker result, a job notice, injected input, a signal,
a paste, a timer expiry. There is no side channel — a new kind of input has
to be argued for by adding an alternative to the variant (`event.h`). `completion_candidates`
is an event type without being a variant alternative, because it answers a
`want_completion` inside the turn that asked rather than arriving at `step`; it
obeys the channel's rules regardless — trivially copyable, and the candidate list
is a pointer into storage the **host** owns and keeps alive until the next
`want_completion` it carries out. Anything an event carries that is not a scalar
is borrowed on the same terms.

**Effect** _[lesh]_:
The only way out of the editor: what one turn of its state machine emits
alongside the new state. A repaint, a worker request, a spawn, an armed or
disarmed timer, a request for completion candidates, and the four that end a
line — accepted, cancelled, end of file, recursive edit. Synchronous, returned as
a value, and carried out by the **host**. Trivially copyable, with one exception:
a `static_assert` per type in `effect.h` says so, because an effect is emitted per
turn and, for a timer, per expiry, and text on the channel travels as a borrowed
view into storage the host owns. `spawn_request` is the exception — it carries an
already-expanded argv and has no producer yet — and `effect.h` names it in the
comment above those assertions, the way `event.h` names
`completion_candidates`. `want_completion` is carried out where it is raised rather
than after the turn, because `lesh_complete` answers a count the same ABI call
reads back; it is the same value type either way.
_Avoid_: using it for a reactor's output, which is a decoration or proposal
arriving later as an event; a host reaching into the editor for something an
effect should have carried.

**Surface** _[lesh]_:
The grid of styled cells leshper renders into. A blitter turns surfaces into
terminal output; tests assert cell grids, never escape sequences. A repaint
REPLACES the frame on screen rather than appending one (#185): it walks up to
that frame's top row, erases from there down, and paints — and how far up that
is is the **host's** answer, because after a resize a reflowing terminal has
rewrapped the frame and the surface that was painted no longer describes it.
Every row knows which KIND of line it begins (#189): a row a long line wrapped
into is SOFT and the blitter reaches it by writing through the right edge, so
the terminal joins it to the row above as one logical line and rewraps the pair
the way the layout does; a row after a hard newline is a line of its own and is
positioned to. Painting a soft row as a hard one is what left clipped fragments
behind on a shrink and the old top row behind on a grow.
_Avoid_: treating the cursor at repaint time as being at the surface's origin;
that is true only of the first paint of a read. _Avoid_: erasing to end of line
or screen while a wrap is pending — the cursor is still on the last column and
the erase eats the glyph just written.

**Grapheme cluster** _[lesh]_:
What a user calls a character: the unit the cursor rests on, one Backspace deletes,
one `forward-char` crosses. UAX #29 extended grapheme cluster boundaries, computed
against tables lesh generates rather than a library's.
_Avoid_: character, which the shell language already uses for a byte; codepoint,
which is one of the several a cluster may hold.

**Cluster width** _[lesh]_:
How many terminal cells one grapheme cluster occupies. Not the sum of its
codepoints' widths — that reads a woman-ZWJ-boy emoji as four columns where a
terminal draws two, and leaves the cursor two cells from where the user sees it.
The half of the problem no surveyed Unicode library answers.
_Avoid_: wcwidth, which names the per-codepoint question and gets that answer.

**Width policy** _[lesh]_:
The replaceable object that turns a width class into a column count. Width belongs
to the terminal on the far side of the pty, not to Unicode: East Asian Ambiguous is
one cell or two depending on the locale, and a Unicode 9 terminal disagrees with a
Unicode 17 one about emoji. The tables give the default; the policy is where a
negotiated protocol or a user override lands, without the segmenter changing.
_Avoid_: treating a width as a constant, which is how a redraw drifts.

**Generation** _[lesh]_:
A counter bumped on every buffer mutation. An async result carries the
generation it was computed against; a stale result is dropped, structurally.

**Topic** _[lesh]_:
A source of loop events — `tty`, `signal`, `worker`, `timer`. The loop polls
topics and drains each into events; a topic's file descriptor or deadline
is its implementation detail. A plugin's fd-readable hook is one more topic.
_Avoid_: naming the fd; the topic is what the editor sees.

**Quiesce** _[lesh]_:
Parking every worker at a known-idle point, holding no lock, before the
loop thread forks — the only thing that makes `fork()` safe in a threaded
shell, since the child inherits every other thread's held locks frozen.
Resumed after the command is reaped. Needed because lesh runs shell code in
forked children (subshells); fish, which only ever execs, does not park and
relies on the child touching nothing before exec — lesh does both.

**Shell thread** _[lesh]_:
The main thread: the one owner of shell state. Executes at accept; while a
line is edited it serves three latest-wins slots — `execute`, `port_call`,
`highlight` — one at a time, so a writing action and a reading highlighter
never overlap. Everything else in the process is stateless or owns only
editor state.
_Avoid_: reading shell state from any other thread; a definitions version or
concurrent collection for it — one owner makes both unnecessary (ADR-0009).

**Loop thread** _[lesh]_:
The **host's** spawned thread — `src/ui/loop.cpp`, not leshper's (#168): owns
editor state and the tty while editing, waits in `poll` on its topics, blocks
across execution, and drops any shell-thread message whose generation is not
current. It keeps the previous frame's INPUT beside the previous frame, so that
a resize can re-lay it at the new size and answer where the terminal has moved
that frame's top row to (#185; `loop_options::assume_reflow` picks the model).
That it is a thread at all is the host's private choice; the editor it drives is
told nothing about it.

**Replay file** _[lesh]_:
The structured (jsonl) record of every loop input — key events, resize,
signals, worker results with generation and timing — written by the logger's
structured sink when asked. Feeding it back through the editor must reproduce
an equal state; that equality is the N-3 harness.
_Avoid_: a second event serialization for tests; the replay file is the one.

**Kill store** _[lesh]_:
The one keyed store of killed and yanked text. Emacs's kill ring and vi's
unnamed register are the same store under its default key; named registers
and a clipboard-backed key arrive later as views over it.
_Avoid_: clipboard as the primary name — the terminal clipboard is a
possible backing for one key, not the store.

**Keymap** _[lesh]_:
A flat table binding key sequences to action names, keyed by symbolic key
events — never raw bytes; the decoder owns escape sequences exactly once.
First-class data: created, copied, modified, pushed and popped at runtime —
modal input is a stack of keymaps, never a second dispatch system.
_Avoid_: binding a key to another key sequence; a binding names an action,
so vim's map-vs-noremap distinction has nothing to distinguish here.

**Autosuggestion** _[lesh]_:
The greyed-out completion of the current line drawn from history.
_Avoid_: hint, which was replxx's word and left with replxx (#28).

**Capability surface** _[lesh]_:
The flat C ABI a plugin is written against. Everything a plugin can do, and nothing
that requires C++ types or pointers into the arena.

**The floor** _[lesh]_:
The POSIX language lesh conforms to. Contrasted with **the curated layer**, the
zsh-inspired features admitted on top by explicit decision.

**Reference shell** _[lesh]_:
The shell a differential test compares against. dash is authoritative for the floor;
zsh only for the curated layer.

**Divergence** _[lesh]_:
A place lesh deliberately answers differently from the reference shell. Written down
in ADR-0001, excluded from the differential comparison — dash cannot be the
expectation of a case that exists because lesh differs — and carried as a
`[divergence: ...]` case that states its own expected output. Counted in its own
tally, so **known-fail** means only a gap not yet implemented.
_Avoid_: "known failure" and "xfail" for one, which is what conflating the two cost:
a score that could not tell "we chose this" from "we have not built this".

**Shell under test** _[lesh]_:
Whichever shell a differential case is currently running in — lesh in one half of a
comparison, the reference shell in the other. Named `$TESTEE` in a case's
environment, absolute, so a case may re-invoke it with arbitrary argv and the
comparison stays lesh-invoking-lesh against dash-invoking-dash.
_Avoid_: "testee" on its own, which does not say under test of what; and "the shell"
in a case that re-invokes, where it names two different processes.

**Strangler** _[lesh]_:
The migration in which the new implementation is built beside the old parser
and driven to parity before the old one is deleted.
