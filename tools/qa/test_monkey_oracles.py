#!/usr/bin/env python3
"""Unit tests for monkey_oracles that need no app: a fake answers the control
queries the oracle asks."""
from __future__ import annotations

import random
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import monkey_oracles  # noqa: E402


class _RelaunchingApp:
    """Two projects; the second holds an unsaved buffer. After the relaunch the
    first is active and the second is a restored catalog entry that loads its
    tabs when first activated -- as the real catalog does."""

    def __init__(self) -> None:
        self.work = None
        self.project = "/p/a"
        self.actions: list[dict] = []
        self.lines: list[str] = []
        self.relaunched = False
        self.active = 1
        self.loaded = [True, True]
        self.docs = {
            0: [{"path": "/p/a/x.txt", "dirty": False, "sha256": "aa", "project": "/p/a"}],
            1: [{"path": "/p/b/y.txt", "dirty": True, "sha256": "bb", "project": "/p/b"}],
        }

    def query(self, verb, args=None):
        if verb == "persistence":
            return {"staged": True}
        if verb == "projects":
            return [{"root": root, "index": i, "active": i == self.active, "loaded": self.loaded[i]}
                    for i, root in enumerate(("/p/a", "/p/b"))]
        if verb == "documents":
            return [doc for i in (0, 1) if self.loaded[i] for doc in self.docs[i]]
        raise AssertionError(verb)

    def send_line(self, line, timeout=15.0, record=True):
        self.lines.append(line)
        if line == "project-next":
            self.active = (self.active + 1) % 2
            self.loaded[self.active] = True
        return {}

    def record_external(self, row) -> None:
        self.actions.append({"external": row})

    def crash_and_relaunch(self, between=None) -> None:
        if between is not None:
            between()
        self.relaunched = True
        self.active = 0
        self.loaded = [True, False]


class Kill9OracleTests(unittest.TestCase):
    def test_a_restored_project_that_has_not_loaded_is_loaded_before_the_compare(self) -> None:
        app = _RelaunchingApp()
        oracle = monkey_oracles.DataOracle(app)
        with mock.patch.object(monkey_oracles, "CRASH_SAFETY_QUIET_S", 0.0), \
                mock.patch.object(monkey_oracles.time, "sleep", lambda _s: None):
            found = oracle.kill9_and_check(random.Random(1), waited=True)
        self.assertEqual(found, [], found)
        self.assertEqual(app.active, 0, "the lap ends on the project the relaunch made active")


if __name__ == "__main__":
    unittest.main()
