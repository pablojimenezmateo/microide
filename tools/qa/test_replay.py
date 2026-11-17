"""A replay re-runs the run's oracles where it ran them, and can keep its pace."""
import time
import unittest
from pathlib import Path

import microide_qa as q


class FakeApp:
    replay_root = None

    def __init__(self):
        self.sent: list[tuple[float, str]] = []

    def drop(self, x, y, path):
        self.sent.append((time.monotonic(), f"drop {path}"))

    def send_line(self, line):
        self.sent.append((time.monotonic(), line))

    def key(self, key):
        self.sent.append((time.monotonic(), key))

    def settle_quiet(self, most=0.0):
        pass

    def check(self, label=""):
        return []


class StatefulApp(FakeApp):
    """An app whose state is what the last thing done to it made it, read by
    check() as the real one's `ui` answer is."""

    def __init__(self):
        super().__init__()
        self.now = "start"
        self.seen = "start"   # what the last check read: app.last_ui's stand-in
        self.labels: list[str] = []

    def key(self, key):
        super().key(key)
        self.now = f"after {key}"

    def type(self, text):
        self.sent.append((time.monotonic(), f"type {text}"))

    def drag(self, *points, modifiers=(), clicks=1):
        self.sent.append((time.monotonic(), f"drag {points}"))

    def check(self, label=""):
        self.labels.append(label)
        self.seen = self.now
        return []


class PathCheckTests(unittest.TestCase):
    """q.replay's `follow`: the run stamps its state ("s") on the last row a
    step recorded, and the replay compares its own wherever that stamp sits."""

    def follow(self, actions, **callbacks):
        import monkey_model as mm
        app, divergence = StatefulApp(), mm.Divergence()
        q.replay(app, actions, follow=lambda index, action: divergence.see(index, action["s"], app.seen, action),
                 **callbacks)
        return app, divergence

    def test_a_stamp_on_a_kill_row_is_compared_and_carried_forward(self):
        # A kill -9 step: the relaunch changes the state, and the step's only row
        # is the kill's. Skipped with its row, the stamp left the two "same as
        # before" stamps after it compared against the state BEFORE the kill:
        # every replay of a run with a kill in it "left the run's path".
        def relaunch(app):
            def external(row):
                app.now = "relaunched"
            return external
        actions = [{"key": "a", "t": 1.0, "s": "after a"},
                   {"kill9": True, "waited": False, "t": 2.0, "s": "relaunched"},
                   {"line": "noop", "t": 3.0, "s": mm_same()},
                   {"line": "noop", "t": 4.0, "s": mm_same()}]
        import monkey_model as mm
        app, divergence = StatefulApp(), mm.Divergence()
        q.replay(app, actions, external=relaunch(app),
                 follow=lambda index, action: divergence.see(index, action["s"], app.seen, action))
        self.assertEqual((divergence.compared, divergence.differed), (4, 0), divergence.first)
        self.assertEqual(divergence.verdict(), "followed")

    def test_a_stamp_on_any_row_without_a_check_of_its_own_is_compared(self):
        # The last row of a step can be an oracle's, one of its commands, an
        # outside-world change, a churn or a character typed through the control
        # channel: each carries the stamp of the step it ended.
        for row in ({"oracle": "invariants"}, {"line": "undo", "o": 1}, {"external": "rewrite", "target": "a"},
                    {"churn": "x", "counter": 1}, {"type-via-control": "😀"}, {"note": "typed slowly"}):
            with self.subTest(row=row):
                actions = [{"key": "a", "t": 1.0}, {**row, "t": 2.0, "s": "after a"}]
                app, divergence = self.follow(actions, oracle=lambda row: None, external=lambda row: None,
                                              churn=lambda row: None)
                self.assertEqual((divergence.compared, divergence.differed), (1, 0))

    def test_a_replay_that_is_somewhere_else_still_says_so(self):
        # The negative control: the stamps are read, not waved through.
        actions = [{"key": "a", "t": 1.0, "s": "after a"},
                   {"kill9": True, "waited": False, "t": 2.0, "s": "relaunched"},
                   {"line": "noop", "t": 3.0, "s": mm_same()}]
        app, divergence = self.follow(actions, external=lambda row: None)  # the kill does not happen
        self.assertEqual(divergence.differed, 2)
        self.assertEqual(divergence.first["action"], 1)

    def test_a_row_without_a_stamp_costs_no_check(self):
        actions = [{"key": "a", "t": 1.0, "s": "after a"}, {"external": "rewrite", "target": "a", "t": 2.0}]
        app, _ = self.follow(actions, external=lambda row: None)
        self.assertEqual(app.labels, ["step 0"])


def mm_same() -> str:
    import monkey_model as mm
    return mm.SAME_STATE


class ReplayLabelTests(unittest.TestCase):
    """A replayed finding has the signature the run gave it, whatever action it
    follows: the signature is the message less the check's label."""

    MESSAGE = "keyboard focus is on the panel, which is hidden"
    STATE = "f=overlay o=project-search s=search p=terminal+max m=open d=modal g=3 r=1 h=diag t=compare b=compare:d:sel"

    def replayed_signature(self, action):
        import findings as fnd
        app = StatefulApp()
        q.replay(app, [action])
        return fnd.signature("focus", f"after {app.labels[-1]}: {self.MESSAGE}")

    def test_the_signature_survives_any_action(self):
        import findings as fnd
        run = fnd.signature("focus", f"after monkey step 32: {self.MESSAGE}")
        for action in ({"key": "ctrl+shift+f", "t": 1.0},
                       # Over 200 characters as JSON: the label was cut before its brace closed.
                       {"type": "x" * 300, "t": 1.0},
                       {"drag": [42, 737, 1, 737], "modifiers": ["ctrl", "shift"], "clicks": 1, "t": 2.5, "s": self.STATE},
                       # A brace in the text: the label's pattern stops at one.
                       {"type": "if (x) { return {1: 2}; }", "t": 1.0},
                       {"line": "set-setting editor.rulers {\"a\": 1}", "t": 1.0}):
            with self.subTest(action=str(action)[:40]):
                self.assertEqual(self.replayed_signature(action), run)

    def test_the_old_label_is_what_broke_it(self):
        # The negative control: the same finding under the label a replay used to
        # give its check had another signature.
        import findings as fnd, json
        run = fnd.signature("focus", f"after monkey step 32: {self.MESSAGE}")
        old_label = json.dumps({"type": "x" * 300, "t": 1.0})[:200]
        self.assertNotEqual(fnd.signature("focus", f"after {old_label}: {self.MESSAGE}"), run)


class ReplayTests(unittest.TestCase):
    def test_a_drop_lands_on_the_replay_projects_file(self):
        # The run's mirror (or fixture) is gone by the time anything replays it.
        recorded = q.App.__new__(q.App)
        recorded.replay_root = Path("/tmp/run-1/mirror")
        recorded.actions, recorded._pointer = [], lambda *args: None
        recorded._record = recorded.actions.append
        recorded.drop(10, 20, "/tmp/run-1/mirror/qa-weird/binary.bin")
        self.assertEqual(recorded.actions[0]["projectPath"], "qa-weird/binary.bin")
        app = FakeApp()
        app.replay_root = Path("/tmp/replay/mirror")
        q.replay(app, recorded.actions, check=False)
        self.assertEqual(app.sent[0][1], "drop /tmp/replay/mirror/qa-weird/binary.bin")

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
