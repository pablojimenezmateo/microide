"""Jobs run the driver from a snapshot of the worktree (snapshot_driver), so
everything the driver reads from the repository has to be in it. The TSan
suppressions were not: every TSan job died at startup, silently, for four days."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import microide_qa as q  # noqa: E402
import soak  # noqa: E402


def covered(path: Path) -> bool:
    relative = path.relative_to(q.REPO).as_posix()
    return any(relative == entry or relative.startswith(entry + "/") for entry in soak.DRIVER_PATHS)


class SnapshotTests(unittest.TestCase):
    def test_every_file_the_sanitizer_options_name_is_in_the_snapshot(self):
        named = [Path(option.split("=", 1)[1]) for value in q.SANITIZER_ENV.values()
                 for option in value.split(":") if option.startswith("suppressions=")]
        self.assertTrue(named, "the driver no longer names a suppressions file: update this test")
        for path in named:
            self.assertTrue(path.is_file(), f"{path} does not exist")
            self.assertTrue(covered(path), f"{path} is not under soak.DRIVER_PATHS")

    def test_the_driver_and_what_it_loads_are_in_the_snapshot(self):
        for path in (q.CAPTURE, q.REPO / "plugins", Path(q.__file__), HERE / "minimize.py"):
            self.assertTrue(covered(path), f"{path} is not under soak.DRIVER_PATHS")

    def test_a_snapshot_holds_files_and_an_older_one_is_filled_in(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as scratch:
            worktree, drivers = Path(scratch) / "wt", Path(scratch) / "drivers"
            (worktree / "tools/qa").mkdir(parents=True)
            (worktree / "tools/qa/run.py").write_text("# driver\n")
            (worktree / "tests").mkdir()
            (worktree / "tests/tsan.supp").write_text("race:x\n")
            with mock.patch.object(soak, "WORKTREE", worktree), mock.patch.object(soak, "DRIVERS", drivers):
                target = soak.snapshot_driver("a" * 40)
                self.assertEqual((target / "tests/tsan.supp").read_text(), "race:x\n")
                self.assertTrue((target / "tools/qa/run.py").is_file())
                # What a runner with the shorter list left behind for this commit.
                (target / "tests/tsan.supp").unlink()
                self.assertEqual(soak.snapshot_driver("a" * 40), target)
                self.assertTrue((target / "tests/tsan.supp").is_file())


if __name__ == "__main__":
    unittest.main()
