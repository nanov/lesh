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
i=0; while [ $i -lt 2 ]; do echo $i; i=$(expr $i + 1); done

--- for loop [xfail(legacy): legacy has no compound commands]
for x in a b c; do echo $x; done

--- case statement [xfail(legacy): legacy has no compound commands]
case abc in a*) echo matched;; *) echo no;; esac

--- subshell [xfail(legacy): legacy has no compound commands]
(echo inside)

--- brace group [xfail(legacy): legacy has no compound commands]
{ echo grouped; }

--- default value expansion [xfail: #22 - the ${...} family]
echo ${undefined_var:-fallback}

--- length expansion [xfail: #22]
x=hello; echo ${#x}

--- suffix trimming [xfail: #22]
x=file.txt; echo ${x%.txt}

--- prefix trimming [xfail: #22]
x=/a/b/c; echo ${x##*/}

--- exit status parameter [xfail: #22 - special parameters]
false; echo $?

--- positional parameters [xfail: #22]
set -- one two; echo $1 $2

--- glob expansion [xfail(legacy): legacy has no pathname expansion]
echo /usr/bin/tru*

--- cd builtin [xfail(legacy): legacy forks builtins, so cd changes the child and exits]
cd /tmp && pwd

--- variable assignment persists [xfail(legacy): legacy parses assignments and applies them only via setenv in the child]
x=value; echo $x

--- function definition and call [xfail: #25 - functions]
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
