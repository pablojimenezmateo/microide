"""A fix no replay can check is verified by the soak it survives, and an open
finding the soak stopped showing is quiet -- neither by a replay that never
showed the defect in the first place."""
import importlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import exposure as ex  # noqa: E402

CELL = "editing/default/1440x900@1"


def runs(job: str, times, sha: str = "old") -> list[dict]:
    return [{"t": float(t), "job": job, "sha": sha, "steps": 1500} for t in times]


class ExposureTests(unittest.TestCase):
    def test_the_rate_is_sightings_over_the_runs_of_the_jobs_that_showed_it(self):
        exposure = ex.Exposure(runs("editing", range(0, 1000, 10)) + runs("layout", range(0, 1000, 10)))
        sightings = [(200.0, CELL, "old"), (500.0, CELL, "old")]
        # 51 editing runs up to the last sighting; layout never showed it.
        self.assertAlmostEqual(exposure.rate(sightings), 2 / 51)

    def test_quiet_counts_only_later_runs_of_those_jobs(self):
        exposure = ex.Exposure(runs("editing", range(0, 1000, 10)) + runs("layout", range(0, 100000, 10)))
        quiet = exposure.quiet([(200.0, CELL, "old"), (500.0, CELL, "old")])
        self.assertEqual(quiet.runs, 49)
        self.assertAlmostEqual(quiet.expected, 49 * 2 / 51)
        self.assertLess(quiet.expected, ex.QUIET_EXPECTED)

    def test_a_single_old_sighting_needs_as_many_quiet_runs_as_it_took_to_show(self):
        exposure = ex.Exposure(runs("editing", range(0, 40000, 10)))
        quiet = exposure.quiet([(9990.0, CELL, "old")])
        self.assertAlmostEqual(quiet.rate, 1 / 1000)
        self.assertAlmostEqual(quiet.expected, 3.0, places=1)

    def test_a_fix_counts_only_runs_on_builds_that_have_it(self):
        exposure = ex.Exposure(runs("editing", range(0, 100)) + runs("editing", range(100, 200), sha="fixed"))
        quiet = exposure.quiet([(10.0, CELL, "old"), (20.0, CELL, "old")], since=0.0,
                               on_build=lambda sha: sha == "fixed")
        self.assertEqual(quiet.runs, 100)

    def test_a_run_that_took_no_step_exposed_nothing(self):
        dead = [{"t": float(t), "job": "editing", "sha": "old", "steps": 0} for t in range(1000, 2000, 10)]
        exposure = ex.Exposure(runs("editing", range(0, 1000, 10)) + dead)
        self.assertEqual(exposure.quiet([(500.0, CELL, "old")]).runs, 49)

    def test_no_sighting_or_no_run_is_no_rate(self):
        self.assertIsNone(ex.Exposure([]).rate([(1.0, CELL, "old")]))
        self.assertIsNone(ex.Exposure(runs("editing", [1])).quiet([]))


class LedgerVerdictTests(unittest.TestCase):
    """Against a soak directory of its own: the ledger, the run log, no git."""

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
        # "fix" is in the builds named fixed*, in no other.
        contains = mock.patch.object(self.soak, "has_commit", lambda fix, commit: commit.startswith("fixed"))
        contains.start()
        self.addCleanup(contains.stop)
        self.book = self.soak.ledger()

    def write_runs(self, rows: list[dict]) -> None:
        self.soak.RUN_LOG.write_text("".join(json.dumps(row) + "\n" for row in rows))
        self.soak._EXPOSURE = None

    def sight(self, sig: str, when: float, commit: str = "old") -> None:
        event = "seen" if sig in self.book.entries() else "new"
        with mock.patch("ledger.time.time", return_value=when):
            self.book.append(sig, event, kind="data", label="data: lost", commit=commit, cell=CELL)

    def test_a_fix_no_replay_reproduced_waits_for_exposure(self):
        for when in (100.0, 200.0, 300.0, 400.0):
            self.sight("flaky", when)
        self.book.append("flaky", "checked", rate=0.0, runs=12, deterministic=False)
        self.book.append("flaky", "fixed", commit="fix")
        # 41 runs up to the last sighting: one sighting in ten runs.
        before = runs("editing", range(0, 410, 10))
        self.write_runs(before + runs("editing", range(1000, 1200, 10), sha="fixed1"))
        self.assertEqual(list(self.soak.fixes_to_verify({"good": {"default": {"sha": "fixed1"}}})), [])
        log: list[str] = []
        self.soak.verify_by_exposure(log.append)
        self.assertEqual(self.book.entries()["flaky"].status, "fixed", "20 runs: 2 sightings due, not enough")
        self.write_runs(before + runs("editing", range(1000, 1600, 10), sha="fixed1"))
        self.soak.verify_by_exposure(log.append)
        entry = self.book.entries()["flaky"]
        self.assertEqual((entry.status, entry.verified_by), ("verified", "exposure"))
        self.assertIn("by exposure", log[-1])

    def test_runs_of_builds_without_the_fix_verify_nothing(self):
        for when in (100.0, 200.0, 300.0, 400.0):
            self.sight("flaky", when)
        self.book.append("flaky", "fixed", commit="fix")
        self.write_runs(runs("editing", range(0, 410, 10)) + runs("editing", range(1000, 3000, 10)))
        self.soak.verify_by_exposure(lambda _: None)
        self.assertEqual(self.book.entries()["flaky"].status, "fixed")

    def test_a_sighting_on_a_build_with_the_fix_reopens_it(self):
        self.sight("flaky", 100.0)
        self.book.append("flaky", "fixed", commit="fix")
        self.sight("flaky", 500.0, commit="fixed1")
        self.assertEqual(self.book.entries()["flaky"].status, "reopened")

    def test_a_finding_a_replay_reproduces_is_verified_by_its_replay(self):
        self.sight("det", 100.0)
        self.book.append("det", "checked", rate=1.0, runs=3, deterministic=True)
        self.book.append("det", "fixed", commit="fix")
        self.write_runs(runs("editing", range(0, 110, 10)) + runs("editing", range(1000, 9000, 10), sha="fixed1"))
        queued = [entry.sig for entry, _ in self.soak.fixes_to_verify({"good": {"default": {"sha": "fixed1"}}})]
        self.assertEqual(queued, ["det"])
        self.soak.verify_by_exposure(lambda _: None)
        self.assertEqual(self.book.entries()["det"].status, "fixed", "its replay decides, not the silence")

    def test_an_open_finding_the_soak_stopped_showing_is_quiet_and_sorts_last(self):
        for when in (100.0, 200.0, 300.0, 400.0):
            self.sight("gone", when)
        self.sight("live", 5000.0)
        self.write_runs(runs("editing", range(0, 5010, 10)))
        entries = self.book.entries()
        self.assertTrue(self.soak.is_quiet(entries["gone"]))
        self.assertFalse(self.soak.is_quiet(entries["live"]))
        ordered = sorted(entries.values(), key=self.soak.priority_key)
        self.assertEqual([entry.sig for entry in ordered], ["live", "gone"])
        # One sighting and it is work again.
        self.sight("gone", 5005.0)
        self.assertFalse(self.soak.is_quiet(self.book.entries()["gone"]))

    def test_the_reproduction_line_says_how_many_replays(self):
        self.sight("flaky", 100.0)
        self.book.append("flaky", "checked", rate=0.0, runs=12, deterministic=False)
        text = self.soak.reproduction_text(self.book.entries()["flaky"])
        self.assertIn("0 of 12 replays", text)
        self.assertIn("will not reproduce", text)


if __name__ == "__main__":
    unittest.main()
