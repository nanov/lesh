# What deleting the old shell cost, pinned against dash so it cannot drift quietly.
#
# src/legacy/ carried two things the replacement does not: brace expansion, and
# array and map variables. #28 deleted it and decision #86 accepted the loss - "we
# will lose them and then fill gaps when needed" - which puts them in Phase 3, the
# curated zsh layer re-landed on the new core, rather than in a regression report.
#
# THEY ARE NOT KNOWN FAILURES, and that is the finding worth writing down. An
# `[xfail: ...]` says a case fails against the reference shell; dash has neither
# brace expansion nor arrays, so lesh's answer today IS dash's answer, and a marker
# here would XPASS and fail the gate. What the corpus can hold instead is the POSIX
# answer, asserted - which is the same thing from the other side: each case below
# FAILS LOUDLY on the day Phase 3 lands, and is then rewritten as a
# `[divergence: ...]` carrying zsh's answer. That is the staleness detector this
# loss would otherwise not have had.
#
# Worth recording next to it: legacy's brace expansion was BROKEN. `echo {a,b}`
# printed `b` rather than `a b`, `echo pre{1,2}post` printed `pre2post`, and
# `typeset -a a=(x y); echo $a` printed nothing at all - the array went into shell
# state and no expansion could read it back out. What Phase 3 re-lands is the
# feature. What was deleted was not an implementation of it.

--- a brace list is literal text, not an expansion
echo {a,b}

--- a brace list with a prefix and a suffix is literal too
echo pre{1,2}post

--- a brace range is literal text
echo {1..3}

--- a subscript after a parameter is literal text, not an array element
a=x
echo $a[1]

--- braces around a word list are a grouping keyword, not a list
{ echo grouped; }

--- an array assignment is a syntax error, not an array
a=(x y)
