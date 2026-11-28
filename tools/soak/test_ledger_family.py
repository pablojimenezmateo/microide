#!/usr/bin/env python3
"""`soak.py ledger fixed|claim|close SIG --family` books every open signature of
SIG's family at once (TD-2026-11-15-452 item 1: one defect is often many
signatures, and a fix was booked one signature at a time)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_housekeeping import scratch_soak  # noqa: E402

STALE = "log: [statusBar|{}|area|status:line-column] QA-STALE frame=<n> reason={}"


class FamilyBookingTests(unittest.TestCase):
    def setUp(self):
        self.soak = scratch_soak(self)
        book = self.soak.ledger()
        for sig, rel, why in (("stale1", "outside", "prepared"), ("stale2", "clip-edge", "prepared"),
                              ("stale3", "outside", "render-settle"), ("stale4", "outside", "prepared")):
            book.append(sig, "new", kind="log", label=STALE.format(rel, why), cell="c", commit="old")
        book.append("stale4", "fixed", commit="earlier")
        book.append("stale4", "verified")
        book.append("other1", "new", kind="log", label="log: [statusBar|outside|area|status:encoding] QA-STALE "
                    "frame=<n> reason=prepared", cell="c", commit="old")

    def ledger(self, action: str, sig: str, extra: str | None, family: bool) -> int:
        with mock.patch.object(self.soak, "git", return_value="f1x"):
            return self.soak.cmd_ledger(SimpleNamespace(action=action, sig=sig, extra=extra, family=family))

    def test_fixed_with_family_books_every_open_signature_of_it(self):
        self.assertEqual(self.ledger("fixed", "stale2", "HEAD", True), 0)
        entries = self.soak.ledger().entries()
        self.assertEqual({sig for sig, e in entries.items() if e.status == "fixed"}, {"stale1", "stale2", "stale3"})
        self.assertEqual(entries["stale1"].fix_commit, "f1x")
        self.assertEqual(entries["stale4"].status, "verified", "a closed member is left as it is")
        self.assertEqual(entries["other1"].status, "new", "another element is another family")

    def test_without_family_one_signature_is_booked(self):
        self.assertEqual(self.ledger("fixed", "stale2", "HEAD", False), 0)
        entries = self.soak.ledger().entries()
        self.assertEqual({sig for sig, e in entries.items() if e.status == "fixed"}, {"stale2"})

    def test_claim_and_close_take_the_family_too(self):
        self.ledger("claim", "stale1", "me", True)
        self.assertEqual({sig for sig, e in self.soak.ledger().entries().items() if e.status == "claimed"},
                         {"stale1", "stale2", "stale3"})
        self.ledger("close", "stale1", "a harness fault", True)
        self.assertEqual({sig for sig, e in self.soak.ledger().entries().items() if e.status == "closed"},
                         {"stale1", "stale2", "stale3"})


if __name__ == "__main__":
    unittest.main()
