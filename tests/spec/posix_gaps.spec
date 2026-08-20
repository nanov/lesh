# Constructs the new front end does not implement yet, one ticket per group.
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

--- glob expansion [xfail: #23 - pathname expansion]
cd /tmp && echo /tmp/*

--- cd builtin [xfail: #24 - builtins run in this process]
cd /tmp && pwd

--- variable assignment persists [xfail: #24 - assignments are parsed, never applied]
x=value; echo $x

--- function definition and call [xfail: #25 - functions]
greet() { echo hello; }; greet

--- here-document [xfail: #21 - here-documents]
cat <<EOT
body
EOT
