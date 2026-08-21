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
Splitting the result of an unquoted expansion on the characters in `IFS`.
_Avoid_: word splitting, tokenizing.

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
**Front end** below. Also avoid "pipe", which names the `|` operator and the syscall.

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
at execution time.

**Executor** _[lesh]_:
Runs a tree. An interface, so the back end stays replaceable.

**Front end** _[lesh]_:
The lexer and parser together. Use this, or name the components individually, when
you would otherwise reach for "pipeline" — that word belongs to POSIX.

**Shell state** _[lesh]_:
Variables, scopes, functions, aliases, options, working directory, and `$?`. Read by
both the expander and the executor.

**Span** _[lesh]_:
A token or node's location in the source, carried so errors and highlighting can
point at it.

**Error node** _[lesh]_:
A tree node standing in for input the parser could not make sense of. What makes the
parser total.

**LLE** _[lesh]_:
The Lesh Line Editor. lesh's own terminal input layer, named after zsh's ZLE.
_Avoid_: the reader, readline.

**Widget** _[curated]_:
A unit of editing behaviour in LLE, bindable to a key. Borrowed from zsh.

**Keymap** _[curated]_:
A table binding keys to widgets.

**Autosuggestion** _[curated]_:
The greyed-out completion of the current line drawn from history.
_Avoid_: hint, which is replxx's word and leaves with replxx.

**Capability surface** _[lesh]_:
The flat C ABI a plugin is written against. Everything a plugin can do, and nothing
that requires C++ types or pointers into the arena.

**The floor** _[lesh]_:
The POSIX language lesh conforms to. Contrasted with **the curated layer**, the
zsh-inspired features admitted on top by explicit decision.

**Reference shell** _[lesh]_:
The shell a differential test compares against. dash is authoritative for the floor;
zsh only for the curated layer.

**Shell under test** _[lesh]_:
Whichever shell a differential case is currently running in — lesh in one half of a
comparison, the reference shell in the other. Named `$TESTEE` in a case's
environment, absolute, so a case may re-invoke it with arbitrary argv and the
comparison stays lesh-invoking-lesh against dash-invoking-dash.
_Avoid_: "testee" on its own, which does not say under test of what; and "the shell"
in a case that re-invokes, where it names two different processes.

**Strangler** _[lesh]_:
The migration in which the new front end is built beside the old parser and driven to
parity before the old one is deleted.
