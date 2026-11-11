"""A replay re-runs the run's oracles where it ran them, and can keep its pace."""
import time
import unittest

import microide_qa as q


class FakeApp:
    def __init__(self):
        self.sent: list[tuple[float, str]] = []

    def send_line(self, line):
        self.sent.append((time.monotonic(), line))

    def key(self, key):
        self.sent.append((time.monotonic(), key))

    def settle_quiet(self, most=0.0):
        pass

    def check(self, label=""):
        return []


class ReplayTests(unittest.TestCase):
    def test_an_oracle_row_reruns_the_oracle_and_skips_its_commands(self):
        actions = [
            {"line": "open a.txt", "t": 1.0},
            {"oracle": "undo", "asked": 2, "t": 2.0},
            {"line": "undo", "o": 1, "t": 2.1},
            {"line": "redo", "o": 1, "t": 2.2},
            {"key": "a", "t": 3.0},
        ]
        app, ran = FakeApp(), []
        q.replay(app, actions, oracle=ran.append, check=False)
        self.assertEqual([line for _, line in app.sent], ["open a.txt", "a"])
        self.assertEqual(ran, [{"oracle": "undo", "asked": 2, "t": 2.0}])
        # Without an oracle callback the commands replay as they were sent.
        plain = FakeApp()
        q.replay(plain, actions, check=False)
        self.assertEqual([line for _, line in plain.sent], ["open a.txt", "undo", "redo", "a"])

    def test_a_paced_replay_keeps_the_recorded_gaps(self):
        actions = [{"key": "a", "t": 10.0}, {"key": "b", "t": 10.3}, {"key": "c", "t": 99.0}]
        app = FakeApp()
        started = time.monotonic()
        q.replay(app, actions, check=False, paced=True)
        times = [t for t, _ in app.sent]
        self.assertGreaterEqual(times[1] - times[0], 0.29)
        # A gap past MAX_PACED_GAP (a relaunch, a stall) is cut to it.
        self.assertLess(time.monotonic() - started, q.MAX_PACED_GAP + 1.0)
        fast = FakeApp()
        q.replay(fast, actions[:2], check=False)
        self.assertLess(fast.sent[1][0] - fast.sent[0][0], 0.2)


if __name__ == "__main__":
    unittest.main()
