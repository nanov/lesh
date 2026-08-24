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
