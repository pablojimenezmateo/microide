#!/usr/bin/env python3
"""The `overlap` oracle's verdicts (microide_qa._surface_and_overlap_findings):
two controls laid out on top of each other are a finding; a floating layer over
what it floats over is not."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import microide_qa as q  # noqa: E402


def _ui(*elements: tuple[str, list[int]]) -> dict:
    return {"elements": [{"name": name, "kind": name.split(":", 1)[0], "rect": rect} for name, rect in elements]}


class OverlapOracleTests(unittest.TestCase):
    def overlaps(self, ui: dict) -> list[tuple[str, str]]:
        return [f for f in q._surface_and_overlap_findings(ui) if f[0] == "overlap"]

    def test_two_chrome_buttons_on_each_other_are_a_finding(self) -> None:
        ui = _ui(("button:debug-toolbar/continue", [1246, 136, 26, 26]), ("button:banner/Reload", [1240, 130, 60, 20]))
        self.assertEqual(len(self.overlaps(ui)), 1)

    def test_the_debug_toolbar_floats_over_a_code_lens(self) -> None:
        # Soak 5dd22e100ed0 (561 sightings): the toolbar is a floating card over
        # the editor's content, and a code lens is that content -- it scrolls
        # under the card like the text does. VS Code's floating toolbar covers it
        # the same way.
        ui = _ui(("button:lens/microide ✦ greet (0)", [1248, 126, 160, 16]),
                 ("button:debug-toolbar/continue", [1246, 136, 26, 26]))
        self.assertEqual(self.overlaps(ui), [])

    def test_the_debug_toolbar_floats_over_a_diffs_collapsed_lines_bar(self) -> None:
        # Soak 4df2b07d4b82: the bar's buttons scroll with the diff's rows.
        ui = _ui(("button:compare/context-0/all", [1246, 127, 76, 16]),
                 ("button:debug-toolbar/continue", [1246, 104, 26, 26]))
        self.assertEqual(self.overlaps(ui), [])

    def test_two_lenses_on_each_other_still_are(self) -> None:
        ui = _ui(("button:lens/a", [10, 10, 100, 16]), ("button:lens/b", [50, 12, 100, 16]))
        self.assertEqual(len(self.overlaps(ui)), 1)


if __name__ == "__main__":
    unittest.main()
