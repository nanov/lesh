#!/usr/bin/env python3
"""Drives an interactive lesh on a pty and asserts #159's terminal handoff.

Four things, and they are the mechanism behind acceptance criteria that need a
human at a real terminal to watch (`nvim ./` taking the screen, `less` paging):

  a. a foreground external command's process group BECOMES the terminal's
     foreground group while it runs - checked from outside the session with
     `ps -o tpgid`, and from inside by the child itself;
  b. the terminal comes BACK to the shell's group after the job, not at the end
     of the line;
  c. the child execs with DEFAULT SIGTTOU/SIGTTIN/SIGTSTP - the SIG_IGN the
     editing loop installs does not leak through execve;
  d. `read` after a tty-taking command still gets the user's typing, which is
     the `nvim .; read x` shape and the reason the reclaim is per job.

#161 adds a fifth, on the same mechanism: Ctrl-Z of a foreground job reports the
stop, gives the terminal back, sets `$?` to 128+SIGTSTP and returns to a prompt,
leaving the process alive and stopped for `kill -CONT`.

#160 completes the FOREGROUND-JOB SET and asserts the negative space:

  e. a foreground PIPELINE is one job - the terminal goes to its process group,
     and the LAST stage, which only joined that group, reports owning it;
  f. a foreground `( )` gives the terminal to the command inside it;
  g. `&` and `$( )` never take it, which is the half a widening change can break
     silently and so is checked rather than assumed;
  h. EVERY stage execs with SIGTTOU/SIGTTIN/SIGTSTP at their defaults, not just
     the one that made the handoff - the handoff is per process GROUP and the
     reset is per PROCESS, so Ctrl-Z of `sleep 30 | sleep 30` must stop BOTH;
  i. Ctrl-C of either new shape still returns 130 AND abandons the rest of the
     line, which the handoff itself is what puts at risk.

Note that (e) INVERTS an assertion this file used to make: before #160 a pipeline
was on the "never takes the terminal" list.

Usage: tools/tty_handoff_probe.py [path-to-lesh]   (default ./build/release/lesh)
"""

import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

TIMEOUT = 20.0


class Shell:
	def __init__(self, path):
		master, slave = pty.openpty()
		fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 100, 0, 0))
		home = tempfile.mkdtemp(prefix="lesh-tty-probe-")
		env = {
			"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
			"TERM": "xterm-256color",
			"HOME": home,
			"ENV": "/dev/null",
			"LC_ALL": "C",
		}
		pid = os.fork()
		if pid == 0:
			os.setsid()
			fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
			os.dup2(slave, 0)
			os.dup2(slave, 1)
			os.dup2(slave, 2)
			if slave > 2:
				os.close(slave)
			os.close(master)
			os.execve(path, [path, "-i"], env)
			os._exit(127)
		self.pid = pid
		self.master = master
		# Kept open so the pty pair does not collapse when the shell dies mid-run.
		self.slave = slave
		self.log = ""

	def fg_pgrp(self):
		# `ps -o tpgid` is the foreground process group of the process's own
		# controlling terminal, which is exactly what is being asserted - and it is
		# the only reading available from OUTSIDE the session. `tcgetpgrp` on the
		# slave fd is not: POSIX lets it answer ENOTTY to a process that does not
		# share the controlling terminal, and macOS takes that option.
		try:
			out = subprocess.run(["ps", "-o", "tpgid=", "-p", str(self.pid)],
			                     capture_output=True, text=True, timeout=5).stdout.strip()
			return int(out) if out else -1
		except (ValueError, OSError, subprocess.SubprocessError):
			return -1

	def pump(self, seconds=0.05):
		deadline = time.time() + seconds
		while True:
			left = deadline - time.time()
			if left <= 0:
				return
			r, _, _ = select.select([self.master], [], [], left)
			if not r:
				return
			try:
				chunk = os.read(self.master, 65536)
			except OSError:
				return
			if not chunk:
				return
			self.log += chunk.decode("utf-8", "replace")

	def send(self, text):
		os.write(self.master, text.encode())

	def wait_for(self, pattern, timeout=TIMEOUT, times=1):
		# A COUNT, not just a search, for the same reason the C++ pty harness counts:
		# `self.log` holds everything the session ever wrote, so a second stop report
		# or a second prompt has to be told from the first or the wait answers
		# instantly with the old one.
		deadline = time.time() + timeout
		while time.time() < deadline:
			if len(re.findall(pattern, self.log)) >= times:
				return True
			self.pump(0.05)
		return len(re.findall(pattern, self.log)) >= times

	def close(self):
		try:
			self.send("exit\n")
			self.pump(0.5)
		except OSError:
			pass
		try:
			os.kill(self.pid, signal.SIGKILL)
		except ProcessLookupError:
			pass
		try:
			os.waitpid(self.pid, 0)
		except ChildProcessError:
			pass
		os.close(self.master)
		os.close(self.slave)


results = []


def check(name, ok, detail=""):
	results.append((name, ok, detail))
	print(("  PASS  " if ok else "  FAIL  ") + name + (("   " + detail) if detail else ""))


def main():
	path = sys.argv[1] if len(sys.argv) > 1 else "./build/release/lesh"
	path = os.path.abspath(path)
	py = sys.executable

	sh = Shell(path)
	# Every pid the session ever reported stopped. A set, because the wire is
	# cumulative and each case has to count only the stops it caused.
	stopped_pids_seen = set()
	try:
		sh.wait_for(r"\$|>|%", timeout=5)
		shell_pgrp = sh.fg_pgrp()
		check("shell owns the terminal at the prompt",
		      shell_pgrp == sh.pid, "tpgid=%d shell pid/pgrp=%d" % (shell_pgrp, sh.pid))

		# (a) The child says so itself, from inside the job.
		sh.send("%s -c \"import os;print('SELF',os.tcgetpgrp(0)==os.getpgid(0),os.getpgid(0))\"\n" % py)
		sh.wait_for(r"SELF (True|False) \d+")
		m = re.search(r"SELF (True|False) (\d+)", sh.log)
		check("the child is the terminal's foreground group",
		      bool(m) and m.group(1) == "True",
		      m.group(0) if m else "no report from the child")
		child_pgrp = int(m.group(2)) if m else -1
		check("and that group is NOT the shell's",
		      child_pgrp != -1 and child_pgrp != shell_pgrp,
		      "child pgrp=%d shell pgrp=%d" % (child_pgrp, shell_pgrp))

		# (a) again, watched from outside while the job runs.
		sh.send("sleep 1\n")
		seen = set()
		deadline = time.time() + 1.5
		while time.time() < deadline:
			seen.add(sh.fg_pgrp())
			sh.pump(0.02)
		handed_to = sorted(g for g in seen if g not in (shell_pgrp, -1))
		check("an outside observer sees the terminal move to another group",
		      bool(handed_to), "foreground groups seen while `sleep 1` ran: %s" % sorted(seen))

		# (b) and it comes back.
		sh.pump(0.4)
		check("the terminal is the shell's again at the next prompt",
		      sh.fg_pgrp() == shell_pgrp, "tpgid=%d" % sh.fg_pgrp())

		# (c) dispositions after execve.
		prog = ("import signal;"
		        "d=lambda s: 'IGN' if signal.getsignal(s) is signal.SIG_IGN else "
		        "('DFL' if signal.getsignal(s) is signal.SIG_DFL else 'OTHER');"
		        "print('DISP',d(signal.SIGTTOU),d(signal.SIGTTIN),d(signal.SIGTSTP))")
		sh.send("%s -c \"%s\"\n" % (py, prog))
		sh.wait_for(r"DISP \w+ \w+ \w+")
		m = re.search(r"DISP (\w+) (\w+) (\w+)", sh.log)
		check("the child execs with default SIGTTOU/SIGTTIN/SIGTSTP",
		      bool(m) and m.groups() == ("DFL", "DFL", "DFL"),
		      m.group(0) if m else "no report from the child")

		# (d) the `nvim .; read x` shape: a tty-reading builtin after a tty-taking
		# command, on the SAME line, so only a per-job reclaim can save it. The
		# command TAKES THE TERMINAL ITSELF and leaves it taken, which is what any
		# full-screen program leaves behind when the shell has handed over; without
		# the post-wait reclaim `read` meets EIO on a terminal it is background on,
		# and the loop's line-end reclaim comes far too late to help it.
		sh.send("%s -c 'import os;os.tcsetpgrp(0,os.getpgid(0))'; read x; echo GOT=$x\n" % py)
		sh.pump(0.6)
		sh.send("typed-after-the-job\n")
		got = sh.wait_for(r"GOT=typed-after-the-job", timeout=6)
		check("`read` after a tty-taking command on the same line gets input",
		      got, "" if got else "read never produced GOT=... (tail: %r)" % sh.log[-200:])
		# SCOPE (#158 decision 3), COMPLETED BY #160 - and this is the half of the
		# probe #160 changed rather than added to. The foreground-job set is now
		# three shapes wide, and a pipeline moved from the "must not" list to the
		# "must" list: it is ONE job, so its group gets the terminal and every stage
		# is in that group. A probe still asserting `sleep 1 | cat` never takes the
		# terminal would now be contradicting the shell it is here to check.
		#
		# `tcgetpgrp(2)` AND NOT `(0)` for the inner reports below. A pipeline
		# stage's fd 0 is a PIPE by the time it runs, so asking fd 0 answers ENOTTY
		# and says nothing about the terminal; stderr is still the terminal in every
		# one of these shapes. The `SELF` check further up can use fd 0 because a
		# plain foreground command's stdin was never replaced.
		inner = ("import os,sys;"
		         "print(sys.argv[1], os.tcgetpgrp(2) == os.getpgid(0), os.getpgid(0))")

		# (e) THE POSITIVES: the two job shapes #160 adds, each reporting from
		# INSIDE itself. The pipeline's report comes from its LAST stage, which is
		# the discriminating one - it never called `tcsetpgrp`, it only joined the
		# leader's group, so it owning the terminal is what proves one handoff
		# covers the job. The subshell's comes from the command INSIDE the `( )`,
		# which is one fork further down than the subshell itself and only reachable
		# because the saved tty fd survives `enter_subshell` for a foreground `( )`.
		for label, line, tag in (
			("a foreground pipeline's last stage",
			 "sleep 1 | %s -c \"%s\" PIPE" % (py, inner), "PIPE"),
			("the command inside a foreground ( )",
			 "(%s -c \"%s\" SUBSH)" % (py, inner), "SUBSH"),
		):
			sh.send(line + "\n")
			sh.wait_for(r"%s (True|False) \d+" % tag)
			m = re.search(r"%s (True|False) (\d+)" % tag, sh.log)
			check("%s is the terminal's foreground group" % label,
			      bool(m) and m.group(1) == "True",
			      m.group(0) if m else "no report from inside the job")
			check("and %s is in a group of its own, not the shell's" % label,
			      bool(m) and int(m.group(2)) != shell_pgrp,
			      "job pgrp=%s shell pgrp=%d" % (m.group(2) if m else "?", shell_pgrp))
			sh.pump(0.4)

		# (f) THE NEGATIVES, which #160 asserts rather than assumes. Each reaches
		# the very same `run_simple_command` a foreground command does, one fork
		# further down, and the only thing between it and the terminal is
		# `enter_subshell` clearing the saved fd - which #160 turned into a decision
		# with two answers, so "the default is still clear" needs checking. A
		# `$(read x)` that became the foreground group would take the user's
		# keystrokes away from the prompt they were typed at.
		for label, line in (("a background job", "sleep 1 &"),
		                    ("a command substitution", "x=$(sleep 1)")):
			sh.send(line + "\n")
			seen = set()
			deadline = time.time() + 1.4
			while time.time() < deadline:
				seen.add(sh.fg_pgrp())
				sh.pump(0.02)
			stolen = sorted(g for g in seen if g not in (shell_pgrp, -1))
			check("%s never takes the terminal" % label,
			      not stolen, "foreground groups seen: %s" % sorted(seen))
			sh.pump(0.4)

		# (g) `sleep 5 | cat` STILL DIES TO A SINGLE CTRL-C, the acceptance
		# criterion #160's own handoff puts at risk: the pipeline's group is the
		# foreground group now, so the shell is excluded from the keyboard interrupt
		# and the reap has to synthesize the delivery. Both halves, because the
		# first version of #159's synthesis had the status right and the unwind
		# wrong: 130, AND the rest of the line abandoned.
		sh.send("sleep 5 | cat; echo PIPEAFTER=$((1 + 1))\n")
		sh.pump(0.4)
		sh.send("\x03")
		sh.pump(1.0)
		sh.send("echo PIPEINT=$?\n")
		ok = sh.wait_for(r"PIPEINT=130", timeout=8)
		check("Ctrl-C of a foreground pipeline returns 130 to a usable prompt",
		      ok, "" if ok else "no PIPEINT=130 (tail: %r)" % sh.log[-200:])
		check("and it abandons the rest of the line",
		      "PIPEAFTER=2" not in sh.log,
		      "`echo` after the pipeline ran anyway" if "PIPEAFTER=2" in sh.log else "")

		# (g2) EVERY STAGE EXECS WITH THE STOP SIGNALS AT THEIR DEFAULT, which the
		# handoff alone does not give them. The handoff is one per process GROUP -
		# the leader makes it for the job - but #158 decision 4's reset is one per
		# PROCESS THAT EXECS, and while the two were one function only the leader got
		# either: `less` at the end of a pipeline had an ignored SIGTSTP. Ctrl-Z of a
		# two-stage pipeline is the cheapest way to see it, because a stage that
		# ignored the stop keeps running and the shell blocks on it.
		sh.send("sleep 30 | sleep 30\n")
		sh.pump(0.4)
		sh.send("\x1a")
		sh.wait_for(r"stopped: pid \d+", timeout=6, times=2)
		stage_pids = re.findall(r"stopped: pid (\d+)", sh.log)
		# The Ctrl-Z check further down reports one stop of its own, so count the
		# ones this case added rather than the ones on the wire.
		before = len(stopped_pids_seen)
		for p in stage_pids:
			stopped_pids_seen.add(p)
		check("every stage of a foreground pipeline stops on Ctrl-Z",
		      len(stopped_pids_seen) - before >= 2,
		      "stopped pids reported: %s" % stage_pids)
		sh.send("echo PIPETSTP=$?\n")
		want = 128 + int(signal.SIGTSTP)
		ok = sh.wait_for(r"PIPETSTP=%d" % want, timeout=6)
		check("and the pipeline's $? is 128+SIGTSTP from a usable prompt",
		      ok, "" if ok else "no PIPETSTP=%d (tail: %r)" % (want, sh.log[-200:]))
		# Continued and then killed: nothing else will, and a stopped `sleep 30`
		# outliving this probe would be the probe's own leak.
		for p in stage_pids:
			for sig in (signal.SIGCONT, signal.SIGKILL):
				try:
					os.kill(int(p), sig)
				except (ProcessLookupError, PermissionError):
					pass

		# (h) The same pair for a foreground `( )`, which needs more than the note
		# to keep: the subshell is excluded too and has no interactive default left,
		# so it takes SIGINT's real default action and dies of it rather than
		# exiting 130 - the only thing the waiting parent can tell from `exit 130`.
		sh.send("(sleep 5); echo SUBAFTER=$((1 + 1))\n")
		sh.pump(0.4)
		sh.send("\x03")
		sh.pump(1.0)
		sh.send("echo SUBINT=$?\n")
		ok = sh.wait_for(r"SUBINT=130", timeout=8)
		check("Ctrl-C of a foreground ( ) returns 130 to a usable prompt",
		      ok, "" if ok else "no SUBINT=130 (tail: %r)" % sh.log[-200:])
		check("and it abandons the rest of the line",
		      "SUBAFTER=2" not in sh.log,
		      "`echo` after the subshell ran anyway" if "SUBAFTER=2" in sh.log else "")

		# NO REGRESSION on the one thing the SIG_IGN leak was propping up: Ctrl-C
		# of a foreground job. The child is the foreground group now, so the kernel
		# delivers straight to it and the shell only has to reap.
		sh.send("sleep 30\n")
		sh.pump(0.4)
		sh.send("\x03")
		sh.pump(0.5)
		sh.send("echo INTOK=$?\n")
		ok = sh.wait_for(r"INTOK=130", timeout=5)
		check("Ctrl-C of a foreground command returns to a usable prompt",
		      ok, "" if ok else "no INTOK=130 (tail: %r)" % sh.log[-200:])

		# #161, the sibling of the check above and the case the SIG_IGN leak was
		# hiding until #159 removed it. `\x1a` is the driver's VSUSP, so with the
		# child holding the terminal and SIGTSTP back at its default the job really
		# stops - and the shell's foreground wait had no WUNTRACED, so it blocked
		# here forever. Everything below is unreachable without the fix.
		sh.send("sleep 30\n")
		sh.pump(0.4)
		sh.send("\x1a")
		# THE LAST REPORT ON THE WIRE, not the first. `sh.log` is cumulative and the
		# pipeline case above already reported two stops of its own, so a `re.search`
		# here would answer about one of THOSE - a pid this probe has already killed,
		# which then passes the liveness check below on a zombie that still accepts
		# signals. Found by adding that case: the check went green about the wrong
		# process.
		reported = sh.wait_for(r"stopped: pid \d+", timeout=5,
		                       times=len(stopped_pids_seen) + 1)
		all_reported = re.findall(r"stopped: pid (\d+)", sh.log)
		m = all_reported[-1] if all_reported else None
		check("Ctrl-Z of a foreground command reports the stop and returns",
		      reported, "" if reported else "no stopped report (tail: %r)" % sh.log[-200:])
		check("the terminal is the shell's again after the stop",
		      sh.fg_pgrp() == shell_pgrp, "tpgid=%d" % sh.fg_pgrp())
		sh.send("echo TSTPOK=$?\n")
		want = 128 + int(signal.SIGTSTP)
		ok = sh.wait_for(r"TSTPOK=%d" % want, timeout=5)
		check("$? is 128+SIGTSTP after the stop",
		      ok, "" if ok else "no TSTPOK=%d (tail: %r)" % (want, sh.log[-200:]))
		# STILL THERE, which is the other half of the contract: the shell reported a
		# pid and walked away, and `kill -CONT` of it has to be a real way out. Then
		# killed, because nothing else is going to - the shell keeps no job table,
		# and a stopped process outliving this probe would be the probe's own leak.
		stopped_pid = int(m) if m else -1
		alive = False
		if stopped_pid > 0:
			try:
				os.kill(stopped_pid, signal.SIGCONT)
				alive = True
			except (ProcessLookupError, PermissionError):
				alive = False
			try:
				os.kill(stopped_pid, signal.SIGKILL)
			except (ProcessLookupError, PermissionError):
				pass
		check("the stopped process is alive and `kill -CONT` reaches it",
		      alive, "pid=%d" % stopped_pid)
	finally:
		sh.close()

	failed = [n for n, ok, _ in results if not ok]
	print()
	print("%d/%d checks pass" % (len(results) - len(failed), len(results)))
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
