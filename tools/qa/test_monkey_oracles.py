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

if __name__ == "__main__":
    unittest.main()
