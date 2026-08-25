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
# The arithmetic short-circuit cases near the end are #56. `&&`, `||` and `?:` answered the
# right VALUE while still running the operand they should have skipped, because
# the mini-parser computes as it parses. Each one therefore prints the variable
# afterwards: the value alone never showed the bug. The last two are what the fix
# must not overreach into - a skipped `1/0` is not a division error and a skipped
# unset variable is not a nounset error, because neither operand was evaluated -
# and the one before them is what it must not under-reach: `0 && (x=1) || (x=2)`
# does reach the second assignment, and dash, bash and zsh all agree it does.
#
# The overflow cases at the very end are #59, and they are ordinary differential
# cases rather than divergences because lesh matches dash on every one: an
# operator WRAPS, where all three reference shells agree, and a literal too large
# to represent SATURATES, which is dash alone - bash wraps the literal too and zsh
# diagnoses it. `$((-9223372036854775808))` is the case that tells saturation from
# wrapping apart, there being no negative literal for it to be: dash answers
# -9223372036854775807. The last case is the one #56's `_live` rule would have
# constrained had overflow been made an error; it is not one, so a skipped operand
# whose literal overflows still reports nothing and still assigns nothing.
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

--- an arithmetic operator wraps rather than overflowing [xfail(legacy): legacy has no arithmetic expansion]
echo $(( 9223372036854775807 + 1 )) $(( -9223372036854775807 - 2 )) $(( 9223372036854775807 * 2 ))

--- a literal too large to represent saturates, in every base [xfail(legacy): legacy has no arithmetic expansion]
echo $(( 99999999999999999999 )) $(( 0xFFFFFFFFFFFFFFFFF )) $(( 077777777777777777777777 ))
echo $(( 99999999999999999999 - 7 )) $(( -9223372036854775808 ))

--- dividing the negative limit by -1 wraps [xfail(legacy): legacy has no arithmetic expansion]
m=$(( -9223372036854775807 - 1 )); echo $(( m / -1 )) $(( m % -1 )) $(( -m ))

--- overflow in a skipped operand is not reported [xfail(legacy): legacy has no arithmetic expansion]
x=0; echo $(( 0 && 99999999999999999999 )) $(( 1 || 9223372036854775807 + 1 )); echo $(( 0 && (x=99999999999999999999) )); echo "x=$x"

# A `case` pattern is a PATTERN, which is the same property `${x#word}` already
# carries: quoting inside it becomes a backslash escape rather than nothing,
# because a matcher's only channel for "this asterisk is data" is `\*`. It is also
# expanded as ONE value rather than a field list, so a pattern that expands to
# nothing is an EMPTY pattern that matches an empty subject - not a pattern that
# is skipped, which is what dropped the item in `case $(true) in $(true))`.

--- a quoted metacharacter in a case pattern is data [xfail(legacy): legacy has no case clause]
case '*ab' in \*\*\*) echo no1;; '***') echo no2;; "***") echo no3;; \**) echo matched;; esac

--- and so is a quoted question mark or bracket [xfail(legacy): legacy has no case clause]
case '?a' in \?\?) echo no1;; '??') echo no2;; \??) echo yes1;; esac
case '[a' in \[\[abc]) echo no3;; '[['abc]) echo no4;; \[[abc]) echo yes2;; esac

--- an unquoted metacharacter in a case pattern still wildcards [xfail(legacy): legacy has no case clause]
case abc in a*) echo star;; esac; case abc in a?c) echo quest;; esac; case abc in [ab]bc) echo brack;; esac

--- a backslash arriving from an expansion in a case pattern is special unless quoted [xfail(legacy): legacy has no case clause]
bs='\a\z'; case az in $bs) echo bs1;; esac; case '\a\z' in "$bs") echo bs2;; esac

--- quoting in a case subject comes off without leaving escapes behind [xfail(legacy): legacy has no case clause]
bs='\a\z'; case $bs in '\a\z') echo bs1;; esac; case "$bs" in '\a\z') echo bs2;; esac

--- a case pattern that expands to nothing is an empty pattern, not no pattern [xfail(legacy): legacy has no case clause]
n=; case '' in $n) echo one;; esac; case $n in $n) echo two;; esac; case $(true) in $(true)) echo three;; esac

--- a case pattern is not field-split however IFS reads [xfail(legacy): legacy has no case clause]
IFS=:; p='a:b'; case 'a:b' in $p) echo joined;; esac; case a in $p) echo no;; esac

# A bracket expression is not just a list of bytes. POSIX admits `[:class:]`,
# `[.symbol.]` and `[=equivalent=]` inside the brackets, and the shell's own
# quoting arrives here as a backslash - so what looks like one loop over
# characters is three kinds of element and a range that may be written the long
# way. fnmatch-p.tst's 'brackets' is the whole of it in eighteen lines.

--- a bracket expression holds character classes [xfail(legacy): legacy has no case clause]
for c in a Z 7 ' ' '!' '	'; do
  for k in lower upper alpha digit alnum punct graph print cntrl blank space xdigit; do
    eval "case \"\$c\" in [[:\$k:]]) printf '%s ' \"\$k\";; esac"
  done
  echo
done

--- a class is one member of a set like any other [xfail(legacy): legacy has no case clause]
for c in a 7 b; do case $c in [a[:digit:]]) echo "$c in";; *) echo "$c out";; esac; done
for c in a 7; do case $c in [![:digit:]]) echo "$c in";; *) echo "$c out";; esac; done

--- collating symbols and equivalence classes name one character each [xfail(legacy): legacy has no case clause]
case a in [[.a.]]) echo dot;; esac
case a in [[=a=]]) echo equiv;; esac
case 1 in [[.0.]-[.2.]]) echo range;; esac
case 3 in [[.0.]-[.2.]]) echo notreached;; esac

--- a quoted metacharacter inside a bracket expression is one member of it [xfail(legacy): legacy has no case clause]
case '*' in ["*"]) echo star;; esac
case '\' in ["*"]) echo notreached;; esac
case '-' in [a\-c]) echo dash;; esac
case b in [a\-c]) echo notreached;; esac

--- an unknown class name matches nothing at all [xfail(legacy): legacy has no case clause]
for c in a '[' ':'; do case $c in [[:nosuch:]]) echo "$c in";; *) echo "$c out";; esac; done

--- a case subject is one value rather than a field list [xfail(legacy): legacy has no case clause]
c=' '; case $c in ' ') echo space;; *) echo other;; esac
d='a b'; case $d in 'a b') echo joined;; *) echo split;; esac
set -- x y; case $@ in 'x y') echo at;; *) echo notat;; esac
case $* in 'x y') echo star;; *) echo notstar;; esac

--- and IFS does not break it in two [xfail(legacy): legacy has no case clause]
IFS=:; v='a:b'; case $v in 'a:b') echo whole;; *) echo halves;; esac

# POSIX 2.9.1's DECLARATION UTILITY. `export` and `readonly` take operands that
# LOOK like assignments and must be expanded like them: a tilde is eligible after
# an unquoted colon, and neither field splitting nor pathname expansion applies.
# Which words those are is decided from the word AS WRITTEN, so `export $a` is an
# ordinary argument that really does split - declutil-p.tst asserts both halves.

--- a declaration utility's assignment operand is neither split nor globbed [xfail(legacy): legacy has no field splitting]
>tmpfile; a='1  *  2'; export A=$a; readonly R=$a; printf '[%s][%s]\n' "$A" "$R"

--- a tilde after a colon is eligible in a declaration utility's operand [xfail(legacy): legacy has no tilde expansion]
HOME=/foo; export A=~:~; readonly R=x:~/y:~; printf '[%s][%s]\n' "$A" "$R"

--- a declaration utility's other operands are split and globbed as usual [xfail(legacy): legacy has no field splitting]
A=foo B=bar a='A B'; export $a && printf '[%s][%s]\n' "$A" "$B"

--- command does not stop a declaration utility from being one [xfail(legacy): legacy has no field splitting]
a='1  *  2'; command command export A=$a; command readonly R=$a; printf '[%s][%s]\n' "$A" "$R"

--- the assignment form is read from the word as written, not from its expansion [xfail(legacy): legacy has no tilde expansion]
HOME=/foo; export "A"=~:~; a='x y'; export "B=$a"; printf '[%s][%s]\n' "$A" "$B"

--- an ordinary utility's NAME=value argument is still a command argument [xfail(legacy): legacy has no field splitting]
a='x y'; printf '[%s]\n' A=$a
