# Parameter expansion, against dash (the POSIX floor).
#
# xfail markers record what is known broken, with the cause. They are the score,
# not an excuse: a marked case that starts passing is reported as XPASS and fails
# the run until its marker is removed.
#
# The field-splitting cases at the end are POSIX 2.6.5 (#42). A separator has a
# SHAPE - IFS white space, then at most one non-white-space IFS character, then
# more IFS white space - and the observable consequence is EMPTY fields, which a
# loop that merely drops IFS bytes cannot produce. They print brackets so a lost
# empty field is visible rather than invisible.
#
# The value-context cases at the end are #42's other half. One flag meant both "no
# field splitting" and "double-quoted backslash rules", so switching off the first
# switched on the second: an assignment kept the backslash in `x=\!`, a redirection
# operand looked for a file called `\in0`, and a here-document body had its quotes
# REMOVED because it was lexed as the interior of a word.
#
# The arithmetic cases at the very end are #56. `&&`, `||` and `?:` answered the
# right VALUE while still running the operand they should have skipped, because
# the mini-parser computes as it parses. Each one therefore prints the variable
# afterwards: the value alone never showed the bug. The last two are what the fix
# must not overreach into - a skipped `1/0` is not a division error and a skipped
# unset variable is not a nounset error, because neither operand was evaluated -
# and the one before them is what it must not under-reach: `0 && (x=1) || (x=2)`
# does reach the second assignment, and dash, bash and zsh all agree it does.
#
# Prose lives HERE and not between cases: a case body runs to the next `---`, so a
# comment block in the middle of the file becomes part of the PRECEDING case - and
# the legacy front end aborts on one, which showed up as a FAIL on a case this
# ticket never touched.

--- bare parameter
echo $HOME

--- parameter with prefix
echo pre$HOME

--- parameter followed by a separate word
echo $HOME post

--- braced parameter with suffix
echo ${HOME}x

--- braced parameter with prefix and suffix
echo a${HOME}b

--- braced unset parameter
echo ${NOPE}y

--- parameter with a suffix [xfail(legacy): the name scan runs past the parameter and swallows the suffix]
echo a$HOME-b

--- parameter with a dot suffix [xfail(legacy): same name-scan defect]
echo a$HOME.b

--- parameter followed by a path separator [xfail(legacy): same name-scan defect]
echo $HOME/sub

--- unset parameter with prefix and suffix [xfail(legacy): same name-scan defect]
echo x$NOPE-y

--- two parameters in one word [xfail(legacy): the second expansion overwrites rather than appends]
echo $HOME$HOME

--- two braced parameters around a literal [xfail(legacy): the second expansion overwrites rather than appends]
echo ${HOME}x${HOME}

--- parameters and literals alternating [xfail(legacy): both name-scan and accumulation defects]
echo a$HOME-$HOME-b

--- braced parameter as the last word of a pipeline stage
echo ${HOME} | cat

--- braced parameter alone
echo ${HOME}

--- IFS is a variable the shell sets, not only a mechanism [xfail(legacy): legacy has no IFS]
printf '[%s]' "$IFS" | od -c | head -1

--- IFS is set even when the environment says otherwise [xfail(legacy): legacy has no IFS]
IFS=X "$TESTEE" -c 'printf "[%s]" "$IFS"' | od -c | head -1

--- successive non-whitespace separators leave an empty field [xfail(legacy): legacy has no field splitting]
IFS=-; a=1--2; for f in $a; do printf '[%s]' "$f"; done; echo

--- a leading non-whitespace separator leaves an empty field [xfail(legacy): legacy has no field splitting]
IFS=-; a=-1; for f in $a; do printf '[%s]' "$f"; done; echo

--- a leading whitespace separator does not [xfail(legacy): legacy has no field splitting]
IFS=' '; a=' 1'; for f in $a; do printf '[%s]' "$f"; done; echo

--- whitespace around a non-whitespace separator belongs to it [xfail(legacy): legacy has no field splitting]
IFS=' -'; a='1 - 2'; for f in $a; do printf '[%s]' "$f"; done; echo

--- a separator that used its slot starts a new one [xfail(legacy): legacy has no field splitting]
IFS=' -'; a='  --33'; for f in $a; do printf '[%s]' "$f"; done; echo

--- a trailing separator adds no empty last field [xfail(legacy): legacy has no field splitting]
IFS=-; for a in 1- 1-- - --; do set -- $a; printf '%d:' $#; for f in "$@"; do printf '[%s]' "$f"; done; echo; done

--- one separator may span two expansions [xfail(legacy): legacy has no field splitting]
IFS=' '; a='1 '; b=' 2'; for f in $a$b; do printf '[%s]' "$f"; done; echo

--- an arithmetic result is field split like any other [xfail(legacy): legacy has no arithmetic expansion]
IFS=' 0'; for f in $((708)); do printf '[%s]' "$f"; done; echo

--- an empty expansion yields no field but quotes make one [xfail(legacy): legacy has no field splitting]
a=; for f in x $a ''$a; do printf '[%s]' "$f"; done; echo

--- an assignment value uses unquoted backslash rules [xfail(legacy): legacy applies no quote removal to an assignment value]
x=\!; y=a\ b; printf '[%s][%s]\n' "$x" "$y"

--- a redirection operand has its quotes removed [xfail(legacy): legacy ignores redirect nodes entirely]
echo body > /tmp/lesh_spec_in0; cat </tmp/lesh_spec_i'n'"0"

--- a here-document body keeps its quotes [xfail(legacy): legacy has no here-documents]
cat <<END
it's a"b" c
END

--- a here-document body escapes only dollar, backquote and backslash [xfail(legacy): legacy has no here-documents]
cat <<END
\$ \\ \` \" \z
END

--- an expansion error in a here-document body fails the redirection [xfail(legacy): legacy has no here-documents]
echo not printed <<END
${nope?}
END

--- an unquoted dollar-at in an assignment joins rather than keeping the last field [xfail(legacy): legacy has no positional parameters]
set a b c; x=$@; y="$@"; printf '[%s][%s]\n' "$x" "$y"

--- a default argument is expanded in the context of the expansion [xfail(legacy): legacy has no parameter expansion beyond $name]
for f in ${u-\!a b} "${u-\!a b}"; do printf '[%s]' "$f"; done; echo

--- a single quote in a default is a quote outside double quotes [xfail(legacy): legacy has no parameter expansion beyond $name]
for f in ${u-a'b c'd}; do printf '[%s]' "$f"; done; echo

--- assign-default removes quotes before assigning and splits the value after [xfail(legacy): legacy has no parameter expansion beyond $name]
for f in ${a=\ x}; do printf '[%s]' "$f"; done; echo; printf '[%s]\n' "$a"

--- only hash and percent have a doubled form [xfail(legacy): legacy has no parameter expansion beyond $name]
printf '[%s][%s]\n' "${u--x}" "${u-=x}"

--- a command substitution result is glob-eligible [xfail(legacy): legacy has no command substitution]
mkdir -p /tmp/lesh_spec_glob && : > /tmp/lesh_spec_glob/dummyfile && cd /tmp/lesh_spec_glob && for f in $(echo 'dumm*ile'); do printf '[%s]' "$f"; done; echo

--- a named tilde expands to that user's home [xfail(legacy): legacy has no tilde expansion]
printf '[%s][%s]\n' ~root ~root/sub

--- an unknown user leaves the word alone
printf '[%s]\n' ~nosuchuser12345 ~nosuchuser12345/x

--- a quoted tilde name is not a login name but its quotes come off [xfail(legacy): legacy has no tilde expansion]
printf '[%s][%s][%s][%s]\n' ~"root" ~'root' ~ro\ot ~\/

--- a tilde is eligible after an unquoted colon in an assignment [xfail(legacy): legacy has no tilde expansion]
a=x:~root:~root; b=x:~; c=':'~root; printf '[%s][%s][%s]\n' "$a" "$b" "$c"

--- a tilde after a colon is not eligible outside an assignment
printf '[%s]\n' x:~ ~:~

--- a tilde prefix holding an expansion is not a login name [xfail(legacy): legacy has no tilde expansion]
u=root; printf '[%s]\n' ~$u

--- an absent dollar-at in double quotes yields no field at all [xfail(legacy): legacy has no positional parameters]
set --; for f in "$@"; do printf '[%s]' "$f"; done; echo END

--- but an empty variable in those quotes still starts one [xfail(legacy): legacy has no positional parameters]
set --; n=; for f in "$n""$@" "=$@=" ""; do printf '[%s]' "$f"; done; echo END

--- two absent dollar-ats in one quoted region [divergence: dash prints one empty field here where bash, zsh and macOS /bin/sh all print none. POSIX makes `"$@"` with no positional parameters zero fields, quotes and all, and lesh applies that to a quoted region holding nothing else; dash's own `"$@""$@"` prints none, so it is inconsistent with itself]
set --; for f in "$@$@"; do printf '[%s]' "$f"; done; echo END
=== expect
END

--- assigning to a positional parameter is refused [xfail(legacy): legacy has no parameter expansion beyond $name]
echo ${1:=x}

--- assigning to a special parameter is refused [xfail(legacy): legacy has no parameter expansion beyond $name]
echo ${*:=x}

--- but not when the parameter is already set [xfail(legacy): legacy has no parameter expansion beyond $name]
set a; echo ${1=x}

--- dollar-at and dollar-star are always set [xfail(legacy): legacy has no parameter expansion beyond $name]
printf '[%s][%s][%s]\n' "${@-unset}" "${*-unset}" "${@:-unset}"

--- the length form needs a name after the hash [xfail(legacy): legacy has no parameter expansion beyond $name]
set a b; printf '[%s][%s][%s][%s][%s]\n' "${#+y}" "${#-y}" "${#=y}" "${#?}" "${#?X}"

--- a quoted metacharacter in a trim pattern is data [xfail(legacy): legacy has no parameter expansion beyond $name]
s='***'; h='###'; printf '[%s][%s][%s][%s]\n' "${s#'*'}" "${s##'*'}" "${s#\*}" "${h#'#'}"

--- an unquoted metacharacter in a trim pattern still wildcards [xfail(legacy): legacy has no parameter expansion beyond $name]
a=1-2-3-4; printf '[%s][%s][%s]\n' "${a#*-}" "${a##*-}" "${a#*1}"

--- an expansion in a trim pattern is a pattern unless it was quoted [xfail(legacy): legacy has no parameter expansion beyond $name]
w='ab\bc'; a='*'; printf '[%s]\n' "${w#${a}b}"

--- a substitution inside double quotes may hold quotes of its own [xfail(legacy): legacy has no command substitution]
echoraw() { printf '%s\n' "$*"; }
echoraw "$(echoraw "x")" "`echoraw "a"'b'`" "${e=a"b"c}"

--- a brace inside quotes does not close an expansion [xfail(legacy): legacy has no parameter expansion beyond $name]
a=set; printf '[%s][%s][%s]\n' "${a+'}'}" "${a+"}"}" "${a+a\}b}"

--- and an opening brace is not special there [xfail(legacy): legacy has no parameter expansion beyond $name]
a=set; printf '[%s]\n' "${a+\{}"

--- a single quote inside braces inside double quotes is an ordinary byte [xfail(legacy): legacy has no parameter expansion beyond $name]
printf '[%s]\n' "${x-'}"

--- backquotes have their escapes removed before the body is parsed [xfail(legacy): legacy has no command substitution]
echoraw() { printf '%s\n' "$*"; }
echoraw `echoraw \`echoraw x\``
echoraw `echoraw '\$y'`
echoraw `printf '%s\n' \\\\`

--- and double quotes add the quote to that set [xfail(legacy): legacy has no command substitution]
echoraw() { printf '%s\n' "$*"; }
echoraw "`echoraw \"1\"`"
echoraw `echoraw \" "\"" '\"'`
echoraw "`echoraw \'2\'`"

--- a paren inside quotes does not close a command substitution [xfail(legacy): legacy has no command substitution]
echo "$(echo ")")" $(echo ')')

--- a paren inside a comment does not close one either [xfail(legacy): legacy has no command substitution]
echo $(
echo a # ) comment
)

--- a short-circuited && operand is not evaluated [xfail(legacy): legacy has no arithmetic expansion]
x=0; echo $(( 0 && (x=1) )); echo "x=$x"

--- a short-circuited || operand is not evaluated [xfail(legacy): legacy has no arithmetic expansion]
x=0; echo $(( 1 || (x=1) )); echo "x=$x"

--- a conditional evaluates only the branch it takes [xfail(legacy): legacy has no arithmetic expansion]
a=0; b=0; echo $(( 1 ? (a=5) : (b=-5) )); echo "$a $b"
a=0; b=0; echo $(( 0 ? (a=-5) : (b=5) )); echo "$a $b"

--- an operand that is reached still assigns [xfail(legacy): legacy has no arithmetic expansion]
x=0; echo $(( 1 && (x=1) )); echo "x=$x"; echo $(( x = 7 )) $(( x += 3 )); echo "x=$x"

--- skipping stops where the skipped operand does [xfail(legacy): legacy has no arithmetic expansion]
x=0; echo $(( 0 && (x=1) || (x=2) )); echo "x=$x"

--- and it does not resume inside the operand it skipped [xfail(legacy): legacy has no arithmetic expansion]
x=0; y=0; echo $(( 0 && (0 || (x=1)) )); echo $(( 1 || (x=1) && (y=1) )); echo "$x $y"

--- a division by zero in a skipped operand is not an error [xfail(legacy): legacy has no arithmetic expansion]
echo $(( 0 && 1/0 )) $(( 1 || 1%0 )) $(( 1 ? 2 : 1/0 ))

--- nor is an unset variable one, with nounset on [xfail(legacy): legacy has no arithmetic expansion]
set -u; echo $(( 0 && y )); echo after
