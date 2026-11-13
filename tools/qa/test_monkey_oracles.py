#!/usr/bin/env python3
"""Unit tests for monkey_oracles that need no app: a fake answers the control
queries the oracle asks."""
from __future__ import annotations

import random
import time
import sys
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
        self.actions.append({"external": row})

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

if __name__ == "__main__":
    unittest.main()
