"""What a set of check replays says of a finding beyond how many showed it."""
import doctest
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ledger as ld  # noqa: E402
import minimize  # noqa: E402
import soak  # noqa: E402


def checked(**fields) -> ld.Entry:
    entry = ld.Entry(sig="s", rate=0.0, rate_runs=12, deterministic=False)
    for name, value in fields.items():
        setattr(entry, name, value)
    return entry


class CheckReportTests(unittest.TestCase):
    def test_the_minimizers_doctests(self):
        self.assertEqual(doctest.testmod(minimize).failed, 0)

    def test_the_verdict_on_a_finding_no_replay_showed(self):
        self.assertEqual(checked().replay_verdict, "unknown")  # a check from before replays compared paths
        self.assertEqual(checked(replays_ran=0).replay_verdict, "untested")
        self.assertEqual(checked(replays_ran=12, replays_diverged=9, replays_followed=3).replay_verdict, "diverged")
        self.assertEqual(checked(replays_ran=12, replays_followed=11).replay_verdict, "followed")
        self.assertEqual(checked(rate=0.25, replays_ran=12).replay_verdict, "")
        self.assertEqual(ld.Entry(sig="s").replay_verdict, "")

    def test_the_ledger_folds_a_checks_report(self):
        import tempfile
        with tempfile.TemporaryDirectory() as scratch:
            book = ld.Ledger(Path(scratch) / "ledger.jsonl")
            book.append("abc", "new", kind="data", label="data: x")
            book.append("abc", "checked", rate=0.0, runs=12, deterministic=False, ran=12, followed=2, diverged=10,
                        divergence={"action": 41, "run": "t=compare", "replay": "t=editor", "after": "{}"},
                        errors=[], others=["def log: y"])
            entry = book.entries()["abc"]
            self.assertEqual((entry.replays_ran, entry.replays_diverged, entry.divergence["action"]), (12, 10, 41))
            self.assertEqual(entry.replay_others, ["def log: y"])
            self.assertEqual(entry.replay_verdict, "diverged")

    def test_finding_md_says_which_kind_of_zero_it_is(self):
        diverged = soak.reproduction_text(checked(
            replays_ran=12, replays_diverged=10, replays_followed=2,
            divergence={"action": 41, "run": "t=compare", "replay": "t=editor", "after": '{"line": "compare-files"}'}))
        self.assertIn("LEFT THE RUN'S PATH, first after action 41", diverged)
        self.assertIn("a gap in the harness", diverged)
        followed = soak.reproduction_text(checked(replays_ran=12, replays_followed=12))
        self.assertIn("followed the run's states to the end", followed)
        untested = soak.reproduction_text(checked(replays_ran=0, replay_errors=["QaFailure: no window"]))
        self.assertIn("NOT TESTED", untested)
        self.assertIn("QaFailure: no window", untested)
        partly = soak.reproduction_text(checked(rate=0.5, replays_ran=4, replay_errors=["x"]))
        self.assertIn("2 of 4 replays", partly)
        self.assertIn("8 more could not run", partly)


class PinnedBinaryTests(unittest.TestCase):
    def test_a_remote_replay_finds_the_server_beside_the_pinned_binary(self):
        # run.py looks for microide-server beside the binary it is given, and the
        # minimizer gives it a private copy: without the server copied too, no
        # replay of a remote finding ever ran.
        import tempfile
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "qa"))
        import run as qa_run
        with tempfile.TemporaryDirectory() as scratch:
            build, pinned_dir = Path(scratch) / "bin" / "default" / "abc", Path(scratch) / "minimize-x"
            build.mkdir(parents=True)
            pinned_dir.mkdir()
            (build / "microide").write_text("app")
            (build / "microide-server").write_text("server")
            pinned = minimize.pin_binaries(build / "microide", pinned_dir)
            self.assertEqual(pinned.read_text(), "app")
            from unittest import mock
            with mock.patch.dict("os.environ"):
                import os
                os.environ.pop("MICROIDE_SERVER_BINARY", None)
                server = qa_run.server_binary(pinned)
            self.assertEqual(server.read_text(), "server")
            self.assertEqual(server.parent, pinned_dir.resolve())

    def test_a_build_without_a_server_pins_the_app_alone(self):
        import tempfile
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / "microide").write_text("app")
            pinned_dir = Path(scratch) / "pin"
            pinned_dir.mkdir()
            self.assertEqual(minimize.pin_binaries(Path(scratch) / "microide", pinned_dir).read_text(), "app")
            self.assertEqual([p.name for p in pinned_dir.iterdir()], ["microide"])


class CheckBinaryTests(unittest.TestCase):
    """The binary a check replays a finding with."""

    def setUp(self):
        import tempfile
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = Path(self.dir.name)

    def binary(self, name: str) -> str:
        path = self.root / name / "microide"
        path.parent.mkdir(parents=True)
        path.write_text("")
        return str(path)

    def test_the_build_that_found_it_while_the_runner_has_it(self):
        found = self.binary("old")
        state = {"good": {"default": {"sha": "b" * 40, "binary": self.binary("new")}}}
        self.assertEqual(soak.check_binary({"binary": found, "sha": "a" * 40, "build": "default"}, state),
                         (found, "a" * 40))

    def test_the_current_build_of_its_configuration_once_its_own_is_pruned(self):
        # Every open finding older than six hours: its check copied a binary that
        # was gone and was booked "NOT TESTED", which nothing requeues.
        asan = self.binary("asan-new")
        state = {"good": {"default": {"sha": "b" * 40, "binary": self.binary("new")},
                          "asan": {"sha": "c" * 40, "binary": asan}}}
        gone = str(self.root / "pruned" / "microide")
        self.assertEqual(soak.check_binary({"binary": gone, "sha": "a" * 40, "build": "asan"}, state), (asan, "c" * 40))
        # ... and never another configuration's: an ASan finding is not replayed on the default build.
        self.assertEqual(soak.check_binary({"binary": gone, "sha": "a" * 40, "build": "tsan"}, state), (None, ""))

    def test_a_check_with_no_binary_books_no_measurement(self):
        import importlib
        import json
        import os
        from unittest import mock
        patcher = mock.patch.dict(os.environ, {"MICROIDE_SOAK_DIR": str(self.root / "soak")})
        self.addCleanup(importlib.reload, soak)
        patcher.start()
        self.addCleanup(patcher.stop)
        module = importlib.reload(soak)
        bundle = module.FINDINGS / "abc123abc123"
        bundle.mkdir(parents=True)
        (bundle / "replay.json").write_text(json.dumps({"actions": []}))
        (bundle / "meta.json").write_text(json.dumps({"binary": str(self.root / "pruned" / "microide"),
                                                      "build": "default", "sha": "a" * 40,
                                                      "finding": {"kind": "data", "label": "data: x"}}))
        module.ledger().append("abc123abc123", "new", kind="data", label="data: x")
        said: list[str] = []
        with mock.patch.object(module.subprocess, "run", side_effect=AssertionError("no replay may run")):
            module.check_finding("abc123abc123", {"good": {}}, said.append)
        self.assertIsNone(module.ledger().entries()["abc123abc123"].rate, "a rate was booked for replays that never ran")
        self.assertIn("left unmeasured", said[0])

    def test_finding_md_says_when_the_replays_ran_on_a_later_build(self):
        entry = checked(replays_ran=12, replays_followed=12, checked_on="c" * 40)
        entry.commits = {"a" * 40}
        self.assertIn("replayed on build `cccccccccccc`, later than any that showed it", soak.reproduction_text(entry))
        entry.commits = {"a" * 40, "c" * 40}
        self.assertNotIn("later than any", soak.reproduction_text(entry))


if __name__ == "__main__":
    unittest.main()
