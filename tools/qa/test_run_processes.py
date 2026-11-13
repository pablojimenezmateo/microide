#!/usr/bin/env python3
"""What an app started dies with its run (microide_qa.kill_run_processes): a
debug session's processes left the app's process group, outlived every run, and
999 of them sat orphaned on the soak machine."""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import microide_qa as q  # noqa: E402


def _gone(pid: int) -> bool:
    try:
        return Path(f"/proc/{pid}/stat").read_text().split()[2] == "Z"
    except (FileNotFoundError, ProcessLookupError):
        return True


class RunProcessesTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.work = Path(scratch.name) / "mqa-run"
        self.other = Path(scratch.name) / "mqa-other"
        self.started: list[subprocess.Popen] = []

    def tearDown(self) -> None:
        for proc in self.started:
            proc.kill()
            proc.wait()

    def _start(self, work: Path, **kwargs) -> int:
        """A process of the run whose work tree is `work`: started with the marker
        the driver gives the app, which every descendant inherits."""
        env = {**os.environ, q.RUN_MARKER: str(work)}
        proc = subprocess.Popen(["sleep", "60"], env=env, **kwargs)
        self.started.append(proc)
        return proc.pid

    def test_the_runs_processes_die_wherever_they_went(self) -> None:
        in_group = self._start(self.work)
        # gdb runs its program in a process group of its own; a terminal's shell
        # starts a session: killing the app's group reaches neither.
        own_session = self._start(self.work, start_new_session=True)
        another_run = self._start(self.other)
        # A marker that only STARTS with this run's path is another run's.
        prefixed = self._start(Path(str(self.work) + "-2"))
        self.assertEqual(q.kill_run_processes(self.work), 2)
        deadline = time.monotonic() + 5
        while not (_gone(in_group) and _gone(own_session)) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(_gone(in_group) and _gone(own_session), "the run's processes outlived it")
        self.assertFalse(_gone(another_run) or _gone(prefixed), "another run's processes were killed")


if __name__ == "__main__":
    unittest.main()
