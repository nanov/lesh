#!/usr/bin/env python3
"""Runs a single yash conformance case and dumps its .trs, safely.

Same process-group reaping as tools/conformance.py: a hung case previously spun
a core for nineteen minutes, so the child gets its own session and is killed by
group on timeout.
"""
import argparse, os, signal, subprocess, sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("case")
ap.add_argument("--suite", default="third_party/yash-tests")
ap.add_argument("--shell", default="./build/debug/lesh")
ap.add_argument("--timeout", type=float, default=20.0)
a = ap.parse_args()

suite = Path(a.suite)
case = suite / a.case
opts = [o for o in os.environ.get("ASAN_OPTIONS", "").split(":")
        if o and not o.startswith("detect_leaks")]
os.environ["ASAN_OPTIONS"] = ":".join(opts + ["detect_leaks=0"])

trs = case.with_suffix(".trs")
if trs.exists():
    trs.unlink()

proc = subprocess.Popen(["sh", "run-test.sh", os.path.abspath(a.shell), case.name],
                        cwd=suite, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        stdin=subprocess.DEVNULL, text=True, start_new_session=True)
try:
    out, _ = proc.communicate(timeout=a.timeout)
    print(out, end="")
except subprocess.TimeoutExpired:
    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    proc.communicate()
    print("=== TIMED OUT ===")

if trs.exists():
    print(trs.read_text(errors="replace"))
    text = trs.read_text(errors="replace")
    # PASSED_UNEXPECTEDLY counts as passing, for the reason tools/conformance.py
    # spells out: the suite inverts a `-f` case, so satisfying one is the shell doing
    # what the case describes. Counting only the two obvious markers dropped it from
    # both halves of the ratio and made return-p.tst read 26/26 of 27.
    unexpected = text.count("PASSED_UNEXPECTEDLY")
    print(f"=== {text.count('%%% OK[PASSED]') + unexpected} passed, "
          f"{text.count('%%% ERROR[FAILED]')} failed"
          f"{f' ({unexpected} unexpectedly)' if unexpected else ''} ===")
else:
    print("=== no .trs produced ===")
