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


class DyingTests(unittest.TestCase):
    """App.dying(): whether a request that just failed failed against a dead app."""

    def app(self, work: str, recorder_pid: int, app_pid: int, recording: bool) -> q.App:
        import subprocess
        app = q.App(binary=Path("/bin/true"), work=Path(work), display=":0", size=(800, 600), env={})
        app.proc = subprocess.Popen.__new__(subprocess.Popen)
        app.proc.pid, app.proc.returncode = recorder_pid, None
        app.proc.poll = lambda: None  # type: ignore[method-assign]
        app._rr_mode = "1" if recording else ""
        app._app_pid = app_pid if recording else None
        return app

    def test_an_app_rr_has_reaped_is_gone_while_rr_still_stands(self) -> None:
        # rr gave up at a system call it cannot record: it kills and reaps the
        # app and is still exiting. The request in flight was filed as "the app
        # closed the control connection", an app finding, for the recorder's limit.
        import os
        import subprocess
        gone = subprocess.Popen(["true"])
        gone.wait()
        with tempfile.TemporaryDirectory() as work:
            self.assertTrue(self.app(work, os.getpid(), gone.pid, recording=True).dying())
            # ... and a live app under a live recorder is not (this process, for both).
            self.assertFalse(self.app(work, os.getpid(), os.getpid(), recording=True).dying())

    def test_unrecorded_the_process_itself_is_what_counts(self) -> None:
        import os
        with tempfile.TemporaryDirectory() as work:
            self.assertFalse(self.app(work, os.getpid(), 0, recording=False).dying())


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
