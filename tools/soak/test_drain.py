"""When the runner stops starting jobs so a build can run: only when there is a
build or a verification to do, never on the clock alone."""
import sys
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import soak  # noqa: E402

HEAD = "a" * 40
# Every configuration tried at HEAD: what a build of it leaves in the state.
TRIED = {name: HEAD for name in soak.BUILDS}


class RefreshDueTests(unittest.TestCase):
    def due(self, state, head=HEAD, fixes=()):
        with mock.patch.object(soak, "git", return_value=head), \
                mock.patch.object(soak, "fixes_to_verify", return_value=iter(fixes)):
            return soak.refresh_due(state)

    def test_an_unmoved_main_with_nothing_to_verify_is_no_reason_to_drain(self):
        # The case that held the soak at one job: a check minimizing for hours
        # while the runner waited on a build that would have done nothing.
        self.assertFalse(self.due({"built_sha": HEAD, "good": {}, "tried": TRIED}))

    def test_a_moved_main_drains(self):
        self.assertTrue(self.due({"built_sha": "b" * 40, "good": {}, "tried": TRIED}))

    def test_a_fix_waiting_for_its_replay_drains(self):
        self.assertTrue(self.due({"built_sha": HEAD, "good": {}, "tried": TRIED}, fixes=[("entry", {"sha": HEAD})]))

    def test_a_missing_binary_drains(self):
        state = {"built_sha": HEAD, "good": {"default": {}}, "tried": TRIED}
        with mock.patch.object(soak.BUILDS["default"], "binary", return_value=Path("/nonexistent/microide")):
            self.assertTrue(self.due(state))

    def test_a_configuration_added_since_the_commit_was_built_is_built_now(self):
        # A runner restarted onto a new build type (the coverage build, the day it
        # landed) would otherwise wait for the next push to build it.
        older = {name: HEAD for name in soak.BUILDS if name != "coverage"}
        with mock.patch.object(soak.codecov, "unusable_reason", return_value=None):
            self.assertTrue(self.due({"built_sha": HEAD, "good": {}, "tried": older}))
        # ... unless this machine cannot build it at all: then there is nothing to wait for.
        with mock.patch.object(soak.codecov, "unusable_reason", return_value="clang++ is not installed"):
            self.assertFalse(self.due({"built_sha": HEAD, "good": {}, "tried": older}))

    def test_a_build_that_failed_is_not_tried_again_on_the_clock(self):
        # Tried and not good: a finding, booked once, not a rebuild every interval.
        self.assertFalse(self.due({"built_sha": HEAD, "good": {}, "tried": TRIED}))


if __name__ == "__main__":
    unittest.main()
