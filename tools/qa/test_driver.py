"""Driver regression tests; no window, subprocess or real sleeping required."""
import unittest
import subprocess
import contextlib
import io
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import microide_qa as q
from flows_common import expect_text


class PollingTimeoutTests(unittest.TestCase):
    def test_state_wait_uses_slow_factor(self):
        app = SimpleNamespace(expect=Mock())
        predicate = Mock(side_effect=(False, False, True))
        with patch.object(q, "SLOW_FACTOR", 3), \
             patch.object(q.time, "monotonic", side_effect=(0, 0, 1, 2)), \
             patch.object(q.time, "sleep"):
            self.assertTrue(q.App.wait_until(app, predicate, 1, "ready"))
        app.expect.assert_not_called()
        self.assertEqual(predicate.call_count, 3)

    def test_text_wait_uses_slow_factor(self):
        app = SimpleNamespace(text=Mock(side_effect=("old", "old", "ready")), expect=Mock())
        with patch.object(q, "SLOW_FACTOR", 3), \
             patch.object(q.time, "monotonic", side_effect=(0, 0, 1, 2)), \
             patch.object(q.time, "sleep"):
            self.assertTrue(expect_text(app, "ready", "loaded", 1))
        app.expect.assert_not_called()
        self.assertEqual(app.text.call_count, 3)

    def test_scaled_timeout_still_reports_failure(self):
        app = SimpleNamespace(expect=Mock())
        with patch.object(q, "SLOW_FACTOR", 3), \
             patch.object(q.time, "monotonic", side_effect=(0, 0, 3)), \
             patch.object(q.time, "sleep"):
            self.assertFalse(q.App.wait_until(app, lambda: False, 1, "ready"))
        app.expect.assert_called_once_with(False, "timed out waiting for ready")

    def test_startup_wait_uses_slow_factor(self):
        predicate = Mock(side_effect=(False, False, True))
        with patch.object(q, "SLOW_FACTOR", 3), \
             patch.object(q.time, "monotonic", side_effect=(0, 0, 1, 2)), \
             patch.object(q.time, "sleep"):
            q._wait(predicate, 1, "ready")
        self.assertEqual(predicate.call_count, 3)

    def test_text_timeout_reports_the_difference(self):
        app = SimpleNamespace(text=Mock(return_value="old"), expect=Mock())
        with patch.object(q, "SLOW_FACTOR", 3), \
             patch.object(q.time, "monotonic", side_effect=(0, 0, 3)), \
             patch.object(q.time, "sleep"):
            self.assertFalse(expect_text(app, "new", "loaded", 1))
        app.expect.assert_called_once()
        self.assertFalse(app.expect.call_args.args[0])
        self.assertIn("loaded: buffer differs", app.expect.call_args.args[1])

    def test_transient_control_failure_can_recover(self):
        app = SimpleNamespace(expect=Mock())
        predicate = Mock(side_effect=(q.QaFailure("not ready"), True))
        with patch.object(q, "SLOW_FACTOR", 1), \
             patch.object(q.time, "monotonic", side_effect=(0, 0, 0.5)), \
             patch.object(q.time, "sleep"):
            self.assertTrue(q.App.wait_until(app, predicate, 1, "ready"))
        app.expect.assert_not_called()


class StartupFailureTests(unittest.TestCase):
    def test_launch_preserves_logs_before_cleaning_work(self):
        work_dirs = []

        def fail_start(app, project, extra_args):
            work_dirs.append(app.work)
            (app.work / "microide.log").write_text("startup diagnostic")
            raise q.QaFailure("startup failed")

        with tempfile.TemporaryDirectory() as scratch:
            shots = Path(scratch) / "evidence"
            with patch.object(q.App, "_start", fail_start), \
                 patch.dict(q.os.environ, {"MQA_KEEP_WORK": "0"}):
                with self.assertRaisesRegex(q.QaFailure, "startup failed"):
                    with q.App.launch(binary=Path(__file__), shots=shots):
                        self.fail("a failed startup must never yield an app")
            self.assertEqual((shots / "startup/microide.log").read_text(), "startup diagnostic")
            self.assertFalse(work_dirs[0].exists())

    def test_startup_timeout_is_a_nonretryable_scenario_failure(self):
        import run as runner
        with tempfile.TemporaryDirectory() as scratch:
            args = SimpleNamespace(out=Path(scratch), binary=Path("microide"))
            error = subprocess.TimeoutExpired(["microide", "control-list"], 9)
            with patch.object(runner, "fixture", return_value=Path(scratch)), \
                 patch.object(q.App, "launch", side_effect=error):
                name, lines, count, kinds, coverage = runner.run_scenario("menus", args)
            self.assertEqual((name, count, kinds, coverage), ("menus", 1, {"driver"}, {}))
            self.assertIn("TimeoutExpired", "\n".join(lines))
            self.assertIn("control-list", (Path(scratch) / "menus/driver-error.txt").read_text())

    def test_suite_continues_after_driver_error_without_retrying_it(self):
        import run as runner

        def scenario(name, args, out=None):
            if name == "menus":
                return name, ["driver failed"], 1, {"driver"}, {}
            return name, ["chrome passed"], 0, set(), {"catalogue": ["test"], "commands": {"test": 1}}

        with tempfile.TemporaryDirectory() as scratch:
            args = SimpleNamespace(out=Path(scratch), names=[], jobs=2, retries=1, list=False)
            with patch.object(runner, "select_scenarios", return_value=["menus", "chrome"]), \
                 patch.object(runner, "run_scenario", side_effect=scenario) as run, \
                 patch.object(runner, "write_command_coverage", return_value="coverage") as ledger, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.cmd_suite(args), 1)
            self.assertEqual(run.call_count, 2)
            self.assertEqual(set(ledger.call_args.args[2]), {"menus", "chrome"})


class AppEnvironmentTests(unittest.TestCase):
    def test_the_desktop_input_method_is_not_inherited(self):
        # TD-2026-10-27-395: a desktop's XMODIFIERS=@im=ibus reached the app on
        # Xvfb and its IBus double-encoded typed Latin-1 text.
        env = q.app_base_env({"XMODIFIERS": "@im=ibus", "GTK_IM_MODULE": "ibus",
                              "QT_IM_MODULE": "ibus", "SDL_IM_MODULE": "fcitx",
                              "LANG": "en_US.UTF-8", "PATH": "/usr/bin"})
        self.assertEqual(env, {"LANG": "en_US.UTF-8", "PATH": "/usr/bin"})


class RegistrySourceTests(unittest.TestCase):
    def test_duplicate_flow_keys_are_detected_before_import_loses_them(self):
        import scenarios
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / "flows_example.py"
            source.write_text('FLOWS = {"one": first, "one": second}\n')
            self.assertEqual(scenarios._duplicate_literal_keys(source), [("one", 1)])

    def test_equal_keys_in_separate_dictionaries_are_valid(self):
        import scenarios
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / "flows_example.py"
            source.write_text('FLOWS = {"one": first}\nENV = {"one": {"KEY": "value"}}\n')
            self.assertEqual(scenarios._duplicate_literal_keys(source), [])


class SearchRowReaderTests(unittest.TestCase):
    def test_match_reader_excludes_headers_and_keeps_colons_in_paths(self):
        from flows_common import search_hit_names, search_rows
        rows = [{"name": name} for name in ("row:search/file.txt", "row:search/file.txt:2",
                                           "row:search/a:b.txt:17", "row:search/123")]
        app = SimpleNamespace(elements=Mock(return_value=rows))
        self.assertEqual(search_rows(app), rows)
        self.assertEqual(search_hit_names(app), ["row:search/file.txt:2", "row:search/a:b.txt:17"])
        app.elements.assert_called_with("row", prefix="row:search/")


class PointerInputBarrierTests(unittest.TestCase):
    """A control command waits for the pointer input sent before it, as it does
    for keys: a query right after a click was answered before the click was
    handled on the slow ASan build (journey-rename-refactor's Discard All)."""

    def setUp(self):
        backend = patch.object(q, "BACKEND", "x11")
        backend.start()
        self.addCleanup(backend.stop)

    def _app(self, log: Path) -> q.App:
        app = q.App.__new__(q.App)  # the X11 backend: q.BACKEND is pinned in setUp
        app.log_path = log
        app.size = (1440, 900)
        app.origin = (0, 0)
        app.actions = []
        app._typed_pending = ""
        app._keys_pending = 0
        app._buttons_pending = 0
        app._wheels_pending = 0
        app._motion_pending = None
        app._pointer_at = (5, 5)
        app._keys_sent = []
        app._typed_log_offset = 0
        return app

    def test_a_click_waits_for_its_press_and_release(self):
        with tempfile.TemporaryDirectory() as scratch:
            log = Path(scratch) / "microide.log"
            log.write_bytes(b"earlier output\n")
            app = self._app(log)
            app._expect_pointer_input(buttons=2, at=(100, 200))
            with open(log, "ab") as out:
                out.write(b"microide input: button=1 down=1 clicks=1 x=100 y=200 focus=editor->editor handled=1\n"
                          b"microide input: button=1 down=0 clicks=1 x=100 y=200 focus=editor->editor handled=1\n")
            app._await_typed_input()
            self.assertEqual(app.actions, [], "both events seen: no timeout note")
            self.assertFalse(app._input_pending())

    def test_a_release_not_yet_handled_is_reported(self):
        with tempfile.TemporaryDirectory() as scratch:
            log = Path(scratch) / "microide.log"
            log.write_bytes(b"")
            app = self._app(log)
            app._expect_pointer_input(buttons=2, at=(100, 200))
            log.write_bytes(b"microide input: button=1 down=1 clicks=1 x=100 y=200 focus=editor->editor handled=1\n")
            with patch.object(q, "SLOW_FACTOR", 1), \
                 patch.object(q.time, "monotonic", side_effect=(0, 0, 5, 5)), \
                 patch.object(q.time, "sleep"), \
                 contextlib.redirect_stdout(io.StringIO()):
                app._await_typed_input()
            self.assertIn("1 of 2 button events", app.actions[-1]["note"])

    def test_a_move_waits_for_its_motion_unless_the_pointer_is_there(self):
        with tempfile.TemporaryDirectory() as scratch:
            log = Path(scratch) / "microide.log"
            log.write_bytes(b"")
            app = self._app(log)
            app._expect_pointer_input(motion=(5, 5))
            self.assertFalse(app._input_pending(), "X sends no motion for a move to where the pointer is")
            app._expect_pointer_input(motion=(40.4, 60.6))
            self.assertEqual(app._motion_pending, (40, 61))
            log.write_bytes(b"microide input: motion x=12 y=13\nmicroide input: motion x=40 y=60\n")
            app._await_typed_input()
            self.assertEqual(app.actions, [], "a motion within a pixel of the target counts")
            self.assertEqual(app._pointer_at, (40, 61))


    def test_a_press_the_window_manager_takes_expects_nothing(self):
        """A press on a resize edge or the title bar's drag area, or past the
        window's current size, never reaches the app; waiting for it cost a 3 s
        timeout per click (9 s under ASan) in the monkey."""
        with tempfile.TemporaryDirectory() as scratch:
            app = self._app(Path(scratch) / "microide.log")
            app._last_click = (0.0, -100.0, -100.0)
            regions = {(0, 0): "resize", (400, 10): "draggable", (2000, 300): "outside",
                       (400, 300): "normal"}

            def hit_test(payload, timeout, what):
                x, y = payload["args"]["x"], payload["args"]["y"]
                return {"ok": True, "result": {"region": regions[(x, y)]}}
            with patch.object(q.App, "_socket_request", side_effect=hit_test), \
                 patch.object(q.App, "_xdotool"):
                for point in ((0, 0), (400, 10), (2000, 300)):
                    app.click(*point)
                    self.assertEqual(app._buttons_pending, 0, f"no button events expected at {point}")
                app.click(400, 300)
                self.assertEqual(app._buttons_pending, 2, "a press inside reaches the app")


class GitSettleTests(unittest.TestCase):
    """The git view's rows move when a refresh lands, so a git element is read
    once it has settled; a refresh is asked for only after an outside change
    (`git-refresh` also reloads clean buffers, standing in for the watcher)."""

    def _app(self, settled_after: int) -> q.App:
        app = q.App.__new__(q.App)
        app.outside_change_pending = False
        app.send = Mock(return_value={"ok": True})
        app.expect = Mock()
        answers = iter([False] * settled_after + [True] * 10)
        app.ui = Mock(side_effect=lambda: {"git": {"settled": next(answers)}})
        return app

    def test_settling_asks_for_a_refresh_only_after_an_outside_change(self):
        with patch.object(q.time, "sleep"):
            app = self._app(settled_after=2)
            self.assertTrue(app.wait_git_settled())
            app.send.assert_not_called()
            self.assertEqual(app.ui.call_count, 3, "it waits through the unsettled answers")

            app.outside_change_pending = True
            self.assertTrue(app.wait_git_settled())
            app.send.assert_called_once_with("git-refresh")
            self.assertFalse(app.outside_change_pending)

if __name__ == "__main__":
    unittest.main()
