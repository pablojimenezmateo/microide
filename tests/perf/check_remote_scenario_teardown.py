#!/usr/bin/env python3
"""A remote perf scenario leaves no microide-server and no temp tree behind.

Each scenario runs in a forked child that leaves with _exit (ScenarioProcessIsolation),
so nothing a scenario starts may rely on a static destructor to stop it: the
microide-server daemon of the remote scenarios was such a static, and every run
of one left a live daemon and ~60 MB under /tmp (35 daemons and 2 GB of tmpfs in
one day of A/B runs, 2026-11-23).

Usage: check_remote_scenario_teardown.py PATH/TO/microide_perf
"""

import os
import pathlib
import subprocess
import sys
import tempfile
import time


def live_processes_mentioning(text):
    found = []
    for proc in pathlib.Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            cmdline = (proc / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        if text in cmdline:
            found.append(f"{proc.name}: {cmdline.strip()}")
    return found


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="mqa-perf-teardown.") as tmp:
        env = dict(os.environ, TMPDIR=tmp)
        done = subprocess.run([binary, "--scenarios=remote_first_prompt", "--iterations=1"], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
        left_dirs = sorted(p.name for p in pathlib.Path(tmp).glob("mip-perf.*"))
        # `microide-server stop` asks the daemon to go and returns: give it a moment
        # to finish exiting before calling it left behind.
        deadline = time.monotonic() + 5
        left_procs = live_processes_mentioning(tmp)
        while left_procs and time.monotonic() < deadline:
            time.sleep(0.05)
            left_procs = live_processes_mentioning(tmp)
        # Whatever the check finds, it must not leak them itself.
        for line in left_procs:
            pid = int(line.split(":", 1)[0])
            try:
                os.kill(pid, 15)
            except OSError:
                pass
        if done.returncode != 0:
            print(done.stdout[-4000:])
            print(f"microide_perf exited {done.returncode}")
            return 1
        if "remote_first_prompt" not in done.stdout:
            print(done.stdout[-4000:])
            print("the scenario did not run: nothing was checked")
            return 1
        if left_dirs or left_procs:
            print("left behind after the scenario:")
            for name in left_dirs:
                print("  dir", name)
            for line in left_procs:
                print("  process", line[:200])
            return 1
    print("remote_first_prompt left nothing behind")
    return 0


if __name__ == "__main__":
    sys.exit(main())
