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

--- the SIG prefix is accepted on a signal name [xfail: divergence - dash recognises only the unprefixed spellings POSIX lists and rejects SIGURG outright; bash, ksh, yash and zsh all accept the prefix, and no case in the conformance suite asserts rejection]
trap : SIGURG; kill -s SIGURG $$; echo ok

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

--- trap lists nothing for a signal it cannot trap [xfail: divergence - dash and zsh accept the trap command, list it back, and then never run it; bash lists nothing and lesh follows bash, because a listing that names an action the shell will not take is the same defect as a builtin that succeeds without doing anything. See ADR-0001.]
trap '' URG; "$TESTEE" -c 'trap "echo x" URG; trap; echo end'
