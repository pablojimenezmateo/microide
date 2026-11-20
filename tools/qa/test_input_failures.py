"""An input the app never answered is not an input it refused."""
import unittest

import microide_qa as q


def _app(send_line):
    app = q.App.__new__(q.App)
    app.expected = []
    app.expect = lambda condition, message: app.expected.append(message) or condition
    app.send_line = send_line
    app._record = lambda *a, **k: None
    return app


class InputFailureTests(unittest.TestCase):
    def test_a_refusal_is_filed_as_one(self):
        app = _app(lambda line, timeout=15.0, record=True: {"ok": False, "error": "no such verb"})
        app._pointer("drop", 10.0, 20.0, "/tmp/x")
        self.assertEqual(len(app.expected), 1)
        self.assertIn("was refused", app.expected[0])

    def test_no_reply_is_not_a_refusal(self):
        # Soak 8a6169d5fde8: every sighting of "pointer drop ... was refused" was an
        # app that did not answer at all -- a stalled main thread, a lost wake, a
        # dead process -- filed as a drop it refused, under the expect oracle.
        def silent(line, timeout=15.0, record=True):
            raise q.QaFailure(f"{line!r} got no reply: timed out")
        app = _app(silent)
        with self.assertRaises(q.QaFailure):
            app._pointer("drop", 10.0, 20.0, "/tmp/x")
        self.assertEqual(app.expected, [])

    def test_a_dead_app_is_not_a_refusal(self):
        def gone(line, timeout=15.0, record=True):
            raise q.AppGone("the app is gone: no control socket to connect to")
        app = _app(gone)
        with self.assertRaises(q.AppGone):
            app._pointer("drop", 10.0, 20.0, "/tmp/x")
        self.assertEqual(app.expected, [])


if __name__ == "__main__":
    unittest.main()
