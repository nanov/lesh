#!/usr/bin/env python3
"""Runs an external POSIX conformance suite against lesh, safely.

The suite itself is vendored under third_party/ and is GPL; this runner is not
derived from it and only invokes it.

SAFETY IS THE POINT OF THIS FILE. Of every shell test runner surveyed for
issue #4, only FreeBSD's kyua reaps process groups correctly; yash's harness has
no timeout anywhere. A single hung case from this exact corpus previously spun a
CPU core for nineteen minutes and took this machine to a load average of 29, so
every case here runs in its own process group and is killed by group.

63 of the 122 POSIX files are signal tests that hang against an incomplete
shell. They are excluded by default rather than left to time out one by one.
"""

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

# Signal-related tests hang against a shell without job control or trap. Listed
# by prefix rather than by name so the exclusion survives suite updates.
# Only JOB CONTROL is out of scope, per ADR-0001: POSIX puts it in the User
# Portability option rather than the mandatory core.
#
# `trap`, `wait` and `kill` are NOT job control - trap and wait are special
# builtins and kill is a required utility, all mandatory. Nor is how a
# non-interactive shell responds to SIGINT, SIGHUP, SIGTERM or SIGQUIT.
#
# An earlier version excluded every file starting with "sig" plus trap, wait and
# kill, which quietly removed mandatory POSIX from the denominator and inflated
# the score. Excluding work you have not done is not the same as it being out of
# scope.
#
# SIGTSTP, SIGTTIN, SIGTTOU, SIGSTOP and SIGCONT are the terminal stop signals
# and exist only to serve job control, so they go with it.
JOB_CONTROL_PREFIXES = (
    "job", "fg", "bg", "suspend",
    "sigtstp", "sigttin", "sigttou", "sigstop", "sigcont",
)


def is_excluded(name: str) -> bool:
    return any(name.startswith(p) for p in JOB_CONTROL_PREFIXES)


def parse_results(trs: Path) -> tuple[int, int, int]:
    """Counts (passed, total, skipped) from a .trs result file.

    run-test.sh ALWAYS exits 0 and writes its verdicts to this file, so the exit
    status carries no information - reading it reported every case as a failure.
    """
    if not trs.exists():
        return 0, 0, 0
    text = trs.read_text(errors="replace")
    # The markers are literally these three strings. Guessing at "NG[FAILED]"
    # counted zero failures, so every file reported 100% and a file whose
    # assertions ALL failed reported "no assertions ran" - a scoreboard that
    # flattered the shell it was measuring.
    #
    # SKIPPED is counted because the suite skips a case itself when a prerequisite
    # is missing - `checkfg`, a helper binary the suite builds and we do not vendor,
    # gates every -m file. Twenty-four files were reported as ERRORED for that,
    # which reads as "lesh broke" when it means "not measured here".
    passed = text.count("%%% OK[PASSED]")
    failed = text.count("%%% ERROR[FAILED]")
    skipped = text.count("%%% SKIPPED:")
    return passed, passed + failed, skipped


def run_case(runner: Path, shell: str, case: Path,
             timeout: float) -> tuple[str, str, float]:
    """Runs one case in its own process group. Returns (outcome, detail, seconds)."""
    try:
        proc = subprocess.Popen(
            # runner.name, not str(runner): cwd is already the suite directory, so a
            # relative path would be resolved twice against it.
            ["sh", runner.name, shell, case.name],
            cwd=case.parent,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            text=True,
            start_new_session=True,  # own group, so killpg reaches grandchildren
        )
    except OSError as e:
        return "error", str(e), 0.0

    started = time.monotonic()
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        # Kill the GROUP. Killing the child alone is what orphans a hung
        # grandchild to init, which is the failure this whole file exists for.
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            proc.kill()
        proc.communicate()
        return "timeout", "", time.monotonic() - started

    elapsed = time.monotonic() - started
    passed, total, skipped = parse_results(case.with_suffix(".trs"))
    if total == 0:
        # The suite declined to run its own cases - a missing prerequisite, not a
        # failure of the shell. Distinguishing this from a runner malfunction is
        # the difference between "not measured" and "broken".
        if skipped > 0:
            return "suite-skipped", f"{skipped} cases skipped by the suite", elapsed
        return "error", "no assertions ran", elapsed
    return "counted", f"{passed}/{total}", elapsed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--suite", default="third_party/yash-tests",
                    help="directory holding run-test.sh and the .tst files")
    ap.add_argument("--shell", default="./build/debug/lesh")
    ap.add_argument("--frontend", choices=["legacy", "next"], default="next")
    # 10 seconds was the original default, and it was WRONG: a signal file holds 180
    # cases, each spawning several processes, and takes a few seconds on an idle
    # machine. Anything else running pushed it past the line, so six files dropped out
    # of the denominator and the score moved with the machine's load rather than with
    # the shell. A timeout that decides the score is a scoreboard bug, not a setting.
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--include-signal-tests", action="store_true",
                    help="run the job-control tests too; they hang without terminal ownership")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    suite = Path(args.suite)
    runner = suite / "run-test.sh"
    if not runner.exists():
        print(f"conformance suite not vendored at {suite}", file=sys.stderr)
        print("this is a scoreboard, not a gate: skipping", file=sys.stderr)
        return 0  # absence is not a failure

    os.environ["LESH_FRONTEND"] = args.frontend
    # Leak detection off: this measures CONFORMANCE, and leaks are a separate
    # gate with its own expected result.
    opts = [o for o in os.environ.get("ASAN_OPTIONS", "").split(":")
            if o and not o.startswith("detect_leaks")]
    os.environ["ASAN_OPTIONS"] = ":".join(opts + ["detect_leaks=0"])

    cases = sorted(p for p in suite.glob("*.tst"))
    # Stale result files from a previous run would be counted as this run's.
    for stale in suite.glob("*.trs"):
        stale.unlink()

    assertions_passed = 0
    assertions_total = 0
    files = {"counted": 0, "timeout": 0, "error": 0, "skipped": 0,
             "suite-skipped": 0}
    slow: list[tuple[str, float]] = []

    for case in cases:
        if not args.include_signal_tests and is_excluded(case.name):
            files["skipped"] += 1
            continue
        outcome, detail, elapsed = run_case(runner, os.path.abspath(args.shell),
                                            case, args.timeout)
        files[outcome] += 1
        # Report anything that got close to the limit, so the next timeout is seen
        # coming instead of silently shrinking the denominator.
        if outcome != "timeout" and elapsed > args.timeout / 4:
            slow.append((case.name, elapsed))
        if outcome == "counted":
            p, t_ = detail.split("/")
            assertions_passed += int(p)
            assertions_total += int(t_)
        if args.verbose or outcome in ("timeout", "error", "suite-skipped"):
            print(f"  {outcome:8} {case.name} {detail}")

    for name, elapsed in sorted(slow, key=lambda p: -p[1]):
        print(f"  slow     {name} took {elapsed:.1f}s of a {args.timeout:.0f}s limit")

    pct = (100.0 * assertions_passed / assertions_total) if assertions_total else 0.0
    print(f"\nyash POSIX subset, front end '{args.frontend}': "
          f"{assertions_passed}/{assertions_total} assertions pass ({pct:.1f}%) "
          f"across {files['counted']} files; "
          f"{files['timeout']} timed out, {files['error']} errored, "
          f"{files['suite-skipped']} skipped by the suite, "
          f"{files['skipped']} excluded (job control)")

    # A SCOREBOARD, not a gate. It reports and always succeeds; the number is
    # what moves. Only a runner malfunction is an error.
    return 0  # always succeeds: a scoreboard reports, it does not gate


if __name__ == "__main__":
    sys.exit(main())
