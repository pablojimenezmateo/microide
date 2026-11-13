#!/usr/bin/env python3
"""Fixes are verified by jobs the runner schedules (soak.verify_fixed queues,
start_verify runs `soak.py verify SIG`), not inline in the scheduling loop:
inline, a build that brought in five fixes kept the runner from starting any
job while their replays ran one after another."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ledger as ld  # noqa: E402
import soak  # noqa: E402


def _fixed(*sigs: str):
    good = {"sha": "abc", "binary": "/bin/true"}
    return [(ld.Entry(sig=sig), good) for sig in sigs]


class VerifyQueueTests(unittest.TestCase):
    def test_the_build_step_queues_fixes_and_replays_nothing(self) -> None:
        state: dict = {}
        with mock.patch.object(soak, "fixes_to_verify", return_value=_fixed("a1", "b2")), \
             mock.patch.object(soak.subprocess, "run") as run:
            soak.verify_fixed(state, lambda _msg: None)
        run.assert_not_called()
        self.assertEqual(state["pending_verifies"], ["a1", "b2"])

    def test_a_fix_queued_or_being_verified_is_not_queued_again(self) -> None:
        state: dict = {"pending_verifies": ["a1"], "current": ["verify/b2", "default/default/1440x900@1"]}
        with mock.patch.object(soak, "fixes_to_verify", return_value=_fixed("a1", "b2", "c3")):
            soak.verify_fixed(state, lambda _msg: None)
        self.assertEqual(state["pending_verifies"], ["a1", "c3"])


if __name__ == "__main__":
    unittest.main()
