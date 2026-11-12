#!/usr/bin/env python3
"""The ledger's fold: a sighting reopens a fixed signature only from a build that
has the fix. The runner keeps jobs of the previous build going while it builds the
next, so a fix marked against a commit the soak has not built yet was "reopened"
by the very bug it fixes (5508be921fb2, reopened by a 5f577c2a sighting after
15b3a202a fixed it)."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ledger as ld  # noqa: E402

# A history main -> fix -> later: "old" lacks the fix, "later" has it.
_HAS = {("fix", "fix"), ("fix", "later")}


def _contains(fix: str, commit: str) -> bool:
    return (fix, commit) in _HAS


class ReopenTests(unittest.TestCase):
    def _fold(self, *seen_commits: str, status: str = "fixed") -> ld.Entry:
        with tempfile.TemporaryDirectory() as work:
            book = ld.Ledger(Path(work) / "ledger.jsonl", contains=_contains)
            book.append("s", "new", commit="old")
            book.append("s", "fixed", commit="fix")
            if status == "verified":
                book.append("s", "verified", commit="later")
            for commit in seen_commits:
                book.append("s", "seen", commit=commit)
            return book.entries()["s"]

    def test_a_sighting_on_a_build_without_the_fix_does_not_reopen(self) -> None:
        entry = self._fold("old")
        self.assertEqual(entry.status, "fixed")
        self.assertEqual(entry.count, 2)  # still counted

    def test_a_sighting_on_a_build_with_the_fix_reopens(self) -> None:
        self.assertEqual(self._fold("old", "later").status, "reopened")

    def test_a_verified_fix_reopens_only_from_a_build_with_it(self) -> None:
        self.assertEqual(self._fold("old", status="verified").status, "verified")
        self.assertEqual(self._fold("later", status="verified").status, "reopened")

    def test_without_an_ancestry_check_any_sighting_reopens(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            book = ld.Ledger(Path(work) / "ledger.jsonl")
            book.append("s", "new", commit="old")
            book.append("s", "fixed", commit="fix")
            book.append("s", "seen", commit="old")
            self.assertEqual(book.entries()["s"].status, "reopened")

    def test_fixed_in(self) -> None:
        entry = self._fold()
        self.assertTrue(entry.fixed_in("later", _contains))
        self.assertFalse(entry.fixed_in("old", _contains))


if __name__ == "__main__":
    unittest.main()
