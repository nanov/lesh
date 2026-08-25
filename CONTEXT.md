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

**Exit status** _[floor]_:
The value a command reports on completion, readable as `$?`.

**Here-document** _[floor]_:
A redirection whose input is taken from the lines following the command.

## The implementation

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
The interactive layer as a whole: leshper, prompts, rendering — what
`src/ui/` holds. The shell proper is everything else.

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
lesh Prompt Editing & Reading — the shell's own line editor, the largest
resident of the UI. Integrated: compiled into the lesh binary, not a library.
_Avoid_: LLE, the retired earlier name; the reader; readline.

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
A named keymap set defining an editing paradigm — emacs, vi-insert,
vi-command. Not an enum anywhere: adding a mode is defining keymaps and
actions.

**Reactor** _[lesh]_:
A subscriber to editor state-change events that computes derived state —
decorations and proposals — asynchronously on a worker. Never mutates buffer,
cursor or selection. The highlighter and the autosuggester are reactors.
_Avoid_: calling a reactor's work an effect; an **effect** is synchronous and
the loop's own.

**Binding** _[lesh]_:
A language's frontend to the action/reactor ABI: the lesh binding exposes
editor state as shell variables, the Lua binding (later) as a table API. A
binding wraps the one C surface; the ABI itself belongs to no binding.

**Request token** _[lesh]_:
The opaque, generation-bound handle a reactor computes against. The only
mint for results: decorations and proposals are emitted through it or not at
all, which is what makes applying a stale result impossible rather than
checked.

**Proposal** _[lesh]_:
Derived state offered to the user — an autosuggestion, a candidate list. It
becomes a buffer edit only through an accepting action; it never auto-applies.

**Decoration** _[lesh]_:
A namespaced annotation anchored to buffer positions: a highlight span or
virtual text. Survives edits. The one system highlighting and autosuggestions
both render through.

**Effect** _[lesh]_:
What one turn of the editor's state machine emits alongside the new state:
render output and worker requests. Synchronous, owned by the loop.
_Avoid_: using it for a reactor's output, which is a decoration or proposal
arriving later as an event.

**Surface** _[lesh]_:
The grid of styled cells leshper renders into. A blitter turns surfaces into
terminal output; tests assert cell grids, never escape sequences.

**Generation** _[lesh]_:
A counter bumped on every buffer mutation. An async result carries the
generation it was computed against; a stale result is dropped, structurally.

**Keymap** _[lesh]_:
A table binding key sequences to action names. First-class data: created,
copied, modified, pushed and popped at runtime — modal input is a stack of
keymaps, never a second dispatch system.

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
