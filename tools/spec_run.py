#!/usr/bin/env python3
"""Differential test runner: compare lesh against a reference shell.

Each case is a shell snippet run through both lesh and a reference shell. stdout,
stderr and exit status must match. There are no golden files for such a case: the
reference shell IS the expectation, so nothing drifts and nothing needs
regenerating.

dash is authoritative for the POSIX floor; zsh only for the curated zsh layer.
Where they disagree about POSIX, dash wins. See docs/adr/0001.

A DELIBERATE DIVERGENCE IS THE ONE THING THAT PRINCIPLE CANNOT EXPRESS, and the
exception is stated here rather than left to contradict it silently. For a
divergence, dash cannot be the expectation - the whole content of the decision is
that lesh answers differently - so the case carries its own:

    --- name [divergence: why, and what dash does instead]
    code
    === expect [status: 127] [stderr]
    the exact stdout lesh is expected to produce

The reference shell is NOT RUN for such a case. Before this existed all three
things a divergence needs were conflated into one `[xfail: divergence ...]`
marker: the case still ran against dash, the difference was expected, and it was
tallied as a known failure alongside the gaps nobody has got to yet - so the score
could not tell "we chose this" from "we have not built this". The three are now
separate. Documented: ADR-0001's divergence section. Excluded: no comparison
happens. Tested: the expectation above, asserted, and a FAIL if it stops holding.
`known-fail` therefore means exactly one thing again - a real gap.

Consequences worth stating, because each is a cost:

  - A divergence has NO STALENESS DETECTOR, where an xfail has XPASS. Nothing here
    notices if dash later adopts lesh's answer and the divergence stops being one.
    That is deliberate: a divergence going away is a decision to unmake in ADR-0001,
    not a fact for a runner to discover, and re-asserting dash's answer to detect it
    would make dash the oracle again for exactly the case where it cannot be one.
    What dash does is recorded in the marker text, in prose, where a decision lives.
  - `=== expect` is recognised in EVERY case, and is an error in one that is not a
    divergence. Recognising it only inside a `[divergence: ...]` case was tried
    first, to keep `===` - legal shell input - out of the delimiter space of the 547
    cases that are not divergences. It has a silent failure mode: an expectation
    block whose marker was forgotten becomes two shell commands, `===` and the
    output, and dash runs the same two, so the case PASSES while asserting nothing.
    A reserved line that hard-errors beats a body that can be silently misread,
    which is the same argument that makes an unknown `[directive]` fatal below.
  - The expectation ends at the first BLANK LINE, not at the next `---`. It has to:
    a .spec puts the comment block introducing the next case after a blank line,
    inside the previous case's region, and those comments would otherwise be read
    as expected output. So an expectation whose stdout contains or ends with a
    blank line cannot be written - and would FAIL loudly rather than pass quietly.
  - A divergence is asserted on the `next` front end only. `legacy` is the strangler
    quarantine ADR-0002 deletes; it is missing most of the language and has no
    chosen behaviour to record, so asserting lesh's answer there would turn every
    divergence into a legacy failure that means nothing. On legacy they are reported
    as not asserted, and counted as neither pass nor known-fail there either.

HOW A CASE IS INVOKED (issue #41). Until #41 every case ran as `shell -c code` and
nothing else, so no case could see a bug in how the shell reads its own command
line or its own standard input - and two real ones escaped: `+i` taken for a script
pathname (#33, 3,600 conformance assertions), and `read` returning EOF forever when
the script arrived on fd 0 (#31, read-p.tst at 1/32). Two mechanisms close that:

  - `[stdin]` on a case feeds the body to the shell on file descriptor 0 with no
    arguments at all, which is the path every real script and every yash case
    takes. A per-case directive, rather than a separate corpus file or a second
    run of the whole corpus in the other mode: the invocation form is a property
    of what a case is testing, not of the file it happens to sit in. Running all
    321 cases both ways would double the wall clock to re-assert three hundred
    that do not care, and would need a mode-scoped xfail axis on top, because
    `-c` and stdin legitimately differ - `set -v` echoes input and not `-c`, and
    a `read` on fd 0 competes with the script arriving there.
  - `$TESTEE` in the environment of every case names the shell CURRENTLY UNDER
    TEST, absolute. A case can therefore re-invoke it with any argv it likes and
    the comparison stays honest: lesh-invoking-lesh against dash-invoking-dash.
    This is what the yash suite does, and it is why the yash suite found what this
    corpus could not. A case that needs a particular argv[0] makes its own symlink
    (`ln -s "$TESTEE" $d/sh`) - in the case, where it is visible, rather than in
    the harness, where it would be a guess. dash's option parsing turned out not
    to depend on argv[0]; the yash suite's symlink is for PATH resolution.

Every case runs in its own process group and is killed by group on timeout. Of the
shell test runners surveyed for issue #4, only FreeBSD's kyua did this; every other
one used a plain timeout, which kills the direct child and orphans its grandchildren.
That is not theoretical - a single hung case cost this project a CPU core for 19
minutes. macOS ships no setsid(1), which is why this is Python rather than shell.
A case that re-invokes `$TESTEE` makes this load-bearing twice over: the shell the
harness spawns is no longer the only process to reap.
"""

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

_FRONTEND = None

# The front end a divergence's expectation describes. See the module docstring:
# legacy is a quarantine, not a shell with chosen behaviour.
DIVERGENCE_FRONTEND = "next"

# A case header is `--- name` followed by any number of bracketed directives:
#
#   [stdin]                 feed the body to the shell on fd 0 instead of -c
#   [xfail: reason]         known failure on every front end
#   [xfail(next): reason]   known failure on one front end
#   [divergence: reason]    deliberate difference from the reference shell; the
#                           case carries its own expectation and dash is not run
#
# xfail scoping became necessary the moment both front ends were live: they fail
# different things, and one shared marker list would hide a regression in
# whichever front end happened not to be under test.
CASE_RE = re.compile(r"^---\s+(?P<name>.+?)\s*(?P<tags>(?:\[[^\]]*\]\s*)*)$")
TAG_RE = re.compile(r"\[([^\]]*)\]")
XFAIL_RE = re.compile(r"^xfail(?:\((?P<scope>[a-z]+)\))?:\s*(?P<reason>.*)$")
DIVERGENCE_RE = re.compile(r"^divergence:\s*(?P<reason>.*)$")

# The expectation of a divergence case: `=== expect` with the same bracketed
# directive grammar the header uses, then the expected stdout verbatim.
#
#   [status: N]   expected exit status, default 0 - `a command_file that cannot be
#                 opened exits 127` is a divergence about nothing else
#   [stderr]      a diagnostic is expected; without it stderr must be EMPTY.
#                 Whether, not what: the text is implementation-specific, which is
#                 the same rule the differential comparison applies.
EXPECT_RE = re.compile(r"^===\s+expect\s*(?P<tags>(?:\[[^\]]*\]\s*)*)$")
STATUS_RE = re.compile(r"^status:\s*(?P<status>-?\d+)$")


@dataclass
class Expect:
    """What a divergence case asserts of lesh, in place of the reference shell."""
    stdout: str
    status: int
    stderr: bool  # True: a diagnostic is expected. False: stderr must be empty.


@dataclass
class Case:
    name: str
    code: str
    xfail: str | None
    xfail_scope: str | None  # None = all front ends
    divergence: str | None  # `[divergence: reason]`; mutually exclusive with xfail
    expect: Expect | None  # set exactly when divergence is
    on_stdin: bool  # `[stdin]`: the body arrives on fd 0, not as -c
    path: Path
    line: int


@dataclass
class Run:
    stdout: str
    stderr: str
    status: int
    timed_out: bool


def parse_directives(tags: str, where: str) -> tuple[bool, str | None, str | None, str | None]:
    """Read a case header's directives. Returns (on_stdin, xfail, scope, divergence).

    An unrecognised directive is a hard error rather than a warning or a silent
    skip. A misspelled `[stdni]` that quietly ran the case as `-c` would recreate
    the exact blind spot #41 exists to close - a case that looks like it covers
    the stdin path and does not.
    """
    on_stdin = False
    xfail = scope = divergence = None
    for tag in TAG_RE.findall(tags):
        directive = tag.strip()
        if directive == "stdin":
            on_stdin = True
        elif m := XFAIL_RE.match(directive):
            xfail, scope = m["reason"], m["scope"]
        elif m := DIVERGENCE_RE.match(directive):
            divergence = m["reason"]
        else:
            raise SystemExit(f"{where}: unknown case directive [{tag}]")
    if xfail is not None and divergence is not None:
        raise SystemExit(f"{where}: a case is either an xfail or a divergence, not both - "
                         "an xfail is a gap to close, a divergence is a decision to keep")
    return on_stdin, xfail, scope, divergence


def parse_expect(tags: str, lines: list[str], where: str) -> Expect:
    """Build the expectation of a divergence case from its `=== expect` block."""
    status, stderr = 0, False
    for tag in TAG_RE.findall(tags):
        directive = tag.strip()
        if directive == "stderr":
            stderr = True
        elif m := STATUS_RE.match(directive):
            status = int(m["status"])
        else:
            raise SystemExit(f"{where}: unknown expectation directive [{tag}]")
    return Expect("".join(line + "\n" for line in lines), status, stderr)


def parse_spec(path: Path) -> list[Case]:
    cases: list[Case] = []
    name = xfail = scope = divergence = expect_tags = None
    on_stdin = expect_open = False
    start = 0
    body: list[str] = []
    expect_body: list[str] = []

    def flush():
        if name is None or not (code := "\n".join(body).strip()):
            return
        where = f"{path}:{start}"
        if divergence is not None and expect_tags is None:
            raise SystemExit(f"{where}: [divergence: ...] with no `=== expect` block - dash "
                             "cannot be the expectation of a case that deliberately differs")
        expect = parse_expect(expect_tags, expect_body, where) if divergence is not None else None
        cases.append(Case(name, code, xfail, scope, divergence, expect, on_stdin, path, start))

    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        if m := CASE_RE.match(raw):
            flush()
            name, start, body, expect_body, expect_tags = m["name"], lineno, [], [], None
            expect_open = False
            on_stdin, xfail, scope, divergence = parse_directives(m["tags"], f"{path}:{lineno}")
        elif m := EXPECT_RE.match(raw):
            if divergence is None:
                raise SystemExit(f"{path}:{lineno}: `=== expect` on a case that is not a "
                                 "[divergence: ...] - the reference shell is its expectation")
            if expect_tags is not None:
                raise SystemExit(f"{path}:{lineno}: a second `=== expect` in one case")
            expect_tags, expect_open = m["tags"], True
        elif name is None and (not raw.strip() or raw.lstrip().startswith("#")):
            continue  # file header
        elif expect_open:
            if raw.strip():
                expect_body.append(raw)
            else:
                expect_open = False  # a blank line ends the expected output
        elif expect_tags is not None:
            # Past the expectation only the comment block introducing the next case
            # may follow. Code there would be appended to a snippet whose output has
            # already been stated, so it is a hard error rather than a silent append.
            if raw.strip() and not raw.lstrip().startswith("#"):
                raise SystemExit(f"{path}:{lineno}: code after a divergence's "
                                 "`=== expect` block, which ended at the blank line")
        else:
            body.append(raw)
    flush()
    return cases


def testee_path(shell: str) -> str:
    """Absolute path to `shell`, for $TESTEE.

    Absolute because a case that re-invokes the shell is free to `cd` first, and
    `--shell ./build/debug/lesh` would then resolve to nothing. Per-shell because
    the whole point is that a case comparing `"$TESTEE" +i +m` compares
    lesh-invoking-lesh against dash-invoking-dash; one shared value would compare
    lesh against lesh and pass no matter what either shell did.
    """
    found = shutil.which(shell)
    return str(Path(found if found else shell).resolve())


def child_env(testee: str) -> dict[str, str]:
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
    if _FRONTEND is not None:
        env["LESH_FRONTEND"] = _FRONTEND
    # The shell under test, reachable from inside a case. See the module docstring.
    env["TESTEE"] = testee
    return env


def run_shell(shell: str, testee: str, code: str, timeout: float, on_stdin: bool) -> Run:
    """Run one snippet in its own process group, reaping the whole group on timeout.

    `on_stdin` picks the invocation form. The default is `shell -c code`; a
    `[stdin]` case is instead spawned with NO arguments and the body written to fd
    0, which is what a script really looks like to a shell and is the path that
    hid #31.

    EVERY CASE RUNS IN ITS OWN EMPTY DIRECTORY. Not for isolation between cases -
    they use absolute paths - but because a case is also run against `legacy`, which
    has no command substitution and no compound commands, so it reads a one-line
    case as a single command with every later word as an operand. One `mkdir` case
    therefore created twelve directories named `for`, `in`, `do`, `;`, `[%s]` and
    friends. They were EMPTY, so `git status` could not see them, and they sat in the
    repo root until someone listed it by hand. A runner that litters the tree it is
    testing is a runner nobody trusts.
    """
    with tempfile.TemporaryDirectory(prefix="spec.") as work:
        proc = subprocess.Popen(
            [shell] if on_stdin else [shell, "-c", code],
            cwd=work,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.PIPE if on_stdin else subprocess.DEVNULL,
            text=True,
            env=child_env(testee),
            start_new_session=True,  # own group, so killpg reaches grandchildren
        )
        try:
            # A script ends in a newline; parse_spec strips the body, so put it back.
            out, err = proc.communicate(code + "\n" if on_stdin else None,
                                        timeout=timeout)
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
    ap.add_argument("--frontend", default=None, choices=["legacy", "next"],
                    help="which lesh front end to exercise (sets LESH_FRONTEND)")
    ap.add_argument("paths", nargs="*", default=["tests/spec"])
    args = ap.parse_args()

    global _FRONTEND
    _FRONTEND = args.frontend

    files: list[Path] = []
    for p in map(Path, args.paths):
        files.extend(sorted(p.glob("*.spec")) if p.is_dir() else [p])
    if not files:
        print("no .spec files found", file=sys.stderr)
        return 2

    passed = xfailed = failed = xpassed = diverged = unasserted = 0
    ref_testee = testee_path(args.ref)
    shell_testee = testee_path(args.shell)
    # The shells themselves are addressed absolutely for the same reason $TESTEE is:
    # each case now runs in its own empty directory, so `--shell ./build/debug/lesh`
    # would resolve against that and vanish. testee_path already does the work.
    args.ref = ref_testee
    args.shell = shell_testee

    if args.frontend:
        print(f"front end under test: {args.frontend}")

    for path in files:
        print(f"\n{path}")
        for case in parse_spec(path):
            label = f"{case.name} [stdin]" if case.on_stdin else case.name

            # A DIVERGENCE IS NOT COMPARED. Its own expectation stands in for the
            # reference shell, which is not run at all here.
            if case.divergence is not None:
                if (args.frontend or "legacy") != DIVERGENCE_FRONTEND:
                    unasserted += 1
                    print(f"  skip   {label}  -- divergence, not asserted on "
                          f"{args.frontend or 'legacy'}")
                    continue
                got = run_shell(args.shell, shell_testee, case.code, args.timeout, case.on_stdin)
                problems = ["      TIMED OUT (process group killed)"] if got.timed_out else []
                problems += diff("stdout", got.stdout, case.expect.stdout)
                problems += diff("status", str(got.status), str(case.expect.status))
                problems += diff("stderr?", str(bool(got.stderr)), str(case.expect.stderr))
                if problems:
                    failed += 1
                    print(f"  FAIL   {label}  ({path}:{case.line})"
                          f"  -- the recorded divergence no longer holds")
                    print("\n".join(problems))
                else:
                    diverged += 1
                    print(f"  diverge {label}  ({case.divergence})")
                continue

            ref = run_shell(args.ref, ref_testee, case.code, args.timeout, case.on_stdin)
            got = run_shell(args.shell, shell_testee, case.code, args.timeout, case.on_stdin)

            problems = []
            if got.timed_out:
                problems.append("      TIMED OUT (process group killed)")
            problems += diff("stdout", got.stdout, ref.stdout)
            problems += diff("status", str(got.status), str(ref.status))
            # stderr text is implementation-specific; compare only whether it is empty.
            problems += diff("stderr?", str(bool(got.stderr)), str(bool(ref.stderr)))

            # A scoped marker applies only when that front end is under test.
            expected_to_fail = case.xfail is not None and (
                case.xfail_scope is None or case.xfail_scope == (args.frontend or "legacy"))

            if problems and expected_to_fail:
                xfailed += 1
                print(f"  xfail  {label}  ({case.xfail})")
                if args.verbose:
                    print("\n".join(problems))
            elif problems:
                failed += 1
                print(f"  FAIL   {label}  ({path}:{case.line})")
                print("\n".join(problems))
            elif expected_to_fail:
                xpassed += 1
                print(f"  XPASS  {label}  -- now passes, remove the xfail marker")
            else:
                passed += 1
                print(f"  pass   {label}")

    total = passed + xfailed + failed + xpassed + diverged + unasserted
    # A divergence is neither a pass nor a known failure. Its own tally is the whole
    # point: `known-fail` now counts only gaps, so the number means one thing.
    tally = f"{passed} pass, {xfailed} known-fail, {failed} FAIL, {xpassed} XPASS"
    if diverged:
        tally += f", {diverged} divergence"
    if unasserted:
        tally += f", {unasserted} divergence not asserted on {args.frontend or 'legacy'}"
    print(f"\n{total} cases against {args.ref}: {tally}")
    # Known failures are the score, not a gate. Unexpected failures and cases that
    # started passing without their marker being removed are both regressions - and
    # so is a divergence whose recorded expectation has stopped holding, which lands
    # in `failed` rather than in a tally of its own for exactly that reason.
    return 1 if (failed or xpassed) else 0


if __name__ == "__main__":
    sys.exit(main())
