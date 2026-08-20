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
