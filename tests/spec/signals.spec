# Signal NAMES and DEFAULT ACTIONS, against dash (the POSIX floor).
#
# Issue #38. The name table was a hand-typed array of twenty entries, and a
# signal missing from it was not merely unnamed but unreachable:
#
#   lesh -c 'kill -s URG $$; echo survived'   -> lesh: kill: bad signal
#   dash -c 'kill -s URG $$; echo survived'   -> survived
#
# That cost 336 conformance assertions in sigurg1/2-p.tst, because `trap` is a
# special builtin and its error exits a non-interactive shell - so one unknown
# name aborted every case in the file before the case's own subject ran.
#
# WHAT IS ASSERTED AND WHAT IS NOT. `kill -l` now prints what the PLATFORM has,
# not a list compiled into lesh, so its full output is deliberately NOT compared
# against dash's: dash carries a fixed 32-entry table, which on glibc omits the
# real-time signals the platform really does have. Comparing the whole list would
# assert dash's table rather than the platform's. What is compared is the shape
# (`0` first, one name per line) and the properties that matter - every name the
# list contains is a name `trap` accepts, and every name resolves to the number
# whose default action the kernel then takes.
#
# ASan intercepts SIGBUS, SIGSEGV, SIGILL and SIGFPE and aborts on them, so a
# child killed by those reports ABRT under the debug build and the real signal
# under release. No case here uses one; the fatal class is asserted with signals
# no sanitizer touches.
#
# The `2>/dev/null` on the cases that kill a child is not hiding a signal-table
# result: dash writes a "Terminated"-style notification for a signal-killed
# child and lesh writes nothing, which is a separate divergence and not this
# ticket's.

--- SIGURG is reachable at all, which is the whole of issue #38 [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill, which cannot take $$ from a shell that does not expand it]
kill -s URG $$; echo survived

--- an unknown signal name is still refused, because the suite depends on refusal [xfail(legacy): legacy never expands a parameter inside double quotes, so "$TESTEE" is not a command it can run]
"$TESTEE" -c 'trap : NOSUCHSIGNAL' 2>/dev/null; [ $? -ne 0 ] && echo refused

--- trap takes SIGURG in each of its three forms [xfail(legacy): legacy has no trap builtin and runs /bin/trap, which does not exist]
trap '' URG; trap - URG; trap : URG; echo $?

--- the SIG prefix is accepted on a signal name [divergence: dash recognises only the unprefixed spellings POSIX lists and rejects SIGURG outright; bash, ksh, yash and zsh all accept the prefix, and no case in the conformance suite asserts rejection]
trap : SIGURG; kill -s SIGURG $$; echo ok
=== expect
ok

--- a trap on SIGURG runs between commands, not during one [xfail(legacy): legacy has neither trap nor a kill builtin]
trap 'echo trapped' URG; kill -s URG $$; echo after

--- SIGURG's default action is to discard it, so the shell survives [xfail(legacy): legacy never expands a parameter inside double quotes, so "$TESTEE" is not a command it can run]
"$TESTEE" -c 'kill -s URG $$; echo alive'; echo $?

--- the spare signals are all discarded rather than fatal [xfail(legacy): same quote-expansion defect]
for s in URG CHLD CONT WINCH; do "$TESTEE" -c "kill -s $s \$\$" && echo "$s spared"; done

--- a fatal signal's default action still kills, and the status names it back [xfail(legacy): same quote-expansion defect]
for s in TERM HUP USR1 USR2 SYS XCPU; do "$TESTEE" -c "kill -s $s \$\$" 2>/dev/null; kill -l $?; done

--- kill -l lists the null signal first and one name per line [xfail(legacy): legacy has no kill builtin, so `kill -l` runs /bin/kill and prints its own format]
kill -l | sed -n 1p; kill -l | grep -x URG; kill -l | grep -c '^[A-Z]'

--- every name kill -l lists is a name trap accepts [xfail(legacy): legacy has neither trap nor a kill builtin]
kill -l | tail -n +2 | while read s; do trap : "$s" 2>/dev/null || echo "trap rejected $s"; done; echo checked

--- kill -l NUMBER names a signal number [xfail(legacy): legacy has no kill builtin, so `kill -l 1` runs /bin/kill]
kill -l 1; kill -l 9; kill -l 15

--- kill -l EXITSTATUS takes the 128 back off, which is what run-test.sh needs [xfail(legacy): legacy has no kill builtin, so `kill -l 129` runs /bin/kill]
kill -l 129; kill -l 137; kill -l 143

--- kill -l refuses an operand that names no signal, rather than inventing one [xfail(legacy): legacy has no kill builtin, so `kill -l 0` runs /bin/kill]
for n in 0 128 99999; do kill -l $n 2>/dev/null; [ $? -ne 0 ] && echo "refused $n"; done

--- the terminal-stop signals are named even though job control is not implemented [xfail(legacy): legacy has neither trap nor a kill builtin]
trap : STOP TSTP TTIN TTOU; echo $?

--- SIGSTOP's default action stops the shell and a subshell resumes it [xfail(legacy): legacy has no kill builtin, so `kill -s STOP $$` runs /bin/kill]
(kill -s STOP $$; s=$?; kill -s CONT $$; exit $s); echo st=$?

# ISSUE #37. POSIX XCU 2.11: a signal ignored on entry to a NON-INTERACTIVE shell
# cannot be trapped or reset. Every case below needs a shell whose SIGINT or
# SIGURG is ALREADY ignored, which no `sh -c` snippet can create for itself - so
# the case ignores the signal and then re-invokes `$TESTEE`, and the comparison
# stays honest because dash does the same to dash. Without $TESTEE (#41) this rule
# cannot be expressed differentially at all.
#
# The interactive half of the rule is deliberately NOT here. dash applies the rule
# to `sh -i` as well and POSIX does not, so a differential case would only assert
# dash's bug; the exclusion is covered by sigurg6-p.tst, which runs the testee
# interactively with SIGURG ignored and requires the trap to fire.

--- a signal ignored on entry cannot be RESET, which is the half that killed the shell [xfail(legacy): legacy never expands a parameter inside double quotes, so "$TESTEE" is not a command it can run]
trap '' INT; "$TESTEE" -c 'trap - INT; kill -s INT $$; echo survived-reset'; echo "status=$?"

--- a signal ignored on entry cannot be TRAPPED, so the handler never runs [xfail(legacy): same quote-expansion defect, and legacy has no trap builtin]
trap '' INT; "$TESTEE" -c 'trap "echo trapped" INT; kill -s INT $$; echo after'

--- trap on such a signal still reports success, because POSIX asks for no error [xfail(legacy): same quote-expansion defect]
trap '' URG; "$TESTEE" -c 'trap "echo x" URG; echo $?; trap - URG; echo $?'

--- the rule survives a subshell, where an ordinary handler would not [xfail(legacy): same quote-expansion defect]
trap '' URG; "$TESTEE" -c '( trap "echo trapped" URG; kill -s URG $$; echo after )'

--- a child shell inherits SIG_IGN and so discovers the rule for itself [xfail(legacy): same quote-expansion defect]
trap '' URG; "$TESTEE" -c '"$TESTEE" -c "trap \"echo trapped\" URG; kill -s URG \$\$; echo after"'

--- exec carries it, because SIG_IGN survives execve [xfail(legacy): same quote-expansion defect]
trap '' INT; "$TESTEE" -c 'exec "$TESTEE" -c "trap - INT; kill -s INT \$\$; echo survived-exec"'

--- the rule is per signal: another signal in the same shell traps normally [xfail(legacy): same quote-expansion defect]
trap '' URG; "$TESTEE" -c 'trap "echo trapped" USR1; kill -s USR1 $$; echo after'

--- an EXIT trap is unaffected, since EXIT has no inherited disposition [xfail(legacy): same quote-expansion defect]
trap '' INT; "$TESTEE" -c 'trap "echo bye" EXIT; echo hi'

--- trap lists nothing for a signal it cannot trap [divergence: dash and zsh accept the trap command, list it back, and then never run it; bash lists nothing and lesh follows bash, because a listing that names an action the shell will not take is the same defect as a builtin that succeeds without doing anything. See ADR-0001.]
trap '' URG; "$TESTEE" -c 'trap "echo x" URG; trap; echo end'
=== expect
end

# ISSUE #52. POSIX XCU 2.11's third rule: an INTERACTIVE shell ignores SIGQUIT and
# SIGTERM and catches SIGINT, so a keyboard interrupt or a stray `kill` abandons the
# command being run rather than ending the session.
#
# Every case re-invokes `"$TESTEE" -i +m`, because the rule is about how the shell
# was INVOKED and no snippet can arrange that for itself - and the comparison stays
# honest because dash does the same to dash (#41).
#
# THE REFERENCE SHELL IS SPLIT ON THIS ONE, so each case says which half it is on.
# dash ignores SIGQUIT and SIGTERM interactively and gets those right, but dies on
# SIGINT: with input that is not a terminal its top-level unwind exits instead of
# reading the next command, which is 5 assertions per file in sigint5/6-p.tst that
# dash fails and lesh does not. bash gets SIGINT right - it abandons the command and
# carries on - and is the shell lesh follows there. The conformance suite agrees with
# bash, and neither shell gets the subshell half right (both keep sparing where POSIX
# takes the default action back), so ADR-0001's "dash is authoritative where they
# disagree about POSIX" is settled here by the suite rather than by either shell.

--- an interactive shell ignores SIGQUIT at its top level [xfail(legacy): legacy never expands a parameter inside double quotes, so "$TESTEE" is not a command it can run]
"$TESTEE" -i +m -c 'trap - QUIT
kill -s QUIT $$
echo spared'; echo "status=$?"

--- an interactive shell ignores SIGTERM at its top level [xfail(legacy): same quote-expansion defect]
"$TESTEE" -i +m -c 'trap - TERM
kill -s TERM $$
echo spared'; echo "status=$?"

--- SIGHUP still terminates an interactive shell, so the rule did not spread [xfail(legacy): same quote-expansion defect]
"$TESTEE" -i +m -c 'trap - HUP
kill -s HUP $$
echo not printed' 2>/dev/null; kill -l $?

--- a NON-interactive shell is untouched: the same three signals still kill it [xfail(legacy): same quote-expansion defect]
for s in INT QUIT TERM; do "$TESTEE" +i +m -c "kill -s $s \$\$" 2>/dev/null; kill -l $?; done

--- an explicit trap wins over the interactive default [xfail(legacy): same quote-expansion defect, and legacy has no trap builtin]
"$TESTEE" -i +m -c 'trap "echo trapped" TERM
kill -s TERM $$
echo after'

--- trap '' on such a signal is a real ignore the shell can report [xfail(legacy): same quote-expansion defect, and legacy has no trap builtin]
"$TESTEE" -i +m -c "trap '' TERM
trap
kill -s TERM \$\$
echo after"

--- trap reports the interactive default as the default action, because that is what was asked for [xfail(legacy): same quote-expansion defect, and legacy has no trap builtin]
"$TESTEE" -i +m -c 'trap - TERM
trap
echo end'

--- a subshell of an interactive shell takes the default action back [divergence: dash and bash both keep sparing the subshell, and the conformance suite says both are wrong: sigterm5-p.tst requires `( "$TESTEE" -c "kill -s TERM \$PPID" )` under `sh -i +m` to be killed, because a subshell is not the process that reads commands and has a prompt to return to]
"$TESTEE" -i +m -c 'trap - TERM
( "$TESTEE" -c '\''kill -s TERM $PPID'\''
echo not printed )' 2>/dev/null; kill -l $?
=== expect
TERM

--- a command the interactive shell runs is not handed an unkillable SIGTERM [divergence: dash hands its interactive SIG_IGN straight to the child and the child then survives a SIGTERM aimed at itself; bash kills it, lesh follows bash, and sigterm5-p.tst's `target=child` cases require it. dash is inconsistent with its own `exec`, which DOES drop the ignore - the case below passes against it]
"$TESTEE" -i +m -c 'trap - TERM
"$TESTEE" -c '\''kill -s TERM $$
echo not printed'\''' 2>/dev/null; kill -l $?
=== expect
TERM

--- exec hands over a killable SIGTERM, since SIG_IGN would survive execve [xfail(legacy): same quote-expansion defect]
"$TESTEE" -i +m -c 'trap - TERM
exec "$TESTEE" -c '\''kill -s TERM $$
echo not printed'\''' 2>/dev/null; kill -l $?

--- an interactive shell catches SIGINT and carries on to the next command [divergence: dash dies with status 130 here: its top-level unwind exits when the input is not a terminal instead of reading on. bash prints `spared` and lesh follows bash, which is also what sigint5-p.tst requires of the 5 assertions dash fails in it]
"$TESTEE" -i +m -c 'trap - INT
kill -s INT $$
echo spared'; echo "status=$?"
=== expect
spared
status=0

--- catching SIGINT means ABANDONING the command, not merely surviving it [divergence: dash dies, as above. bash abandons the loop and prints the command after it, and a shell that only stayed alive would run every iteration - which is the stub this case exists to fail]
"$TESTEE" -i +m -c 'trap - INT
n=0
while [ "$n" -lt 5 ]; do n=$((n+1)); kill -s INT $$; done
echo abandoned after $n'
=== expect
abandoned after 1

# `exit` inside a trap action. The whole body runs even when the shell is already
# on its way out, an explicit status REPLACES the one the shell was leaving with,
# and with no operand the status is the one the trap action was entered with -
# POSIX XCU `exit`: "the last command is considered to be the command that
# executed immediately preceding the trap action".

--- the EXIT trap runs its whole body after an exit [xfail(legacy): legacy has no trap builtin]
trap 'echo A; echo B' EXIT; exit 1

--- an exit in the EXIT trap replaces the shell's status [xfail(legacy): legacy has no trap builtin]
trap 'exit 7' EXIT; exit 1

--- an exit with status zero in the EXIT trap wins too [xfail(legacy): legacy has no trap builtin]
trap 'exit 0' EXIT; exit 1

--- an EXIT trap that only runs commands leaves the status alone [xfail(legacy): legacy has no trap builtin]
trap '(exit 2)' EXIT; (exit 1); exit

--- exit with no operand in the EXIT trap reports the status the trap was entered with [xfail(legacy): legacy has no trap builtin]
trap '(exit 1); exit' EXIT; (exit 2); exit

--- an exit in a signal trap replaces the shell's status [xfail(legacy): legacy has neither trap nor a kill builtin]
trap '(exit 2); exit 3' INT; (exit 1); kill -INT $$

--- exit with no operand in a signal trap reports the status the trap interrupted [xfail(legacy): legacy has neither trap nor a kill builtin]
trap '(exit 2); exit' INT; (exit 1); kill -INT $$

--- a signal trap that exits stops the commands after it [xfail(legacy): legacy has neither trap nor a kill builtin]
trap 'echo A; exit 4; echo B' USR1; kill -s USR1 $$; echo after

--- an exit in a subshell's EXIT trap is the subshell's status [xfail(legacy): legacy has no trap builtin]
( trap 'exit 7' EXIT; exit 1 ); echo "sub=$?"

--- return with no operand in a trap reports the status the trap interrupted [divergence: ADR-0001. POSIX makes "the last command" the one before the trap action for `return` as much as for `exit`, which is what return-p.tst's 'default exit status in function in trap' requires (19, not 0). dash applies the rule to `exit` only; bash and zsh to neither]
fn() { true; return; }; trap 'fn; echo trapped $?' EXIT; (exit 19); exit
=== expect [status: 19]
trapped 19

# ISSUE #53. A PIPELINE STAGE IS A SUBSHELL ENVIRONMENT, so POSIX 2.9.2 resets its
# traps to default and keeps only the ignores - the same asymmetry `( ... )`, `&`
# and `$( ... )` already obeyed and the pipeline fork did not.
#
# The `2>/dev/null` on the first case is not hiding a trap result: dash writes a
# "User defined signal 1"-style notification for a signal-killed child and lesh
# writes nothing, which is a separate divergence and not this ticket's.
#
# Each case that must see the STAGE take the signal re-invokes "$TESTEE" and sends
# to $PPID, because `$$` inside a subshell is the PARENT's pid - `kill -s SIG $$`
# from a stage signals the shell, and the shell then runs the trap perfectly
# correctly, which is how this bug survived twenty signal files.

--- a pipeline stage does not run the trap handler it inherited [xfail(legacy): legacy never expands a parameter inside double quotes, so "$TESTEE" is not a command it can run]
"$TESTEE" -c 'trap "echo TRAP" USR1; { "$TESTEE" -c "kill -s USR1 \$PPID"; echo body; } | cat; echo done' 2>/dev/null

--- but an IGNORE does carry into a pipeline stage, which is the asymmetry [xfail(legacy): same quote-expansion defect]
trap '' USR1; { "$TESTEE" -c 'kill -s USR1 $PPID'; echo body; } | cat

--- a trap set INSIDE a stage still fires there [xfail(legacy): same quote-expansion defect]
{ trap 'echo INSIDE' USR1; "$TESTEE" -c 'kill -s USR1 $PPID'; echo body; } | cat

--- a stage of an ASYNC pipeline keeps the SIGINT ignore XCU 2.11 gave it [xfail(legacy): same quote-expansion defect]
{ "$TESTEE" -c 'kill -s INT $PPID'; echo body; } | cat & wait

--- a pipeline stage runs its OWN EXIT trap [xfail(legacy): legacy has no trap builtin]
{ trap 'echo S' EXIT; echo body; } | cat

--- and runs it when the stage exits early [xfail(legacy): legacy has no trap builtin]
{ trap 'echo S' EXIT; exit 3; } | cat; echo "st=$?"

--- the LAST stage runs its own EXIT trap too, and the shell still runs the one it kept [xfail(legacy): legacy has no trap builtin]
trap 'echo T' EXIT; echo a | { trap 'echo S' EXIT; cat; }

--- a stage does NOT run the EXIT trap it inherited [xfail(legacy): legacy has no trap builtin]
trap 'echo T' EXIT; { echo body; } | cat; echo after

--- both an inherited EXIT trap and the stage's own, each in its own process [xfail(legacy): legacy has no trap builtin]
trap 'echo T' EXIT; { trap 'echo S' EXIT; echo body; } | cat

--- $$ in a pipeline stage is still the shell's pid [xfail(legacy): legacy has no compound commands, so the brace group is not a stage it can run]
p=$$; { [ "$$" = "$p" ] && echo same; } | cat

--- $! in a pipeline stage is the one the shell recorded [xfail(legacy): legacy has no compound commands, so the brace group is not a stage it can run]
sleep 0 & b=$!; { [ "$!" = "$b" ] && echo inherited; } | cat; wait

--- trap still REPORTS what a stage inherited, though the action is gone [divergence: the #33 divergence reaching a pipeline stage - dash reports nothing in any subshell, which leaves `saved=$(trap)` with nothing to save, and POSIX.1-2024 requires the listing. bash reports the inherited traps, and lesh follows bash]
trap 'echo TRAP' USR1; { trap; } | cat
=== expect
trap -- 'echo TRAP' USR1

# ISSUE #45. WHAT `kill` VALIDATES. Every form below reported success or signalled
# something other than what it was given, because the pid operand went through
# `atoi` - which answers 0 for `notanumber`, for `--` and for `%1`, and
# `kill(0, sig)` signals THE WHOLE PROCESS GROUP. `kill -s TERM notanumber`
# therefore killed the shell it was typed into. dash answers 2 for a line it
# refused to run and 1 only for one the system refused, and lesh now agrees.

--- kill with a signal and no pid is a usage error, not a silent success [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill, which answers its own way]
kill -s TERM; echo "st=$?"

--- kill with nothing at all is a usage error [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill; echo "st=$?"

--- kill -s with no signal name names no signal [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s; echo "st=$?"

--- a pid operand that is not a number is refused, rather than becoming the process group [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s TERM notanumber; echo "st=$?"

--- and so is one that only starts as a number [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s TERM 12abc; echo "st=$?"

--- an empty pid operand is refused too [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s TERM ''; echo "st=$?"

--- a job specification is refused rather than taken for the process group [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill %1; echo "st=$?"

--- a bare negative operand is an option, and an unknown one [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill, which reads it as a process group]
kill -s 0 -1; echo "st=$?"

--- `--` ends kill's options, which is how a POSIX process group is written [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s 0 -- $$; echo "st=$?"

--- `--` with no operand after it is a usage error, not a signal to the whole group [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s TERM --; echo "st=$?"

--- an unknown option is refused by name [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -x 1; echo "st=$?"

--- an unknown signal name is a usage error [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s NOSUCH $$; echo "st=$?"

--- kill -l refuses an operand that names no signal with the same status [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -l 0; echo "st=$?"

--- the forms that ARE valid still work, and the null signal still asks nothing [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s 0 $$; echo "s=$?"; kill -0 $$; echo "n=$?"; kill -s URG $$; echo "u=$?"

--- EXIT is a trap condition and not a signal kill can send [xfail(legacy): legacy has no kill builtin, so `kill` runs /bin/kill]
kill -s EXIT $$; echo "st=$?"
