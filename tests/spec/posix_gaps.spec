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

# `cd -L`, `cd -P`, $CDPATH and the failure status (#46). Every case starts with an
# ABSOLUTE cd into a fresh directory of its own: the harness runs a case with the
# repo as $PWD and a temporary directory as the real one, and an absolute cd is what
# makes the two agree before anything relative is measured. Nothing prints an
# absolute path either - the two shells get different temporary directories, so the
# expectation is what is left after $d is trimmed off.

--- cd -P resolves a symlink where -L keeps it [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p real/inner; ln -s real link; cd -L link/inner; echo "L=${PWD#$d}"; cd -L "$d"; cd -P link/inner; echo "P=${PWD#$d}"

--- dot-dot follows the mode it was reached in [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p real/inner; ln -s real link; cd link/inner; cd ..; echo "L=${PWD#$d}"; cd -P "$d/link/inner"; cd -P ..; echo "P=${PWD#$d}"

--- a CDPATH match is written to standard output [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p one/target; CDPATH=$d/one; cd target > "$d/printed"; read line < "$d/printed"; echo "printed=${line#$d} pwd=${PWD#$d}"

--- an empty CDPATH entry means the current directory and prints nothing [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p one/target target; CDPATH=:$d/one; cd target > "$d/printed"; read line < "$d/printed"; echo "printed=[${line#$d}] pwd=${PWD#$d}"

--- CDPATH is searched left to right [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p first/want second/want; CDPATH=$d/first:$d/second; cd want > /dev/null; echo "${PWD#$d}"

--- CDPATH is not searched for a dot-prefixed operand [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p one/here here; CDPATH=$d/one; cd ./here > "$d/printed"; read line < "$d/printed"; echo "printed=[${line#$d}] pwd=${PWD#$d}"

--- the last of cd -L and cd -P wins [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p real/inner; ln -s real link; cd -P -L -PL link/inner; echo "${PWD#$d}"; cd -L "$d"; cd -L -P -LP link/inner; echo "${PWD#$d}"

--- a cd that did not happen is status 2 [xfail(legacy): legacy has no parameter expansion beyond $name]
cd /nonexistent 2>/dev/null; echo "status=$?"

--- an illegal cd option is status 2 [xfail(legacy): legacy has no parameter expansion beyond $name]
cd -x 2>/dev/null; echo "status=$?"

--- cd - prints where it went and swaps OLDPWD [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p a b; cd a; cd ../b; cd - > "$d/printed"; read line < "$d/printed"; echo "printed=${line#$d} pwd=${PWD#$d} oldpwd=${OLDPWD#$d}"

--- cd -- ends the options [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -- -L; cd -- -L; echo "${PWD#$d}"

# STARTUP `PWD`, and `pwd`'s own -L/-P (#51). POSIX 2.5.3 has the SHELL set PWD when
# it starts: from the environment only when the inherited value still names the
# current directory, and from getcwd otherwise. lesh did neither, so a lie in the
# environment reached `pwd` and then steered every relative `cd` after it.
#
# These cases re-invoke `$TESTEE` because the value under test is one the shell reads
# at INVOCATION - a case that only assigned PWD in the running shell would measure
# `cd` instead. `[ "$out" = "$d" ]` rather than printing the path: the two shells get
# different temporary directories, so the comparison has to happen inside the case.

--- the shell sets PWD at startup when the environment has none [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; unset PWD; out=$("$TESTEE" -c 'printf %s "$PWD"'); [ "$out" = "$d" ] && echo same || echo "differs [$out] wanted [$d]"

--- an inherited PWD that names another directory is replaced at startup [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; out=$(PWD=/etc "$TESTEE" -c 'printf %s "$PWD"'); [ "$out" = "$d" ] && echo same || echo "differs [$out] wanted [$d]"

--- a relative PWD is not an absolute pathname of anywhere [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; out=$(PWD=relative "$TESTEE" -c 'printf %s "$PWD"'); [ "$out" = "$d" ] && echo same || echo "differs [$out] wanted [$d]"

--- a wrong inherited PWD does not steer a relative cd [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p a/b; out=$(PWD=/etc "$TESTEE" -c 'cd a/b 2>/dev/null; printf %s "$PWD"'); echo "${out#$d}"

# The other half of the same rule, and the one a string comparison would break: on a
# system where /tmp is a symlink to /private/tmp, a LOGICAL PWD legitimately differs
# from getcwd's answer. That is what #46's -L exists for, so the startup check has to
# accept it - device and inode say the two name one directory where the text does not.

--- an inherited PWD reached through a symlink is kept [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir real; ln -s real link; cd link; out=$(PWD="$d/link" "$TESTEE" -c 'printf %s "$PWD"'); echo "${out#$d}"

--- pwd -P reports the physical directory and -L the logical one [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir real; ln -s real link; cd link; l=$(pwd -L); p=$(pwd -P); echo "L=${l#$d} P=${p#$d}"

--- pwd with no option is pwd -L [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir real; ln -s real link; cd link; w=$(pwd); echo "${w#$d}"

--- the last of pwd -L and pwd -P wins [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir real; ln -s real link; cd link; a=$(pwd -P -L); b=$(pwd -L -P); echo "${a#$d} ${b#$d}"

--- pwd falls back to the physical directory when PWD has gone stale [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir sub; readonly PWD; cd sub 2>/dev/null; w=$(pwd); echo "${w#$d}"

--- a stale PWD does not steer a later relative cd either [xfail(legacy): legacy has no command substitution]
d=$(mktemp -d); cd -P "$d"; d=$PWD; mkdir -p sub/deeper; readonly PWD; cd sub 2>/dev/null; cd deeper 2>/dev/null; w=$(pwd); echo "${w#$d}"

--- an illegal pwd option is status 2 [xfail(legacy): legacy has no parameter expansion beyond $name]
pwd -x 2>/dev/null; echo "status=$?"

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

--- a backslash-quoted delimiter suppresses expansion and still terminates [xfail(legacy): legacy has no here-documents]
cat <<\EOT
hello $x `false`
EOT
echo after

--- a partly quoted delimiter has quote removal applied [xfail(legacy): legacy has no here-documents]
cat <<E'O'T
body
EOT
echo after

--- a delimiter quoted mid-word with a backslash still terminates [xfail(legacy): legacy has no here-documents]
cat <<EO\T
body
EOT
echo after

--- a double-quoted delimiter fragment has quote removal applied [xfail(legacy): legacy has no here-documents]
cat <<"EO"T
body
EOT
echo after

--- <<- strips tabs with a backslash-quoted delimiter [xfail(legacy): legacy has no here-documents]
cat <<-\EOT
	hello $x
	EOT
echo after

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

--- PPID is set at startup [xfail(legacy): legacy does not set PPID]
[ "$PPID" -gt 1 ] && echo ok

--- PPID is not exported
env | grep -c '^PPID='

--- PPID is an ordinary variable and can be assigned [xfail(legacy): legacy does not set PPID]
PPID=42; echo "$PPID"

--- PPID does not change in a subshell [xfail(legacy): legacy does not set PPID]
outer=$PPID; inner=$( echo "$PPID" ); [ "$outer" = "$inner" ] && echo same

--- an async command inside a brace group runs in the background [xfail(legacy): legacy has no compound commands]
F=/tmp/lesh_spec_fifo_brace; rm -f $F; mkfifo $F; { echo foo >$F & }; cat $F; rm -f $F

--- an async command inside a subshell runs in the background [xfail(legacy): legacy has no compound commands]
F=/tmp/lesh_spec_fifo_sub; rm -f $F; mkfifo $F; (echo foo >$F &); cat $F; rm -f $F

--- an async command inside a loop body runs in the background [xfail(legacy): legacy has no compound commands]
F=/tmp/lesh_spec_fifo_loop; rm -f $F; mkfifo $F; for i in 1; do echo foo >$F & done; cat $F; rm -f $F

--- a redirection to a FIFO is opened once, not speculatively [xfail(legacy): legacy ignores redirect nodes entirely]
F=/tmp/lesh_spec_fifo_once; rm -f $F; mkfifo $F; cat $F & echo foo >$F; wait; rm -f $F

--- ! negates a command's status [xfail(legacy): legacy has no `!` negation]
! false; echo $?; ! true; echo $?

--- ! negates the whole pipeline, not its first command [xfail(legacy): legacy has no `!` negation]
! echo hi | grep -q x; echo $?

--- ! in an if condition [xfail(legacy): legacy has no `!` negation]
if ! false; then echo yes; fi

--- set -e exits the shell rather than ending the list [xfail(legacy): legacy does not honour set -e]
set -e; while true; do echo r1; false; echo nope; break; done; echo nope3

--- set -e is suppressed in an if condition [xfail(legacy): legacy does not honour set -e]
set -e; if false; then echo a; fi; echo after

--- set -e is suppressed in a while condition [xfail(legacy): legacy does not honour set -e]
set -e; while false; do :; done; echo after

--- set -e does not fire on a short-circuited and list [xfail(legacy): legacy does not honour set -e]
set -e; false && echo a; echo after

--- the short-circuit exemption travels out of a brace group [xfail(legacy): legacy does not honour set -e]
set -e; { false && echo a; }; echo after

--- the short-circuit exemption travels out of a loop body [xfail(legacy): legacy does not honour set -e]
set -e; while true; do false && echo a; break; done; echo after

--- the short-circuit exemption stops at a subshell [xfail(legacy): legacy does not honour set -e]
set -e; (false && echo a); echo after

--- the short-circuit exemption stops at a function call [xfail(legacy): legacy does not honour set -e]
set -e; f(){ false && echo a; }; f; echo after

--- set -e fires on the last command of an and list [xfail(legacy): legacy does not honour set -e]
set -e; true && false; echo after

--- set -e fires when every operand of an or list fails [xfail(legacy): legacy does not honour set -e]
set -e; false || false; echo after

--- set -e does not fire on a negated pipeline [xfail(legacy): legacy does not honour set -e]
set -e; ! true; echo after

--- set -e fires on a case body [xfail(legacy): legacy does not honour set -e]
set -e; case x in x) false;; esac; echo after

--- set -e reads the pipeline's status, not the first stage's [xfail(legacy): legacy does not honour set -e]
set -e; false | true; echo after

--- a single quote inside double quotes is an ordinary character [xfail(legacy): legacy strips single quotes inside double quotes]
echo "it's"; echo "'b'"; echo "a'b'c"

--- single and double quoted runs concatenate without losing quotes
echo 'x'"'y'"'z'

--- a tilde inside double quotes is not expanded [xfail(legacy): legacy strips single quotes inside double quotes]
echo "~"; echo "~/x"; echo "a~b"

--- a single quote survives an expansion in the same word [xfail(legacy): legacy strips single quotes inside double quotes]
x="it's"; echo "[$x]"; echo $x

--- a single quote inside double quotes around a command substitution [xfail(legacy): legacy strips single quotes inside double quotes]
echo "a$(echo "b'c")d"

--- a file without a shebang is run as a shell script [xfail(legacy): legacy has no ENOEXEC fallback]
D=/tmp/lesh_spec_noexec; rm -r $D 2>/dev/null; mkdir -p $D; printf 'echo "ran: $1"\n' > $D/s; chmod +x $D/s; $D/s hello; rm -r $D

--- ENOEXEC fallback also applies to a command found on PATH [xfail(legacy): legacy has no ENOEXEC fallback]
D=/tmp/lesh_spec_noexec2; rm -r $D 2>/dev/null; mkdir -p $D; printf 'echo "ran: $1"\n' > $D/prog; chmod +x $D/prog; PATH=$D:$PATH prog hello; rm -r $D

--- a trap listing can be read back in [xfail(legacy): legacy has no trap builtin]
# Written to a FILE rather than captured with $( ), deliberately. Whether `trap`
# inside a command substitution reports the parent's traps is a point where dash
# and POSIX.1-2024 disagree, and this case is about re-inputtable QUOTING.
F=/tmp/lesh_spec_traplist; trap 'echo "a"'"'b'"'\c' USR1; trap > $F; trap - USR1; . $F; kill -s USR1 $$; rm -f $F

--- trap with a numeric first operand resets every condition [xfail(legacy): legacy has no trap builtin]
trap 'echo trapped' 2 QUIT; trap 2 QUIT; trap; echo listed-nothing

--- trap -- lets a command start with a hyphen [xfail(legacy): legacy has no trap builtin]
trap -- '- x' USR1; trap

--- an ignored trap prints as an empty action [xfail(legacy): legacy has no trap builtin]
trap '' INT TERM; trap

--- wait with no operands reports zero, not the last job's status [xfail(legacy): legacy has no asynchronous lists]
false & wait; echo "wait=$?"

--- wait with a pid reports that job's status [xfail(legacy): legacy has no asynchronous lists]
false & wait $!; echo "one=$?"; true & wait $!; echo "two=$?"

--- wait on a pid that is not a child reports 127 [xfail(legacy): legacy has no asynchronous lists]
wait 99999; echo "wait=$?"

--- exec replaces the shell rather than forking [xfail(legacy): legacy has no exec builtin]
exec echo replaced; echo notreached

--- exec with no command runs only the redirections [xfail(legacy): legacy has no exec builtin]
exec; echo reached; exec 2>/dev/null; echo still-here

--- exec redirection outlives the command [xfail(legacy): legacy has no exec builtin]
F=/tmp/lesh_spec_exec_redir; (exec > $F; echo written); cat $F; rm -f $F

--- an assignment prefixing exec persists because exec is a special builtin [xfail(legacy): legacy has no exec builtin]
x=1 exec; echo "x=$x"

--- an assignment prefixing exec reaches the new image's environment [xfail(legacy): legacy has no exec builtin]
LESH_SPEC_EXEC=bar exec sh -c 'echo "got=$LESH_SPEC_EXEC"'

--- exec in a subshell replaces only the subshell [xfail(legacy): legacy has no exec builtin]
(exec echo inner); echo "after=$?"

--- exec in a pipeline stage replaces only that stage [xfail(legacy): legacy has no exec builtin]
exec echo piped | cat; echo "after=$?"

--- exec with a command that is not found reports 127 and exits [xfail(legacy): legacy has no exec builtin]
(exec ./_lesh_no_such_command_); echo "sub=$?"; exec ./_lesh_no_such_command_; echo notreached

--- exec with a file that is not executable reports 126 and exits [xfail(legacy): legacy has no exec builtin]
exec /etc/hosts; echo notreached

--- a redirection failure on exec exits a non-interactive shell [xfail(legacy): legacy has no exec builtin]
exec < /_lesh_no_such_file_; echo notreached

--- command exec demotes the special builtin so a redirection failure is survivable [xfail(legacy): legacy has no exec builtin]
command exec < /_lesh_no_such_file_; status=$?; echo "survived=$status"

--- exec keeps the fd it opened inside a grouping [xfail(legacy): legacy has no exec builtin]
{ exec 4>&3; } 3>&2; echo grouped >&4; echo "after=$?"

--- exec preserves the process id [xfail(legacy): legacy has no exec builtin]
exec sh -c "[ \$\$ -eq $$ ] && echo same-pid"

--- an ignored signal stays ignored across exec [xfail(legacy): legacy has no exec builtin]
trap '' INT; exec sh -c 'kill -INT $$; echo survived'

--- the EXIT trap runs when exec fails [xfail(legacy): legacy has no exec builtin]
trap 'echo trapped' EXIT; exec ./_lesh_no_such_command_; echo notreached

# --- #34: a redirection's file descriptor must not outlive its construct -------
#
# apply_redirection remembered a displaced fd with dup(2), so a fd that was CLOSED
# left nothing to remember and restore_fds never closed it again: the descriptor
# the redirection opened survived the construct that opened it. Every case below
# was wrong because of that one missing sentinel, or because the status a failed
# redirection reports was 1 where dash answers 2.

--- a redirection on a grouping closes the fd it opened [xfail(legacy): legacy ignores redirect nodes entirely]
{ :; } 3>&2; echo foo >&3; echo "status=$?"

--- a redirection on a loop closes the fd it opened [xfail(legacy): legacy ignores redirect nodes entirely]
for i in 1; do :; done 3>&2; echo x >&3; echo "status=$?"

--- a redirection on a function call closes the fd it opened [xfail(legacy): legacy ignores redirect nodes entirely]
f() { :; }; f 3>&2; echo x >&3; echo "status=$?"

--- a redirection on eval closes the fd it opened [xfail(legacy): legacy ignores redirect nodes entirely]
eval : 3>&2; echo x >&3; echo "status=$?"

--- a redirection on a builtin closes the fd it opened [xfail(legacy): legacy ignores redirect nodes entirely]
echo hi 3>&2; echo x >&3; echo "status=$?"

--- a nested grouping closes only the fd it opened [xfail(legacy): legacy ignores redirect nodes entirely]
{ { :; } 3>&2; echo mid >&3; } 4>&2; echo out >&3; echo "status=$?"

--- a displaced descriptor is put back rather than left closed [xfail(legacy): legacy ignores redirect nodes entirely]
exec 3>&1; { echo inner >&3; } 3>&- 2>/dev/null; echo outer >&3

--- the saved copy of a descriptor is out of the way of later redirections [xfail(legacy): legacy ignores redirect nodes entirely]
exec 3>&1; { echo inner >&3; } 3>&2 4>&3 2>/dev/null; echo outer >&3

--- a redirection failure reports two [xfail(legacy): legacy ignores redirect nodes entirely]
echo x < /_lesh_no_such_file_; echo "status=$?"

--- a redirection failure on a compound command reports two [xfail(legacy): legacy ignores redirect nodes entirely]
{ :; } < /_lesh_no_such_file_; echo "status=$?"

--- a redirection failure on an external command reports two [xfail(legacy): legacy ignores redirect nodes entirely]
/bin/echo x < /_lesh_no_such_file_; echo "status=$?"

--- a redirection failure on a special builtin exits a non-interactive shell [xfail(legacy): legacy ignores redirect nodes entirely]
: < /_lesh_no_such_file_; echo notreached

--- command demotes a special builtin so a redirection failure is survivable [xfail(legacy): legacy ignores redirect nodes entirely]
command : < /_lesh_no_such_file_; echo "survived=$?"

--- a redirection failure on a pipeline stage reports two [xfail(legacy): legacy ignores redirect nodes entirely]
true | echo x < /_lesh_no_such_file_; echo "status=$?"

--- closing a descriptor that was never open is not an error [xfail(legacy): legacy ignores redirect nodes entirely]
echo a 5>&-; echo "out=$?"; echo b 5<&-; echo "in=$?"

--- closing stdout makes a builtin that writes fail [xfail(legacy): legacy ignores redirect nodes entirely]
echo x >&-; echo "status=$?"

--- a descriptor closed for a command is closed only for that command [xfail(legacy): legacy ignores redirect nodes entirely]
exec 3>&2; { echo a >&3; } 3>&- 2>/dev/null; echo "inner=$?"; echo b >&3; echo "outer=$?"

--- a command that is only redirections performs them [xfail(legacy): legacy ignores redirect nodes entirely]
rm -f /tmp/lesh_spec_bare; >/tmp/lesh_spec_bare; echo "st=$?"; test -f /tmp/lesh_spec_bare && echo created; rm -f /tmp/lesh_spec_bare

--- a failing redirection with no command name reports two [xfail(legacy): legacy ignores redirect nodes entirely]
< /_lesh_no_such_file_; echo "status=$?"

--- an assignment is skipped when its redirection fails [xfail(legacy): legacy ignores redirect nodes entirely]
x=set < /_lesh_no_such_file_; echo "status=$? x=$x"

# ISSUE #70. #50 gave a pipeline stage with no command name this rule already;
# these four give it to the plain no-command-name case, which is `run_simple_command`
# forking to give the redirection OPERAND a subshell environment
# (RedirectionsWithNoCommandNameRunInASubshell's own case) - and that fork is
# exactly why the substitution's status went missing: it happened in a process
# whose memory never rejoined the shell's.

--- a bare redirection with no substitution in its operand reports zero [xfail(legacy): legacy ignores redirect nodes entirely]
rm -f /tmp/lesh_spec_p70_a; >/tmp/lesh_spec_p70_a; echo "st=$?"; rm -f /tmp/lesh_spec_p70_a

--- a bare redirection reports its operand's command substitution status [xfail(legacy): legacy ignores redirect nodes entirely]
rm -f /tmp/lesh_spec_p70_b; >/tmp/lesh_spec_p70_b$(exit 17); echo "st=$?"; rm -f /tmp/lesh_spec_p70_b

--- a bare redirection takes the last of several operand substitutions [xfail(legacy): legacy ignores redirect nodes entirely]
rm -f /tmp/lesh_spec_p70_c; >/tmp/lesh_spec_p70_c$(exit 3) >/tmp/lesh_spec_p70_c$(exit 9); echo "st=$?"; rm -f /tmp/lesh_spec_p70_c

--- a bare redirection's substitution survives a later plain assignment [xfail(legacy): legacy ignores redirect nodes entirely]
rm -f /tmp/lesh_spec_p70_d; >/tmp/lesh_spec_p70_d$(exit 17) x=1; echo "st=$? x=$x"; rm -f /tmp/lesh_spec_p70_d

--- a bare redirection's substitution yields to a later assignment's [xfail(legacy): legacy ignores redirect nodes entirely]
rm -f /tmp/lesh_spec_p70_e; >/tmp/lesh_spec_p70_e$(exit 17) x=$(exit 3); echo "st=$? x=[$x]"; rm -f /tmp/lesh_spec_p70_e

--- a here-document can be fed to a descriptor other than stdin [xfail(legacy): legacy ignores redirect nodes entirely]
cat 3<<END <&3
foo
END

--- several here-documents on one command each keep their own descriptor [xfail(legacy): legacy ignores redirect nodes entirely]
{ cat <&5; cat <&4; cat <&3; cat; } <<A 3<<B 4<<C 5<<D
zero
A
three
B
four
C
five
D

# Shell options (#31). `$-`, `set -o`/`+o`, and the letters that were parsed,
# recorded, and then read by nothing at all.
#
# The `$-` cases match on the CONTENT of the string rather than printing it: POSIX
# leaves the order unspecified and dash's is its own internal table order, so
# comparing the text would be comparing an unspecified detail.

--- dollar-dash reports the letters that are on [xfail(legacy): legacy has no shell options and no $-]
set -ae; case $- in *a*) echo has-a;; *) echo no-a;; esac; case $- in *e*) echo has-e;; *) echo no-e;; esac

--- dollar-dash drops a letter turned back off [xfail(legacy): legacy has no shell options and no $-]
set -ae; set +a; case $- in *a*) echo has-a;; *) echo no-a;; esac

--- set -o name and set -e reach the same option [xfail(legacy): legacy has no shell options and no $-]
set -o errexit; case $- in *e*) echo has-e;; *) echo no-e;; esac; set +o errexit; case $- in *e*) echo has-e;; *) echo no-e;; esac

--- set +o output restores the options it was printed from [xfail(legacy): legacy has no shell options]
set -aeu; saved=$(set +o); set +aeu -f; eval "$saved"; for l in a e u f; do case $- in *$l*) echo "on $l";; *) echo "off $l";; esac; done

--- an unknown option letter is an error that exits the shell [xfail(legacy): legacy accepts anything after a dash]
set -Z; echo after

--- an unknown -o name is an error that exits the shell [xfail(legacy): legacy accepts anything after a dash]
set -o bogus; echo after

--- noexec reads without running [xfail(legacy): legacy has no shell options]
set -n; echo executed

--- noclobber refuses to truncate an existing file [xfail(legacy): legacy ignores redirect nodes entirely]
rm -f /tmp/lesh_spec_noclobber; echo first > /tmp/lesh_spec_noclobber; set -C; echo second > /tmp/lesh_spec_noclobber; echo "status=$?"; cat /tmp/lesh_spec_noclobber; rm -f /tmp/lesh_spec_noclobber

--- noclobber is overridden by the pipe form [xfail(legacy): legacy ignores redirect nodes entirely]
rm -f /tmp/lesh_spec_clobber; echo first > /tmp/lesh_spec_clobber; set -C; echo second >| /tmp/lesh_spec_clobber; echo "status=$?"; cat /tmp/lesh_spec_clobber; rm -f /tmp/lesh_spec_clobber

--- noclobber still allows an existing file that is not regular [xfail(legacy): legacy ignores redirect nodes entirely]
set -C; echo hidden > /dev/null; echo "status=$?"

--- allexport marks an assignment for export [xfail(legacy): legacy has no shell options]
set -a; exported=yes; sh -c 'echo "${exported-unset}"'

--- allexport off leaves an assignment unexported [xfail(legacy): legacy has no shell options]
set +a; plain=yes; sh -c 'echo "${plain-unset}"'

--- nounset makes an unset parameter fatal [xfail(legacy): legacy has no shell options]
set -u; echo "[${x}]"; echo after

--- nounset covers the length and trim expansions [xfail(legacy): legacy has no shell options]
set -u; echo "[${x#y}]"; echo after

--- nounset leaves the defaulting expansions alone [xfail(legacy): legacy has no parameter expansion beyond $name]
set -u; echo "[${x-d}][${x+s}][${x=v}][${x}]"

--- the question mark expansion is fatal on its own [xfail(legacy): legacy has no parameter expansion beyond $name]
echo "[${x?}]"; echo after

--- without pipefail a pipeline reports its last stage [xfail(legacy): legacy has no pipelines that report a status]
exit 1 | exit 2 | exit 0; echo "b $?"; exit 0 | exit 0 | exit 4; echo "d $?"

--- xtrace writes the expanded command to standard error [xfail(legacy): legacy has no shell options]
set -x; foo=bar; echo $foo

--- xtrace expands PS4 [xfail(legacy): legacy has no shell options]
foo=XY; PS4='${foo#X} '; set -x; echo traced

--- getopts parses one option per call [xfail(legacy): legacy has no getopts]
getopts ab:c o -a -b arg -c; echo "1[$o]"; getopts ab:c o -a -b arg -c; echo "2[$o][$OPTARG]"; getopts ab:c o -a -b arg -c; echo "3[$o]"

--- getopts parses grouped letters one at a time [xfail(legacy): legacy has no getopts]
for i in 1 2 3 4; do getopts abc o -abc; echo "$i[$o] st=$?"; done

--- getopts takes an option argument adjoined or separate [xfail(legacy): legacy has no getopts]
getopts a:b o -a'  foo' -b; echo "[$OPTARG][$OPTIND]"; OPTIND=1; getopts a:b o -a '-x  foo' -b; echo "[$OPTARG][$OPTIND]"; OPTIND=1; getopts a:b o -a '' -b; echo "[$OPTARG][$OPTIND]"

--- getopts reports the end of the options with a question mark [xfail(legacy): legacy has no getopts]
getopts a x -a; getopts a x -a; echo "st=$? [$x]"

--- a lone hyphen is an operand and a double hyphen ends the options [xfail(legacy): legacy has no getopts]
getopts '' x -; echo "hyphen st=$? [$OPTIND]"; OPTIND=1; getopts ab x -a -- -b; echo "1[$x]"; getopts ab x -a -- -b; echo "2 st=$? [$OPTIND]"

--- getopts leaves OPTIND at the first operand [xfail(legacy): legacy has no getopts]
getopts '' x; echo "$OPTIND"; OPTIND=1; getopts '' x operand; echo "$OPTIND"; OPTIND=1; getopts '' x --; echo "$OPTIND"; OPTIND=1; getopts '' x -- operand; echo "$OPTIND"

--- getopts parses the positional parameters when given no operands [xfail(legacy): legacy has no getopts]
set -- -a -b arg -c; getopts ab:c o; echo "1[$o]"; getopts ab:c o; echo "2[$o][$OPTARG]"; getopts ab:c o; echo "3[$o]"; echo "$OPTIND"

--- resetting OPTIND to 1 starts a new parse [xfail(legacy): legacy has no getopts]
getopts ab p -a -b; getopts ab p -a -b; getopts ab p -a -b; OPTIND=1; getopts xy q -x -y; echo "1[$q]"; getopts xy q -x -y; echo "2[$q]"; getopts xy q -x -y; echo "3[$OPTIND]"

--- an unknown option is a question mark that keeps the loop going [xfail(legacy): legacy has no getopts]
getopts ab v -z 2>/tmp/lesh_spec_getopts_err; echo "st=$? [$v] [${OPTARG-unset}]"; [ -s /tmp/lesh_spec_getopts_err ] && echo diagnosed; rm -f /tmp/lesh_spec_getopts_err

--- a leading colon puts the offending letter in OPTARG and prints nothing [xfail(legacy): legacy has no getopts]
getopts :ab v -z 2>/tmp/lesh_spec_getopts_quiet; echo "st=$? [$v] [$OPTARG]"; [ -s /tmp/lesh_spec_getopts_quiet ] || echo silent; rm -f /tmp/lesh_spec_getopts_quiet

--- a missing option argument is a colon under the colon form [xfail(legacy): legacy has no getopts]
getopts :a: v -a 2>/tmp/lesh_spec_getopts_quiet; echo "st=$? [$v] [$OPTARG] [$OPTIND]"; [ -s /tmp/lesh_spec_getopts_quiet ] || echo silent; rm -f /tmp/lesh_spec_getopts_quiet

--- a missing option argument is a question mark otherwise [xfail(legacy): legacy has no getopts]
getopts a: v -a 2>/tmp/lesh_spec_getopts_err; echo "st=$? [$v] [${OPTARG-unset}] [$OPTIND]"; [ -s /tmp/lesh_spec_getopts_err ] && echo diagnosed; rm -f /tmp/lesh_spec_getopts_err

--- the colon in an optstring marks an argument and is never an option letter [xfail(legacy): legacy has no getopts]
getopts a:b v -: 2>/dev/null; echo "st=$? [$v]"

--- option letters may be digits [xfail(legacy): legacy has no getopts]
getopts ab:01: o -a -b arg -1 -2 -0; getopts ab:01: o -a -b arg -1 -2 -0; getopts ab:01: o -a -b arg -1 -2 -0; echo "[$o][$OPTARG]"; getopts ab:01: o -a -b arg -1 -2 -0; echo "[$o]"

--- OPTIND and OPTARG are not exported [xfail(legacy): legacy has no getopts]
getopts a: o -a arg; getopts a: o -a arg; sh -c 'echo ${OPTIND-unset} ${OPTARG-unset}'

--- getopts is a regular builtin, so a usage error does not exit the shell [xfail(legacy): legacy has no getopts]
getopts 2>/dev/null; echo "st=$?"; getopts ab 1bad -a 2>/dev/null; echo "st=$?"; echo reached

# `test`, `[` and `readonly` - the three builtins of #35. Each was CLASSIFIED and
# unimplemented, so the command search never reached PATH and the dispatch's false
# return was discarded: `test 1 = 2` reported 0, and so did `readonly OPTIND`.
# Statuses are asserted everywhere below because 0, 1 and 2 are the three answers
# the broken version could not tell apart.

--- test follows the argument-count rules for zero through four arguments [xfail(legacy): legacy does not dispatch builtins and its lexer swallows the `;`]
test; echo $?; test ''; echo $?; test -n; echo $?; x=-n; test "$x"; echo $?; test ! -n; echo $?; test = = =; echo $?; test ! ! ''; echo $?; test ! 1 = 2; echo $?

--- test compares integers numerically and reports a non-integer [xfail(legacy): legacy does not dispatch builtins and its lexer swallows the `;`]
test 1 -eq 1; echo $?; test 10 -gt 9; echo $?; test 007 -eq 7; echo $?; test x -eq 1 2>/dev/null; echo $?

--- test file predicates answer about the file rather than succeeding [xfail(legacy): legacy does not dispatch builtins and its lexer swallows the `;`]
test -d /; echo $?; test -f /; echo $?; test -s /nonexistent; echo $?; test -e /etc/hosts; echo $?; test -h /nonexistent; echo $?

--- test combines expressions with -a, -o and parentheses [xfail(legacy): legacy does not dispatch builtins and its lexer swallows the `;`]
test 1 = 1 -a 2 = 2; echo $?; test 1 = 2 -o 2 = 2; echo $?; test ! \( 1 = 1 \); echo $?; test -z '' -a -n x -o -n ''; echo $?; test \( 1 = 2 \) -o \( 3 = 3 \); echo $?

--- the bracket form is the same builtin and requires its closing bracket [xfail(legacy): legacy has no `[` builtin and no exit status parameter]
[ 1 = 1 ]; echo $?; [ 1 = 2 ]; echo $?; [ 1 = 1 2>/dev/null; echo $?; [ ! ]; echo $?

--- readonly with no operands lists in re-inputtable form [xfail(legacy): legacy has no readonly builtin at all]
readonly r=1; readonly; readonly u; readonly -p

--- readonly takes -- and marks a name without assigning to it [xfail(legacy): legacy has no readonly builtin at all]
b=B; readonly -- a=A b c=C; echo $a $b $c; readonly

--- assigning to a readonly variable exits a non-interactive shell [xfail(legacy): legacy has no readonly builtin at all]
readonly a=1; echo $a; a=2; echo not reached

--- unset refuses a readonly variable, and unset is a special builtin [xfail(legacy): legacy has no readonly builtin at all]
readonly a=1; unset a; echo not reached

--- exporting a readonly variable is allowed and assigning to it is not [xfail(legacy): legacy has no readonly builtin at all]
readonly a=1; export a; export a=2; echo not reached

--- a refused assignment inside a parameter expansion is fatal too [xfail(legacy): legacy has no readonly builtin at all]
readonly x; : ${x=1}; echo not reached

--- a refused assignment inside arithmetic is fatal too [xfail(legacy): legacy has no readonly builtin at all]
readonly x; echo $((x=1)); echo not reached

--- a readonly name with no value is still unset [xfail(legacy): legacy has no readonly builtin at all]
readonly x; echo ${x-unset}; x=1

# Issue #71: `export NAME` and `readonly NAME` ask the SAME question - does marking
# a name CREATE it? - and answered it two ways. `export` created the variable with
# an empty value, so `${xa-unset}` and `${xa:-empty}` stopped being distinguishable
# after a bare `export xa`, a child saw `xa=` where dash exports nothing, and
# `export -p` listed a variable that does not exist - the re-inputtability defect
# #40 and #38 both hit. POSIX makes the export attribute a property of the NAME,
# marked "whether or not it is set", which is the rule #24 already recorded for
# readonly; the flag lives on the variable, and marking no longer assigns.

--- export marks a name without creating the variable [xfail(legacy): legacy's export path is incomplete]
export xa; echo "[${xa-unset}][${xa:-empty}]"; xa=1; echo "[${xa-unset}]"

--- a name marked for export but unset is in no child's environment [xfail(legacy): legacy's export path is incomplete]
export xb; /usr/bin/env | /usr/bin/grep -c '^xb='; sh -c 'echo "[${xb-unset}]"'

--- assigning after export exports the value the assignment gave it [xfail(legacy): legacy's export path is incomplete]
export xc; sh -c 'echo "[${xc-unset}]"'; xc=1; sh -c 'echo "[${xc-unset}]"'

--- export -p prints a marked-but-unset name bare, so re-input does not create it [xfail(legacy): legacy has no export -p]
export xd; export -p | /usr/bin/grep '^export xd'

--- an export -p listing round-trips a marked-but-unset name [xfail(legacy): legacy has no export -p]
export xe; e=$(export -p | /usr/bin/grep '^export xe'); unset xe; eval "$e"; echo "[${xe-unset}]"

--- an operand that field-splits marks every name it yields rather than emptying them [xfail(legacy): legacy does not split an export operand]
n='xf xg'; export $n; echo "[${xf-unset}][${xg-unset}]"; export -p | /usr/bin/grep -c '^export xf$'

--- export NAME=VALUE still assigns and still exports [xfail(legacy): legacy's export path is incomplete]
export xh=1; echo "$xh"; sh -c 'echo "[${xh-unset}]"'; export -p | /usr/bin/grep '^export xh='

--- unset drops the export mark along with the variable [xfail(legacy): legacy's export path is incomplete]
export xi=1; unset xi; xi=2; sh -c 'echo "[${xi-unset}]"'

--- a readonly name with no value still refuses a later assignment [xfail(legacy): legacy has no readonly builtin at all]
readonly xj; echo "[${xj-unset}]"; xj=1; echo not reached

--- read reports a readonly target and, being regular, does not exit the shell [xfail(legacy): legacy has no readonly builtin at all]
readonly v; echo hi | { read v 2>/dev/null; echo "st=$?"; }; echo reached

--- getopts fails rather than ignore a readonly OPTIND [xfail(legacy): legacy has no getopts and no readonly]
readonly OPTIND; getopts a o -a 2>/dev/null; echo "st=$?"; echo reached

# Issue #50: a pipeline stage is FORKED before anything in it is evaluated, so a
# command substitution in its WORDS runs with the pipe on fd 0 and not the shell's
# own stdin. Expanding first, in the shell, made every case below read the wrong
# descriptor - empty here, because this runner gives each case /dev/null, and a
# HANG on a terminal. The terminal half is asserted in tests/unit/executor_tests.cpp,
# which is the only place that can: stdin=DEVNULL here is by design.

--- a command substitution in a pipeline stage reads the pipe
echo a | echo $(cat)

--- a command substitution in an EXTERNAL stage reads the pipe [xfail(legacy): beyond legacy's single-pass parser]
echo a | /bin/echo $(cat)

--- a command substitution in a FUNCTION stage's arguments reads the pipe [xfail(legacy): beyond legacy's single-pass parser]
f() { echo "[$1]"; }; echo a | f $(cat)

--- a command substitution inside arithmetic in a stage reads the pipe [xfail(legacy): beyond legacy's single-pass parser]
echo 3 | echo $(( $(cat) + 1 ))

--- a command substitution in a MIDDLE stage reads its own pipe [xfail(legacy): beyond legacy's single-pass parser]
printf 'm\n' | echo $(cat) | tr m M

--- a command substitution in a stage may be a pipeline itself [xfail(legacy): beyond legacy's single-pass parser]
echo a | echo $(cat | tr a-z A-Z)

--- a stage's words are expanded BEFORE its redirections are performed [xfail(legacy): beyond legacy's single-pass parser]
F=/tmp/lesh_spec_p50_f; echo FILE > $F; echo PIPE | echo $(cat) < $F; rm -f $F

--- a fatal expansion error in a stage ends the stage, not the shell [xfail(legacy): beyond legacy's single-pass parser]
echo a | echo ${x?bad}; echo "rc=$?"

--- a stage with no command name still performs its redirections [xfail(legacy): legacy ignores redirect nodes entirely]
F=/tmp/lesh_spec_p50_out; rm -f $F; echo a | > $F; echo "rc=$?"; ls $F; rm -f $F

--- a stage with no command name reports its last command substitution [xfail(legacy): beyond legacy's single-pass parser]
echo a | x=$(exit 3); echo "rc=$?"

# ONE NUMERIC-OPERAND PARSER (#63). Four builtins read their operand with
# `std::atoi`, which cannot report failure at all: it answers 0 for `notanumber`,
# for `--` and for `0x10`, and truncates `3x` to 3. Three of the four turned that
# into a wrong ANSWER a script branches on - `exit notanumber` reported SUCCESS -
# and the fourth reached waitpid(2) as pid 0, which means ANY CHILD IN THE PROCESS
# GROUP: the wrong-syscall half of #45, in the one path that ticket did not fix.
#
# Each case runs in a subshell. `exit`, `shift` and `return` are SPECIAL builtins,
# so a usage error ends a non-interactive shell - in dash as in lesh - and the
# subshell is what keeps the rest of the case running to report it. stderr goes to
# /dev/null because only the wording differs: dash writes `Illegal number` and
# lesh names the operand the way its other builtins do.

--- exit refuses an operand that is not a number [xfail(legacy): legacy has no subshells or command lists, so the whole line reaches it as one command]
( exit notanumber ) 2>/dev/null; echo "a=$?"
( exit 3x ) 2>/dev/null; echo "b=$?"
( exit 99999999999999999999 ) 2>/dev/null; echo "c=$?"
( exit 3 ) 2>/dev/null; echo "d=$?"

# `exit 256` is 0 and `exit 300` is 44 because POSIX takes the status MODULO 256,
# which is a separate rule from the range check above and stays separate: 300 is a
# perfectly representable number, and only the low byte survives a waitpid.

--- an exit status is taken modulo 256 rather than refused [xfail(legacy): legacy has no subshells or command lists]
( exit 256 ) 2>/dev/null; echo "a=$?"
( exit 300 ) 2>/dev/null; echo "b=$?"
( exit 255 ) 2>/dev/null; echo "c=$?"

--- shift refuses an operand that is not a number [xfail(legacy): legacy has no subshells, command lists or positional parameters]
( set -- a b c; shift notanumber; echo "shifted" ) 2>/dev/null; echo "a=$?"
( set -- a b c; shift 99999999999999999999 ) 2>/dev/null; echo "b=$?"
( set -- a b c; shift 2; echo "n=$#" ) 2>/dev/null; echo "c=$?"

--- return refuses an operand that is not a number [xfail(legacy): legacy has no functions, subshells or command lists]
( f() { return notanumber; }; f; echo "reached" ) 2>/dev/null; echo "a=$?"
( f() { return 99999999999999999999; }; f; echo "reached" ) 2>/dev/null; echo "b=$?"
( f() { return 3; }; f; echo "s=$?" ) 2>/dev/null; echo "c=$?"

# `wait <not a pid>` is the one that reached a SYSCALL with the wrong argument
# rather than merely reporting the wrong status: waitpid(0, ...) waits for any
# child in the process group, so `wait notanumber` reaped a job the script did not
# name. Refused before the call, as #45 made `kill` refuse a non-numeric pid.

--- wait refuses a pid operand that is not a number [xfail(legacy): legacy has no subshells or command lists, and no wait builtin]
( wait notanumber ) 2>/dev/null; echo "a=$?"
( wait 99999999999999999999 ) 2>/dev/null; echo "b=$?"
( wait -- 1 ) 2>/dev/null; echo "c=$?"

# `--` ENDS THE OPTIONS at every utility POSIX gives operands and no options
# (XCU 1.4), and `unalias` is one: all four reference shells discard it and remove
# the alias named after it, where lesh looked for an alias called `--`.

--- unalias takes its operand after a `--` separator [xfail(legacy): legacy has no aliases or command lists]
alias foo=bar; unalias -- foo; echo "a=$?"; alias foo 2>/dev/null; echo "b=$?"

# A POSITIONAL PARAMETER INDEX past the end is an unset parameter, which is what a
# clamp lands on. It used to wrap a size_t: `${18446744073709551617}` is 2^64 + 1,
# which wrapped onto `$1` and substituted the first argument.

--- a positional parameter index too large to be one is unset rather than wrapped [xfail(legacy): legacy has no positional parameters or command lists]
set -- a b c; echo "[${18446744073709551617}][${4}][${2}]"

# A DIGIT RUN THAT CANNOT BE A FILE DESCRIPTOR IS NOT ONE. It used to be taken for
# an IO_NUMBER and accumulated into a uint32_t, so `4294967298>file` wrapped onto
# fd 2 and redirected the shell's STDERR. dash, zsh and ksh all read an over-long
# run as an ordinary word instead - dash's threshold is a single digit, which is
# why `99>` is already a word there - and lesh's is what a descriptor can hold, so
# it agrees with bash below the limit and with the other three above it.

--- a digit run too large to be a file descriptor is an ordinary word [xfail(legacy): legacy has no redirections or brace groups]
{ echo one 99999999999999999999>/dev/null; }; echo "a=$?"
{ echo two 4294967298>/dev/null; }; echo "b=$?"
{ echo three 9>/dev/null; }; echo "c=$?"

# DELIBERATE divergences from dash, recorded rather than chosen quietly
# (handoff.md: where dash is behind the standard, say so in writing). Each is an
# assertion dash itself fails in the yash suite, so each is a place lesh is ahead.
# dash is NOT RUN for these: it cannot be the expectation of a case that exists
# because lesh answers differently, so each carries its own `=== expect` block.
#
# The `set -o` listing is the one entry that is not a standards question: POSIX
# leaves the format unspecified, dash lists four options of its own that lesh does
# not have, and printing a name for a switch that does not exist would be the lie
# `set -o` is meant to expose.

--- redirection operands with no command name are expanded in a subshell [divergence: dash expands them in the current environment; POSIX 2.9.1 requires a subshell]
unset x; < ${x=no/such/file}; ${x+echo leaked}; echo done
=== expect [stderr]
done

--- duplicating a read-only descriptor onto an output fd is an error [divergence: dash does not check the access mode; POSIX 2.7.6 requires it]
3</dev/null >&3; echo "status=$?"
=== expect [stderr]
status=2

--- duplicating a write-only descriptor onto an input fd is an error [divergence: dash does not check the access mode; POSIX 2.7.5 requires it]
cat 3>/dev/null <&3; echo "status=$?"
=== expect [stderr]
status=2

--- nounset applies inside arithmetic [divergence: dash expands $((x)) on an unset x to zero; POSIX requires the error, and dash fails option-p.tst for it]
set -u; echo "[$((x))]"; echo after
=== expect [status: 2] [stderr]

--- pipefail makes a pipeline report its rightmost failing stage [divergence: dash has no pipefail and fails both of pipeline-p.tst's cases for it; POSIX Issue 8 defines it]
set -o pipefail; exit 1 | exit 2 | exit 0; echo "b $?"; exit 3 | exit 0 | exit 0; echo "c $?"
=== expect
b 2
c 3

--- set -o lists only the options the shell has [divergence: dash also lists interactive, stdin, emacs and debug, which POSIX does not name and lesh does not have]
set -o
=== expect
Current option settings
allexport       off
notify          off
noclobber       off
errexit         off
noglob          off
monitor         off
noexec          off
nounset         off
verbose         off
xtrace          off
ignoreeof       off
nolog           off
pipefail        off
vi              off

--- an incomplete test expression is an error rather than a crash [divergence: dash SEGFAULTS on `test x -a` and reports 139; POSIX leaves the two-argument case unspecified, and 2 with a diagnostic is the only answer that is not a crash]
test x -a 2>/dev/null; echo $?
=== expect
2

--- OPTIND names the argument still being parsed rather than the next one [divergence: dash advances OPTIND on entering a word, so a mid-word `shift $((OPTIND-1))` discards letters nobody examined; bash, ksh and zsh all report it as lesh does]
set -- -abc; getopts abc o; echo "$OPTIND"; getopts abc o; echo "$OPTIND"; getopts abc o; echo "$OPTIND"
=== expect
1
1
2

--- OPTARG is unset when the option takes no argument [divergence: dash leaves the previous OPTARG as an empty string and fails both of getopts-p.tst's assertions about it; POSIX requires unset]
getopts a:b o -a foo -b; getopts a:b o -a foo -b; echo "${OPTARG-unset}"; OPTIND=1; getopts a x -a; getopts a x -a; echo "${OPTARG-unset}"
=== expect
unset
unset

--- getopts state is per-shell, so a function continues the shell's parse [divergence: dash resets its internal index on function entry while leaving the OPTIND variable alone, so the two disagree inside a function; bash and ksh share it as lesh does]
f() { getopts ab o; echo "f[$o]"; }; set -- -a -b; getopts ab o; echo "main[$o]"; f -a -b; echo "$OPTIND"
=== expect
main[a]
f[b]
3

--- an OPTIND past the last argument is the end of the options [divergence: dash silently restarts the parse from word 1, re-reporting options the script has already acted on; POSIX calls a modified OPTIND unspecified and bash clamps as lesh does]
set -- -a -b; OPTIND=99; getopts ab o; echo "st=$? [$o] [$OPTIND]"
=== expect
st=1 [?] [3]

--- a non-numeric or unset OPTIND restarts the parse [divergence: dash routes the value through its numeric parser inside the assignment hook and aborts the shell with "Illegal number"; POSIX specifies only the value 1]
set -- -abc; getopts abc o; unset OPTIND; getopts abc o; echo "[$o]"; OPTIND=junk; getopts abc o; echo "[$o]"
=== expect
[a]
[a]

# The `command` builtin (#31). It was once implemented for `-v` alone and let every
# other use silently succeed, which regressed command-p.tst from 38/49 to 19/49 -
# the original sin the whole project's stub-that-succeeds rule was written from.
# `-V` and `-p` were then taken for command NAMES, which is the same failure one
# step out: `command -p ls` reported `-p: No such file or directory` and
# `command -V echo` the same. One of the 14 assertions that DID pass passed only
# because of that message, which is a non-zero status.
#
# `-v` output is compared as a CATEGORY rather than as text wherever a pathname
# would appear: the two shells search the same PATH but they do not run in the
# same directory, so `/*` or not is the assertion and the bytes are not.

--- command -v names every reserved word [xfail(legacy): legacy has no command builtin]
command -v if; command -v then; command -v while; command -v esac; command -v '!'; command -v '{'; command -v '}'

--- command -V says which category a name falls in [xfail(legacy): legacy has no command builtin]
command -V if; command -V :; command -V read; command -V export

--- command describes an alias as a command line that re-creates it [xfail(legacy): legacy has no command builtin]
alias abc='echo ABC'
c="$(command -v abc)"
unalias abc
eval "$c"
abc
command -V abc

--- command describes a function by name [xfail(legacy): legacy has no command builtin and no functions]
f() { :; }; command -v f; command -V f

--- command -v is silent about a name it cannot find and command -V is not [xfail(legacy): legacy has no command builtin]
PATH= command -v _no_such_command_; echo "v=$?"; PATH= command -V _no_such_command_; echo "V=$?"

--- command -p searches the standard path when PATH is empty [xfail(legacy): legacy has no command builtin]
PATH= command -p echo foo bar | command -p cat

--- command -pv finds a standard utility with PATH empty [xfail(legacy): legacy has no command builtin]
PATH= command -pv cat >/dev/null; echo "v=$?"; PATH= command -pV cat >/dev/null; echo "V=$?"

--- command -V wins over -v whichever order they came in [xfail(legacy): legacy has no command builtin]
command -v -V :; command -V -v :

--- command rejects an option it does not have [xfail(legacy): legacy has no command builtin]
command -z cat 2>/dev/null; echo "st=$?"

--- command with nothing to run succeeds [xfail(legacy): legacy has no command builtin]
command; echo "bare=$?"; command --; echo "sep=$?"; command -p; echo "p=$?"

--- command -- ends the options, so -v after it is a command name [xfail(legacy): legacy has no command builtin]
command -- -v 2>/dev/null; echo "st=$?"

--- command runs an external command in a pipeline stage
echo hi | command cat

--- a function named command shadows the builtin, and only the first prefix [xfail(legacy): legacy has no command builtin and no functions]
command() { echo FUNCTION; }; command XXX; command command echo hi

--- command in a pipeline stage bypasses a function of the same name [xfail(legacy): legacy has no command builtin and no functions]
cat() { echo FUNCTION; }; echo hi | command cat

--- command -v describes an external command by its absolute pathname [xfail(legacy): legacy has no command builtin]
case "$(command -v cat)" in (/*cat) echo pathname;; (*) echo "[$(command -v cat)]";; esac

--- a regular built-in utility is described by a pathname [divergence: dash writes the bare name for `command -v echo` and fails command-p.tst's 'output of describing non-special built-in (-v)'; POSIX XCU writes a REGULAR built-in utility, one that also exists on PATH, as an absolute pathname and reserves the bare name for the built-ins that must be built in]
case "$(command -v echo)" in (/*) echo pathname;; (*) echo name;; esac
=== expect
pathname

--- a command name containing a slash is described by an absolute pathname [divergence: dash writes the operand back exactly as typed and fails command-p.tst's 'output of describing external command (-v, with slash)'; POSIX XCU requires an absolute pathname for a command_name containing a slash]
: >foo; chmod a+x foo; case "$(command -v ./foo)" in (/*/foo) echo absolute;; (*) echo relative;; esac
=== expect
absolute

--- an inherited PWD with a dot component is replaced at startup [divergence: dash, bash and ksh keep it and then print a pathname with a `.` in it for a directory whose real name they know; POSIX 2.5.3 says PWD holds an absolute pathname containing no component that is dot or dot-dot, and zsh replaces it as lesh does]
d=$(mktemp -d); cd -P "$d"; d=$PWD; out=$(PWD="$d/." "$TESTEE" -c 'printf %s "$PWD"'); [ "$out" = "$d" ] && echo replaced || echo "kept [$out]"
=== expect
replaced

--- an inherited PWD with a dot-dot component is replaced at startup [divergence: same rule, and the component that can actually mislead - `cd` canonicalizes `..` lexically, so a dot-dot the shell did not put in PWD itself is a claim about the tree that nothing has checked; dash, bash and ksh keep it, zsh replaces it]
d=$(mktemp -d); cd -P "$d"; d=$PWD; b=$(basename "$d"); out=$(PWD="$d/../$b" "$TESTEE" -c 'printf %s "$PWD"'); [ "$out" = "$d" ] && echo replaced || echo "kept [$out]"
=== expect
replaced

# Assignment prefixes that never reached their command (#31). The prefix was
# applied by four different paths and two of them dropped it: `x=1 eval 'echo $x'`
# printed a blank line, and an external command's values were expanded in the
# CHILD, by an expander built with no command runner, so a command substitution in
# one of them had nothing to run it.
#
# The three rules the cases below separate, all POSIX 2.9.1 and all confirmed
# against dash: a prefix on a SPECIAL builtin persists, a prefix on anything else
# is restored, and `command` demotes a special builtin so its prefix is restored
# too.

--- a command substitution in an assignment prefix reaches the command [xfail(legacy): legacy has no command substitution]
x=$(echo z) "$TESTEE" -c 'echo "[$x]"'

--- a prefix on eval is visible to the code it reads and persists after it [xfail(legacy): legacy has no eval]
x=1 eval 'echo "in=[$x]"'; echo "after=[$x]"

--- a prefix on dot persists too, and unexported [xfail(legacy): legacy has no dot builtin]
x=1 . /dev/null; echo "after=[$x]"; x=2 eval 'env' | grep '^x=' ; echo "env=$?"

--- command demotes a special builtin, so its prefix is restored [xfail(legacy): legacy has no command builtin]
a=a; a=b command :; echo "colon=[$a]"; x=1 command eval 'echo "in=[$x]"'; echo "eval=[$x]"; x=1 command . /dev/null; echo "dot=[$x]"

--- a prefix on exec with no command persists, and not under command [xfail(legacy): legacy has no exec builtin]
x=1 exec; echo "exec=[$x]"; y=1 command exec; echo "demoted=[$y]"

--- a prefix on wait is restored, wait being a regular builtin [xfail(legacy): legacy has no wait builtin]
x=1 wait; echo "after=[$x]"

--- a prefix on an external command is exported to it and not to the shell [xfail(legacy): legacy has no command substitution]
x=$(echo z) env | grep '^x='; echo "after=[$x]"

--- a prefix in a pipeline stage is expanded in the stage [xfail(legacy): legacy has no command substitution]
echo body | x=$(cat) "$TESTEE" -c 'echo "[$x]"'

# `--` ends the options of the operand-only special builtins (POSIX XCU 1.4
# Utility Description Defaults: a standard utility that accepts operands and no
# options "shall recognize `--` as a first argument to be discarded"). dash
# rejects it for all of them and fails 'separator preceding operand' in
# return-p.tst, exit-p.tst and eval-p.tst; `. --` is the one it accepts, so that
# case is not marked. The same divergence already stands for `exec --`.

--- the separator precedes the operand of exit [divergence: dash reports `exit: Illegal number: --` and fails exit-p.tst's 'separator preceding operand'; POSIX XCU 1.4 discards a leading -- for a utility with no options]
exit -- 56
=== expect [status: 56]

--- the separator precedes the operand of return [divergence: dash reports `return: Illegal number: --` and fails return-p.tst's 'separator preceding operand'; POSIX XCU 1.4 discards a leading -- for a utility with no options]
f() { return -- 56; }; f; echo "st=$?"
=== expect
st=56

--- the separator precedes the operand of eval [divergence: dash looks for a command named -- and fails eval-p.tst's 'separator preceding operand'; POSIX XCU 1.4 discards a leading -- for a utility with no options]
eval -- 'echo foo'; echo "st=$?"
=== expect
foo
st=0

--- the separator precedes the operand of dot [xfail(legacy): legacy has no dot builtin]
printf 'echo sourced\n(exit 3)\n' >script; . -- ./script; echo "st=$?"

--- a separator with no operand after it leaves the default [divergence: dash rejects the separator itself; POSIX XCU 1.4 discards it and `exit --` is then `exit`]
(exit 41); exit --
=== expect [status: 41]

# `alias` is the same shape one builtin further out: POSIX gives it OPTIONS "None."
# and the operands `alias-name[=string]`, so the rule above applies unchanged. It is
# what makes the LISTING re-inputtable, which is the property alias-p.tst:93 asserts
# by writing every alias to a file, clearing the table and feeding the file back
# through `eval alias -- $(cat ...)`. Without the separator a name beginning with `-`
# could not survive that round trip. dash and bash both report `--` as an alias that
# is not found; zsh and yash discard it.

--- the separator precedes the operands of alias [stdin] [divergence: dash and bash report `alias: -- not found` and fail alias-p.tst's 'printing all aliases'; POSIX XCU 1.4 discards a leading -- for a utility with no options]
alias -- e='echo hi'
alias e
e
=== expect
e='echo hi'
hi

--- a separator with no operands after it still lists every alias [divergence: dash and bash look for an alias named `--`; POSIX XCU 1.4 discards it and `alias --` is then `alias`]
alias b=b a='echo hi'; alias --
=== expect
a='echo hi'
b='b'

# Control flow unwinds PAST the operator that follows it. `break`, `continue` and
# a bare `return` all report 0, and 0 is what `&&` continues on, so an and-or list
# that read only the left operand's status ran the right-hand side of every one of
# them.

--- a break before && does not run the right-hand side [xfail(legacy): legacy has no compound commands]
for i in 1; do break && echo reached1; echo reached2; done; echo "st=$?"

--- a continue before && does not run the right-hand side [xfail(legacy): legacy has no compound commands]
for i in 1 2; do echo "in $i"; continue && echo reached1; echo reached2; done

--- a return before && does not run the right-hand side [xfail(legacy): legacy has no functions]
f() { return && echo reached1; echo reached2; }; f; echo "st=$?"

--- an and-or list reports the operand the unwind came from [xfail(legacy): legacy has no functions]
f() { return 7 && echo reached; }; f; echo "st=$?"

--- an exit before && does not run the right-hand side [xfail(legacy): legacy has no exit builtin and runs /bin/exit, which does not exist]
exit 5 && echo reached

# `break 0` is an ERROR, not a no-op. POSIX XCU makes n a positive decimal
# integer; zero levels is what the unwind counter reads as "already arrived", so
# the break vanished and the loop carried on.

--- a zero operand to break is an error [xfail(legacy): legacy has no compound commands]
for i in 1; do break 0; echo reached; done 2>/dev/null; echo "st=$?"

--- a zero operand to continue is an error [xfail(legacy): legacy has no compound commands]
for i in 1; do continue 0; echo reached; done 2>/dev/null; echo "st=$?"

--- a non-numeric operand to break is an error [xfail(legacy): legacy has no compound commands]
for i in 1; do break x; echo reached; done 2>/dev/null; echo "st=$?"

# `eval` and `.` report ZERO when no command runs. Starting from the caller's `$?`
# reported the status of whatever ran before instead.

--- eval on null operands reports zero [xfail(legacy): legacy has no eval builtin and runs /bin/eval, which does not exist]
(exit 1); eval '' '' ''; echo "st=$?"

--- eval on nothing but a comment reports zero [xfail(legacy): legacy has no eval builtin and runs /bin/eval, which does not exist]
(exit 1); eval '# nothing here'; echo "st=$?"

--- dotting a file with no commands reports zero [xfail(legacy): legacy has no dot builtin]
(exit 1); . /dev/null; echo "st=$?"

--- a dot script still sees the caller's exit status [xfail(legacy): legacy has no dot builtin]
printf 'echo $?\n' >script; (exit 5); . ./script

# `.` searches $PATH for an operand with no slash, and its failure to find the
# script is FATAL to a non-interactive shell - `.` being a special builtin, the
# rule #34 established for a redirection failure. It fopen()ed the operand
# instead, which searches the working directory, and reported 127 and carried on.

--- dot searches PATH for an operand with no slash [xfail(legacy): legacy has no dot builtin]
mkdir p; printf 'exit 11\n' >p/sourced; PATH=./p; . sourced

--- dot does not fall back to the working directory [xfail(legacy): legacy has no dot builtin]
printf 'echo sourced\n' >here; PATH=/nonexistent; . here 2>/dev/null; echo "st=$?"

--- a dot script need only be readable, not executable [xfail(legacy): legacy has no dot builtin]
mkdir p; printf 'echo sourced\n' >p/readable; chmod 444 p/readable; PATH=./p; . readable

--- dot failing to find its script exits a non-interactive shell [xfail(legacy): legacy has no dot builtin]
. ./_no_such_file_ 2>/dev/null; echo not reached

--- dot failing to find its script on PATH exits a non-interactive shell too [xfail(legacy): legacy has no dot builtin]
. _no_such_file_ 2>/dev/null; echo not reached

--- a dot failure in a subshell leaves the shell that forked it running [xfail(legacy): legacy has no dot builtin]
(. ./_no_such_file_ 2>/dev/null); echo "reached st=$?"

--- a status the dot script itself reported is not a search failure [xfail(legacy): legacy has no dot builtin]
printf '(exit 3)\n' >script; . ./script; echo "reached st=$?"

# A dot script is a RETURN BOUNDARY: `return` in a sourced script returns from
# that script and no further, so whatever invoked it carries on. `eval` is NOT a
# boundary - `eval return` inside a function returns from the function.

--- return from a dot script nested in another dot script [xfail(legacy): legacy has no dot builtin]
printf 'echo in inner\nreturn\necho not reached\n' >inner; printf 'echo in outer\n. ./inner\necho out outer\n' >outer; . ./outer; echo after

--- return from a dot script nested in a function [xfail(legacy): legacy has no dot builtin]
printf 'echo in inner\nreturn\necho not reached\n' >inner; f() { echo in f; . ./inner; echo out f; }; f; echo after

--- return from a dot script reports the status it was given [xfail(legacy): legacy has no dot builtin]
printf '(exit 1)\nreturn 17\n' >script; . ./script; echo "st=$?"

--- return out of an eval returns from the enclosing function [xfail(legacy): legacy has no eval builtin and runs /bin/eval, which does not exist]
f() { eval return; echo not reached; }; f; echo "after st=$?"

# A `return` outside any function and any dot script. POSIX leaves it
# unspecified; dash and zsh both end the current input with the status it asked
# for, and lesh ran the next command instead.

--- a return outside a function ends the input [xfail(legacy): legacy has no return builtin]
return; echo x

--- a return outside a function reports its operand [xfail(legacy): legacy has no return builtin]
return 7; echo x

--- a return inside a brace group ends the input [xfail(legacy): legacy has no compound commands]
{ return 7; echo x; }; echo y

--- a return inside a loop ends the input [xfail(legacy): legacy has no compound commands]
while :; do return 4; done; echo x

--- a return ends a script arriving on standard input [stdin] [xfail(legacy): legacy has no return builtin]
echo one
return 6
echo two

--- a return still returns from a function [xfail(legacy): legacy has no functions]
f() { return 5; }; f; echo "after $?"

# `break` and `continue` with no enclosing loop. POSIX leaves it unspecified;
# dash makes it a silent no-op and a FUNCTION CALL is a boundary, so the loops
# the caller is inside are not loops the body is inside. `.` and `eval` are
# transparent, which is dash's answer for all three.

--- a break with no enclosing loop does nothing [xfail(legacy): legacy has no break builtin and runs /bin/break, which does not exist]
break; echo x

--- a break with no enclosing loop does nothing inside a brace group [xfail(legacy): legacy has no compound commands]
{ break; echo x; }; echo y

--- a continue with no enclosing loop does nothing [xfail(legacy): legacy has no continue builtin and runs /bin/continue, which does not exist]
continue; echo x

--- a break in a function does not break the caller's loop [xfail(legacy): legacy has no functions]
f() { break; }; for i in 1 2; do f; echo in; done; echo out

--- a continue in a function does not continue the caller's loop [xfail(legacy): legacy has no functions]
f() { continue; }; for i in 1 2; do f; echo in; done; echo out

--- a break level does not travel out of a function either [xfail(legacy): legacy has no functions]
f() { for i in 1 2; do break 3; done; echo in f; }; for j in 1 2; do f; echo body; done; echo out

--- a break past the nesting depth stops at the outermost loop [xfail(legacy): legacy has no compound commands]
for i in 1; do for j in a; do break 3; echo n1; done; echo n2; done; echo after

--- a break in a command substitution unwinds only the substitution [xfail(legacy): legacy has no compound commands]
for i in 1; do echo "[$(break; echo insub)]"; done

--- a break in a subshell unwinds only the subshell [xfail(legacy): legacy has no compound commands]
for i in 1; do (break; echo insub); echo body; done; echo after

--- a break in a dot script breaks the caller's loop [xfail(legacy): legacy has no dot builtin]
printf 'echo lib\nbreak\necho not reached\n' >lib; for i in 1 2; do . ./lib; echo body; done; echo after

--- a break in a dot script with no loop around it does nothing [xfail(legacy): legacy has no dot builtin]
printf 'echo lib\nbreak\necho lib end\n' >lib; . ./lib; echo "caller $?"

--- a break in the condition of a while loop leaves the loop [xfail(legacy): legacy has no compound commands]
while break; do echo body; done; echo after

# `set -v` writes input to standard error AS IT IS READ, and a dot script is
# input. Only `.` - an `eval` operand and a trap body have already been echoed
# once, as part of the line that carried them.

--- the verbose option echoes a dot script [stdin] [xfail(legacy): legacy has no dot builtin]
set -v
printf 'echo one\nexit 3\n' >script
. ./script

--- the verbose option does not echo an eval operand [stdin] [xfail(legacy): legacy has no eval builtin and runs /bin/eval, which does not exist]
set -v
eval 'echo hi'
echo done

--- test -ot against a file that does not exist [divergence: ADR-0001. A file that is not there has no modification time, so an existing file is newer than it: test-p.tst and bash both say so, while dash, zsh and macOS test(1) report false the moment either stat fails]
: >newer; test XXXXX -ot newer; echo "ot=$?"
=== expect
ot=0

--- test -nt against a file that does not exist [divergence: ADR-0001, the same rule read the other way round]
: >newer; test newer -nt XXXXX; echo "nt=$?"
=== expect
nt=0

--- test -nt and -ot with both operands missing are false, which dash agrees with [xfail(legacy): legacy mis-splits the line - `test: YYYYY;: unexpected operator` - so the second command's operand lands in the first]
test XXXXX -nt YYYYY; printf 'nt=%s ' $?; test XXXXX -ot YYYYY; echo "ot=$?"

# AN EXPANSION ERROR IS FATAL TO A NON-INTERACTIVE SHELL (#39).
#
# POSIX 2.8.1 makes an expansion error end a non-interactive shell, and dash
# answers 2 for every shape of it. Arithmetic was the shape with no diagnostic
# at all: the evaluator already refused each of these, and the refusal was
# turned into `unsupported construct` above it, which reached the command line
# as an empty field at status zero.

--- division by zero is an expansion error [xfail(legacy): legacy has no arithmetic expansion]
echo $((1/0)); echo st=$?

--- a malformed arithmetic expression is an expansion error [xfail(legacy): legacy has no arithmetic expansion]
echo $((--)); echo st=$?

--- an arithmetic error in the middle of a word stops the whole command [xfail(legacy): legacy has no arithmetic expansion]
echo A$((1/0))B; echo st=$?

--- an arithmetic error in an assignment leaves the variable alone [xfail(legacy): legacy has no arithmetic expansion]
x=$((1/0)); echo "st=$? x=$x"

--- an arithmetic error in a special builtin's operand stops the shell [xfail(legacy): legacy has no arithmetic expansion]
: $((1/0)); echo reached

--- a short-circuited operand that will not parse is still an error [xfail(legacy): legacy has no arithmetic expansion]
echo $(( 0 && + )); echo st=$?

--- arithmetic that evaluates is untouched by any of this [xfail(legacy): legacy has no arithmetic expansion]
echo $((6/2)) $((1+1)); echo st=$?

# A SYNTAX ERROR INSIDE `$(...)` REACHES THE OUTER SHELL (#57). The child
# refused and _exit(2)ed, and nothing read that status - so the body is now
# parsed on THIS side of the fork, where the refusal is a fact rather than an
# exit code indistinguishable from `exit 2`.
#
# Written one command per LINE. dash parses a substitution when it parses the
# command around it, so `echo before; echo $(if true)` on ONE line refuses the
# whole line and prints nothing; lesh reads a command at a time and would print
# `before` first. The line is where the two agree.

--- a syntax error inside a command substitution stops the shell [stdin] [xfail(legacy): legacy has no if clause, so the substitution body runs `if` as a command name]
echo before
echo $(if true)
echo reached

--- an unexpected token inside a command substitution stops the shell [stdin] [xfail(legacy): legacy accepts `;;` and carries on]
echo before
echo $(echo ;;)
echo reached

--- a syntax error inside a command substitution in an assignment stops the shell [stdin] [xfail(legacy): legacy has no if clause, so the substitution body runs `if` as a command name]
echo before
v=$(if true)
echo reached

--- a syntax error inside a BACKQUOTED substitution stops the shell [stdin] [xfail(legacy): legacy has no if clause, so the substitution body runs `if` as a command name]
echo before
echo `if true`
echo reached

# The boundary the fix must not cross: a RUNTIME failure inside a substitution
# is not a syntax error, and dash reports it through $? and carries on.

--- a runtime syntax error inside a substitution is the substitution's status, not the shell's [xfail(legacy): legacy has no eval builtin and runs /bin/eval, which does not exist]
x=$(eval "if true"); echo "st=$? x=[$x]"; echo reached

--- a command substitution that exits non-zero does not stop anything [xfail(legacy): legacy has no command substitution]
echo "[$(exit 7)]"; echo st=$?

--- a command substitution running a missing command does not stop anything [xfail(legacy): legacy has no command substitution]
echo "[$(_lesh_no_such_command_zz 2>/dev/null)]"; echo st=$?

# `set -u` reaching the compound commands (#39). Both were the same cause: each
# caller built its own expander and decided for itself, so a rule added in one
# place was missing in the others. `for` reported and carried on; `case` turned
# globbing off by constructing an expander directly and dropped error_on_unset
# with it, so it could not even report.

--- nounset in a for loop's word list stops the shell [xfail(legacy): legacy has no shell options]
set -u; for i in $nope; do echo x; done; echo reached

--- nounset in a case subject stops the shell [xfail(legacy): legacy has no shell options]
set -u; case $nope in *) echo x;; esac; echo reached

--- a case pattern is still not pathname-expanded [xfail(legacy): legacy has no case clause]
case a in *) echo star;; esac

--- nounset leaves a defaulted case subject alone [xfail(legacy): legacy has no case clause]
set -u; case ${x-ok} in ok) echo defaulted;; esac

# UNCHECKED INTEGER ACCUMULATORS (#62), the three left after #59 fixed the
# arithmetic evaluator's. Each was reachable from a single line and each was
# undefined behaviour - an abort under the debug preset's UBSan, a silent wrap in
# release - so what a value MEANS at each site is what these assert, not how it is
# computed. #59's answer for arithmetic was "an over-large literal saturates";
# none of the three wanted it, because none of them is computing a number.

--- the integer limits are test operands rather than overflows [xfail(legacy): legacy has no command lists, so the `;` and everything after it reach `test` as operands]
test -9223372036854775808 -eq 1; echo "a=$?"
test -9223372036854775808 -lt 0; echo "b=$?"
test 9223372036854775807 -gt 0; echo "c=$?"
test -9223372036854775808 -eq -9223372036854775808; echo "d=$?"

# `test` COMPARES, so an operand it cannot represent has no nearest answer worth
# giving: it is a usage error and stays one. dash and bash both report and exit 2.
# Saturating - arithmetic's answer - would silently compare a number the script
# never wrote.

--- a test operand past the integer limit is a usage error rather than a saturated comparison [xfail(legacy): legacy has no command lists or redirections]
test 9223372036854775808 -gt 0 2>/dev/null; echo "a=$?"
test -9223372036854775809 -eq 1 2>/dev/null; echo "b=$?"
test 99999999999999999999 -eq 1 2>/dev/null; echo "c=$?"

# A SIGNAL NUMBER has a bounded valid set, so there is nothing above the ceiling
# for a large one to become - a saturated signal number would name a real signal
# the script never asked for.
#
# THE SUBSHELLS THAT USED TO WRAP THESE FOUR LINES ARE GONE. #62 needed them
# because a special builtin's error ended a non-interactive lesh where it does not
# end dash, so the four refusals could not be read back in one shell; #66 was that
# difference, and with it fixed the workaround was a lie in the corpus - a reader
# would have taken the parentheses for something this case was asserting.

--- a signal number too large to be one is refused rather than overflowed [xfail(legacy): legacy has no trap builtin, so it runs `trap` as a command name]
trap - 99999999999999999999 2>/dev/null; echo "a=$?"
trap : 99999999999999999999 2>/dev/null; echo "b=$?"
trap - 99 2>/dev/null; echo "c=$?"
trap : 2 2>/dev/null; echo "d=$?"

--- kill refuses a signal number too large to be one [xfail(legacy): legacy has no command lists, and its kill is the external one]
kill -s 99999999999999999999 $$ 2>/dev/null; echo "a=$?"

# A FILE DESCRIPTOR is an int, so a number too large to be one is not invalid in
# some new way - it is invalid the way an unopened fd is, and takes the same
# diagnostic and status. dash reaches the same answer by a different road: its
# parser takes only a SINGLE digit after `>&`, so it refuses the word outright.

--- a redirection fd too large to be one is refused [xfail(legacy): legacy has no redirections, so `>&99999999999999999999` is an echo operand]
echo hi >&99999999999999999999

--- an input redirection fd too large to be one is refused [xfail(legacy): legacy has no redirections, so cat reads the script's own stdin]
cat <&99999999999999999999

--- case executes the next item after ;& without re-testing its pattern [divergence: dash predates ;& and refuses it with a syntax error - POSIX.1-2024 adds it]
case 1 in
    0) echo not reached 0;;
    1) echo matched 1;&
    2) echo matched 2; (exit 42);&
esac
=== expect [status: 42]
matched 1
matched 2

--- exit status after an empty ;& item leaves $? untouched [divergence: dash predates ;& and refuses it with a syntax error - POSIX.1-2024 adds it, and no shell answers this one: zsh resets $? to 0 for an empty ;& item and bash 3.2 rejects an empty item before ;& outright, so the yash test file is the only oracle]
(exit 1)
case i in
    i) ;&
    j) echo $?
esac
=== expect
1

# `--` before a NUMERIC operand is the same question one layer out: where do the
# options stop and the operand begin. #44 answered it for the command line and
# `first_operand` answers it for the five builtins POSIX gives operands and no
# options; `shift` was the sixth and read argv[1] directly, so `shift -- 2` sent
# `--` to std::atoi, got 0, and shifted NOTHING while reporting success.

--- shift takes its operand after a `--` separator [divergence: dash reports `Illegal number: --` and exits 2, failing shift-p.tst's 'separator preceding operand'; bash, zsh and ksh all shift by the operand after the separator, which is POSIX XCU 1.4 - and lesh already discards `--` this way for exit, return, break, continue, eval and dot]
set -- a b c d e
shift -- 2 && echo "[$#][$1]"
shift -- && echo "[$#][$1]"
=== expect
[3][c]
[2][d]

# WHICH SPECIAL BUILTINS KEEP THEIR FATALITY (#66). The narrowing that made an
# invalid signal name soft was made once, in the single place the rule is applied,
# so every OTHER special builtin was in its blast radius. These cases are the
# fence: each is one of POSIX XCU 2.8.1's rows reached through a different
# builtin, each already agreed with dash before #66, and each has to still agree
# after it. They were surveyed against dash, bash, zsh and yash first - dash is
# the strictest of the four and lesh follows dash, per ADR-0001.
#
# `echo before` on every case so an assertion of "nothing after this line ran"
# cannot be satisfied by a shell that died BEFORE the line, which an empty stdout
# would otherwise allow.

--- a utility syntax error in `set` still ends the shell [xfail(legacy): legacy has no set builtin]
echo before; set -Z 2>/dev/null; echo notreached

--- an operand that is not a number still ends the shell in `shift` [xfail(legacy): legacy has no shift builtin]
echo before; shift abc 2>/dev/null; echo notreached

# A shift count PAST THE END is fatal here too - POSIX 2.14 lets a shell treat it
# as a syntax error and dash does. It could not be a case while lesh reported 1
# for it and 2 for `shift abc`, because a case would have been asserting that
# unrelated difference; #73 made the two agree on 2, so it is a case now.

--- a shift count past the end still ends the shell [xfail(legacy): legacy has no shift builtin]
set -- a; echo before; shift 5 2>/dev/null; echo notreached

--- and the two ways of refusing a shift report the SAME status [xfail(legacy): legacy has no shift builtin]
set -- a; ("$TESTEE" -c 'set -- a; shift 5') 2>/dev/null; echo "past=$?"; ("$TESTEE" -c 'shift abc') 2>/dev/null; echo "bad=$?"

--- an operand that is not a NAME still ends the shell in `export` [xfail(legacy): legacy has no export builtin]
echo before; export 1bad=x 2>/dev/null; echo notreached

--- and in `readonly` [xfail(legacy): legacy has no readonly builtin]
echo before; readonly 1bad=x 2>/dev/null; echo notreached

# `trap`'s option loop used to BREAK on anything it did not recognise, so `-Z`
# became the ACTION STRING and the signal operand was read as a condition. A
# utility syntax error under POSIX XCU 2.8.1, which for a special builtin is
# fatal - and `failure_kind` defaults to `usage`, so it gets that for free.

--- an unknown option still ends the shell in `trap` [xfail(legacy): legacy has no trap builtin, so it runs `trap` as a command name]
echo before; trap -Z x INT 2>/dev/null; echo notreached

# The same case with stderr LEFT ALONE, so that a diagnostic is asserted to exist
# rather than only a status. The harness compares stderr for emptiness, which is
# exactly the question here: lesh reported `x: bad signal` - about the wrong
# operand entirely - where it now reports the option.

--- and says so on stderr [xfail(legacy): legacy has no trap builtin, so it runs `trap` as a command name]
echo before; trap -Z x INT; echo notreached

# The two things that must NOT become usage errors. A bare `-` is the RESET
# ACTION and not an option, and everything after `--` is an operand however many
# hyphens it starts with - which is the whole reason the separator is there.

--- a bare hyphen is still the reset action [xfail(legacy): legacy has no trap builtin, so it runs `trap` as a command name]
trap 'echo caught' USR1; trap - USR1; trap -- '- trapped' USR1; trap; echo "st=$?"

--- a variable assignment error through `export` still ends the shell [xfail(legacy): legacy has no export builtin]
readonly r=1; echo before; export r=2 2>/dev/null; echo notreached

--- and through `readonly` [xfail(legacy): legacy has no readonly builtin]
readonly r=1; echo before; readonly r=2 2>/dev/null; echo notreached

--- unsetting a readonly variable still ends the shell [xfail(legacy): legacy has no unset builtin]
readonly r=1; echo before; unset r 2>/dev/null; echo notreached

# And an operand that is not a NAME, which `unset` accepted at status 0 while
# `export` and `readonly` two functions away refused the same word. dash reports
# and exits 2 for all three.

--- an operand that is not a NAME still ends the shell in `unset` [xfail(legacy): legacy has no unset builtin]
echo before; unset 1bad 2>/dev/null; echo notreached

--- and a NAME with a character that cannot be in one [xfail(legacy): legacy has no unset builtin]
echo before; unset a.b 2>/dev/null; echo notreached

# The `-f` FORM IS NOT VALIDATED, and that is dash's answer rather than an
# oversight: `unset -f 1bad` reports nothing and succeeds there. A fix that
# validated every operand of every form would have broken this.

--- unsetting a FUNCTION by a name that is not a NAME is not an error [xfail(legacy): legacy has no unset builtin]
unset -f 1bad; echo "st=$?"; echo reached

--- a shell language syntax error inside `eval` still ends the shell [xfail(legacy): legacy has no eval builtin]
echo before; eval 'if' 2>/dev/null; echo notreached

--- a dot script that cannot be found still ends the shell [xfail(legacy): legacy has no dot builtin]
echo before; . ./_lesh_no_such_file_ 2>/dev/null; echo notreached

# The `2>/dev/null` is on the GROUP rather than on `:` itself. It had to be while
# lesh reported a refused assignment prefix BEFORE the command's own redirections
# and dash reported it through them: the two shells agreed about the shell's fate
# and differed about where the diagnostic went. #73 moved the report, and the case
# below now asserts the difference this one was written to avoid.

--- an assignment prefix refused before a special builtin still ends the shell [xfail(legacy): legacy has no assignment prefixes]
readonly r=1; echo before; { r=2 : ; } 2>/dev/null; echo notreached

# A refused prefix is diagnosed through the COMMAND'S OWN redirections, which is
# the rule `command`'s bad-option path already states in executor.cpp: reported
# where the command runs and not where the prefix was read.

--- a refused assignment prefix is reported through the command's own redirections [xfail(legacy): legacy has no assignment prefixes]
readonly r=1; echo before; r=2 : 2>/dev/null; echo notreached

--- and through them for an EXTERNAL command too [xfail(legacy): legacy has no assignment prefixes]
readonly r=1; echo before; r=2 /bin/echo hi 2>/dev/null; echo notreached

# The redirections are really PERFORMED, not merely consulted: dash creates and
# truncates the file before it reports. `readonly` goes INSIDE the invoked shell -
# the prefix has to be refused in the shell that does the redirecting, and a
# `$TESTEE -c` that only inherited the value would find r perfectly writable and
# assert nothing.

--- the refused command's redirections are performed before the diagnostic [xfail(legacy): legacy has no assignment prefixes]
d=$(mktemp -d); ("$TESTEE" -c "readonly r=1; r=2 : > $d/made") 2>/dev/null; test -f "$d/made"; echo "made=$?"

# And a redirection that CANNOT be performed leaves the shell's fate unchanged:
# dash reports the redirection error instead of the read-only one and still dies.
# Which of the two messages was written is NOT asserted - the harness compares
# stderr for emptiness alone - so this pins the fate and the status.

--- a redirection error on the refused command still ends the shell [xfail(legacy): legacy has no assignment prefixes]
readonly r=1; echo before; r=2 : 2>/_lesh_no_such_dir_/x; echo notreached

--- a redirection failure on a special builtin still ends the shell [xfail(legacy): legacy has no redirections]
echo before; : 2>/dev/null <./_lesh_no_such_file_; echo notreached

# ISSUE #74. `eval` and `.` re-enter the front end through `run_source`, which
# carried a SECOND COPY of the top-level command loop rather than calling it.
# Copies drift: `run_pending_traps` was missing from it for as long as the path
# existed and cost fifteen signal files three assertions each the moment #67
# routed a command substitution through it. These are the two guards still
# missing when the two loops were merged into one.

--- errexit inside an eval stops the eval and the shell [xfail(legacy): legacy has no eval builtin and runs /bin/eval, which does not exist]
set -e; eval "false
echo notreached"; echo alsonotreached

--- errexit inside a dot script stops the script and the shell [stdin] [xfail(legacy): legacy has no dot builtin]
set -e
printf 'false\necho notreached\n' >script
. ./script
echo alsonotreached

--- noexec set inside an eval stops the rest of that eval [xfail(legacy): legacy has no eval builtin and runs /bin/eval, which does not exist]
eval "set -n
echo notreached"; echo alsonotreached

--- noexec set inside a dot script stops the rest of that script [stdin] [xfail(legacy): legacy has no dot builtin]
printf 'set -n\necho notreached\n' >script
. ./script
echo alsonotreached

# The guard the merge must NOT pick up. `return` inside an `eval` returns from
# the enclosing FUNCTION, so `eval` has to let the unwind through where the top
# level consumes it and ends its own input. A shared loop has to be told which of
# the two it is running, or one of these two cases breaks the other.

--- a return inside an eval returns from the enclosing function [xfail(legacy): legacy has no functions]
f() { eval 'return 7'; echo notreached; }; f; echo "st=$? after=yes"

--- a return inside a dot script ends only that script [stdin] [xfail(legacy): legacy has no dot builtin]
printf 'return 7\necho notreached\n' >script
. ./script
echo "st=$? after=yes"

# ISSUE #76. `LINENO` - the last POSIX shell variable lesh did not set, and the
# only file in the yash suite scoring zero. dash implements it and agrees with
# lesh everywhere below except inside a FUNCTION, where no two shells agree at
# all; see the divergence at the end.
#
# The line comes from the OFFSET of the command being run, through the mapper in
# src/syntax/source_map.h. That is what makes the multi-line-expansion cases
# work: a counter would have to be told how many physical lines a `$(...)`
# swallowed, where a lookup never has to be told anything.

--- LINENO starts from one [stdin] [xfail(legacy): legacy does not set LINENO]
echo $LINENO

--- LINENO counts physical lines, blank ones included [stdin] [xfail(legacy): legacy does not set LINENO]
echo $LINENO
echo $LINENO

echo $LINENO

--- LINENO counts the physical lines a multi-line command substitution spans [stdin] [xfail(legacy): legacy does not set LINENO]
: $(echo foo
echo \
baz)
echo $LINENO

--- LINENO counts the physical lines a multi-line parameter expansion spans [stdin] [xfail(legacy): legacy does not set LINENO]
: ${foo#
bar \
baz}
echo $LINENO

--- LINENO counts the physical lines a multi-line arithmetic expansion spans [stdin] [xfail(legacy): legacy does not set LINENO]
: $((1
+ \
2))
echo $LINENO

--- LINENO in a -c string counts that string's lines [xfail(legacy): legacy does not set LINENO]
echo $LINENO
echo $LINENO

--- LINENO in a loop body reports the physical line on every iteration [stdin] [xfail(legacy): legacy does not set LINENO]
for i in 1 2 3
do
echo $LINENO
done

--- LINENO counts the lines a here-document body occupies [stdin] [xfail(legacy): legacy does not set LINENO]
cat <<END
one
two
END
echo $LINENO

--- LINENO through an alias reports the line the alias was invoked on [stdin] [xfail(legacy): legacy substitutes aliases in its own way]
alias a='echo $LINENO'
a

--- an explicit assignment to LINENO wins over the shell's own value [stdin] [xfail(legacy): legacy does not set LINENO]
LINENO=99
echo $LINENO

--- LINENO is readable from arithmetic [stdin] [xfail(legacy): legacy does not set LINENO]
echo $((LINENO))
echo $((LINENO))

--- LINENO is not exported to a child [stdin] [xfail(legacy): legacy does not set LINENO]
echo $LINENO
env | grep -c '^LINENO=' || echo none

--- LINENO inside a command substitution counts the outer script's lines [stdin] [xfail(legacy): legacy does not set LINENO]
echo $(echo $LINENO)
echo $LINENO

# The one case the yash file deliberately does not assert, and says why:
# "existing shells disagree as to how line numbers are counted within functions".
# Measured on a body whose one line is line 4 of the script:
#
#   dash  2   relative to the definition, counting the `f() {` line as one
#   zsh   1   relative to the body, counting its first line as one
#   bash  4   the absolute line in the script
#
# Three shells, three answers. lesh reports the ABSOLUTE line, with bash, for two
# reasons that are lesh's own rather than borrowed. It falls straight out of the
# mapper - a function body is a node in the tree its DEFINITION was parsed from,
# so its offset is already a real offset into the script and no separate origin
# has to be carried on the function. And it is the number that AGREES WITH THE
# DIAGNOSTIC: a fault on that line prints `file:LINE:col`, and a shell whose
# `$LINENO` and whose error message named different lines for the same physical
# line would be indefensible. dash's and zsh's answers buy a number no two shells
# and no test agree on, at the cost of state to carry it.

--- LINENO inside a function body is the absolute line in the script [stdin] [divergence: dash reports it relative to the definition (2) and zsh relative to the body (1); the yash suite declines to assert any answer because no two shells agree]
echo one
echo two
f() {
echo $LINENO
}
f
=== expect
one
two
4


# ISSUE #61, decision 1, built on #76's mapper. A runtime diagnostic is
# `file:line:col: message`, positioned at the INVOCATION SITE, with the alias
# chain in brackets. Every case below is a divergence, because the format differs
# from all four reference shells - and every one asserts the text through `2>&1`,
# since the corpus otherwise compares only whether stderr is empty.
#
# The field, measured at b662b5c for a command not found on line 3 of a script:
#
#   dash  n.sh: 3: nosuchcmd: not found
#   zsh   n.sh:3: command not found: nosuchcmd
#   bash  n.sh: line 3: nosuchcmd: command not found
#   lesh  lesh: nosuchcmd: No such file or directory     <- own name, no position
#
# dash and zsh already agree on the invocation LINE and the EXPANDED name. No
# shell reports a COLUMN and none names the ALIAS; both are lesh's addition, and
# it can make them because every node carries a source span (ast.h:23) and #40's
# regions say which text is an alias body.

--- a command not found from a script names the file, line and column [divergence: dash prints `n.sh: 3: nosuchcmd: not found`, zsh `n.sh:3: command not found: nosuchcmd`, bash `n.sh: line 3: ...`; none reports a column]
printf 'echo one\necho two\nnosuchcmd\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 127]
n.sh:3:1: nosuchcmd: not found

--- a command not found under -c names $0 [divergence: dash prints `MYNAME: 1: nosuchcmd: not found` and zsh its own name rather than $0; neither reports a column]
"$TESTEE" -c 'nosuchcmd' MYNAME 2>&1 >/dev/null
=== expect [status: 127]
MYNAME:1:1: nosuchcmd: not found

--- a command not found from standard input names $0 [divergence: dash prints `./sh: 2: nosuchcmd: not found`; zsh omits the position entirely when the script arrived on stdin]
ln -s "$TESTEE" ./sh
printf 'echo one\nnosuchcmd\n' | ./sh 2>&1 >/dev/null
=== expect [status: 127]
./sh:2:1: nosuchcmd: not found

--- the column counts from one along the line [divergence: no shell reports a column at all]
printf ': ; nosuchcmd\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 127]
n.sh:1:5: nosuchcmd: not found

# COLUMNS COUNT CHARACTERS, NOT BYTES. `é` is two bytes, so on `: é; nosuchcmd`
# the command word is at BYTE 7 and at CHARACTER 6. Character, because
# `file:line:col` is a convention with readers - GCC defaults to
# `-fdiagnostics-column-unit=display`, and every editor's problem matcher places
# its caret by character - and a byte column lands in the wrong place on any line
# holding a non-ASCII character. See src/syntax/source_map.h.

--- the column counts characters rather than bytes [divergence: no shell reports a column at all]
printf ': \303\251; nosuchcmd\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 127]
n.sh:1:6: nosuchcmd: not found

--- a command not found through an alias is reported at the word that was typed [divergence: dash and zsh report the invocation line and the expanded name as lesh does, but neither names the alias; bash does not substitute aliases in a script at all]
printf 'alias a=nosuchcmd\na\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 127]
n.sh:2:1: nosuchcmd: not found (via alias a)

--- a nested alias reports the whole chain, outermost first [divergence: dash and zsh report `nosuchcmd` at the invocation line and name neither alias; bash reports `a`, because it does not substitute aliases in a script]
printf 'alias a=b\nalias b=nosuchcmd\na\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 127]
n.sh:3:1: nosuchcmd: not found (via alias a → b)

# THE PREFIX IS NOT THE COMMAND SEARCH'S. #61 asks where the decision lives, and
# the answer has to be one place: every runtime diagnostic the shell prints goes
# through runtime/diagnostic.h, so a builtin, a redirection and an expansion are
# positioned by the same code that positions a command that was not found. dash
# and zsh position all three too.

--- a builtin's diagnostic is positioned the same way [divergence: dash prints `n.sh: 2: cd: can't cd to /nosuchdir`; the shape is the same and the column is lesh's]
printf 'echo one\ncd /nosuchdir\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 2]
n.sh:2:1: cd: /nosuchdir: No such file or directory

--- a redirection failure is positioned at the command that carried it [divergence: dash prints `n.sh: 2: cannot create /nosuch/dir/f: Directory nonexistent`; the column is lesh's, and it names the COMMAND rather than the operator, because that is the word the reader is looking for]
printf 'echo one\n: >/nosuch/dir/f\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 2]
n.sh:2:1: /nosuch/dir/f: No such file or directory

--- an expansion error is positioned at the command being expanded [divergence: dash prints `n.sh: 3: NOPE: parameter not set`; the column is lesh's]
printf 'echo one\nset -u\necho $NOPE\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 2]
n.sh:3:1: NOPE: parameter not set

# A DOT SCRIPT NAMES ITSELF. `$0` everywhere else, but a dot script's lines are
# not `$0`'s lines, and naming `$0` beside a line counted in another file is the
# one way this format can lie. bash and zsh both name the script; dash names $0
# and then appends the script's path, saying the same thing at greater length.

--- a diagnostic inside a dot script names the dot script [divergence: dash prints `n.sh: 2: /path/dot.sh: nosuchcmd: not found`, naming both; lesh names the file the line belongs to, as bash and zsh do]
printf 'echo inner\nnosuchcmd\n' >dot.sh
printf 'echo one\n. ./dot.sh\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 127]
./dot.sh:2:1: nosuchcmd: not found

# A SYNTAX ERROR POSITIONS ITSELF. It is reported before any node of its command
# has run, so nothing has told the shell where it is and the origin still holds
# the PREVIOUS command's line - which is the one line in the file that is fine.
# The offending token's `error_offset` answers it: the lexer already records where
# the quote was opened, rather than only which node ended up defective.

--- a syntax error names the line and column of the defect [divergence: dash prints `n.sh: 2: Syntax error: ";;" unexpected`; lesh reports the column, and reports the byte the lexer recorded rather than the token that follows it]
printf 'echo one\necho ;;\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 2]
n.sh:2:6: syntax error

--- an unterminated quote is reported where the quote was opened [divergence: dash prints `n.sh: 2: Syntax error: Unterminated quoted string` with no column; lesh points at the opening quote itself]
printf 'echo one\necho "abc\n' >n.sh
"$TESTEE" n.sh 2>&1 >/dev/null
=== expect [status: 2]
n.sh:2:6: syntax error: unterminated quoted string

# ISSUE #77. `run_compound_list` was the THIRD copy of the top-level command
# loop - after run_parsed and the copy #74 removed from run_source - and it had
# drifted the way a copy always does. EVERY compound body in the language runs
# through it: a brace group, a function body, a loop body, an `if` branch, a
# `case` item and a subshell. Two guards were missing, and only the first of the
# two had ever been noticed.

--- noexec set inside a brace group stops the rest of that group [xfail(legacy): legacy has no compound commands]
{ eval "set -n"; echo notreached; }; echo alsonotreached

--- noexec set inside a function body stops the rest of that body [xfail(legacy): legacy has no functions]
f() { eval "set -n"; echo notreached; }; f; echo alsonotreached

--- noexec set inside a for body stops the body and the loop [xfail(legacy): legacy has no compound commands]
for i in 1 2 3; do eval "set -n"; echo notreached; done; echo alsonotreached

--- noexec set inside an if branch stops the rest of that branch [xfail(legacy): legacy has no compound commands]
if :; then eval "set -n"; echo notreached; fi; echo alsonotreached

--- noexec set inside a case item stops the rest of that item [xfail(legacy): legacy has no compound commands]
case a in a) eval "set -n"; echo notreached;; esac; echo alsonotreached

--- noexec set inside a nested body stops every body enclosing it [xfail(legacy): legacy has no compound commands]
for i in 1 2; do { eval "set -n"; echo notreached; }; done; echo alsonotreached

# The half an over-eager guard breaks. `set -n` is an option of the SUBSHELL that
# set it, so it stops that body and nothing after the child ends. A test that
# reached out of the subshell would take the `after` with it.
#
# `while` is deliberately absent from the noexec cases above: with the body
# unable to execute, the command that would end the loop cannot run either, and
# dash itself spins forever on `while :; do eval "set -n"; break; done`. There is
# no answer to differ from.

--- noexec set inside a subshell stops that subshell only [xfail(legacy): legacy has no compound commands]
( eval "set -n"; echo notreached ); echo after

# THE SECOND DIVERGENCE, which reading the two loops side by side found and the
# ticket had not. run_parsed RE-READS `$?` when a trap body asked to exit,
# because the status the shell leaves with is the trap body's and not that of the
# command the signal interrupted. run_compound_list returned the interrupted
# command's status instead, and the top level then wrote that stale value over
# the real one - so a trap that exited 5 from inside ANY compound body left the
# shell reporting whatever it had interrupted. dash and bash both say 5.

--- a trap that exits from inside a brace group carries its status out [xfail(legacy): legacy has no compound commands]
trap "exit 5" USR1; { kill -USR1 $$; echo notreached; }; echo alsonotreached

--- a trap that exits from inside a function body carries its status out [xfail(legacy): legacy has no functions]
trap "exit 5" USR1; f() { kill -USR1 $$; echo notreached; }; f; echo alsonotreached

--- a trap that exits from inside a loop body carries its status out [xfail(legacy): legacy has no compound commands]
trap "exit 5" USR1; for i in 1 2; do kill -USR1 $$; echo notreached; done; echo alsonotreached

--- a trap that exits from inside an if branch carries its status out [xfail(legacy): legacy has no compound commands]
trap "exit 5" USR1; if :; then kill -USR1 $$; echo notreached; fi; echo alsonotreached

# THE ROWS THAT MUST NOT MOVE. A compound list is a BODY, so an escaping `break`,
# `continue` or `return` has to TRAVEL OUTWARD to the construct that owns it
# rather than ending the shell's input the way the top level's does. #58, #49 and
# #24 all rest on these, and a shared loop that consumed the unwind here would
# break every loop and every function in the language.

--- break escapes a brace group inside a loop [xfail(legacy): legacy has no compound commands]
for i in 1 2 3; do { break; }; echo notreached; done; echo after

--- continue escapes two brace groups inside a loop [xfail(legacy): legacy has no compound commands]
for i in 1 2 3; do { { continue; }; }; echo notreached; done; echo after

--- break escapes an if branch inside a loop [xfail(legacy): legacy has no compound commands]
for i in 1 2 3; do if [ "$i" = 2 ]; then break; fi; echo "$i"; done; echo after

--- break escapes a case item inside a loop [xfail(legacy): legacy has no compound commands]
for i in 1 2 3; do case $i in 2) break;; esac; echo "$i"; done; echo after

--- break with a level escapes both loops [xfail(legacy): legacy has no compound commands]
for i in 1 2; do for j in a b; do break 2; done; echo notreached; done; echo after

--- an inner break leaves the outer loop running [xfail(legacy): legacy has no compound commands]
for i in 1 2; do for j in a b; do break; done; echo "$i"; done; echo after

--- return escapes a brace group inside a function [xfail(legacy): legacy has no functions]
f() { { return 7; }; echo notreached; }; f; echo "st=$?"

--- return escapes a loop inside a function [xfail(legacy): legacy has no functions]
f() { for i in 1 2 3; do return 7; done; echo notreached; }; f; echo "st=$?"

# `set -e` inside a compound body already matched dash before #77, so these are a
# GUARD rather than a fix: the shared loop has to keep the answer the copy
# already gave. The `while` case is the one #58 traced - breaking out of the list
# alone left the enclosing loop free to iterate again, and it ran forever.

--- errexit inside a brace group exits the shell [xfail(legacy): legacy has no compound commands]
set -e; { false; echo notreached; }; echo alsonotreached

--- errexit inside a function body exits the shell [xfail(legacy): legacy has no functions]
set -e; f() { false; echo notreached; }; f; echo alsonotreached

--- errexit inside a while body exits rather than iterating again [xfail(legacy): legacy has no compound commands]
set -e; while true; do false; done; echo notreached

--- errexit does not fire on a condition an if tested [xfail(legacy): legacy has no compound commands]
set -e; if false; then echo notreached; else echo taken; fi; echo after

--- errexit does not fire on a status an or-list tested [xfail(legacy): legacy has no compound commands]
set -e; { false; } || echo handled; echo after

# The one row a shared loop CANNOT take from run_parsed. The top level answers a
# unit holding no command with `$?`; a body answers ZERO, and a `case` item is
# the shape that proves it because POSIX makes its list optional. The mirror of
# #74's empty-unit trap, and the reason the starting status is a parameter.

--- an empty case item body answers zero and not the previous status [xfail(legacy): legacy has no compound commands]
(exit 9); case a in a) ;; esac; echo "st=$?"
