"""The soak and the fixing agent take turns on the machine: a phase pause stops
new jobs AND builds, `pause --wait` returns once the running jobs are done, and
`resume` lifts only a phase pause -- never one that waits for a person."""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import soak  # noqa: E402


class PhasePauseTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        patch = mock.patch.object(soak, "PAUSE", Path(self.dir.name) / "PAUSE")
        patch.start()
        self.addCleanup(patch.stop)

    def test_a_phase_pause_stops_jobs_and_resume_lifts_it(self):
        self.assertTrue(soak.pause_for_phase("fixing"))
        self.assertEqual(soak.busy_reason(0.0, 0.0), "paused (build/soak/PAUSE)")
        self.assertTrue(soak.build_paused())
        self.assertTrue(soak.resume_from_phase())
        self.assertFalse(soak.PAUSE.exists())
        self.assertFalse(soak.build_paused())

    def test_a_containment_pause_is_neither_replaced_nor_lifted(self):
        soak.PAUSE.write_text("CONTAINMENT BREACH: QA git activity reached the checkout\n")
        self.assertFalse(soak.pause_for_phase("fixing"))
        self.assertFalse(soak.resume_from_phase())
        self.assertIn("CONTAINMENT", soak.PAUSE.read_text())

    def test_wait_returns_once_no_job_runs(self):
        states = iter([{"current": ["a", "b"]}, {"current": ["b"]}, {"current": []}])
        with mock.patch.object(soak, "load_state", side_effect=lambda: next(states)), \
                mock.patch.object(soak.time, "sleep"):
            self.assertTrue(soak.wait_for_idle(timeout=60.0, poll=1.0))

    def test_wait_gives_up_at_its_timeout(self):
        clock = iter(range(0, 1000, 30))
        with mock.patch.object(soak, "load_state", return_value={"current": ["a"]}), \
                mock.patch.object(soak.time, "sleep"), \
                mock.patch.object(soak.time, "monotonic", side_effect=lambda: float(next(clock))):
            self.assertFalse(soak.wait_for_idle(timeout=60.0, poll=1.0))


if __name__ == "__main__":
    unittest.main()
