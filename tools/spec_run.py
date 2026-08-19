#!/usr/bin/env python3
"""Differential test runner: compare lesh against a reference shell.

Each case is a shell snippet run through both lesh and a reference shell. stdout,
stderr and exit status must match. There are no golden files: the reference shell
IS the expectation, so nothing drifts and nothing needs regenerating.

dash is authoritative for the POSIX floor; zsh only for the curated zsh layer.
Where they disagree about POSIX, dash wins. See docs/adr/0001.

Every case runs in its own process group and is killed by group on timeout. Of the
shell test runners surveyed for issue #4, only FreeBSD's kyua did this; every other
one used a plain timeout, which kills the direct child and orphans its grandchildren.
That is not theoretical - a single hung case cost this project a CPU core for 19
minutes. macOS ships no setsid(1), which is why this is Python rather than shell.
"""

import argparse
import os
import re
import signal
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

CASE_RE = re.compile(r"^---\s+(?P<name>.+?)\s*(?:\[xfail:\s*(?P<xfail>[^\]]*)\])?\s*$")


@dataclass
class Case:
    name: str
    code: str
    xfail: str | None
    path: Path
    line: int


@dataclass
class Run:
    stdout: str
    stderr: str
    status: int
    timed_out: bool


def parse_spec(path: Path) -> list[Case]:
    cases: list[Case] = []
    name = xfail = None
    start = 0
    body: list[str] = []

    def flush():
        if name is not None and (code := "\n".join(body).strip()):
            cases.append(Case(name, code, xfail, path, start))

    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        if m := CASE_RE.match(raw):
            flush()
            name, xfail, start, body = m["name"], m["xfail"], lineno, []
        elif name is None and (not raw.strip() or raw.lstrip().startswith("#")):
            continue  # file header
        else:
            body.append(raw)
    flush()
    return cases


def child_env() -> dict[str, str]:
    """Environment for the shells under test.

    Leak detection is turned OFF here deliberately. This harness measures
    behaviour; leaks are a separate gate with its own expected result. Leaving it
    on means one ownerless allocation at startup turns every behavioural case
    non-zero, so 25 cases fail for one reason that has nothing to do with any of
    them. Once ADR-0007 (free everything on shutdown) is implemented, lesh will
    exit clean and this override can be deleted.
    """
    env = dict(os.environ)
    opts = [o for o in env.get("ASAN_OPTIONS", "").split(":") if o and not o.startswith("detect_leaks")]
    env["ASAN_OPTIONS"] = ":".join(opts + ["detect_leaks=0"])
    return env


def run_shell(shell: str, code: str, timeout: float) -> Run:
    """Run one snippet in its own process group, reaping the whole group on timeout."""
    proc = subprocess.Popen(
        [shell, "-c", code],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        stdin=subprocess.DEVNULL,
        text=True,
        env=child_env(),
        start_new_session=True,  # own process group, so killpg reaches grandchildren
    )
    try:
        out, err = proc.communicate(timeout=timeout)
        return Run(out, err, proc.returncode, False)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            proc.kill()
        out, err = proc.communicate()
        return Run(out, err, -1, True)


def diff(label: str, got: str, want: str) -> list[str]:
    if got == want:
        return []
    return [f"      {label}: got {got!r}", f"      {' ' * len(label)}  want {want!r}"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shell", default="./build/debug/lesh", help="shell under test")
    ap.add_argument("--ref", default="dash", help="reference shell (dash for the POSIX floor)")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--verbose", "-v", action="store_true", help="show diffs for expected failures too")
    ap.add_argument("paths", nargs="*", default=["tests/spec"])
    args = ap.parse_args()

    files: list[Path] = []
    for p in map(Path, args.paths):
        files.extend(sorted(p.glob("*.spec")) if p.is_dir() else [p])
    if not files:
        print("no .spec files found", file=sys.stderr)
        return 2

    passed = xfailed = failed = xpassed = 0

    for path in files:
        print(f"\n{path}")
        for case in parse_spec(path):
            ref = run_shell(args.ref, case.code, args.timeout)
            got = run_shell(args.shell, case.code, args.timeout)

            problems: list[str] = []
            if got.timed_out:
                problems.append("      TIMED OUT (process group killed)")
            problems += diff("stdout", got.stdout, ref.stdout)
            problems += diff("status", str(got.status), str(ref.status))
            # stderr text is implementation-specific; compare only whether it is empty.
            problems += diff("stderr?", str(bool(got.stderr)), str(bool(ref.stderr)))

            if problems and case.xfail is not None:
                xfailed += 1
                print(f"  xfail  {case.name}  ({case.xfail})")
                if args.verbose:
                    print("\n".join(problems))
            elif problems:
                failed += 1
                print(f"  FAIL   {case.name}  ({path}:{case.line})")
                print("\n".join(problems))
            elif case.xfail is not None:
                xpassed += 1
                print(f"  XPASS  {case.name}  -- now passes, remove the xfail marker")
            else:
                passed += 1
                print(f"  pass   {case.name}")

    total = passed + xfailed + failed + xpassed
    print(f"\n{total} cases against {args.ref}: "
          f"{passed} pass, {xfailed} known-fail, {failed} FAIL, {xpassed} XPASS")
    # Known failures are the score, not a gate. Unexpected failures and cases that
    # started passing without their marker being removed are both regressions.
    return 1 if (failed or xpassed) else 0


if __name__ == "__main__":
    sys.exit(main())
