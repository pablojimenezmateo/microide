"""A job that ran nothing is a finding. "No findings" is what a healthy cell
reports too, and a cell dead at startup looked exactly like one."""
import importlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))


class DeadJobTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        self.addCleanup(self.dir.cleanup)
        import soak
        # Cleanups run last-registered first: the environment is restored, THEN
        # the module is reloaded onto it (the other tests' soak directory).
        self.addCleanup(importlib.reload, soak)
        patcher = mock.patch.dict(os.environ, {"MICROIDE_SOAK_DIR": self.dir.name})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.soak = importlib.reload(soak)
        self.log: list[str] = []

    def job(self, name: str = "tsan", record: str = "") -> "object":
        job_type = next(j for j in self.soak.JOB_TYPES if j.name == name)
        out = self.soak.RUNS / f"run-{len(list(self.soak.RUNS.glob('*'))) if self.soak.RUNS.exists() else 0}"
        (out / "seed-7" / "startup").mkdir(parents=True)
        (out / "driver.log").write_text("QaFailure: microide exited at startup\n")
        (out / "seed-7" / "startup" / "microide.log").write_text(
            "ThreadSanitizer: failed to read suppressions file 'tests/tsan.supp'\n")
        return self.soak.Job(job_type, 7, out, ["python3", "tools/qa/run.py", "monkey", "--seed", "7"],
                             f"{name}/{job_type.build}/1440x900@1", 0.0, SimpleNamespace(returncode=1),
                             "c0ffee", "/bin/microide", record=record)

    def test_a_job_with_no_result_is_recorded_with_its_logs(self):
        self.soak.reap_job(self.job(), {}, self.log.append)
        (entry,) = self.soak.ledger().entries().values()
        self.assertEqual((entry.kind, entry.status, self.soak.TIER_NAMES[self.soak.impact_tier(entry)]),
                         ("job", "new", "severe"))
        self.assertIn("a tsan job on the tsan build ended without a step", entry.label)
        bundle = self.soak.FINDINGS / entry.sig
        self.assertIn("suppressions", (bundle / "evidence" / "microide.log").read_text())
        finding = (bundle / "FINDING.md").read_text()
        self.assertIn("failed to read suppressions file", finding)
        self.assertNotIn("Full replay", finding, "there is no replay of a job that never started")
        self.assertTrue(any("DEAD" in line for line in self.log))

    def test_the_second_dead_run_is_a_sighting_of_the_same_finding(self):
        self.soak.reap_job(self.job(), {}, self.log.append)
        self.soak.reap_job(self.job(), {}, self.log.append)
        (entry,) = self.soak.ledger().entries().values()
        self.assertEqual(len(entry.sightings), 2)

    def test_a_run_that_took_steps_is_not_dead(self):
        job = self.job("default")
        (job.out / "seed-7" / "findings.json").write_text(json.dumps({"steps": 1500, "findings": []}))
        self.soak.reap_job(job, {}, self.log.append)
        self.assertEqual(self.soak.ledger().entries(), {})

    def test_a_timeout_and_a_recorded_job_are_findings_of_their_own(self):
        self.soak.reap_job(self.job("default"), {}, self.log.append)
        self.soak.reap_job(self.job("default"), {}, self.log.append, timed_out=True)
        self.soak.reap_job(self.job("default", record="1"), {}, self.log.append)
        labels = sorted(entry.label for entry in self.soak.ledger().entries().values())
        self.assertEqual(len(labels), 3)
        self.assertTrue(any("hard timeout" in label for label in labels))
        self.assertTrue(any("under rr" in label for label in labels))

    def test_a_dead_run_is_no_exposure(self):
        self.soak.reap_job(self.job(), {}, self.log.append)
        (row,) = [json.loads(line) for line in self.soak.RUN_LOG.read_text().splitlines()]
        self.assertEqual(row["steps"], 0)
        self.assertEqual(self.soak.current_exposure().runs_until(["tsan"], 1e18), 0)


class RunnerHealthTests(unittest.TestCase):
    """`health` on the runner itself: its code, and what it leaves behind."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        self.addCleanup(self.dir.cleanup)
        import soak
        self.addCleanup(importlib.reload, soak)
        patcher = mock.patch.dict(os.environ, {"MICROIDE_SOAK_DIR": self.dir.name})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.soak = importlib.reload(soak)

    def health(self) -> tuple[int, str]:
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out), \
             mock.patch.object(self.soak.recording, "rr_unusable_reason", return_value=None):
            code = self.soak.cmd_health(SimpleNamespace(hours=24.0))
        return code, out.getvalue()

    def test_a_runner_on_older_code_than_the_checkout_is_said_so(self):
        # The runner is one long-lived process: a change to tools/soak does
        # nothing until the service is restarted, and nothing said it had not been.
        self.soak.save_state({"runner_code": self.soak.runner_code(), "good": {}})
        self.assertNotIn("runner: STALE", self.health()[1])
        self.soak.save_state({"runner_code": "0123456789abcdef", "good": {}})
        code, text = self.health()
        self.assertIn("runner: STALE", text)
        self.assertIn("systemctl --user restart microide-soak", text)
        self.assertEqual(code, 1)

    def test_a_state_from_before_the_digest_is_not_called_stale(self):
        self.soak.save_state({"good": {}})
        self.assertNotIn("runner: STALE", self.health()[1])

    def test_the_digest_follows_the_sources(self):
        before = self.soak.runner_code()
        self.assertEqual(before, self.soak.runner_code())
        with mock.patch.object(self.soak, "HERE", Path(self.dir.name)):
            self.assertNotEqual(self.soak.runner_code(), before)

    def test_run_directories_nobody_owns_are_counted(self):
        self.soak.save_state({"good": {}, "current": ["a/default/1x1@1"]})
        for index in range(2 * self.soak.max_jobs(os.cpu_count() or 4) + 2):
            (self.soak.RUNS / f"20261114-000000-diff-{index}").mkdir(parents=True)
        code, text = self.health()
        self.assertIn("run directories no running job owns", text)
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
