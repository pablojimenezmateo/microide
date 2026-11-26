#!/usr/bin/env python3
"""The project shapes the external source makes for the soak (TD-2026-11-08-435
item 5): a file of BIG_FILE_BYTES, empty folders, a git submodule through its
lifecycle, and a history LONG_HISTORY_COMMITS deep. Each is deterministic in
its counter, so a replay makes the same."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import monkey as mk  # noqa: E402


class ProjectShapeTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.project = Path(scratch.name) / "taskflow"
        self.project.mkdir()
        (self.project / "a.txt").write_text("a\n")
        for args in (["init", "-q", "-b", "main"], ["add", "-A"], ["commit", "-q", "-m", "first"]):
            self.git(*args)

    def git(self, *args: str, cwd: Path | None = None) -> str:
        return subprocess.run(["git", "-C", str(cwd or self.project), *args], check=True, capture_output=True,
                              text=True, env=mk.git_env(self.project)).stdout

    def test_the_big_file_has_its_size_and_two_slots(self) -> None:
        mk.apply_external(self.project, "huge-file", "", 3)
        mk.apply_external(self.project, "huge-file", "", 4)
        mk.apply_external(self.project, "huge-file", "", 5)
        files = sorted((self.project / "qa-big").iterdir())
        self.assertEqual([f.name for f in files], ["huge0.txt", "huge1.txt"])
        for path in files:
            size = path.stat().st_size
            self.assertGreater(size, mk.BIG_FILE_BYTES - 200)
            self.assertLessEqual(size, mk.BIG_FILE_BYTES)
        self.assertTrue(files[1].read_text().startswith("00000005 "), "the newest write of the slot")
        self.assertEqual(mk.BIG_FILE_BYTES, 50 * 1024 * 1024)

    def test_empty_folders_nest_and_hold_nothing(self) -> None:
        mk.apply_external(self.project, "empty-folder", "", 2)
        mk.apply_external(self.project, "empty-folder", "", 7)
        self.assertEqual(list((self.project / "qa-ext" / "empty2").iterdir()), [])
        inner = self.project / "qa-ext" / "empty3" / "inner"
        self.assertTrue(inner.is_dir())
        self.assertEqual(list(inner.iterdir()), [])

    def test_a_submodule_is_added_moved_dirtied_and_taken_out(self) -> None:
        sub = self.project / "qa-sub"
        mk.apply_external(self.project, "submodule", "", 0)
        self.assertTrue((sub / "sub.txt").is_file())
        self.assertIn("qa-sub", (self.project / ".gitmodules").read_text())
        self.assertEqual(self.git("status", "--porcelain"), "", "the submodule is committed")

        mk.apply_external(self.project, "submodule", "", 1)
        self.assertIn(" M qa-sub", self.git("status", "--porcelain"), "a commit inside moves its pointer")

        mk.apply_external(self.project, "submodule", "", 2)
        self.assertIn("dirty.txt", self.git("status", "--porcelain", cwd=sub))

        mk.apply_external(self.project, "submodule", "", 3)
        self.assertTrue(sub.is_dir())
        self.assertEqual(list(sub.iterdir()), [], "deinit leaves the folder empty")

        mk.apply_external(self.project, "submodule", "", 4)
        self.assertTrue((sub / "sub.txt").is_file(), "and update --init brings it back")

    def test_the_source_repository_sits_beside_the_project(self) -> None:
        mk.apply_external(self.project, "submodule", "", 0)
        self.assertTrue((self.project.parent / mk.SUBMODULE_SOURCE / ".git").is_dir())

    def test_a_long_history_is_made_once(self) -> None:
        before = int(self.git("rev-list", "--count", "HEAD"))
        mk.apply_external(self.project, "long-history", "", 9)
        after = int(self.git("rev-list", "--count", "HEAD"))
        self.assertEqual(after - before, mk.LONG_HISTORY_COMMITS)
        self.assertEqual(self.git("status", "--porcelain"), "", "the working tree matches the new head")
        self.assertEqual((self.project / "qa-history.txt").read_text(),
                         f"change {mk.LONG_HISTORY_COMMITS - 1}\n")
        mk.apply_external(self.project, "long-history", "", 10)
        self.assertEqual(int(self.git("rev-list", "--count", "HEAD")), after, "only once a project")
        self.assertEqual(mk.LONG_HISTORY_COMMITS, 3000)

    def test_the_second_project_is_a_clone_beside_the_first(self) -> None:
        mk.apply_external(self.project, "second-project", "", 1)
        second = self.project.parent / mk.SECOND_PROJECT
        self.assertEqual((second / "a.txt").read_text(), "a\n")
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=second), self.git("rev-parse", "HEAD"))
        (second / "a.txt").write_text("changed\n")
        mk.apply_external(self.project, "second-project", "", 2)
        self.assertEqual((second / "a.txt").read_text(), "changed\n", "made once")


class SecondProjectCloseTests(unittest.TestCase):
    second = Path("/w/qa-second-project")

    def test_only_the_second_project_in_front_with_another_open_closes(self) -> None:
        both = [{"root": "/w/taskflow", "active": False}, {"root": str(self.second), "active": True}]
        self.assertTrue(mk.second_project_closable(both, self.second))

    def test_never_the_runs_own_project(self) -> None:
        both = [{"root": "/w/taskflow", "active": True}, {"root": str(self.second), "active": False}]
        self.assertFalse(mk.second_project_closable(both, self.second))

    def test_never_the_last_project_open(self) -> None:
        alone = [{"root": str(self.second), "active": True}]
        self.assertFalse(mk.second_project_closable(alone, self.second))
        self.assertFalse(mk.second_project_closable([], self.second))


if __name__ == "__main__":
    unittest.main()
