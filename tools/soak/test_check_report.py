"""What a set of check replays says of a finding beyond how many showed it."""
import doctest
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ledger as ld  # noqa: E402
import minimize  # noqa: E402
import soak  # noqa: E402


def checked(**fields) -> ld.Entry:
    entry = ld.Entry(sig="s", rate=0.0, rate_runs=12, deterministic=False)
    for name, value in fields.items():
        setattr(entry, name, value)
    return entry


class CheckReportTests(unittest.TestCase):
    def test_the_minimizers_doctests(self):
        self.assertEqual(doctest.testmod(minimize).failed, 0)

    def test_the_verdict_on_a_finding_no_replay_showed(self):
        self.assertEqual(checked().replay_verdict, "unknown")  # a check from before replays compared paths
        self.assertEqual(checked(replays_ran=0).replay_verdict, "untested")
        self.assertEqual(checked(replays_ran=12, replays_diverged=9, replays_followed=3).replay_verdict, "diverged")
        self.assertEqual(checked(replays_ran=12, replays_followed=11).replay_verdict, "followed")
        self.assertEqual(checked(rate=0.25, replays_ran=12).replay_verdict, "")
        self.assertEqual(ld.Entry(sig="s").replay_verdict, "")

    def test_the_ledger_folds_a_checks_report(self):
        import tempfile
        with tempfile.TemporaryDirectory() as scratch:
            book = ld.Ledger(Path(scratch) / "ledger.jsonl")
            book.append("abc", "new", kind="data", label="data: x")
            book.append("abc", "checked", rate=0.0, runs=12, deterministic=False, ran=12, followed=2, diverged=10,
                        divergence={"action": 41, "run": "t=compare", "replay": "t=editor", "after": "{}"},
                        errors=[], others=["def log: y"])
            entry = book.entries()["abc"]
            self.assertEqual((entry.replays_ran, entry.replays_diverged, entry.divergence["action"]), (12, 10, 41))
            self.assertEqual(entry.replay_others, ["def log: y"])
            self.assertEqual(entry.replay_verdict, "diverged")

    def test_finding_md_says_which_kind_of_zero_it_is(self):
        diverged = soak.reproduction_text(checked(
            replays_ran=12, replays_diverged=10, replays_followed=2,
            divergence={"action": 41, "run": "t=compare", "replay": "t=editor", "after": '{"line": "compare-files"}'}))
        self.assertIn("LEFT THE RUN'S PATH, first after action 41", diverged)
        self.assertIn("a gap in the harness", diverged)
        followed = soak.reproduction_text(checked(replays_ran=12, replays_followed=12))
        self.assertIn("followed the run's states to the end", followed)
        untested = soak.reproduction_text(checked(replays_ran=0, replay_errors=["QaFailure: no window"]))
        self.assertIn("NOT TESTED", untested)
        self.assertIn("QaFailure: no window", untested)
        partly = soak.reproduction_text(checked(rate=0.5, replays_ran=4, replay_errors=["x"]))
        self.assertIn("2 of 4 replays", partly)
        self.assertIn("8 more could not run", partly)


if __name__ == "__main__":
    unittest.main()
