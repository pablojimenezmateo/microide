#!/usr/bin/env python3
"""The remote profile's view of a project's two sides (remote_host.content_set)
and its convergence oracle's verdicts (monkey_oracles.RemoteConvergence)."""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import monkey_oracles as mo  # noqa: E402
import remote_host as rh  # noqa: E402


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class ContentSetTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.host = Path(scratch.name) / "host"
        self.mirror = Path(scratch.name) / "mirror"
        _write(self.host / ".gitignore", "/build/\n")
        _write(self.host / "src" / "a.cpp", "int a;\n")
        _write(self.host / "build" / "out.o", "binary\n")  # ignored: never synced
        os.symlink(self.host / "src" / "a.cpp", self.host / "abs-link")  # absolute: a copy in the mirror
        os.symlink("does/not/exist", self.host / "dangling")
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        subprocess.run(["git", "init", "-q", str(self.host)], check=True, env=env)
        _write(self.mirror / ".gitignore", "/build/\n")
        _write(self.mirror / "src" / "a.cpp", "int a;\n")
        _write(self.mirror / "abs-link", "int a;\n")

    def diff(self) -> list[str]:
        host, mirror = rh.content_set(self.host, self.mirror)
        return mo.RemoteConvergence.differing(host, mirror)

    def test_a_synced_project_agrees(self) -> None:
        # The ignored build output, the dangling link and the link the mirror holds
        # as a copy are all as the sync defines them.
        self.assertEqual(self.diff(), [])

    def test_an_ignored_file_in_the_mirror_is_not_owed_to_the_host(self) -> None:
        _write(self.mirror / "build" / "local.o", "x\n")
        self.assertEqual(self.diff(), [])

    def test_a_changed_file_differs(self) -> None:
        _write(self.mirror / "src" / "a.cpp", "int b;\n")
        self.assertEqual(self.diff(), ["src/a.cpp"])

    def test_a_file_created_in_the_mirror_is_owed_to_the_host(self) -> None:
        _write(self.mirror / "src" / "new.cpp", "x\n")
        self.assertEqual(self.diff(), ["src/new.cpp"])


class ConvergenceTests(unittest.TestCase):
    def _oracle(self, statuses: list[dict]) -> mo.RemoteConvergence:
        app = mock.Mock()
        app.alive.return_value = True
        app.query.side_effect = statuses + [statuses[-1]] * 400
        oracle = mo.RemoteConvergence(app, Path("/m"), Path("/h"), "remote-open qa-local:/h")
        return oracle

    def test_a_conflict_open_is_not_a_verdict(self) -> None:
        ready = {"projectRoot": "/m", "remote": {"state": "ready", "conflicts": 1}}
        oracle = self._oracle([ready])
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.outcomes["conflicted"], 1)

    def test_another_project_in_front_is_brought_back(self) -> None:
        elsewhere = {"projectRoot": "/local", "remote": {}}
        ready = {"projectRoot": "/m", "remote": {"state": "ready", "conflicts": 1}}
        oracle = self._oracle([elsewhere, ready])
        oracle.check()
        oracle.app.send_line.assert_called_once_with("remote-open qa-local:/h", record=False)

    def test_saves_the_host_refused_are_blocked_not_a_stall(self) -> None:
        # Soak 464a40fee2bd: refused saves are `blocked`, not unsent; the project
        # settles, and their files differ on purpose until the user chooses.
        blocked = {"projectRoot": "/m", "remote": {"state": "ready", "filesPending": 0, "unsentPushes": 0,
                                                    "blocked": 2}}
        oracle = self._oracle([blocked])
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.outcomes["blocked"], 1)

    def test_a_project_that_never_settles_is_reported(self) -> None:
        stuck = {"projectRoot": "/m", "remote": {"state": "connecting", "filesPending": 3}}
        oracle = self._oracle([stuck])
        with mock.patch.object(mo.time, "sleep"), mock.patch.object(mo.RemoteConvergence, "SETTLE_SECONDS", 0.0):
            found = oracle.check()
        self.assertEqual(len(found), 1)
        self.assertIn("did not settle", found[0][1])


if __name__ == "__main__":
    unittest.main()
