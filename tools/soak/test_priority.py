"""What the soak runs and what it asks to have fixed first: tiny windows rarely,
and stale paint after everything a user could lose work to."""
import random
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import ledger as ld  # noqa: E402
import soak  # noqa: E402


def entry(sig: str, kind: str, label: str = "", cells=("default/default/1440x900@1",), count: int = 1,
          deterministic=None) -> ld.Entry:
    return ld.Entry(sig=sig, kind=kind, label=label, cells=set(cells), count=count, deterministic=deterministic)


class CellTests(unittest.TestCase):
    def test_tiny_is_the_logical_window(self):
        self.assertTrue(soak.is_tiny((640, 480), "1"))
        self.assertTrue(soak.is_tiny((1024, 700), "2"))
        self.assertFalse(soak.is_tiny((1920, 1080), "2"))
        self.assertFalse(soak.is_tiny((1440, 900), "1.75"))
        self.assertTrue(soak.cell_is_tiny("editing/default/640x480@1.25"))
        self.assertFalse(soak.cell_is_tiny("check/abc"))

    def test_tiny_cells_still_run_but_rarely(self):
        rng = random.Random(7)
        drawn = [soak.is_tiny(*soak.choose_cell(rng)) for _ in range(20000)]
        share = sum(drawn) / len(drawn)
        self.assertGreater(share, 0.0)
        self.assertLess(share, 0.06)


class PriorityTests(unittest.TestCase):
    def test_severity_leads(self):
        stale = entry("a", "log", "log: QA-STALE frame=<n>", count=900, deterministic=True)
        asan = entry("b", "log", "log: ERROR: AddressSanitizer: heap-use-after-free")
        lost = entry("c", "data", "data: the clean buffer of x differs", deterministic=False)
        squeezed = entry("d", "bounds", "bounds: a label leaves its row", count=50)
        caret = entry("e", "state", "state: the status bar says Ln <n>")
        ordered = sorted([stale, squeezed, caret, lost, asan], key=soak.priority_key)
        self.assertEqual([e.sig for e in ordered], ["b", "c", "e", "d", "a"])

    def test_a_finding_only_tiny_windows_show_waits_within_its_tier(self):
        tiny = entry("t", "log", "log: QA-STALE", cells=("default/default/640x480@1",), count=500,
                     deterministic=True)
        real = entry("r", "log", "log: QA-STALE", cells=("default/default/640x480@1", "layout/default/1920x1080@1"),
                     count=2)
        self.assertTrue(soak.tiny_only(tiny))
        self.assertFalse(soak.tiny_only(real))
        self.assertEqual(sorted([tiny, real], key=soak.priority_key)[0].sig, "r")
        self.assertEqual(soak.priority_tag(tiny), "paint/tiny")
        self.assertEqual(soak.priority_tag(real), "paint")


if __name__ == "__main__":
    unittest.main()
