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
from pathlib import Path

# Signal-related tests hang against a shell without job control or trap. Listed
# by prefix rather than by name so the exclusion survives suite updates.
SIGNAL_PREFIXES = ("kill", "trap", "signal", "job", "fg", "bg", "wait", "suspend")


def is_excluded(name: str) -> bool:
    return any(name.startswith(p) for p in SIGNAL_PREFIXES)


def parse_results(trs: Path) -> tuple[int, int]:
    """Counts (passed, total) from a .trs result file.

    run-test.sh ALWAYS exits 0 and writes its verdicts to this file, so the exit
    status carries no information - reading it reported every case as a failure.
    """
    if not trs.exists():
        return 0, 0
    text = trs.read_text(errors="replace")
    # The markers are literally these two strings. Guessing at "NG[FAILED]"
    # counted zero failures, so every file reported 100% and a file whose
    # assertions ALL failed reported "no assertions ran" - a scoreboard that
    # flattered the shell it was measuring.
    passed = text.count("%%% OK[PASSED]")
    failed = text.count("%%% ERROR[FAILED]")
    return passed, passed + failed


def run_case(runner: Path, shell: str, case: Path, timeout: float) -> tuple[str, str]:
    """Runs one case in its own process group. Returns (outcome, detail)."""
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
        return "error", str(e)

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
        return "timeout", ""

    passed, total = parse_results(case.with_suffix(".trs"))
    if total == 0:
        return "error", "no assertions ran"
    return "counted", f"{passed}/{total}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--suite", default="third_party/yash-tests",
                    help="directory holding run-test.sh and the .tst files")
    ap.add_argument("--shell", default="./build/debug/lesh")
    ap.add_argument("--frontend", choices=["legacy", "next"], default="next")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--include-signal-tests", action="store_true",
                    help="run the signal tests too; they hang against an incomplete shell")
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
    files = {"counted": 0, "timeout": 0, "error": 0, "skipped": 0}

    for case in cases:
        if not args.include_signal_tests and is_excluded(case.name):
            files["skipped"] += 1
            continue
        outcome, detail = run_case(runner, os.path.abspath(args.shell), case, args.timeout)
        files[outcome] += 1
        if outcome == "counted":
            p, t_ = detail.split("/")
            assertions_passed += int(p)
            assertions_total += int(t_)
        if args.verbose or outcome in ("timeout", "error"):
            print(f"  {outcome:8} {case.name} {detail}")

    pct = (100.0 * assertions_passed / assertions_total) if assertions_total else 0.0
    print(f"\nyash POSIX subset, front end '{args.frontend}': "
          f"{assertions_passed}/{assertions_total} assertions pass ({pct:.1f}%) "
          f"across {files['counted']} files; "
          f"{files['timeout']} timed out, {files['error']} errored, "
          f"{files['skipped']} skipped (signal tests)")

    # A SCOREBOARD, not a gate. It reports and always succeeds; the number is
    # what moves. Only a runner malfunction is an error.
    return 0  # always succeeds: a scoreboard reports, it does not gate


if __name__ == "__main__":
    sys.exit(main())
