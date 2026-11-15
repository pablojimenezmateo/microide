#!/usr/bin/env python3
"""Unit tests for monkey_oracles that need no app: a fake answers the control
queries the oracle asks."""
from __future__ import annotations

import random
import time
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import monkey_oracles  # noqa: E402


class _RelaunchingApp:
    """Two projects; the second, the active one, holds an unsaved buffer. The
    relaunch behaves as the app's startup does: `microide <project>` opens THAT
    project instead of the saved workspace (WorkspaceLifecycleCoordinator), and a
    launch with no project restores every project of the workspace, the saved
    active one active and the others loading their tabs on first activation."""

    ROOTS = ("/p/a", "/p/b")

    def __init__(self) -> None:
        self.work = None
        self.project = "/p/a"
        self.actions: list[dict] = []
        self.lines: list[str] = []
        self.relaunched = False
        self.catalog = [0, 1]  # indices into ROOTS, in catalog order
        self.active = 1
        self.loaded = {0: True, 1: True}
        self.docs = {
            0: [{"path": "/p/a/x.txt", "dirty": False, "sha256": "aa", "project": "/p/a"}],
            1: [{"path": "/p/b/y.txt", "dirty": True, "sha256": "bb", "project": "/p/b"}],
        }
        # `editor.autosave`: with on_focus_change, leaving a project saves its
        # dirty buffers -- the save's transforms (a final newline) change them.
        self.autosave = "off"

    def query(self, verb, args=None):
        if verb == "persistence":
            return {"staged": True}
        if verb == "projects":
            return [{"root": self.ROOTS[p], "index": i, "active": p == self.active, "loaded": self.loaded[p]}
                    for i, p in enumerate(self.catalog)]
        if verb == "settings":
            return [{"id": "editor.autosave", "value": self.autosave}]
        if verb == "status":
            return {"projectRoot": self.ROOTS[self.active]}
        if verb == "documents":
            return [dict(doc) for p in self.catalog if self.loaded[p] for doc in self.docs[p]]
        raise AssertionError(verb)

    def send_line(self, line, timeout=15.0, record=True):
        self.lines.append(line)
        if line == "project-next":
            if self.autosave == "on_focus_change":
                for doc in self.docs[self.active]:
                    if doc["dirty"]:
                        doc.update(dirty=False, sha256=doc["sha256"] + "-saved")
            at = self.catalog.index(self.active)
            self.active = self.catalog[(at + 1) % len(self.catalog)]
            self.loaded[self.active] = True
        return {}

    def record_external(self, row) -> None:
        # As App.record_external: the row itself. Wrapped in {"external": ...} it
        # read as an external step to `_churned`, which then excused every file
        # the disk check compares -- and no test here could see that check fire.
        self.actions.append(row)

    def crash_and_relaunch(self, project=None, between=None, restore_workspace=False) -> None:
        if between is not None:
            between()
        self.relaunched = True
        if restore_workspace:
            # The saved workspace: every project, the one active at the kill active.
            self.loaded = {p: p == self.active for p in self.catalog}
        else:
            # The command line's project alone; the other is not in the catalog.
            self.catalog = [self.ROOTS.index(project or self.project)]
            self.active = self.catalog[0]
            self.loaded = {p: p == self.active for p in (0, 1)}


class Kill9OracleTests(unittest.TestCase):
    def test_the_first_wait_is_for_the_active_projects_buffers_alone(self) -> None:
        # A background project's buffer can only come back through the lap; the
        # wait before it waited for that one too and ran to its 10 s deadline
        # on every such kill.
        app = _RelaunchingApp()
        app.active = 0  # the dirty buffer is in the background project
        oracle = monkey_oracles.DataOracle(app)
        started = time.monotonic()
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            found = oracle.kill9_and_check(random.Random(1), waited=True)
        self.assertEqual(found, [], found)
        self.assertLess(time.monotonic() - started, 3.0, "no wait for a buffer the lap has not loaded")

    def test_a_restored_project_that_has_not_loaded_is_loaded_before_the_compare(self) -> None:
        app = _RelaunchingApp()
        app.active = 0  # the buffer's project is a background one at the kill
        oracle = monkey_oracles.DataOracle(app)
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            found = oracle.kill9_and_check(random.Random(1), waited=True)
        self.assertEqual(found, [], found)
        self.assertEqual(app.active, 0, "the lap ends on the project the relaunch made active")

    def test_the_relaunch_restores_the_workspace_not_only_the_run_project(self) -> None:
        # Soak e946ec9333d7: the relaunch named the run's project on its command
        # line, which opens that project INSTEAD of the saved workspace, so every
        # unsaved buffer of a second project read as lost -- though its session
        # record held them and reopening the project brought them back.
        app = _RelaunchingApp()
        oracle = monkey_oracles.DataOracle(app)
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            found = oracle.kill9_and_check(random.Random(1), waited=True)
        self.assertEqual(found, [], found)
        self.assertEqual([app.ROOTS[p] for p in app.catalog], ["/p/a", "/p/b"])


    def test_a_save_the_oracles_own_lap_made_is_not_a_changed_buffer(self) -> None:
        # Soak 0afa15c55c0f: with autosave on focus change, the lap that loads the
        # other projects is the user leaving each one, and saves its buffers.
        app = _RelaunchingApp()
        app.active = 0  # the buffer's project loads only through the lap
        app.autosave = "on_focus_change"
        oracle = monkey_oracles.DataOracle(app)
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            found = oracle.kill9_and_check(random.Random(1), waited=True)
        self.assertEqual(found, [], found)

    def test_a_restored_buffer_read_before_the_lap_is_still_compared(self) -> None:
        # The active project's buffers are read before any lap: one that came back
        # different is reported, autosave or not.
        app = _RelaunchingApp()
        app.autosave = "on_focus_change"
        oracle = monkey_oracles.DataOracle(app)
        original_relaunch = app.crash_and_relaunch

        def relaunch_changing(*args, **kwargs):
            original_relaunch(*args, **kwargs)
            app.docs[1][0] = {**app.docs[1][0], "sha256": "changed"}
        app.crash_and_relaunch = relaunch_changing
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            found = oracle.kill9_and_check(random.Random(1), waited=True)
        self.assertEqual(len(found), 1, found)
        self.assertIn("different content", found[0][1])


    def test_a_lap_save_through_a_symlinked_folder_is_not_an_unsaved_change(self) -> None:
        # Soak ce9c3a0386c3: qa-weird/loop links to qa-weird, and a second project
        # rooted at the link held a dirty buffer of a file the first project had
        # open clean under its own path. The lap's autosave wrote that buffer, and
        # the clean tab's file -- the same file -- read as changed by nothing.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "a"
            root.mkdir()
            (root / "x.txt").write_text("before\n")
            (root / "loop").symlink_to(".")
            clean, alias = root / "x.txt", root / "loop" / "x.txt"

            class _AliasingApp(_RelaunchingApp):
                ROOTS = (str(root), str(root / "loop"))

                def __init__(self) -> None:
                    super().__init__()
                    self.project = self.ROOTS[0]
                    self.docs = {
                        0: [{"path": str(clean), "dirty": False, "project": self.ROOTS[0],
                             "sha256": monkey_oracles._sha256_file(clean)}],
                        1: [{"path": str(alias), "dirty": True, "sha256": "edited",
                             "project": self.ROOTS[1]}],
                    }

                def send_line(self, line, timeout=15.0, record=True):
                    if line == "project-next" and self.autosave == "on_focus_change":
                        for doc in self.docs[self.active]:
                            if doc["dirty"]:
                                Path(doc["path"]).write_text("edited and saved\n")
                    return super().send_line(line, timeout, record)

            app = _AliasingApp()
            app.active = 0  # the buffer's project loads, and is left, through the lap
            app.autosave = "on_focus_change"
            oracle = monkey_oracles.DataOracle(app)
            with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                    mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
                found = oracle.kill9_and_check(random.Random(1), waited=True)
            self.assertEqual(clean.read_text(), "edited and saved\n", "the lap saved through the link")
            self.assertEqual(found, [], found)

    def test_a_clean_file_changed_by_no_save_is_still_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "a"
            root.mkdir()
            clean = root / "x.txt"
            clean.write_text("before\n")

            class _ChangingApp(_RelaunchingApp):
                ROOTS = (str(root), "/p/b")

                def __init__(self) -> None:
                    super().__init__()
                    self.project = self.ROOTS[0]
                    self.docs[0] = [{"path": str(clean), "dirty": False, "project": self.ROOTS[0],
                                     "sha256": monkey_oracles._sha256_file(clean)}]

                def crash_and_relaunch(self, *args, **kwargs) -> None:
                    super().crash_and_relaunch(*args, **kwargs)
                    clean.write_text("changed by the relaunch\n")

            app = _ChangingApp()
            oracle = monkey_oracles.DataOracle(app)
            with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                    mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
                found = oracle.kill9_and_check(random.Random(1), waited=True)
            self.assertEqual([f[1] for f in found],
                             ["x.txt changed on disk across a kill -9 and relaunch, and nothing saved it"])


class IdleEvidenceTests(unittest.TestCase):
    """Soak a64655101a5a: an idle app redrawing 60 times a second, and the evidence
    could not say what it redrew. The frames' dirty bounds name the surface."""

    def test_the_repainted_bounds_name_the_elements_under_them(self) -> None:
        app = mock.Mock()
        app.ui.return_value = {"elements": [
            {"name": "tab:README.md", "rect": [125, 18, 80, 14]},
            {"name": "status:line-column", "rect": [900, 300, 60, 12]},
        ]}
        oracle = monkey_oracles.IdleOracle(app)
        frames = [f"microide redraw frame: frame={i} reason=scheduled-partial dirty=1 clips=1 "
                  f"elapsed=1.0 ms bounds=120,16,200,18" for i in range(5)]
        frames.append("microide redraw frame: frame=9 reason=event-partial bounds=0,0,0,0")
        lines = oracle._repainted_regions(frames)
        self.assertEqual(lines[0], "repainted bounds (x,y,w,h), most common first:")
        self.assertIn("5  120,16,200,18  under: tab:README.md", lines[1])
        self.assertNotIn("status:line-column", lines[1])

    def test_frames_without_bounds_add_nothing(self) -> None:
        oracle = monkey_oracles.IdleOracle(mock.Mock())
        self.assertEqual(oracle._repainted_regions(["microide redraw frame: frame=1 dirty=1"]), [])


class ResourceOracleTests(unittest.TestCase):
    """Soak 6da9bc4a526a / d1e39df0281e / bdea62052550: the terminal profile keeps
    up to 200 terminals open, each with its pty, reader thread and shell, and every
    run crossed the fixed budgets with nothing leaked (20 opened and closed return
    to the baseline exactly). What the open panes hold is not growth."""

    def _oracle(self, samples):
        oracle = monkey_oracles.ResourceOracle(mock.Mock())
        it = iter(samples)

        def sample():
            current = next(it)
            oracle.samples.append(current)
            if oracle.baseline is None:
                oracle.baseline = current
            return current
        oracle.sample = sample  # type: ignore[method-assign]
        return oracle

    def test_open_terminals_are_not_a_leak(self) -> None:
        start = {"fds": 33, "threads": 50, "children": 1, "rss_kb": 1, "panes": 1}
        many = {"fds": 33 + 3 * 120, "threads": 50 + 120, "children": 121, "rss_kb": 1, "panes": 121}
        oracle = self._oracle([start, many])
        oracle.check()
        self.assertEqual(oracle.check(), [])

    def test_growth_past_the_open_panes_is_reported(self) -> None:
        start = {"fds": 33, "threads": 50, "children": 1, "rss_kb": 1, "panes": 1}
        leaky = {"fds": 33 + 3 * 10 + 400, "threads": 60, "children": 11, "rss_kb": 1, "panes": 11}
        oracle = self._oracle([start, leaky])
        oracle.check()
        found = oracle.check()
        self.assertEqual(len(found), 1)
        self.assertIn("fds grew by 430", found[0][1])
        self.assertIn("plus 40 for 10 more open terminal pane(s)", found[0][1])


class _UndoEditor:
    """One clean editor buffer. `undo` and `redo` walk a real history; a churn
    hook may rewrite the file under the buffer between two commands, as the
    monkey's ExternalChurn thread does, and the buffer then reloads it."""

    def __init__(self, history: list[str], at: int, redo_drops: bool = False) -> None:
        self.project = "/w"
        self.actions: list[dict] = []
        self.last_ui = {"focus": "editor"}
        self.history = history
        self.at = at
        self.text = history[at]
        self.revision = 1
        self.redo_drops = redo_drops
        self.on_line = None  # called with each line sent, before it applies

    def query(self, verb, args=None):
        assert verb == "buffer", verb
        return {"kind": "editor", "path": "/w/CHANGELOG.md", "text": self.text,
                "revision": self.revision}

    def churn(self, counter: int) -> None:
        """The file rewritten on disk and the clean buffer reloaded; the row
        is recorded as App.record_external does, after the write."""
        self.text = f"churn {counter}\n" + self.text
        self.revision += 1
        self.actions.append({"churn": "CHANGELOG.md", "counter": counter,
                             "t": round(time.monotonic(), 3)})

    def send_line(self, line, timeout=15.0, record=True):
        if self.on_line is not None:
            self.on_line(line)
        if line == "undo" and self.at > 0:
            self.at -= 1
        elif line == "redo" and self.at + 1 < len(self.history):
            self.at += 1
            if self.redo_drops:
                self.text = self.history[self.at][:-1]
                self.revision += 1
                return {}
        else:
            return {}
        self.text = self.history[self.at]
        self.revision += 1
        return {}


class UndoAlgebraTests(unittest.TestCase):
    def test_a_churn_rewrite_during_the_check_is_not_an_undo_bug(self) -> None:
        # Soak d6710c671734: the churn thread prepended "churn 13\n" to the open
        # CHANGELOG.md between the check's two undos (at the bottom of the
        # history, so neither applied); the buffer reloaded it and the check
        # blamed "0 undo(s) then 0 redo(s)" for the 9 new characters.
        app = _UndoEditor(["v0"], at=0)
        lines = []

        def churn_on_second_undo(line: str) -> None:
            lines.append(line)
            if lines.count("undo") == 2:
                app.churn(13)

        app.on_line = churn_on_second_undo
        oracle = monkey_oracles.UndoAlgebra(app, random.Random(0))
        self.assertEqual(oracle.check(asked=2), [])

    def test_a_reload_from_a_churn_just_before_the_check_is_not_an_undo_bug(self) -> None:
        # The row is recorded when the write lands, but the buffer reloads from
        # the file watcher a moment later -- inside the check.
        app = _UndoEditor(["v0"], at=0)
        app.actions.append({"churn": "CHANGELOG.md", "counter": 4,
                            "t": round(time.monotonic(), 3)})
        reloaded = []

        def late_reload(line: str) -> None:
            if not reloaded:
                reloaded.append(line)
                app.text = "churn 4\n" + app.text
                app.revision += 1

        app.on_line = late_reload
        oracle = monkey_oracles.UndoAlgebra(app, random.Random(0))
        self.assertEqual(oracle.check(asked=1), [])

    def test_a_redo_that_loses_text_is_still_reported(self) -> None:
        app = _UndoEditor(["v0", "v1", "v12"], at=2, redo_drops=True)
        # A churn of another file, and an old one of this file, excuse nothing.
        app.actions += [{"churn": "README.md", "counter": 1, "t": round(time.monotonic(), 3)},
                        {"churn": "CHANGELOG.md", "counter": 0, "t": round(time.monotonic() - 60, 3)}]
        oracle = monkey_oracles.UndoAlgebra(app, random.Random(0))
        found = oracle.check(asked=2)
        self.assertEqual(len(found), 1)
        self.assertIn("2 undo(s) then 2 redo(s) changed the text of CHANGELOG.md", found[0][1])


if __name__ == "__main__":
    unittest.main()
