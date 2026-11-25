#!/usr/bin/env python3
"""Unit tests for monkey_oracles that need no app: a fake answers the control
queries the oracle asks."""
from __future__ import annotations

import collections
import contextlib
import hashlib
import json
import os
import random
import re
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
            shown = [p for p in self.catalog if self.loaded[p] and (p == self.active or (args or {}).get("all"))]
            return [dict(doc) for p in shown for doc in self.docs[p]]
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


class ForgetFilesOfTests(unittest.TestCase):
    def test_a_file_reached_through_a_link_is_forgotten_with_the_link(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "crlf.txt"
            target.write_text("x\r\n")
            link = Path(folder) / "link-to-crlf.txt"
            link.symlink_to("crlf.txt")
            other = Path(folder) / "other.txt"
            other.write_text("y\n")
            disk = {str(target): "a", str(link): "a", str(other): "b"}
            monkey_oracles._forget_files_of(disk, {str(link)})
            self.assertEqual(disk, {str(other): "b"},
                             "crlf.txt is the file the save through its link wrote; other.txt is not")


class TerminalOriginsTests(unittest.TestCase):
    def test_alive_shells_are_grouped_by_the_code_that_started_them(self) -> None:
        # Soak 2d33d0bc377e: 70 interactive shells alive under five panes, and the
        # evidence counted them without saying which path made them or whether
        # their sessions were ever stopped.
        log = "\n".join([
            "microide terminal-life: start pid=10",
            "  #1 microide(+0x10) [0x1]",
            "  #2 microide(+0x20) [0x2]",
            "microide terminal-life: start pid=11",
            "  #1 microide(+0x10) [0x1]",
            "  #2 microide(+0x20) [0x2]",
            "microide terminal-life: start pid=12",
            "  #1 microide(+0x30) [0x3]",
            "microide terminal-life: stop pid=12",
            "microide terminal-life: start pid=13",
            "  #1 microide(+0x30) [0x3]",
            "microide terminal-life: stop pid=13",
            "microide redraw frame: frame=1",
        ])
        text = monkey_oracles.terminal_origins(log, {10, 11, 12, 99})
        lines = text.splitlines()
        self.assertEqual(lines[0], "2 alive shell(s) started here: 10 (never stopped) 11 (never stopped)")
        self.assertEqual(lines[1:3], ["  #1 microide(+0x10) [0x1]", "  #2 microide(+0x20) [0x2]"])
        self.assertEqual(lines[3], "1 alive shell(s) started here: 12 (stopped, still alive)")
        self.assertEqual(lines[-1], "1 alive child(ren) with no traced start: 99")

    def test_no_trace_says_nothing(self) -> None:
        self.assertEqual(monkey_oracles.terminal_origins("microide redraw frame: frame=1\n", {5}),
                         "1 alive child(ren) with no traced start: 5\n")


class RemoteSideEvidenceTests(unittest.TestCase):
    def test_a_differing_file_carries_what_the_sync_engine_decided_about_it(self) -> None:
        # Soak 37ec61829b08: the evidence said "host: No such file or directory"
        # and the mirror's bytes, and nothing of which push, delete or resolution
        # left the user's file on one side only.
        with tempfile.TemporaryDirectory() as tmp:
            work, host, mirror = Path(tmp) / "work", Path(tmp) / "host", Path(tmp) / "mirror"
            for folder in (work, host / "qa-weird", mirror / "qa-weird"):
                folder.mkdir(parents=True)
            (mirror / "qa-weird" / "unreadable.txt").write_text("mine\n")
            (work / "microide.log").write_text(
                "microide mirror-sync: push path=qa-weird/unreadable.txt base=- status=error refused=1\n"
                "microide mirror-sync: push path=qa-weird/other.txt base=- status=ok\n"
                "microide buffer-io: write path=qa-weird/unreadable.txt bytes=5\n"
                "microide mirror-sync: host-delete path=qa-weird/unreadable.txt before\n")

            class _App:
                pass
            app = _App()
            app.work = str(work)
            oracle = monkey_oracles.RemoteConvergence(app, mirror, host, "remote-open qa-local:/h")
            evidence = oracle._side_evidence("qa-weird/unreadable.txt")
            self.assertEqual(evidence["diff-qa-weird__unreadable.txt.sync.txt"].decode().splitlines(), [
                "microide.log: microide mirror-sync: push path=qa-weird/unreadable.txt base=- status=error refused=1",
                "microide.log: microide mirror-sync: host-delete path=qa-weird/unreadable.txt before",
            ])


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

    def _merge_result_beside_the_files_buffer(self, result_back: bool) -> list:
        """A merge's typed result and the file's own unsaved buffer (a diff's
        side), one path, two texts. The relaunch brings the file's buffer back on
        the file and, when `result_back`, the result in an Untitled tab."""
        app = _RelaunchingApp()
        path = "/p/b/scheduler.cpp"
        app.docs[1] = [{"path": path, "dirty": True, "sha256": "file", "bytes": 910, "project": "/p/b",
                        "title": "compare: scheduler.cpp", "surface": "compare"},
                       {"path": path, "dirty": True, "sha256": "result", "bytes": 669, "project": "/p/b",
                        "title": "merge: scheduler.cpp", "surface": "merge"}]
        relaunch = app.crash_and_relaunch

        def restore(*args, **kwargs):
            relaunch(*args, **kwargs)
            app.docs[1] = [{"path": path, "dirty": True, "sha256": "file", "bytes": 910, "project": "/p/b",
                            "title": "compare: scheduler.cpp", "surface": "compare"}]
            if result_back:
                app.docs[1].append({"path": "", "dirty": True, "sha256": "result", "bytes": 669,
                                    "project": "/p/b", "title": "Untitled-1", "surface": "editor"})
        app.crash_and_relaunch = restore
        oracle = monkey_oracles.DataOracle(app)
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            return [f[1] for f in oracle.kill9_and_check(random.Random(1), waited=True)]

    def test_a_merge_result_beside_the_files_unsaved_buffer_comes_back_untitled(self) -> None:
        # Soak 02922e8eed90: both texts were keyed by the one path; the result
        # now comes back in an Untitled tab, and the file's buffer on the file.
        self.assertEqual(self._merge_result_beside_the_files_buffer(result_back=True), [])

    def test_a_merge_result_beside_the_files_buffer_that_is_lost_is_reported(self) -> None:
        found = self._merge_result_beside_the_files_buffer(result_back=False)
        self.assertEqual(len(found), 1, found)
        self.assertIn("the unsaved merge result for scheduler.cpp did not come back", found[0])

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


    def _deferred_after_a_save(self, disk: bytes) -> list:
        """The unsaved buffer was written by an autosave just before the kill, so
        the relaunch reopens its tab clean and deferred -- not read until shown,
        and so absent from `documents` -- with `disk` in its file."""
        import hashlib
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "README.md"
            path.write_bytes(disk)
            app = _RelaunchingApp()
            app.active = 1
            app.docs[1] = [{"path": str(path), "dirty": True, "project": "/p/b",
                            "sha256": hashlib.sha256(b"unsaved text").hexdigest()}]
            relaunch = app.crash_and_relaunch

            def relaunch_deferred(*args, **kwargs):
                relaunch(*args, **kwargs)
                app.docs[1] = []  # the tab is back, its content still pending
            app.crash_and_relaunch = relaunch_deferred
            oracle = monkey_oracles.DataOracle(app)
            with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                    mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None), \
                    mock.patch.object(monkey_oracles.time, "monotonic", side_effect=range(0, 10**6, 100)):
                return oracle.kill9_and_check(random.Random(1), waited=True)

    def test_a_buffer_saved_before_the_kill_is_not_lost_though_its_tab_is_deferred(self) -> None:
        # Soak 93826423aa06: autosave wrote README.md, the kill landed in that
        # write's durable sequence, and the relaunch reopened the tab clean and
        # deferred -- the file holds the buffer -- which `documents` does not list.
        self.assertEqual(self._deferred_after_a_save(b"unsaved text"), [])

    def test_a_buffer_absent_after_the_kill_whose_file_does_not_hold_it_is_lost(self) -> None:
        # The negative control: the file has other bytes, so the buffer is gone.
        found = self._deferred_after_a_save(b"older text")
        self.assertEqual(len(found), 1, found)
        self.assertIn("did not come back", found[0][1])

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

    def _autosaved_before_the_kill(self, holds: bool) -> list:
        """`editor.autosave = after_delay` wrote the dirty buffer between the
        oracle's snapshot and the kill; a clean tab shows the same file through a
        link. The relaunch brings the buffer back clean; `holds` says whether its
        file holds what it came back with."""
        import hashlib
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "a"
            root.mkdir()
            target = root / "names.txt"
            target.write_text("before\n")
            link = root / "link0"
            link.symlink_to("names.txt")
            edited = b"edited\n"

            class _AutosavingApp(_RelaunchingApp):
                ROOTS = (str(root), "/p/b")

                def __init__(self) -> None:
                    super().__init__()
                    self.project = self.ROOTS[0]
                    self.active = 0
                    self.docs = {
                        0: [{"path": str(target), "dirty": True, "project": self.ROOTS[0],
                             "sha256": hashlib.sha256(edited).hexdigest()},
                            {"path": str(link), "dirty": False, "project": self.ROOTS[0],
                             "sha256": monkey_oracles._sha256_file(link)}],
                        1: [],
                    }

                def crash_and_relaunch(self, *args, **kwargs) -> None:
                    target.write_bytes(edited if holds else b"a different save\n")
                    self.docs[0][0] = {**self.docs[0][0], "dirty": False}
                    self.docs[0][1] = {**self.docs[0][1], "sha256": monkey_oracles._sha256_file(link)}
                    super().crash_and_relaunch(*args, **kwargs)

            oracle = monkey_oracles.DataOracle(_AutosavingApp())
            with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                    mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
                return oracle.kill9_and_check(random.Random(1), waited=True)

    def test_an_autosave_before_the_kill_is_not_a_lost_save_or_a_changed_link(self) -> None:
        # Soak 1bee2822b458 / fd10a1441e1d: the buffer was reported "marked saved;
        # its file does not hold it" without the file being read, and the link to
        # the file the autosave wrote as "changed on disk, and nothing saved it".
        self.assertEqual(self._autosaved_before_the_kill(holds=True), [])

    def test_a_buffer_back_clean_whose_file_does_not_hold_it_is_still_reported(self) -> None:
        found = [f[1] for f in self._autosaved_before_the_kill(holds=False)]
        self.assertIn("the unsaved buffer names.txt came back after a kill -9 marked saved; "
                      "its file does not hold it", found)

    def _transformed_by_the_autosave(self, logged: bytes) -> list:
        """The autosave wrote the dirty buffer before the kill with a final newline
        added, and the relaunch reopened its tab clean and deferred (absent from
        `documents`). The killed process logged writing `logged`."""
        import hashlib
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "no-final-newline.txt"
            path.write_bytes(b"text")
            app = _RelaunchingApp()
            app.docs[1] = [{"path": str(path), "dirty": True, "project": "/p/b",
                            "sha256": hashlib.sha256(b"text edited").hexdigest()}]
            log = [b""]
            app._log_size = lambda: len(log[0])
            app.log_since = lambda offset, max_bytes=0: log[0][offset:].decode()
            relaunch = app.crash_and_relaunch

            def autosave_then_relaunch(*args, **kwargs):
                path.write_bytes(b"text edited\n")
                sha = hashlib.sha256(logged).hexdigest()[:12]
                log[0] += (f"microide buffer-io: write path={path} doc=0x1 rev=2 dirty=0 bytes={len(logged)} "
                           f"sha={sha} disk_exists=1 disk_size=12 disk_mtime=1 disk_racy=0\n").encode()
                relaunch(*args, **kwargs)
                app.docs[1] = []  # back clean and deferred
            app.crash_and_relaunch = autosave_then_relaunch
            oracle = monkey_oracles.DataOracle(app)
            with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                    mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None), \
                    mock.patch.object(monkey_oracles.time, "monotonic", side_effect=range(0, 10**6, 100)):
                return oracle.kill9_and_check(random.Random(1), waited=True)

    def test_a_save_with_transforms_before_the_kill_is_not_a_lost_buffer(self) -> None:
        # 1bee2822b458's run: autosave wrote rtl.txt, code.py and
        # no-final-newline.txt -- trailing whitespace trimmed, a final newline
        # added -- and the relaunch reopened them clean and deferred; the file was
        # not the snapshot's text, so all three read as "did not come back".
        self.assertEqual(self._transformed_by_the_autosave(b"text edited\n"), [])

    def test_a_logged_save_the_file_does_not_hold_does_not_excuse_a_lost_buffer(self) -> None:
        found = self._transformed_by_the_autosave(b"something else\n")
        self.assertEqual(len(found), 1, found)
        self.assertIn("did not come back", found[0][1])

    def test_a_buffer_back_clean_through_the_saves_transforms_is_not_changed(self) -> None:
        # fd10a1441e1d's verify: rtl.txt, dirty at 4103 bytes, came back CLEAN at
        # 4104 -- the autosave added the final newline -- and read as "came back
        # with different content", though the app logged writing those 4104 bytes.
        import hashlib
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "rtl.txt"
            path.write_bytes(b"text")
            saved = b"text edited\n"
            app = _RelaunchingApp()
            app.docs[1] = [{"path": str(path), "dirty": True, "project": "/p/b",
                            "sha256": hashlib.sha256(b"text edited").hexdigest()}]
            log = [b""]
            app._log_size = lambda: len(log[0])
            app.log_since = lambda offset, max_bytes=0: log[0][offset:].decode()
            relaunch = app.crash_and_relaunch

            def autosave_then_relaunch(*args, **kwargs):
                path.write_bytes(saved)
                sha = hashlib.sha256(saved).hexdigest()
                log[0] += (f"microide buffer-io: write path={path} doc=0x1 rev=2 dirty=0 bytes={len(saved)} "
                           f"sha={sha[:12]} disk_exists=1 disk_size=12 disk_mtime=1 disk_racy=0\n").encode()
                relaunch(*args, **kwargs)
                app.docs[1] = [{**app.docs[1][0], "dirty": False, "sha256": sha}]
            app.crash_and_relaunch = autosave_then_relaunch
            with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                    mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
                found = monkey_oracles.DataOracle(app).kill9_and_check(random.Random(1), waited=True)
            self.assertEqual(found, [], found)

    def test_a_link_to_a_file_the_app_rewrote_unchanged_is_not_reported(self) -> None:
        # fd10a1441e1d's verify: the autosave rewrote "名前 with spaces.txt" with the
        # text it already held (its buffer dirty, its content not), so that buffer
        # took the "file holds the snapshot" branch -- which excused nothing -- and
        # link0, a link to it whose clean tab predates the save, read as changed.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "a"
            root.mkdir()
            target = root / "names.txt"
            target.write_text("old\n")
            link = root / "link0"
            link.symlink_to("names.txt")

            class _App(_RelaunchingApp):
                ROOTS = (str(root), "/p/b")

                def __init__(self) -> None:
                    super().__init__()
                    self.project = self.ROOTS[0]
                    self.active = 0
                    self.docs = {0: [{"path": str(link), "dirty": False, "project": self.ROOTS[0],
                                      "sha256": monkey_oracles._sha256_file(link)}], 1: []}

            app = _App()
            log = [b""]
            app._log_size = lambda: len(log[0])
            app.log_since = lambda offset, max_bytes=0: log[0][offset:].decode()
            relaunch = app.crash_and_relaunch

            def save_then_relaunch(*args, **kwargs):
                target.write_text("new\n")
                sha = monkey_oracles._sha256_file(target)
                log[0] += (f"microide buffer-io: write path={target} doc=0x1 rev=2 dirty=0 bytes=4 "
                           f"sha={sha[:12]} disk_exists=1 disk_size=4 disk_mtime=1 disk_racy=0\n").encode()
                relaunch(*args, **kwargs)
            app.crash_and_relaunch = save_then_relaunch
            with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                    mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
                found = monkey_oracles.DataOracle(app).kill9_and_check(random.Random(1), waited=True)
            self.assertEqual(found, [], found)

    def test_the_write_lines_name_paths_with_spaces(self) -> None:
        log = ("microide buffer-io: write path=/a/ünï cødé/名前 with spaces.txt doc=0x7 rev=103 dirty=0 "
               "bytes=16 sha=ce68aabc783e disk_exists=1 disk_size=16 disk_mtime=1 disk_racy=1\n"
               "microide buffer-io: session-stage path=/a/b.txt doc=0x8 rev=1 dirty=1 bytes=1 sha=aa\n")
        self.assertEqual(monkey_oracles._writes_logged(log), {"/a/ünï cødé/名前 with spaces.txt": "ce68aabc783e"})

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


class CaretBoundsReportTests(unittest.TestCase):
    LOG = ("microide redraw frame: frame=1\n"
           "microide caret-bounds: after a move or an edit: /p/bom.txt has its caret at 2:801 "
           "(line length 0, 3 lines); the code that left it:\n"
           "  #1 /soak/bin/microide(+0xc4ccdc) [0x629b2a4b4cdc]\n"
           "  #2 /usr/lib/x86_64-linux-gnu/libc.so.6(__libc_start_main+0x88) [0x700eee22a718]\n"
           "microide input: key=Return\n")

    def test_without_a_binary_the_report_is_kept_as_logged(self) -> None:
        self.assertEqual(monkey_oracles.caret_bounds_reports(self.LOG), "".join(self.LOG.splitlines(True)[1:4]))

    def test_with_a_binary_the_frames_go_through_the_crash_stack_resolver(self) -> None:
        # Soak 872c8984d39b: the raw offsets cost a hand addr2line run. With no
        # such binary the resolver keeps the offsets, numbered in the crash-stack
        # shape; with the soak's binary they name the function and line.
        text = monkey_oracles.caret_bounds_reports(self.LOG, Path("/nonexistent/microide"))
        lines = text.splitlines()
        self.assertTrue(lines[0].startswith("microide caret-bounds: after a move or an edit"))
        self.assertEqual(lines[1], "#0 0xc4ccdc in ?? ?? [microide+0xc4ccdc]")
        self.assertEqual(lines[2], "#1 0x88 in __libc_start_main ?? [libc.so.6+0x88]")


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

    def test_the_frames_are_counted_by_the_event_that_asked_for_them(self) -> None:
        # Soak a64655101a5a: 60 event-partial frames a second, and nothing said
        # which event kept asking.
        app = mock.Mock()
        app.ui.return_value = {"elements": []}
        oracle = monkey_oracles.IdleOracle(app)
        frames = [f"microide redraw frame: frame={i} reason=event-partial bounds=0,86,515,1198 event=plugin-thread"
                  for i in range(4)]
        frames.append("microide redraw frame: frame=9 reason=event-partial bounds=0,86,515,1198 event=0x400")
        lines = oracle._repainted_regions(frames)
        self.assertEqual(lines[0], "frames by the event that asked for them: 4 x plugin-thread, 1 x 0x400")

    def test_frames_without_bounds_add_nothing(self) -> None:
        oracle = monkey_oracles.IdleOracle(mock.Mock())
        self.assertEqual(oracle._repainted_regions(["microide redraw frame: frame=1 dirty=1"]), [])


class ResourceOracleTests(unittest.TestCase):
    """Soak 6da9bc4a526a / d1e39df0281e / bdea62052550: the terminal profile keeps
    up to 200 terminals open, each with its pty, reader thread and shell, and every
    run crossed the fixed budgets with nothing leaked (20 opened and closed return
    to the baseline exactly). What the open panes hold is not growth."""

    def test_a_leak_finding_says_which_threads_and_where_the_memory_is(self) -> None:
        # The counts alone said "threads grew by 70" and "rss grew by 1.6 GB";
        # the bundle names the threads against the baseline and the mappings.
        import collections
        import os
        oracle = monkey_oracles.ResourceOracle.__new__(monkey_oracles.ResourceOracle)
        oracle.baseline_threads = collections.Counter({"gone-worker": 3})
        oracle.app = None  # no run: no log to read terminal starts from
        oracle.rss_steps = []
        evidence = oracle._what_it_holds(os.getpid())
        threads = evidence["threads.txt"].decode()
        self.assertIn("(   3, -3)  gone-worker", threads)
        self.assertIn("largest resident mappings (kB):", evidence["memory.txt"].decode())


    def test_the_rss_timeline_names_the_actions_of_the_largest_growth(self) -> None:
        # Soak e53948e728c8: 3.5 GB held after 150 steps, sampled every 50, and
        # nothing said which action took it.
        app = mock.Mock()
        app.actions = [{"key": "a"}, {"key": "b"}, {"foreign-clipboard": "x" * 1000}, {"key": "c"}]
        oracle = monkey_oracles.ResourceOracle(app)
        oracle.rss_steps = [(0, 1, 100), (1, 2, 110), (2, 3, 900_000), (3, 4, 900_010)]
        timeline = oracle.rss_timeline(jumps=1, around=0)
        self.assertIn("+899890 kB between step 1 and step 2 (actions 2..3):", timeline)
        self.assertIn('"foreign-clipboard": "xxxx', timeline)
        self.assertIn("(1025 chars)", timeline)

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

    def __init__(self, history: list[str], at: int, redo_drops: bool = False, kind: str = "editor") -> None:
        self.kind = kind
        self.project = "/w"
        self.actions: list[dict] = []
        self.last_ui = {"focus": "editor"}
        self.history = history
        self.at = at
        self.text = history[at]
        self.revision = 1
        self.redo_drops = redo_drops
        self.on_line = None  # called with each line sent, before it applies
        # A language server's edit (a format-document) asked for and not yet
        # answered: it lands on the buffer as one new history entry when the
        # app next processes its answer.
        self.pending_edit: str | None = None

    def land_pending_edit(self) -> None:
        if self.pending_edit is not None:
            self.history = self.history[:self.at + 1] + [self.pending_edit]
            self.at += 1
            self.text = self.pending_edit
            self.revision += 1
            self.pending_edit = None

    def query(self, verb, args=None):
        if verb == "status":
            in_flight = int(self.pending_edit is not None)
            # The answer arrives while the caller waits for it.
            self.land_pending_edit()
            return {"lspRequestsInFlight": in_flight}
        assert verb == "buffer", verb
        return {"kind": self.kind, "path": "/w/CHANGELOG.md", "text": self.text,
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

    def test_a_format_answered_during_the_check_is_not_an_undo_bug(self) -> None:
        # Soak 76e3925879dc: format-document went out, the check read the
        # buffer, and clangd's 199 edits landed before the first undo. Four
        # undos took back the formatting and three edits, four redos put them
        # all back, and the text was format(before) byte for byte: an exact
        # history, compared with a text read before the formatting landed.
        app = _UndoEditor(["v0", "v1", "v12"], at=2)
        app.pending_edit = "v12 formatted"

        def answer_before_the_first_undo(line: str) -> None:
            app.land_pending_edit()

        app.on_line = answer_before_the_first_undo
        oracle = monkey_oracles.UndoAlgebra(app, random.Random(0))
        self.assertEqual(oracle.check(asked=3), [])
        self.assertEqual(app.text, "v12 formatted")

    def test_a_redo_that_loses_text_is_still_reported(self) -> None:
        app = _UndoEditor(["v0", "v1", "v12"], at=2, redo_drops=True)
        # A churn of another file, and an old one of this file, excuse nothing.
        app.actions += [{"churn": "README.md", "counter": 1, "t": round(time.monotonic(), 3)},
                        {"churn": "CHANGELOG.md", "counter": 0, "t": round(time.monotonic() - 60, 3)}]
        oracle = monkey_oracles.UndoAlgebra(app, random.Random(0))
        found = oracle.check(asked=2)
        self.assertEqual(len(found), 1)
        self.assertIn("2 undo(s) then 2 redo(s) changed the text of CHANGELOG.md", found[0][1])


class UndoAlgebraSurfaceTests(unittest.TestCase):
    def test_a_diff_side_and_a_merge_result_are_held_to_the_rule(self) -> None:
        # The rule was asked of editor buffers alone; a diff's editable side and
        # a merge's result have histories of their own.
        for kind in ("compare", "merge"):
            app = _UndoEditor(["v0", "v1", "v12"], at=2, redo_drops=True, kind=kind)
            found = monkey_oracles.UndoAlgebra(app, random.Random(0)).check(asked=2)
            self.assertEqual(len(found), 1, kind)

    def test_a_terminal_or_an_overlay_is_not(self) -> None:
        app = _UndoEditor(["v0", "v1"], at=1, redo_drops=True, kind="terminal")
        self.assertEqual(monkey_oracles.UndoAlgebra(app, random.Random(0)).check(asked=1), [])


class _DocumentsApp:
    """Answers `documents` with a fixed list."""

    def __init__(self, docs: list[dict]) -> None:
        self.docs = docs

    def query(self, verb, args=None):
        assert verb == "documents", verb
        return [dict(doc) for doc in self.docs]


class UntitledNamesTests(unittest.TestCase):
    def _doc(self, title: str, sha: str, project: str = "/p") -> dict:
        return {"path": "", "title": title, "dirty": True, "sha256": sha, "bytes": len(sha), "project": project}

    def test_two_different_unsaved_buffers_with_one_title_are_reported(self) -> None:
        # Soak 93ba7c8703e3: two different buffers both "Untitled-1"; a kill -9's
        # restore then brought both back with one of their texts.
        app = _DocumentsApp([self._doc("Untitled-1", "aa"), self._doc("Untitled-1", "bbbb")])
        found = monkey_oracles.DataOracle(app).check_untitled_names()
        self.assertEqual(len(found), 1)
        self.assertIn("2 different unsaved buffers are all titled Untitled-1", found[0][1])

    def test_one_buffer_in_two_panes_and_other_projects_are_not(self) -> None:
        app = _DocumentsApp([self._doc("Untitled-1", "aa"), self._doc("Untitled-1", "aa"),
                             self._doc("Untitled-1", "cc", project="/q"), self._doc("Untitled-2", "dd")])
        self.assertEqual(monkey_oracles.DataOracle(app).check_untitled_names(), [])



class FrameLatencyTest(unittest.TestCase):
    class FakeApp:
        def __init__(self) -> None:
            self.log = ""

        def _log_size(self) -> int:
            return len(self.log.encode())

        def log_since(self, offset: int, max_bytes: int) -> str:
            return self.log.encode()[offset:offset + max_bytes].decode()

    @staticmethod
    def frame(index: int, ms: float) -> str:
        return (f"microide redraw frame: frame={index} reason=prepared requested=partial rendered=partial "
                f"promoted=no dirty=1 clips=1 elapsed={ms:.2f} ms bounds=0,0,0,0\n")

    def test_a_slow_frame_is_reported_once_and_a_quick_one_never(self) -> None:
        app = self.FakeApp()
        oracle = monkey_oracles.FrameLatency(app, budget=0.5)
        oracle.budget_ms = 500.0
        self.assertEqual(oracle.check(), [], "the first check only notes where the log is")
        app.log += self.frame(1, 12.0)
        self.assertEqual(oracle.check(), [], "a 12 ms frame is no finding")
        app.log += self.frame(2, 800.0)
        found = oracle.check()
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][0], "latency")
        self.assertIn("800 ms", found[0][1])
        self.assertIn("frame-latency.txt", oracle.evidence)
        app.log += self.frame(3, 900.0)
        self.assertEqual(oracle.check(), [], "a second slow frame of the same reason is not filed again")


class InvariantOracleTests(unittest.TestCase):
    def app(self, answers):
        app = mock.Mock()
        app.log_since.return_value = ""
        asked = []

        def query(verb, args=None):
            asked.append((verb, args))
            answer = answers[verb]
            if isinstance(answer, Exception):
                raise answer
            return answer
        app.query.side_effect = query
        return app, asked

    def test_each_broken_rule_is_one_finding_with_the_state_beside_it(self):
        violations = {"violations": [{"rule": "tab-payload", "detail": "tab 2 has no editor"},
                                     {"rule": "untitled-unique", "detail": "Untitled-1 twice"}]}
        app, asked = self.app({"invariants": violations, "tabs": [{"title": "a"}], "documents": [], "editor": {}})
        oracle = monkey_oracles.InvariantOracle(app)
        found = oracle.check(deep=True)
        self.assertEqual(found, [("invariant", "tab-payload: tab 2 has no editor"),
                                 ("invariant", "untitled-unique: Untitled-1 twice")])
        self.assertEqual(asked[0], ("invariants", {"deep": True}))
        self.assertEqual(set(oracle.evidence), {"invariants.json", "tabs.json", "documents.json", "editor.json"})
        self.assertIn(b"tab 2 has no editor", oracle.evidence["invariants.json"])
        self.assertEqual(oracle.checks, 1)
        self.assertEqual(oracle.check(), [], "a rule already reported is not filed again")
        self.assertEqual(oracle.evidence, {}, "and a check that files nothing keeps no evidence")

    def test_a_new_rule_is_filed_after_an_old_one(self):
        app, _ = self.app({"invariants": {"violations": [{"rule": "a", "detail": "x"}]},
                           "tabs": [], "documents": [], "editor": {}})
        oracle = monkey_oracles.InvariantOracle(app)
        oracle.check()
        app.query.side_effect = lambda verb, args=None: (
            {"violations": [{"rule": "a", "detail": "x"}, {"rule": "b", "detail": "y"}]}
            if verb == "invariants" else {})
        self.assertEqual(oracle.check(), [("invariant", "b: y")])

    def test_a_clean_answer_files_nothing(self):
        app, asked = self.app({"invariants": {"ok": True, "violations": []}})
        oracle = monkey_oracles.InvariantOracle(app)
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.checks, 1)
        self.assertEqual([verb for verb, _ in asked], ["invariants"], "no evidence is gathered for a clean answer")

    def test_a_binary_without_the_query_is_not_asked_again(self):
        app, asked = self.app({"invariants": monkey_oracles.q.QaFailure("unknown query \"invariants\"")})
        oracle = monkey_oracles.InvariantOracle(app)
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.check(), [])
        self.assertEqual(len(asked), 1)
        self.assertTrue(oracle.unavailable)
        self.assertEqual(oracle.checks, 0)

    def test_another_failure_or_a_gone_app_is_no_finding_and_is_asked_again(self):
        app, asked = self.app({"invariants": monkey_oracles.q.QaFailure("timed out")})
        oracle = monkey_oracles.InvariantOracle(app)
        self.assertEqual(oracle.check(), [])
        self.assertFalse(oracle.unavailable)
        app.query.side_effect = monkey_oracles.q.AppGone("gone")
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.checks, 0)


class LivenessTests(unittest.TestCase):
    def app(self, what="query 'ui'", seconds=0.0, sched=None, stall="", waits="", helpers=None, profile=""):
        from types import SimpleNamespace
        return SimpleNamespace(slowest_answer=(what, seconds), slowest_sched=sched, slowest_stall=stall,
                               slowest_waits=waits, slowest_helpers=helpers, slowest_profile=profile)

    def oracle(self, app):
        with mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 1.0):
            return monkey_oracles.Liveness(app, budget=2.0)

    def test_an_answer_within_the_budget_files_nothing_and_resets_the_window(self):
        app = self.app(seconds=2.0, stall="x", waits="w", helpers=(1.0, 2.0), profile="p")
        self.assertEqual(self.oracle(app).check(), [])
        self.assertEqual((app.slowest_answer, app.slowest_sched, app.slowest_stall, app.slowest_waits,
                          app.slowest_helpers, app.slowest_profile), (("", 0.0), None, "", "", None, ""))

    def test_a_late_answer_names_its_subject_and_carries_the_stall(self):
        app = self.app(seconds=3.5, stall="#0 Frame()", waits="futex_wait", profile="perf lines")
        oracle = self.oracle(app)
        self.assertEqual(oracle.check(), [("latency", "the ui query took 3.50s to answer (budget 2.0s)")])
        latency = oracle.evidence["latency.txt"].decode()
        self.assertIn("query 'ui' took 3.50s (budget 2.0s)", latency)
        self.assertIn("#0 Frame()", latency)
        self.assertIn("futex_wait", latency)
        self.assertEqual(oracle.evidence["latency-profile.txt"], b"perf lines")
        oracle.app.slowest_answer = ("query 'ui'", 9.0)
        self.assertEqual(oracle.check(), [], "one finding per subject per run")
        oracle.app.slowest_answer = (repr("format-document"), 9.0)
        self.assertEqual(len(oracle.check()), 1, "another subject is filed")

    def test_a_late_answer_without_a_watchdog_stack_says_so(self):
        oracle = self.oracle(self.app(seconds=3.0))
        oracle.check()
        self.assertIn("printed nothing", oracle.evidence["latency.txt"].decode())
        self.assertNotIn("latency-profile.txt", oracle.evidence)

    def test_where_the_main_thread_sat(self):
        computing = self.oracle(self.app(seconds=4.0, sched=(3.0, 0.5)))
        self.assertTrue(computing.check()[0][1].endswith("its main thread was mostly computing"))
        self.assertIn("main thread over 4.00s", computing.evidence["latency.txt"].decode())
        blocked = self.oracle(self.app(seconds=4.0, sched=(0.5, 0.5)))
        self.assertTrue(blocked.check()[0][1].endswith("mostly blocked (I/O, a lock, a child)"))

    def test_a_starved_machine_is_counted_not_filed(self):
        oracle = self.oracle(self.app(seconds=4.0, sched=(0.5, 2.0)))
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.starved, 1)
        self.assertEqual(oracle.reported, set(), "a starved answer leaves its subject open")

    def test_a_wait_on_starved_helper_threads_is_counted_not_filed(self):
        oracle = self.oracle(self.app(seconds=4.0, sched=(0.5, 0.5), helpers=(2.5, 4.0)))
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.starved, 1)
        computing = self.oracle(self.app(seconds=4.0, sched=(3.0, 0.5), helpers=(2.5, 4.0)))
        self.assertEqual(len(computing.check()), 1, "a main thread that computed is the app's own time")


class IdleOracleCheckTests(unittest.TestCase):
    """IdleOracle.check: the wake rate over a settled window, and what it saw."""

    def run_window(self, wakes, logged=(), samples=("do_poll poll",)):
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        log = directory / "app.log"
        log.write_text("before the window\n")
        app = mock.Mock()
        app.log_path = log
        app.ui.return_value = {"elements": []}
        oracle = monkey_oracles.IdleOracle(app)
        counts = iter(wakes)
        sampled = iter(samples * 1000)

        def sample():
            if logged and log.read_text() == "before the window\n":
                with open(log, "a") as out:
                    out.write("".join(line + "\n" for line in logged))
            return next(sampled)
        with mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.04), \
                mock.patch.object(oracle, "_main_thread_wakes", side_effect=lambda: next(counts)), \
                mock.patch.object(oracle, "_sample_main_thread", side_effect=sample):
            found = oracle.check()
        app.settle_quiet.assert_called_once_with(most=1.0)
        return oracle, found

    def test_a_quiet_main_thread_files_nothing(self):
        oracle, found = self.run_window([100, 104])
        self.assertEqual(found, [])
        self.assertFalse(oracle.reported)
        self.assertIsNone(oracle.evidence)

    def test_a_busy_main_thread_is_one_finding_with_what_it_did(self):
        frames = [f"microide redraw frame: frame={i} reason=scheduled-partial dirty=1 bounds=1,2,3,4"
                  for i in range(3)]
        oracle, found = self.run_window([0, 40], logged=frames + ["other line"],
                                        samples=("ep_poll epoll_wait", "futex_wait futex"))
        self.assertEqual(found, [("idle", "the main thread woke 1000 times a second while the app was idle "
                                          "(budget 250)")])
        self.assertTrue(oracle.reported)
        self.assertIn("voluntary wakes/s: 1000 (budget 250)", oracle.evidence)
        self.assertIn("frames in the window: 3 {'scheduled-partial': 3}", oracle.evidence)
        self.assertIn("log lines in the window: 4", oracle.evidence)
        self.assertIn("ep_poll epoll_wait", oracle.evidence)
        self.assertNotIn("before the window", oracle.evidence, "only what the window logged")
        oracle.app.settle_quiet.reset_mock()
        self.assertEqual(oracle.check(), [], "one finding per run")
        oracle.app.settle_quiet.assert_not_called()

    def test_the_budget_is_per_second_at_its_edge(self):
        _, found = self.run_window([0, 10])
        self.assertEqual(found, [], "exactly the budget is within it")
        _, found = self.run_window([0, 11])
        self.assertEqual(len(found), 1)

    def test_an_app_that_went_away_files_nothing(self):
        for wakes in ([None, 500], [0, None]):
            oracle, found = self.run_window(wakes)
            self.assertEqual(found, [])
            self.assertFalse(oracle.reported)


class Kill9VerdictTests(unittest.TestCase):
    """What kill9_and_check says about each buffer once the relaunch is back:
    the branches mutation-test.sh found no test held (265 mutants survived)."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(self.dir, ignore_errors=True))

    def kill(self, before, after, waited=True, point=None, kill_at=None, between=None):
        app = _RelaunchingApp()
        app.docs[1] = [dict(doc) for doc in before]
        relaunch = app.crash_and_relaunch

        def restore(*args, **kwargs):
            relaunch(*args, **kwargs)
            if between is not None:
                between()
            app.docs[1] = [dict(doc) for doc in after]
        app.crash_and_relaunch = restore
        oracle = monkey_oracles.DataOracle(app)
        patches = [mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0),
                   mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None),
                   mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.001)]
        if kill_at is not None:
            patches.append(mock.patch.object(oracle, "_kill_at", side_effect=kill_at))
        with contextlib.ExitStack() as stack:
            for patch in patches:
                stack.enter_context(patch)
            found = oracle.kill9_and_check(random.Random(1), waited=waited, point=point)
        self.assertTrue(all(kind == "data" for kind, _ in found), found)
        return [message for _, message in found]

    def file(self, name, text):
        path = self.dir / name
        path.write_text(text)
        return str(path), monkey_oracles._sha256_file(path)

    def test_a_buffer_back_as_it_was_staged_is_fine(self):
        doc = {"path": "/p/b/y.txt", "dirty": True, "sha256": "bb", "bytes": 3, "project": "/p/b"}
        self.assertEqual(self.kill([doc], [doc]), [])

    def test_a_buffer_that_is_lost_is_named(self):
        doc = {"path": "/p/b/y.txt", "dirty": True, "sha256": "bb", "bytes": 3, "project": "/p/b"}
        found = self.kill([doc], [])
        self.assertEqual(len(found), 1, found)
        self.assertTrue(found[0].startswith("the unsaved buffer y.txt did not come back after a kill -9"), found)

    def test_a_buffer_back_with_other_text_says_both_sizes(self):
        before = {"path": "/p/b/y.txt", "dirty": True, "sha256": "bb", "bytes": 3, "project": "/p/b"}
        after = {**before, "sha256": "cc", "bytes": 7}
        self.assertEqual(self.kill([before], [after]),
                         ["the unsaved buffer y.txt came back after a kill -9 with different content (3 -> 7 bytes)"])

    def test_a_buffer_back_marked_saved_must_be_in_its_file(self):
        path, held = self.file("y.txt", "saved text\n")
        before = {"path": path, "dirty": True, "sha256": "bb", "bytes": 3, "project": "/p/b"}
        self.assertEqual(self.kill([before], [{**before, "dirty": False}]),
                         ["the unsaved buffer y.txt came back after a kill -9 marked saved; its file does not hold it"])
        self.assertEqual(self.kill([{**before, "sha256": held}], [{**before, "sha256": held, "dirty": False}]), [],
                         "back clean and its file holds it: it was saved before the kill")

    def test_an_untitled_buffer_is_named_by_its_title(self):
        doc = {"path": "", "title": "Untitled-3", "dirty": True, "sha256": "bb", "bytes": 3, "project": "/p/b"}
        found = self.kill([doc], [])
        self.assertTrue(found and found[0].startswith("the unsaved buffer Untitled-3 did not come back"), found)

    def test_a_kill_without_the_wait_holds_no_buffer_to_coming_back(self):
        doc = {"path": "/p/b/y.txt", "dirty": True, "sha256": "bb", "bytes": 3, "project": "/p/b"}
        self.assertEqual(self.kill([doc], [], waited=False), [])

    def test_a_clean_file_changed_on_disk_across_the_kill_is_reported(self):
        path, held = self.file("x.txt", "clean\n")
        doc = {"path": path, "dirty": False, "sha256": held, "bytes": 6, "project": "/p/b"}
        found = self.kill([doc], [doc], between=lambda: Path(path).write_text("changed\n"))
        self.assertEqual(found, ["x.txt changed on disk across a kill -9 and relaunch, and nothing saved it"])
        self.assertEqual(self.kill([doc], [doc]), [], "an untouched file is fine")

    def test_a_kill_at_a_named_point_says_where(self):
        doc = {"path": "/p/b/y.txt", "dirty": True, "sha256": "bb", "bytes": 3, "project": "/p/b"}
        edited = {**doc, "sha256": "edited"}
        found = self.kill([doc], [], point="kill:durable.renaming@session|edit",
                          kill_at=lambda point: (True, [edited], None))
        self.assertEqual(len(found), 1, found)
        self.assertTrue(found[0].endswith(" (killed at durable.renaming@session)"), found)
        self.assertEqual(self.kill([doc], [edited], point="kill:durable.renaming@session|edit",
                                   kill_at=lambda point: (True, [edited], None)), [],
                         "the edit the trigger made may come back instead of the staged text")
        found = self.kill([doc], [], point="kill:durable.renaming@session|edit",
                          kill_at=lambda point: (False, [doc], None))
        self.assertTrue(found and "killed at" not in found[0], "a trigger that did not die names no point")

    def test_a_merge_result_back_on_its_file_is_not_back(self):
        path = "/p/b/scheduler.cpp"
        before = [{"path": path, "dirty": True, "sha256": "file", "bytes": 9, "project": "/p/b",
                   "title": "compare: scheduler.cpp", "surface": "compare"},
                  {"path": path, "dirty": True, "sha256": "result", "bytes": 6, "project": "/p/b",
                   "title": "merge: scheduler.cpp", "surface": "merge"}]
        after = [before[0], {"path": "/p/b/other.cpp", "dirty": True, "sha256": "result", "bytes": 6,
                             "project": "/p/b", "title": "other.cpp", "surface": "editor"}]
        found = self.kill(before, after)
        self.assertTrue(any("the unsaved merge result for scheduler.cpp did not come back" in f for f in found),
                        found)


def _diff_row(kind="changed", left=1, right=1, left_text="a", right_text="b", left_spans=None, right_spans=None,
              hunk=0, **extra):
    row = {"kind": kind, "leftLine": left, "rightLine": right, "leftText": left_text, "rightText": right_text,
           "hunk": hunk}
    if left_spans is not None:
        row["leftSpans"] = left_spans
    if right_spans is not None:
        row["rightSpans"] = right_spans
    row.update(extra)
    return row


class StructuralDiffOracleTests(unittest.TestCase):
    def check(self, rows, **diff):
        app = mock.Mock()

        def query(verb, args=None):
            self.assertEqual((verb, args), ("compare", None))
            return {"available": True, "rows": rows, **diff}
        app.query.side_effect = query
        oracle = monkey_oracles.StructuralDiffOracle(app)
        found = [message for kind, message in oracle.check()]
        self.assertTrue(all(kind == "diff" for kind, _ in oracle.check()))
        return oracle, found

    def test_no_diff_in_front_or_no_answer_is_nothing(self):
        app = mock.Mock()
        app.query.return_value = {"available": False, "rows": [_diff_row(kind="unchanged", left_spans=[[0, 1]])]}
        self.assertEqual(monkey_oracles.StructuralDiffOracle(app).check(), [])
        app.query.side_effect = RuntimeError("busy")
        self.assertEqual(monkey_oracles.StructuralDiffOracle(app).check(), [])

    def test_a_marked_real_change_is_fine(self):
        _, found = self.check([_diff_row(left_text="x = 1", right_text="x = 2",
                                         left_spans=[[4, 5]], right_spans=[[4, 5]])])
        self.assertEqual(found, [])

    def test_a_span_on_a_side_that_did_not_change_or_is_absent(self):
        _, found = self.check([_diff_row(kind="unchanged", left_text="a", right_text="a", left_spans=[[0, 1]],
                                         hunk=-1)])
        self.assertEqual(found, ["the left side of row 1 (unchanged, left line 1) marks novel tokens on a side "
                                 "that did not change"])
        _, found = self.check([_diff_row(kind="added", left=0, left_text="", right_text="new",
                                         left_spans=[[0, 1]], right_spans=[[0, 3]])])
        self.assertEqual(found, ["the left side of row 1 (added, left line 0) marks novel tokens on a side "
                                 "that is absent"])

    def test_spans_must_be_inside_the_line_and_in_order(self):
        for spans in ([[0, 9]], [[2, 1]], [[3, 4], [1, 2]], [[1, 3], [2, 4]], [[-1, 1]]):
            _, found = self.check([_diff_row(left_text="abcd", right_text="abce", left_spans=spans)])
            self.assertEqual(len(found), 1, (spans, found))
            self.assertIn("outside its 4-byte line or out of order", found[0])
        _, found = self.check([_diff_row(left_text="abcd", right_text="abce", left_spans=[[0, 1], [1, 4]])])
        self.assertEqual(found, [], "adjacent spans are in order")

    def test_a_span_that_splits_a_character_keeps_the_row(self):
        oracle, found = self.check([_diff_row(left_text="\u00e9t\u00e9", right_text="ete",
                                              left_spans=[[1, 3]])])
        self.assertEqual(found, ["the left side of row 1 (changed, left line 1) has a novel span [1, 3) "
                                 "that splits a character"])
        self.assertIn("split-span-row-1.json", oracle.evidence)
        _, found = self.check([_diff_row(left_text="\u00e9t\u00e9", right_text="ete", left_spans=[[0, 2]])])
        self.assertEqual(found, [])

    def test_a_line_that_is_not_utf8_is_read_from_its_bytes(self):
        _, found = self.check([_diff_row(left_text="\ufffdb", right_text="ab", leftHex="ff62",
                                         left_spans=[[0, 1]])])
        self.assertEqual(found, [], "a lone 0xff is one character of its own")
        _, found = self.check([_diff_row(left_text="\ufffdb", right_text="ab", leftHex="ff62",
                                         left_spans=[[0, 3]])])
        self.assertIn("outside its 2-byte line", found[0])

    def test_a_cut_row_is_skipped(self):
        _, found = self.check([_diff_row(kind="unchanged", left_spans=[[0, 1]], textTruncated=True, hunk=-1)])
        self.assertEqual(found, [])

    def test_a_layout_only_hunk_marks_nothing(self):
        rows = [_diff_row(left_text="f(a,  b)", right_text="f(a, b)", left_spans=[[4, 5]])]
        oracle, found = self.check(rows)
        self.assertEqual(found, ["hunk 1 changes only whitespace and line breaks, and marks tokens novel"])
        self.assertIn("structural-hunk-1.json", oracle.evidence)
        _, found = self.check([_diff_row(left_text="f(a,  b)", right_text="f(a, b)")])
        self.assertEqual(found, [], "unmarked, it is right")
        _, found = self.check([_diff_row(left_text='s("a  b")', right_text='s("a b")', left_spans=[[3, 7]])])
        self.assertEqual(found, [], "inside a string whitespace can matter")

    def test_a_reflowed_hunk_is_joined_across_its_rows(self):
        rows = [_diff_row(left_text="call(a,", right_text="call(a, b)", hunk=0),
                _diff_row(kind="deleted", right=0, left_text="     b)", right_text="", hunk=0)]
        _, found = self.check(rows)
        self.assertEqual(found, [])

    def test_a_real_change_must_show(self):
        _, found = self.check([_diff_row(left_text="x = 1", right_text="x = 2")])
        self.assertEqual(found, ["hunk 1 changes more than whitespace, and shows it as layout only "
                                 "(no novel token, no added or deleted line)"])
        _, found = self.check([_diff_row(kind="added", left=0, left_text="", right_text="y = 3")])
        self.assertEqual(found, [], "an added line with text shows itself")
        _, found = self.check([_diff_row(left_text="x = 1", right_text="x = 2", hunk=0),
                               _diff_row(kind="added", left=0, left_text="", right_text="   ", hunk=0)])
        self.assertEqual(len(found), 1, "an added blank line does not show a change")

    def test_a_cut_row_list_checks_complete_hunks_only(self):
        rows = [_diff_row(left_text="x = 1", right_text="x = 2", hunk=0),
                _diff_row(left_text="y = 1", right_text="y = 2", hunk=1)]
        _, found = self.check(rows, rowsTruncated=True)
        self.assertEqual(found, ["hunk 1 changes more than whitespace, and shows it as layout only "
                                 "(no novel token, no added or deleted line)"])
        _, found = self.check([_diff_row(left_text="x = 1", right_text="x = 2", textTruncated=True)])
        self.assertEqual(found, [], "a hunk with a cut row is not judged")

    def test_the_token_rule_is_the_diffs(self):
        tokens = monkey_oracles.StructuralDiffOracle._tokens
        self.assertEqual(tokens("a->b += c.-d"), ["a", "->", "b", "+=", "c", ".-", "d"])
        self.assertEqual(tokens("f(x, 'y');"), ["f", "(", "x", ",", "'", "y", "'", ")", ";"])
        self.assertNotEqual(tokens("-S . -B"), tokens("-S .-B"))


class RemoteConvergenceTests(unittest.TestCase):
    SETTLED = {"state": "ready", "filesPending": 0, "unsentPushes": 0}

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(self.dir, ignore_errors=True))
        self.oracle = None
        self.notifications_fail = False
        self.mirror = self.dir / "mirror"
        self.host = self.dir / "host"
        self.mirror.mkdir()
        self.host.mkdir()

    def check(self, statuses, trees=(({}, {}),), alive=True):
        """`statuses`: the status query's `remote` answers in turn (the last repeats);
        a dict with "elsewhere" stands for another project in front. `trees`: the
        (host, mirror) content sets in turn (the last repeats)."""
        app = mock.Mock()
        app.alive.return_value = alive
        app.log_since.return_value = ""
        app.work = None
        answers = list(statuses)

        def query(verb, args=None):
            if verb == "notifications":
                self.assertEqual(args, {"recent": True})
                if self.notifications_fail:
                    raise monkey_oracles.q.QaFailure("busy")
                return [{"message": "push refused"}]
            self.assertEqual((verb, args), ("status", None))
            remote = answers.pop(0) if len(answers) > 1 else answers[0]
            root = "/elsewhere" if remote.get("elsewhere") else str(self.mirror)
            return {"projectRoot": root, "remote": remote}
        app.query.side_effect = query
        sets = list(trees)
        fake_host = mock.Mock()

        def content_set(host, mirror):
            self.assertEqual((host, mirror), (self.host, self.mirror))
            return sets.pop(0) if len(sets) > 1 else sets[0]
        fake_host.content_set.side_effect = content_set
        oracle = self.oracle or monkey_oracles.RemoteConvergence(app, self.mirror, self.host, "remote-open host:/root")
        oracle.app = app
        self.slept = []
        with mock.patch.dict(sys.modules, {"remote_host": fake_host}), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.0001), \
                mock.patch.object(monkey_oracles.time, "sleep", self.slept.append):
            found = oracle.check()
        self.assertTrue(all(kind == "remote" for kind, _ in found), found)
        return oracle, app, [message for _, message in found]

    def test_a_gone_app_is_not_compared(self):
        oracle, _, found = self.check([self.SETTLED], alive=False)
        self.assertEqual(found, [])
        self.assertEqual(sum(oracle.outcomes.values()), 0)

    def test_settled_trees_that_agree_are_fine(self):
        oracle, app, found = self.check([self.SETTLED], trees=(({"a": "1", "b": "2"}, {"a": "1", "b": "2"}),))
        self.assertEqual(found, [])
        self.assertEqual(oracle.outcomes["agreed:2"], 1)
        app.send_line.assert_not_called()

    def test_another_project_in_front_is_reopened_first(self):
        oracle, app, found = self.check([{"elsewhere": True}, self.SETTLED])
        self.assertEqual(found, [])
        app.send_line.assert_called_once_with("remote-open host:/root", record=False)
        self.assertEqual(oracle.outcomes["reopened"], 1)

    def test_a_project_that_never_comes_back_in_front(self):
        oracle, _, found = self.check([{"elsewhere": True}])
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].startswith("the remote project is not in front "), found)
        self.assertTrue(found[0].endswith("after `remote-open` asked for it"), found)
        self.assertEqual(oracle.outcomes["elsewhere"], 1)

    def test_a_disconnected_project_is_reconnected(self):
        _, app, found = self.check([{"state": "disconnected"}, self.SETTLED])
        self.assertEqual(found, [])
        app.send_line.assert_called_once_with("remote-reconnect", record=False)

    def test_a_project_that_never_settles_says_where_it_stopped(self):
        oracle, _, found = self.check([{"state": "syncing", "filesPending": 3, "unsentPushes": 1}])
        self.assertEqual(len(found), 1)
        self.assertIn("did not settle within", found[0])
        self.assertTrue(found[0].endswith("(state syncing, 3 pending, 1 unsent)"), found)
        self.assertIn(b"push refused", oracle.evidence["remote-status.json"])
        self.assertEqual(oracle.outcomes["unsettled"], 1)

    def test_settled_needs_ready_and_nothing_owed(self):
        settled = monkey_oracles.RemoteConvergence.settled
        self.assertTrue(settled(self.SETTLED))
        self.assertFalse(settled({**self.SETTLED, "state": "offline"}))
        self.assertFalse(settled({**self.SETTLED, "filesPending": 1}))
        self.assertFalse(settled({**self.SETTLED, "unsentPushes": 2}))

    def test_a_conflict_a_held_delete_or_a_blocked_save_is_divergence_on_purpose(self):
        for key, outcome in (("conflicts", "conflicted"), ("heldDeletes", "conflicted"), ("blocked", "blocked")):
            oracle, _, found = self.check([{**self.SETTLED, key: 1}], trees=(({"a": "1"}, {}),))
            self.assertEqual(found, [], key)
            self.assertEqual(oracle.outcomes[outcome], 1, key)

    def test_trees_that_still_differ_once_settled_are_a_finding(self):
        (self.host / "a.txt").write_text("host\n")
        (self.mirror / "a.txt").write_text("mirror\n")
        oracle, _, found = self.check([self.SETTLED], trees=(({"a.txt": "h1", "b.txt": "2"}, {"a.txt": "m1"}),))
        self.assertEqual(found, ["the settled mirror and the host differ in 2 file(s); a.txt is with different "
                                 "content"])
        self.assertIn(b"a.txt: host h1 mirror m1", oracle.evidence["trees.txt"])
        self.assertIn(b"b.txt: host 2 mirror absent", oracle.evidence["trees.txt"])
        self.assertEqual(oracle.evidence["diff-a.txt.host"], b"host\n")
        self.assertEqual(oracle.evidence["diff-a.txt.mirror"], b"mirror\n")
        self.assertEqual(oracle.outcomes["differed"], 1)
        _, _, found = self.check([self.SETTLED], trees=(({"only.txt": "1"}, {}),))
        self.assertTrue(found[0].endswith("only.txt is only on the host"), found)
        _, _, found = self.check([self.SETTLED], trees=(({}, {"only.txt": "1"}),))
        self.assertTrue(found[0].endswith("only.txt is only in the mirror"), found)

    def test_trees_that_agree_within_the_window_are_fine(self):
        oracle, _, found = self.check([self.SETTLED], trees=(({"a": "1"}, {}), ({"a": "1"}, {"a": "1"})))
        self.assertEqual(found, [])

    def test_a_project_that_moved_while_it_compared_is_no_verdict(self):
        oracle, _, found = self.check([self.SETTLED, {**self.SETTLED, "filesPending": 2}],
                                      trees=(({"a": "1"}, {}),))
        self.assertEqual(found, [])
        self.assertEqual(oracle.outcomes["moved"], 1)

    def test_differing_lists_both_sides(self):
        self.assertEqual(monkey_oracles.RemoteConvergence.differing({"a": "1", "b": "2"}, {"a": "1", "c": "3"}),
                         ["b", "c"])


class Utf8BoundaryTests(unittest.TestCase):
    def test_each_well_formed_character_is_one_unit(self):
        boundaries = monkey_oracles._utf8_boundaries
        self.assertEqual(boundaries(b""), {0})
        self.assertEqual(boundaries(b"ab"), {0, 1, 2})
        self.assertEqual(boundaries("\u00e9a".encode()), {0, 2, 3})
        self.assertEqual(boundaries("\u4f60".encode()), {0, 3})
        self.assertEqual(boundaries("\U0001f600".encode()), {0, 4})
        self.assertEqual(boundaries("a\u00e9\u4f60\U0001f600".encode()), {0, 1, 3, 6, 10})

    def test_a_byte_outside_any_character_stands_alone(self):
        boundaries = monkey_oracles._utf8_boundaries
        self.assertEqual(boundaries(b"\x97a"), {0, 1, 2}, "a stray continuation byte")
        self.assertEqual(boundaries(b"\xc3a"), {0, 1, 2}, "a lead cut off by an ASCII byte")
        self.assertEqual(boundaries(b"\xe4\xbd"), {0, 1, 2}, "a three-byte lead cut off at the end")
        self.assertEqual(boundaries(b"\xc0\x80"), {0, 1, 2}, "an overlong lead is not a lead")
        self.assertEqual(boundaries(b"\xf5\x80\x80\x80"), {0, 1, 2, 3, 4}, "past U+10FFFF is not a lead")
        self.assertEqual(boundaries(b"\xed\xa0\x80"), {0, 1, 2, 3}, "a surrogate does not decode")
        self.assertEqual(boundaries(b"\xdf\xbf\xef\xbf\xbf\xf4\x8f\xbf\xbf"), {0, 2, 5, 9},
                         "the top of each length")


class LatencySubjectTests(unittest.TestCase):
    def test_a_query_by_its_verb_a_command_by_its_command(self):
        subject = monkey_oracles.latency_subject
        self.assertEqual(subject("query 'ui'"), "the ui query")
        self.assertEqual(subject("query 'crash-points'"), "the crash-points query")
        self.assertEqual(subject(repr("type a long line of text")), "the type command")
        self.assertEqual(subject('"format-document --all"'), "the format-document command")
        self.assertEqual(subject(repr("  key ctrl+s")), "the key command")
        self.assertEqual(subject("''"), "an empty command")
        self.assertEqual(subject(repr("   ")), "an empty command")
        self.assertEqual(subject(repr("it's")), "the it command")

    def test_anything_else_is_named_as_it_came(self):
        subject = monkey_oracles.latency_subject
        self.assertEqual(subject("hit-test"), "hit-test")
        self.assertEqual(subject("query 'a b'"), "query 'a b'")
        self.assertEqual(subject("'"), "'", "one quote is not a quoted line")
        self.assertEqual(subject("'unbalanced\""), "'unbalanced\"")
        self.assertEqual(subject("'bad \\x escape'"), "the bad command", "a line literal_eval refuses")


class ResourceOracleProcessTests(unittest.TestCase):
    """The leak oracle against a real process: a child that holds two files, a
    pipe and a sleeping child of its own."""

    def setUp(self):
        import subprocess
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(self.dir, ignore_errors=True))
        script = self.dir / "holder.py"
        script.write_text(
            "import os, subprocess, sys, time\n"
            f"a = open({str(self.dir / 'a.txt')!r}, 'w')\n"
            f"b = open({str(self.dir / 'b.txt')!r}, 'w')\n"
            "r, w = os.pipe()\n"
            "child = subprocess.Popen(['sleep', '30'])\n"
            "print('ready', flush=True)\n"
            "time.sleep(30)\n")
        self.proc = subprocess.Popen([sys.executable, str(script)], stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.stop)
        self.assertEqual(self.proc.stdout.readline().strip(), "ready")

    def stop(self):
        import signal
        for child in Path(f"/proc/{self.proc.pid}/task/{self.proc.pid}/children").read_text().split() \
                if self.proc.poll() is None else []:
            with contextlib.suppress(OSError):
                os.kill(int(child), signal.SIGKILL)
        self.proc.kill()
        self.proc.wait()

    def app(self, terminals=None):
        app = mock.Mock()
        app.proc = self.proc
        app.pid = self.proc.pid
        app.work = None
        app.binary = None
        app.actions = []
        app.query.return_value = terminals if terminals is not None else []
        return app

    def test_a_sample_counts_what_the_process_holds(self):
        oracle = monkey_oracles.ResourceOracle(self.app())
        sample = oracle.sample()
        self.assertEqual(sample["children"], 1)
        self.assertGreaterEqual(sample["fds"], 5, "stdin/out/err, two files and a pipe's two ends")
        self.assertGreaterEqual(sample["threads"], 1)
        self.assertGreater(sample["rss_kb"], 0)
        self.assertEqual(sample["panes"], 0)
        self.assertIs(oracle.baseline, sample, "the first sample is the baseline")
        self.assertEqual(oracle.pid, self.proc.pid)
        self.assertGreater(sum(oracle.baseline_threads.values()), 0)
        second = oracle.sample()
        self.assertIs(oracle.baseline, sample, "a later sample keeps it")
        self.assertEqual(oracle.samples, [sample, second])

    def test_a_relaunch_starts_a_new_baseline_and_a_gone_app_samples_nothing(self):
        app = self.app()
        oracle = monkey_oracles.ResourceOracle(app)
        oracle.sample()
        oracle.pid = 1
        fresh = oracle.sample()
        self.assertIs(oracle.baseline, fresh)
        app.pid = None
        self.assertIsNone(oracle.sample())
        app.proc = None
        self.assertIsNone(oracle.sample())

    def test_open_terminal_panes_are_counted_in_either_answer_shape(self):
        oracle = monkey_oracles.ResourceOracle(self.app([{"panes": [1, 2]}, {"panes": []}]))
        self.assertEqual(oracle.sample()["panes"], 3, "a tab with no pane list is one pane")
        oracle = monkey_oracles.ResourceOracle(self.app({"tabs": [{"panes": [1, 2, 3]}]}))
        self.assertEqual(oracle.sample()["panes"], 3)
        app = self.app()
        app.query.side_effect = RuntimeError("busy")
        self.assertEqual(monkey_oracles.ResourceOracle(app).sample()["panes"], 0)

    def test_the_evidence_names_children_descriptors_threads_and_memory(self):
        oracle = monkey_oracles.ResourceOracle(self.app())
        oracle.sample()
        held = oracle._what_it_holds(self.proc.pid)
        children = held["children.txt"].decode()
        self.assertTrue(children.startswith("1 direct children by program: {'sleep': 1}"), children)
        self.assertIn("sleep 30", children)
        fds = held["fds.txt"].decode()
        self.assertRegex(fds, r"\s2  file:\.txt")
        pipes = re.search(r"(\d+)  pipe", fds)
        self.assertTrue(pipes and int(pipes.group(1)) >= 2, fds)
        threads = held["threads.txt"].decode()
        self.assertTrue(threads.startswith("threads by name: now (at the baseline, change)"))
        self.assertIn("+0)", threads)
        memory = held["memory.txt"].decode()
        self.assertIn("Rss:", memory)
        self.assertIn("largest resident mappings (kB):", memory)
        self.assertIn("python", memory.lower())

    def test_a_thread_gone_since_the_baseline_is_listed_at_zero(self):
        oracle = monkey_oracles.ResourceOracle(self.app())
        oracle.sample()
        oracle.baseline_threads["worker-gone"] = 2
        threads = oracle._what_it_holds(self.proc.pid)["threads.txt"].decode()
        self.assertIn("     0  (   2, -2)  worker-gone", threads)


class ResourceBudgetTests(unittest.TestCase):
    BASE = {"fds": 50, "threads": 20, "children": 2, "rss_kb": 100_000, "panes": 0}

    def check(self, now, reported=()):
        oracle = monkey_oracles.ResourceOracle(mock.Mock())
        oracle.baseline = dict(self.BASE)
        oracle.pid = None
        oracle.reported = set(reported)
        with mock.patch.object(oracle, "sample", return_value={**self.BASE, **now}), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 1.0):
            return [message for _, message in oracle.check()]

    def test_growth_within_every_budget_is_fine(self):
        self.assertEqual(self.check({"fds": 50 + 256, "threads": 20 + 64, "children": 2 + 48,
                                     "rss_kb": 100_000 + 1_500_000}), [])

    def test_growth_past_a_budget_is_one_leak_per_resource(self):
        self.assertEqual(self.check({"fds": 50 + 257}),
                         ["the app's fds grew by 257 over the run (50 -> 307; budget 256)"])
        self.assertEqual(self.check({"threads": 85, "children": 51}),
                         ["the app's threads grew by 65 over the run (20 -> 85; budget 64)",
                          "the app's children grew by 49 over the run (2 -> 51; budget 48)"])
        self.assertEqual(self.check({"fds": 1000}, reported={"fds"}), [], "once per run")

    def test_open_terminal_panes_are_allowed_what_they_hold(self):
        self.assertEqual(self.check({"fds": 50 + 256 + 40, "panes": 10}), [])
        self.assertEqual(self.check({"fds": 50 + 256 + 41, "panes": 10}),
                         ["the app's fds grew by 297 over the run (50 -> 347; budget 256, "
                          "plus 40 for 10 more open terminal pane(s))"])

    def test_a_slow_build_gets_more_memory(self):
        oracle = monkey_oracles.ResourceOracle(mock.Mock())
        oracle.baseline, oracle.pid = dict(self.BASE), None
        with mock.patch.object(oracle, "sample", return_value={**self.BASE, "rss_kb": 100_000 + 2_000_000}), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 3.0):
            self.assertEqual(oracle.check(), [], "the budget is 1.5 GB x slow factor / 2")

    def test_no_sample_or_no_baseline_is_nothing(self):
        oracle = monkey_oracles.ResourceOracle(mock.Mock())
        with mock.patch.object(oracle, "sample", return_value=None):
            self.assertEqual(oracle.check(), [])


class CrashPointTests(unittest.TestCase):
    """DataOracle's kills at a named durable-write boundary: which boundaries it
    knows, which it picks, what makes the app pass one, and the kill itself."""

    def oracle(self, answers):
        app = mock.Mock()
        app.project = None

        def query(verb, args=None):
            answer = answers.get(verb, {})
            if isinstance(answer, Exception):
                raise answer
            return answer(args) if callable(answer) else answer
        app.query.side_effect = query
        return monkey_oracles.DataOracle(app), app

    def test_the_boundaries_passed_are_kept_across_launches(self):
        oracle, app = self.oracle({"crash-points": {"installed": True, "sites": [
            {"site": "durable.renaming", "files": {"session": 3, "recents": 1}},
            {"site": "", "files": {"x": 1}}, {"site": "durable.writing", "files": {"": 1}}]}})
        self.assertEqual(oracle.crash_points(), [("durable.renaming", "recents"), ("durable.renaming", "session")])
        app.query.side_effect = lambda verb, args=None: {"installed": True, "sites": [
            {"site": "durable.writing", "files": {"config (staging)": 1}}]}
        self.assertEqual(oracle.crash_points(), [("durable.renaming", "recents"), ("durable.renaming", "session"),
                                                 ("durable.writing", "config (staging)")])

    def test_a_binary_without_crash_points_knows_none(self):
        oracle, _ = self.oracle({"crash-points": {"installed": False, "sites": [
            {"site": "durable.writing", "files": {"session": 1}}]}})
        self.assertEqual(oracle.crash_points(), [])
        oracle, _ = self.oracle({"crash-points": RuntimeError("unknown query")})
        self.assertEqual(oracle.crash_points(), [])
        self.assertIsNone(oracle.choose_point(random.Random(1)))

    def test_a_point_is_a_rule_and_its_trigger_least_tried_first(self):
        oracle, _ = self.oracle({"crash-points": {"installed": True, "sites": [
            {"site": "durable.writing", "files": {"session (staging)": 1, "recents (staging)": 1}}]},
            "buffer": {"kind": "editor", "path": "/p/main.cpp"}})
        picks = collections.Counter(oracle.choose_point(random.Random(seed),
                                                        tried=lambda key: 1000 if "session" in key else 0)
                                    for seed in range(50))
        self.assertGreater(picks["kill:durable.writing@recents (staging)#1|open"], 45, picks)
        untried = {oracle.choose_point(random.Random(seed)) for seed in range(50)}
        self.assertEqual(untried, {"kill:durable.writing@recents (staging)#1|open",
                                   "kill:durable.writing@session (staging)#1|edit"})

    def test_the_trigger_is_what_passes_the_files_boundary(self):
        oracle, app = self.oracle({"buffer": {"kind": "editor", "path": "/p/main.cpp"}})
        self.assertEqual(oracle._trigger_for("recents"), "open")
        self.assertEqual(oracle._trigger_for("recents.bak"), "open")
        self.assertEqual(oracle._trigger_for("workspace-session (staging)"), "project")
        self.assertEqual(oracle._trigger_for("main.cpp (staging)"), "save")
        self.assertEqual(oracle._trigger_for("session"), "edit")
        app.query.side_effect = lambda verb, args=None: {"kind": "editor", "path": "/p/main.cpp", "readOnly": True}
        self.assertEqual(oracle._trigger_for("main.cpp"), "edit", "a read-only buffer is not saved")
        app.query.side_effect = lambda verb, args=None: {"kind": "compare", "path": "/p/main.cpp"}
        self.assertEqual(oracle._trigger_for("main.cpp"), "edit")
        app.query.side_effect = RuntimeError("busy")
        self.assertEqual(oracle._trigger_for("main.cpp"), "edit")

    def dying_app(self, trigger_kills=True, buffer=None, staged_after=None):
        oracle, app = self.oracle({})
        state = {"alive": True, "polls": 0}
        sent = []
        armed = []
        self.polls = state

        def query(verb, args=None):
            if verb == "crash-points":
                armed.append(args)
                return {}
            if verb == "documents":
                self.assertEqual(args, {"all": True})
                return [{"path": "/p/a.txt", "dirty": True, "sha256": "edited", "project": "/p"}]
            if verb == "buffer":
                return buffer or {}
            if verb == "persistence":
                state["polls"] += 1
                return {"staged": staged_after is not None and state["polls"] >= staged_after}
            return {}
        app.query.side_effect = query

        def send_line(line, record=True):
            self.assertIs(record, False, line)
            sent.append(line)
            if trigger_kills and line != "insert-line-below":
                state["alive"] = False
        app.send_line.side_effect = send_line
        app.alive.side_effect = lambda: state["alive"]
        app.dying.return_value = False
        return oracle, app, sent, armed

    def test_a_save_trigger_saves_the_dirty_buffer_and_the_app_dies(self):
        oracle, app, sent, armed = self.dying_app(buffer={"kind": "editor", "path": "/nowhere/a.txt", "dirty": True})
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.001):
            died, after, saved = oracle._kill_at("kill:durable.renaming@a.txt#1|save")
        self.assertTrue(died)
        self.assertEqual(saved, "/nowhere/a.txt")
        self.assertEqual(sent, ["insert-line-below", "save"])
        self.assertEqual(armed[0], {"arm": "kill:durable.renaming@a.txt#1"})
        self.assertEqual(after[0]["sha256"], "edited")
        self.assertEqual(oracle.point_outcomes["hit:durable.renaming"], 1)

    def test_a_trigger_that_never_reaches_the_boundary_disarms_it(self):
        oracle, app, sent, armed = self.dying_app(trigger_kills=False)
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.001):
            died, _, saved = oracle._kill_at("kill:durable.writing@workspace-session#1|project")
        self.assertFalse(died)
        self.assertIsNone(saved)
        self.assertEqual(sent, ["insert-line-below", "project-next"])
        self.assertEqual(armed[-1], {"disarm": True})
        self.assertEqual(oracle.point_outcomes["missed"], 1)

    def test_an_open_trigger_opens_a_file_recents_has_not_held(self):
        oracle, app, sent, _ = self.dying_app()
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        app.project = directory
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.001):
            oracle._kill_at("kill:durable.renaming@recents#1|open")
        self.assertEqual((directory / "qa-point-1.txt").read_text(), "1\n")
        self.assertTrue(sent[1].startswith("open ") and "qa-point-1.txt" in sent[1], sent)

    def test_a_binary_without_the_query_is_not_killed(self):
        oracle, app = self.oracle({"crash-points": monkey_oracles.q.QaFailure("unknown query")})
        self.assertEqual(oracle._kill_at("kill:durable.writing@session#1|edit"), (False, [], None))
        app.send_line.assert_not_called()


class CleanBufferTests(unittest.TestCase):
    """DataOracle.check_clean: a clean buffer must hold its file's bytes once the
    file stops changing."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(self.dir, ignore_errors=True))

    def oracle(self, docs, content=None):
        app = mock.Mock()
        app.work = self.dir
        app.project = self.dir

        def query(verb, args=None):
            if verb == "documents" and args and "content" in args:
                return content or []
            return [dict(doc) for doc in docs]
        app.query.side_effect = query
        return monkey_oracles.DataOracle(app), app

    def file(self, name, data):
        path = self.dir / name
        path.write_bytes(data)
        return str(path), hashlib.sha256(data).hexdigest()

    def check(self, oracle):
        with mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            return [message for _, message in oracle.check_clean()]

    def test_a_clean_buffer_that_holds_its_file_is_fine(self):
        path, sha = self.file("a.txt", b"same\n")
        oracle, _ = self.oracle([{"path": path, "sha256": sha, "dirty": False}])
        self.assertEqual(self.check(oracle), [])

    def test_a_stable_disagreement_is_a_finding_with_both_sides(self):
        path, disk_sha = self.file("a.txt", b"disk text\n")
        (self.dir / "microide-1.log").write_text(
            f"microide buffer-io: read path={path} bytes=10\nmicroide other: path={path} \n"
            f"microide buffer-io: write path={self.dir / 'b.txt'} bytes=3\n")
        doc = {"path": path, "sha256": "f" * 64, "dirty": False}
        oracle, _ = self.oracle([doc], content=[{"path": path, "contentHex": b"buffer text\n".hex()}])
        self.assertEqual(self.check(oracle), [f"the clean buffer of a.txt differs from its file on disk "
                                              f"(buffer {'f' * 12}, disk {disk_sha[:12]}) and the file is not "
                                              f"changing"])
        self.assertEqual(oracle.evidence["mismatch-a.txt.disk"], b"disk text\n")
        self.assertEqual(oracle.evidence["mismatch-a.txt.buffer"], b"buffer text\n")
        summary = oracle.evidence["mismatch-a.txt.txt"].decode()
        self.assertIn("buffer 12 bytes, disk 10 bytes; first difference at byte 0", summary)
        self.assertIn(f"disk sha256:   {disk_sha}", summary)
        history = oracle.evidence["mismatch-a.txt.io.txt"].decode()
        self.assertIn("microide-1.log: microide buffer-io: read", history)
        self.assertNotIn("other", history)
        self.assertNotIn("b.txt", history)

    def test_the_first_difference_is_where_they_part(self):
        path, _ = self.file("a.txt", b"abcdXf")
        oracle, _ = self.oracle([], content=[{"path": path, "contentHex": b"abcdef".hex()}])
        summary = oracle._mismatch_evidence(path, ("x", "y"))[f"mismatch-a.txt.txt"].decode()
        self.assertIn("first difference at byte 4", summary)
        self.assertIn("buffer[4:+48]: b'ef'", summary)
        path, _ = self.file("b.txt", b"abc")
        oracle, _ = self.oracle([], content=[{"path": path, "contentHex": b"abcdef".hex()}])
        self.assertIn("first difference at byte 3", oracle._mismatch_evidence(path, ("x", "y"))[
            "mismatch-b.txt.txt"].decode(), "a prefix parts where the shorter ends")

    def test_a_file_still_being_rewritten_is_not_a_finding(self):
        path, _ = self.file("a.txt", b"one\n")
        oracle, _ = self.oracle([{"path": path, "sha256": "f" * 64, "dirty": False}])
        writes = iter([b"two\n", b"three\n", b"four\n"])
        real = oracle.clean_mismatches

        def changing(docs):
            found = real(docs)
            with contextlib.suppress(StopIteration):
                Path(path).write_bytes(next(writes))
            return found
        with mock.patch.object(oracle, "clean_mismatches", side_effect=changing):
            self.assertEqual(self.check(oracle), [])

    def test_dirty_read_only_mixed_and_pathless_buffers_are_not_compared(self):
        path, _ = self.file("a.txt", b"disk\n")
        for extra in ({"dirty": True}, {"readOnly": True}, {"mixedLineEndings": True}):
            oracle, _ = self.oracle([{"path": path, "sha256": "f" * 64, "dirty": False, **extra}])
            self.assertEqual(self.check(oracle), [], extra)
        oracle, _ = self.oracle([{"path": "", "sha256": "f" * 64, "dirty": False},
                                 {"path": str(self.dir / "gone.txt"), "sha256": "f" * 64, "dirty": False}])
        self.assertEqual(self.check(oracle), [], "no path, or no file to compare with")

    def test_a_save_temporary_of_this_process_is_a_leftover(self):
        (self.dir / ".git").mkdir()
        (self.dir / ".git" / ".x.microide-staging.42.0").write_text("")
        (self.dir / "sub").mkdir()
        (self.dir / "sub" / ".a.txt.microide-staging.42.3").write_text("")
        (self.dir / ".b.txt.microide-staging.7.0").write_text("")
        oracle, app = self.oracle([])
        self.assertEqual(oracle.staging_leftovers(42), [self.dir / "sub" / ".a.txt.microide-staging.42.3"])
        self.assertEqual(oracle.staging_leftovers(None), [])
        app.project = None
        self.assertEqual(oracle.staging_leftovers(42), [])


class UndoAlgebraGuardTests(unittest.TestCase):
    """When UndoAlgebra.check stands aside, and what it counts: the conditions
    mutation-test.sh found no test held (65 mutants of check survived)."""

    def check(self, app, asked=2):
        oracle = monkey_oracles.UndoAlgebra(app, random.Random(0))
        with mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.001):
            return oracle, oracle.check(asked)

    def test_a_redo_that_loses_text_is_a_finding_with_both_texts(self):
        app = _UndoEditor(["a", "ab", "abc"], at=2, redo_drops=True)
        oracle, found = self.check(app)
        self.assertEqual(found, [("undo", "2 undo(s) then 2 redo(s) changed the text of CHANGELOG.md (3 -> 2 chars)")])
        self.assertEqual(oracle.evidence, {"undo-before.txt": b"abc", "undo-after.txt": b"ab"})

    def test_only_the_undos_that_applied_are_redone(self):
        app = _UndoEditor(["a", "ab"], at=1)
        lines = []
        app.on_line = lines.append
        _, found = self.check(app, asked=4)
        self.assertEqual(found, [])
        self.assertEqual(lines, ["undo"] * 4 + ["redo"], "one undo applied, so one redo")

    def test_an_editor_not_in_front_is_not_checked(self):
        for ui in ({"focus": "sidebar"}, {"focus": "editor", "overlay": {"visible": True}},
                   {"focus": "editor", "modal": "dirty-prompt"}, {"focus": "editor", "menu": {"open": True}}):
            app = _UndoEditor(["a", "ab", "abc"], at=2, redo_drops=True)
            app.last_ui = ui
            lines = []
            app.on_line = lines.append
            oracle, found = self.check(app)
            self.assertEqual(found, [], ui)
            self.assertEqual(lines, [], ui)
            self.assertEqual(oracle.evidence, {}, ui)
        app = _UndoEditor(["a", "ab", "abc"], at=2, redo_drops=True)
        app.last_ui = {"focus": "editor", "overlay": {"visible": False}, "menu": {"open": False}}
        self.assertEqual(len(self.check(app)[1]), 1, "a hidden overlay and a closed menu are not in the way")

    def test_a_read_only_or_textless_buffer_is_not_checked(self):
        for extra in ({"readOnly": True}, {"text": None}):
            app = _UndoEditor(["a", "ab", "abc"], at=2, redo_drops=True)
            answer = app.query

            def query(verb, args=None, extra=extra, answer=answer):
                out = answer(verb, args)
                if verb == "buffer":
                    out.update(extra)
                    if out.get("text") is None:
                        del out["text"]
                return out
            app.query = query
            lines = []
            app.on_line = lines.append
            self.assertEqual(self.check(app)[1], [], extra)
            self.assertEqual(lines, [], extra)

    def test_a_buffer_without_revisions_counts_every_undo(self):
        app = _UndoEditor(["a", "ab"], at=1)
        answer = app.query
        app.query = lambda verb, args=None: {k: v for k, v in answer(verb, args).items() if k != "revision"}
        lines = []
        app.on_line = lines.append
        self.check(app, asked=3)
        self.assertEqual(lines.count("redo"), 3)

    def test_a_tab_that_changes_under_the_commands_ends_the_check(self):
        for field in ("path", "kind"):
            app = _UndoEditor(["a", "ab", "abc"], at=2, redo_drops=True)
            answer = app.query
            calls = {"n": 0}

            def query(verb, args=None, field=field):
                out = answer(verb, args)
                if verb == "buffer":
                    calls["n"] += 1
                    if calls["n"] > 1:
                        out[field] = "elsewhere"
                return out
            app.query = query
            lines = []
            app.on_line = lines.append
            self.assertEqual(self.check(app)[1], [], field)
            self.assertNotIn("redo", lines, field)


class UntitledNameTests(unittest.TestCase):
    def oracle(self, docs, tabs=None):
        app = mock.Mock()
        app.query.side_effect = lambda verb, args=None: (
            [dict(d) for d in docs] if verb == "documents" else
            (tabs if not isinstance(tabs, Exception) else (_ for _ in ()).throw(tabs)))
        return monkey_oracles.DataOracle(app)

    def test_two_texts_under_one_untitled_name_are_a_data_finding_with_the_tabs(self):
        docs = [{"path": "", "title": "Untitled-1", "sha256": "a", "project": "/p"},
                {"path": "", "title": "Untitled-1", "sha256": "b", "project": "/p"},
                {"path": "/p/x.txt", "title": "x.txt", "sha256": "c", "project": "/p"}]
        oracle = self.oracle(docs, tabs=[{"title": "Untitled-1", "group": 0}])
        self.assertEqual(oracle.check_untitled_names(),
                         [("data", "2 different unsaved buffers are all titled Untitled-1")])
        untitled = json.loads(oracle.untitled_evidence["untitled-documents.json"])
        self.assertEqual([d["sha256"] for d in untitled], ["a", "b"], "only the pathless buffers")
        self.assertEqual(json.loads(oracle.untitled_evidence["tabs.json"]), [{"title": "Untitled-1", "group": 0}])
        self.assertIn(b"\n ", oracle.untitled_evidence["tabs.json"], "indented for reading")

    def test_one_buffer_in_two_panes_or_a_titled_file_is_fine(self):
        same = [{"path": "", "title": "Untitled-1", "sha256": "a", "project": "/p"}] * 2
        self.assertEqual(self.oracle(same).check_untitled_names(), [])
        files = [{"path": "/p/a/x.txt", "title": "x.txt", "sha256": "a", "project": "/p"},
                 {"path": "/p/b/x.txt", "title": "x.txt", "sha256": "b", "project": "/p"}]
        self.assertEqual(self.oracle(files).check_untitled_names(), [], "files with one name are not untitled")
        other_projects = [{"path": "", "title": "Untitled-1", "sha256": "a", "project": "/p"},
                          {"path": "", "title": "Untitled-1", "sha256": "b", "project": "/q"}]
        self.assertEqual(self.oracle(other_projects).check_untitled_names(), [], "each project numbers its own")
        nameless = [{"path": "", "title": "", "sha256": "a"}, {"path": "", "title": "", "sha256": "b"}]
        self.assertEqual(self.oracle(nameless).check_untitled_names(), [])

    def test_the_documents_given_are_the_ones_judged(self):
        oracle = self.oracle([{"path": "", "title": "Untitled-1", "sha256": "a", "project": "/p"},
                              {"path": "", "title": "Untitled-1", "sha256": "b", "project": "/p"}])
        self.assertEqual(oracle.check_untitled_names([]), [], "an explicit list is not replaced by a query")

    def test_tabs_that_cannot_be_asked_leave_the_rest_of_the_evidence(self):
        docs = [{"path": "", "title": "Untitled-2", "sha256": "a", "project": "/p"},
                {"path": "", "title": "Untitled-2", "sha256": "b", "project": "/p"}]
        oracle = self.oracle(docs, tabs=RuntimeError("busy"))
        self.assertEqual(len(oracle.check_untitled_names()), 1)
        self.assertEqual(oracle.untitled_evidence["tabs.json"], b"null")


class TerminalOriginsEdgeTests(unittest.TestCase):
    def log(self, pids, stack=("  #0 0x1 in StartPty", "  #1 0x2 in OpenTerminal")):
        return "".join(f"microide terminal-life: start pid={pid}\n" + "".join(line + "\n" for line in stack)
                       for pid in pids)

    def test_more_than_twenty_shells_are_cut_with_an_ellipsis(self):
        pids = set(range(100, 121))
        text = monkey_oracles.terminal_origins(self.log(sorted(pids)), pids)
        first = text.splitlines()[0]
        self.assertTrue(first.startswith("21 alive shell(s) started here: 100 (never stopped)"), first)
        self.assertTrue(first.endswith(" ..."), first)
        self.assertNotIn("120 (", first)
        twenty = set(range(100, 120))
        self.assertFalse(monkey_oracles.terminal_origins(self.log(sorted(twenty)), twenty)
                         .splitlines()[0].endswith(" ..."), "exactly twenty fit")

    def test_with_a_binary_the_stack_is_symbolized_without_its_frame_numbers(self):
        seen = {}

        def symbolize(block, binary):
            seen["block"], seen["binary"] = block, binary
            return "StartPty at TerminalPty.cpp:12\n"
        with mock.patch.object(monkey_oracles.q, "symbolize_backtrace", side_effect=symbolize):
            text = monkey_oracles.terminal_origins(self.log([7]), {7}, Path("/bin/microide"))
        self.assertEqual(seen["block"], "microide terminal-life: start\n0x1 in StartPty\n0x2 in OpenTerminal")
        self.assertEqual(seen["binary"], Path("/bin/microide"))
        self.assertIn("StartPty at TerminalPty.cpp:12", text)
        self.assertNotIn("#0", text)

    def test_lines_between_starts_end_a_stack(self):
        text = "microide terminal-life: start pid=1\n  #0 a\nother line\n  #1 not-part\n"
        out = monkey_oracles.terminal_origins(text, {1})
        self.assertIn("  #0 a", out)
        self.assertNotIn("not-part", out)
        self.assertEqual(monkey_oracles.terminal_origins("", set()), "")


class _LoggingRelaunchingApp(_RelaunchingApp):
    """The relaunching app with a work tree (its state records) and a log the
    killed process wrote, as the driver's App has."""

    def __init__(self, work: Path) -> None:
        super().__init__()
        self.work = work
        self.log = ""
        self.persistence = {"staged": True}
        self.read_limits: list[int] = []

    def _log_size(self) -> int:
        return len(self.log)

    def log_since(self, mark: int, limit: int) -> str:
        self.read_limits.append(limit)
        return self.log[mark:mark + limit]

    def query(self, verb, args=None):
        if verb == "persistence":
            if isinstance(self.persistence, Exception):
                raise self.persistence
            return self.persistence
        return super().query(verb, args)


def _io_line(kind: str, path: str, sha: str) -> str:
    return f"microide buffer-io: {kind} path={path} doc=0x1 rev=1 dirty=1 bytes=3 sha={sha[:12]}\n"


class Kill9ProcedureTests(unittest.TestCase):
    """What kill9_and_check does around the verdicts: the row it records, the wait,
    the evidence it keeps, and the save a kill cut part-way."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(self.dir, ignore_errors=True))

    def app(self):
        return _LoggingRelaunchingApp(self.dir / "work")

    def run_kill(self, app, *, rng=None, waited=True, point=None, choose=None, kill_at=None, after=None,
                 between=None):
        oracle = monkey_oracles.DataOracle(app)
        relaunch = app.crash_and_relaunch

        def restore(*args, **kwargs):
            relaunch(*args, **kwargs)
            if between is not None:
                between()
            if after is not None:
                app.docs[1] = [dict(d) for d in after]
        app.crash_and_relaunch = restore
        slept = []
        with mock.patch.object(monkey_oracles.time, "sleep", slept.append), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.001), \
                contextlib.ExitStack() as stack:
            if kill_at is not None:
                stack.enter_context(mock.patch.object(oracle, "_kill_at", side_effect=lambda p: kill_at(oracle, p)))
            found = oracle.kill9_and_check(rng or random.Random(1), waited=waited, point=point, choose=choose)
        self.assertTrue(all(kind == "data" for kind, _ in found), found)
        return oracle, [message for _, message in found], slept

    def test_the_kill_is_recorded_for_a_replay(self):
        app = self.app()
        self.run_kill(app)
        self.assertEqual(app.actions[-1], {"kill9": True, "waited": True})
        app = self.app()
        self.run_kill(app, point="kill:durable.writing@session#1|edit",
                      kill_at=lambda oracle, p: (False, [], None))
        self.assertEqual(app.actions[-1], {"kill9": True, "waited": True,
                                           "point": "kill:durable.writing@session#1|edit"})

    def test_whether_it_waits_is_drawn_seven_in_ten(self):
        for draw, waited in ((0.69, True), (0.71, False)):
            app = self.app()
            rng = mock.Mock()
            rng.random.return_value = draw
            _, _, slept = self.run_kill(app, rng=rng, waited=None)
            self.assertEqual(app.actions[-1]["waited"], waited, draw)
            self.assertEqual(bool(slept) and slept[0] == monkey_oracles.CRASH_SAFETY_QUIET_S * 0.001, waited)
        app = self.app()
        self.run_kill(app, waited=False, point="kill:x@y#1|edit", kill_at=lambda oracle, p: (False, [], None))
        self.assertTrue(app.actions[-1]["waited"], "a kill at a point always waits for the flush")

    def test_a_point_chosen_once_the_flush_has_run(self):
        app = self.app()
        chosen = []

        def choose():
            chosen.append(app.query("persistence"))
            return "kill:durable.renaming@session#1|edit"
        self.run_kill(app, waited=None, choose=choose, kill_at=lambda oracle, p: (False, [], None))
        self.assertEqual(len(chosen), 1)
        self.assertEqual(app.actions[-1]["point"], "kill:durable.renaming@session#1|edit")
        self.assertTrue(app.actions[-1]["waited"])

    def test_a_flush_that_never_stages_is_a_finding_and_an_old_binary_is_not(self):
        app = self.app()
        app.persistence = {"staged": False}
        _, found, _ = self.run_kill(app)
        self.assertIn("the crash-safety flush did not stage the unsaved buffers within 20s of quiet "
                      "(the `persistence` query never answered staged)", found)
        app = self.app()
        app.persistence = monkey_oracles.q.QaFailure("unknown query \"persistence\"")
        _, found, _ = self.run_kill(app)
        self.assertEqual(found, [])

    def test_the_killed_process_state_is_kept_beside_the_run(self):
        app = self.app()
        (app.work / "state" / "projects").mkdir(parents=True)
        (app.work / "state" / "projects" / "session").write_text("record")
        (app.work / "kill-state").mkdir()
        (app.work / "kill-state" / "stale").write_text("from an earlier kill")
        oracle, _, _ = self.run_kill(app)
        self.assertEqual(oracle.last_kill_state, app.work / "kill-state")
        self.assertEqual((app.work / "kill-state" / "projects" / "session").read_text(), "record")
        self.assertFalse((app.work / "kill-state" / "stale").exists(), "the previous kill's copy is replaced")
        self.assertEqual(set(oracle.last_kill), {"before", "after"})
        self.assertIn("/p/b/y.txt", [doc["path"] for doc in oracle.last_kill["before"]])
        app = _RelaunchingApp()
        oracle, _, _ = self.run_kill(app)
        self.assertIsNone(oracle.last_kill_state, "no work tree, nothing kept")

    def test_a_save_cut_part_way_leaves_the_old_or_the_written_bytes(self):
        path = self.dir / "a.txt"
        old, written, other = b"old\n", b"new\n", b"ne\n"

        def scenario(now: bytes):
            path.write_bytes(old)
            app = self.app()
            app.docs[1] = [{"path": str(path), "dirty": True, "sha256": "edited", "bytes": 4, "project": "/p/b"}]

            def kill_at(oracle, point):
                oracle.saved_file_before = hashlib.sha256(old).hexdigest()
                app.log += _io_line("writing", str(path), hashlib.sha256(written).hexdigest())
                path.write_bytes(now)
                return True, list(app.docs[1]), str(path)
            _, found, _ = self.run_kill(app, point="kill:durable.renaming@a.txt#1|save", kill_at=kill_at,
                                        after=app.docs[1])
            return found
        self.assertEqual(scenario(old), [])
        self.assertEqual(scenario(written), [])
        self.assertIn("a save of a.txt killed part-way (killed at durable.renaming@a.txt#1) left the file neither "
                      "as it was nor as the save was writing it", scenario(other))

    def test_a_buffer_saved_before_the_kill_is_owed_nothing(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"saved\n")
        sha = hashlib.sha256(b"saved\n").hexdigest()
        app = self.app()
        app.docs[1] = [{"path": str(path), "dirty": True, "sha256": "unsaved", "bytes": 4, "project": "/p/b"}]
        relaunch = app.crash_and_relaunch

        def crash(*args, **kwargs):
            if kwargs.get("between"):
                app.log += _io_line("write", str(path), sha)
            relaunch(*args, **kwargs)
        app.crash_and_relaunch = crash
        _, found, _ = self.run_kill(app, after=[])
        self.assertEqual(found, [], "the log says it was saved and the file holds what it wrote")

    def test_a_buffer_the_trigger_changed_back_clean_must_be_in_its_file(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"trigger saved\n")
        held = hashlib.sha256(b"trigger saved\n").hexdigest()
        before = {"path": str(path), "dirty": True, "sha256": "staged", "bytes": 6, "project": "/p/b"}

        def scenario(back_sha):
            app = self.app()
            app.docs[1] = [dict(before)]
            edited = {**before, "sha256": "edited"}
            _, found, _ = self.run_kill(app, point="kill:durable.writing@session#1|edit",
                                        kill_at=lambda oracle, p: (True, [edited], None),
                                        after=[{**before, "sha256": back_sha, "dirty": False}])
            return found
        self.assertEqual(scenario(held), [])
        self.assertEqual(scenario("other"), ["the unsaved buffer a.txt came back after a kill -9 marked saved; its "
                                             "file does not hold it (killed at durable.writing@session#1)"])

    def test_a_clean_file_the_trigger_touched_may_change_on_disk(self):
        path = self.dir / "c.txt"
        path.write_bytes(b"clean\n")
        sha = hashlib.sha256(b"clean\n").hexdigest()
        clean = {"path": str(path), "dirty": False, "sha256": sha, "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [clean]

        def kill_at(oracle, point):
            path.write_bytes(b"saved by the trigger\n")
            return True, [{**clean, "sha256": "changed"}], None
        _, found, _ = self.run_kill(app, point="kill:durable.writing@c.txt#1|save", kill_at=kill_at, after=[clean])
        self.assertEqual(found, [])


class FrameLatencyEdgeTests(unittest.TestCase):
    App = FrameLatencyTest.FakeApp

    @staticmethod
    def frame(index, ms, reason="prepared", requested="partial", rendered="full"):
        return (f"microide redraw frame: frame={index} reason={reason} requested={requested} rendered={rendered} "
                f"promoted=no dirty=1 clips=1 elapsed={ms:.2f} ms bounds=0,0,0,0\n")

    def oracle(self, app, slow=1.0):
        with mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", slow):
            return monkey_oracles.FrameLatency(app, budget=0.5)

    def test_the_budget_is_stretched_by_the_slow_factor_and_held_at_its_edge(self):
        self.assertEqual(self.oracle(self.App(), slow=3.0).budget_ms, 1500.0)
        app = self.App()
        oracle = self.oracle(app)
        oracle.check()
        app.log += self.frame(1, 500.0)
        self.assertEqual(oracle.check(), [], "exactly the budget is within it")
        app.log += self.frame(2, 500.5)
        self.assertEqual(oracle.check(), [("latency", "a prepared frame took 500 ms to prepare and paint "
                                                      "(partial requested, full rendered; budget 500 ms)")])

    def test_each_reason_is_filed_once_and_another_reason_is_filed(self):
        app = self.App()
        oracle = self.oracle(app)
        oracle.check()
        app.log += self.frame(1, 900.0) + self.frame(2, 950.0, reason="event-partial")
        first = oracle.check()
        self.assertEqual(len(first), 1, "one finding per check")
        self.assertIn("prepared frame", first[0][1])
        app.log += self.frame(3, 950.0, reason="event-partial")
        self.assertIn("event-partial frame", oracle.check()[0][1])

    def test_the_evidence_is_the_trace_up_to_the_slow_frame(self):
        app = self.App()
        oracle = self.oracle(app)
        oracle.check()
        app.log += "".join(f"trace line {i}\n" for i in range(60)) + self.frame(9, 900.0) + "after\n"
        oracle.check()
        evidence = oracle.evidence["frame-latency.txt"].decode()
        self.assertTrue(evidence.endswith("elapsed=900.00 ms bounds=0,0,0,0\n"), evidence[-80:])
        self.assertNotIn("after", evidence)
        self.assertEqual(evidence.count("\n"), 41, "forty lines before it and the frame itself")
        self.assertNotIn("trace line 19\n", evidence)
        self.assertIn("trace line 20\n", evidence)
        app.log += self.frame(10, 10.0)
        oracle.check()
        self.assertEqual(oracle.evidence, {}, "a check that files nothing keeps none")

    def test_a_new_reason_after_a_reported_one_in_the_same_window_is_filed(self):
        app = self.App()
        oracle = self.oracle(app)
        oracle.check()
        app.log += self.frame(1, 900.0)
        oracle.check()
        app.log += self.frame(2, 900.0) + self.frame(3, 900.0, reason="event-partial")
        found = oracle.check()
        self.assertEqual(len(found), 1)
        self.assertIn("event-partial frame", found[0][1])

    def test_a_slow_first_line_is_its_own_evidence(self):
        app = self.App()
        oracle = self.oracle(app)
        oracle.check()
        app.log += self.frame(1, 900.0)
        oracle.check()
        self.assertEqual(oracle.evidence["frame-latency.txt"].decode(), self.frame(1, 900.0))

    def test_a_window_reads_at_most_four_mebibytes(self):
        app = mock.Mock()
        app._log_size.side_effect = [10, 20]
        app.log_since.return_value = ""
        oracle = self.oracle(app)
        oracle.check()
        oracle.check()
        app.log_since.assert_called_once_with(10, 4 * 1024 * 1024)

    def test_only_the_lines_since_the_last_check_are_read(self):
        app = self.App()
        oracle = self.oracle(app)
        app.log = self.frame(1, 900.0)
        self.assertEqual(oracle.check(), [], "what was logged before the first check is not read")
        self.assertEqual(oracle.check(), [])
        app.log = ""
        self.assertEqual(oracle.check(), [], "a relaunched app's log starts over")
        app.log += self.frame(2, 900.0)
        self.assertEqual(len(oracle.check()), 1)


class ProcReaderTests(unittest.TestCase):
    """The oracles' /proc readers, against this test process and a child of it."""

    def test_the_main_threads_voluntary_wakes_are_read_and_grow(self):
        app = mock.Mock()
        app.proc.poll.return_value = None
        app.pid = os.getpid()
        oracle = monkey_oracles.IdleOracle(app)
        first = oracle._main_thread_wakes()
        self.assertIsInstance(first, int)
        for _ in range(5):
            time.sleep(0.001)
        self.assertGreater(oracle._main_thread_wakes(), first)
        app.proc.poll.return_value = 0
        self.assertIsNone(oracle._main_thread_wakes(), "an exited app has none")
        app.proc = None
        self.assertIsNone(oracle._main_thread_wakes())
        app.proc = mock.Mock()
        app.proc.poll.return_value = None
        app.pid = 2 ** 22 + 12345
        self.assertIsNone(oracle._main_thread_wakes(), "a pid that is gone")

    def test_a_sample_is_the_wait_channel_and_the_syscall(self):
        app = mock.Mock()
        app.pid = os.getpid()
        sample = monkey_oracles.IdleOracle(app)._sample_main_thread()
        parts = sample.split(" ")
        self.assertEqual(len(parts), 2, sample)
        self.assertTrue(all(parts), sample)
        app.pid = 2 ** 22 + 12345
        self.assertEqual(monkey_oracles.IdleOracle(app)._sample_main_thread(), "? ?")
        app.proc = None
        self.assertEqual(monkey_oracles.IdleOracle(app)._sample_main_thread(), "")

    def test_each_step_notes_the_rss_and_a_relaunch_starts_over(self):
        app = mock.Mock()
        app.proc.poll.return_value = None
        app.pid = os.getpid()
        app.actions = [1, 2, 3]
        oracle = monkey_oracles.ResourceOracle(app)
        oracle.note_step(1)
        oracle.note_step(2)
        self.assertEqual([(step, actions) for step, actions, _ in oracle.rss_steps], [(1, 3), (2, 3)])
        self.assertTrue(all(rss > 0 for _, _, rss in oracle.rss_steps))
        app.pid = os.getppid()
        oracle.note_step(3)
        self.assertEqual([step for step, _, _ in oracle.rss_steps], [3], "a new process starts the list afresh")
        app.proc.poll.return_value = 1
        oracle.note_step(4)
        self.assertEqual(len(oracle.rss_steps), 1, "an exited app notes nothing")
        app.proc = None
        oracle.note_step(5)
        self.assertEqual(len(oracle.rss_steps), 1)


class RssTimelineTests(unittest.TestCase):
    def oracle(self, steps, actions=()):
        app = mock.Mock()
        app.actions = list(actions)
        oracle = monkey_oracles.ResourceOracle(app)
        oracle.rss_steps = list(steps)
        return oracle

    def test_one_step_is_no_timeline(self):
        self.assertEqual(self.oracle([(0, 0, 10)]).rss_timeline(), "")
        self.assertEqual(self.oracle([]).rss_timeline(), "")

    def test_the_rows_are_thinned_to_forty(self):
        timeline = self.oracle([(i, i, 100 + i) for i in range(200)]).rss_timeline(jumps=0)
        rows = timeline.splitlines()
        self.assertEqual(rows[0], "step  actions  rss_kb")
        self.assertEqual(len(rows), 41)
        self.assertEqual(rows[1], "    0        0  100")
        self.assertEqual(rows[2], "    5        5  105")

    def test_the_largest_jumps_come_first_and_a_fall_is_none(self):
        steps = [(0, 0, 100), (1, 1, 150), (2, 2, 400), (3, 3, 300), (4, 4, 320)]
        actions = [{"a": i} for i in range(5)]
        timeline = self.oracle(steps, actions).rss_timeline(jumps=5, around=1)
        jumps = [line for line in timeline.splitlines() if line.startswith("+")]
        self.assertEqual(jumps, ["+250 kB between step 1 and step 2 (actions 1..2):",
                                 "+50 kB between step 0 and step 1 (actions 0..1):",
                                 "+20 kB between step 3 and step 4 (actions 3..4):"])
        block = timeline.split("+250 kB", 1)[1].split("\n+", 1)[0]
        self.assertIn('{"a": 0}', block, "`around` actions before the jump's own")
        self.assertIn('{"a": 1}', block)
        self.assertNotIn('{"a": 2}', block)
        self.assertTrue(timeline.endswith("\n"))

    def test_a_long_action_is_cut_at_three_hundred_characters(self):
        timeline = self.oracle([(0, 0, 1), (1, 1, 2)], [{"t": "x" * 400}]).rss_timeline(jumps=1, around=0)
        line = next(line for line in timeline.splitlines() if '"t"' in line)
        self.assertEqual(len(line), 2 + 300 + len("... (409 chars)"))
        self.assertTrue(line.endswith("... (409 chars)"))
        short = self.oracle([(0, 0, 1), (1, 1, 2)], [{"t": "x"}]).rss_timeline(jumps=1, around=0)
        self.assertIn('  {"t": "x"}\n', short)


class UndoPickTests(unittest.TestCase):
    def test_one_to_four_undos_from_the_oracles_own_generator(self):
        picks = {monkey_oracles.UndoAlgebra(mock.Mock(), random.Random(seed)).pick() for seed in range(60)}
        self.assertEqual(picks, {1, 2, 3, 4})
        self.assertEqual(monkey_oracles.UndoAlgebra(mock.Mock(), random.Random(7)).pick(),
                         random.Random(7).randint(1, 4))


class Kill9DetailTests(Kill9ProcedureTests):
    """The details of kill9_and_check the procedure tests left open."""

    def test_the_snapshot_is_every_projects_documents(self):
        app = self.app()
        app.active = 0
        oracle, _, _ = self.run_kill(app)
        self.assertEqual({doc["path"] for doc in oracle.last_kill["before"]}, {"/p/a/x.txt", "/p/b/y.txt"})

    def test_the_killed_log_is_read_up_to_sixteen_mebibytes(self):
        app = self.app()
        self.run_kill(app)
        self.assertEqual(app.read_limits, [16 * 1024 * 1024])

    def test_the_state_copy_keeps_links_as_links(self):
        app = self.app()
        state = app.work / "state"
        state.mkdir(parents=True)
        (state / "session").write_text("record")
        (state / "session.link").symlink_to("session")
        oracle, _, _ = self.run_kill(app)
        self.assertTrue((oracle.last_kill_state / "session.link").is_symlink())

    def test_an_app_without_a_work_tree_is_killed_all_the_same(self):
        app = _RelaunchingApp()
        del app.work
        oracle, found, _ = self.run_kill(app)
        self.assertIsNone(oracle.last_kill_state)
        self.assertEqual(found, [])

    def test_the_point_is_what_the_kill_is_armed_with(self):
        app = self.app()
        armed = []
        self.run_kill(app, point="kill:durable.writing@session#1|edit",
                      kill_at=lambda oracle, point: (armed.append(point), (False, [], None))[1])
        self.assertEqual(armed, ["kill:durable.writing@session#1|edit"])

    def test_a_point_always_waits_whatever_the_draw(self):
        app = self.app()
        rng = mock.Mock()
        rng.random.return_value = 0.99
        self.run_kill(app, rng=rng, waited=None, point="kill:x@y#1|edit", kill_at=lambda oracle, p: (False, [], None))
        self.assertTrue(app.actions[-1]["waited"])
        app = self.app()
        rng.random.return_value = 0.7
        self.run_kill(app, rng=rng, waited=None)
        self.assertFalse(app.actions[-1]["waited"], "0.7 itself does not wait")

    def test_the_point_is_named_by_its_rule_whatever_its_file_holds(self):
        for point, named in (("kill:durable.writing@a:b.txt#1|edit", "durable.writing@a:b.txt#1"),
                             ("kill:durable.writing@a|b.txt#1|edit", "durable.writing@a"),
                             ("durable.writing@x#1|edit", "durable.writing@x#1")):
            app = self.app()
            doc = {"path": "/p/b/y.txt", "dirty": True, "sha256": "bb", "bytes": 3, "project": "/p/b"}
            app.docs[1] = [doc]
            _, found, _ = self.run_kill(app, point=point, kill_at=lambda oracle, p: (True, [doc], None), after=[])
            self.assertEqual(found, [f"the unsaved buffer y.txt did not come back after a kill -9 (the crash-safety "
                                     f"flush had 3.5s of quiet) (killed at {named})"], point)
        app = self.app()
        app.docs[1] = [doc]
        _, found, _ = self.run_kill(app, point="kill:x@y#1|edit", kill_at=lambda oracle, p: (False, [doc], None),
                                    after=[])
        self.assertEqual(found, ["the unsaved buffer y.txt did not come back after a kill -9 (the crash-safety "
                                 "flush had 3.5s of quiet)"], "a trigger that did not die names no point")

    def test_an_untouched_clean_file_is_still_held_across_a_point_kill(self):
        path = self.dir / "c.txt"
        path.write_bytes(b"clean\n")
        clean = {"path": str(path), "dirty": False, "sha256": hashlib.sha256(b"clean\n").hexdigest(),
                 "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [clean]
        _, found, _ = self.run_kill(app, point="kill:durable.writing@session#1|edit",
                                    kill_at=lambda oracle, p: (True, [clean], None), after=[clean],
                                    between=lambda: path.write_bytes(b"changed\n"))
        self.assertEqual(found, ["c.txt changed on disk across a kill -9 and relaunch, and nothing saved it"])



class Kill9EachBufferTests(Kill9ProcedureTests):
    """Every buffer is judged: a buffer the check passes over must not end the
    check for the buffers after it."""

    LOST = {"path": "/p/b/z.txt", "dirty": True, "sha256": "zz", "bytes": 2, "project": "/p/b"}
    LOST_MESSAGE = ("the unsaved buffer z.txt did not come back after a kill -9 (the crash-safety flush had 3.5s "
                    "of quiet)")

    def test_after_a_buffer_saved_before_the_kill(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"saved\n")
        first = {"path": str(path), "dirty": True, "sha256": hashlib.sha256(b"saved\n").hexdigest(),
                 "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [first, self.LOST]
        _, found, _ = self.run_kill(app, after=[])
        self.assertEqual(found, [self.LOST_MESSAGE])

    def test_after_a_buffer_its_log_says_was_saved(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"saved\n")
        sha = hashlib.sha256(b"saved\n").hexdigest()
        first = {"path": str(path), "dirty": True, "sha256": "unsaved", "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [first, self.LOST]
        relaunch = app.crash_and_relaunch

        def crash(*args, **kwargs):
            if kwargs.get("between"):
                app.log += _io_line("write", str(path), sha)
            relaunch(*args, **kwargs)
        app.crash_and_relaunch = crash
        _, found, _ = self.run_kill(app, after=[])
        self.assertEqual(found, [self.LOST_MESSAGE])

    def test_a_buffer_back_dirty_was_not_saved_whatever_the_log_says(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"saved\n")
        sha = hashlib.sha256(b"saved\n").hexdigest()
        first = {"path": str(path), "dirty": True, "sha256": "unsaved", "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [first]
        relaunch = app.crash_and_relaunch

        def crash(*args, **kwargs):
            if kwargs.get("between"):
                app.log += _io_line("write", str(path), sha)
            relaunch(*args, **kwargs)
        app.crash_and_relaunch = crash
        _, found, _ = self.run_kill(app, after=[{**first, "sha256": sha, "bytes": 6}])
        self.assertEqual(found, ["the unsaved buffer a.txt came back after a kill -9 with different content "
                                 "(6 -> 6 bytes)"])

    def test_after_a_buffer_in_the_file_a_killed_save_was_writing(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"old\n")
        first = {"path": str(path), "dirty": True, "sha256": "staged", "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [first, self.LOST]

        def kill_at(oracle, point):
            oracle.saved_file_before = hashlib.sha256(b"old\n").hexdigest()
            return True, [first, self.LOST], str(path)
        _, found, _ = self.run_kill(app, point="kill:durable.renaming@a.txt#1|save", kill_at=kill_at,
                                    after=[{**first, "sha256": "transformed"}])
        self.assertEqual(found, [self.LOST_MESSAGE + " (killed at durable.renaming@a.txt#1)"])

    def test_a_save_point_with_no_logged_write_is_not_a_part_way_save(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"other\n")
        doc = {"path": str(path), "dirty": True, "sha256": "staged", "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [doc]

        def kill_at(oracle, point):
            oracle.saved_file_before = "something else"
            return True, [doc], str(path)
        _, found, _ = self.run_kill(app, point="kill:durable.renaming@a.txt#1|save", kill_at=kill_at, after=[doc])
        self.assertEqual(found, [])

    def test_after_a_trigger_changed_buffer_back_clean_in_its_file(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"saved\n")
        held = hashlib.sha256(b"saved\n").hexdigest()
        first = {"path": str(path), "dirty": True, "sha256": "staged", "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [first, self.LOST]
        _, found, _ = self.run_kill(app, point="kill:x@y#1|edit",
                                    kill_at=lambda oracle, p: (True, [{**first, "sha256": "edited"}, self.LOST], None),
                                    after=[{**first, "sha256": held, "dirty": False}])
        self.assertEqual(found, [self.LOST_MESSAGE + " (killed at x@y#1)"])

    def test_after_an_untouched_buffer_back_clean_in_its_file(self):
        path = self.dir / "a.txt"
        path.write_bytes(b"saved\n")
        held = hashlib.sha256(b"saved\n").hexdigest()
        first = {"path": str(path), "dirty": True, "sha256": held, "bytes": 6, "project": "/p/b"}
        app = self.app()
        app.docs[1] = [first, self.LOST]
        _, found, _ = self.run_kill(app, after=[{**first, "dirty": False}])
        self.assertEqual(found, [self.LOST_MESSAGE])

    def test_after_a_merge_result_that_came_back_untitled(self):
        path = "/p/b/m.cpp"
        file_buffer = {"path": path, "dirty": True, "sha256": "file", "bytes": 4, "project": "/p/b",
                       "title": "compare: m.cpp", "surface": "compare"}
        result = {"path": path, "dirty": True, "sha256": "result", "bytes": 6, "project": "/p/b",
                  "title": "merge: m.cpp", "surface": "merge"}
        app = self.app()
        app.docs[1] = [result, file_buffer, self.LOST]
        untitled = {"path": "", "dirty": True, "sha256": "result", "bytes": 6, "project": "/p/b",
                    "title": "Untitled-1", "surface": "editor"}
        _, found, _ = self.run_kill(app, after=[file_buffer, untitled])
        self.assertEqual(found, [self.LOST_MESSAGE])

    def test_a_merge_result_may_come_back_as_the_trigger_left_it(self):
        path = "/p/b/m.cpp"
        file_buffer = {"path": path, "dirty": True, "sha256": "file", "bytes": 4, "project": "/p/b",
                       "title": "compare: m.cpp", "surface": "compare"}
        result = {"path": path, "dirty": True, "sha256": "result", "bytes": 6, "project": "/p/b",
                  "title": "merge: m.cpp", "surface": "merge"}
        app = self.app()
        app.docs[1] = [result, file_buffer]
        edited = {**result, "sha256": "edited"}
        for back_sha in ("result", "edited"):
            untitled = {"path": "", "dirty": True, "sha256": back_sha, "bytes": 6, "project": "/p/b",
                        "title": "Untitled-1", "surface": "editor"}
            app = self.app()
            app.docs[1] = [result, file_buffer]
            _, found, _ = self.run_kill(app, point="kill:x@y#1|edit",
                                        kill_at=lambda oracle, p: (True, [edited, file_buffer], None),
                                        after=[file_buffer, untitled])
            self.assertEqual(found, [], back_sha)

    def test_a_buffer_back_as_staged_after_a_trigger_edit_is_fine(self):
        doc = {"path": "/p/b/y.txt", "dirty": True, "sha256": "staged", "bytes": 3, "project": "/p/b"}
        for back_sha in ("staged", "edited"):
            app = self.app()
            app.docs[1] = [doc]
            _, found, _ = self.run_kill(app, point="kill:x@y#1|edit",
                                        kill_at=lambda oracle, p: (True, [{**doc, "sha256": "edited"}], None),
                                        after=[{**doc, "sha256": back_sha}])
            self.assertEqual(found, [], back_sha)


class _LappingApp(_LoggingRelaunchingApp):
    """Two projects that both owe a buffer, so the relaunch loads one and the
    oracle's lap the other; `keeps` are paths an autosave cannot write, and
    `drop_after_lap` paths the lap's documents no longer list."""

    def __init__(self, work: Path) -> None:
        super().__init__(work)
        self.keeps: set[str] = set()
        self.drop_after_lap: set[str] = set()
        self.docs = {0: [{"path": "/p/a/x.txt", "dirty": True, "sha256": "xx", "bytes": 2, "project": "/p/a"}],
                     1: [{"path": "/p/b/y.txt", "dirty": True, "sha256": "yy", "bytes": 2, "project": "/p/b"}]}

    def send_line(self, line, timeout=15.0, record=True):
        if line == "project-next":
            self.lines.append(line)
            if self.autosave == "on_focus_change":
                for doc in self.docs[self.active]:
                    if doc["dirty"] and doc["path"] not in self.keeps:
                        doc.update(dirty=False, sha256=doc["sha256"] + "-saved")
            at = self.catalog.index(self.active)
            self.active = self.catalog[(at + 1) % len(self.catalog)]
            self.loaded[self.active] = True
            for p in self.docs:
                self.docs[p] = [d for d in self.docs[p] if d["path"] not in self.drop_after_lap]
            return {}
        return super().send_line(line, timeout, record)


class Kill9LapTests(Kill9ProcedureTests):
    """The lap that loads the projects the relaunch did not, with and without
    an autosave that saves what it leaves."""

    def app(self):
        return _LappingApp(self.dir / "work")

    def test_the_lap_with_autosave_saves_both_and_owes_nothing(self):
        app = self.app()
        app.autosave = "on_focus_change"
        _, found, _ = self.run_kill(app)
        self.assertEqual(found, [], "the active project's buffer is judged as read before the lap")
        self.assertIn("project-next", app.lines)

    def test_a_background_buffer_back_changed_without_autosave_is_reported(self):
        app = self.app()
        relaunch = app.crash_and_relaunch

        def changed(*args, **kwargs):
            relaunch(*args, **kwargs)
            app.docs[0] = [{**app.docs[0][0], "sha256": "other", "bytes": 5}]
        app.crash_and_relaunch = changed
        _, found, _ = self.run_kill(app)
        self.assertEqual(found, ["the unsaved buffer x.txt came back after a kill -9 with different content "
                                 "(2 -> 5 bytes)"])

    def test_a_background_buffer_the_lap_could_not_save_is_still_judged(self):
        app = self.app()
        app.autosave = "on_focus_change"
        app.keeps = {"/p/a/x.txt"}
        relaunch = app.crash_and_relaunch

        def changed(*args, **kwargs):
            relaunch(*args, **kwargs)
            app.docs[0] = [{**app.docs[0][0], "sha256": "other", "bytes": 5}]
        app.crash_and_relaunch = changed
        _, found, _ = self.run_kill(app)
        self.assertEqual(found, ["the unsaved buffer x.txt came back after a kill -9 with different content "
                                 "(2 -> 5 bytes)"])

    def test_a_buffer_after_one_the_lap_saved_is_still_judged(self):
        app = self.app()
        app.autosave = "on_focus_change"
        app.docs[0].append({"path": "/p/a/z.txt", "dirty": True, "sha256": "zz", "bytes": 2, "project": "/p/a"})
        app.drop_after_lap = {"/p/a/z.txt"}
        _, found, _ = self.run_kill(app)
        self.assertEqual(found, ["the unsaved buffer z.txt did not come back after a kill -9 (the crash-safety "
                                 "flush had 3.5s of quiet)"])

    def test_a_buffer_read_before_the_lap_is_back_though_the_lap_lists_it_no_more(self):
        app = self.app()
        app.drop_after_lap = {"/p/b/y.txt"}
        _, found, _ = self.run_kill(app)
        self.assertEqual(found, [])

    def test_a_link_to_a_file_the_lap_saved_is_not_changed_by_nothing(self):
        target = self.dir / "x.txt"
        target.write_bytes(b"before\n")
        link = self.dir / "link.txt"
        link.symlink_to(target)
        app = self.app()
        app.autosave = "on_focus_change"
        app.docs[0] = [{"path": str(target), "dirty": True, "sha256": "xx", "bytes": 2, "project": "/p/a"}]
        app.docs[1].append({"path": str(link), "dirty": False, "sha256": hashlib.sha256(b"before\n").hexdigest(),
                            "bytes": 7, "project": "/p/b"})
        send = app.send_line

        def lap(line, timeout=15.0, record=True):
            if line == "project-next" and app.active == 0:
                target.write_bytes(b"autosaved\n")
            return send(line, timeout, record)
        app.send_line = lap
        _, found, _ = self.run_kill(app)
        self.assertEqual(found, [])
        app = self.app()
        app.docs[0] = [{"path": str(target), "dirty": True, "sha256": "xx", "bytes": 2, "project": "/p/a"}]
        target.write_bytes(b"before\n")
        app.docs[1].append({"path": str(link), "dirty": False, "sha256": hashlib.sha256(b"before\n").hexdigest(),
                            "bytes": 7, "project": "/p/b"})
        _, found, _ = self.run_kill(app, between=lambda: target.write_bytes(b"changed\n"))
        self.assertIn("link.txt changed on disk across a kill -9 and relaunch, and nothing saved it", found)

    def test_every_clean_file_is_checked_past_one_a_save_explains(self):
        saved = self.dir / "s.txt"
        saved.write_bytes(b"s\n")
        other = self.dir / "o.txt"
        other.write_bytes(b"o\n")
        app = self.app()
        app.autosave = "on_focus_change"
        app.docs[0] = [{"path": str(saved), "dirty": True, "sha256": "s0", "bytes": 2, "project": "/p/a"}]
        app.docs[1].extend([
            {"path": str(saved), "dirty": False, "sha256": hashlib.sha256(b"s\n").hexdigest(), "bytes": 2,
             "project": "/p/b", "title": "s"},
            {"path": str(other), "dirty": False, "sha256": hashlib.sha256(b"o\n").hexdigest(), "bytes": 2,
             "project": "/p/b"}])
        send = app.send_line

        def lap(line, timeout=15.0, record=True):
            if line == "project-next" and app.active == 0:
                saved.write_bytes(b"autosaved\n")
            return send(line, timeout, record)
        app.send_line = lap
        _, found, _ = self.run_kill(app, between=lambda: other.write_bytes(b"changed\n"))
        self.assertEqual(found, ["o.txt changed on disk across a kill -9 and relaunch, and nothing saved it"])


class Kill9SavedFilesTests(Kill9LapTests):
    """Which files the disk check may excuse as saved by the lap: only those of
    background buffers the lap's autosave actually left clean."""

    def link_case(self, *, autosave, back_x_clean=False, keep_x=False, x_in_active=False):
        target = self.dir / "x.txt"
        target.write_bytes(b"before\n")
        link = self.dir / "link.txt"
        if not link.is_symlink():
            link.symlink_to(target)
        app = self.app()
        app.autosave = autosave
        if keep_x:
            app.keeps = {str(target)}
        project = 1 if x_in_active else 0
        app.docs[project] = [{"path": str(target), "dirty": True, "sha256": "xx", "bytes": 2,
                              "project": app.ROOTS[project]}] + (app.docs[project] if x_in_active else [])
        app.docs[1].append({"path": str(link), "dirty": False, "sha256": hashlib.sha256(b"before\n").hexdigest(),
                            "bytes": 7, "project": "/p/b"})
        relaunch = app.crash_and_relaunch

        def crash(*args, **kwargs):
            relaunch(*args, **kwargs)
            target.write_bytes(b"changed outside\n")
            if back_x_clean:
                app.docs[project][0] = {**app.docs[project][0], "dirty": False}
        app.crash_and_relaunch = crash
        _, found, _ = self.run_kill(app)
        return found

    LINK = "link.txt changed on disk across a kill -9 and relaunch, and nothing saved it"

    def test_without_autosave_nothing_is_excused(self):
        self.assertIn(self.LINK, self.link_case(autosave="off", back_x_clean=True))

    def test_the_active_projects_buffer_is_not_excused(self):
        self.assertIn(self.LINK, self.link_case(autosave="off", x_in_active=True))

    def test_a_buffer_the_lap_left_dirty_is_not_excused(self):
        self.assertIn(self.LINK, self.link_case(autosave="on_focus_change", keep_x=True))


class LeakEvidenceFormatTests(unittest.TestCase):
    @staticmethod
    def stat(pid, comm, state="S", start=500):
        rest = [state] + ["0"] * 18 + [str(start)] + ["0"] * 5
        return f"{pid} ({comm}) " + " ".join(rest)

    def test_children_by_program_with_state_age_and_command(self):
        report = monkey_oracles.ResourceOracle.children_report([
            (11, self.stat(11, "bash", "S", 500), b"/bin/bash\0-l\0"),
            (12, self.stat(12, "sle) ep", "Z", 900), b"sleep\x0030\0"),
            (13, self.stat(13, "bash", "R", 300), b"/usr/bin/bash\0"),
        ], uptime=20.0, ticks=100)
        self.assertEqual(report, "3 direct children by program: {'bash': 2, 'sleep': 1}\n"
                                 "pid state age_s  command\n"
                                 "11 S     15.0  /bin/bash -l\n"
                                 "12 Z     11.0  sleep 30\n"
                                 "13 R     17.0  /usr/bin/bash\n")

    def test_a_child_with_an_unreadable_stat_or_no_command_line(self):
        report = monkey_oracles.ResourceOracle.children_report([
            (11, "11 (x) S 1 2", b"x\0"),
            (12, self.stat(12, "y"), b""),
            (13, self.stat(13, "z", start=10).replace(" 10 ", " ten "), b"z\0"),
        ], uptime=10.0, ticks=100)
        self.assertEqual(report, "3 direct children by program: {'?': 1}\n"
                                 "pid state age_s  command\n"
                                 "12 S      5.0  \n")

    def test_a_long_command_line_is_cut_at_two_hundred(self):
        report = monkey_oracles.ResourceOracle.children_report(
            [(5, self.stat(5, "p"), b"p\0" + b"a" * 300)], uptime=10.0, ticks=100)
        line = report.splitlines()[2]
        self.assertEqual(line, "5 S      5.0  " + ("p " + "a" * 300)[:200])

    def test_a_descriptor_is_counted_by_what_it_points_at(self):
        kind = monkey_oracles.ResourceOracle.fd_kind
        self.assertEqual(kind("pipe:[123]"), "pipe")
        self.assertEqual(kind("anon_inode:[eventfd]"), "anon_inode")
        self.assertEqual(kind("socket:[1:2]"), "socket")
        self.assertEqual(kind("/dev/pts/3"), "/dev/pts")
        self.assertEqual(kind("/dev/null"), "/dev/null")
        self.assertEqual(kind("/home/u/a.txt"), "file:.txt")
        self.assertEqual(kind("/home/u/odd:name.log"), "file:.log")
        self.assertEqual(kind("/home/u/Makefile"), "file:")
        self.assertEqual(monkey_oracles.ResourceOracle.fds_report(
            ["pipe:[1]", "pipe:[2]", "/dev/null", "/a/b.txt", "pipe:[3]"]),
            "    3  pipe\n    1  /dev/null\n    1  file:.txt\n")

    def test_threads_now_against_the_baseline(self):
        report = monkey_oracles.ResourceOracle.threads_report(
            collections.Counter({"worker": 5, "main": 1}), collections.Counter({"worker": 2, "gone": 3}))
        self.assertEqual(report, "threads by name: now (at the baseline, change)\n"
                                 "     5  (   2, +3)  worker\n"
                                 "     1  (   0, +1)  main\n"
                                 "     0  (   3, -3)  gone\n")

    def test_the_evidence_reads_the_logs_in_order_and_names_the_origins(self):
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        app = mock.Mock()
        app.work = directory
        app.binary = None
        app.actions = []
        oracle = monkey_oracles.ResourceOracle(app)
        first = directory / "microide-2.log"
        second = directory / "microide-1.log"
        first.write_text("microide terminal-life: start pid=99\n")
        second.write_text("  #0 0x1 in StartPty\n")
        os.utime(first, (1, 1))
        os.utime(second, (2, 2))
        (directory / "other.log").write_text("microide terminal-life: start pid=98\n")
        with mock.patch.object(monkey_oracles, "terminal_origins", wraps=monkey_oracles.terminal_origins) as origins:
            out = oracle._what_it_holds(os.getpid())
        log_text, alive, binary = origins.call_args.args
        self.assertEqual(log_text, "microide terminal-life: start pid=99\n  #0 0x1 in StartPty\n",
                         "microide logs only, oldest first")
        self.assertIsNone(binary)
        self.assertNotIn("children-origin.txt", out, "no alive child has a traced start")
        self.assertNotIn("rss-timeline.txt", out)
        oracle.rss_steps = [(0, 0, 1), (1, 0, 2)]
        with mock.patch.object(monkey_oracles, "terminal_origins", return_value="origins\n"):
            out = oracle._what_it_holds(os.getpid())
        self.assertEqual(out["children-origin.txt"], b"origins\n")
        self.assertTrue(out["rss-timeline.txt"].startswith(b"step  actions  rss_kb"))
        app.binary = "/bin/microide"
        with mock.patch.object(monkey_oracles, "terminal_origins", return_value="") as origins:
            oracle._what_it_holds(os.getpid())
        self.assertEqual(origins.call_args.args[2], Path("/bin/microide"))


class LeakEvidenceReaderTests(unittest.TestCase):
    def test_uptime_is_the_boot_clock_or_zero(self):
        uptime = monkey_oracles.ResourceOracle._uptime
        self.assertAlmostEqual(uptime(), time.clock_gettime(time.CLOCK_BOOTTIME), delta=2.0)
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        (directory / "u").write_text("123.5 9999.0\n")
        self.assertEqual(uptime(directory / "u"), 123.5)
        for text in ("", "garbage 1\n"):
            (directory / "u").write_text(text)
            self.assertEqual(uptime(directory / "u"), 0.0, repr(text))
        self.assertEqual(uptime(directory / "missing"), 0.0)

    def test_a_gone_process_has_no_children_no_descriptors_and_no_child_record(self):
        gone = 2 ** 22 + 12345
        oracle = monkey_oracles.ResourceOracle
        self.assertEqual(oracle._child_pids(gone), [])
        self.assertIsNone(oracle._read_child(gone))
        self.assertEqual(oracle._fd_targets(gone), [])
        holds = monkey_oracles.ResourceOracle(mock.Mock(work=None, binary=None, actions=[]))._what_it_holds(gone)
        self.assertTrue(holds["children.txt"].startswith(b"0 direct children by program: {}"))
        self.assertEqual(holds["fds.txt"], b"\n")

    def test_this_process_reads_as_itself(self):
        child = monkey_oracles.ResourceOracle._read_child(os.getpid())
        self.assertEqual(child[0], os.getpid())
        self.assertTrue(child[1].startswith(f"{os.getpid()} ("))
        self.assertIn(b"python", child[2])
        with open(__file__) as held:
            self.assertIn(os.path.realpath(__file__), monkey_oracles.ResourceOracle._fd_targets(os.getpid()))
            del held

    def test_a_descriptor_closed_while_it_is_listed_is_skipped(self):
        read, write = os.pipe()
        real = os.readlink

        def closing(path, *args, **kwargs):
            if str(path).endswith(f"/{write}"):
                raise FileNotFoundError(path)
            return real(path, *args, **kwargs)
        try:
            with mock.patch.object(monkey_oracles.os, "readlink", side_effect=closing):
                targets = monkey_oracles.ResourceOracle._fd_targets(os.getpid())
        finally:
            os.close(read)
            os.close(write)
        self.assertTrue(any(t.startswith("pipe:") for t in targets))

    def test_a_command_line_or_log_that_is_not_utf8_is_still_read(self):
        report = monkey_oracles.ResourceOracle.children_report(
            [(5, LeakEvidenceFormatTests.stat(5, "p"), b"caf\xe9\0x\0")], uptime=10.0, ticks=100)
        self.assertIn("caf\ufffd x", report)
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        (directory / "microide-1.log").write_bytes(b"\xff microide terminal-life: start pid=1\n")
        app = mock.Mock(work=directory, binary=None, actions=[])
        with mock.patch.object(monkey_oracles, "terminal_origins", return_value="") as origins:
            monkey_oracles.ResourceOracle(app)._what_it_holds(os.getpid())
        self.assertTrue(origins.call_args.args[0].startswith("\ufffd microide"))

    def test_the_start_field_must_be_there_and_a_number(self):
        stat20 = "5 (p) S " + " ".join(["0"] * 18) + " 500"
        stat19 = "5 (p) S " + " ".join(["0"] * 18)
        report = monkey_oracles.ResourceOracle.children_report([(5, stat20, b"p\0"), (6, stat19, b"q\0")],
                                                               uptime=10.0, ticks=100)
        self.assertEqual(report.splitlines()[2:], ["5 S      5.0  p"])

    def test_the_program_is_the_first_word_whatever_follows(self):
        report = monkey_oracles.ResourceOracle.children_report(
            [(5, LeakEvidenceFormatTests.stat(5, "p"), b"/bin/sh\0-c\0a b\0")], uptime=10.0, ticks=100)
        self.assertTrue(report.startswith("1 direct children by program: {'sh': 1}"), report)
        self.assertEqual(monkey_oracles.ResourceOracle.fd_kind("socket:[1:2:3]"), "socket")


class LeakEvidenceChildTests(unittest.TestCase):
    def test_the_children_of_a_process_are_listed(self):
        import subprocess
        child = subprocess.Popen(["sleep", "30"])
        try:
            self.assertIn(child.pid, monkey_oracles.ResourceOracle._child_pids(os.getpid()))
        finally:
            child.kill()
            child.wait()

    def test_an_unreadable_log_is_passed_over(self):
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        locked = directory / "microide-1.log"
        locked.write_text("secret\n")
        locked.chmod(0)
        self.addCleanup(locked.chmod, 0o600)
        (directory / "microide-2.log").write_text("readable\n")
        app = mock.Mock(work=directory, binary=None, actions=[])
        with mock.patch.object(monkey_oracles, "terminal_origins", return_value="") as origins:
            monkey_oracles.ResourceOracle(app)._what_it_holds(os.getpid())
        self.assertEqual(origins.call_args.args[0], "readable\n")


class StructuralDiffDetailTests(StructuralDiffOracleTests):
    REAL = "changes more than whitespace, and shows it as layout only (no novel token, no added or deleted line)"

    def test_a_row_outside_any_hunk_is_not_joined_to_one(self):
        row = _diff_row(left_text="x = 1", right_text="x = 2")
        del row["hunk"]
        _, found = self.check([row])
        self.assertEqual(found, [])

    def test_a_cut_row_does_not_stop_the_rows_after_it(self):
        cut = _diff_row(kind="unchanged", left_spans=[[0, 1]], textTruncated=True, hunk=-1)
        bad = _diff_row(kind="unchanged", left_text="a", right_text="a", right_spans=[[0, 1]], hunk=-1)
        _, found = self.check([cut, bad])
        self.assertEqual(found, ["the right side of row 2 (unchanged, right line 1) marks novel tokens on a side "
                                 "that did not change"])

    def test_a_side_with_no_line_number_is_absent(self):
        row = _diff_row(kind="added", left_text="", right_text="new", left_spans=[[0, 1]], right_spans=[[0, 3]])
        del row["leftLine"]
        _, found = self.check([row])
        self.assertEqual(found, ["the left side of row 1 (added, left line None) marks novel tokens on a side "
                                 "that is absent"])
        row = _diff_row(kind="deleted", right=0, left_text="old", right_text="", left_spans=[[0, 3]],
                        right_spans=[[0, 1]])
        _, found = self.check([row])
        self.assertEqual(found, ["the right side of row 1 (deleted, right line 0) marks novel tokens on a side "
                                 "that is absent"])
        del row["rightLine"]
        _, found = self.check([row])
        self.assertEqual(found, ["the right side of row 1 (deleted, right line None) marks novel tokens on a side "
                                 "that is absent"])

    def test_both_sides_of_a_row_are_judged(self):
        row = _diff_row(kind="added", left=0, left_text="", right_text="abc", left_spans=[[0, 1]],
                        right_spans=[[0, 9]])
        _, found = self.check([row])
        self.assertEqual(found, ["the left side of row 1 (added, left line 0) marks novel tokens on a side that is "
                                 "absent",
                                 "the right side of row 1 (added, right line 1) has a novel span [0, 9) outside its "
                                 "3-byte line or out of order"])
        row = _diff_row(left_text="\u00e9", right_text="abcd", left_spans=[[0, 1]], right_spans=[[0, 9]])
        _, found = self.check([row])
        self.assertEqual(len(found), 2, "a split on the left does not stop the right")

    def test_a_side_with_no_text_is_an_empty_line(self):
        row = _diff_row(left_text="x", right_text="y", left_spans=[[0, 2]])
        del row["leftText"]
        _, found = self.check([row])
        self.assertIn("has a novel span [0, 2) outside its 0-byte line or out of order", found[0])

    def test_an_empty_span_is_out_of_order(self):
        _, found = self.check([_diff_row(left_text="ab", right_text="ac", left_spans=[[1, 1]])])
        self.assertIn("has a novel span [1, 1) outside its 2-byte line", found[0])

    def test_the_split_evidence_is_the_row_the_side_and_the_bytes(self):
        row = _diff_row(left_text="\u00e9", right_text="e", left_spans=[[0, 1]])
        oracle, _ = self.check([row])
        evidence = oracle.evidence["split-span-row-1.json"]
        self.assertEqual(json.loads(evidence), {"row": row, "side": "left", "bytes": "c3a9"})
        self.assertTrue(evidence.startswith(b'{\n "row": {\n  "kind"'), evidence[:30])
        oracle, _ = self.check([row, dict(row)])
        self.assertEqual(set(oracle.evidence), {"split-span-row-1.json", "split-span-row-2.json"})

    def test_a_cut_row_list_drops_only_its_last_hunk(self):
        rows = [_diff_row(left_text="x = 1", right_text="x = 1", hunk=0),
                _diff_row(left_text="y = 1", right_text="y = 2", hunk=1),
                _diff_row(left_text="z = 1", right_text="z = 2", hunk=2)]
        _, found = self.check(rows, rowsTruncated=True)
        self.assertEqual(found, [f"hunk 2 {self.REAL}"])

    def test_a_hunk_with_a_cut_row_does_not_stop_the_hunks_after_it(self):
        rows = [_diff_row(left_text="x = 1", right_text="x = 2", hunk=0, textTruncated=True),
                _diff_row(left_text="y = 1", right_text="y = 2", hunk=1)]
        _, found = self.check(rows)
        self.assertEqual(found, [f"hunk 2 {self.REAL}"])

    def test_a_layout_only_hunk_over_several_rows_is_judged_as_one_text(self):
        rows = [_diff_row(left_text="call(a,", right_text="call(a, b)", left_spans=[[0, 4]], hunk=0),
                _diff_row(kind="deleted", right=0, left_text="     b)", right_text="", hunk=0)]
        oracle, found = self.check(rows)
        self.assertEqual(found, ["hunk 1 changes only whitespace and line breaks, and marks tokens novel"])
        self.assertEqual(json.loads(oracle.evidence["structural-hunk-1.json"]), rows)
        rows = [_diff_row(left_text="call(a, b)", right_text="call(a,", right_spans=[[0, 4]], hunk=0),
                _diff_row(kind="added", left=0, left_text="", right_text="     b)", hunk=0)]
        _, found = self.check(rows)
        self.assertEqual(found, ["hunk 1 changes only whitespace and line breaks, and marks tokens novel"])

    def test_a_comment_or_a_quote_anywhere_in_the_hunk_excuses_its_marks(self):
        for left, right in (("a  # x", "a # x"), ("a /* x */", "a  /* x */"), ("a // b", "a  // b"),
                            ("f('a  b')", "f('a b')")):
            _, found = self.check([_diff_row(left_text=left, right_text=right, left_spans=[[0, 1]])])
            self.assertEqual(found, [], left)

    def test_an_added_or_deleted_line_with_text_shows_the_change(self):
        for kind, left, right in (("deleted", 1, 0), ("added", 0, 1)):
            rows = [_diff_row(left_text="x = 1", right_text="x = 2", hunk=0),
                    _diff_row(kind=kind, left=left, right=right, left_text="q" if left else "",
                              right_text="q" if right else "", hunk=0)]
            _, found = self.check(rows)
            self.assertEqual(found, [], kind)
        rows = [_diff_row(left_text="x = 1", right_text="x = 2", hunk=0),
                _diff_row(kind="changed", left_text="q", right_text="r", hunk=0)]
        _, found = self.check(rows)
        self.assertEqual(found, [f"hunk 1 {self.REAL}"], "a changed row is not an added or deleted line")


class StructuralDiffHunkTextTests(StructuralDiffOracleTests):
    """How a hunk's two texts are put together: the lines of each side, in order,
    a missing text an empty line, a side with no line number not there."""

    def test_a_row_without_its_text_is_an_empty_line_of_its_side(self):
        rows = [_diff_row(left_text="x", right_text="x", hunk=0, left_spans=[[0, 1]]),
                _diff_row(kind="changed", left_text="y", right_text="y", hunk=0)]
        del rows[1]["leftText"]
        del rows[1]["rightText"]
        _, found = self.check(rows)
        self.assertEqual(found, ["hunk 1 changes only whitespace and line breaks, and marks tokens novel"])

    def test_a_side_without_a_line_number_adds_nothing_to_its_text(self):
        rows = [_diff_row(left_text="x", right_text="x", hunk=0, left_spans=[[0, 1]]),
                _diff_row(kind="added", left_text="ghost", right_text="", hunk=0)]
        del rows[1]["leftLine"]
        rows[1]["rightLine"] = 0
        _, found = self.check(rows)
        self.assertEqual(found, ["hunk 1 changes only whitespace and line breaks, and marks tokens novel"])
        rows[1]["leftLine"] = 0
        del rows[1]["rightLine"]
        rows[1]["rightText"] = "ghost"
        rows[1]["leftText"] = ""
        _, found = self.check(rows)
        self.assertEqual(found, ["hunk 1 changes only whitespace and line breaks, and marks tokens novel"])

    def test_an_added_line_without_text_does_not_show_a_change(self):
        for missing in ("leftText", "rightText"):
            extra = _diff_row(kind="added", left=0, left_text="", right_text="   ", hunk=0)
            del extra[missing]
            rows = [_diff_row(left_text="x = 1", right_text="x = 2", hunk=0), extra]
            _, found = self.check(rows)
            self.assertEqual(len(found), 1, missing)

    def test_the_real_change_evidence_is_its_rows(self):
        rows = [_diff_row(left_text="x = 1", right_text="x = 2", hunk=4)]
        oracle, found = self.check(rows)
        self.assertEqual(found, ["hunk 5 changes more than whitespace, and shows it as layout only (no novel token, "
                                 "no added or deleted line)"])
        evidence = oracle.evidence["structural-hunk-5.json"]
        self.assertEqual(json.loads(evidence), rows)
        self.assertTrue(evidence.startswith(b'[\n {\n  "kind"'), evidence[:20])
        oracle, _ = self.check([_diff_row(left_text="f(a,  b)", right_text="f(a, b)", left_spans=[[4, 5]], hunk=2)])
        self.assertTrue(oracle.evidence["structural-hunk-3.json"].startswith(b'[\n {\n  "kind"'))


class RemoteConvergenceDetailTests(RemoteConvergenceTests):
    def test_outcomes_accumulate_over_checks(self):
        oracle, _, _ = self.check([self.SETTLED], trees=(({"a": "1"}, {"a": "1"}),))
        self.oracle = oracle
        for statuses, trees in (([self.SETTLED], (({"a": "1"}, {"a": "1"}),)),
                                ([{"elsewhere": True}, self.SETTLED], (({"a": "1"}, {"a": "1"}),)),
                                ([{"elsewhere": True}], (({}, {}),)),
                                ([{"elsewhere": True}], (({}, {}),)),
                                ([{"state": "x"}], (({}, {}),)), ([{"state": "x"}], (({}, {}),)),
                                ([{**self.SETTLED, "conflicts": 1}], (({}, {}),)),
                                ([{**self.SETTLED, "conflicts": 1}], (({}, {}),)),
                                ([{**self.SETTLED, "blocked": 1}], (({}, {}),)),
                                ([{**self.SETTLED, "blocked": 1}], (({}, {}),)),
                                ([self.SETTLED], (({"a": "1"}, {}),)), ([self.SETTLED], (({"a": "1"}, {}),)),
                                ([self.SETTLED, {**self.SETTLED, "filesPending": 1}], (({"a": "1"}, {}),)),
                                ([self.SETTLED, {**self.SETTLED, "filesPending": 1}], (({"a": "1"}, {}),))):
            self.check(statuses, trees=trees)
        self.assertEqual(dict(oracle.outcomes), {"agreed:1": 3, "reopened": 3, "elsewhere": 2, "unsettled": 2,
                                                 "conflicted": 2, "blocked": 2, "differed": 2, "moved": 2})

    def test_a_check_that_files_nothing_keeps_no_evidence(self):
        oracle, _, _ = self.check([{"state": "x"}])
        self.assertIn("remote-status.json", oracle.evidence)
        self.oracle = oracle
        self.check([self.SETTLED])
        self.assertEqual(oracle.evidence, {})

    def test_it_waits_half_a_second_between_reads(self):
        self.check([{"state": "syncing"}, self.SETTLED])
        self.assertEqual(self.slept, [0.5])
        self.check([self.SETTLED], trees=(({"a": "1"}, {}), ({"a": "1"}, {"a": "1"})))
        self.assertEqual(self.slept, [0.5])

    def test_the_unsettled_evidence_is_the_status_and_the_recent_notices(self):
        remote = {"state": "syncing", "filesPending": 3, "unsentPushes": 1}
        oracle, _, _ = self.check([remote])
        evidence = oracle.evidence["remote-status.json"]
        self.assertEqual(json.loads(evidence), {"remote": remote, "notifications": [{"message": "push refused"}]})
        self.assertTrue(evidence.startswith(b'{\n "remote": {\n  "state"'))
        self.notifications_fail = True
        oracle, _, _ = self.check([remote])
        self.assertEqual(json.loads(oracle.evidence["remote-status.json"])["notifications"], [])

    def test_each_reason_a_project_moved_while_it_compared(self):
        for key in ("conflicts", "heldDeletes", "blocked"):
            oracle, _, found = self.check([self.SETTLED, {**self.SETTLED, key: 1}], trees=(({"a": "1"}, {}),))
            self.assertEqual(found, [], key)
            self.assertEqual(dict(oracle.outcomes), {"moved": 1}, key)


class RemoteConvergenceEvidenceTests(RemoteConvergenceTests):
    def test_the_trees_evidence_names_each_path_on_both_sides(self):
        host = {"a.txt": "0123456789abcdef", "b.txt": "2", "c.txt": "3", "d.txt": "4"}
        mirror = {"a.txt": "fedcba9876543210", "c.txt": "x", "d.txt": "y", "e.txt": "5"}
        for name in ("a.txt", "b.txt", "c.txt", "d.txt"):
            (self.host / name).write_text(name)
        oracle, _, found = self.check([self.SETTLED], trees=((host, mirror),))
        self.assertEqual(found, ["the settled mirror and the host differ in 5 file(s); a.txt is with different content"])
        self.assertEqual(oracle.evidence["trees.txt"].decode(),
                         "a.txt: host 0123456789ab mirror fedcba987654\n"
                         "b.txt: host 2 mirror absent\n"
                         "c.txt: host 3 mirror x\n"
                         "d.txt: host 4 mirror y\n"
                         "e.txt: host absent mirror 5")
        sides = {name for name in oracle.evidence if name.startswith("diff-") and name.endswith(".host")}
        self.assertEqual(sides, {"diff-a.txt.host", "diff-b.txt.host", "diff-c.txt.host"}, "the first three paths")


class MemoryReportTests(unittest.TestCase):
    ROLLUP = ("55a0-7ffc ---p 00000000 00:00 0 [rollup]\nRss:  900 kB\nPss:  800 kB\nPss_Anon: 1 kB\n"
              "Anonymous:  700 kB\nSwap:  0 kB\nPrivate_Dirty:  600 kB\nShared_Clean: 5 kB\n")
    SMAPS = ("55a0-55a1 r--p 00000000 08:01 12 /usr/bin/microide\nSize: 4 kB\nRss:  100 kB\n"
             "55b0-55b1 rw-p 00000000 00:00 0 [heap]\nRss:  300 kB\nRss:  50 kB\n"
             "7f00-7f01 rw-p 00000000 00:00 0\nRss:  200 kB\n"
             "7f10-7f11 r--p 00000000 08:01 13 /usr/lib/libc.so\nRss:  100 kB\n"
             "VmFlags: rd wr mr mw me ac\n")

    def test_the_rollup_totals_and_the_largest_mappings(self):
        report = monkey_oracles.ResourceOracle.memory_report(self.ROLLUP, self.SMAPS)
        self.assertEqual(report, "Rss:  900 kB\nPss:  800 kB\nAnonymous:  700 kB\nSwap:  0 kB\n"
                                 "Private_Dirty:  600 kB\n"
                                 "largest resident mappings (kB):\n"
                                 "         350  [heap]\n"
                                 "         200  [anonymous]\n"
                                 "         100  /usr/bin/microide\n"
                                 "         100  /usr/lib/libc.so\n")

    def test_rss_before_any_mapping_and_only_twelve_mappings(self):
        smaps = "Rss: 7 kB\n" + "".join(f"{i:x}0-{i:x}1 r--p 0 0:0 1 /f{i}\nRss: {100 + i} kB\n" for i in range(14))
        report = monkey_oracles.ResourceOracle.memory_report("", smaps).splitlines()
        self.assertEqual(report[0], "largest resident mappings (kB):")
        self.assertEqual(len(report), 13)
        self.assertEqual(report[1], "         113  /f13")
        self.assertNotIn("?", "\n".join(report), "seven kB before any mapping is not among the largest")
        report = monkey_oracles.ResourceOracle.memory_report("", "Rss: 7 kB\n")
        self.assertEqual(report, "largest resident mappings (kB):\n           7  ?\n")

    def test_a_total_whose_value_has_a_colon_and_a_mapping_with_every_permission(self):
        report = monkey_oracles.ResourceOracle.memory_report(
            "Swap:  0 kB [a:b]\n", "50-60 rwxp 0 0:0 1 /jit\nRss: 9 kB\n")
        self.assertEqual(report, "Swap:  0 kB [a:b]\nlargest resident mappings (kB):\n           9  /jit\n")

    def test_a_line_that_only_looks_like_a_mapping_is_not_one(self):
        smaps = ("10-20 r--p 0 0:0 1 /real\nRss: 1 kB\n"
                 "Some-Field: 2 kB x y z\nRss: 2 kB\n"
                 "30-40 r--p 0\nRss: 4 kB\n")
        report = monkey_oracles.ResourceOracle.memory_report("", smaps)
        self.assertEqual(report, "largest resident mappings (kB):\n           7  /real\n")

    def test_this_process_has_both_parts_and_a_gone_one_has_neither(self):
        report = monkey_oracles.ResourceOracle._memory(os.getpid())
        self.assertTrue(report.startswith("Rss:"), report[:40])
        self.assertIn("python", report)
        self.assertEqual(monkey_oracles.ResourceOracle._memory(2 ** 22 + 12345), "largest resident mappings (kB):\n")


class InvariantEvidenceTests(InvariantOracleTests):
    VIOLATION = {"violations": [{"rule": "r", "detail": "d"}]}

    def test_checks_are_counted_and_the_state_queries_asked_as_they_should_be(self):
        app, asked = self.app({"invariants": {"violations": []}})
        oracle = monkey_oracles.InvariantOracle(app)
        oracle.check()
        oracle.check(deep=True)
        self.assertEqual(oracle.checks, 2)
        self.assertEqual(asked, [("invariants", {"deep": False}), ("invariants", {"deep": True})])
        app, asked = self.app({"invariants": self.VIOLATION, "tabs": [1], "documents": [2], "editor": {"e": 3}})
        oracle = monkey_oracles.InvariantOracle(app)
        oracle.check()
        self.assertEqual(asked[1:], [("tabs", None), ("documents", {"all": True}), ("editor", None)])
        self.assertEqual(oracle.evidence["invariants.json"], json.dumps(self.VIOLATION, indent=1).encode())
        self.assertEqual(oracle.evidence["tabs.json"], b"[\n 1\n]")
        self.assertEqual(oracle.evidence["documents.json"], b"[\n 2\n]")
        self.assertEqual(oracle.evidence["editor.json"], b'{\n "e": 3\n}')

    def test_a_state_query_that_fails_leaves_the_others(self):
        app, _ = self.app({"invariants": self.VIOLATION, "tabs": RuntimeError("busy"), "documents": [2],
                           "editor": {}})
        oracle = monkey_oracles.InvariantOracle(app)
        self.assertEqual(len(oracle.check()), 1)
        self.assertNotIn("tabs.json", oracle.evidence)
        self.assertIn("documents.json", oracle.evidence)

    def test_the_caret_reports_are_read_from_the_whole_log_with_the_binary(self):
        app, _ = self.app({"invariants": self.VIOLATION, "tabs": [], "documents": [], "editor": {}})
        app.log_since.return_value = "the log"
        app.binary = Path("/bin/microide")
        oracle = monkey_oracles.InvariantOracle(app)
        with mock.patch.object(monkey_oracles, "caret_bounds_reports", return_value="caret report\n") as reports:
            oracle.check()
        app.log_since.assert_called_once_with(0, 8 * 1024 * 1024)
        reports.assert_called_once_with("the log", Path("/bin/microide"))
        self.assertEqual(oracle.evidence["caret-bounds.txt"], b"caret report\n")
        app.log_since.side_effect = RuntimeError("gone")
        oracle.reported.clear()
        oracle.check()
        self.assertNotIn("caret-bounds.txt", oracle.evidence)
        app.log_since.side_effect = None
        del app.binary
        oracle.reported.clear()
        with mock.patch.object(monkey_oracles, "caret_bounds_reports", return_value="") as reports:
            oracle.check()
        self.assertIsNone(reports.call_args.args[1])
        self.assertNotIn("caret-bounds.txt", oracle.evidence)


class KillAtDetailTests(CrashPointTests):
    def kill(self, oracle, point, slow=0.001):
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", slow), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            return oracle._kill_at(point)

    def test_the_save_needs_a_dirty_editor_with_a_path(self):
        for buffer in ({"kind": "compare", "path": "/x/a.txt", "dirty": True},
                       {"kind": "editor", "path": "", "dirty": True},
                       {"kind": "editor", "path": "/x/a.txt", "dirty": False}):
            oracle, app, sent, _ = self.dying_app(trigger_kills=False, buffer=buffer)
            _, _, saved = self.kill(oracle, "kill:durable.renaming@a.txt#1|save")
            self.assertIsNone(saved, buffer)
            self.assertNotIn("save", sent, buffer)

    def test_the_file_a_save_will_write_is_read_before_it(self):
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        (directory / "a.txt").write_bytes(b"on disk\n")
        oracle, _, _, _ = self.dying_app(buffer={"kind": "editor", "path": str(directory / "a.txt"), "dirty": True})
        self.kill(oracle, "kill:durable.renaming@a.txt#1|save")
        self.assertEqual(oracle.saved_file_before, hashlib.sha256(b"on disk\n").hexdigest())
        oracle_two, _, _, _ = self.dying_app()
        oracle_two.saved_file_before = "from an earlier kill"
        self.kill(oracle_two, "kill:durable.writing@session#1|edit")
        self.assertIsNone(oracle_two.saved_file_before, "each kill starts with no saved file")

    def test_an_edit_trigger_opens_nothing_and_open_triggers_count_up(self):
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        oracle, app, sent, _ = self.dying_app(trigger_kills=False)
        app.project = directory
        self.kill(oracle, "kill:durable.writing@session#1|edit")
        self.assertEqual(sent, ["insert-line-below"])
        self.kill(oracle, "kill:durable.renaming@recents#1|open")
        self.kill(oracle, "kill:durable.renaming@recents#1|open")
        self.assertEqual((directory / "qa-point-2.txt").read_text(), "2\n")
        self.assertTrue(sent[-1].endswith("qa-point-2.txt") or "qa-point-2.txt" in sent[-1], sent)
        oracle, app, sent, _ = self.dying_app(trigger_kills=False)
        app.project = None
        self.kill(oracle, "kill:durable.renaming@recents#1|open")
        self.assertEqual(sent, ["insert-line-below"], "with no project there is nowhere to make the file")

    def test_an_open_trigger_in_a_project_it_cannot_write_still_opens(self):
        oracle, app, sent, _ = self.dying_app(trigger_kills=False)
        app.project = Path("/proc")
        self.kill(oracle, "kill:durable.renaming@recents#1|open")
        self.assertTrue(sent[1].startswith("open "), sent)

    def test_a_trigger_that_fails_mid_request_is_the_death_asked_for(self):
        oracle, app, _, _ = self.dying_app()
        app.send_line.side_effect = monkey_oracles.q.QaFailure("the app died under the request")
        app.alive.side_effect = lambda: False
        self.assertEqual(self.kill(oracle, "kill:durable.writing@session#1|edit"), (True, [], None))

    def test_the_wait_polls_until_staged_and_a_dying_app_is_waited_for(self):
        oracle, app, _, _ = self.dying_app(trigger_kills=False, staged_after=3)
        died, _, _ = self.kill(oracle, "kill:durable.writing@session#1|edit", slow=1.0)
        self.assertFalse(died)
        self.assertEqual(self.polls["polls"], 3)
        oracle, app, _, _ = self.dying_app(trigger_kills=False)
        app.dying.return_value = True
        self.kill(oracle, "kill:durable.writing@session#1|edit", slow=1.0)
        self.assertEqual(self.polls["polls"], 0, "a dying app is not polled")
        app.wait_gone.assert_called_once_with(60.0)

    def test_the_rule_ends_at_the_first_bar(self):
        oracle, app, sent, armed = self.dying_app(trigger_kills=False)
        self.kill(oracle, "kill:durable.writing@a|b#1|edit")
        self.assertEqual(armed[0], {"arm": "kill:durable.writing@a"})


class KillAtOutcomeTests(KillAtDetailTests):
    def test_the_site_a_rule_names(self):
        site = monkey_oracles._rule_site
        self.assertEqual(site("kill:durable.writing@session (staging)#1"), "durable.writing")
        self.assertEqual(site("kill:durable.renaming#2"), "durable.renaming")
        self.assertEqual(site("delay=5000:durable.fsyncing@a:b.txt"), "durable.fsyncing")
        self.assertEqual(site("fail=ENOSPC:tree-op.moved@a@b"), "tree-op.moved")
        self.assertEqual(site("kill:s#1#2"), "s")
        self.assertEqual(site("durable.written@x"), "durable.written")

    def test_outcomes_count_hits_by_site_and_misses(self):
        oracle, app, _, _ = self.dying_app(trigger_kills=False)
        slept = []
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.0001), \
                mock.patch.object(monkey_oracles.time, "sleep", slept.append):
            oracle._kill_at("kill:durable.writing@session#1|edit")
            oracle._kill_at("kill:durable.writing@session#1|edit")
        self.assertEqual(dict(oracle.point_outcomes), {"missed": 2})
        self.assertTrue(slept and all(s == 0.05 for s in slept), slept)
        oracle, _, _, _ = self.dying_app()
        self.kill(oracle, "kill:durable.renaming@a.txt#1|project")
        self.kill(oracle, "kill:durable.renaming@a.txt#1|project")
        self.assertEqual(dict(oracle.point_outcomes), {"hit:durable.renaming": 2})

    def test_a_disarm_the_app_refuses_is_not_an_error(self):
        oracle, app, _, _ = self.dying_app(trigger_kills=False)
        query = app.query.side_effect

        def refusing(verb, args=None):
            if verb == "crash-points" and args == {"disarm": True}:
                raise monkey_oracles.q.QaFailure("gone")
            return query(verb, args)
        app.query.side_effect = refusing
        self.assertEqual(self.kill(oracle, "kill:x@y#1|edit")[0], False)


class LivenessDetailTests(LivenessTests):
    def test_the_evidence_in_full(self):
        app = self.app(seconds=3.5, sched=(3.0, 0.0), stall="#0 Frame()", waits="futex_wait 202", profile="")
        oracle = self.oracle(app)
        oracle.check()
        self.assertEqual(oracle.evidence["latency.txt"].decode(),
                         "query 'ui' took 3.50s (budget 2.0s)\n"
                         "main thread over 3.50s: 3.00s on a CPU, 0.00s waiting for one, 0.50s blocked\n"
                         "the app's stall watchdog during it (MICROIDE_TRACE_STALL_MS):\n"
                         "#0 Frame()\n"
                         "where the main thread sat while the answer was late (wchan, syscall nr and first arg; "
                         "sampled every 0.2s):\n"
                         "futex_wait 202\n")
        self.assertEqual(set(oracle.evidence), {"latency.txt"})
        oracle = self.oracle(self.app(seconds=3.5))
        oracle.check()
        self.assertEqual(oracle.evidence["latency.txt"].decode(),
                         "query 'ui' took 3.50s (budget 2.0s)\n"
                         "the app's stall watchdog printed nothing during it (no single loop turn ran that long, "
                         "or the binary predates the watchdog)\n")

    def test_an_app_without_the_newer_fields(self):
        from types import SimpleNamespace
        app = SimpleNamespace(slowest_answer=("query 'ui'", 3.5), slowest_sched=None, slowest_stall="")
        oracle = self.oracle(app)
        self.assertEqual(len(oracle.check()), 1)
        self.assertEqual(set(oracle.evidence), {"latency.txt"})
        self.assertNotIn("where the main thread sat", oracle.evidence["latency.txt"].decode())

    def test_a_quiet_check_clears_the_evidence_and_starved_ones_add_up(self):
        app = self.app(seconds=3.5)
        oracle = self.oracle(app)
        oracle.check()
        app.slowest_answer = ("query 'ui'", 1.0)
        oracle.check()
        self.assertEqual(oracle.evidence, {})
        for _ in range(2):
            app.slowest_answer, app.slowest_sched = ("query 'tabs'", 4.0), (0.5, 3.0)
            oracle.check()
        app.slowest_answer, app.slowest_sched, app.slowest_helpers = ("query 'x'", 4.0), (0.5, 0.5), (3.0, 4.0)
        oracle.check()
        self.assertEqual(oracle.starved, 3)

    def test_the_blocked_time_and_its_edges(self):
        def verdict(seconds, ran, queued, helpers=(3.0, 4.0)):
            oracle = self.oracle(self.app(seconds=seconds, sched=(ran, queued), helpers=helpers))
            found = oracle.check()
            return found[0][1].rsplit("mostly ", 1)[1] if found else "starved"
        self.assertEqual(verdict(3.0, 1.4, 1.0), "computing", "0.6 s blocked is less than 1.4 s run")
        self.assertEqual(verdict(3.0, 0.9, 1.4), "computing", "0.7 s blocked is less than 0.9 s run")
        self.assertEqual(verdict(3.0, 1.0, 1.0), "computing", "as long blocked as run is not blocked on helpers")
        self.assertEqual(verdict(3.0, 1.0, 1.0, helpers=None), "computing", "as long as it ran counts as computing")
        self.assertEqual(verdict(3.0, 0.9, 1.0, helpers=None), "blocked (I/O, a lock, a child)")


class RepaintedRegionTests(unittest.TestCase):
    def regions(self, frames, elements=None, ui_error=False):
        app = mock.Mock()
        if ui_error:
            app.ui.side_effect = RuntimeError("busy")
        else:
            app.ui.return_value = {} if elements is None else {"elements": elements}
        return monkey_oracles.IdleOracle(app)._repainted_regions(frames)

    @staticmethod
    def frame(bounds, event=None):
        return f"microide redraw frame: frame=1 reason=x bounds={bounds}" + (f" event={event}" if event else "")

    def test_the_first_field_of_its_name_is_read(self):
        self.assertEqual(monkey_oracles._trace_field("a bounds=1,2,3,4 x bounds=9", "bounds="), "1,2,3,4")
        self.assertEqual(monkey_oracles._trace_field("a bounds=", "bounds="), "")

    def test_no_elements_or_no_ui_leaves_the_regions_unnamed(self):
        self.assertEqual(self.regions([self.frame("0,0,5,5")]),
                         ["repainted bounds (x,y,w,h), most common first:", "    1  0,0,5,5  under: -"])
        self.assertEqual(self.regions([self.frame("0,0,5,5")], ui_error=True)[1], "    1  0,0,5,5  under: -")

    def test_the_three_most_repainted_and_the_five_most_asking(self):
        frames = [self.frame(f"{i},0,1,1", event=f"e{i}") for i in range(7) for _ in range(7 - i)]
        lines = self.regions(frames, elements=[])
        self.assertEqual(lines[0], "frames by the event that asked for them: "
                                   "7 x e0, 6 x e1, 5 x e2, 4 x e3, 3 x e4")
        self.assertEqual(lines[2:], ["    7  0,0,1,1  under: -", "    6  1,0,1,1  under: -", "    5  2,0,1,1  under: -"])

    def test_bounds_that_do_not_parse_do_not_stop_the_rest(self):
        lines = self.regions([self.frame("a,b,c,d")] * 3 + [self.frame("1,2,3,4")], elements=[])
        self.assertEqual(lines, ["repainted bounds (x,y,w,h), most common first:", "    1  1,2,3,4  under: -"])

    def test_an_element_is_under_a_region_only_when_they_overlap(self):
        def under(rect):
            return self.regions([self.frame("10,10,10,10")], elements=[{"name": "e", "rect": rect}])[1]
        for rect in ([19, 19, 5, 5], [5, 5, 6, 6], [15, 0, 1, 11], [0, 15, 11, 1]):
            self.assertTrue(under(rect).endswith("under: e"), rect)
        for rect in ([20, 10, 5, 5], [10, 20, 5, 5], [5, 10, 5, 5], [10, 5, 5, 5], [0, 0, 100], None):
            self.assertTrue(under(rect).endswith("under: -"), rect)
        self.assertTrue(under([12, 30, 1, 1]).endswith("under: -"), "x overlaps, y does not")

    def test_at_most_twelve_names(self):
        elements = [{"name": f"n{i}", "rect": [0, 0, 5, 5]} for i in range(13)]
        line = self.regions([self.frame("0,0,5,5")], elements=elements)[1]
        self.assertEqual(line, "    1  0,0,5,5  under: " + ", ".join(f"n{i}" for i in range(12)))


class IdleEvidenceDetailTests(IdleOracleCheckTests):
    def test_the_evidence_in_full(self):
        frames = [f"microide redraw frame: frame={i} reason=scheduled-partial dirty=1 bounds=1,2,3,4" for i in range(2)]
        lines = frames + [f"line {i}" for i in range(42)] + ["bad \udcff".encode("utf-8", "surrogateescape")
                                                              .decode("utf-8", "replace")]
        oracle, found = self.run_window([0, 40], logged=lines, samples=("ep_poll epoll_wait",))
        evidence = oracle.evidence.splitlines()
        self.assertEqual(evidence[:4], ["voluntary wakes/s: 1000 (budget 250)",
                                        "frames in the window: 2 {'scheduled-partial': 2}",
                                        "repainted bounds (x,y,w,h), most common first:",
                                        "    2  1,2,3,4  under: -"])
        self.assertEqual(evidence[4], "log lines in the window: 45")
        self.assertEqual(evidence[5], "main thread wchan + syscall, sampled:")
        self.assertRegex(evidence[6], r"^\s+\d+  ep_poll epoll_wait$")
        self.assertEqual(evidence[7], "last log lines:")
        self.assertEqual(evidence[8], "  line 3")
        self.assertEqual(len(evidence), 8 + 40)
        self.assertTrue(oracle.evidence.endswith("\n") and not oracle.evidence.endswith("\n\n"))

    def test_a_log_that_is_not_utf8_is_read(self):
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        log = directory / "app.log"
        log.write_text("")
        app = mock.Mock()
        app.log_path = log
        app.ui.return_value = {"elements": []}
        oracle = monkey_oracles.IdleOracle(app)
        counts = iter([0, 40])

        def sample():
            if log.stat().st_size == 0:
                log.write_bytes(b"\xff\xfe broken\n")
            return "x y"
        with mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.04), \
                mock.patch.object(oracle, "_main_thread_wakes", side_effect=lambda: next(counts)), \
                mock.patch.object(oracle, "_sample_main_thread", side_effect=sample), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            oracle.check()
        self.assertIn("\ufffd\ufffd broken", oracle.evidence)

    def test_an_app_without_a_log_still_reports(self):
        for app in (mock.Mock(spec=["settle_quiet", "ui"]), mock.Mock(log_path=Path("/nonexistent/app.log"))):
            app.ui.return_value = {"elements": []}
            oracle = monkey_oracles.IdleOracle(app)
            counts = iter([0, 40])
            with mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 0.04), \
                    mock.patch.object(oracle, "_main_thread_wakes", side_effect=lambda: next(counts)), \
                    mock.patch.object(oracle, "_sample_main_thread", return_value="x y"):
                self.assertEqual(len(oracle.check()), 1)
            self.assertIn("log lines in the window: 0", oracle.evidence)

    def test_the_window_is_sampled_forty_times_and_a_slow_build_is_allowed_more(self):
        oracle = monkey_oracles.IdleOracle(mock.Mock(spec=["settle_quiet", "ui"]))
        slept = []
        counts = iter([0, 600])
        with mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 2.0), \
                mock.patch.object(oracle, "_main_thread_wakes", side_effect=lambda: next(counts)), \
                mock.patch.object(oracle, "_sample_main_thread", return_value="x y"), \
                mock.patch.object(monkey_oracles, "_past", side_effect=[False] * 3 + [True]), \
                mock.patch.object(monkey_oracles.time, "sleep", slept.append):
            found = oracle.check()
        self.assertEqual(slept, [2.0 / 40] * 3)
        self.assertEqual(found, [], "300 wakes a second is within 250 x 2")


class ResourceCheckDetailTests(unittest.TestCase):
    BASE = {"fds": 50, "threads": 20, "children": 2, "rss_kb": 100_000}

    def oracle(self, baseline, sample, slow=1.0, reported=()):
        oracle = monkey_oracles.ResourceOracle(mock.Mock())
        oracle.baseline = baseline
        oracle.pid = 7
        oracle.reported = set(reported)
        holds = mock.Mock(return_value={"children.txt": b"x"})
        with mock.patch.object(oracle, "sample", return_value=sample), \
                mock.patch.object(oracle, "_what_it_holds", holds), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", slow):
            found = oracle.check()
        return oracle, found, holds

    def test_no_baseline_yet_is_nothing(self):
        _, found, _ = self.oracle(None, dict(self.BASE))
        self.assertEqual(found, [])

    def test_samples_without_a_pane_count_count_none(self):
        _, found, _ = self.oracle(dict(self.BASE), {**self.BASE, "fds": 50 + 257})
        self.assertEqual(found, [("leak", "the app's fds grew by 257 over the run (50 -> 307; budget 256)")])
        _, found, _ = self.oracle(dict(self.BASE), {**self.BASE, "fds": 50 + 257, "panes": 1})
        self.assertEqual(found, [], "one pane more than a baseline without a count allows its four")

    def test_the_memory_budget_grows_with_half_the_slow_factor_and_never_shrinks(self):
        _, found, _ = self.oracle(dict(self.BASE), {**self.BASE, "rss_kb": 100_000 + 1_600_000}, slow=1.0)
        self.assertEqual(len(found), 1)
        _, found, _ = self.oracle(dict(self.BASE), {**self.BASE, "rss_kb": 100_000 + 2_300_000}, slow=3.0)
        self.assertEqual(found, [("leak", "the app's rss_kb grew by 2300000 over the run (100000 -> 2400000; "
                                          "budget 2250000)")])

    def test_a_leak_is_reported_once_and_its_evidence_gathered_once(self):
        oracle = monkey_oracles.ResourceOracle(mock.Mock())
        oracle.baseline, oracle.pid = dict(self.BASE), 7
        holds = mock.Mock(return_value={"children.txt": b"x"})
        grown = {**self.BASE, "fds": 400, "threads": 100}
        with mock.patch.object(oracle, "sample", return_value=grown), \
                mock.patch.object(oracle, "_what_it_holds", holds), \
                mock.patch.object(monkey_oracles.q, "SLOW_FACTOR", 1.0):
            self.assertEqual(len(oracle.check()), 2)
            self.assertEqual(oracle.check(), [])
        holds.assert_called_once_with(7)
        self.assertEqual(oracle.evidence, {"children.txt": b"x"})
        self.assertEqual(oracle.reported, {"fds", "threads"})
        _, _, holds = self.oracle(dict(self.BASE), grown)
        holds.assert_called_once_with(7)
        oracle = monkey_oracles.ResourceOracle(mock.Mock())
        oracle.baseline, oracle.pid = dict(self.BASE), None
        with mock.patch.object(oracle, "sample", return_value=grown), \
                mock.patch.object(oracle, "_what_it_holds") as unasked:
            oracle.check()
        unasked.assert_not_called()

if __name__ == "__main__":
    unittest.main()
