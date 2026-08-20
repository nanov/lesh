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

--- redirection to a file [xfail: #20 - the executor ignores redirect nodes]
echo hi > /tmp/lesh_spec_redirect && cat /tmp/lesh_spec_redirect

--- append redirection [xfail: #20]
echo a > /tmp/lesh_spec_app && echo b >> /tmp/lesh_spec_app && cat /tmp/lesh_spec_app

--- stderr redirection [xfail: #20]
ls /nonexistent 2>/dev/null; echo done

--- if statement [xfail: #19 - compound commands]
if true; then echo yes; fi

--- if else [xfail: #19]
if false; then echo yes; else echo no; fi

--- while loop [xfail: #19]
i=0; while [ $i -lt 2 ]; do echo $i; i=$((i+1)); done

--- for loop [xfail: #19]
for x in a b c; do echo $x; done

--- case statement [xfail: #19]
case abc in a*) echo matched;; *) echo no;; esac

--- subshell [xfail: #19]
(echo inside)

--- brace group [xfail: #19]
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

--- here-document [xfail: #21 - here-documents]
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
