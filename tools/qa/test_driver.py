"""Driver regression tests; no window, subprocess or real sleeping required."""
import unittest
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


if __name__ == "__main__":
    unittest.main()
