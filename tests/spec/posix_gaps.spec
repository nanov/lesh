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
