"""The latency oracle's profile of a late reply: perf of the app's main thread."""
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import microide_qa as q


def _perf_usable() -> bool:
    if shutil.which("perf") is None:
        return False
    try:
        return int(Path("/proc/sys/kernel/perf_event_paranoid").read_text()) <= 1
    except (OSError, ValueError):
        return False


class _FakeApp:
    """What MainThreadProfiler reads of an App: its process, pid, work tree, binary."""

    def __init__(self, proc: subprocess.Popen, work: Path, recording: bool = False) -> None:
        self.proc, self.pid, self.work, self.recording = proc, proc.pid, work, recording
        self.binary = Path(sys.executable)


class MainThreadProfilerTests(unittest.TestCase):
    @unittest.skipUnless(_perf_usable(), "perf cannot profile our own processes here")
    def test_a_busy_main_thread_reports_where_its_cpu_went(self) -> None:
        busy = subprocess.Popen([sys.executable, "-c", "while True: sum(range(1000))"])
        try:
            with tempfile.TemporaryDirectory() as work:
                profiler = q.MainThreadProfiler.start(_FakeApp(busy, Path(work)))
                self.assertIsNotNone(profiler)
                time.sleep(1.5)
                report = profiler.finish()
                self.assertIn("Samples", report)
                self.assertFalse((Path(work) / "latency.perf.data").exists(), "the data file is removed")
        finally:
            busy.kill()
            busy.wait()

    def test_no_profile_under_rr(self) -> None:
        # The recorder owns the tracee's counters; the recording replays the turn.
        sleeper = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(5)"])
        try:
            with tempfile.TemporaryDirectory() as work:
                self.assertIsNone(q.MainThreadProfiler.start(_FakeApp(sleeper, Path(work), recording=True)))
        finally:
            sleeper.kill()
            sleeper.wait()


if __name__ == "__main__":
    unittest.main()
