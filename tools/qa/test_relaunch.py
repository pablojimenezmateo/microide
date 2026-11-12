#!/usr/bin/env python3
"""App.restart / App.crash_and_relaunch start the next process with the right
command line. Every suite scenario that restarts the app goes through these, and
a NameError in restart() failed eleven of them in CI (15b3a202a) while the
unittests stayed green: nothing called it outside a live app."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import microide_qa as q  # noqa: E402


def _stopped_app(work: str) -> tuple[q.App, list[Path | None]]:
    app = q.App(binary=Path("/bin/true"), work=Path(work), display=":0", size=(800, 600), env={})
    app.project = Path(work) / "proj"
    spawned: list[Path | None] = []
    app._spawn = lambda project, extra_args: spawned.append(project)  # type: ignore[method-assign]
    app.alive = lambda: False  # type: ignore[method-assign]
    app.new_log_lines = lambda: []  # type: ignore[method-assign]
    return app, spawned


class RelaunchTests(unittest.TestCase):
    def test_restart_relaunches_on_the_run_project(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            app, spawned = _stopped_app(work)
            app.restart()
        self.assertEqual(spawned, [Path(work) / "proj"])

    def test_restart_relaunches_on_a_named_project(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            app, spawned = _stopped_app(work)
            app.restart(project=Path("/other"))
        self.assertEqual(spawned, [Path("/other")])

    def test_crash_and_relaunch_into_the_workspace_names_no_project(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            app, spawned = _stopped_app(work)
            app.crash_and_relaunch(restore_workspace=True)
            app.crash_and_relaunch()
        self.assertEqual(spawned, [None, Path(work) / "proj"])


if __name__ == "__main__":
    unittest.main()
