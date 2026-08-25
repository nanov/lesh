#!/usr/bin/env python3
"""Runs one snippet through a shell, safely, and prints its status and output.

Output goes to a FILE, not a pipe. With a pipe, communicate() waits for every
writer to close it - and lesh puts each child in its own process group, so killing
the shell's group does not reach a hung grandchild that still holds the write end.
The reaper then blocks on the very process it just failed to kill. A file has no
such dependency: once the direct child is gone, the result is readable.

This is the same reaping problem tools/conformance.py has, and the reason a hung
case can outlive its runner.
"""
import os, pathlib, signal, subprocess, sys, tempfile
from pathlib import Path

# Absolute, because the child runs in a fresh temp directory: a relative
# "./build/debug/lesh" would be resolved against that instead of the repo.
shell, code = str(pathlib.Path(sys.argv[1]).resolve()), sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

work = Path(tempfile.mkdtemp(prefix="probe."))
(work / "script").write_text(code)
out_path = work / "out"
env = dict(os.environ, LESH_FRONTEND=os.environ.get("LESH_FRONTEND", "next"))
with out_path.open("wb") as sink:
    proc = subprocess.Popen([shell, "script"], cwd=work, env=env,
                            stdout=sink, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    verdict = None
    try:
        proc.wait(timeout=timeout)
        verdict = f"rc={proc.returncode}"
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)   # pgid == pid: start_new_session
        except (ProcessLookupError, PermissionError):
            pass
        proc.kill()
        proc.wait()
        verdict = "HANG"
text = out_path.read_text(errors="replace").replace("\n", "|")
print(f"{verdict} out=[{text}]")
