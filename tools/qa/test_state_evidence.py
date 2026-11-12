#!/usr/bin/env python3
"""The state oracles' evidence: a status bar that disagrees with the caret is
reported with what tells its causes apart (soak 5a2cb8ba7f6b)."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import microide_qa as q  # noqa: E402


class StateEvidenceTests(unittest.TestCase):
    def test_a_stale_line_column_label_keeps_the_line_and_the_frames_read(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            log = Path(work) / "microide.log"
            log.write_text("microide redraw frame: frame=1\nmicroide redraw frame: frame=2\n")
            app = q.App(binary=Path("/bin/true"), work=Path(work), display=":0", size=(800, 600), env={})
            app.log_path = log
            ui = {"elements": [{"name": "status:line-column", "label": "Ln 1, Col 4", "rect": [0, 0, 1, 1]}]}
            buffer = {"available": True, "kind": "editor", "text": "col\tx\n", "tabSize": 4, "revision": 7,
                      "cursors": [{"line": 1, "column": 6}]}
            app.query = lambda verb, args=None: buffer if verb == "buffer" else {}
            app.ui = lambda: ui
            with mock.patch.object(q.time, "sleep", lambda _s: None):
                found = app._state_oracles(ui)
        self.assertEqual(found, [("state", "the status bar says Ln 1, Col 4; the caret is at Ln 1, Col 6")])
        evidence = "\n".join(app.state_evidence)
        self.assertIn("'col\\tx'", evidence)
        self.assertIn("tabSize 4", evidence)
        self.assertIn("read 0: 2 frame(s) logged", evidence)


class UndecodableRecordEvidenceTests(unittest.TestCase):
    def test_a_record_that_did_not_decode_is_kept_with_its_backup(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            state = Path(work) / "state"
            state.mkdir()
            (state / "config").write_bytes(b"MIDE\x00body")
            (state / "config.bak").write_bytes(b"MIDE\x00older")
            app = q.App(binary=Path("/bin/true"), work=Path(work), display=":0", size=(800, 600), env={})
            app.shots = Path(work) / "shots"
            app.log_path = Path(work) / "microide.log"
            app.log_path.write_text("")
            app.alive = lambda: False  # type: ignore[method-assign]
            detail = f"persistence: {state / 'config'} read but did not decode"
            app._write_finding("log", [q.Finding("log", detail)])
            folder = next(app.shots.glob("finding-*"))
            self.assertEqual((folder / "undecodable-config").read_bytes(), b"MIDE\x00body")
            self.assertEqual((folder / "undecodable-config.bak").read_bytes(), b"MIDE\x00older")


if __name__ == "__main__":
    unittest.main()
