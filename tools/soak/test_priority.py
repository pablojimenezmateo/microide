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


class FamilyTests(unittest.TestCase):
    def test_stale_paint_is_its_own_family_listing(self):
        stale = [entry(sig, "log", f"log: [statusBar|{rel}|area|status:line-column] QA-STALE frame=<n> reason={why}",
                       count=n)
                 for sig, rel, why, n in (("s1", "outside", "prepared", 900), ("s2", "clip-edge", "prepared", 3),
                                          ("s3", "outside", "render-settle", 4))]
        crash = entry("c", "crash", "crash: SIGSEGV")
        self.assertEqual([name for name, _ in soak.open_families(stale + [crash])], ["crash: SIGSEGV"])
        paint = soak.open_families(stale + [crash], paint=True)
        self.assertEqual(len(paint), 1)
        self.assertEqual({e.sig for e in paint[0][1]}, {"s1", "s2", "s3"})


class NextCheckTests(unittest.TestCase):
    def setUp(self):
        stale = "log: [statusBar|outside|area|status:line-column] QA-STALE frame=<n> reason=prepared"
        self.known = {
            "sev": entry("sev", "crash", "crash: SIGSEGV in A", count=50),
            "sev-done": entry("sev-done", "crash", "crash: SIGSEGV in B", deterministic=True),
            "sev-twin": entry("sev-twin", "crash", "crash: SIGSEGV in B", count=90),
            "paint": entry("paint", "log", stale, count=900),
            "paint-twin": entry("paint-twin", "log", stale.replace("outside", "clip-edge"), count=5),
        }

    def test_a_family_nobody_measured_goes_before_another_signature_of_a_measured_one(self):
        pending = ["sev-twin", "sev"]
        self.assertEqual(soak.next_check(pending, self.known, 0), "sev")

    def test_the_most_severe_unmeasured_family_leads(self):
        pending = ["paint", "sev"]
        self.assertEqual([soak.next_check(pending, self.known, n) for n in (0, 1)], ["sev", "sev"])

    def test_every_third_check_is_a_lower_tier_one_so_paint_never_starves(self):
        pending = ["paint", "paint-twin", "sev"]
        self.assertEqual(soak.next_check(pending, self.known, soak.CHECK_LOW_TIER_EVERY - 1), "paint")

    def test_with_only_measured_families_left_the_order_is_by_priority(self):
        self.assertEqual(soak.next_check(["sev-twin"], self.known, 0), "sev-twin")


class IdleNoteTests(unittest.TestCase):
    def test_a_lasting_stall_is_logged_once_and_its_end_with_how_long(self):
        idle: dict = {}
        disk = "only 29 GB free on the disk (MICROIDE_SOAK_MIN_FREE_GB)"
        self.assertIsNone(soak.idle_note(idle, disk, 1000.0))
        self.assertIsNone(soak.idle_note(idle, disk.replace("29", "28"), 1000.0 + soak.IDLE_NOTE_SECONDS - 1))
        note = soak.idle_note(idle, disk, 1000.0 + soak.IDLE_NOTE_SECONDS)
        self.assertIn("starting nothing", note)
        self.assertIn("GB free on the disk", note)
        self.assertIsNone(soak.idle_note(idle, disk, 5000.0), "once")
        self.assertIn("2.8 h", soak.idle_note(idle, None, 1000.0 + 2.8 * 3600))
        self.assertEqual(idle, {})

    def test_a_short_wait_says_nothing(self):
        idle: dict = {}
        self.assertIsNone(soak.idle_note(idle, "the machine is busy (9.0 cores not the soak's; load 12.0)", 0.0))
        self.assertIsNone(soak.idle_note(idle, None, 60.0))

    def test_another_reason_starts_its_own_clock(self):
        idle: dict = {}
        soak.idle_note(idle, "paused (build/soak/PAUSE)", 0.0)
        self.assertIsNone(soak.idle_note(idle, "only 3 GB free on the disk", soak.IDLE_NOTE_SECONDS))


class PlateauTests(unittest.TestCase):
    def test_a_job_type_that_stopped_finding_anything_new_gives_way(self):
        job = soak.JobType("a", 10, "default")
        fresh = soak.job_weight(job, {"quiet_runs": soak.PLATEAU_RUNS - 1})
        flat = soak.job_weight(job, {"quiet_runs": soak.PLATEAU_RUNS})
        self.assertEqual(fresh, 10.0)
        self.assertEqual(flat, 10.0 * soak.PLATEAU_SHARE)

    def test_a_run_with_anything_new_ends_the_plateau(self):
        stats = {"quiet_runs": 40}
        soak.note_novelty(stats, [{"steps": 300, "new_states": 0, "new_transitions": 2}], 0)
        self.assertEqual(stats["quiet_runs"], 0)
        soak.note_novelty(stats, [{"steps": 300, "new_states": 0, "new_transitions": 0}], 0)
        soak.note_novelty(stats, [], 0)
        soak.note_novelty(stats, [{"steps": 0}], 0)
        self.assertEqual(stats["quiet_runs"], 1, "a run that took no step says nothing")
        soak.note_novelty(stats, [{"steps": 300, "new_states": 0}], 1)
        self.assertEqual(stats["quiet_runs"], 0, "a new finding is new")


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
