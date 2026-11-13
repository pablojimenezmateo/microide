#!/usr/bin/env python3
"""JobLoad measures the cores the soak's jobs use (their sessions' CPU time):
the 1.5-cores-per-job estimate it replaces booked half the soak's own load as
someone else's, and the runner stopped starting jobs on a machine running only
the soak."""
from __future__ import annotations

import os
import subprocess
import sys
import time
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import soak  # noqa: E402


class JobLoadTests(unittest.TestCase):
    def test_a_busy_session_measures_about_one_core(self) -> None:
        spin = subprocess.Popen([sys.executable, "-c", "import time\nt=time.time()\nwhile time.time()-t<4: pass"],
                                start_new_session=True)
        try:
            def cpu_seconds() -> float:
                fields = Path(f"/proc/{spin.pid}/stat").read_text().rsplit(")", 1)[1].split()
                return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")

            load = soak.JobLoad()
            load.WINDOW = 0.0
            self.assertIsNone(load.sample({spin.pid}), "the first sample has nothing to compare with")
            cpu0, wall0 = cpu_seconds(), time.monotonic()
            time.sleep(1.5)
            cores = load.sample({spin.pid})
            got = (cpu_seconds() - cpu0) / (time.monotonic() - wall0)
            self.assertIsNotNone(cores)
            # What the spinner was actually given, not one whole core: on a machine
            # with every core taken (the soak's own, at load 48 on 16) it gets a
            # fraction, and the measurement has to say so.
            self.assertGreater(got, 0.05, "the spinner ran")
            self.assertAlmostEqual(cores, got, delta=max(0.15, 0.25 * got))
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


class BusyReasonTests(unittest.TestCase):
    def test_a_lane_running_does_not_stop_the_soak(self) -> None:
        # The soak runs at the lowest priority, so a lane takes the cores first;
        # only the load (a lane's included) keeps new jobs from starting.
        lane = subprocess.Popen(["bash", "-c", "exec -a 'bash tools/run-checks.sh qa' sleep 30"])
        try:
            with mock.patch.object(soak.os, "getloadavg", return_value=(0.5, 0.5, 0.5)), \
                 mock.patch.object(soak, "PAUSE", Path("/nonexistent/PAUSE")):
                self.assertIsNone(soak.busy_reason(0.0))
        finally:
            lane.kill()
            lane.wait()

    def test_a_busy_machine_still_does(self) -> None:
        cores = os.cpu_count() or 4
        with mock.patch.object(soak.os, "getloadavg", return_value=(cores * 0.9, 0.0, 0.0)), \
             mock.patch.object(soak, "PAUSE", Path("/nonexistent/PAUSE")):
            self.assertIn("busy", soak.busy_reason(0.0) or "")


class OthersTests(unittest.TestCase):
    def test_others_are_the_machine_less_the_soak_not_the_load_average(self) -> None:
        # Two jobs just ended: their CPU is still in the minute-long load average
        # (11 here), and no longer in the soak's own measurement. Counted from
        # the load average it was "someone else's" and the budget fell to zero.
        load = soak.JobLoad()
        load.cores, load.busy = 2.0, 3.0
        self.assertAlmostEqual(load.others(11.0), 1.0)

    def test_before_a_measurement_the_load_average_stands_in(self) -> None:
        load = soak.JobLoad()
        self.assertAlmostEqual(load.others(5.0), 5.0)

    def test_the_machine_ticks_read_this_machine(self) -> None:
        busy, total = soak.JobLoad.machine_ticks()
        self.assertGreater(total, busy)
        self.assertGreater(busy, 0)

    def test_a_busy_gate_reads_the_measured_others(self) -> None:
        cores = os.cpu_count() or 4
        with mock.patch.object(soak.os, "getloadavg", return_value=(cores * 0.9, 0.0, 0.0)), \
             mock.patch.object(soak, "PAUSE", Path("/nonexistent/PAUSE")):
            self.assertIsNone(soak.busy_reason(0.0, others=0.5), "a lagging load average is not a busy machine")


if __name__ == "__main__":
    unittest.main()
