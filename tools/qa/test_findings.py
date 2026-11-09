"""A redraw-verifier finding's signature tells its defects apart."""
import unittest

import findings as fnd

SURFACES = {"editorSurface": [0, 24, 1440, 850]}


def stale(bbox: str, first: str, clips: str) -> str:
    return (f"QA-STALE frame=1 reason=event-partial pixels=9 bbox={bbox} first={first} "
            f"clips={clips}")


class StaleSignatureTests(unittest.TestCase):
    def test_clip_relation(self):
        # The soak's 49a23ffecdf7 held all three of these under one signature.
        self.assertEqual(fnd.stale_clip_relation(stale("703,152,1,9", "703,152", "325,85,378,160")),
                         "clip-edge")
        self.assertEqual(fnd.stale_clip_relation(stale("434,81,206,96", "483,81", "0,0,214,26")),
                         "outside")
        self.assertEqual(fnd.stale_clip_relation(stale("10,30,5,5", "10,30", "0,24,100,100")),
                         "in-clip")
        self.assertEqual(fnd.stale_clip_relation("data: not a stale frame"), "")

    def test_one_signature_per_defect_shape(self):
        edge = stale("703,152,1,9", "703,152", "325,85,378,160")
        missing = stale("434,81,206,96", "483,81", "0,0,214,26")
        sig = lambda d: fnd.signature("log", d, "", fnd.stale_context(d, SURFACES))  # noqa: E731
        self.assertNotEqual(sig(edge), sig(missing))
        # The same defect elsewhere in the frame keeps its signature.
        self.assertEqual(sig(edge), sig(stale("1151,458,1,32", "1151,458", "0,24,1151,825")))

    def test_other_findings_have_no_context(self):
        self.assertEqual(fnd.stale_context("undo: 3 undo(s) changed the text", SURFACES), "")


if __name__ == "__main__":
    unittest.main()
