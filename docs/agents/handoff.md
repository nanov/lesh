# Handoff briefs

Every ticket item on a wayfinder map is implemented by a subagent with fresh
context, driven by a written brief. The brief is the contract: if it is wrong or
incomplete, the implementation will be too, so it carries the measurements and the
traps rather than a summary of them.

## What a brief must contain

1. **The item**, and the ticket it belongs to.
2. **What is already true** — measured numbers, not impressions. A stale number in a
   brief is how a session spends an hour on the wrong thing; that has happened on
   this project (issue #33 was chased from a `1/42` that was really `28/42`).
3. **The seams to touch**, by file and function.
4. **How to verify**, with the exact commands.
5. **The traps**, specific to this repo. See below.
6. **What is out of scope** for the item, so the ledger stays attributable to
   tickets.

## Traps every brief repeats

- **Never run the conformance sweep alongside a build or the differential corpus.**
  The per-file timeout is real time. One overlapping run reported 37 timeouts,
  including `umask-p.tst`, which takes under a second on an idle machine.
- **A loaded machine does not merely time files out - it makes the signal tests report
  WRONG ANSWERS.** One after-sweep read 4763 where the same binary reads 5061, and
  looked exactly like a 162-assertion regression. Nothing timed out. `sigint1-p.tst`
  scored 65/180 in it and 180/180 alone minutes later. The suite self-signals, and its
  own `macos_kill_workaround` calls the delivery timing "more likely" rather than
  guaranteed, so a busy scheduler changes the ANSWER instead of the runtime. **A sweep
  showing a large unexplained drop is a property of the measurement until it has been
  re-run idle.**
- **Build with `-j3`, not `-j8`.** `-O3 -flto=thin` across three presets pegs the
  machine, and the machine belongs to someone.
- **Check for orphans before and after**:
  `ps -Ao pid=,command= | grep "yash-tests/tmp\."` and `ls -d third_party/yash-tests/tmp.*`
- **Distrust the scoreboard.** `tools/conformance.py` has had five bugs: an exit
  status that is always zero, a guessed failure marker, an exclusion that removed
  mandatory POSIX, "errored" for files the suite itself skipped, and a ten-second
  per-file timeout that made the score move with machine load.
- **When a proven fix moves nothing, look at what runs BEFORE the thing under
  test.** Three times on map #17 a correct fix scored nothing because something
  earlier aborted the file.
- **Set `LESH_FRONTEND=next` on every invocation.** The DEFAULT is still `legacy`, the
  strangler quarantine ADR-0002 deletes, and it is missing most of the language. A bare
  `./build/debug/lesh -c 'set -m'` reports `set: No such file or directory` — not a bug,
  just the wrong shell. This has already produced one false finding in a handoff report.
  `tools/spec_run.py` and `tools/conformance.py` set it for you; a hand-written probe
  does not.
- **Verify against dash before believing anything.** dash is authoritative for the
  POSIX floor (ADR-0001); zsh only for the curated layer. Where dash is behind the
  current standard, say so in writing and record the divergence rather than choosing
  quietly.
