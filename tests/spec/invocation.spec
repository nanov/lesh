# How the shell is INVOKED, against dash (the POSIX floor).
#
# This file exists because the harness could not express any of it until issue
# #41. Every case used to run as `shell -c code` and nothing else, so no case
# could see a bug in how the shell reads its own command line or its own standard
# input - and two real bugs escaped through exactly that gap:
#
#   - `+i` was taken for a script pathname (#33). The yash suite runs the testee
#     as `sh +i +m`; lesh exited 127 before reading a byte, and ~3,600 assertions
#     failed for a reason that had nothing to do with what they tested.
#   - `read` returned EOF forever when the script arrived on fd 0 (#31), because
#     `main` drains fd 0 and `read` used a stale FILE*. read-p.tst scored 1/32.
#     The fix had to be asserted in a unit test, because no case could say it.
#
# Two mechanisms, both from #41. `[stdin]` on a case header feeds the body to the
# shell on file descriptor 0 with no arguments, which is what a real script looks
# like. `$TESTEE` names the shell currently under test, so a case can re-invoke it
# with any argv it likes and the comparison stays lesh-invoking-lesh against
# dash-invoking-dash. A case that needs a particular argv[0] symlinks it itself.
#
# Files go under `mktemp -d`, as everywhere else in this corpus: cases run in
# whatever directory ctest was started from.

--- +i and +m are accepted, and the script still arrives on standard input
d=$(mktemp -d); printf 'echo ok\n' > $d/s; "$TESTEE" +i +m < $d/s

--- +i and +m are accepted alongside -c
"$TESTEE" +i +m -c 'echo ok'

--- an option and its negation on the same command line
"$TESTEE" -f +f -c 'echo ok'

--- read reads fd 0 when the script itself arrives on standard input [stdin]
read a <<\END
A
END
echo "$? [${a-unset}]"

--- read reads fd 0 in a shell whose script arrives on standard input
d=$(mktemp -d); printf 'read a <<\\END\nA\nEND\necho "$? [${a-unset}]"\n' > $d/s; "$TESTEE" < $d/s

--- a script on standard input is read ahead of itself, so read sees EOF [stdin]
printf 'first\n'
read x
echo "[${x-unset}]"

--- the shell with no arguments at all reads its script from standard input [stdin]
echo ok
echo "[$#]"

--- the exit status of a script that arrived on standard input [stdin]
exit 17

--- with -c the operand after the command string is $0 and the next is $1
"$TESTEE" -c 'printf "[%s]\n" "$0" "$@"' 'command  name' 1 '2  2'

--- with -c and no operands there are no positional parameters
"$TESTEE" -c 'echo "[$#]"'

--- -s reads standard input even though operands were given
"$TESTEE" -s X 'Y  Y' <<'EOF'
printf '[%s]\n' "$@"
EOF

--- -c does not consume standard input
d=$(mktemp -d); printf 'printed text\n' > $d/in; "$TESTEE" -c cat < $d/in

--- a script's pathname is its own $0
d=$(mktemp -d); printf 'echo "$0"\n' > $d/s; cd $d && "$TESTEE" ./s

--- -cn reads the n as an option and still honours noexec
d=$(mktemp -d); cd $d && "$TESTEE" -cn 'echo hi > f'; echo "status=$? [$(ls $d)]"

--- -e on the command line makes a failing command exit the shell
"$TESTEE" -e -c 'false; echo not reached'; echo "status=$?"

--- an invalid option letter is refused rather than run
"$TESTEE" -Z -c 'echo hi' 2>/dev/null; echo "status=$?"

--- exec replaces the shell with another shell
"$TESTEE" -c 'exec "$TESTEE" -c "echo inner"; echo not reached'

--- $0 is the pathname the shell was invoked as
d=$(mktemp -d); ln -s "$TESTEE" $d/sh; cd $d && ./sh -c 'echo "$0"'

--- $0 is argv[0] for a shell whose script arrives on standard input
d=$(mktemp -d); ln -s "$TESTEE" $d/sh; cd $d && ./sh -s X <<'EOF'
printf '[%s]\n' "$0" "$@"
EOF

--- a word after -c's command string is an operand, not another option
"$TESTEE" -c 'printf "[%s]\n" "$0" "$@"' -- -x

--- a word after -c's command string is an operand even when it is a valid option letter
"$TESTEE" -c 'printf "[%s]\n" "$0" "$@"' -s foo

--- the command string is the first OPERAND, so an option may sit between -c and it
"$TESTEE" -c -e 'false; echo not reached'; echo "status=$?"

--- a single hyphen before the command string is an operand and then ignored
"$TESTEE" -c - 'echo OK'; "$TESTEE" -c -- 'echo OK'

--- a command_file that cannot be opened exits 127 [divergence: dash reports 2; startup-p.tst's 'reading non-existing file' requires 127 and bash agrees]
d=$(mktemp -d); "$TESTEE" $d/no_such_file 2>/dev/null; echo "status=$?"
=== expect
status=127

# HOW MUCH INPUT THE SHELL CONSUMES (issue #67).
#
# POSIX 1.4, "Input Files": when a shell's input is a SEEKABLE regular file, the
# shell shall not consume more of it than the command it is about to run needs,
# so that a command reading fd 0 gets the bytes that follow. Over-reading a PIPE
# is expressly permitted, because a pipe cannot be un-read.
#
# ADR-0001 CANNOT SETTLE THIS ONE. dash over-reads and so does zsh; bash and yash
# implement the rule. Every case below that dash fails is therefore a divergence
# carrying its own expectation, argued from POSIX and from bash and yash - not a
# gap where dash was consulted and agreed.
#
# The seekable path is reached by re-invoking `$TESTEE` with its standard input
# redirected FROM A FILE, which is what the yash harness does and is the whole
# reason its input-p.tst could see this. A `[stdin]` case here would be a pipe
# and would assert the opposite.

--- a seekable script is consumed a command at a time, so read takes the line after it [divergence: dash and zsh read ahead and lose the line; bash and yash consume only what the command needs (input-p.tst's 'no input more than needed is read')]
d=$(mktemp -d); printf 'read x\nhello\necho "[${x-unset}]"\n' > $d/s; "$TESTEE" < $d/s
=== expect
[hello]

--- a pipeline reading a seekable script's own following lines gets them as data [divergence: dash and zsh swallow the data and then execute it; bash and yash feed it to the pipeline (pipeline-p.tst's 'stdin for first command & stdout for last are not modified')]
d=$(mktemp -d); printf 'cat | tail -n 1\nfoo\nbar\n' > $d/s; "$TESTEE" < $d/s
=== expect
bar

--- a nested shell reading fd 0 consumes exactly its own line of the outer script [divergence: dash and zsh let the nested shell run past its line; bash and yash stop at it (input-p.tst's 'no input more than needed is read')]
d=$(mktemp -d)
cat > $d/s <<'END'
"$TESTEE" -c 'read -r line && printf "%s\n" "$line"'
echo - read by the nested shell and printed by printf
echo - read and executed by the outer shell
END
"$TESTEE" < $d/s
=== expect
echo - read by the nested shell and printed by printf
- read and executed by the outer shell

--- a here-document body is collected from the seekable script and the line after it still runs
d=$(mktemp -d)
cat > $d/s <<'END'
cat <<EOF
body one
body two
EOF
echo after
END
"$TESTEE" < $d/s

--- a command reading fd 0 after a here-document gets the line following the whole construct [divergence: dash and zsh have already read it; bash and yash have not (the here-document body is consumed by the parser, the data line by read)]
d=$(mktemp -d)
cat > $d/s <<'END'
cat <<EOF
in the body
EOF
read x
data line
echo "[${x-unset}]"
END
"$TESTEE" < $d/s
=== expect
in the body
[data line]

--- a compound command spanning several lines of a seekable script leaves the next line unread [divergence: dash and zsh have already consumed the data line; bash and yash have not]
d=$(mktemp -d)
cat > $d/s <<'END'
for i in 1 2
do
	echo "loop $i"
done
read x
data line
echo "[${x-unset}]"
END
"$TESTEE" < $d/s
=== expect
loop 1
loop 2
[data line]

--- an alias defined on one line of a seekable script is in effect on the next
d=$(mktemp -d); printf 'alias f=:\nf\n' > $d/s; "$TESTEE" < $d/s; echo "[$?]"

--- eval re-entering the front end does not disturb the outer script's position [divergence: dash and zsh have already consumed the data line; bash and yash have not]
d=$(mktemp -d)
cat > $d/s <<'END'
eval 'read x
echo "[${x-unset}]"'
data line
echo tail
END
"$TESTEE" < $d/s
=== expect
[data line]
tail

--- a dot script re-entering the front end does not disturb the outer script's position [divergence: dash and zsh have already consumed the data line; bash and yash have not]
d=$(mktemp -d)
printf 'read y\necho "[${y-unset}]"\n' > $d/lib
printf '. %s/lib\ndata line\necho tail\n' "$d" > $d/s
"$TESTEE" < $d/s
=== expect
[data line]
tail

--- an alias defined inside a command substitution is in effect on its next line [divergence: bash, dash and zsh all parse the substituted text as one unit; yash reads it a command at a time, which is what POSIX 2.3.1 requires of ANY shell input (input-p.tst's 'shell input is line-wise (command substitution)')]
x=$(alias f=:
f)
echo "[$?][$x]"
=== expect
[0][]

--- a command substitution that runs nothing reports zero rather than the status before it
false; x=$( ); echo "[$?]"

# The other half of the rule, and the half that must NOT change: a PIPE cannot be
# un-read, so POSIX lets the shell buffer ahead of itself there and dash agrees.
# These are ordinary differential cases for that reason - if the fix for the
# seekable path leaked into the non-seekable one, they would start failing.

--- input from a pipe is not seekable, so the shell may read past the command it runs [stdin]
read x
data line
echo "[${x-unset}]"

--- a pipeline's following lines on a pipe are not data for it [stdin]
cat | tail -n 1
foo
bar
