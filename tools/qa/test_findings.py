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

    def test_two_controls_on_one_surface_are_two_defects(self):
        # Soak 250931ec505a (640x480 at ui.scale 3) held both of these: Replace
        # All left lit under a toast, and a split pane's scrollbar drawn over the
        # sidebar's edge. Same surface, same clip relation, same shape.
        surfaces = {"sidebar": [0, 25, 120, 83], "editorSurface": [121, 86, 92, 50]}
        elements = [
            {"name": "button:search/replace-all", "rect": [76, 107, 34, 20]},
            {"name": "field:search/query", "rect": [10, 83, 100, 20]},
            {"name": "sidebar-view:search", "rect": [36, 29, 22, 19]},
        ]
        lit = stale("228,321,102,3", "228,321", "121,24,93,85", "3")
        spill = stale("351,264,12,108", "351,264", "121,24,93,115", "3")
        sig = lambda d: fnd.signature("log", d, "", fnd.stale_context(d, surfaces, elements))  # noqa: E731
        self.assertEqual(fnd.stale_element(lit, elements), "button:search/replace-all")
        self.assertEqual(fnd.stale_element(spill, elements), "-")
        self.assertNotEqual(sig(lit), sig(spill))
        # Without the elements (an older `ui` answer) the context is what it was.
        self.assertEqual(fnd.stale_context(lit, surfaces), "sidebar|outside|area")

    def test_element_keys_drop_what_differs_between_runs(self):
        self.assertEqual(fnd.element_key("button:search/replace-all"), "button:search/replace-all")
        self.assertEqual(fnd.element_key("scrollbar:editor/vertical#2"), "scrollbar:editor/vertical")
        self.assertEqual(fnd.element_key("tab:0/a.txt"), "tab")
        self.assertEqual(fnd.element_key("close-tab/3/notes.md".replace("/", ":", 1)), "close-tab")
        self.assertEqual(fnd.element_key("row:tree/src/main.cpp"), "row:tree")
        self.assertEqual(fnd.element_key("menu-item:File/Open Recent"), "menu-item:File")
        self.assertEqual(fnd.element_key("divider:editor/0"), "divider:editor")

    def test_the_label_names_the_context(self):
        self.assertTrue(fnd.describe("log", "QA-STALE frame=1", "sidebar|outside|area|-")
                        .startswith("log: [sidebar|outside|area|-] QA-STALE"))
        self.assertEqual(fnd.describe("undo", "3 undo(s)"), "undo: <n> undo(s)")

    def test_other_findings_have_no_context(self):
        self.assertEqual(fnd.stale_context("undo: 3 undo(s) changed the text", SURFACES), "")


if __name__ == "__main__":
    unittest.main()
