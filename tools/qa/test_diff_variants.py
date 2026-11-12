#!/usr/bin/env python3
"""The structural-diff fixtures and oracle (tools/qa/diff_variants.py,
monkey_oracles.StructuralDiffOracle)."""
from __future__ import annotations

import random
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import diff_variants as dv  # noqa: E402
import monkey_oracles as mo  # noqa: E402


class VariantTests(unittest.TestCase):
    def test_a_counter_names_one_pair(self) -> None:
        self.assertEqual(dv.variant_pair(17), dv.variant_pair(17))

    def test_every_edit_runs_on_every_language(self) -> None:
        for language in dv.LANGUAGES:
            base = dv._base(random.Random(1), language, 4)
            for edit in dv.EDITS:
                out = edit(random.Random(2), list(base), language)
                self.assertIsInstance(out, list, f"{edit.__name__} on {language}")

    def test_most_pairs_differ(self) -> None:
        differing = sum(left != right for _, left, right in (dv.variant_pair(c) for c in range(60)))
        self.assertGreater(differing, 50)


def _row(kind: str, hunk: int, left: str = "", right: str = "", left_spans=(), right_spans=()) -> dict:
    return {"kind": kind, "hunk": hunk, "leftLine": 1 if kind != "added" else 0,
            "rightLine": 1 if kind != "deleted" else 0, "leftText": left, "rightText": right,
            "leftSpans": [list(s) for s in left_spans], "rightSpans": [list(s) for s in right_spans]}


def _check(rows: list[dict]) -> list[tuple[str, str]]:
    app = mock.Mock()
    app.query.return_value = {"available": True, "rows": rows, "rowsTruncated": False}
    return mo.StructuralDiffOracle(app).check()


class StructuralOracleTests(unittest.TestCase):
    def test_a_marked_real_change_is_clean(self) -> None:
        self.assertEqual(_check([_row("modified", 0, "foo(x);", "foo(y);", [(4, 5)], [(4, 5)])]), [])

    def test_a_reindent_that_marks_a_token_is_reported(self) -> None:
        found = _check([_row("modified", 0, "foo(x);", "    foo(x);", right_spans=[(4, 7)])])
        self.assertEqual(len(found), 1)
        self.assertIn("only whitespace", found[0][1])

    def test_a_real_change_shown_as_layout_only_is_reported(self) -> None:
        found = _check([_row("modified", 0, "foo(x);", "foo(y);")])
        self.assertEqual(len(found), 1)
        self.assertIn("layout only", found[0][1])

    def test_an_added_line_shows_a_change_without_spans(self) -> None:
        self.assertEqual(_check([_row("added", 0, right="bar();")]), [])

    def test_a_span_outside_its_line_or_inside_a_character_is_reported(self) -> None:
        self.assertEqual(len(_check([_row("modified", 0, "ab", "ac", [(1, 2)], [(1, 9)])])), 1)
        self.assertEqual(len(_check([_row("modified", 0, "é", "e", [(1, 2)], [(0, 1)])])), 1)

    def test_spans_on_an_unchanged_row_are_reported(self) -> None:
        found = _check([_row("unchanged", -1, "x", "x", [(0, 1)])])
        self.assertEqual(len(found), 1)
        self.assertIn("did not change", found[0][1])

    def test_no_diff_in_front_checks_nothing(self) -> None:
        app = mock.Mock()
        app.query.return_value = {"available": False}
        self.assertEqual(mo.StructuralDiffOracle(app).check(), [])


if __name__ == "__main__":
    unittest.main()
