#!/usr/bin/env python3
"""The coverage report's per-commit delta (TD-2026-11-08-435 item 7): of the
branches on the lines the commits since the last report changed, how many the
soak executed each way."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import codecov  # noqa: E402

DIFF = """diff --git a/src/editor/A.cpp b/src/editor/A.cpp
--- a/src/editor/A.cpp
+++ b/src/editor/A.cpp
@@ -10,2 +10,3 @@ void F() {
+  if (x) {
+    y();
+  }
@@ -40 +41,0 @@
diff --git a/src/editor/Gone.cpp b/src/editor/Gone.cpp
--- a/src/editor/Gone.cpp
+++ /dev/null
@@ -1,4 +0,0 @@
diff --git a/tools/x.py b/tools/x.py
--- a/tools/x.py
+++ b/tools/x.py
@@ -3 +3 @@
diff --git a/src/New.cpp b/src/New.cpp
--- /dev/null
+++ b/src/New.cpp
@@ -0,0 +1 @@
"""


def branch(line: int, taken: int, not_taken: int) -> list[int]:
    return [line, 3, line, 9, taken, not_taken, 0, 0, 4]


class ChangedCoverageTests(unittest.TestCase):
    def test_the_diff_names_each_changed_source_line(self):
        self.assertEqual(codecov.changed_lines(DIFF), {"editor/A.cpp": {10, 11, 12}, "New.cpp": {1}})

    def test_only_the_branches_on_changed_lines_count_each_way(self):
        export = {"data": [{"files": [
            {"filename": "/w/src/editor/A.cpp",
             "branches": [branch(10, 5, 0), branch(11, 2, 7), branch(30, 0, 0)]},
            {"filename": "/w/src/New.cpp", "branches": [branch(1, 0, 0)]},
            {"filename": "/w/src/Other.cpp", "branches": [branch(10, 0, 0)]},
        ]}]}
        changed = codecov.changed_lines(DIFF)
        self.assertEqual(codecov.changed_branches(export, changed, "/w/src/"),
                         {"editor/A.cpp": [3, 4], "New.cpp": [0, 2]})

    def test_the_section_says_what_the_commits_added_and_what_ran(self):
        text = codecov.changed_markdown({"editor/A.cpp": [3, 4], "New.cpp": [0, 2], "Flat.cpp": [0, 0]},
                                        "aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb")
        self.assertIn("aaaaaaaaaaaa..bbbbbbbbbbbb", text)
        self.assertIn("3 of 6", text)
        self.assertIn("| `New.cpp` |", text)
        self.assertNotIn("Flat.cpp", text, "a changed file without branches is not listed")
        self.assertLess(text.index("New.cpp"), text.index("editor/A.cpp"), "the least executed first")


class PreviousReportTests(unittest.TestCase):
    def test_the_base_is_the_newest_report_at_another_commit(self):
        from test_housekeeping import scratch_soak
        soak = scratch_soak(self)
        self.assertEqual(soak.previous_report_sha("c"), "")
        (soak.SOAK / "code-coverage.jsonl").write_text(
            '{"sha": "a"}\n{"sha": "b"}\nnot json\n{"sha": "c"}\n{"sha": "c"}\n')
        self.assertEqual(soak.previous_report_sha("c"), "b")
        self.assertEqual(soak.previous_report_sha("d"), "c")


if __name__ == "__main__":
    unittest.main()
