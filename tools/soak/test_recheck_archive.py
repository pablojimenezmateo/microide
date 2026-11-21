#!/usr/bin/env python3
"""Open findings a replay reproduced are replayed again on newer builds
(soak.recheck_open / recheck_one), and a severe finding's evidence is archived,
not deleted, when its bundle is pruned (soak.prune_verified_bundle)."""
from __future__ import annotations

import json
import sys
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_housekeeping import scratch_soak  # noqa: E402


def _bundle(soak, sig: str, kind: str, label: str, replay: bool = True) -> Path:
    bundle = soak.FINDINGS / sig
    bundle.mkdir(parents=True, exist_ok=True)
    (bundle / "meta.json").write_text(json.dumps({"finding": {"kind": kind, "label": label}}))
    if replay:
        (bundle / "replay.json").write_text("{}")
    return bundle


def _seen(soak, sig: str, kind: str, label: str, rate: float | None = 1.0) -> None:
    soak.ledger().append(sig, "new", kind=kind, label=label, cell="default/default/800x600@1", commit="old")
    if rate is not None:
        soak.ledger().append(sig, "checked", rate=rate, runs=3, ran=3, deterministic=rate >= 2 / 3)


class RecheckQueueTests(unittest.TestCase):
    def setUp(self):
        self.soak = scratch_soak(self)
        self.state = {"good": {"default": {"sha": "newsha", "binary": "/bin/true"}}}

    def test_reproducing_open_findings_are_queued_most_severe_first_and_capped(self):
        soak = self.soak
        for i in range(soak.RECHECK_BATCH + 2):
            sig = f"paint{i:07d}"
            _bundle(soak, sig, "log", "log: QA-STALE frame=1")
            _seen(soak, sig, "log", "log: QA-STALE frame=1")
        _bundle(soak, "crash000000a", "crash", "crash: exited")
        _seen(soak, "crash000000a", "crash", "crash: exited")
        soak.recheck_open(self.state, lambda _m: None)
        queued = self.state["pending_verifies"]
        self.assertEqual(len(queued), soak.RECHECK_BATCH)
        self.assertEqual(queued[0], "crash000000a", "the severe finding first")

    def test_what_is_not_rechecked(self):
        soak = self.soak
        _bundle(soak, "neverrepro01", "crash", "c")
        _seen(soak, "neverrepro01", "crash", "c", rate=0.0)  # no replay ever showed it
        _bundle(soak, "unmeasured01", "crash", "c")
        _seen(soak, "unmeasured01", "crash", "c", rate=None)
        _bundle(soak, "claimed00001", "crash", "c")
        _seen(soak, "claimed00001", "crash", "c")
        soak.ledger().append("claimed00001", "claimed", by="someone")
        _bundle(soak, "doneonthis01", "crash", "c")
        _seen(soak, "doneonthis01", "crash", "c")
        soak.ledger().append("doneonthis01", "rechecked", on="newsha", rate=1.0)
        _bundle(soak, "recentcheck1", "crash", "c")
        _seen(soak, "recentcheck1", "crash", "c")
        soak.ledger().append("recentcheck1", "rechecked", on="oldersha", rate=1.0)
        _bundle(soak, "noreplay0001", "crash", "c", replay=False)
        _seen(soak, "noreplay0001", "crash", "c")
        self.assertEqual(soak.open_to_recheck(self.state), [])
        # A day later the one checked on an older build is due again.
        due = [e.sig for e in soak.open_to_recheck(self.state, now=time.time() + soak.RECHECK_INTERVAL + 1)]
        self.assertEqual(due, ["recentcheck1"])


class RecheckVerdictTests(unittest.TestCase):
    def setUp(self):
        self.soak = scratch_soak(self)
        self.good = {"sha": "newsha1234567", "binary": "/bin/true"}
        self.state = {"good": {"default": self.good}}
        _bundle(self.soak, "abc123abc123", "crash", "crash: exited")
        _seen(self.soak, "abc123abc123", "crash", "crash: exited")

    def _run(self, rate: float, ran: int = 3):
        out = mock.Mock(stdout=json.dumps({"rate": rate, "ran": ran}) + "\n")
        with mock.patch.object(self.soak.subprocess, "run", return_value=out):
            self.soak.verify_one("abc123abc123", self.state, lambda _m: None)
        return self.soak.ledger().entries()["abc123abc123"]

    def test_gone_on_the_new_build_closes_it(self):
        entry = self._run(0.0)
        self.assertEqual(entry.status, "closed")

    def test_still_there_records_the_recheck(self):
        entry = self._run(1.0)
        self.assertEqual(entry.status, "new")
        self.assertEqual(entry.rechecked_on, "newsha1234567")

    def test_no_replay_ran_measures_nothing(self):
        entry = self._run(0.0, ran=0)
        self.assertEqual(entry.status, "new", "a rate of 0 from replays that never ran closes nothing")

    def test_a_later_sighting_reopens_a_closed_one(self):
        self._run(0.0)
        self.soak.ledger().append("abc123abc123", "seen", cell="default/default/800x600@1", commit="newer")
        self.assertEqual(self.soak.ledger().entries()["abc123abc123"].status, "reopened")


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.soak = scratch_soak(self)

    def _evidence(self, bundle: Path, size: int = 1000) -> None:
        for name in ("evidence/frame.png", "sightings/s1/rr/trace", "verify/run0/log.txt"):
            (bundle / name).parent.mkdir(parents=True, exist_ok=True)
            (bundle / name).write_bytes(b"x" * size)

    def test_a_severe_findings_evidence_is_archived_not_deleted(self):
        bundle = _bundle(self.soak, "sev123sev123", "crash", "crash: exited")
        self._evidence(bundle)
        self.assertEqual(self.soak.prune_verified_bundle(bundle), 3000)
        self.assertFalse((bundle / "sightings").exists(), "the bundle is pruned")
        self.assertTrue((self.soak.ARCHIVE / "sev123sev123" / "sightings" / "s1" / "rr" / "trace").exists(),
                        "the recording survives in the archive")

    def test_a_paint_findings_evidence_is_deleted(self):
        bundle = _bundle(self.soak, "pnt123pnt123", "log", "log: QA-STALE frame=1")
        self._evidence(bundle)
        self.soak.prune_verified_bundle(bundle)
        self.assertFalse((self.soak.ARCHIVE / "pnt123pnt123").exists())

    def test_the_archive_keeps_the_newest_within_its_size(self):
        soak = self.soak
        for i, sig in enumerate(("old000000001", "mid000000002", "new000000003")):
            bundle = _bundle(soak, sig, "crash", "c")
            self._evidence(bundle)
            soak.prune_verified_bundle(bundle)
            import os
            os.utime(soak.ARCHIVE / sig, (1000 + i, 1000 + i))
        soak.trim_archive(limit=7000)
        self.assertEqual(sorted(p.name for p in soak.ARCHIVE.iterdir()), ["mid000000002", "new000000003"])


if __name__ == "__main__":
    unittest.main()
