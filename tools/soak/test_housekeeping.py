"""What the soak keeps on disk: corpus prefixes it can use, and a fixed
finding's record without the evidence the fix made moot."""
import importlib
import json
import os
import random
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import monkey_model as mm  # noqa: E402
import soak  # noqa: E402


def scratch_dir(test: unittest.TestCase) -> Path:
    """A directory of the test's own, gone when it ends (these tests left one in
    /tmp per run: 1274 of them on the soak machine)."""
    scratch = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
    test.addCleanup(scratch.cleanup)
    return Path(scratch.name)


def scratch_soak(test: unittest.TestCase):
    """The soak module on a soak directory of the test's own, and back on the
    other tests' when it ends. Setting MICROIDE_SOAK_DIR for good left every
    later test in the process on this one's directory -- whose disk the busy
    gate then read (test_job_load)."""
    patcher = mock.patch.dict(os.environ, {"MICROIDE_SOAK_DIR": str(scratch_dir(test))})
    # LIFO: the environment goes back first, then the module is reloaded onto it.
    test.addCleanup(importlib.reload, soak)
    patcher.start()
    test.addCleanup(patcher.stop)
    return importlib.reload(soak)

HEADER = {"fixture": "readme", "size": [640, 480]}


def actions(n: int) -> list[dict]:
    return [{"key": "a"}] * n


class CorpusTests(unittest.TestCase):
    def setUp(self):
        self.dir = scratch_dir(self)
        self.corpus = mm.Corpus(self.dir)

    def test_a_prefix_too_long_to_pick_is_not_saved(self):
        self.assertTrue(self.corpus.save("short", HEADER, actions(mm.MAX_PREFIX_ACTIONS)))
        self.assertFalse(self.corpus.save("long", HEADER, actions(mm.MAX_PREFIX_ACTIONS + 1)))
        self.assertEqual(len(list(self.dir.glob("*.json"))), 1)

    def test_every_saved_prefix_can_be_picked(self):
        self.corpus.save("s", HEADER, actions(mm.MAX_PREFIX_ACTIONS))
        record = self.corpus.pick(random.Random(1), mm.Coverage(), HEADER)
        self.assertEqual(record["state"], "s")

    def test_prune_drops_the_prefixes_pick_skips(self):
        self.corpus.save("keep", HEADER, actions(3))
        stale = self.dir / "0123456789abcdef.json"
        stale.write_text(json.dumps({**HEADER, "state": "old", "actions": actions(mm.MAX_PREFIX_ACTIONS + 1)}))
        (self.dir / "broken0000000000.json").write_text("{")
        removed, freed = self.corpus.prune()
        self.assertEqual(removed, 2)
        self.assertGreater(freed, 0)
        self.assertEqual([json.loads(p.read_text())["state"] for p in self.dir.glob("*.json")], ["keep"])


class VerifiedBundleTests(unittest.TestCase):
    def test_a_verified_bundle_keeps_its_record_and_drops_its_evidence(self):
        soak = scratch_soak(self)
        bundle = soak.FINDINGS / "abc123abc123"
        for name in ("evidence/frame.png", "sightings/s1/frame.png", "verify/run0/log.txt"):
            (bundle / name).parent.mkdir(parents=True, exist_ok=True)
            (bundle / name).write_bytes(b"x" * 1000)
        for name in ("FINDING.md", "meta.json", "minimal.json", "replay.json"):
            (bundle / name).write_text("{}")
        freed = soak.prune_verified_bundle(bundle)
        self.assertEqual(freed, 3000)
        self.assertEqual(sorted(p.name for p in bundle.iterdir()),
                         ["FINDING.md", "meta.json", "minimal.json", "replay.json"])


class SightingTests(unittest.TestCase):
    def setUp(self):
        import ledger as ld
        soak = scratch_soak(self)
        self.soak, self.ld = soak, ld
        self.bundle = soak.FINDINGS / "abc123abc123"
        (self.bundle / "evidence").mkdir(parents=True)
        (self.bundle / "FINDING.md").write_text("")
        (self.bundle / "meta.json").write_text(json.dumps({
            "finding": {"signature": "abc123abc123", "kind": "data", "detail": "d", "label": "data: d",
                        "step": 5, "state": "", "actions": 9},
            "cell": "c/d/1x1@1", "sha": "0" * 40, "binary": "", "build": "default", "job": "default",
            "seed": 1, "header": {}}))
        self.out = scratch_dir(self)

    def sighting(self, *files: str, sha: str = "0" * 40) -> None:
        evidence = self.out / "seed-1" / "finding-01-monkey-step-5"
        evidence.mkdir(parents=True, exist_ok=True)
        for name in files:
            (evidence / name).write_bytes(b"x")
        job = self.soak.Job(type=self.soak.JOB_TYPES[0], seed=1, out=self.out, command=[], cell="c/d/1x1@1",
                            started=0.0, proc=None, sha=sha, binary="")
        entry = self.ld.Entry(sig="abc123abc123", status="new")
        self.soak.keep_sighting(job, {"evidence": str(evidence), "detail": "d"}, entry)

    def kept(self) -> list[str]:
        sightings = self.bundle / "sightings"
        return sorted(p.name for s in sightings.iterdir() for p in s.iterdir()) if sightings.is_dir() else []

    def test_a_new_findings_sighting_with_nothing_new_is_not_kept(self):
        self.sighting("now.png")
        self.assertEqual(self.kept(), [])

    def test_a_new_findings_sighting_carrying_kill_state_the_bundle_lacks_is_kept(self):
        self.sighting("now.png", "kill-state.tar.gz")
        self.assertIn("kill-state.tar.gz", self.kept())

    def test_the_first_sighting_on_a_build_the_bundle_has_none_from_is_kept(self):
        # An open finding whose evidence all predates a related fix: the only
        # evidence of what the current code does was deleted with its run
        # (ed121110a40f, seen on 5f577c2a after 3793bc00b, nothing kept).
        self.sighting("now.png", sha="1" * 40)
        self.assertEqual(self.kept(), ["now.png", "sighting.json"])
        self.sighting("now.png", "log-tail.txt", sha="1" * 40)
        self.assertEqual(self.kept(), ["now.png", "sighting.json"])

    def test_kill_state_the_bundle_already_has_is_not_kept_again(self):
        (self.bundle / "evidence" / "kill-state.tar.gz").write_bytes(b"x")
        self.sighting("now.png", "kill-state.tar.gz")
        self.assertEqual(self.kept(), [])


class OrphanRunTests(unittest.TestCase):
    """build/soak/runs holds a directory per running job and nothing else."""

    def setUp(self):
        self.soak = scratch_soak(self)
        self.soak.RUNS.mkdir(parents=True)
        self.logged: list[str] = []
        self.now = 1_000_000.0

    def run_dir(self, name: str, age_hours: float, *files: str) -> Path:
        path = self.soak.RUNS / name
        stamp = self.now - age_hours * 3600
        for relative in files or ("driver.log",):
            (path / relative).parent.mkdir(parents=True, exist_ok=True)
            (path / relative).write_text("x")
        for at in [path, *path.rglob("*")]:
            os.utime(at, (stamp, stamp))
        return path

    def sweep(self, *running: Path) -> int:
        return self.soak.sweep_orphan_runs(set(running), self.logged.append, now=self.now)

    def test_a_directory_left_by_a_runner_that_stopped_goes(self):
        left = self.run_dir("20261114-004407-terminal-1105724772", 80, "driver.log", "fixtures/fx-1/taskflow/a.txt")
        self.assertEqual(self.sweep(), 1)
        self.assertFalse(left.exists())
        self.assertIn("1 run directory no job owns", self.logged[0])

    def test_a_directory_a_later_run_wrote_into_goes_once_it_is_quiet(self):
        # The run ended a day ago; a replayed corpus prefix had the app save into
        # it an hour ago.
        stray = self.run_dir("20261117-053411-diff-1021163423", 1, "fixtures/fx-1/taskflow/qa-diff/v2.right.js")
        self.assertEqual(self.sweep(), 0)
        self.now += 5 * 3600
        self.assertEqual(self.sweep(), 1)
        self.assertFalse(stray.exists())

    def test_a_running_jobs_directory_stays_however_old(self):
        # A check minimizes for up to three hours and may write nothing at the top.
        mine = self.run_dir("check-14cc1f5f0cf0", 6)
        self.assertEqual(self.sweep(mine), 0)
        self.assertTrue(mine.exists())

    def test_a_hunt_run_by_hand_is_not_swept_from_under_it(self):
        hunt = self.run_dir("hunt-abc123abc123", 0.5, "attempt-2/replay/findings.json")
        self.assertEqual(self.sweep(), 0)
        self.assertTrue(hunt.exists())
        self.assertEqual(self.logged, [])


class CoverageFileTests(unittest.TestCase):
    """The coverage file every soak job reads at its start and rewrites at its
    end: it must stay small and must count each run's steps exactly once."""

    def setUp(self):
        self.path = scratch_dir(self) / "coverage.json"

    def run_steps(self, steps):
        coverage = mm.Coverage.load(self.path)
        news = [coverage.record(*step) for step in steps]
        coverage.note("commands", "save")
        coverage.merge_into(self.path)
        return news

    def test_runs_add_up_once_each(self):
        self.run_steps([("s0", "key:a", "s1")])
        news = self.run_steps([("s0", "key:a", "s1"), ("s1", "key:b", "s2")])
        self.assertEqual(news, [(False, False), (True, True)])
        coverage = mm.Coverage.load(self.path)
        self.assertEqual(coverage.pair_count("s0", "key:a"), 2)
        self.assertEqual(coverage.states["s1"], 2)
        self.assertEqual(coverage.commands["save"], 2)

    def test_transitions_and_pairs_are_stored_as_digests(self):
        long_state = "f=editor o=- s=tree p=terminal m=open d=- g=1 r=0 h= t=editor b=editor::caret"
        self.run_steps([(long_state, "key:a", long_state + "x")])
        data = json.loads(self.path.read_text())
        self.assertEqual(data["format"], mm.Coverage.FORMAT)
        for name in ("transitions", "pairs"):
            self.assertTrue(all(len(key) == 16 for key in data[name]), name)

    def test_a_file_from_before_the_digests_keeps_its_counts(self):
        self.path.write_text(json.dumps({"states": {"s1": 3}, "transitions": {"s0\tkey:a\ts1": 3},
                                         "pairs": {"s0\tkey:a": 3}, "commands": {"save": 1}}))
        coverage = mm.Coverage.load(self.path)
        self.assertEqual(coverage.pair_count("s0", "key:a"), 3)
        self.assertEqual(coverage.record("s0", "key:a", "s1"), (False, False))
        coverage.merge_into(self.path)
        self.assertEqual(mm.Coverage.load(self.path).pair_count("s0", "key:a"), 4)

    def test_an_older_driver_merging_into_a_converted_file_adds_up(self):
        self.run_steps([("s0", "key:a", "s1")])
        data = json.loads(self.path.read_text())
        data.pop("format")
        data["pairs"]["s0\tkey:a"] = 5  # what the older driver writes back
        self.path.write_text(json.dumps(data))
        self.assertEqual(mm.Coverage.load(self.path).pair_count("s0", "key:a"), 6)


if __name__ == "__main__":
    unittest.main()


class BinaryPruneTests(unittest.TestCase):
    """Soak 15d0bae54bb7 / 6a8bab5043ad / 89a4437078a2 / ab3bacbd4195 /
    eb4768093ac2: a binary that served for more than six hours went the moment
    the next build replaced it, dated by when it was MADE, under the jobs still
    running it."""

    def _old_binary(self, soak, sha: str) -> Path:
        binary = soak.SOAK / "bin" / "default" / sha / "microide"
        binary.parent.mkdir(parents=True)
        binary.write_text("")
        old = time.time() - 7 * 3600
        os.utime(binary.parent, (old, old))
        return binary

    def test_a_binary_a_job_started_on_lately_is_kept(self):
        soak = scratch_soak(self)
        used = self._old_binary(soak, "aaaaaaaaaaaa")
        unused = self._old_binary(soak, "bbbbbbbbbbbb")
        # A job starts on the old binary just before the next build replaces it.
        state = {"good": {"default": {"sha": "a" * 40, "binary": str(used)}}}
        job_type = next(t for t in soak.JOB_TYPES if t.build == "default")
        with mock.patch.object(soak, "choose_recording", return_value=""), \
                mock.patch.object(soak.subprocess, "Popen") as popen:
            soak.start_job(job_type, state, random.Random(1))
        env = popen.call_args.kwargs["env"]
        self.assertEqual((env["PYTHONUNBUFFERED"], env["PYTHONFAULTHANDLER"]), ("1", "1"),
                         "driver.log keeps what was printed before a kill, and the abort's stack")
        current = {"default": {"sha": "c" * 40, "binary": str(soak.SOAK / "bin" / "default" / "cccccccccccc" / "microide")}}
        soak.prune_binaries(current)
        self.assertTrue(used.exists(), "a job is running it")
        self.assertFalse(unused.parent.exists(), "nothing started on it for six hours")


class BinaryInUseTests(unittest.TestCase):
    """2026-11-26: 11 GB of the soak's 20 under a 15 GB budget were superseded
    builds' binaries, kept two hours after a job last started on them, five
    builds deep on a day of pushes -- while a three-hour check could still lose
    its binary to the same rule. What runs a build is a live process naming it."""

    def _binary(self, soak, sha: str, age_s: float) -> Path:
        binary = soak.SOAK / "bin" / "default" / sha / "microide"
        binary.parent.mkdir(parents=True)
        binary.write_text("")
        then = time.time() - age_s
        os.utime(binary.parent, (then, then))
        return binary

    def test_a_superseded_build_goes_once_no_live_process_names_it(self):
        soak = scratch_soak(self)
        checked = self._binary(soak, "aaaaaaaaaaaa", 7 * 3600)
        idle = self._binary(soak, "bbbbbbbbbbbb", 20 * 60)
        starting = self._binary(soak, "dddddddddddd", 5 * 60)
        current = self._binary(soak, "cccccccccccc", 9 * 3600)
        names = b"\0".join((b"python3", b"run.py", b"--binary", bytes(checked), b"replay", b"/usr/bin/bash"))
        with mock.patch.object(soak, "live_process_names", return_value=names):
            soak.prune_binaries({"default": {"sha": "c" * 40, "binary": str(current)}})
        self.assertTrue(checked.exists(), "a check started seven hours ago still runs it")
        self.assertFalse(idle.parent.exists(), "superseded, and nothing runs it")
        self.assertTrue(starting.exists(), "a job started on it minutes ago may not have spawned yet")
        self.assertTrue(current.exists(), "the current build stays")

    def test_a_directory_whose_name_extends_a_named_one_is_not_kept_by_it(self):
        soak = scratch_soak(self)
        named = self._binary(soak, "aaaaaaaaaaaa", 3600)
        sibling = self._binary(soak, "aaaaaaaaaaa", 3600)
        names = bytes(named)
        with mock.patch.object(soak, "live_process_names", return_value=names):
            soak.prune_binaries({})
        self.assertTrue(named.exists())
        self.assertFalse(sibling.parent.exists(), "a prefix of a named directory is another build")

    def test_live_process_names_holds_this_process_command_line_and_executable(self):
        soak = scratch_soak(self)
        names = soak.live_process_names()
        self.assertIn(os.readlink("/proc/self/exe").encode(), names)
        self.assertIn(Path("/proc/self/cmdline").read_bytes().rstrip(b"\0"), names)


class DeadJobEvidenceTests(unittest.TestCase):
    def test_a_dead_job_says_how_it_ended_and_how_loaded_the_machine_was(self):
        # Soak dd0052b5e422: hard timeout, driver.log empty, nothing to act on.
        soak = scratch_soak(self)
        proc = mock.Mock(returncode=-6)
        job = soak.Job(soak.JOB_TYPES[0], 1, scratch_dir(self), ["python3", "run.py", "monkey"], "editing/default",
                       time.time() - 100, proc, "f" * 40, "/bin/true")
        report = soak.exit_report(job, timed_out=True)
        self.assertIn("ended: killed by signal 6 (SIGABRT) after the hard timeout", report)
        self.assertIn("load (1/5/15 min):", report)
        self.assertIn("command: python3 run.py monkey", report)


class RecordingPolicyTests(unittest.TestCase):
    def setUp(self):
        self.soak = scratch_soak(self)

    def trace(self, sig: str, where: str, age: float) -> Path:
        path = self.soak.FINDINGS / sig / where / self.soak.RECORDING
        path.mkdir(parents=True)
        (path / "trace").write_bytes(b"x" * 1000)
        os.utime(path, (time.time() - age, time.time() - age))
        return path

    def test_a_done_finding_loses_its_traces_and_an_open_one_keeps_its_newest(self):
        fixed = self.trace("aaaaaaaaaaaa", "evidence", 100)
        old = self.trace("bbbbbbbbbbbb", "evidence", 300)
        newest = self.trace("bbbbbbbbbbbb", "sightings/20261125-1", 10)
        entries = {"aaaaaaaaaaaa": mock.Mock(status="fixed"), "bbbbbbbbbbbb": mock.Mock(status="new")}
        self.assertGreater(self.soak.drop_recordings(entries, open_beyond_newest=False), 0)
        self.assertFalse(fixed.exists())
        self.assertTrue(old.exists() and newest.exists(), "an open finding keeps its traces in a gc")
        self.soak.drop_recordings(entries, open_beyond_newest=True)
        self.assertFalse(old.exists())
        self.assertTrue(newest.exists(), "over budget an open finding keeps its newest trace")

    def test_only_a_stale_minimize_scratch_goes(self):
        stale = self.soak.FINDINGS / "cccccccccccc" / "verify" / "minimize-old"
        live = self.soak.FINDINGS / "cccccccccccc" / "verify" / "minimize-live"
        for path in (stale, live):
            path.mkdir(parents=True)
        long_ago = time.time() - self.soak.STALE_SCRATCH_SECONDS - 60
        os.utime(stale, (long_ago, long_ago))
        self.soak.drop_stale_scratch()
        self.assertFalse(stale.exists())
        self.assertTrue(live.exists(), "a minimize still running keeps its scratch")

    def test_over_budget_even_the_last_traces_go_oldest_first(self):
        old = self.trace("aaaaaaaaaaaa", "evidence", 300)
        new = self.trace("bbbbbbbbbbbb", "evidence", 10)
        entries = {"aaaaaaaaaaaa": mock.Mock(status="new"), "bbbbbbbbbbbb": mock.Mock(status="new")}
        sizes = iter([2000, 1000])
        with mock.patch.object(self.soak, "disk_usage", side_effect=lambda _p: next(sizes, 0)):
            self.assertEqual(self.soak.drop_newest_recordings(entries, 1500), 1)
        self.assertFalse(old.exists())
        self.assertTrue(new.exists(), "dropping stops once the soak fits")

