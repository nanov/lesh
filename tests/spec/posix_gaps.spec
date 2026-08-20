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

# DELIBERATE divergences from dash, recorded rather than chosen quietly
# (handoff.md: where dash is behind the standard, say so in writing). Each is an
# assertion dash itself fails in the yash suite, so the xfail marker here means
# "lesh is ahead", and an XPASS would mean dash had changed.
#
# The `set -o` listing is the one entry that is not a standards question: POSIX
# leaves the format unspecified, dash lists four options of its own that lesh does
# not have, and printing a name for a switch that does not exist would be the lie
# `set -o` is meant to expose.

--- redirection operands with no command name are expanded in a subshell [xfail: divergence - dash expands them in the current environment; POSIX 2.9.1 requires a subshell]
unset x; < ${x=no/such/file}; ${x+echo leaked}; echo done

--- duplicating a read-only descriptor onto an output fd is an error [xfail: divergence - dash does not check the access mode; POSIX 2.7.6 requires it]
3</dev/null >&3; echo "status=$?"

--- duplicating a write-only descriptor onto an input fd is an error [xfail: divergence - dash does not check the access mode; POSIX 2.7.5 requires it]
cat 3>/dev/null <&3; echo "status=$?"

--- nounset applies inside arithmetic [xfail: divergence - dash expands $((x)) on an unset x to zero; POSIX requires the error, and dash fails option-p.tst for it]
set -u; echo "[$((x))]"; echo after

--- pipefail makes a pipeline report its rightmost failing stage [xfail: divergence - dash has no pipefail and fails both of pipeline-p.tst's cases for it; POSIX Issue 8 defines it]
set -o pipefail; exit 1 | exit 2 | exit 0; echo "b $?"; exit 3 | exit 0 | exit 0; echo "c $?"

--- set -o lists only the options the shell has [xfail: divergence - dash also lists interactive, stdin, emacs and debug, which POSIX does not name and lesh does not have]
set -o
