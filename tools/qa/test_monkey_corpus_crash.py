"""A crash under the corpus prefix that starts a monkey run is a crash finding."""
import unittest
from unittest import mock

import microide_qa as q
import monkey as mk


class CorpusPrefixCrashTests(unittest.TestCase):
    def _monkey(self, keep_going: bool) -> mk.Monkey:
        monkey = mk.Monkey.__new__(mk.Monkey)
        monkey.app = mock.Mock(findings=[])
        monkey.app.alive.return_value = False
        monkey.resources = mock.Mock()
        monkey.keep_going, monkey.relaunches, monkey.max_relaunches = keep_going, 0, 1
        monkey._start_from_corpus = mock.Mock(side_effect=q.AppGone("the app is gone: no control socket"))
        monkey._final_oracles = mock.Mock()
        monkey.step = mock.Mock(side_effect=AssertionError("no step after the run ended"))
        return monkey

    def test_a_crash_under_the_prefix_is_checked_and_ends_a_run_that_stops_at_one(self) -> None:
        # Soak 63db46df221a: AppGone out of the prefix's replay ended the driver
        # with a traceback, the job with no step taken and no result written.
        monkey = self._monkey(keep_going=False)
        monkey.run(5)
        monkey.app.check.assert_called_once_with("the corpus prefix")
        monkey._final_oracles.assert_called_once()
        monkey.step.assert_not_called()

    def test_a_run_that_keeps_going_relaunches_after_it(self) -> None:
        monkey = self._monkey(keep_going=True)
        monkey.app.crash_and_relaunch.side_effect = RuntimeError("no display")
        monkey.run(5)
        monkey.app.crash_and_relaunch.assert_called_once()
        self.assertEqual(monkey.relaunches, 1)
        monkey.step.assert_not_called()


if __name__ == "__main__":
    unittest.main()
