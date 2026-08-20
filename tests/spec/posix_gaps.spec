# Constructs the new front end does not implement yet, one ticket per group.
#
# NOTE ON REFERENCE SHELLS. Two cases here legitimately differ under zsh, and
# lesh matching dash is CORRECT (ADR-0001: dash is authoritative for the POSIX
# floor, zsh only for the curated layer, dash wins on conflict):
#
#   - zsh does not glob the result of an unquoted expansion without `globsubst`
#   - zsh errors on a pattern that matches nothing rather than passing it through
#
# Do not "fix" lesh toward zsh here. ctest gates on dash for exactly this reason.
#
# These exist so the scoreboard keeps measuring. A corpus everything passes has
# stopped being a compass, and the markers name which ticket closes each gap.

--- redirection to a file [xfail(legacy): legacy ignores redirect nodes entirely]
echo hi > /tmp/lesh_spec_redirect && cat /tmp/lesh_spec_redirect

--- append redirection [xfail(legacy): legacy ignores redirect nodes entirely]
echo a > /tmp/lesh_spec_app && echo b >> /tmp/lesh_spec_app && cat /tmp/lesh_spec_app

--- stderr redirection [xfail(legacy): legacy ignores redirect nodes entirely]
ls /nonexistent 2>/dev/null; echo done

--- if statement [xfail(legacy): legacy has no compound commands]
if true; then echo yes; fi

--- if else [xfail(legacy): legacy has no compound commands]
if false; then echo yes; else echo no; fi

--- while loop [xfail(legacy): legacy has no compound commands]
i=0; while [ $i -lt 2 ]; do echo $i; i=$((i+1)); done

--- for loop [xfail(legacy): legacy has no compound commands]
for x in a b c; do echo $x; done

--- case statement [xfail(legacy): legacy has no compound commands]
case abc in a*) echo matched;; *) echo no;; esac

--- subshell [xfail(legacy): legacy has no compound commands]
(echo inside)

--- brace group [xfail(legacy): legacy has no compound commands]
{ echo grouped; }

--- default value expansion [xfail(legacy): legacy has no parameter expansion beyond $name]
echo ${undefined_var:-fallback}

--- length expansion [xfail(legacy): legacy has no parameter expansion beyond $name]
x=hello; echo ${#x}

--- suffix trimming [xfail(legacy): legacy has no parameter expansion beyond $name]
x=file.txt; echo ${x%.txt}

--- prefix trimming [xfail(legacy): legacy has no parameter expansion beyond $name]
x=/a/b/c; echo ${x##*/}

--- exit status parameter [xfail(legacy): legacy has no parameter expansion beyond $name]
false; echo $?

--- positional parameters [xfail(legacy): legacy has no parameter expansion beyond $name]
set -- one two; echo $1 $2

--- glob expansion [xfail(legacy): legacy has no pathname expansion]
echo /usr/bin/tru*

--- cd builtin [xfail(legacy): legacy forks builtins, so cd changes the child and exits]
cd /tmp && pwd

--- variable assignment persists [xfail(legacy): legacy parses assignments and applies them only via setenv in the child]
x=value; echo $x

--- function definition and call [xfail(legacy): legacy has no functions]
greet() { echo hello; }; greet

--- here-document [xfail(legacy): legacy has no here-documents]
cat <<EOT
body
EOT

--- cd keeps a logical PWD through a symlink [xfail(legacy): legacy resolves the physical path]
cd /tmp && pwd

--- cd .. is lexical, not physical [xfail(legacy): legacy resolves the physical path]
cd /tmp && cd .. && pwd

--- export makes a variable visible to a child [xfail(legacy): legacy's export path is incomplete]
export EXPORTED_VAR=visible; /usr/bin/env | /usr/bin/grep '^EXPORTED_VAR='

--- an unexported variable is not visible to a child [xfail(legacy): legacy leaks assignments into the environment]
LOCAL_VAR=hidden; /usr/bin/env | /usr/bin/grep -c '^LOCAL_VAR=' || true

--- assignment prefixed to a command does not persist [xfail(legacy): legacy applies prefixed assignments with setenv, so they persist]
x=outer; x=inner /usr/bin/true; echo $x

--- exit sets the status [xfail(legacy): legacy matches `exit` as a string in the REPL rather than as a builtin]
exit 3

--- quoting suppresses pathname expansion
echo "*.md"

--- an unquoted expansion result is globbed [xfail(legacy): legacy has no pathname expansion]
p='/usr/bin/tru*'; echo $p

--- a pattern that matches nothing expands to itself
echo /tmp/definitely_no_match_here_zzz_*

--- assignment values are expanded, not stored raw [xfail(legacy): legacy stores assignment values as raw source text]
y=world; x="hello $y"; echo "[$x]"

--- break leaves the loop [xfail(legacy): legacy has no compound commands]
for x in a b c; do if [ $x = b ]; then break; fi; echo $x; done

--- continue skips an iteration [xfail(legacy): legacy has no compound commands]
for x in a b c; do if [ $x = b ]; then continue; fi; echo $x; done

--- case patterns are not pathname-expanded [xfail(legacy): legacy has no compound commands]
case xyz in a*) echo matched;; *) echo fallback;; esac

--- alternative case patterns [xfail(legacy): legacy has no compound commands]
case b in a|b|c) echo alt;; esac

--- a subshell does not leak its changes [xfail(legacy): legacy has no compound commands]
x=1; (x=2); echo $x

--- a reserved word after a command name is an argument
echo done fi esac

--- until loops until true [xfail(legacy): legacy has no compound commands]
until true; do echo never; done; echo after

--- input redirection [xfail(legacy): legacy ignores redirect nodes entirely]
d=$(mktemp -d); echo content > $d/f; cat < $d/f

--- 2>&1 merges stderr into stdout [xfail(legacy): legacy ignores redirect nodes entirely]
ls /nonexistent_zzz 2>&1 | grep -c .

--- a redirection target is expanded [xfail(legacy): legacy ignores redirect nodes entirely]
d=$(mktemp -d); f=$d/target; echo written > $f; cat $f

--- redirections apply left to right [xfail(legacy): legacy ignores redirect nodes entirely]
d=$(mktemp -d); echo one > $d/a > $d/b; cat $d/b

--- a builtin's redirection does not leak into the shell [xfail(legacy): legacy ignores redirect nodes entirely]
d=$(mktemp -d); pwd > $d/p; cat $d/p > /dev/null; echo still-here

--- here-document body is expanded when the delimiter is unquoted [xfail(legacy): legacy has no here-documents]
x=world; cat <<EOT
hello $x
EOT

--- a quoted delimiter suppresses expansion [xfail(legacy): legacy has no here-documents]
cat <<'EOT'
hello $x
EOT

--- two here-documents on one line, in order [xfail(legacy): legacy has no here-documents]
cat <<A; cat <<B
first
A
second
B

--- a here-document feeds a pipeline [xfail(legacy): legacy has no here-documents]
cat <<EOT | wc -l
a
b
c
EOT

--- an empty here-document body [xfail(legacy): legacy has no here-documents]
cat <<EOT
EOT

--- arithmetic expansion [xfail(legacy): legacy has no arithmetic expansion]
echo $((1 + 2))

--- arithmetic in an assignment [xfail(legacy): legacy has no arithmetic expansion]
i=1; i=$((i + 1)); echo $i

--- arithmetic as a loop counter [xfail(legacy): legacy has no arithmetic expansion]
i=0; while [ $i -lt 3 ]; do i=$((i+1)); done; echo $i

--- arithmetic precedence follows C [xfail(legacy): legacy has no arithmetic expansion]
echo $((2 + 3 * 4)) $(((2 + 3) * 4))

--- arithmetic reads variables without a dollar [xfail(legacy): legacy has no arithmetic expansion]
i=5; echo $((i * i))

--- arithmetic assignment writes back [xfail(legacy): legacy has no arithmetic expansion]
i=5; echo $((i += 3)); echo $i

--- an unset variable is zero in arithmetic [xfail(legacy): legacy has no arithmetic expansion]
echo $((no_such_var + 1))

--- arithmetic bases follow C [xfail(legacy): legacy has no arithmetic expansion]
echo $((0x1f)) $((010))

--- the colon distinguishes unset from empty [xfail(legacy): legacy has no parameter expansion beyond $name]
x=; echo "[${x:-colon}] [${x-nocolon}]"

--- assign-default writes the value back [xfail(legacy): legacy has no parameter expansion beyond $name]
echo ${undef_assign:=written}; echo $undef_assign

--- use-alternate substitutes only when set [xfail(legacy): legacy has no parameter expansion beyond $name]
x=yes; echo "[${x:+alt}] [${unset_alt:+alt}]"

--- all four trimming forms [xfail(legacy): legacy has no parameter expansion beyond $name]
p=/a/b/c.txt; echo "${p#*/} ${p##*/} ${p%.*} ${p%%.*}"

--- a default value may contain blanks [xfail(legacy): legacy has no parameter expansion beyond $name]
echo "${undef_blanks:-a default with spaces}"

--- the positional count [xfail(legacy): legacy has no parameter expansion beyond $name]
echo $#

--- unquoted star and at behave alike [xfail(legacy): legacy has no parameter expansion beyond $name]
set -- one two; echo $* ; echo $@

--- quoted at produces one field per parameter [xfail(legacy): legacy has no parameter expansion beyond $name]
set -- a b; for x in "$@"; do echo "[$x]"; done

--- quoted star joins with the first IFS character [xfail(legacy): legacy has no parameter expansion beyond $name]
set -- a b; echo "[$*]"

--- shift renumbers the positional parameters [xfail(legacy): legacy has no parameter expansion beyond $name]
set -- a b c; shift; echo "$1 $2"; echo $#

--- set -f disables pathname expansion [xfail(legacy): legacy has no parameter expansion beyond $name]
set -f; echo /usr/bin/tru*

--- a function receives positional parameters [xfail(legacy): legacy has no functions]
f() { echo "$1-$2"; }; f a b

--- return sets the status and leaves the function [xfail(legacy): legacy has no functions]
f() { return 3; }; f; echo $?

--- return stops execution mid-function [xfail(legacy): legacy has no functions]
f() { echo in; return; echo never; }; f

--- positional parameters are restored after a call [xfail(legacy): legacy has no functions]
set -- outer; f() { echo "$1"; }; f inner; echo "$1"

--- a redefinition replaces the previous body [xfail(legacy): legacy has no functions]
f() { echo one; }; f() { echo two; }; f

--- a function sees and mutates global variables [xfail(legacy): legacy has no functions]
x=before; f() { x=after; }; f; echo $x

--- a function can be recursive [xfail(legacy): legacy has no functions]
countdown() { if [ $1 -gt 0 ]; then echo $1; countdown $(($1 - 1)); fi; }; countdown 3
