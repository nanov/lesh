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
# EVERY case here is xfail on the legacy front end, and for one reason each time:
# legacy never expands a parameter inside double quotes, so `"$TESTEE"` is not a
# command it can run. That is the same defect basics.spec records; it is not new
# here, and none of these cases will pass on legacy before it is fixed.
#
# Files go under `mktemp -d`, as everywhere else in this corpus: cases run in
# whatever directory ctest was started from.

--- +i and +m are accepted, and the script still arrives on standard input [xfail(legacy): legacy never expands a parameter inside double quotes, so "$TESTEE" is not a command it can run]
d=$(mktemp -d); printf 'echo ok\n' > $d/s; "$TESTEE" +i +m < $d/s

--- +i and +m are accepted alongside -c [xfail(legacy): same quote-expansion defect]
"$TESTEE" +i +m -c 'echo ok'

--- an option and its negation on the same command line [xfail(legacy): same quote-expansion defect]
"$TESTEE" -f +f -c 'echo ok'

--- read reads fd 0 when the script itself arrives on standard input [stdin] [xfail(legacy): legacy has neither here-documents nor expansion inside double quotes]
read a <<\END
A
END
echo "$? [${a-unset}]"

--- read reads fd 0 in a shell whose script arrives on standard input [xfail(legacy): same quote-expansion defect]
d=$(mktemp -d); printf 'read a <<\\END\nA\nEND\necho "$? [${a-unset}]"\n' > $d/s; "$TESTEE" < $d/s

--- a script on standard input is read ahead of itself, so read sees EOF [stdin] [xfail(legacy): legacy never expands a parameter inside double quotes]
printf 'first\n'
read x
echo "[${x-unset}]"

--- the shell with no arguments at all reads its script from standard input [stdin] [xfail(legacy): legacy has no positional parameters and never expands inside double quotes]
echo ok
echo "[$#]"

--- the exit status of a script that arrived on standard input [stdin] [xfail(legacy): legacy matches only the bare word exit, so `exit 17` is looked up as a command]
exit 17

--- with -c the operand after the command string is $0 and the next is $1 [xfail(legacy): same quote-expansion defect]
"$TESTEE" -c 'printf "[%s]\n" "$0" "$@"' 'command  name' 1 '2  2'

--- with -c and no operands there are no positional parameters [xfail(legacy): same quote-expansion defect]
"$TESTEE" -c 'echo "[$#]"'

--- -s reads standard input even though operands were given [xfail(legacy): same quote-expansion defect]
"$TESTEE" -s X 'Y  Y' <<'EOF'
printf '[%s]\n' "$@"
EOF

--- -c does not consume standard input [xfail(legacy): same quote-expansion defect]
d=$(mktemp -d); printf 'printed text\n' > $d/in; "$TESTEE" -c cat < $d/in

--- a script's pathname is its own $0 [xfail(legacy): same quote-expansion defect]
d=$(mktemp -d); printf 'echo "$0"\n' > $d/s; cd $d && "$TESTEE" ./s

--- -cn takes the command string from the next word and still honours noexec [xfail(legacy): same quote-expansion defect]
d=$(mktemp -d); cd $d && "$TESTEE" -cn 'echo hi > f'; echo "status=$? [$(ls $d)]"

--- -e on the command line makes a failing command exit the shell [xfail(legacy): same quote-expansion defect]
"$TESTEE" -e -c 'false; echo not reached'; echo "status=$?"

--- an invalid option letter is refused rather than run [xfail(legacy): same quote-expansion defect]
"$TESTEE" -Z -c 'echo hi' 2>/dev/null; echo "status=$?"

--- exec replaces the shell with another shell [xfail(legacy): same quote-expansion defect]
"$TESTEE" -c 'exec "$TESTEE" -c "echo inner"; echo not reached'

--- $0 is the pathname the shell was invoked as [xfail: #43 - lesh reports the literal string "lesh" for every invocation that names no command_file]
d=$(mktemp -d); ln -s "$TESTEE" $d/sh; cd $d && ./sh -c 'echo "$0"'

--- a word after -c's command string is an operand, not another option [xfail: #44 - lesh keeps parsing options past the command string, so the word after the command string is read as options instead of as $0]
"$TESTEE" -c 'printf "[%s]\n" "$0" "$@"' -- -x

--- a command_file that cannot be opened exits 127 [xfail: divergence - dash reports 2; startup-p.tst's 'reading non-existing file' requires 127 and bash agrees]
d=$(mktemp -d); "$TESTEE" $d/no_such_file 2>/dev/null; echo "status=$?"
