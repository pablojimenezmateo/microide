"""A share of the jobs runs under rr, and a finding that matters keeps the
trace: for a finding no replay of its actions reproduces, the recording is the
reproduction."""
import importlib
import json
import os
import random
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import ledger as ld  # noqa: E402

DATA = {"signature": "d" * 12, "kind": "data", "label": "data: the unsaved buffer a.txt did not come back",
        "detail": "after kill -9: lost", "step": 5, "actions": 9, "state": "s"}
PAINT = {"signature": "p" * 12, "kind": "log", "label": "log: QA-STALE frame=<n> reason=event-partial",
         "detail": "QA-STALE frame=3", "step": 5, "actions": 9, "state": "s"}


class RecordingTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        self.addCleanup(self.dir.cleanup)
        import soak
        self.addCleanup(importlib.reload, soak)
        patcher = mock.patch.dict(os.environ, {"MICROIDE_SOAK_DIR": self.dir.name})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.soak = importlib.reload(soak)
        usable = mock.patch.object(self.soak.recording, "rr_unusable_reason", return_value=None)
        usable.start()
        self.addCleanup(usable.stop)

    def job_type(self, name: str):
        return next(j for j in self.soak.JOB_TYPES if j.name == name)

    def run_with(self, finding: dict, recorded: bool = True) -> tuple["object", Path]:
        """A reaped-looking job directory with one finding, the run's logs and
        (recorded) its traces; returns the job and the finding's evidence."""
        out = self.soak.RUNS / f"run-{random.randrange(10**9)}"
        seed = out / "seed-7"
        evidence = seed / "finding-01-monkey-step-5"
        evidence.mkdir(parents=True)
        (evidence / "replay.json").write_text(json.dumps({"actions": [{"key": "a"}]}))
        (evidence / "log-tail.txt").write_text("tail\n")
        (seed / "run-logs").mkdir()
        (seed / "run-logs" / "microide.log.gz").write_bytes(b"gz" * 500)
        if recorded:
            (seed / "rr" / "launch-0").mkdir(parents=True)
            (seed / "rr" / "launch-0" / "data").write_bytes(b"t" * 4000)
            (evidence / "recording.txt").write_text("trace: rr/launch-0\nmode: plain\n")
        job = self.soak.Job(self.job_type("default"), 7, out, ["python3", "run.py", "monkey", "--seed", "7"],
                            "default/default+rr/1440x900@1", 0.0, SimpleNamespace(returncode=1), "c0ffee",
                            "/bin/microide", record="1" if recorded else "", slow=1.5)
        return job, evidence

    def summary(self, finding: dict, evidence: Path) -> dict:
        return {"steps": 100, "header": {}, "findings": [{**finding, "evidence": str(evidence)}]}

    def test_only_the_default_build_is_recorded_and_only_a_share_of_it(self):
        rng = random.Random(3)
        self.assertEqual({self.soak.choose_recording(self.job_type("asan"), rng) for _ in range(200)}, {""})
        drawn = [self.soak.choose_recording(self.job_type("default"), rng) for _ in range(4000)]
        share = sum(1 for mode in drawn if mode) / len(drawn)
        self.assertAlmostEqual(share, self.soak.RECORD_FRACTION, delta=0.04)
        self.assertEqual(set(drawn), {"", "1", "chaos"})

    def test_a_recorded_job_gets_a_window_the_software_renderer_can_keep_up_with(self):
        rng = random.Random(5)
        sizes = {self.soak.choose_cell(rng, self.soak.RECORD_MAX_PIXELS)[0] for _ in range(2000)}
        self.assertIn((1440, 900), sizes)
        self.assertTrue(all(w * h <= 1440 * 900 for w, h in sizes), sizes)
        self.assertIn((2560, 1440), {self.soak.choose_cell(rng)[0] for _ in range(2000)})
        state = {"good": {"default": {"sha": "c0ffee", "binary": "/bin/microide"}}}
        with mock.patch.object(self.soak, "choose_recording", return_value="1"), \
                mock.patch.object(self.soak.subprocess, "Popen") as popen:
            for seed in range(60):
                self.soak.start_job(self.job_type("default"), state, random.Random(seed))
                size = popen.call_args.args[0][popen.call_args.args[0].index("--size") + 1]
                width, height = (int(v) for v in size.split("x"))
                self.assertLessEqual(width * height, 1440 * 900)

    def test_a_profile_rr_cannot_follow_is_not_recorded(self):
        with mock.patch.object(self.soak, "UNRECORDED_PROFILES", frozenset({"terminal"})):
            drawn = {self.soak.choose_recording(self.job_type("terminal"), random.Random(1)) for _ in range(200)}
        self.assertEqual(drawn, {""})
        drawn = {self.soak.choose_recording(self.job_type("terminal"), random.Random(1)) for _ in range(200)}
        self.assertIn("1", drawn)

    def test_a_run_that_lost_its_recorder_is_counted_not_booked(self):
        job, evidence = self.run_with(DATA)
        lost = {"signature": "r" * 12, "kind": "recorder", "label": "recorder: rr stopped recording",
                "detail": "rr stopped recording after monkey step 9: Assertion ... 'tee' ... unsupported by rr",
                "evidence": str(evidence)}
        (job.out / "seed-7" / "findings.json").write_text(json.dumps({"steps": 9, "findings": [lost]}))
        self.soak.reap_job(job, {}, lambda _: None)
        self.assertEqual(self.soak.ledger().entries(), {})
        (row,) = [json.loads(line) for line in self.soak.RUN_LOG.read_text().splitlines()]
        self.assertIn("unsupported by rr", row["recorder_lost"])

    def test_nothing_is_recorded_where_rr_cannot_run(self):
        with mock.patch.object(self.soak.recording, "rr_unusable_reason", return_value="no rr"):
            drawn = {self.soak.choose_recording(self.job_type("default"), random.Random(1)) for _ in range(200)}
        self.assertEqual(drawn, {""})

    def test_a_recorded_job_is_told_so_and_given_time(self):
        state = {"good": {"default": {"sha": "c0ffee", "binary": "/bin/microide"}}}
        with mock.patch.object(self.soak, "choose_recording", return_value="chaos"), \
                mock.patch.object(self.soak.subprocess, "Popen") as popen:
            job = self.soak.start_job(self.job_type("editing"), state, random.Random(1))
        env = popen.call_args.kwargs["env"]
        self.assertEqual((env["MQA_RR"], env["MQA_SLOW_FACTOR"]), ("chaos", "5.0"))
        self.assertIn("editing/default+rr-chaos/", job.cell)
        self.assertFalse(self.soak.cell_is_tiny(job.cell) and "1440x900@1" in job.cell)
        with mock.patch.object(self.soak, "choose_recording", return_value=""), \
                mock.patch.dict(os.environ, {"MQA_RR": "1"}), \
                mock.patch.object(self.soak.subprocess, "Popen") as popen:
            job = self.soak.start_job(self.job_type("editing"), state, random.Random(1))
        self.assertNotIn("MQA_RR", popen.call_args.kwargs["env"], "the runner's own environment does not leak in")
        self.assertIn("editing/default/", job.cell)

    def test_a_severe_finding_keeps_the_trace_and_the_whole_logs(self):
        job, evidence = self.run_with(DATA)
        bundle = self.soak.make_bundle(job, self.summary(DATA, evidence), {**DATA, "evidence": str(evidence)})
        self.assertTrue((bundle / "evidence" / "rr" / "launch-0" / "data").is_file())
        self.assertTrue((bundle / "evidence" / "run-logs" / "microide.log.gz").is_file())
        self.assertTrue(self.soak.has_recording(bundle))
        meta = json.loads((bundle / "meta.json").read_text())
        self.assertEqual((meta["record"], meta["slow"]), ("1", 1.5))
        text = (bundle / "FINDING.md").read_text()
        self.assertIn("## Recording (rr)", text)
        self.assertIn("the finding was reported in `rr/launch-0`", text)
        self.assertIn("rr replay -g <event>", text)
        self.assertIn("evidence/run-logs", text)
        self.assertIn("run.py --binary build/microide/microide monkey --seed 7", text)

    def test_stale_paint_keeps_the_logs_and_not_the_trace(self):
        job, evidence = self.run_with(PAINT)
        bundle = self.soak.make_bundle(job, self.summary(PAINT, evidence), {**PAINT, "evidence": str(evidence)})
        self.assertFalse((bundle / "evidence" / "rr").exists())
        self.assertTrue((bundle / "evidence" / "run-logs").is_dir())
        self.assertNotIn("## Recording", (bundle / "FINDING.md").read_text())

    def test_two_findings_of_one_run_share_one_copy_of_its_trace(self):
        job, evidence = self.run_with(DATA)
        other = {**DATA, "signature": "e" * 12, "kind": "undo", "label": "undo: changed the text of a.txt"}
        first = self.soak.make_bundle(job, self.summary(DATA, evidence), {**DATA, "evidence": str(evidence)})
        second = self.soak.make_bundle(job, self.summary(other, evidence), {**other, "evidence": str(evidence)})
        a, b = (bundle / "evidence" / "rr" / "launch-0" / "data" for bundle in (first, second))
        self.assertEqual(a.stat().st_ino, b.stat().st_ino)
        # ... and the budget counts it once.
        self.assertLess(self.soak.disk_usage(self.soak.FINDINGS), 2 * 4000 + 20000)
        self.assertEqual(self.soak.disk_usage_of([a.parent, b.parent]), 4000)

    def test_the_first_recorded_sighting_of_an_unrecorded_finding_is_kept(self):
        job, evidence = self.run_with(DATA, recorded=False)
        finding = {**DATA, "evidence": str(evidence)}
        bundle = self.soak.make_bundle(job, self.summary(DATA, evidence), finding)
        self.soak.ledger().append(DATA["signature"], "new", kind="data", label=DATA["label"], commit="c0ffee",
                                  cell=job.cell)
        entry = self.soak.ledger().entries()[DATA["signature"]]
        # Same build, nothing new to say: not kept.
        plain, plain_evidence = self.run_with(DATA, recorded=False)
        self.soak.keep_sighting(plain, {**DATA, "evidence": str(plain_evidence)}, entry)
        self.assertFalse((bundle / "sightings").exists())
        recorded, recorded_evidence = self.run_with(DATA)
        self.soak.keep_sighting(recorded, {**DATA, "evidence": str(recorded_evidence)}, entry)
        (kept,) = (bundle / "sightings").iterdir()
        self.assertTrue((kept / "rr" / "launch-0").is_dir())
        self.assertEqual(json.loads((kept / "sighting.json").read_text())["record"], "1")
        self.assertIn(f"sightings/{kept.name}/rr/", (bundle / "FINDING.md").read_text())
        # A second recording says nothing the first does not.
        again, again_evidence = self.run_with(DATA)
        self.soak.keep_sighting(again, {**DATA, "evidence": str(again_evidence)}, entry)
        self.assertEqual(len(list((bundle / "sightings").iterdir())), 1)

    def test_newer_unrecorded_sightings_do_not_push_the_only_recording_out(self):
        base = Path(self.dir.name) / "sightings"
        names = [base / f"2026111{i}-000000-sha-cell" for i in range(1, 7)]
        for path in names:
            path.mkdir(parents=True)
        (names[1] / "rr").mkdir()
        drop = self.soak.sightings_to_drop(names, bundle_recorded=False)
        self.assertEqual(drop, [names[0], names[2]])
        # The bundle's own evidence has one: the usual rule.
        self.assertEqual(self.soak.sightings_to_drop(names, bundle_recorded=True), names[:3])

    def test_the_recording_budget_drops_the_oldest_spare_first(self):
        def holder(sig: str, where: str, age: int) -> Path:
            path = self.soak.FINDINGS / sig / where / "rr"
            (path / "launch-0").mkdir(parents=True)
            (path / "launch-0" / "data").write_bytes(os.urandom(1000) * 400)
            os.utime(path, (1000 + age, 1000 + age))
            return path
        only_old = holder("a" * 12, "evidence", 0)
        spare_old = holder("b" * 12, "evidence", 1)
        spare_new = holder("b" * 12, "sightings/s1", 2)
        log: list[str] = []
        with mock.patch.object(self.soak, "RECORDING_BUDGET_GB", 1.0e6 / 1e9):  # room for two and a half
            self.soak.enforce_recording_budget(log.append)
        self.assertTrue(only_old.is_dir(), "a finding's only recording outlives another's second")
        self.assertFalse(spare_old.exists())
        self.assertTrue(spare_new.is_dir())
        self.assertIn("dropped the 1 oldest", log[-1])

    def test_one_defect_under_many_signatures_reads_as_one_family(self):
        labels = [f"data: the unsaved buffer {name} did not come back after a kill -<n> (the crash-safety flush)"
                  for name in ("rtl.txt", "名前 with spaces.txt", "Untitled-<n>")]
        self.assertEqual(len({self.soak.family("data", label) for label in labels}), 1)
        self.assertNotEqual(self.soak.family("data", labels[0]),
                            self.soak.family("data", "data: the clean buffer of rtl.txt differs from its file"))
        self.assertEqual(self.soak.family("overlap", "overlap: a overlaps b"), "overlap: a overlaps b")

    def test_the_rotation_follows_what_was_lost_not_how_many_signatures(self):
        state: dict = {}
        book = self.soak.ledger()
        for finding in (DATA, PAINT):
            job, evidence = self.run_with(finding, recorded=False)
            job.type = self.job_type("editing" if finding is DATA else "layout")
            (job.out / "seed-7" / "findings.json").write_text(json.dumps(self.summary(finding, evidence)))
            self.soak.reap_job(job, state, lambda _: None)
        self.assertEqual(state["stats"]["editing"]["new_weighted"], 1.0)
        self.assertEqual(state["stats"]["layout"]["new_weighted"], 0.05)
        self.assertEqual(set(book.entries()), {DATA["signature"], PAINT["signature"]})


if __name__ == "__main__":
    unittest.main()
