#!/usr/bin/env python3
"""JobLoad measures the cores the soak's jobs use (their sessions' CPU time):
the 1.5-cores-per-job estimate it replaces booked half the soak's own load as
someone else's, and the runner stopped starting jobs on a machine running only
the soak."""
from __future__ import annotations

import subprocess
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import soak  # noqa: E402


class JobLoadTests(unittest.TestCase):
    def test_a_busy_session_measures_about_one_core(self) -> None:
        spin = subprocess.Popen([sys.executable, "-c", "import time\nt=time.time()\nwhile time.time()-t<4: pass"],
                                start_new_session=True)
        try:
            load = soak.JobLoad()
            load.WINDOW = 0.0
            self.assertIsNone(load.sample({spin.pid}), "the first sample has nothing to compare with")
            time.sleep(1.5)
            cores = load.sample({spin.pid})
            self.assertIsNotNone(cores)
            self.assertGreater(cores, 0.6)
            self.assertLess(cores, 1.4)
        finally:
            spin.kill()
            spin.wait()

    def test_a_session_measures_its_resident_memory(self) -> None:
        # The memory budget (soak.memory_jobs) reads what the jobs hold: systemd-oomd
        # killed the runner at a 53.6 GB peak when only the cores were budgeted.
        holder = subprocess.Popen([sys.executable, "-c", "b = bytearray(200 * 2**20)\nimport time\ntime.sleep(5)"],
                                  start_new_session=True)
        try:
            time.sleep(1.0)
            load = soak.JobLoad()
            load.sample({holder.pid})
            self.assertIsNotNone(load.rss)
            self.assertGreater(load.rss, 150 * 2**20)
        finally:
            holder.kill()
            holder.wait()

    def test_a_session_not_listed_is_not_counted(self) -> None:
        load = soak.JobLoad()
        load.WINDOW = 0.0
        load.sample({999_999_999})
        time.sleep(0.2)
        self.assertEqual(load.sample({999_999_999}), 0.0)


if __name__ == "__main__":
    unittest.main()
