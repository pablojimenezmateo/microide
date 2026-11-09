"""A redraw-verifier finding's signature tells its defects apart."""
import unittest

import findings as fnd

SURFACES = {"editorSurface": [0, 24, 1440, 850]}


def stale(bbox: str, first: str, clips: str, scale: str = "") -> str:
    return (f"QA-STALE frame=1 reason=event-partial pixels=9 bbox={bbox} first={first} "
            f"clips={clips}" + (f" scale={scale}" if scale else ""))


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

    def test_device_pixels_are_set_against_logical_rects(self):
        # Soak 0c1b56e84217's banner sighting at density 2: bbox and first are
        # device pixels, the clips and surfaces logical. Unscaled, the box's centre
        # (1249,202) fell outside every surface and its first pixel outside the
        # clips it sits just under.
        line = stale("578,172,1342,60", "578,172", "0,0,960,86;289,301,671,239", "2")
        self.assertEqual(fnd.stale_surface(line, {"editorSurface": [289, 60, 671, 480]}),
                         "editorSurface")
        self.assertEqual(fnd.stale_clip_relation(line), "clip-edge")
        self.assertEqual(fnd.stale_scale(stale("1,1,1,1", "1,1", "none")), 1.0)
        self.assertEqual(fnd.stale_scale(stale("1,1,1,1", "1,1", "none", "1.25")), 1.25)

    def test_other_findings_have_no_context(self):
        self.assertEqual(fnd.stale_context("undo: 3 undo(s) changed the text", SURFACES), "")


if __name__ == "__main__":
    unittest.main()
