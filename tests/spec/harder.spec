# Harder POSIX cases, added when the corpus first reached 100%.
#
# A scoreboard everything passes has stopped measuring. These probe the corners
# where shells actually differ, so the number keeps meaning something.

--- nested command substitution inside a quoted string [xfail(legacy): beyond legacy's single-pass parser]
echo "outer $(echo "inner $(echo deep)") end"

--- a pipeline inside command substitution [xfail(legacy): beyond legacy's single-pass parser]
echo $(echo a b c | tr ' ' '-')

--- a for loop over a command substitution [xfail(legacy): beyond legacy's single-pass parser]
for x in $(echo a b c); do printf '[%s]' "$x"; done; echo

--- a case inside a function inside a loop [xfail(legacy): beyond legacy's single-pass parser]
f() { case $1 in a*) echo A;; *) echo O;; esac; }; for x in ab cd; do f $x; done

--- an if whose condition is a pipeline [xfail(legacy): beyond legacy's single-pass parser]
if echo hi | grep -q hi; then echo found; fi

--- nested parameter expansion defaults [xfail(legacy): beyond legacy's single-pass parser]
x=; echo "${x:-${HOME:-fallback}}"

--- arithmetic inside a parameter default [xfail(legacy): beyond legacy's single-pass parser]
echo "${undefined:-$((6 * 7))}"

--- trimming applied to a command substitution result [xfail(legacy): beyond legacy's single-pass parser]
p=$(echo /a/b/c.txt); echo "${p##*/}"

--- a redirection inside a loop body [xfail(legacy): beyond legacy's single-pass parser]
d=$(mktemp -d); for x in a b; do echo $x >> $d/f; done; cat $d/f

--- a here-document inside a function [xfail(legacy): beyond legacy's single-pass parser]
f() { cat <<EOT
from a function
EOT
}; f

--- exit status propagates out of a function through a pipeline [xfail(legacy): beyond legacy's single-pass parser]
f() { return 2; }; f | cat; echo $?

--- a subshell does not leak function definitions [xfail(legacy): beyond legacy's single-pass parser]
(g() { echo inner; }); g 2>/dev/null || echo not-defined

--- IFS affects splitting of a command substitution [xfail(legacy): beyond legacy's single-pass parser]
IFS=:; x=$(echo a:b:c); for i in $x; do printf '[%s]' "$i"; done; echo

--- backslash continues a line [xfail(legacy): beyond legacy's single-pass parser]
echo one\
two

--- an empty command list between separators [xfail(legacy): beyond legacy's single-pass parser]
echo a;; echo b

--- an alias is not substituted in the same parse unit [xfail(legacy): legacy's alias model differs]
alias g="echo hi"; g 2>/dev/null; echo after

--- unalias is a builtin [xfail(legacy): legacy's alias model differs]
unalias nothing 2>/dev/null; echo ok

--- a quoted command name bypasses alias substitution [xfail(legacy): legacy's alias model differs]
alias ls=echo; \ls -d /tmp 2>/dev/null | head -1

--- eval runs its arguments as shell input [xfail(legacy): not implemented in legacy]
eval "echo from-eval"

--- eval sets state in the current environment [xfail(legacy): not implemented in legacy]
eval "x=set-by-eval"; echo $x

--- read splits a line on IFS [xfail(legacy): not implemented in legacy]
echo "a b c" | { read x y z; echo "$z-$y-$x"; }

--- the last variable of read takes the remainder [xfail(legacy): not implemented in legacy]
echo "one two three" | { read a b; echo "[$a][$b]"; }

--- a while loop can read a stream [xfail(legacy): not implemented in legacy]
printf 'a\nb\n' | while read l; do echo "[$l]"; done

--- command runs a command bypassing functions [xfail(legacy): not implemented in legacy]
f() { echo function; }; command echo not-the-function

--- command -v reports a builtin
command -v cd

--- a for loop can be redirected [xfail(legacy): not implemented in legacy]
d=$(mktemp -d); for x in a b; do echo $x; done > $d/f; cat $d/f

--- an if can be redirected [xfail(legacy): not implemented in legacy]
d=$(mktemp -d); if true; then echo yes; fi > $d/f; cat $d/f

--- a compound command with a bad redirection spares the shell [xfail(legacy): not implemented in legacy]
if echo x; then echo y; fi <_no_such_dir_/foo 2>/dev/null; printf 'reached\n'

--- a for loop without in iterates the positional parameters [xfail(legacy): not implemented in legacy]
set -- x y; for i do echo $i; done

--- a for loop without in inside a function [xfail(legacy): not implemented in legacy]
f() { for i do echo $i; done; }; f a b

--- for with an empty in list iterates nothing [xfail(legacy): not implemented in legacy]
set -- a; for i in; do echo never; done; echo after

--- a background command does not block the shell [xfail(legacy): legacy runs `&` in the foreground]
d=$(mktemp -d); mkfifo $d/f; cat $d/f & echo piped > $d/f; wait

--- wait reports the background command's status [xfail(legacy): legacy has no asynchronous lists]
true & wait; echo $?

--- an ampersand is not a semicolon [xfail(legacy): legacy has no asynchronous lists]
echo one & echo two; wait

--- an EXIT trap runs on normal completion [xfail(legacy): legacy has no signal handling]
trap "echo caught" EXIT; echo body

--- an EXIT trap runs on explicit exit [xfail(legacy): legacy has no signal handling]
trap "echo bye" EXIT; exit 0

--- trap lists in re-inputtable form [xfail(legacy): legacy has no signal handling]
trap "echo x" INT; trap

--- trap with an empty action lists as ignore [xfail(legacy): legacy has no signal handling]
trap "" INT; trap

--- a trap fires when its signal arrives [xfail(legacy): legacy has no signal handling]
trap "echo got" USR1; kill -USR1 $$; echo after

--- trap - resets to default [xfail(legacy): legacy has no signal handling]
trap "echo a" USR1; trap - USR1; trap

--- a subshell keeps an ignored trap [xfail(legacy): legacy has no signal handling]
trap "" USR1; (trap)
