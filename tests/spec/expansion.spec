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

# yash's tilde-p.tst expects `~/foo` under HOME=/ to collapse to /foo, and `~/~`
# under HOME=/foo/bar/ to give /foo/bar/~. POSIX 2.6.1 replaces the tilde-prefix
# with the VALUE of HOME and says nothing about the slash that follows it, so the
# doubled slash is literally what the rule produces. dash answers as lesh does,
# and so does zsh: yash is the shell NORMALISING, and normalising is the change
# that would need an argument, not keeping the replacement literal.
#
# Three of tilde-p.tst's assertions therefore stay unpassed by choice rather than
# by omission. This is a plain case and not a `[divergence: ...]`: lesh and dash
# agree, and a divergence case is the form for the one thing the differential
# principle cannot express - lesh answering differently from its reference shell.
# What the case pins is that neither shell's answer drifts, and that a later
# reading of the suite does not mistake these three for a gap.
#
# The normalisation is not merely cosmetic and the counter-argument is recorded
# rather than dismissed: POSIX leaves a pathname beginning with exactly two
# slashes implementation-defined, so `//foo` may name something `/foo` does not.
# That argues for collapsing at the point a PATHNAME is resolved, where the rule
# would apply to every doubled slash however it arose, not inside tilde expansion,
# where it would make one expansion's output disagree with the value of HOME it
# was told to substitute.

--- a tilde prefix is replaced by HOME exactly, doubled slash and all [xfail(legacy): legacy has no tilde expansion]
HOME=/foo/bar/; printf '[%s][%s]\n' ~ ~/~
HOME=/; printf '[%s][%s]\n' ~ ~/foo
HOME=//; printf '[%s][%s]\n' ~ ~/foo

# #69: `path-p.tst`'s one failure was NOT the pattern matcher #23 built - matching
# is right - it was the filesystem walk in glob.cpp that turns a match into a
# pathname. A literal component after a matched wildcard directory was appended
# unconditionally, with no existence check at all: the walk could tell "matched"
# from "did not match" for a wildcard component (readdir just does not return an
# entry), but had no way to tell "confirmed" from "could not check" for a literal
# trailing one.
#
# Without search (`x`) permission on `no_search_dir`, resolving `no_search_dir/file`
# by name fails with EACCES - not ENOENT. That is the same shape as #34's sentinel
# bug in `apply_redirection`, which could not tell "nothing to save" from "the fd
# was closed": here, "the file does not exist" and "existence could not be checked"
# were the same case standing in for two different truths, and the walk had chosen
# the wrong one. lesh was asserting a file existed that it was never permitted to
# look for. This case runs identically whether or not it is root: a real kernel
# permission check underneath is what makes dash and lesh agree either way, not a
# fixed expected value asserted here.

--- a literal component past a directory the walk cannot search is not confirmed [xfail(legacy): legacy has no pathname expansion]
mkdir -p foo/no_search_dir; >foo/no_search_dir/file; chmod a-x foo/no_search_dir
echo foo/no_search_d*r/file
chmod a+x foo/no_search_dir

# #68: the scan that finds a command substitution's closing `)` did not
# understand the shell text it was scanning. Two constructs put an unbalanced `)`
# into a command list and both were read as the substitution's own: the `)` that
# ends a case pattern list, and any `)` inside a here-document body. The pattern
# case was hidden by the optional leading `(` - `(a)` balances - so only a clause
# written the ordinary way, and every `*)` is one, reached the paren counter.
#
# bash gets both of these wrong too, byte for byte on the here-document one; dash,
# zsh and yash all get them right. dash gets them right by PARSING the
# substitution where it stands rather than scanning for a paren, which is not the
# shape here: the lexer owns no memory and cannot build a tree, and the body is
# parsed later anyway. So the scan learned the two constructs instead, under the
# same command-position rule the parser uses - `case` and `esac` are reserved
# words only where a command could begin, which is why `echo case` below still
# prints a word rather than opening a clause.

--- a case pattern list's paren does not end a command substitution [xfail(legacy): legacy misreads the pattern's paren]
echo $(case a in a) echo x;; esac)
echo $(case a in (a) echo x;; *) echo y;; esac)
echo $(case b in a) echo x;; *) echo y;; esac)
echo $(case a in a) case b in b) echo nested;; esac;; esac)
echo $(case a in a|b) echo alt;; esac)
echo $(case a in a) (echo sub);; esac)

--- case and esac away from command position stay ordinary words [xfail(legacy): legacy misreads the pattern's paren]
echo $(echo case)
echo $(echo esac in)
echo $(case a in a) echo esac;; esac)
echo $(echo $(echo a) case)

--- a paren in a here-document body does not end a command substitution [xfail(legacy): legacy ends the substitution inside the body]
echo $(cat <<\END
foo)
END
)
echo "$(cat <<'END'
a) b $( ` '
END
)"
echo $(cat <<-END
	tabbed)
	END
)
echo $(cat <<A <<B
one)
A
two)
B
)

--- a here-document inside a substitution leaves the enclosing one alone [xfail(legacy): legacy ends the substitution inside the body]
cat <<\OUTER; echo "$(cat <<\INNER
inner)
INNER
)"
outer)
OUTER

--- arithmetic inside a substitution is not read as a command list [xfail(legacy): legacy misreads the pattern's paren]
echo $((1<<2))
echo $(echo $((1<<2)))
echo $(( (1<<2) + 3 ))
echo $(case a in a) echo $((1<<3));; esac)

# --- $'...' : ANSI-C quoting (#75) -------------------------------------------
#
# EVERY case below is a divergence, and for one reason: dash has no `$'...'` at
# all. It parses `$'a\nb'` as the parameter `$a` followed by the literal text
# `\nb`, so it cannot be the expectation of a single one of them - the whole
# content of the decision is that lesh answers differently.
#
# It is NOT the `;&`/`pipefail` shape of divergence, where lesh stands with the
# standard against dash's age. Two things separate it:
#
#   - bash, zsh and yash all implement `$'...'`. Three shells to one, rather
#     than lesh alone.
#   - lesh used to fail it WORSE than dash. dash mis-parses and runs on; lesh
#     raised `syntax error: unterminated quoted string` and refused the whole
#     line. A script using `$'...'` ran (wrongly) under dash and did not run at
#     all here. Refusing to parse an increasingly common construct is a worse
#     answer than any of the four shells gives, which is the argument for
#     implementing it rather than recording a gap.
#
# So the expectations below are BASH's, measured rather than remembered - with
# exactly one deliberate exception, marked where it appears (`\e`).

--- $'...' decodes the escapes POSIX Issue 8 names [divergence: dash has no $'...' and reads `$'\t'` as the parameter `$` followed by `\t`; bash, zsh and yash all decode it, and these are bash's bytes]
h() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; echo .; }
h $'plain'
h $'\a\b\f\n\r\t\v'
h $'a\\b'
h $'a\'b'
h $'a\"b'
h $'\cA'
h $'\ca'
h $'\cM'
h $'\c?'
h $'\e'
h $'\E'
=== expect
706c61696e.
07080c0a0d090b.
615c62.
612762.
612262.
01.
01.
0d.
7f.
1b.
1b.

# `\cX` is `toupper(X) XOR 0x40`, which is why `\cA` and `\ca` are both 001 and
# `\c?` is 0177. zsh does NOT implement `\cX` - it prints a literal `cA` - so
# this one follows bash and POSIX Issue 8 against zsh rather than with it.
#
# `\e` and `\E` are NOT in POSIX Issue 8's list, and were left out on that
# reading until quote-p.tst:402 - the one assertion this work exists to move -
# turned out to require 033 for `\e`. A set the ticket's own measure rejects is
# the wrong set. bash and zsh both agree with the suite here.

--- $'\cX' reads its argument as an escape, so control-backslash is $'\c\\' [divergence: dash has no $'...'; this is also a deliberate difference from BASH, which fails quote-p.tst:402 for it - see below]
h() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; echo .; }
h $'\cA\ca\c^\c\\\c?'
h $'\c\\'
h $'a\c'
=== expect
01011e1c7f.
1c.
615c63.

# These are quote-p.tst:402's own bytes, and the reason this is not simply
# "follow bash": BASH FAILS THAT ASSERTION. It takes the RAW byte after `\c`, so
# it reads `\c\` as control-backslash and is then left with `\c?` as three stray
# bytes - `1c 5c 63 3f` where the suite expects `1c 7f`. Measured, both shells,
# same machine.
#
# yash's reading is taken instead. The character `\c` applies to is itself
# unescaped first, so control-backslash is spelled `\c\\` and takes four bytes.
# Two reasons: it is what the POSIX conformance suite asserts, and it is the
# only reading under which control-backslash has an unambiguous spelling at all.
# zsh implements no `\cX` whatever and casts no vote.
#
# A `\c` with nothing after it is not an escape and keeps both bytes.

--- $'\xHH' takes at most two hex digits and an incomplete one keeps its backslash [divergence: dash has no $'...'; bash's bytes, and zsh disagrees with bash on the incomplete form - see the comment below]
h() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; echo .; }
h $'\x41'
h $'\x4'
h $'\xaF'
h $'\x414243'
h $'\xZ'
h $'a\x'
h $'\XA'
=== expect
41.
04.
af.
4134323433.
5c785a.
615c78.
5c5841.

# Two digits at most, which is what makes `\x414243` the byte 0x41 followed by
# the four TEXT bytes `4243` and not a wide character. One digit is enough.
# Case-insensitive in the digits, but `\X` is not an escape at all.
#
# An incomplete `\x` - no hex digit after it - is where bash and zsh part
# company: bash keeps the two bytes `\x`, zsh emits a NUL. Bash's rule is the
# one an unrecognised escape already follows below, so taking it keeps ONE rule
# for "this was not an escape after all" instead of two.

--- $'\0NNN' takes at most three octal digits, the leading zero among them [divergence: dash has no $'...'; bash and zsh agree exactly here, and these are their bytes]
h() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; echo .; }
h $'\101'
h $'\0101'
h $'\0377'
h $'\377'
h $'\7'
=== expect
41.
0831.
1f37.
ff.
07.

# POSIX Issue 8 spells the escape `\0nnn`, which reads as a mandatory zero plus
# three digits. It is not what any shell implements, and the difference is
# visible: `\0101` is 0x08 followed by the text `1` in bash AND zsh, not 'A'.
# The rule both implement is up to THREE octal digits after the backslash, with
# the leading zero merely one of them - so `\101` is 'A' and `\0101` is `\010`
# then `1`. Two shells agreeing against a literal reading of the draft is the
# stronger evidence, so lesh implements what they do.

--- a NUL byte truncates the decoded string [divergence: dash has no $'...'; bash truncates and zsh embeds the NUL - lesh follows bash, for the reason below]
h() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; echo .; }
h $'a\0bc'
h $'a\x00bc'
h $'\400'
h $'x\c@y'
=== expect
61.
61.
.
78.

# `\0`, `\x00`, an octal escape that overflows a byte to zero, and `\c@` all
# reach the same place, so they get one answer.
#
# bash truncates the string there; zsh keeps the NUL as a byte. lesh follows
# bash because the alternative is a lie it cannot sustain: a field here is a
# view into an arena, so an embedded NUL survives inside the shell and is then
# truncated by execve on the way to an external command - the same word would
# mean one thing to a builtin and another to /bin/cat. Truncating at decode is
# the honest version of a limit that exists either way.

--- an unrecognised escape keeps its backslash [divergence: dash has no $'...'; bash keeps the backslash and zsh drops it - lesh follows bash]
h() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; echo .; }
h $'a\qb'
h $'plain'
h $'\u0041'
h $'\U00000041'
h $'a\
b'
=== expect
615c7162.
706c61696e.
5c7530303431.
5c553030303030303431.
615c0a62.

# `\q` is `\q`, both bytes, as in bash; zsh would print `aqb`.
#
# `\u` and `\U` ARE THE ESCAPES THIS DELIBERATELY LEAVES OUT, so they fall
# through the rule above and keep their backslashes where bash and zsh would
# both print `A`. They name a CODE POINT rather than a byte, so implementing
# them means choosing an ENCODING, and every other escape here is a byte escape
# that needs no such choice. Neither POSIX Issue 8 nor quote-p.tst asks for
# them. Recorded as an assertion rather than a claim in a commit message, so
# that adding them later is a visible change to these lines.
#
# `\e` was in this list until quote-p.tst:402 - the one assertion this work
# exists to move - turned out to REQUIRE it, carrying 033 in its expected bytes.
# It is implemented; see the escape-set case above.
#
# A backslash-newline is NOT a line continuation inside `$'...'`: it is an
# unrecognised escape like any other, so both bytes survive. That is bash's
# answer and it is the one that falls out of having a single rule.

--- $'...' is a quoting form: its result is neither field-split nor pathname-expanded [divergence: dash has no $'...'; bash's answers]
h() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; echo .; }
set -- $'a b' c; echo "fields=$#"
set -- $''; echo "empty=$#"
d=$(mktemp -d); cd "$d" && touch aa ab; echo $'*'; echo *
h x$'a\nb'y
=== expect
fields=2
empty=1
*
aa ab
78610a6279.

# The point of the last two lines: `echo $'*'` prints an asterisk in a directory
# where `echo *` prints two filenames, so the quoting really did survive into
# pathname expansion rather than the bytes merely happening to be safe. And an
# empty `$''` still STARTS a field, exactly as `''` does.
#
# `x$'a\nb'y` is one word with a newline in the middle of it - the case that
# proves the decoded bytes join the word they are part of rather than becoming a
# word of their own.

--- $'...' is not special inside double quotes [divergence: dash has no $'...'; bash and zsh agree that inside double quotes it is literal text]
h() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; echo .; }
h "$'a\nb'"
h "x$'\t'y"
=== expect
2427615c6e6227.
7824275c742779.

# Seven bytes and nine bytes: the `$`, the quotes and the backslash all survive.
# A single quote is an ordinary byte inside double quotes, so there is no
# `$'...'` there to recognise - the same rule that makes `echo "it's"` print
# `it's`, applied one layer up.

--- $'...' works as a case pattern, quoting included [divergence: dash has no $'...'; bash's answers]
case abc in $'*') echo literal;; *) echo wildcard;; esac
case '*' in $'*') echo literal;; *) echo wildcard;; esac
case $'a\tb' in $'a\x09b') echo match;; *) echo no;; esac
case $'a\tb' in a?b) echo qmark;; *) echo no;; esac
=== expect
wildcard
literal
match
qmark

# `$'*'` is a literal asterisk in a pattern and matches only an asterisk, which
# is the pattern-side proof that this is a QUOTING form and not merely a decoder
# - if the quoting were lost the first line would print `literal`. The third
# line spells the same tab two different ways on the two sides and still
# matches; the fourth shows an unquoted `?` still wildcarding beside it.

--- $'...' works as a here-document delimiter, and quotes the body [divergence: dash has no $'...'; bash and zsh both decode the delimiter and treat the body as quoted]
x=VALUE
cat <<$'E\x4ED'
body $x
END
cat <<$'END'
second $x
END
=== expect
body $x
second $x

# `$'E\x4ED'` is the delimiter `END`, so the body ends at a line reading END -
# POSIX applies quote removal to the delimiter word, and this is one more
# spelling of it beside `<<\END`, `<<'END'` and `<<"END"`. Because the delimiter
# is QUOTED the body is not expanded, so `$x` stays four literal bytes even
# though x is set.

--- an unterminated $'...' is a syntax error [divergence: dash has no $'...' and prints an empty line at status 0, having read `$'abc` as the parameter `$` and some text; bash and zsh both refuse it]
echo $'abc
=== expect [status: 2] [stderr]

# lesh refused this before the feature existed, for the wrong reason - it could
# not parse `$'` at all. It refuses it now for the right one, and the case
# distinguishes them by asserting the status rather than merely the failure.
