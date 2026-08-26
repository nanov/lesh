# A mini-library for defining builtin options: constexpr table, or something off the shelf?

**Date:** 2026-08-26
**Ticket:** [#148](https://github.com/nanov/lesh/issues/148) — parked idea-and-research, unrelated to map #82
**Status:** Research complete, recommendation pending human review
**Scope:** the ticket's **rescoped** reading (owner's comment on #148) — *a small argument-parsing
library*, nothing more. Declarative option specs, compile-time validated; a POSIX-correct
zero-heap parse of `char** argv`; one usage-error reporter; usable by every builtin and by
`main.cpp`; per-command opt-out for the grammar deviants. **Out of scope and not researched:**
the builtin registry (#89) and completion metadata (#137). Where the same table would happen to
serve those, it is noted once and dropped.
**Constrained by:** [ADR-0001](../../adr/0001-posix-as-the-language-spine.md) (dash is the POSIX
floor; divergences are recorded, not chosen quietly),
[ADR-0005](../../adr/0005-no-runtime-shared-library-dependencies.md) (vendored + statically
linked is fully acceptable; "a library that does a job better than a hand-rolled equivalent is a
reason to take the dependency")

lesh tree read at `nanov/lesh@0251f61137d9fd8ecae55dc91e057c857d2210a0` (branch `leshper`,
2026-08-26, working tree clean). The branch advanced to
`f592d8b` (#149) while this note was being written; verified with
`git diff --stat 0251f61 f592d8b -- src/runtime/builtins.{h,cpp} src/runtime/invocation.cpp
CMakeLists.txt tests/unit/allocation_tests.cpp third_party/yash-tests/` that **not one cited file
changed** — #149 is leshper-only — so every line number below is still accurate at `f592d8b`. All measurements taken 2026-08-26 on Apple Silicon
(`arm64-apple-darwin25.2.0`), Homebrew clang, `/opt/homebrew/opt/llvm/bin/clang++`. Every command
in this note reproduces.

---

## Bottom line

**Write it in-house. A `constexpr` option-spec table plus a zero-heap parser is ~100 lines of
live code, and it is already built and measured — not sketched.** The prototype is 158 lines,
99 of them live code and the rest comment in the house style; it is reproduced in full in §8 so
that nothing depends on the scratchpad it was written in. It passes 17 POSIX XBD §12.2 cases, performs **0 heap allocations over 10,000 parses**,
compiles clean under the debug gate's exact sanitizer flags, emits **585 bytes of `__text`** for
a six-option table, and rejects a duplicate option letter **at compile time** with a named
diagnostic. It builds under `-fno-exceptions -fno-rtti` as well as under the tree's current
flags, so it does not bet on today's policy.

It would replace **13 hand-rolled option loops totalling 336 lines** across
`src/runtime/builtins.cpp` and `src/runtime/invocation.cpp`. That is a 3.4x reduction, and the
reduction is the smaller half of the argument: those 13 loops currently disagree with each other
on **seven axes**, catalogued in §1.

Along the way the survey found **one outright bug**: `unset` scans its own options twice by two
different rules, so `unset -qf f` silently unsets the function `f` at status 0 where dash, bash
and zsh all reject `-q` (§1.2 Axis 6, measured in §9.5). One scanner per utility makes that class
of defect unrepresentable.

**clip.hpp — the concrete candidate the owner added mid-research — is the best-fitting artifact
in the survey and is not quite the right base.** It clears zero-heap (0 allocations measured),
is constexpr-testable, and detects duplicate names at compile time. But it is GNU-shaped where
POSIX is needed, it cannot express `cd -P -L -PL` at all, and it costs **2,238 bytes of `__text`
per builtin against the flat design's 77** — 29x, because its parse body is a fully-unrolled fold
per option. §7 measures it in full; §8 is written as a hybrid that takes clip's interface ideas
onto a flat, loop-based core.

**Six of issue #148's stated premises need correcting**, and three of them change the
economics substantially. They are collected in §9. The short version: the conformance corpus does **not**
assert the exact text `Illegal option -x`, and does **not** assert status 2 for any builtin usage
error — it asserts "stderr non-empty, status non-zero" and nothing more. The unification the
ticket calls "a scoreboard tripwire, not a free refactor" is, measured, very nearly a free
refactor. And the allocation gate does not currently cover builtins at all.

---

## 1. What the tree actually does

### 1.1 The inventory

`src/runtime/builtins.cpp` is 2,485 lines and holds 24 handler functions behind one uniform
signature, `builtin_result (*)(shell_state&, char**)`
(`nanov/lesh@0251f61 src/runtime/builtins.cpp:2321-2338`). Dispatch is a linear scan over
`kBuiltins` in `try_run_builtin` (`:2454-2483`). The name/kind registry is a separate
`constexpr std::array<builtin_descriptor, 30>` in `builtins.h:156-204`, and a `static_assert`
holds the two tables in agreement (`:2367-2381`) — that guard is #89's work and is **not** what
this ticket is about.

**Option scanning is where the uniformity stops.** There is no shared scanner. Fifteen distinct
sites read options, thirteen of which are per-utility loops:

| site | file:line | lines |
|---|---|---|
| `scan_directory_options` (`cd`, `pwd`) | `builtins.cpp:91-123` | 33 |
| `builtin_set` | `builtins.cpp:1051-1095` | 45 |
| `builtin_kill` | `builtins.cpp:1421-1454` | 34 |
| `builtin_trap` | `builtins.cpp:1218-1249` | 32 |
| `builtin_read` | `builtins.cpp:1628-1659` | 32 |
| `builtin_bind` | `builtins.cpp:2059-2083` | 25 |
| `run_declaration` (`export`, `readonly`) | `builtins.cpp:861-882` | 22 |
| `builtin_unset` | `builtins.cpp:965-982` | 18 |
| `unset_functions` | `builtins.cpp:944-955` | 12 |
| `unset_selects_functions` | `builtins.cpp:2444-2451` | 8 |
| `builtin_echo` (`-n`) | `builtins.cpp:51-56` | 6 |
| `builtin_unalias` (`-a`) | `builtins.cpp:2159-2162` | 4 |
| `parse_invocation` (the shell's own command line) | `invocation.cpp:27-91` | 65 |
| **total to be replaced** | | **336** |

Two more sites are related but are *not* replacement candidates, and the note says so to keep the
number honest:

- `first_operand` (`builtins.h:294-296`, 3 lines) — discards a leading `--` for the six utilities
  that take operands and no options (`break`, `continue`, `.`, `eval`, `exit`, `return`, plus
  `shift`, `alias`, `unalias`). It is already the shared answer for that one rule, and it is
  already correct. A spec table with an empty option list subsumes it, but replacing it buys
  nothing on its own.
- `getopts_step` (`builtins.cpp:1851-1930`, 80 lines) — this is the POSIX `getopts` **builtin**,
  a parser lesh exposes *to scripts*. It is a feature, not an internal scan. Worth noting for a
  different reason: it is the one **correct** POSIX option parser already in the tree (clustering
  at `:1872-1878`, attached option-argument at `:1901-1904`, separate at `:1905-1907`, `--` at
  `:1861-1863`, lone `-` as operand at `:1858-1860`). The in-house parser is not new science
  here; it is `getopts_step`'s logic, lifted out of the OPTIND/OPTARG shell-variable machinery
  that makes it unusable internally.

### 1.2 The divergence catalogue

Seven axes, all measured against `./build/release/lesh` at this sha.

**Axis 1 — the unknown-option message. Seven distinct wordings.**

```
$ lesh -c 'cd -Z'        cd: Illegal option -Z            status 2   (letter)
$ lesh -c 'pwd -Z'       pwd: Illegal option -Z           status 2   (letter)
$ lesh -c 'export -Z'    export: Illegal option -Z        status 2   (whole word)
$ lesh -c 'readonly -Z'  readonly: Illegal option -Z      status 2   (whole word)
$ lesh -c 'unset -Z'     unset: Illegal option -Z         status 2   (whole word)
$ lesh -c 'trap -Z'      trap: Illegal option -Z          status 2   (whole word)
$ lesh -c 'set -Z'       set: Illegal option -Z           status 2   (sigil + letter)
$ lesh -c 'set -o bogus' set: Illegal option -o bogus     status 2   (sigil + name)
$ lesh -c 'read -Z'      read: illegal option -Z          status 2   (LOWERCASE i)
$ lesh -c 'bind -Z'      bind: -Z: unknown option         status 2   (different shape)
$ lesh -c 'kill -Z 1'    kill: Z: bad signal              status 2   (not an option error at all)
$ lesh -c 'getopts a v -Z'  getopts: illegal option -Z    status 0   (lowercase; 0 by POSIX design)
$ lesh -c 'unalias -Z'   unalias: -Z: not found           status 1   (read as an operand)
$ lesh -c 'alias -Z'     alias: -Z: not found             status 1   (read as an operand)
$ lesh -c 'echo -Z'      -Z                               status 0   (echoed)
$ lesh -Z -c 'echo ok'   lesh: invalid option: -Z         status 2   (+ a usage line)
```

The `read` lowercase `i` (`builtins.cpp:1642`) is a straightforward typo that no test catches —
see §3. `getopts`'s lowercase (`:1892`) is deliberate-adjacent but shares the accident.

**Axis 2 — clustering. Some cluster, some treat the word as an atom.**

`cd`/`pwd` (`:103`), `set` (`:1067`), `read` (`:1635`) and `parse_invocation` (`:42`) iterate the
characters of the word. `export`/`readonly` (`:871`), `unset` (`:974`), `trap` (`:1226`) and
`bind` (`:2068-2072`) compare the **whole word** with `==`, so a cluster is an unknown option:

```
$ lesh -c 'cd -LP /tmp && pwd'   /private/tmp        clusters
$ lesh -c 'export -pp'           export: Illegal option -pp      status 2
$ lesh -c 'unset -vv X'          unset: Illegal option -vv       status 2
$ lesh -c 'trap -pp'             trap: Illegal option -pp        status 2
$ lesh -c 'bind -ll'             bind: -ll: unknown option       status 2
```

These particular clusters are harmless in practice (`-pp` means nothing more than `-p`), so this
axis is a latent hazard rather than a live bug. It becomes a live bug the first time one of those
utilities gains a second option letter.

**Axis 3 — option-argument attachment. Inconsistent, and the inconsistency is invisible.**

`read` and `kill` accept both forms; `bind`, `set -o` and `parse_invocation -o` accept only the
separate form.

```
$ lesh -c 'printf "a:b\n" | { read -d: v; echo "[$v]"; }'    [a]     attached  OK
$ lesh -c 'printf "a:b\n" | { read -d : v; echo "[$v]"; }'   [a]     separate  OK
$ lesh -c 'kill -sTERM $$'   /  'kill -s TERM $$'            both work
$ lesh -c 'bind -m emacs'    bind: no line editor in this shell     separate OK
$ lesh -c 'bind -memacs'     bind: -memacs: unknown option          attached rejected
```

`set -o` is the interesting one, and it needs care, because the naive reading is wrong:

```
$ lesh -c 'set -oerrexit; false; echo REACHED'    REACHED     (errexit never set, status 0)
$ lesh -c 'set -o errexit; false; echo REACHED'   (nothing)   (errexit set, status 1)
```

`set -oerrexit` **silently lists the options instead of setting one** — `builtin_set` sees `o`,
looks at `argv[i+1]`, finds nothing, and takes the bare-`-o` listing branch (`:1069-1077`),
discarding the attached `errexit` entirely. That looks exactly like the stub-that-succeeds shape
this codebase has paid for repeatedly. **But dash does the same thing**, and ADR-0001 makes dash
the floor:

```
$ dash -c 'set -oerrexit; false; echo REACHED'    prints the option listing, then REACHED
```

Same for the invocation parser. `lesh -oerrexit -c 'echo ok'` discards the attached value and
consumes the *following* word as the option name — but so do dash and bash:

```
$ lesh  -oerrexit -c 'echo RAN'   lesh: invalid option name: -c              status 2
$ dash  -oerrexit -c 'echo RAN'   /bin/dash: 0: Illegal option -o -c         status 2
$ bash  -oerrexit -c 'echo RAN'   bash: line 0: bash: -c: invalid option name  status 2
$ zsh   -oerrexit -c 'echo RAN'   RAN                                        status 0
```

**So neither is a conformance bug, and this note explicitly does not report them as one.** Only
zsh handles the attached form. What is left is the internal inconsistency: within one shell,
`read -d:` works and `bind -memacs` does not, for no reason a user could predict. A spec table
settles that per option, deliberately, in one place — and can keep `set -o`'s dash-matching
behaviour as an explicit opt-out rather than as an accident of where the `if` fell.

**Axis 4 — a lone `-`.** Correct nearly everywhere, by four different mechanisms.
`cd` guards with `arg.size() < 2` (`:101`), `set` the same (`:1061`), `read` with
`argv[first][1] != '\0'` (`:1628`), `trap` with `arg.size() >= 2` (`:1243`), and
`parse_invocation` **consumes and ignores** it explicitly (`:33-36`) — a fifth answer, and the
right one for that context per POSIX ("a single hyphen shall be treated as the first operand and
then ignored"). `export -` and `unset -` fall through to the operand check and report
`-: bad variable name`, which is defensible.

**Axis 5 — `--`.** Handled by every loop except `echo` (which has no `--`, matching dash) and
`unalias`'s `-a` fast path (`:2159-2162`), which is checked *before* `first_operand`, so
`unalias -- -a` correctly removes an alias named `-a` (verified: reports `-a: not found`,
status 1). `kill` needs its own because `--` comes after the signal option, not at `argv[1]`
(`:1424-1430`) — a genuine reason `first_operand` cannot serve, and a case the spec table handles
naturally.

**Axis 6 — `unset` reads its options twice, by two different rules.** `unset_selects_functions`
(`:2444-2451`) uses `arg.find('f') != npos` — a **substring search over the whole word**. So
`unset -qf x` selects the function table even though `-q` is not an option `unset` has, and
`builtin_unset`'s own loop (`:965-982`) would have rejected `-q`. Two readings of one command
line that can disagree — and **they do disagree in the shipped binary**: `lesh -c 'unset -q X'`
reports `Illegal option -q`, while `lesh -c 'f(){ :; }; unset -qf f'` silently unsets `f` at
status 0. dash, bash and zsh all reject it. Measured in full in §8.5; this is the one outright
bug this survey found, and it is precisely the drift the `static_assert` in #35 exists to prevent
at the registry level, reappearing one layer down.

**Axis 7 — where the diagnostic and the status are produced.** Counted: **nine** `report(...)`
call sites with hand-written format strings sit inside the thirteen scanning ranges, plus two
more just outside them — `cd` at `builtins.cpp:283` and `pwd` at `:137`, which report on the
`unknown` char that their shared `scan_directory_options` hands back rather than printing it
itself. Eleven in total, plus `parse_invocation`'s out-of-band
`{error, error_operand}` pair (`invocation.h:41-44`) which `main.cpp:47-58` turns into a message,
a usage line and `std::exit(2)`. The builtins return `{2}`; the invocation parser exits. Two
error channels for one class of error.

---

## 2. The gates, measured

**C++ standard: C++23.** `target_compile_features(lesh_options INTERFACE cxx_std_23)`
(`nanov/lesh@0251f61 CMakeLists.txt:39`). Clang only, libc++ (`CMakePresets.json:15-16`,
`CMakeLists.txt:28`). `consteval`, `constexpr` `std::array`, and CTAD are all available.

**Exceptions and RTTI are ON — the brief implies otherwise, and this matters.** A sweep of every
non-vendored `CMakeLists.txt` and `CMakePresets.json` finds **no `-fno-exceptions` and no
`-fno-rtti`** anywhere. The flag set is `-Wall -Wextra -Wno-c99-designator`, `-Werror` in Release
only, and ASan+UBSan in Debug (`CMakeLists.txt:41-61`).

So a throwing library would *compile*. What is true is that the codebase is **de-facto
no-throw**: `src/` contains **zero** real `throw`, `try` or `catch` (all grep hits are comments
or `struct entry` false positives) and **zero** `dynamic_cast`/`typeid`. That is a strong style
convention, not a compiler-enforced policy — a distinction the ticket collapses. It changes the
library assessment in §6 from "these are disqualified" to "these are undesirable", which is a
weaker claim and has to be argued on its merits rather than asserted.

**The allocation gate does not cover builtins.** `tests/unit/allocation_tests.cpp` has 12 tests
(`:59, 78, 96, 119, 138, 205, 218, 246, 267, 354, 376, 406`) covering parsing, expansion, lexing,
logging and the editor loop. **None of them runs a builtin**, and the file contains no reference
to `builtin` or `argv` at all. Builtin option parsing is therefore not currently gated — and
today's code would not pass such a gate anyway: `builtin_set` builds a
`std::vector<std::string>` for the positional parameters (`builtins.cpp:1101-1104`),
`getopts_arguments` builds a `std::vector<std::string_view>` (`:1742-1753`), and `read` builds
`std::string`s (`:1535-1539`).

**This is an opportunity, not an obstacle.** "Zero heap on the option path" is an aspiration the
ticket states as if it were an enforced invariant. It is not enforced, so the new parser can
*establish* the invariant for the scan itself, and a new allocation test can lock it — which is
the probe prescribed in §10.

**The sanitized gate is `ctest --preset debug`** (ASan+UBSan+LSan, `-fno-sanitize-recover=undefined`,
`CMakeLists.txt:58`, `CMakePresets.json:70-73`). The prototype was built and run under exactly
those flags; see §8.

---

## 3. The conformance tripwires — measured, and smaller than the brief says

This is the section that changes the decision, so it is evidenced in detail.

The ticket states: *"The conformance suite tests exact usage errors and statuses — `Illegal
option -x`, status 2 — so a unified reporter is a scoreboard tripwire, not a free refactor."*

**Measured against `third_party/yash-tests/` (122 `.tst` files), that is not so.**

### 3.1 The corpus never asserts the string

```
$ grep -rniE "illegal option|invalid option|unknown option|unrecognized option" *.tst
getopts-p.tst:94:  test_o 'operand variable is set to "?" on unknown option'
getopts-p.tst:101: test_o 'OPTARG is set to the option on unknown option (with :)'
getopts-p.tst:108: test_E 'no error message on unknown option (with :)'
getopts-p.tst:112: test_o 'OPTARG is unset on unknown option (without :)'
getopts-p.tst:119: test_x -d 'error message is printed on unknown option (without :)'
```

Five hits, all of them **test-case names**, all in `getopts-p.tst`, and all about the `getopts`
builtin's contract with scripts rather than about any shell diagnostic's wording. **No `.tst`
file in the corpus contains the string `Illegal option` as an expected output.**

### 3.2 How the corpus actually asserts a usage error

The harness defines its assertion shapes as aliases
(`nanov/lesh@0251f61 third_party/yash-tests/run-test.sh:404-412`):

```sh
alias test_x='testcase "$LINENO" 3<<\__IN__ 4<&- 5<&-'          # stdout/stderr unchecked
alias test_O='testcase "$LINENO" 3<<\__IN__ 4</dev/null 5<&-'   # stdout must be EMPTY
alias test_e='testcase "$LINENO" 3<<\__IN__ 4<&- 5<<\__ERR__'   # stderr compared exactly
```

`testcase`'s own options (`run-test.sh:215-233`) are `-d` (a diagnostic is *required*), `-e`
(expected exit status) and `-f` (invert). The `-d` check, in full
(`run-test.sh:342-355`), is:

```sh
if "$diagnostic_required"; then
    printf '%% standard error (expecting non-empty output):\n'
    cat "$err_file"
    if ! [ -s "$err_file" ]; then failed="true"; ...
```

**`-s` — non-empty. The text is printed for the human and never compared.**

The canonical usage-error assertion across the corpus is therefore `test_O -d -e n`: *stdout
empty, stderr non-empty, exit status non-zero.* Real examples:

```
alias-p.tst:115: test_O -d -e n 'printing undefined alias is error'
break-p.tst:354: test_O -d -e n 'zero operand'
cd-p.tst:435:    test_O -d -e n 'empty operand (-L)'
arith-p.tst:165: test_O -d -e n 'assigning to read-only variable'
```

There are **124** `-d` uses in the corpus and **110** `-e n` uses. Against that,
**`-e 2` appears exactly zero times as a builtin usage-error assertion**. The nine `-e <number>`
sites are all about *program* exit statuses, not diagnostics:

```
$ grep -rn -- "-e 2" *.tst
eval-p.tst:36:      test_OE -e 23  'exit status of evaluation'
exit-p.tst:60:      test_OE -e 2   'default exit status in EXIT trap in exiting with default'
grouping-p.tst:16:  test_x  -e 23  'exit status of subshell'
if-p.tst:172:       test_x  -e 2   'exit status of if-else, false-false'
until-p.tst:47:     test_x  -e 2   'exit status of 2-round loop'
while-p.tst:47:     test_x  -e 2   'exit status of 2-round loop'
startup-p.tst:49:   test_oE -e 23  'one operand with -s'
```

`if-p.tst:172`'s "2" is the exit status of `false` inside an `if`, not a usage error.

### 3.3 Exact-stderr comparison exists, but nowhere near option errors

Only **11** `__ERR__` blocks exist in the whole corpus, in 6 files. Their subjects:

```
cmdsub-p.tst:39   'stderr is not redirected'
dot-p.tst:40      'with verbose option'          (set -v echo)
exec-p.tst:22,30  'exec with redirections'
pipeline-p.tst:114 'stderr is not modified'
option-p.tst:413,423,433  xtrace output and $PS4
```

Every one is about `set -x`/`set -v` trace output or about stderr *plumbing*. **None compares a
diagnostic's wording.**

### 3.4 lesh's own tests do not assert the wording either

The `Illegal option` strings in `tests/` are in **comments**, not assertions
(`tests/unit/options_tests.cpp:161`, `tests/unit/builtin_registry_tests.cpp:209`,
`tests/spec/signals.spec:427`). The assertions themselves check status only:

```cpp
// tests/unit/options_tests.cpp:158-165
EXPECT_EQ(run("set -Z; exit 42"), 2);
EXPECT_EQ(run("set -o bogus; exit 42"), 2);
// tests/unit/readonly_tests.cpp:181
EXPECT_EQ(quietly("readonly -Z x"), 2) << "an unknown option is an error";
```

The one place stderr is inspected is `tests/unit/getopts_tests.cpp:57`'s `capture_stderr`, and it
is used only for emptiness (`EXPECT_NE(..., "")` / `EXPECT_EQ(..., "")` at `:104, 107, 190, 198,
203, 205`) — never for content.

### 3.5 What the real tripwires are

Three, and all three are cheap to hold:

1. **A diagnostic must be produced.** 124 `-d` assertions require stderr to be non-empty. A
   unified reporter that prints *something* on every usage error satisfies all of them.
2. **The status must be non-zero.** 110 `-e n` assertions. Every current site returns 2; keeping
   2 is free and is dash's answer (ADR-0001).
3. **`--` handling must not regress.** Six cases named `separator preceding operand`
   (`eval-p.tst`, `exit-p.tst`, `export-p.tst`, `return-p.tst`, `readonly-p.tst`,
   `shift-p.tst`) assert *behaviour* — e.g. `shift-p.tst:88-92` runs `shift -- 2` and checks
   `$#` and `$@` — not wording. These are real and must be preserved, and the spec table
   preserves them by construction because `--` is handled once.

4. **"The last option wins" must survive, including inside a cluster.** Found late, while
   testing clip.hpp (§7.2a), and missed on the first pass through the corpus:
   `cd-p.tst:354` asserts `cd -P -L -PL` resolves to `-L`, and `cd-p.tst:365` asserts
   `cd -L -P -LP` resolves to `-P`. lesh passes both today because
   `scan_directory_options` assigns `mode` imperatively as it walks
   (`builtins.cpp:104-109`). **Any table-driven replacement that records only *whether* a letter
   appeared loses this** — both `-L` and `-P` come back set, in either order. It is the one
   tripwire in this section that a naive unification really would trip, and §8.1 carries the
   mechanism that answers it.

**Conclusion: the wording is free to change; the presence of a diagnostic, the non-zero status,
`--` semantics, and the last-one-wins tie-break are not.** The unification is very nearly a free refactor, and the `read`
lowercase-`i` typo can simply be fixed in passing. This inverts the ticket's risk assessment.

---

## 4. How the other shells parse builtin options

Read at `magicant/yash@46389f6f6cf9ce18d318f7af1c65f0a157ca1ed4` (branch `trunk`),
`zsh-users/zsh@17af4a46c8983d9b1c164868fe30c0b0738c1e58`,
`git.savannah.gnu.org/git/bash.git@b460816602167718f78a6233164e8875f49b75b2` (the `bminor/bash`
GitHub mirror is now 404), `fish-shell/fish-shell@8510d5906737d6a7fe4537413288066aa881b51f`,
`herbertx/dash@037bbdfd330017c368caf6242f977974123239b5` (0.5.13.5).

### 4.1 zsh — the design lesh should copy

zsh is the only shell of the five where **the option string lives in the central builtin table and
one function parses every builtin's options.** `Src/zsh.h:1440-1453`:

```c
struct builtin {
    struct hashnode node;
    HandlerFunc handlerfunc;
    int minargs, maxargs, funcid;
    char *optstr;     /* string of legal options */
    char *defopts;    /* options set by default */
};
```

Real rows (`Src/builtin.c:55,62,124`):

```c
BUILTIN("cd",      BINF_SKIPINVALID|BINF_SKIPDASH|BINF_DASHDASHVALID, bin_cd, 0, 2, BIN_CD, "qsPL", NULL),
BUILTIN("echo",    BINF_SKIPINVALID, bin_print, 0, -1, BIN_ECHO, "neE", "-"),
BUILTIN("typeset", BINF_PLUSOPTS|..., bin_typeset, 0, -1, 0, "AE:%F:%HL:%R:%TUZ:%afghi:%klp:%rtuxmnz", NULL),
```

74 entries, **48 carrying a non-NULL `optstr`**; lengths min 1 / median 6 / max 38 characters.
Grammar (`Src/zsh.h:1386-1394`): trailing `:` = mandatory argument, `::` = optional and
same-word-only, `:%` = optional numeric.

`execbuiltin` (`Src/builtin.c:250-507`) is **the only option parser in zsh**, and handlers never
see the option words at all. Clustering falls out of its inner `while (*++arg)` loop; `--` and
`-` are handled at `:333-340`; `defopts` is OR'd in afterwards for options not explicitly given
(`:409-417`), which is how `echo` inherits `"-"`.

Results come back in a **128-byte stack struct**, not a bit array (`Src/zsh.h:1416-1420`):
`unsigned char ind[MAX_OPS]`, one byte per option character, packing *seen-as-minus* (bit 0),
*seen-as-plus* (bit 1) and a 6-bit index into an argument array (bits 2-7) — hence
`OPT_ISSET(ops,c)`, `OPT_MINUS`, `OPT_PLUS`, `OPT_ARG` (`Src/zsh.h:1399-1414`). The 6-bit index
caps option-arguments at 63 per invocation.

**Heap: essentially none.** `struct options ops;` is a stack local (`Src/builtin.c:254`) zeroed
with a 128-byte `memset`; the argv copy is a `VARARR` (VLA / `alloca`). The only allocation is
`ops->args`, grown 16 at a time via zsh's arena allocator `zhalloc` (`:230-234`) — and **only if
some option actually takes an argument.** A builtin with no argument-taking options parses with
zero allocation. This is direct precedent for §8's design.

**The deviants opt out by a flag on the table row** (`Src/zsh.h:1455-1479`), which is exactly the
mechanism §7.8 proposes:

| flag | effect | users |
|---|---|---|
| `BINF_HANDLES_OPTS` | NULL `optstr`; the builtin parses everything itself | **`test`, `[`, `set`, `kill`, `trap`** |
| `BINF_PLUSOPTS` | `+x` is a legal form | `typeset`, `export`, `readonly`, `alias`, … |
| `BINF_SKIPDASH` | a bare `-` is an operand | `cd`, `pushd`, `popd`, `printf` |
| `BINF_DASHDASHVALID` | honour `--` even under `SKIPINVALID` | `cd`, `pushd`, `popd` |
| `BINF_SKIPINVALID` | a word containing any unknown char is an operand | `echo`, `cd`, `printf` |

That `BINF_HANDLES_OPTS` list — `test`, `[`, `set`, `kill`, `trap` — is **almost exactly lesh's
deviant list** (§7.8), arrived at independently. Strong corroboration that the opt-out set is
right.

Two corrections to the brief: `BINF_RAWARGS` no longer exists (it is `BINF_HANDLES_OPTS`), and
zsh returns **1**, not 2, for a bad builtin option — `zwarnnam(name, "bad option: %c%c", ...)`,
`Src/builtin.c:388-389`.

### 4.2 yash — the corpus's own shell, and where lesh's error text is *not* from

yash uses a **declarative table per builtin** with a shared parser. `xgetopt.h:28-41` is the whole
public surface — 14 lines:

```c
enum optarg_T { OPTARG_NONE, OPTARG_REQUIRED, OPTARG_OPTIONAL };
struct xgetopt_T {
    wchar_t shortopt;        /* L'-' = no short form; L'\0' terminates */
    const wchar_t *longopt;
    enum optarg_T optarg;
    _Bool posix;             /* visible when posixly_correct */
    void *ptr;               /* caller payload, untouched */
};
```

A real table (`variable.c:2747-2758`, `read`):

```c
const struct xgetopt_T read_options[] = {
    { L'A', L"array",        OPTARG_NONE,     false, NULL, },
    { L'd', L"delimiter",    OPTARG_REQUIRED, true,  NULL, },
    { L'r', L"raw-mode",     OPTARG_NONE,     true,  NULL, },
    { L'\0', NULL, 0, false, NULL, },
};
```

25 such tables, 36 `xgetopt` call sites. **`xgetopt` never allocates** — no `malloc` anywhere in
`xgetopt.c`; it only rotates pointers within the caller's `argv`. But it is **not reentrant**:
`xoptarg`/`xoptind` are externs and the cursor state is file-scope `static`
(`xgetopt.c:65-88`), with three `assert`s (`:150-153`) that fire if two parses interleave.

**`set` and `kill` refuse the library outright, each with an explicit comment** —
`option.c:355-358` (*"We don't use the xgetopt function because of the non-standard syntax"*) and
`sig.c:1325-1326` (same, for `kill -HUP`). Independent confirmation of §8.8's opt-out list.

**Two findings that matter directly for lesh:**

1. **yash's unknown-option message names the whole argv word, not the letter** —
   ``` `%ls' is not a valid option ``` at `xgetopt.c:382`, rendering as
   `` readonly: `-f' is not a valid option ``. lesh's `Illegal option -%c` is **not** yash's
   wording, and since the corpus never compares the text (§3) that was never a constraint.
2. **yash's own usage statuses are not uniformly 2.** `Exit_ERROR` is 2 (`exec.h:36`), and the
   shared helpers return it — but `cd`, `pwd` and `pushd` return literal **5**
   (`path.c:1197,1203,1231,1253`) and `read` returns literal **4** (`variable.c:2799`). So even
   the shell whose suite lesh runs does not hold "usage error == 2" as an invariant. lesh's
   uniform 2 is a deliberate, defensible choice (it is dash's), not a conformance requirement.

### 4.3 dash — the validating datapoint: 29 shared lines

The brief and this note both assumed dash is fully hand-rolled. **Half right.** Shell options are
hand-rolled (`src/options.c:197-243`). But builtin options go through a **shared 29-line
helper** — `nextopt(const char *optstring)` at `src/options.c:529-557`, used from **18 call
sites** across `alias.c`, `cd.c`, `eval.c`, `exec.c`, `jobs.c`, `miscbltin.c`, `trap.c`, `var.c`
and `main.c`. It supports clustering and `:` arguments in both attachment forms, stops on `--`,
and has no permutation, no long options and no `+`.

**This is the strongest single precedent in the survey.** The POSIX floor shell — the one ADR-0001
makes authoritative — solved exactly the problem #148 describes, for its whole builtin set, in
**29 lines**. lesh currently spends 336 on the same job. And dash's own header comment says it
still isn't happy (`src/options.c:518-519`):

> *"XXX - should get rid of. have all builtins use getopt(3)."*

— advice lesh must not take, for the reentrancy reason in §10.1.

**lesh's error wording comes from here.** `"Illegal option -%c"` is `src/options.c:288` and
`:545`; `"Illegal option -o %s"` is `:266`; `"No arg for -%c option"` is `:551`. `sh_error`
(`src/error.c:170-181`) sets `exitstatus = 2` and `longjmp`s. So lesh's text and status are
faithfully dash's — inherited from a shell that produces them from **one** helper, where lesh
reproduces them from thirteen.

### 4.4 bash and fish — the two "don't"s

**bash has no option table at all.** There are two getopts: `sh_getopt` (`builtins/getopt.c`),
used by exactly one caller — the user-facing `getopts` builtin — and `internal_getopt`
(`builtins/bashgetopt.c:50-183`) over bash's `WORD_LIST`, used by everything else. **26 of bash's
43 `.def` files hand-write their own loop** with the optstring as an inline literal;
`builtins/cd.def:278-306` is representative, complete with copy-pasted `CASE_HELPOPT` and
`default: builtin_usage(); return (EX_USAGE);` boilerplate. Neither getopt is reentrant —
`sh_getopt_state_t` (`builtins/getopt.h:64-72`) is a manual save/restore snapshot that `xmalloc`s
a copy of the globals so nested `getopts` can work, **not** a parser context (a second correction
to the brief). Messages: `%s: option requires an argument` / `%s: invalid option`
(`builtins/common.c:175-199`). `EX_USAGE` is **258** internally (`shell.h:71`), mapped to
`EX_BADUSAGE == 2` before it reaches `$?` (`execute_cmd.c:4898-4899`).

**fish is the modern-but-allocating shape.** A short optstring plus a `WOption[]` table per
builtin (`crates/wgetopt/src/lib.rs:76-91`), with **32 hand-written loops** across 31 files.
`WGetopter` is a struct instance with no globals, so unlike yash and bash it is genuinely
reentrant — but it allocates: `argv_opts: Vec<Cow<'args, wstr>>` (`:145`). Messages live on
`Error` (`src/builtins/shared/error.rs:61-94`): `"%s: unknown option"`,
`"%s: option requires an argument"`. `STATUS_INVALID_ARGS == 2`
(`src/builtins/shared/misc.rs:81`).

### 4.5 Summary

| | zsh | yash | dash | bash | fish |
|---|---|---|---|---|---|
| declarative spec | **yes, in the central table** | yes, per-builtin table | no (inline optstring) | **no** | yes, per-builtin table |
| one uniform parser | **yes — `execbuiltin`** | one `xgetopt`, 36 sites | one `nextopt`, 18 sites | helper + 26 hand loops | one `WGetopter`, 32 loops |
| heap on parse | ~none (stack, arena if args) | **none** | **none** | none (in `internal_getopt`) | **yes** (`Vec`) |
| reentrant | n/a (single parse) | **no** (file statics) | no | **no** | yes |
| unknown-option text | `bad option: -x` | `` `-x' is not a valid option `` | `Illegal option -x` | `-x: invalid option` | `-x: unknown option` |
| status | **1** | 2, but `cd`/`pwd` 5, `read` 4 | 2 | 2 (258 internally) | 2 |
| deviants opt out via | **`BINF_*` on the row** | don't call `xgetopt` | don't call `nextopt` | n/a | n/a |

**What lesh should take:** zsh's *shape* — a declarative spec beside the builtin, one parser, and
a per-row flag that lets the deviants out — with dash's *scale* and *wording*, which lesh already
inherits. Every shell here that has a shared parser keeps it under ~30-60 lines of logic; none of
them needs a framework. And three of the five (zsh, yash, dash) confirm the same opt-out set:
`test`, `set`, `kill`, `trap`.

---

## 5. Compact coreutils: the option mini-languages

Read at `landley/toybox@b7ec52ac35e075caffca5d330995d44e8dbfc8c3`,
`mirror/busybox@371fe9f71d445d18be28c82a2a6d82115c8af19d`,
`michaelforney/sbase@b30fb56804bfed69b45ef0e944d2e029e4d26258`,
`uutils/coreutils@728388e76b04cc7d6015974060201869b71a73e7`.

Sizes: `toybox lib/args.c` **536** lines · `busybox libbb/getopt32.c` **622** · `sbase arg.h`
**65** · `uutils uu/ls/src/ls.rs` **1,677**.

### 5.1 toybox — the right grammar, the wrong execution model

toybox packs a whole utility's option spec into **one string literal** in its `NEWTOY()` line.
`toys/posix/cat.c:7` is the whole of `cat`'s option handling:

```c
USE_CAT(NEWTOY(cat, "uvte", TOYFLAG_BIN))
```

and `toys/posix/ls.c:16` is `ls`'s forty options:

```c
USE_LS(NEWTOY(ls, "(sort):(color):;(full-time)(show-control-chars)\377(block-size)#=1024<1\241"
                  "(group-directories-first)\376ZgoACFHLNRSUXabcdfhikl@mnpqrstuw#=80<0x1"
                  "[-Cxm1][-Cxml][-Cxmo][-Cxmg][-cu][-ftS][-HL][-Nqb][-k\377]", TOYFLAG_BIN))
```

The grammar (documented by the author at `lib/args.c:46-93`) is genuinely dense and *local*:
a suffix says what the option takes (`:` string, `#` signed long, `.` double, `@` counter,
`*` appended to a list, `%` a time offset), `<LOW`/`>HIGH`/`=DEFAULT` bound a numeric one,
`(longopt)` attaches to the preceding short option, `|` marks it required, and trailing
`[-abc]` / `[+abc]` / `[!abc]` groups express *switch-off*, *synonym* and *mutual-exclusion*
relations between options already declared. Leading `<N`/`>N` bound the operand count. You read
`w#=80<0` and see "`-w` takes a number, defaults to 80, must be ≥ 0".

**The build-time codegen is the part worth stealing.** `scripts/make.sh:138-161` runs the
harvested `NEWTOY` list through the C preprocessor **twice** — once with the real config, once
with every feature forced on — and pipes both to `scripts/mkflags.c` (266 lines), which emits
`generated/flags.h`: one `#define FLAG_x (1LL<<n)` per option, per command. The two-pass trick
keeps bit numbers **stable** when a config option removes a flag; a disabled flag becomes
constant `0` (`mkflags.c:173-174, 247-248`) so the code testing it is dead-code-eliminated.
Reading a flag is then `FLAG(l)`, i.e. `!!(toys.optflags & FLAG_l)` (`toys.h:134`).

**But two defects make it the wrong model to copy wholesale, and both are things a `constexpr`
table fixes for free:**

1. **It mallocs, unavoidably, on every invocation.** The spec string is re-parsed into a linked
   list *at runtime, every run*: `xzalloc` per option (`args.c:266`), `xmalloc` per `(longopt)`
   (`:283`), `xzalloc` for optargs (`:399`), plus per-occurrence allocations for `*`-type
   options (`:207`) and during long-option prefix disambiguation (`:440`). For `ls` that is
   **≈46 heap allocations before a single directory entry is read**, and the list is freed only
   under `CFG_TOYBOX_FREE` (`:532-535`) — otherwise it leaks by design, relying on `exit()`.
2. **Almost all spec validation is runtime and debug-only.** `mkflags.c` checks just three things
   at build time (empty `()` at `:96`, a bound not followed by a digit at `:123`, `~` not
   followed by `(` at `:136`). The other seven checks — unterminated `(longopt)` `args.c:280`,
   two type suffixes on one option `:299`, malformed `[]` groups `:348-368` — are all
   `CFG_TOYBOX_DEBUG`-gated. The file's own header says so outright (`lib/args.c:6-8`):

   > *"If option parsing segfaults, switch on TOYBOX_DEBUG in menuconfig to add syntax checks to
   > option string parsing which aren't needed in the final code (since get_opt string is
   > hardwired and should be correct when you ship)"*

That comment is the thesis of this whole ticket, stated by someone who could not act on it in
C89. **A `constexpr` parse of the same grammar inverts both defects at once:** the linked list
becomes a static array, the debug-only `error_exit`s become compile errors, and the parse path
drops to zero allocations. lesh would be building the version of `lib/args.c` that its own header
comment apologises for not being.

One sizing datum, measured by the agent by compiling `scripts/mkflags.c` and feeding it the
shipped optstrings: **`ls` uses 40 flag bits, `cat` uses 4.** `toys.optflags` is
`unsigned long long` (`toys.h:114`) — 64 bits — and there is **no guard** against overflowing it
(`args.c:332` and `mkflags.c:247` both shift unchecked). 64 bits is enough for `ls` with room to
spare, which is the same bound §8.1's prototype `static_assert`s.

### 5.2 busybox — the counter-example that justifies build-time generation

`libbb/getopt32.c` splits one utility's spec across **four artifacts**: the opts string, a
NUL-packed long-options table, a `complementary` relation string, and — critically — a
**hand-maintained bit `enum`**. `coreutils/ls.c:221-277` maintains bit numbers by hand with
conditional arithmetic:

```c
	OPTBIT_F = 12,
	OPTBIT_R = OPTBIT_F + 2 * ENABLE_FEATURE_LS_FILETYPES,
	OPTBIT_Z = OPTBIT_R + 1 * ENABLE_FEATURE_LS_RECURSIVE,
```

against an option string carrying hand-computed running totals in comments
(`coreutils/ls.c:209-219`). **Nothing verifies the enum against the string.** This is precisely
the invariant toybox mechanises with `mkflags` and busybox does not, and it is the strongest
single argument in this survey for generating flag identity from the spec rather than restating
it. It is also the same class of defect as lesh's `unset` double-read (§9.5): one fact, two
declarations, free to drift.

The complementary grammar is dense but *referential* rather than positional, and the
documentation admits it is ambiguous — `getopt32.c:289-294` warns that `"?322-22-23X-x-a"` is
legal and means something, advising `:` separators.

Two facts matter for lesh:

- **It does not malloc** — `/* Please keep getopt32 free from xmalloc */` (`:329`), using
  `alloca` (`:373`, `:413`) and a stack array `t_complementary complementary[33]` (`:338`). The
  one exception is `o:*` repeated-list options, which reach `xzalloc` via `llist_add_to_end`
  (`:564`).
- **It wraps libc `getopt_long`** (`:536-541`), and that is **disqualifying for a shell builtin**.
  libc getopt state (`optind`, `optarg`, `optopt`) is process-global and non-reentrant; busybox
  has to defensively `GETOPT_RESET()` before every call (`:527`) with a comment explaining that
  it might be re-entered via `gunzip_main() -> gzip_main()`, and the reset itself differs between
  glibc (`optind = 0`) and BSD (`optind = 1`) (`include/libbb.h:1385-1389`). It also **permutes
  argv in place**. A shell builtin can be invoked from inside another builtin's execution, so
  global parser state is not an option — and this is an independent reason lesh cannot simply
  call `getopt(3)` either.

Its hard ceiling is **32 flags** (`t_complementary complementary[33]` at `:338`,
`if (c >= 32) break;` at `:381`), and overflowing it **silently drops options**. busybox's own
`ls` is at the ceiling — `coreutils/ls.c:255` comments `/* with long opts, we use all 32 bits */`.
A shell that intends to grow builtins should pick its mask width deliberately rather than
inherit either project's number.

### 5.3 sbase — the zero-heap floor, and what it costs

`arg.h` is **65 lines**, 52 of them macro body, and it is the minimalist reference point:
`ARGBEGIN` / `ARGEND` wrap a bare `switch`, with `argc_`/`argv_`/`brk_` as block-scoped locals.
`cat.c:19-24` is the entire option handling of `cat`:

```c
	ARGBEGIN {
	case 'u':
		break;
	default:
		usage();
	} ARGEND
```

**Heap: none** — `grep -c alloc arg.h` is 0; everything is stack locals and pointers into the
original `argv`. `EARGF(usage())` (`arg.h:47-52`) accepts both `-dX` and `-d X`. `--` is handled
(`:19-23`) and a lone `-` terminates option scanning and survives as an operand (`:14`), which is
what lets `cat.c:31` test `if (!strcmp(*argv, "-"))`.

What it does **not** have is the point. There is **no spec object at all** — so no long options
in practice (`LNGARG()` exists and is used by exactly zero sbase tools), no way to *require*
either attachment form, no counters, no numeric parsing or bounds, no required options, no
mutual exclusion, no operand-count checks, no bitmask, and no relationship whatsoever between
the hand-written usage string (`ls.c:365`) and the `switch` twenty lines below it. One sharp
edge worth recording: **`EARGF` on a missing argument calls `abort()`** (`arg.h:52`), not
`exit()` — it only looks sane because the conventional argument is `usage()`, which exits first.

sbase proves the zero-heap parse is trivially achievable in C. It also shows what you get for 65
lines and no spec: nothing is validated, anywhere, ever. lesh's prototype (§7) is ~99 lines and
buys compile-time validation, typed option-arguments and a POSIX-correct `+`/`--`/lone-`-`
grammar for the extra 34.

### 5.4 uutils — one table really does drive everything, and it costs a megabyte

uutils is the only one of the four where a single declaration drives parse **and** `--help`
**and** shell completions **and** manpages. `uu_app()` returns a `clap::Command`; `ls`'s is
**654 lines with 60 `Arg::new` declarations** (`src/uu/ls/src/ls.rs:133-786`), `cat`'s is 86
lines with 11 (`src/uu/cat/src/cat.rs:262-347`). Completions come from the same `Command` via
`clap_complete::generate` (`src/bin/uudoc.rs:212`), reached through a build-script-generated map
that exposes every utility's `uu_app` as a function pointer (`build.rs:69`). Prose lives in
Fluent `locales/*.ftl` files keyed from the builder — note the older `help_about!`/`help_usage!`
markdown mechanism is **gone** at this sha (zero hits repo-wide).

Two things temper the gold-standard reading:

- **Mutual exclusion is O(n²) by hand.** `.overrides_with_all([...])` is repeated in full on
  every member of a group (`ls.rs:149-195`), and the list at `ls.rs:154-160` contains
  `options::format::COLUMNS` **twice** — a harmless artifact, but exactly the redundancy toybox
  writes once as `[-Cxml]`.
- **The runtime cost is large and the project is actively fighting it.** uutils#10471 records
  that `true` — a program with no options at all — ships at over 1 MB. PR #10673, merged,
  removed a single clap call from `true` and cut the binary from **1.26 MiB to 1.13 MiB**: about
  **133 KiB for one parse site**. Open issues #10753, #10846, #10708 and #11340 are all size- or
  startup-cost reductions aimed at clap, and CI tracks binary size per commit
  (`.github/workflows/SizeComment.yml`). `ArgMatches` is a `String`-keyed map of boxed `Any`;
  the builder allocates `Vec`s and `String`s at every `.arg()` call on **every process start**.

**The important separation:** that megabyte attaches to clap's *runtime* machinery — the
suggestion engine, the `ArgMatches` map, `wrap_help` — and **not** to the idea of one
declaration serving several outputs. Deriving usage text from a `constexpr` table is
compile-time work with essentially no runtime footprint. Both C projects here keep help text as
a second, unchecked artifact (toybox's `config …help:` block at `ls.c:18-55`, busybox's
`//usage:` comments harvested into `include/usage.h`); a single table is the one clear
improvement available over toybox, and it is nearly free. It is out of scope for this ticket by
the owner's rescope — noted once, and dropped.

### 5.5 Summary

| | toybox | busybox | sbase | uutils/clap |
|---|---|---|---|---|
| declarative | yes, one string | split over 4 artifacts | no spec at all | yes, builder |
| spec validated | 3 checks at build, 7 debug-only at runtime | runtime, partial; enum↔string **unchecked** | nothing to validate | types at build, semantics at start-up |
| **heap on parse** | **yes, ~46 allocs for `ls`** | **no** (`alloca`); except `o:*` | **no** | **yes, pervasive** |
| one table → parse+help+completion | parse only | parse only | no | **yes** |
| reentrant | yes | **no — wraps libc getopt** | yes | yes |
| mechanism LOC | ~840 (`args.c`+`mkflags.c`) | ~300 + libc | **65** | external crate, ~133 KiB/site |

**What lesh should take:** toybox's *grammar shape* and its *build-time flag generation*, sbase's
*zero-heap execution*, uutils' *single-declaration principle* — and none of their code. That
combination is what §7 builds, and it is why the recommendation is in-house.

---

## 6. The C++ option libraries, against lesh's gates

Every row below was read from source at the sha given **and measured** — the agent built an
`operator new` counting harness and ran each candidate on the same line,
`prog -ab -ofile -- -x - z` (clustered flags, attached option-argument, terminator, lone dash,
operand). The heap and POSIX columns are measurements, not inferences.

Recall from §2 that exceptions and RTTI are **on** in this tree, so throwing is a style and
latency objection here, not a compile failure. Zero-heap is the real gate.

| library | sha | hdr-only | LOC | std | throws | no-throw mode | **heap on parse (measured)** | RTTI | licence | last push | `-abc` | `-ofile` | `--` | lone `-` | constexpr |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CLI11 | `c1cfe00` | y | 6,793 | C++11 | y | n | **15 allocs / 704 B** | n | BSD-3-ish | 2026-08-21 | y | y | y | y | n |
| cxxopts | `b613531` | y | 3,140 | C++11 | y | `std::exit`, not recoverable | **170 allocs / 12,593 B** | n | MIT | 2026-07-13 | y | y | y | y | n |
| p-ranav/argparse | `d924b84` | y | 2,589 | C++17 | y | n | **9 allocs / 669 B** | **y** (`std::any`) | MIT | 2025-01-26 | y | **n** | **n** | y | n |
| Lyra | `da60100` | y | 5,500 | C++11 | **n** | y | **49 allocs / 1,816 B** | n | BSL-1.0 | 2026-07-29 | y | y | **n** | y | n |
| clipp | `2c32b2f` | y | 7,024 | C++11 | n | y | **183 allocs / 32,112 B** | n | MIT | 2019-04-30 | opt-in | y | **n** | y | n |
| argh | `c3f0d8c` | y | **485** | C++11 | n | y | **8 allocs / 520 B** | n | BSD-3 | 2025-01-21 | **n** | **n** | **n** | **n** | n |
| Taywee/args | `903b07d` | y | 5,378 | C++11 | y | **y, `ARGS_NOEXCEPT`** | **5 allocs / 336 B** | n | MIT | 2026-08-03 | y | y | y | y | n |
| Boost.ProgramOptions | `e173046` | **n** | ~5,966 | C++11 | y | n | y | **y** (`typeid`) | BSL-1.0 | 2026-08-12 | y | y | y | y | n |
| **etched** | `716ae73` | y | 2,610 | C++20 | y (not on parse path) | **y, `Expected<>`** | **0 allocs** (3 / 112 B only when collecting operands) | n | MIT | 2026-05-31 | y | y | y | y | **y** |
| arg_router | `e04ed2b` | y | 17,605 | C++20 | y | n | y | n | BSL-1.0 | 2024-07-16 | opt-in | y | **n** | y | partial |
| argum | `e1e64e8` | y | 5,074 | C++20 | optional | y | y | n | BSD-3 | 2026-07-17 | y | y | y | y | n |
| sailormoon/flags | `8f6adfe` | y | 334 | C++20 | n | y | y (`unordered_map`) | n | Unlicense | 2026-04-13 | **n** | n | y | **UB** | n |
| docopt.cpp | `05d507d` | **n** | 1,802 | C++11 | y | n | y (runtime `std::regex`) | n | BSL-1.0 | 2020-06-14 | n/a | n/a | n/a | n/a | n |

### 6.1 No mainstream library achieves a zero-heap parse. Not one.

Every one of CLI11, cxxopts, argparse, Lyra, clipp, argh, Taywee/args and Boost.PO **begins by
copying all of `argv` into a `std::vector<std::string>`** — CLI11 `impl/App_inl.hpp:674-677`,
args `args.hxx:3702-3706`, Lyra `args.hpp:64`, clipp `clipp.h:5327-5329`, argh `argh.h:203-204`,
argparse `argparse.hpp:1912-1914`. That is one allocation per argument before parsing begins,
and it is **architectural, not incidental**: their option objects store `std::string` results
(`CLI/Option.hpp:348`), so the borrowed-view design is closed off downstream too. For a shell
that is one allocation per word on every command invocation.

Worth recording specifically, because each is a different way to fail:

- **cxxopts runs `std::regex_match` on every argv element** (`cxxopts.hpp:927-929`) — 170
  allocations on a seven-token line, the worst measured by count. Its `CXXOPTS_NO_EXCEPTIONS`
  mode prints to stderr and calls **`std::exit(EXIT_FAILURE)`** (`:604-609`) — unrecoverable,
  and unusable in a shell that must report and carry on.
- **argparse needs RTTI** (`std::any_cast`, `argparse.hpp:1556-1598`) and, verified by running
  it, **has no `--` terminator at all** — `--` comes back as `Unknown argument: --`. It also
  rejects the attached form: `-ofile` gives `Too few arguments for '-o'`.
- **argh is the smallest at 485 lines and fails POSIX on all four counts** — no clustering, no
  attached option-arguments, no `--`, and it does not treat a lone `-` as an operand.
- **clipp does not compile under C++23.** It uses `std::result_of` (`clipp.h:163`, `:174`),
  removed in C++20. Last commit 2019-04-30. Dead for this tree.
- **sailormoon/flags has a memory-safety bug on a bare `-`.** `flags.h:71-73` calls
  `remove_prefix(find_first_not_of('-'))`, which for the token `"-"` is `remove_prefix(npos)` —
  undefined behaviour. Under `-fsanitize=address,undefined` it produces a
  `global-buffer-overflow`. lesh's debug preset would reject it on day one, and a lone `-` is
  the single most common operand a shell sees.
- **Boost.PO** is not header-only, requires RTTI (`value_semantic.hpp:355-357` returns
  `typeid(T)`), and drags in Boost.

### 6.2 The two that are actually interesting

**Taywee/args is the least-bad mainstream dependency.** It is the only one passing all five POSIX
checks *and* having a real documented non-throwing mode — `ARGS_NOEXCEPT` (`args.hxx:2919-2922`),
verified to compile and behave under `-fno-exceptions`. Lowest mainstream allocation count at
5/336 B. Single-header, MIT, actively maintained. **But**: 5,378 lines vendored, ~1 allocation
per argv element plus a `std::vector<Command*>` rebuilt on every `Parse` (`args.hxx:3187`) and a
fresh `std::string` per short flag (`:2981-2982`); and the option spec **cannot** be a constexpr
table because `Matcher` holds two runtime-constructed `unordered_set`s. Against lesh's stated
gate that is still a fail — the smallest one.

**etched is the only third-party library that clears the bar**, and it is a **6-star repo by a
single author with no release tags**. The measurements are real: 0 allocations on
`prog -ab -ofile -o f2`, and 3 allocations only when operands are collected (a
`std::vector<std::string_view>` growing 1→2→4, `option.hpp:18`). Tokens are `std::string_view`
into the original `argv` (`lexer.hpp:44`), errors return `Expected<Token, RuntimeError>` rather
than throwing, and the symbol table is a `std::array` perfect hash with a `consteval` salt search
(`symbol_table.hpp:71-77`). It passed every POSIX check including `--` and lone `-`.

**But 2,610 lines from an unproven author, to replace 336 lines of loop, is not a dependency —
it is a reference implementation to read and then write your own from.** That is the same
conclusion §5 reached about toybox, arrived at independently, and it is the conclusion §8 acts
on.

---

## 7. clip.hpp — the leading in-house-shaped candidate, measured

Added to the brief by the owner mid-research. **374 lines, single header, C++20, constexpr-first.**
Read in full and compiled under this tree's standard and flags; full text in Appendix A. Every
number below reproduces from `scratchpad/cliptest/`.

It is by some distance the best-fitting artifact in this whole survey, and it validates the
approach: a `constexpr` option table, NTTP option names so `get<"typo">()` is a compile error,
compile-time duplicate detection, and a `parse()` that is itself a constant expression. Where it
does not fit, it does not fit for reasons that are specific and mostly fixable.

### 7.1 What it gets right — verified, not assumed

```cpp
constexpr auto cli = clip::parser{
    clip::flag<"verbose", 'v'>("verbose output"),
    clip::opt<"count", int, 'c'>("iterations", 10),
    clip::opt<"name", std::string_view>("a name"),
};
```

| gate | result |
|---|---|
| **zero heap on the parse path** | **PASS — 0 allocations, 0 bytes over 10,000 parses** |
| no exceptions | **PASS** — zero `throw` in the header; errors are `string_view` fields on `result` |
| RTTI | **PASS** — none |
| compiles under C++23 / libc++ / `-Wall -Wextra` | **PASS**, clean |
| `parse()` is a constant expression | **PASS** — `static_assert(ct_probe())` compiles |
| compile-time duplicate name / short detection | **PASS** — `static_assert(names_unique())`, `shorts_unique()` (`clip.hpp:352-368`) |
| clustering `-vx` | **PASS** |
| attached `-d:` / separate `-d :` | **PASS** |
| cluster ending in a value option, `-rd:` | **PASS** |
| `--` terminator | **PASS** |
| lone `-` as operand | **PASS** (`clip.hpp:223` — `arg.size() < 2` routes it to positional) |
| missing option-argument | **PASS** — reports `missing value for option` |
| last-wins for a *repeated* option | **PASS** — `std::get<I>(r.values) = ...` overwrites |

The zero-heap result is by construction and holds up: `maybe<T>` is `{T val; bool set;}`
(`clip.hpp:52-66`), `result` is a `std::tuple<maybe<...>...>` plus a
`std::array<std::string_view, MaxPositional>` (`:172-177`), and tokens are `string_view`s into
the caller's `argv`. Nothing owns memory.

### 7.2 Where it does not fit, in order of severity

**(a) It cannot express "the last of `-L`/`-P` wins", and the corpus asserts that twice.**

This is the one gap the owner's three proposed deltas do not cover, and it is the most serious.
`cd-p.tst:354` (`cd -P -L -PL`) and `cd-p.tst:365` (`cd -L -P -LP`) both assert the tie-break,
**including inside a cluster**. clip records *that* a flag was seen, never *when*:

```
cd -P -L  ->  L=1 P=1
cd -L -P  ->  L=1 P=1
```

The two orders are indistinguishable. No arrangement of clip's existing primitives recovers the
answer; it needs a new one. §8 adds mutually-exclusive **mode groups** — toybox's `[-abc]`
bracket group (`lib/args.c:345-377`) — in 18 lines, and all four orderings then come out right.

**(b) POSIX says an operand ends the options; clip keeps parsing.**

```
cd file -L  ->  L=1, positional_count=1
```

clip is GNU-shaped: it interleaves options and operands (`clip.hpp:220-232`). POSIX XBD §12.2 and
every shell here stop at the first operand. This matters concretely — `trap 'cmd' -p` would parse
`-p` as an option rather than as a condition operand. It is a **three-line fix**: set
`only_positional = true` on the first positional instead of appending and continuing.

**(c) GNU-isms the corpus does not want, and one that is actively wrong for a shell.**

`--long` options, `--name=value`, `-c=3`, and a `parse_value<bool>` vocabulary of
`true/1/yes/on` / `false/0/no/off` (`clip.hpp:103-107`) — so `--verbose=off` *turns a flag off*
(verified). POSIX utility syntax has none of this. It is not harmful for a lesh-flavour `ls`
port, but for `cd`/`export`/`trap` it is surface no shell should offer, and `-c=3` in particular
collides with real POSIX usage: `read -d=` means "the delimiter is `=`", not "an empty value".

**(d) Missing: `+x` toggles, `-SIG`, and the `test` grammar.**

No polarity concept at all, so `set +x` and the invocation parser's `+i` are unrepresentable.
No fall-through for `kill -TERM`. `test` needs to opt out entirely, which clip supports by
simply not being used.

**(e) `MaxPositional = 16`, a fixed array.** Operands are *copied* (as `string_view`, so no heap)
into `std::array<std::string_view, 16>` (`clip.hpp:174`). 18 operands gives
`too many positional arguments`. For `echo`, `export`, `unset` and `kill` the operand list is
unbounded. The owner's proposed fix is right and is what §8 does: because POSIX forbids
permutation, **the operand tail is contiguous in the original `argv`**, so a single
`size_t operand` index is zero-copy, unbounded, and strictly less code than the array.

**(f) Error reporting is a generic string plus the whole token.**

`r.error` is `"unknown option"` and `r.error_arg` is the offending **word**:

```
cd -Z    -> error="unknown option"  error_arg="-Z"
cd -LZP  -> error="unknown option"  error_arg="-LZP"     <- the letter Z is lost
```

lesh needs `Illegal option -Z` — the offending **character** — and dash's status 2 (§4.3). Fixing
this is small and is the owner's third delta: replace the two `string_view`s with an **error kind
enum plus the offending char**, and let one shared reporter format the POSIX text. §3 shows the
wording itself is unasserted by the corpus, so this is free to choose; the *character* is what
must be recoverable.

### 7.3 Bugs found on careful reading, confirmed by running

**Bug 1 — `-c=` consumes the next argument *and then* reports an error.** `clip.hpp:275-277`
strips a leading `=` from the remainder; if nothing follows, `rest` is empty, so the parser falls
through to `sval = argv[++i]` and swallows the next word. Then the inner cluster loop continues to
the `=` character, matches no option, and fails:

```
-c= 9    ->  ok=0, count=SET(9), error="unknown option", error_arg="-c="
```

The value is committed **and** an error is reported, naming the wrong thing. Two defects in one
path. (`-c=` alone gives a clean `missing value for option`, which hides it.)

**Bug 2 — `-s=` and `--str=` disagree on the empty value.** For `std::string_view`,
`parse_value` returns the string unconditionally (`clip.hpp:100-101`), so `--str=` sets the empty
string — but `-s=` reports `missing value for option`, because the short path strips the `=` and
finds nothing rather than treating the empty remainder as the value. The two spellings of one
option behave differently.

**Bug 3 (minor) — a value option silently eats a following flag letter in a cluster.** `-cv`
treats `v` as `-c`'s value and reports `invalid value for option`. That is arguably correct
POSIX (the remainder of the word *is* the option-argument), but combined with Bug 1 the
`=`-stripping makes the rule hard to predict.

**Not a bug, worth noting:** `-vc3` (flag, then value option, then attached value) works
correctly, and `---v` is correctly rejected.

### 7.4 The cost: template instantiation

This is the finding that decides the head-to-head. clip instantiates `parser<Opts...>`,
`result<MaxPositional>`, and a `match_any` fold with a lambda per option — **per builtin**.
Measured with identical three-option tables, `-O2`, same compiler:

| | 3 instantiations | 25 instantiations | marginal per builtin | compile (25) |
|---|---|---|---|---|
| **clip.hpp** | 7,186 B `__text` | **56,419 B** | **2,238 B** | 0.50 s |
| **in-house `optspec.h` (§8)** | 698 B | **2,392 B** | **77 B** | 0.08 s |

**29x the code size per builtin, 24x in total.** lesh's release binary is 750 KB; 25 clip
instantiations would add ~56 KB, about 7.5%, to buy long options and a bool vocabulary that
POSIX builtins must not have. The cause is architectural: `result` embeds a 256-byte
`std::array<string_view,16>` and the parse body is a fully-unrolled fold over per-option lambdas,
so nothing is shared between instantiations. The in-house design loops over a small flat array,
so the parse body is one function and only the table differs.

The owner's suggested mitigation — a type-erased runtime core driven by the constexpr table — is
exactly right, and it is what the in-house design already is.

### 7.5 Verdict: not the base, but the best source of ideas in the survey

The owner asked whether clip.hpp *with* the POSIX-strict layer, the operand-span change and the
error-kind change **is** the recommended design, in which case §8 should become a delta over it.
**Measured, it is not** — for two reasons that the three named deltas do not reach:

1. **The mode-group gap (a) is a fourth delta**, and a semantic one, not a layer.
2. **The 29x instantiation cost is architectural.** Removing the GNU surface, the `MaxPositional`
   array and the `match_any` fold — the three things that cause it — leaves a different parser
   with clip's *interface* on top. At that point the delta is larger than the remainder.

So the honest answer is a **hybrid, and clip.hpp is the senior partner on interface**:

| take from clip.hpp | keep from the flat design |
|---|---|
| NTTP `fixed_string` names, so `get<"verbose">()` is compile-checked and reads well | a flat `std::array<option, N>` table and one non-unrolled parse loop |
| `static_assert`ing uniqueness in the `parser` constructor | `consteval` validation that also rejects non-alphanumeric letters and >64 options |
| `maybe<T>` for typed option-arguments — genuinely nicer than a bare `const char*` | `uint64_t seen`/`polarity` masks, and a `size_t operand` index instead of a copied array |
| sink-based `write_help` — no allocation, works with `fputs`, and it is 25 lines | mode groups, `+x` polarity, error kind + offending char |
| the constexpr-testable `parse` and the `std::array` overload for `static_assert` tests | POSIX-strict: an operand ends the options; no `--long`; no `=` |

**§8 is written as that hybrid**, and where it is thinner than clip.hpp — no long options, no
typed values beyond `const char*`, no help writer yet — those are scope choices §10 makes
explicit, not omissions.

---

## 8. The in-house `constexpr` design — built, compiled, measured

Not a sketch. The prototype is at
`scratchpad/proto/optspec.h` (+ `test.cpp`, `dup.cpp`, `ct.cpp`) and everything below reproduces.

### 8.1 The shape

Three types. An `option` row, a `spec<N>` that owns a validated table, and a `result<N>` that a
parse fills in.

```cpp
enum class arg : uint8_t { none, required };   // -r  |  -d X / -dX

struct option {
    char    letter = '\0';
    arg     takes  = arg::none;
    bool    plus   = false;   // admits `+x` as the negation of `-x`
    uint8_t group  = 0;       // mutually exclusive mode group; 0 = none
};

template <size_t N>
struct result {
    uint64_t seen     = 0;      // bit i: table[i] appeared
    uint64_t polarity = 0;      // bit i: it appeared as `+x`
    std::array<const char*, N> value{};   // option-argument, or nullptr
    size_t   operand  = 1;      // argv index where the operands begin
    enum class error : uint8_t { ok, unknown_option, missing_argument } err = error::ok;
    char     bad_letter = '\0';
};
```

`seen` is indexed by **table position**, not by ASCII value. That keeps the mask dense, caps a
table at 64 options (asserted), and leaves room for `set -o`-style long names to join the same
set later without a second mechanism.

`result<N>` is a value type with no indirection: for a six-option table it is
`8 + 8 + 6*8 + 8 + 1 + 1` bytes plus padding. It is returned by value and never allocated.

**`group` is the answer to §3.5's fourth tripwire and to clip.hpp's one unfixable gap.**
`cd -P -L -PL` must mean `-L` and `cd -L -P -LP` must mean `-P` (`cd-p.tst:354`, `:365`), and the
tie-break applies inside a cluster as well as across words. Recording only *whether* a letter
appeared cannot answer it. Group 0 means "ungrouped"; seeing a letter in group *g* clears every
other letter in group *g*, so the last one written is the only one left. This is toybox's
`[-abc]` bracket group (`lib/args.c:345-377`) spelled so a C++ table can validate it. `cd`
declares `-L` and `-P` in group 1 and `-e` ungrouped:

```cpp
constexpr auto kCd = spec{std::array{option{'L', arg::none, false, 1},
                                     option{'P', arg::none, false, 1},
                                     option{'e'}}};
```

Measured, all four orderings and both cluster spellings:

```
cd -P -L -PL  ->  L=1 P=0        cd -LP  ->  L=0 P=1
cd -L -P -LP  ->  L=0 P=1        cd -PL  ->  L=1 P=0
cd -Pe        ->  L=0 P=1 e=1    (an ungrouped letter is unaffected)
10,000 parses ->  0 allocations
```

It cost **18 lines** and did not regress any of §8.6's 17 cases.

### 8.2 The compile-time validation

```cpp
template <size_t N>
consteval bool well_formed(const std::array<option, N>& t) {
    if (N > 64) return false;
    for (size_t i = 0; i < N; ++i) {
        const char c = t[i].letter;
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
        if (!alnum) return false;              // XBD 12.2 Guideline 3
        for (size_t j = i + 1; j < N; ++j)
            if (t[j].letter == c) return false;   // duplicate
    }
    return true;
}
```

The `spec` constructor is `consteval` and, on a bad table, calls a function that is **declared and
deliberately never defined**:

```cpp
void malformed_option_spec_duplicate_or_non_alphanumeric_letter();   // no definition

consteval explicit spec(const std::array<option, N>& t) : table{t} {
    if (!well_formed(t))
        malformed_option_spec_duplicate_or_non_alphanumeric_letter();
}
```

**Why not `throw`:** measured, and this is the one place the prototype's first draft was wrong.
Clang rejects `throw` under `-fno-exceptions` *even inside a `consteval` body that can never be
reached at runtime*:

```
$ clang++ -std=c++23 -fno-exceptions -c ct.cpp
./optspec.h:81:4: error: cannot use 'throw' with exceptions disabled
```

An undefined function is ill-formed in a constant expression under **every** policy, and the
compiler quotes its *name*, which reads better than a string literal would. Verified:

```
$ clang++ -std=c++23 -stdlib=libc++ -fno-exceptions -c dup.cpp
dup.cpp:3:16: error: constexpr variable 'kBad' must be initialized by a constant expression
./optspec.h:93:4: note: non-constexpr function
  'malformed_option_spec_duplicate_or_non_alphanumeric_letter' cannot be used in a constant expression
```

So a duplicate option letter — the defect a hand-rolled `if`-chain hides forever, because the
second branch is simply never reached — **fails the build**, and does so whether or not the tree
ever turns exceptions off.

### 8.3 The parse, in full

The whole of it. Reproduced here rather than referenced, because the scratchpad it was built in
is ephemeral and this is the deliverable someone will want to lift.

```cpp
// POSIX XBD 12.2, and the four rules a hand-rolled loop keeps getting separately
// wrong: clustering (`-abc`), an option-argument attached (`-dX`) or separate
// (`-d X`), `--` ending the options, and a LONE `-` being an operand rather than
// an empty option group.
[[nodiscard]] constexpr result<N> parse(char* const* argv) const noexcept {
    result<N> out;
    size_t at = 1;
    for (; argv[at] != nullptr; ++at) {
        const std::string_view word{argv[at]};
        if (word == "--") { ++at; break; }
        // A lone `-` is an operand - cd's OLDPWD, trap's reset action - and a
        // word that starts with neither sigil ends the options.
        if (word.size() < 2) break;
        const bool minus = word[0] == '-';
        const bool plus  = word[0] == '+';
        if (!minus && !plus) break;

        bool consumed_word = false;
        for (size_t c = 1; c < word.size() && !consumed_word; ++c) {
            const char letter = word[c];
            const size_t i = index_of(letter);
            if (i == N || (plus && !table[i].plus)) {
                out.err = result<N>::error::unknown_option;
                out.bad_letter = letter;
                out.operand = at;
                return out;
            }
            // The mode group, cleared before this letter is recorded, so the
            // last letter written is the only one of its group left set.
            if (table[i].group != 0)
                for (size_t k = 0; k < N; ++k)
                    if (k != i && table[k].group == table[i].group)
                        out.seen &= ~(uint64_t{1} << k);
            out.seen |= uint64_t{1} << i;
            if (plus) out.polarity |=  (uint64_t{1} << i);
            else      out.polarity &= ~(uint64_t{1} << i);   // last one wins
            if (table[i].takes == arg::required) {
                if (c + 1 < word.size()) {
                    out.value[i] = argv[at] + c + 1;   // attached: -dX
                } else if (argv[at + 1] != nullptr) {
                    out.value[i] = argv[++at];         // separate: -d X
                } else {
                    out.err = result<N>::error::missing_argument;
                    out.bad_letter = letter;
                    out.operand = at;
                    return out;
                }
                consumed_word = true;   // the rest of the word was the argument
            }
        }
    }
    out.operand = at;
    return out;
}
```

`index_of` is a linear scan over the table — at most 64 entries, typically two or three, and
constant-folded away entirely when the table is `constexpr` and the letter is known.

### 8.4 The tables, transcribed from the real builtins

```cpp
constexpr auto kCd   = spec{std::array{option{'L'}, option{'P'}, option{'e'}}};
constexpr auto kPwd  = spec{std::array{option{'L'}, option{'P'}}};
constexpr auto kRead = spec{std::array{option{'r'}, option{'d', arg::required}}};
constexpr auto kBind = spec{std::array{option{'l'}, option{'m', arg::required},
                                       option{'N', arg::required}}};
constexpr auto kSet  = spec{std::array{
    option{'a', arg::none, true}, option{'e', arg::none, true},
    option{'f', arg::none, true}, option{'u', arg::none, true},
    option{'x', arg::none, true}, option{'o', arg::required, true}}};
```

One line per utility, against 22–45 lines of loop today.

### 8.5 The parse is a constant expression

The whole parse evaluates at compile time when its inputs do — which means the unit tests for it
can be `static_assert`s that cost nothing at runtime and cannot be skipped by not running the
tests (the same argument `builtins.cpp:2377`'s registry `static_assert` already makes):

```cpp
constexpr bool probe() {
    char w0[] = "read", w1[] = "-rd:", w2[] = "v";
    char* argv[] = {w0, w1, w2, nullptr};
    const auto r = kRead.parse(argv);
    return r.ok() && r.has(0) && r.arg_of(1)[0] == ':' && r.operand == 2;
}
static_assert(probe(), "the whole parse is a constant expression");
```

Compiles clean.

### 8.6 POSIX XBD §12.2 conformance — 17 cases, all passing

```
$ clang++ -std=c++23 -stdlib=libc++ -O2 -o test test.cpp && ./test
=== POSIX XBD 12.2 conformance of the prototype parse ===
=== allocation ===
  10,000 parses -> 0 heap allocations

ALL PASS (0 failures)
```

The cases (each prints only on failure):

| case | asserts |
|---|---|
| `cd -LP /tmp` | clustering, Guideline 5 |
| `read -d: v` | attached option-argument |
| `read -d : v` | separate option-argument |
| `read -rd: v` | cluster ending in an argument-taking option |
| `read -- -r` | `--` terminates |
| `cd -` | a lone `-` is an operand |
| `cd -Z` | unknown option reports the **letter** |
| `cd -LZP` | the right letter inside a cluster |
| `read -d` | missing option-argument |
| `cd -L a b` | operands begin after the options |
| `set -x +x` / `set +x -x` | `+` polarity, last one wins |
| `set -o errexit` / `set -oerrexit` | both attachment forms |
| `cd +L` | `+` rejected where the table does not admit it |
| `bind -memacs` | attached form, which the tree rejects today |

Note the last two rows: the prototype *fixes* the `bind -memacs` inconsistency for free, and
`set -oerrexit` becomes settable. Both are behaviour changes and both must be decided
deliberately — see the opt-out below, and the probe in §9.

### 8.7 Zero heap, and the numbers

Heap counted by replacing global `operator new`:

```
  10,000 parses of `set -eux -o pipefail -- a b`  ->  0 heap allocations
```

Emitted code for one instantiated six-option parse:

```
$ clang++ -std=c++23 -O2 -c size.cpp && llvm-size size.o
__TEXT  __DATA  __OBJC  others   dec   hex
   585       0       0      64   649   289
```

**585 bytes of `__text`.** Each additional utility's table costs another instantiation; the
parse body is small enough that the compiler will often inline and constant-fold it against a
known table.

Under the debug gate's exact flags:

```
$ clang++ -std=c++23 -stdlib=libc++ -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -o test_asan test.cpp
$ ASAN_OPTIONS=detect_leaks=1 ./test_asan
  10,000 parses -> 0 heap allocations
ALL PASS (0 failures)
```

Clean under ASan, UBSan and LSan.

### 8.8 The deviants, and how they opt out

The ticket is right that the grammar deviants are the hard part. They are handled by **not
routing them through the table**, which costs nothing because the table is per-utility data
rather than a framework:

- **`test` / `[`** — not options at all. POSIX's argument-count rules come *first*, and
  `test "$x"` with `x=-n` is a one-argument string test, not a `-n` primary
  (`builtins.cpp:422-429` says exactly this). `test` gets **no spec** and keeps
  `run_test` untouched.
- **`kill -SIG` / `-s`** — `-TERM` and `-15` are signal names in option position. The spec
  handles `-l` and `-s` (with both attachment forms, which `kill` already supports); the
  fall-through for an unrecognised leading `-` stays as the signal reading
  (`builtins.cpp:1450-1453`). This is a per-utility `unknown_option` policy: *reject* for most,
  *hand back to the caller* for `kill`. One extra field on the spec, or simply the caller
  inspecting `result::bad_letter` and deciding — the prototype already exposes it.
- **`set -o name` / `+x`** — the `plus` flag per row covers the polarity; `-o` is
  `arg::required`. The one open decision is whether to keep dash's "bare `-o` lists, attached
  `-oerrexit` also lists" behaviour (§1.2 Axis 3) or take zsh's. That is a deliberate ADR-0001
  divergence call, not a parser question, and the table makes it explicit either way.
- **`trap`** — a lone `-` is the reset action and `trap 2 QUIT` makes every operand a condition.
  Only `-p` is an option; the operand grammar after `result::operand` is untouched.
- **`echo`** — POSIX gives it no options; dash honours `-n`. Keep the six-line special case
  (`builtins.cpp:51-56`) and give it no spec. Routing it through a table would be more code, not
  less.
- **`unalias -a`** — one option, checked before operands. A one-row spec covers it and would
  incidentally fix nothing, because it is already right.

That is the honest accounting: of 24 builtins, roughly **11 route cleanly through the table**
(`cd`, `pwd`, `export`, `readonly`, `unset`, `read`, `bind`, `trap`, `unalias`, `set` partially,
plus `parse_invocation`), `kill` routes with a documented fall-through, and `test`/`echo` opt out
entirely.

### 8.9 What it costs

| | lines |
|---|---|
| prototype header, total (incl. mode groups) | 176 |
| — of which live code | **112** |
| — of which comment (house style) | 64 |
| a uniform `report_option_error()`, budgeted | ~25 |
| **new code** | **~140** |
| hand-rolled loops replaced (13 sites) | **336** |

Net **−196 lines**, plus a compile-time guarantee the tree does not have today, plus one wording
for one error class. That is comfortably inside the ticket's own "~200-400 line" estimate — near
the bottom of it. For scale: dash does the same job for its whole builtin set in 29 shared lines
(§4.3), and clip.hpp does a larger job in 374 (§7).

And the code-size comparison from §7.4, restated because it is the one number that separates the
two candidates:

| | 3 instantiations | 25 instantiations | per builtin |
|---|---|---|---|
| clip.hpp | 7,186 B | 56,419 B | 2,238 B |
| **this design** | **698 B** | **2,392 B** | **77 B** |

---

## 9. Where this contradicts issue #148's brief

Six corrections. Three change the decision.

**9.1 — "The conformance suite tests exact usage errors and statuses — `Illegal option -x`,
status 2 — so a unified reporter is a scoreboard tripwire, not a free refactor." — WRONG, and
this is the big one.** §3 is the evidence. The corpus asserts *stderr non-empty* (`-d`, via
`[ -s "$err_file" ]` at `run-test.sh:346`) and *status non-zero* (`-e n`), 124 and 110 times
respectively. It never compares diagnostic wording anywhere (only 11 `__ERR__` blocks exist,
all about xtrace/`set -v`/stderr plumbing), and `-e 2` is never used for a builtin usage error.
lesh's own tests check status only. **The wording is free to change.** The ticket's central risk
argument does not survive measurement, and the refactor is substantially cheaper than it assumes.

**9.2 — "Exceptions/RTTI policy ... as the tree has them (check CMake)" implies a restrictive
policy. There isn't one.** No `-fno-exceptions` and no `-fno-rtti` appear in any non-vendored
build file (`CMakeLists.txt:41-61` is the whole flag set). The codebase is *de-facto* no-throw —
zero real `throw`/`try`/`catch`, zero `dynamic_cast`/`typeid` in `src/` — but that is convention,
not enforcement. This matters for §6: a throwing library is **undesirable**, not **impossible**,
so its rejection has to be argued rather than asserted. (It also means the in-house design must
not *depend* on exceptions being available — which is why §8.2's first draft, using `throw` in a
`consteval` body, was wrong and had to be changed.)

**9.3 — "The allocation gate: option parsing is on the command path; zero heap." — there is no
such gate today.** `tests/unit/allocation_tests.cpp` has 12 tests covering parse, expand, lex,
log and the editor loop; **none runs a builtin**, and the file never mentions `builtin` or
`argv`. Today's builtins allocate freely on the option path and just past it —
`builtin_set` at `builtins.cpp:1101-1104`, `getopts_arguments` at `:1742-1753`, `read` at
`:1535-1539`. So the constraint is not one the new parser must *satisfy*; it is one the new
parser can *establish*. Prescribed as a probe in §9.

**9.4 — "~25 builtins ... and EVERY one hand-rolls its own option scan." — 24 handlers, and most
have no options at all.** `true`, `false`, `:`, `times`, `exit`, `break`, `continue`, `return`,
`shift` and `alias` scan no options; several of them only discard a leading `--` via the already-
shared `first_operand` (`builtins.h:294-296`). The real count is **13 option-scanning sites
totalling 336 lines** (§1.1). Smaller than the brief implies, which cuts both ways: less code to
delete, but also a more tractable change.

**9.5 — the brief's list of hand-rolled loops is incomplete, and omits the one actual bug.** It
names `cd`/`pwd`, `set`, `trap`, `kill`, `read`, `export`. It misses `readonly` (shares
`export`'s), `unset` (which scans **twice**, by two different rules), `bind`, `unalias` and
`echo`. The `unset` double-read is the most interesting defect found in this survey:
`unset_selects_functions` (`builtins.cpp:2444-2451`) decides the function-vs-variable form with
`arg.find('f') != npos` — a **substring search over the whole word** — while `builtin_unset`
(`:965-982`) does a whole-word comparison that would reject the same input. `unset -qf x`
therefore takes the function path through a word the other reader calls illegal.

**Measured, and it is a real divergence from the ADR-0001 floor:**

```
$ lesh -c 'unset -q X'                        unset: Illegal option -q      (rejected)
$ lesh -c 'f(){ :; }; unset -qf f; f'         f: not found                  (ACCEPTED, f unset, status 0)
$ dash -c 'f(){ :; }; unset -qf f'            unset: Illegal option -q
$ bash -c 'f(){ :; }; unset -qf f'            unset: -q: invalid option
$ zsh  -c 'f(){ :; }; unset -qf f'            unset: bad option: -q
```

lesh accepts an option it rejects one line earlier; all three reference shells reject it. Two
readings of one command line, disagreeing in production — exactly the drift #35's `static_assert`
prevents one layer up. **This is the single strongest concrete argument for the ticket**: one
scanner per utility makes the class of defect unrepresentable, because there is only ever one
reading.

**9.6 — "the shell's own invocation (`main.cpp`'s `-c`/`-i`/`-s`/`-o` handling)" is not in
`main.cpp`.** It is `parse_invocation` in `src/runtime/invocation.cpp:7-127`, deliberately split
out because "main() is not testable, and getting this wrong silently gated 3,600 conformance
assertions" (`invocation.h:14-21`). `main.cpp:47-58` only *renders* the error. Minor, but the
seam already exists and is the reason this site is the easiest of the thirteen to convert.

**One thing the brief gets exactly right and deserves saying:** the deviants really are the hard
part, and the ticket names all four of the ones that matter (`test`, `set -o`/`+x`, `kill -SIG`,
`trap`). §8.8 handles each, and the answer in every case is *opt out per utility*, which is only
cheap because the table is data rather than a framework.

**Two non-bugs this survey nearly reported, and did not.** `set -oerrexit` silently listing
instead of setting, and `sh -oerrexit -c CMD` consuming the `-c`, both look like clear defects —
but dash and bash do the same thing, and ADR-0001 makes dash the floor (§1.2 Axis 3, measured
against all three reference shells). Only zsh differs. They are recorded here as *internal
inconsistency* — `read -d:` works where `bind -memacs` does not — rather than as conformance
failures.

---

## 10. Recommendation, and the probes to run before committing

### 10.1 Recommendation: in-house, and take it in three steps

**Write the `constexpr` spec table in-house, as a hybrid that takes clip.hpp's interface onto a
flat loop-based core.** The reasoning, in priority order:

1. **Almost nothing off the shelf clears the zero-heap bar** (§6). Of thirteen libraries
   measured, exactly one third-party candidate does — `etched`, a 6-star repo with no release
   tags — and clip.hpp does. Every mainstream option (CLI11, cxxopts, argparse, Lyra, clipp,
   argh, Taywee/args, Boost.PO) begins by copying `argv` into a `std::vector<std::string>`, which
   is architectural rather than incidental.
2. **The thing being replaced is 336 lines of loop, and the replacement is 112 lines of live
   code.** A dependency that is larger than the code it deletes is a bad trade under ADR-0005's
   own framing ("a library that does a job better than a hand-rolled equivalent"), and none of
   the candidates does this job *better* — they do a much larger job, GNU-long-option-first,
   with allocation and exceptions, for a shell that needs POSIX short options and nothing else.
3. **The compile-time duplicate-letter check is the feature with the best defect-prevention
   ratio.** Of everything surveyed only clip.hpp (`clip.hpp:352-368`) and `etched` offer it at
   all; no mainstream library does, and neither toybox nor busybox manages it for the defect
   that actually bites them (§5.2).
4. **The tree already contains a correct POSIX parser** in `getopts_step`
   (`builtins.cpp:1851-1930`). This is not new science; it is that logic lifted free of
   `OPTIND`/`OPTARG`.
5. **`getopt(3)` itself is not an option, and this deserves stating** since it is the obvious
   "why not just use libc" answer. Its state (`optind`, `optarg`, `optopt`) is **process-global
   and non-reentrant**, and it **permutes `argv` in place**. A shell builtin can be invoked from
   inside another builtin's execution — `command`, a trap body, `eval` — so a global parser
   cursor is unusable. busybox, which does wrap `getopt_long`, has to defensively reset it before
   every call (`libbb/getopt32.c:527`) with a comment explaining the re-entry path it was bitten
   by, and the correct reset value differs between glibc and BSD
   (`include/libbb.h:1385-1389`). §5.2 has the detail.

6. **clip.hpp is the right teacher and the wrong base** (§7.5). It is closer than anything else
   found, and its NTTP-named accessors, `maybe<T>`, sink-based help and constexpr-testable parse
   should all be taken. But it costs **2,238 bytes per builtin against 77** because its parse
   body is an unrolled fold per option; it is GNU-shaped where POSIX is required; and it cannot
   express `cd -P -L -PL` at all. Stripping the three things that cause the bloat leaves a
   different parser wearing clip's interface — which is exactly what §8 is.
7. **Two of the deltas are not optional.** Mode groups (§8.1) are required by `cd-p.tst:354` and
   `:365`; the operand *index* rather than a fixed array is required by `echo`, `export`, `unset`
   and `kill`, whose operand lists are unbounded.

**Three steps, each independently landable:**

- **Step 1 — the library, with no callers.** Land `src/runtime/optspec.h` plus a unit-test file
  of `static_assert`s, the 17 POSIX cases, and the four `cd -P -L -PL` orderings. Read Appendix A
  (clip.hpp) first and lift its `fixed_string`/`get<"name">()` accessors deliberately —
  measure the instantiation cost again after doing so, because NTTP names are the one clip idea
  that could reintroduce per-builtin template growth. Zero risk: nothing links it yet, and per CLAUDE.md
  a change confined to new files provably cannot move the corpus. Add the allocation test
  (§10.2 probe 3) here.
- **Step 2 — convert the safe sites.** `cd`/`pwd`, `export`/`readonly`, `unset` (fixing the
  double-read), `trap`, `bind`, `unalias`, and `parse_invocation`. Each is one commit with a
  before/after per-file conformance number. `bind -memacs` starts working; decide that
  deliberately.
- **Step 3 — the deviants, or not at all.** `set` (with the `-o` attachment decision recorded per
  ADR-0001), `kill` (with the documented `unknown_option` fall-through). `test` and `echo` are
  never converted, and the note should say so in the header comment so nobody tries later.

Steps 1 and 2 are the value. Step 3 is optional and can be declined without stranding anything.

### 10.2 Probe prescription

Five probes, cheapest first. The first three are gates; the last two are decisions.

**Probe 1 — baseline the per-file conformance score, in this environment, today.** Per CLAUDE.md
totals do not reproduce across environments and four tickets have opened on a stale headline
number. Before touching anything:

```sh
cmake --preset release && cmake --build --preset release -j8
python3 tools/conformance.py            # ~2 min, release binary
```

Record **per-file** numbers for the files that exercise option handling: `cd-p.tst`,
`export-p.tst`, `readonly-p.tst`, `unset-p.tst`, `trap1-p.tst`…`trap*-p.tst`, `kill*-p.tst`,
`read-p.tst`, `set-p.tst`, `option-p.tst`, `startup-p.tst`, `getopts-p.tst`, `alias-p.tst`,
`shift-p.tst`. Quote deltas against those, never against a remembered total.

**Probe 2 — the sanitized gate, which is the real gate.**

```sh
ctest --preset debug
```

`kill` and `trap` are signal-adjacent and ASan intercepts signals; this is where a boundary input
bites (#59, #62, #63 each found real UB from a one-line input the conformance suite never sends).
Never report the change verified without it.

**Probe 3 — establish the allocation invariant that §9.3 shows does not exist.** Add to
`tests/unit/allocation_tests.cpp`, in the style of the file's existing tests:

```cpp
TEST_F(AllocationTest, BuiltinOptionScanningNeverAllocates) {
    for (const char* line : {"cd -LP /tmp", "export -p", "unset -v X",
                             "trap -p INT", "read -rd: v", "set -eux -o pipefail -- a b"}) {
        metrics::allocations().reset();
        /* parse only, via the spec table */
        EXPECT_EQ(heap(), 0u) << "option scan for \"" << line << "\" fell back to malloc";
    }
}
```

Note it must gate the **scan**, not the whole builtin — `set` legitimately allocates for the
positional parameters afterwards (`builtins.cpp:1101-1104`). Getting that boundary right is
itself a design forcing-function for where the parse ends.

**Probe 4 — decide the two behaviour changes, explicitly.** The spec table makes
`bind -memacs` and `set -oerrexit` work. `bind` is lesh's own surface (#117) with no reference
shell and no corpus coverage — take the fix. `set -oerrexit` is an ADR-0001 question: dash lists,
zsh sets. Recommend **keeping dash's behaviour** as an explicit opt-out with the reasoning
recorded in the table, since ADR-0001 makes dash the floor. **Verified, not assumed:**
`option-p.tst` contains zero occurrences of `set -o`/`set +o`, and no attached `-oname` form
appears anywhere in the 122-file corpus — so no case covers this either way and the choice is
free. It should still be recorded per ADR-0001 rather than left to fall out of the code.

**Probe 4b — the last-one-wins cases, specifically.** Before and after, run:

```sh
./build/release/lesh -c 'cd -P -L -PL /tmp && pwd'    # must print the LOGICAL path
./build/release/lesh -c 'cd -L -P -LP /tmp && pwd'    # must print the PHYSICAL path
```

and check `cd-p.tst`'s per-file score. This is the one tripwire in §3 that a naive table-driven
unification really does trip, and it is cheap to watch.

**Probe 5 — confirm the wording change is really free.** After step 2, diff stderr across the
whole corpus before and after:

```sh
python3 tools/conformance.py 2>&1 | tee after.txt
diff before.txt after.txt
```

Expect zero score change. If any file moves, §3's conclusion is wrong somewhere and the
unification must stop until it is understood. This is the cheap insurance against the one claim
in this note that would be expensive to have gotten wrong.

**Do not run `third_party/yash-tests/run-test.sh` directly** — no timeout, reaps nothing, and it
has cost this machine nineteen minutes on one hung case. `tools/conformance.py` gives every case
its own process group.

---

## Sources

All fetched or cloned 2026-08-26 and read at these commits.

| what | repo / path | commit |
|---|---|---|
| lesh itself | `nanov/lesh` (branch `leshper`) | `0251f61137d9fd8ecae55dc91e057c857d2210a0` (cited files unchanged at `f592d8b`) |
| yash | `magicant/yash` (branch `trunk`) | `46389f6f6cf9ce18d318f7af1c65f0a157ca1ed4` |
| zsh | `zsh-users/zsh` | `17af4a46c8983d9b1c164868fe30c0b0738c1e58` |
| bash | `git.savannah.gnu.org/git/bash.git` (the `bminor/bash` mirror is 404) | `b460816602167718f78a6233164e8875f49b75b2` |
| fish | `fish-shell/fish-shell` | `8510d5906737d6a7fe4537413288066aa881b51f` |
| dash | `herbertx/dash` (0.5.13.5) | `037bbdfd330017c368caf6242f977974123239b5` |
| toybox | `landley/toybox` | `b7ec52ac35e075caffca5d330995d44e8dbfc8c3` |
| busybox | `mirror/busybox` | `371fe9f71d445d18be28c82a2a6d82115c8af19d` |
| sbase | `michaelforney/sbase` | `b30fb56804bfed69b45ef0e944d2e029e4d26258` |
| uutils/coreutils | `uutils/coreutils` | `728388e76b04cc7d6015974060201869b71a73e7` |
| CLI11 | `CLIUtils/CLI11` | `c1cfe00` |
| cxxopts | `jarro2783/cxxopts` | `b613531` |
| argparse | `p-ranav/argparse` | `d924b84` |
| Lyra | `bfgroup/Lyra` | `da60100` |
| clipp | `muellan/clipp` | `2c32b2f` |
| argh | `adishavit/argh` | `c3f0d8c` |
| args | `Taywee/args` | `903b07d` |
| Boost.ProgramOptions | `boostorg/program_options` | `e173046` |
| etched | `DavisLCVB/etched` | `716ae73` |
| arg_router | `cmannett85/arg_router` | `e04ed2b` |
| argum | `gershnik/argum` | `e1e64e8` |
| flags | `sailormoon/flags` | `8f6adfe` |
| docopt.cpp | `docopt/docopt.cpp` | `05d507d` |
| clip.hpp | supplied by the owner, not a public repo | Appendix A below |

Standards consulted: POSIX.1-2024 (Issue 8) XBD §12.2 *Utility Syntax Guidelines*, XCU §1.4
*Utility Description Defaults*, XCU §2.8.1 *Consequences of Shell Errors*, and the XCU pages for
`cd`, `pwd`, `set`, `read`, `trap`, `kill`, `unset`, `export`, `readonly`, `getopts` and `test`.

Toolchain for every measurement: Homebrew clang at `/opt/homebrew/opt/llvm/bin/clang++`,
`-std=c++23 -stdlib=libc++`, on `arm64-apple-darwin25.2.0`. Prototype and clip.hpp test
harnesses were written under the session scratchpad
(`scratchpad/proto/`, `scratchpad/cliptest/`) and are **not** added to the repository — per
this ticket's terms the note is the only new file. The prototype is reproduced in full in §8
and clip.hpp in Appendix A, so both are recoverable from this document alone.

---

## Appendix A — `clip.hpp` verbatim

Included in the note itself rather than as a sibling file. Two reasons: this research task's
terms are one new Markdown file, and the existing two notes in
`docs/superpowers/research/` are each self-contained with no companion artifacts, so a loose
`.hpp` beside them would be a new convention. If the owner would rather have it as a compilable
sibling — `docs/superpowers/research/2026-08-26-clip.hpp` — it is a straight copy out of the
fence below, and that is the better choice if anyone intends to `#include` it rather than read
it.

374 lines, as supplied.

````cpp
// clip.hpp — a single-header, constexpr-friendly command-line parser (C++20).
//
// The option table is a compile-time value, option names are checked at
// compile time (get<"typo"> is a compile error), and parse() itself is
// constexpr, so you can unit-test your CLI with static_assert.
//
//   constexpr auto cli = clip::parser{
//       clip::flag<"verbose", 'v'>("verbose output"),
//       clip::opt<"count", int, 'c'>("iterations", 10),   // with default
//       clip::opt<"name", std::string_view>("a name"),     // no short form
//   };
//
//   auto r = cli.parse(argc, argv);
//   if (!r) { /* r.error / r.error_arg */ }
//   bool v  = r.get<"verbose">();        // flags -> bool
//   auto c  = r.get<"count">();          // options -> clip::maybe<T>
//
// Supports: --name value, --name=value, -c value, -c3, -c=3, grouped
// flags (-vx), "--" terminator, positional arguments.

#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace clip {

// ---------------------------------------------------------------- fixed_string
// String usable as a non-type template parameter.
template <std::size_t N>
struct fixed_string {
    char data[N]{};

    constexpr fixed_string(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    constexpr std::string_view view() const { return {data, N - 1}; }
    constexpr bool operator==(const fixed_string&) const = default;
};

// ---------------------------------------------------------------- maybe<T>
// Minimal optional that is fully constexpr in C++20 (std::optional's
// assignment only became constexpr with C++23 / P2231). Requires T to be
// default-constructible, which holds for all supported value types.
template <class T>
struct maybe {
    T val{};
    bool set = false;

    constexpr maybe() = default;
    constexpr maybe(T v) : val(v), set(true) {}

    constexpr explicit operator bool() const { return set; }
    constexpr bool has_value() const { return set; }
    constexpr const T& operator*() const { return val; }
    constexpr const T& value() const { return val; }
    constexpr T value_or(T fb) const { return set ? val : fb; }
};

// ---------------------------------------------------------------- value parsing
namespace detail {

template <class T>
constexpr maybe<T> parse_integral(std::string_view s) {
    if (s.empty()) return {};
    bool neg = false;
    std::size_t i = 0;
    if (s[0] == '+' || s[0] == '-') {
        neg = (s[0] == '-');
        if (s.size() == 1) return {};
        i = 1;
    }
    if constexpr (std::is_unsigned_v<T>) {
        if (neg) return {};
    }
    using U = std::make_unsigned_t<T>;
    const U limit = neg ? U(std::numeric_limits<T>::max()) + U(1)
                        : U(std::numeric_limits<T>::max());
    U acc = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return {};
        const U d = U(c - '0');
        if (acc > (limit - d) / 10) return {};  // overflow
        acc = acc * 10 + d;
    }
    return neg ? T(U(0) - acc) : T(acc);  // two's complement, well-defined in C++20
}

template <class>
inline constexpr bool always_false = false;

template <class T>
constexpr maybe<T> parse_value(std::string_view s) {
    if constexpr (std::is_same_v<T, std::string_view>) {
        return s;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
        return {};
    } else if constexpr (std::is_integral_v<T>) {
        return parse_integral<T>(s);
    } else if constexpr (std::is_enum_v<T>) {
        auto v = parse_integral<std::underlying_type_t<T>>(s);
        return v ? maybe<T>(static_cast<T>(*v)) : maybe<T>{};
    } else {
        static_assert(always_false<T>,
                      "unsupported option type: use an integral type, bool, "
                      "enum, or std::string_view");
    }
}

}  // namespace detail

// ---------------------------------------------------------------- option spec
template <fixed_string Name, char Short, class T, bool IsFlag>
struct option {
    using value_type = T;
    static constexpr std::string_view name = Name.view();
    static constexpr char short_name = Short;
    static constexpr bool is_flag = IsFlag;

    std::string_view help{};
    maybe<T> default_value{};
};

// A boolean switch: present -> true. Also accepts --name=false etc.
template <fixed_string Name, char Short = '\0'>
constexpr auto flag(std::string_view help = {}) {
    return option<Name, Short, bool, true>{help, {}};
}

// An option that takes a value, without / with a default.
template <fixed_string Name, class T, char Short = '\0'>
constexpr auto opt(std::string_view help = {}) {
    return option<Name, Short, T, false>{help, {}};
}
template <fixed_string Name, class T, char Short = '\0'>
constexpr auto opt(std::string_view help, T default_value) {
    return option<Name, Short, T, false>{help, maybe<T>(default_value)};
}

// ---------------------------------------------------------------- parser
template <class... Opts>
struct parser {
    std::tuple<Opts...> opts;

    constexpr explicit parser(Opts... o) : opts(o...) {
        static_assert(names_unique(), "duplicate option names");
        static_assert(shorts_unique(), "duplicate short option names");
    }

    template <fixed_string Name>
    static constexpr std::size_t index_of() {
        constexpr std::array<std::string_view, sizeof...(Opts)> names{
            Opts::name...};
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == Name.view()) return i;
        return sizeof...(Opts);
    }

    // ------------------------------------------------------------ result
    template <std::size_t MaxPositional = 16>
    struct result {
        std::tuple<maybe<typename Opts::value_type>...> values{};
        std::array<std::string_view, MaxPositional> positional{};
        std::size_t positional_count = 0;
        std::string_view error{};      // empty == success
        std::string_view error_arg{};  // the offending token

        constexpr explicit operator bool() const { return error.empty(); }

        template <fixed_string Name>
        constexpr decltype(auto) get() const {
            constexpr std::size_t I = index_of<Name>();
            static_assert(I < sizeof...(Opts), "no option with this name");
            using Opt = std::tuple_element_t<I, std::tuple<Opts...>>;
            if constexpr (Opt::is_flag)
                return std::get<I>(values).value_or(false);
            else
                return std::get<I>(values);  // const maybe<T>&
        }

        template <fixed_string Name>
        constexpr auto value_or(auto fallback) const {
            constexpr std::size_t I = index_of<Name>();
            static_assert(I < sizeof...(Opts), "no option with this name");
            using T = typename std::tuple_element_t<
                I, std::tuple<Opts...>>::value_type;
            return std::get<I>(values).value_or(T(fallback));
        }

        constexpr void fail(std::string_view msg, std::string_view arg) {
            if (error.empty()) { error = msg; error_arg = arg; }
        }
    };

    // ------------------------------------------------------------ parse
    template <std::size_t MaxPositional = 16>
    constexpr result<MaxPositional> parse(int argc, const char* const* argv,
                                          bool skip_program_name = true) const {
        result<MaxPositional> r{};

        // Seed defaults.
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(opts).default_value
                  ? (std::get<I>(r.values) = std::get<I>(opts).default_value, 0)
                  : 0),
             ...);
        }(std::index_sequence_for<Opts...>{});

        bool only_positional = false;

        for (int i = skip_program_name ? 1 : 0;
             i < argc && r.error.empty(); ++i) {
            const std::string_view arg{argv[i]};

            // Positional argument (including "-" by convention).
            if (only_positional || arg.size() < 2 || arg[0] != '-') {
                if (r.positional_count >= MaxPositional)
                    r.fail("too many positional arguments", arg);
                else
                    r.positional[r.positional_count++] = arg;
                continue;
            }

            if (arg == "--") { only_positional = true; continue; }

            if (arg[1] == '-') {  // ---- long option: --name[=value]
                const std::string_view body = arg.substr(2);
                const std::size_t eq = body.find('=');
                const bool has_inline = eq != std::string_view::npos;
                const std::string_view name =
                    has_inline ? body.substr(0, eq) : body;
                const std::string_view inline_val =
                    has_inline ? body.substr(eq + 1) : std::string_view{};

                const bool matched = match_any([&](auto ic) {
                    constexpr std::size_t I = decltype(ic)::value;
                    using Opt = std::tuple_element_t<I, std::tuple<Opts...>>;
                    if (Opt::name != name) return false;

                    if constexpr (Opt::is_flag) {
                        if (has_inline) {
                            auto v = detail::parse_value<bool>(inline_val);
                            if (!v) { r.fail("invalid value for option", arg); return true; }
                            std::get<I>(r.values) = *v;
                        } else {
                            std::get<I>(r.values) = true;
                        }
                    } else {
                        std::string_view sval;
                        if (has_inline) sval = inline_val;
                        else if (i + 1 < argc) sval = std::string_view{argv[++i]};
                        else { r.fail("missing value for option", arg); return true; }

                        auto v = detail::parse_value<typename Opt::value_type>(sval);
                        if (!v) { r.fail("invalid value for option", arg); return true; }
                        std::get<I>(r.values) = *v;
                    }
                    return true;
                });
                if (!matched) r.fail("unknown option", arg);

            } else {  // ---- short option cluster: -v, -vx, -c3, -c=3, -c 3
                const std::string_view body = arg.substr(1);
                bool consumed_rest = false;

                for (std::size_t j = 0;
                     j < body.size() && r.error.empty() && !consumed_rest; ++j) {
                    const char c = body[j];

                    const bool matched = match_any([&](auto ic) {
                        constexpr std::size_t I = decltype(ic)::value;
                        using Opt = std::tuple_element_t<I, std::tuple<Opts...>>;
                        if (Opt::short_name == '\0' || Opt::short_name != c)
                            return false;

                        if constexpr (Opt::is_flag) {
                            std::get<I>(r.values) = true;
                        } else {
                            std::string_view rest = body.substr(j + 1);
                            if (!rest.empty() && rest.front() == '=')
                                rest.remove_prefix(1);

                            std::string_view sval;
                            if (!rest.empty()) { sval = rest; consumed_rest = true; }
                            else if (i + 1 < argc) sval = std::string_view{argv[++i]};
                            else { r.fail("missing value for option", arg); return true; }

                            auto v = detail::parse_value<typename Opt::value_type>(sval);
                            if (!v) { r.fail("invalid value for option", arg); return true; }
                            std::get<I>(r.values) = *v;
                        }
                        return true;
                    });
                    if (!matched) r.fail("unknown option", arg);
                }
            }
        }
        return r;
    }

    // Convenience overload for std::array (handy in static_assert tests).
    template <std::size_t N, std::size_t MaxPositional = 16>
    constexpr auto parse(const std::array<const char*, N>& args,
                         bool skip_program_name = true) const {
        return parse<MaxPositional>(int(N), args.data(), skip_program_name);
    }

    // ------------------------------------------------------------ help
    // Sink-based so it works with anything: fwrite, std::cout, a string...
    //   cli.write_help("prog", [](std::string_view s){ std::cout << s; });
    template <class Out>
    constexpr void write_help(std::string_view prog, Out out) const {
        out("usage: "); out(prog); out(" [options] [args...]\n\noptions:\n");
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (write_one<I>(out), ...);
        }(std::index_sequence_for<Opts...>{});
    }

private:
    template <class F>
    static constexpr bool match_any(F&& f) {
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return (f(std::integral_constant<std::size_t, I>{}) || ...);
        }(std::index_sequence_for<Opts...>{});
    }

    template <std::size_t I, class Out>
    constexpr void write_one(Out& out) const {
        using Opt = std::tuple_element_t<I, std::tuple<Opts...>>;
        std::size_t col = 0;
        auto emit = [&](std::string_view s) { out(s); col += s.size(); };

        emit("  ");
        if constexpr (Opt::short_name != '\0') {
            const char buf[4] = {'-', Opt::short_name, ',', ' '};
            emit({buf, 4});
        } else {
            emit("    ");
        }
        emit("--"); emit(Opt::name);
        if constexpr (!Opt::is_flag) emit(" <value>");
        while (col < 28) emit(" ");
        out(std::get<I>(opts).help);
        out("\n");
    }

    static constexpr bool names_unique() {
        constexpr std::array<std::string_view, sizeof...(Opts)> n{Opts::name...};
        for (std::size_t i = 0; i < n.size(); ++i)
            for (std::size_t j = i + 1; j < n.size(); ++j)
                if (n[i] == n[j]) return false;
        return true;
    }
    static constexpr bool shorts_unique() {
        constexpr std::array<char, sizeof...(Opts)> s{Opts::short_name...};
        for (std::size_t i = 0; i < s.size(); ++i)
            for (std::size_t j = i + 1; j < s.size(); ++j)
                if (s[i] != '\0' && s[i] == s[j]) return false;
        return true;
    }
};

template <class... Opts>
parser(Opts...) -> parser<Opts...>;

}  // namespace clip
````
