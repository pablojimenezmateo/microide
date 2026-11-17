"""What the soak reaches and how it knows: hunts, stale checks, a build finding's
end, the surface's count and the source coverage."""
import doctest
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
import codecov  # noqa: E402
import surface  # noqa: E402


class SoakDirTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        self.addCleanup(self.dir.cleanup)
        import soak
        self.addCleanup(importlib.reload, soak)
        patcher = mock.patch.dict(os.environ, {"MICROIDE_SOAK_DIR": self.dir.name})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.soak = importlib.reload(soak)
        self.log: list[str] = []

    def finding(self, sig: str, kind: str, label: str, rate: float | None = 0.0, **checked) -> None:
        book = self.soak.ledger()
        book.append(sig, "new", kind=kind, label=label, cell="editing/default/1440x900@1", commit="c0ffee")
        if rate is not None:
            book.append(sig, "checked", rate=rate, runs=12, deterministic=rate >= 2 / 3, **checked)
        bundle = self.soak.FINDINGS / sig
        bundle.mkdir(parents=True, exist_ok=True)
        (bundle / "replay.json").write_text(json.dumps({"actions": []}))


class SurfaceLaunchTests(unittest.TestCase):
    def test_the_inventory_is_taken_from_a_copy_of_the_tools_with_no_build_directory(self):
        # The runner runs surface.py from its driver snapshot: tools/ and nothing
        # else. The fixture went under <repository>/build, which a snapshot has
        # not, and the inventory failed on every build.
        import contextlib
        with tempfile.TemporaryDirectory() as scratch:
            snapshot, out = Path(scratch) / "driver" / "abc123", Path(scratch) / "soak"
            made: list[Path] = []

            def make_fixture(path: Path) -> Path:
                path.mkdir(parents=True)
                made.append(path)
                return path

            @contextlib.contextmanager
            def launch(project, binary):
                yield mock.Mock(query=lambda verb: {"commands": ["save"], "verb": verb})
            with mock.patch.object(surface, "REPO", snapshot), \
                 mock.patch.object(surface.q, "make_fixture", make_fixture), \
                 mock.patch.object(surface.q.App, "launch", launch):
                answer = surface.read_surface(Path("/bin/true"), out)
            self.assertEqual(answer["commands"], ["save"])
            self.assertEqual(made[0].parent.parent, out, "the fixture is made under the soak's own directory")
            self.assertEqual(list(out.iterdir()), [], "and is gone when the answer is in")
            self.assertFalse((snapshot / "build").exists())


class HuntTests(SoakDirTests):
    def test_what_is_worth_a_hunt(self):
        self.finding("lost", "data", "data: the unsaved buffer a.txt did not come back", ran=12, followed=12)
        self.finding("old", "data", "data: the unsaved buffer b.txt did not come back")  # checked before paths
        self.finding("paint", "log", "log: [x] QA-STALE frame=1")                         # replays from its actions
        self.finding("shown", "data", "data: the clean buffer of c.txt differs", rate=0.5, ran=12)
        self.finding("diverged", "data", "data: d.txt", ran=12, diverged=11)              # tools/qa's to fix first
        self.finding("slow", "latency", "latency: query took 3s", ran=12, followed=12)   # rr's timing, not the app's
        self.finding("unmeasured", "undo", "undo: 2 undos changed the text", rate=None)
        candidates = [e.sig for e in self.soak.hunt_candidates(self.soak.ledger().entries().values())]
        self.assertEqual(sorted(candidates), ["lost", "old"])

    def test_a_recording_or_enough_failed_hunts_end_it(self):
        self.finding("lost", "data", "data: the unsaved buffer a.txt did not come back", ran=12, followed=12)
        for _ in range(self.soak.HUNT_LIMIT - 1):
            self.soak.ledger().append("lost", "note", text=f"{self.soak.HUNT_NOTE}6 paced replays did not show it")
        entries = self.soak.ledger().entries()
        self.assertEqual(self.soak.hunts_made(entries["lost"]), self.soak.HUNT_LIMIT - 1)
        self.assertEqual([e.sig for e in self.soak.hunt_candidates(entries.values())], ["lost"])
        self.soak.ledger().append("lost", "note", text=f"{self.soak.HUNT_NOTE}6 paced replays did not show it")
        self.assertEqual(self.soak.hunt_candidates(self.soak.ledger().entries().values()), [])
        self.finding("traced", "data", "data: e.txt", ran=12, followed=12)
        (self.soak.FINDINGS / "traced" / "sightings" / "x-hunt" / "rr").mkdir(parents=True)
        self.assertEqual(self.soak.hunt_candidates(self.soak.ledger().entries().values()), [])

    def test_a_hunt_replays_the_newest_sightings_record(self):
        self.finding("lost", "data", "data: a.txt")
        bundle = self.soak.FINDINGS / "lost"
        self.assertEqual(self.soak.hunt_replay(bundle), bundle / "replay.json")
        for name in ("20261101-000000-aaa-x", "20261116-000000-bbb-y"):
            (bundle / "sightings" / name).mkdir(parents=True)
            (bundle / "sightings" / name / "replay.json").write_text("{}")
        self.assertEqual(self.soak.hunt_replay(bundle).parent.name, "20261116-000000-bbb-y")


class RequeueTests(SoakDirTests):
    def test_a_rate_from_before_faithful_replays_is_measured_again_and_only_once(self):
        self.finding("old-severe", "data", "data: a.txt")
        self.finding("old-paint", "log", "log: [x] QA-STALE frame=1")
        self.finding("measured", "data", "data: b.txt", ran=12, followed=12)
        self.finding("shown", "data", "data: c.txt", rate=1.0)
        state: dict = {}
        self.assertEqual(self.soak.requeue_unmeasured(state, self.log.append), 2)
        self.assertEqual(state["pending_checks"], ["old-severe", "old-paint"])  # the most severe first
        self.assertEqual(self.soak.requeue_unmeasured(state, self.log.append), 0)
        # Checked again, it carries a count of the replays that ran: not requeued.
        self.soak.ledger().append("old-severe", "checked", rate=0.0, runs=12, deterministic=False, ran=12, followed=12)
        state = {}
        self.soak.requeue_unmeasured(state, self.log.append)
        self.assertEqual(state["pending_checks"], ["old-paint"])

    def test_a_finding_this_runner_has_not_seen_is_not_replayed(self):
        # Seen last before the runner came up: for all the runner knows a finding
        # under a signature rule the driver no longer has, which no replay can
        # show again. Hundreds of them were queued, a dozen replays each.
        import time
        self.finding("before", "data", "data: a.txt")
        state = {"started": time.time() + 60}
        self.assertEqual(self.soak.requeue_unmeasured(state, self.log.append), 0)
        # ... and one it has seen since is.
        self.soak.ledger().append("before", "seen", kind="data", label="data: a.txt", cell="c", commit="c0ffee")
        state = {"started": time.time() - 60}
        self.assertEqual(self.soak.requeue_unmeasured(state, self.log.append), 1)

    def test_a_check_no_replay_of_which_ran_is_tried_again_on_the_next_build(self):
        self.finding("untested", "remote", "remote: the mirror differs", ran=0, on="a" * 40,
                     errors=["FileNotFoundError: no microide-server"])
        self.assertEqual(self.soak.requeue_unmeasured({"built_sha": "a" * 40}, self.log.append), 0,
                         "the same build would fail the same way")
        state = {"built_sha": "b" * 40}
        self.assertEqual(self.soak.requeue_unmeasured(state, self.log.append), 1)
        self.assertEqual(state["pending_checks"], ["untested"])


class BuildFindingTests(SoakDirTests):
    def test_a_build_that_is_good_again_closes_its_finding(self):
        sig = self.soak.build_signature("default")
        self.soak.ledger().append(sig, "new", kind="build", label="build: the default build fails", cell="default")
        self.soak.close_build_finding("default", "c0ffee" * 4, self.log.append)
        self.assertEqual(self.soak.ledger().entries()[sig].status, "closed")
        # Nothing to close the second time, and nothing for a build that never failed.
        self.soak.close_build_finding("default", "c0ffee" * 4, self.log.append)
        self.soak.close_build_finding("asan", "c0ffee" * 4, self.log.append)
        self.assertEqual(len(self.log), 1)
        # It fails again: a sighting reopens it.
        self.soak.ledger().append(sig, "seen", kind="build", label="build: the default build fails", cell="default")
        self.assertEqual(self.soak.ledger().entries()[sig].status, "reopened")


class SurfaceTests(unittest.TestCase):
    INVENTORY = {"commands": ["open", "quit", "tab"],
                 "keybindings": [{"id": "close-tab", "chord": "Ctrl+W", "context": "global"},
                                 {"id": "quit", "chord": "Ctrl+Q", "context": "global"},
                                 {"id": "fold-all", "chord": "Ctrl+K Ctrl+0", "context": "editor"}],
                 "settings": ["editor.wrap", "control.enabled"], "focusTargets": ["editor", "panel"]}

    def count(self, coverage: dict, excluded: dict) -> dict:
        inventory = surface.flatten(self.INVENTORY)
        return surface.summarize(inventory, surface.gaps(inventory, surface.reached(coverage, set())), excluded)

    def test_a_binding_is_reached_when_it_was_pressed_where_it_is_live(self):
        # Its id is not a command's name ("close-tab"): the old count read every
        # such binding as never pressed, off a field that had become a digest.
        coverage = {"commands": {"open": 3, "tab": 1}, "keys": {"close-tab": 9},
                    "settings": {"editor.wrap=true": 1}, "states": {"f=editor o=-": 1, "f=panel o=-": 1}}
        summary = self.count(coverage, {"commands": {"quit": "ends the run"}, "keybindings": {"quit": "ends the run"},
                                        "settings": {"control.enabled": "ends the channel"}})
        self.assertEqual(summary["categories"]["keybindings"]["unreached"], ["fold-all (Ctrl+K Ctrl+0)"])
        self.assertEqual(summary["categories"]["keybindings"]["excluded"], ["quit (Ctrl+Q)"])
        self.assertEqual((summary["unreached"], summary["excluded"]), (1, 3))
        self.assertEqual(summary["reached"], summary["total"] - 4)

    def test_an_exclusion_of_an_item_the_app_no_longer_has_is_stale(self):
        summary = self.count({}, {"commands": {"quit": "ends the run", "gone-command": "was removed"}})
        self.assertEqual(summary["staleExclusions"], ["commands: gone-command"])

    def test_the_shipped_exclusions_parse_and_carry_reasons(self):
        import tomllib
        extra = tomllib.loads((HERE / "surface-extra.toml").read_text())
        excluded = surface.excluded_items(extra)
        self.assertIn("quit", excluded["commands"])
        self.assertTrue(all(reason.strip() for names in excluded.values() for reason in names.values()))
        self.assertEqual(doctest.testmod(surface).failed, 0)


class CodeCoverageTests(SoakDirTests):
    def test_the_examples_hold(self):
        self.assertEqual(doctest.testmod(codecov).failed, 0)

    def test_the_report_names_the_files_nothing_entered(self):
        summary = {"totals": {"lines": [40, 100], "functions": [4, 10], "branches": [3, 20]},
                   "files": {"editor/Used.cpp": {"lines": [40, 60], "functions": [4, 6], "branches": [3, 12]},
                             "workspace/render/Dark.cpp": {"lines": [0, 40], "functions": [0, 4], "branches": [0, 8]}},
                   "areas": {"editor": {"lines": [40, 60], "functions": [4, 6], "branches": [3, 12]},
                             "workspace/render": {"lines": [0, 40], "functions": [0, 4], "branches": [0, 8]}}}
        text = codecov.report_markdown(summary, "c0ffee" * 7, 12)
        self.assertIn("lines: **40.0%** (40 of 100)", text)
        self.assertIn("## Files no job ever entered (1)", text)
        self.assertIn("`workspace/render/Dark.cpp` (40 lines, 4 functions)", text)
        self.assertIn("| workspace/render |   0.0% |", text)

    def test_a_job_with_no_profile_is_counted_as_one_that_measured_nothing(self):
        from types import SimpleNamespace
        job = SimpleNamespace(out=Path(self.dir.name) / "run", cell="coverage/coverage/1440x900@1", seed=3)
        (job.out / "profraw").mkdir(parents=True)
        state: dict = {}
        self.soak.merge_code_profile(job, state, self.log.append)
        self.assertEqual((state.get("code_runs", 0), state["code_failures"]), (0, 1))
        self.assertIn("no profile from coverage/coverage/1440x900@1", self.log[0])

    def test_the_coverage_job_runs_the_profiles_the_default_build_runs(self):
        import random
        job = next(j for j in self.soak.JOB_TYPES if j.name == "coverage")
        drawn = {self.soak.job_profile(job, random.Random(seed)) for seed in range(200)}
        self.assertGreater(len(drawn), 5)
        self.assertFalse(drawn & {"commands", "editor", self.soak.ANY_PROFILE})


if __name__ == "__main__":
    unittest.main()
