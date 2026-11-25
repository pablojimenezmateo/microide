"""The soak's build runs beside the scheduling loop (soak.BackgroundBuild)."""

from __future__ import annotations

import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import soak  # noqa: E402


class BackgroundBuildTest(unittest.TestCase):
    def test_the_loop_keeps_its_own_changes_and_takes_the_builds(self) -> None:
        release = threading.Event()

        def fake_build(state: dict, log) -> None:
            release.wait(5.0)
            state.setdefault("good", {})["default"] = {"sha": "abc"}
            state["built_sha"] = "abc"

        original = soak.build_all
        soak.build_all = fake_build
        try:
            live = {"good": {}, "current": ["old"], "pending_checks": []}
            build = soak.BackgroundBuild(live, lambda line: None)
            self.assertFalse(build.done(), "the build is still running while the loop goes on")
            live["current"] = ["new"]
            live["pending_checks"] = ["sig"]
            release.set()
            build.thread.join(5.0)
            self.assertIsNone(build.finish(live))
        finally:
            soak.build_all = original
        self.assertEqual(live["good"], {"default": {"sha": "abc"}})
        self.assertEqual(live["built_sha"], "abc")
        self.assertEqual(live["current"], ["new"], "what the loop changed meanwhile is kept")
        self.assertEqual(live["pending_checks"], ["sig"])

    def test_a_failing_build_reports_its_error(self) -> None:
        def broken(state: dict, log) -> None:
            raise RuntimeError("main is at a QA monkey commit")

        original = soak.build_all
        soak.build_all = broken
        try:
            live: dict = {"good": {"default": {"sha": "old"}}}
            build = soak.BackgroundBuild(live, lambda line: None)
            build.thread.join(5.0)
            self.assertEqual(build.finish(live), "main is at a QA monkey commit")
        finally:
            soak.build_all = original
        self.assertEqual(live["good"], {"default": {"sha": "old"}}, "the last good build stays")


if __name__ == "__main__":
    unittest.main()
