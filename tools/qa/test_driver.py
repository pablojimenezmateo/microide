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


if __name__ == "__main__":
    unittest.main()
