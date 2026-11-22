"""A minimize stops on its budget or its stop file and keeps what it found."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import minimize  # noqa: E402


class FakeTester(minimize.Tester):
    """Reproduces when action 7 is kept; creates `stop_after`'s file after that
    many replays, as a pause taken mid-minimize does."""

    def __init__(self, budget, stop_file=None, stop_after=None):
        super().__init__({"actions": []}, "sig", Path("unused"), Path(tempfile.gettempdir()), 1, budget)
        self.stop_file = stop_file
        self.stop_after = stop_after
        self.replays = 0

    def _run(self, actions):
        self.replays += 1
        if self.stop_after is not None and self.replays >= self.stop_after:
            self.stop_file.write_text("lanes")
        return any(a["i"] == 7 for a in actions)


ACTIONS = [{"i": i, "key": "a"} for i in range(64)]


class MinimizeBudgetTests(unittest.TestCase):
    def test_a_minimize_with_no_stop_reaches_the_one_action(self):
        budget = minimize.Budget(3600)
        kept = minimize.ddmin(FakeTester(budget), ACTIONS, 500, budget)
        self.assertEqual(kept, [7])
        self.assertIsNone(budget.reason)

    def test_a_stop_file_ends_it_with_the_smallest_list_that_reproduced(self):
        with tempfile.TemporaryDirectory() as scratch:
            stop = Path(scratch) / "PAUSE"
            budget = minimize.Budget(3600, stop)
            tester = FakeTester(budget, stop, stop_after=6)
            seen = []
            kept = minimize.ddmin(tester, ACTIONS, 500, budget, progress=lambda c: seen.append(list(c)))
            self.assertEqual(budget.reason, "stopped: PAUSE exists")
            self.assertLessEqual(tester.replays, 8, "it stops within the replays in flight")
            self.assertIn(7, kept, "what it keeps still reproduces")
            self.assertLess(len(kept), len(ACTIONS))
            self.assertEqual(seen[-1], kept, "the last saved list is the one it returns")

    def test_a_candidate_the_stop_skipped_is_unknown_and_not_cached(self):
        with tempfile.TemporaryDirectory() as scratch:
            stop = Path(scratch) / "PAUSE"
            stop.write_text("lanes")
            budget = minimize.Budget(3600, stop)
            tester = FakeTester(budget)
            self.assertEqual(tester.test_many([[7], [1]], lambda idx: [ACTIONS[i] for i in idx]), [None, None])
            self.assertEqual(tester.cache, {})
            self.assertEqual(tester.tests, 0)

    def test_the_budget_counts_from_the_start(self):
        budget = minimize.Budget(0)
        kept = minimize.ddmin(FakeTester(budget), ACTIONS, 500, budget)
        self.assertEqual(kept, list(range(64)), "a spent budget runs nothing")
        self.assertEqual(budget.reason, "budget spent")


class StoppedReplayTests(unittest.TestCase):
    """A stop ends a replay between actions: a slow-factor-5 replay of 897
    actions held a stopped minimize 23 minutes past its PAUSE (17a471bc0fcd)."""

    def test_a_replay_asked_to_stop_sends_nothing_more(self):
        class NoApp:
            replay_root = None

            def __getattr__(self, name):
                raise AssertionError(f"the replay touched the app ({name}) after its stop")

        with self.assertRaises(minimize.q.ReplayStopped):
            minimize.q.replay(NoApp(), [{"key": "a"}, {"key": "b"}], stop=lambda: True)

    def test_a_candidate_stopped_mid_replay_is_unknown_and_not_cached(self):
        def stopped_replay(*args, **kwargs):
            raise minimize.q.ReplayStopped("stopped before action 3 of 9")

        real = minimize.qa_run.replay_record
        minimize.qa_run.replay_record = stopped_replay
        try:
            budget = minimize.Budget(3600)
            tester = minimize.Tester({"actions": []}, "sig", Path("unused"), Path(tempfile.gettempdir()), 1, budget)
            self.assertIsNone(tester.run_detailed(ACTIONS)["hit"])
            self.assertEqual(tester.test_many([[7]], lambda idx: [ACTIONS[i] for i in idx]), [None])
            self.assertEqual(tester.cache, {})
        finally:
            minimize.qa_run.replay_record = real


if __name__ == "__main__":
    unittest.main()
