# yash test suite (vendored POSIX subset)

Upstream: https://github.com/magicant/yash — `tests/`
Licence: **GPL-2.0-or-later**, © magicant.

Segregated under `third_party/` because lesh's own sources are not GPL. Nothing
here is modified; `tools/conformance.py` invokes it and is not derived from it.

## What is vendored

Only the `*-p.tst` files — yash marks its POSIX-required assertions with a `-p`
suffix and keeps its own extensions separately. Those extensions are not a
conformance signal for lesh.

## Why lesh supplies its own runner

`run-test.sh` has **no timeout anywhere**, and issue #4 established that of every
shell test runner surveyed only FreeBSD's kyua reaps process groups correctly. A
hung case from this exact corpus previously spun a CPU core for nineteen minutes
and took a development machine to a load average of 29.

`tools/conformance.py` therefore runs each case in its own process group and
kills by group on timeout. It also excludes the signal tests by default, which
hang against a shell without job control rather than failing.

## Status

A **scoreboard, not a gate.** It reports a number and always succeeds; the number
is what moves.
