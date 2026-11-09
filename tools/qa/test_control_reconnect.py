"""A control connection the app sheds costs one request, not the rest of the run."""
import json
import os
import socket
import tempfile
import threading
import unittest
from pathlib import Path

import microide_qa as q


class FakeChannel:
    """Answers each connection's first request, then closes it (a shed)."""

    def __init__(self, path: Path):
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(str(path))
        self.server.listen(4)
        self.connections = 0
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            self.connections += 1
            with conn:
                line = conn.makefile("rb").readline()
                request = json.loads(line)
                conn.sendall(json.dumps({"id": request["id"], "ok": True}).encode() + b"\n")


class ReconnectTests(unittest.TestCase):
    def test_the_request_after_a_shed_connects_afresh(self):
        directory = Path(tempfile.mkdtemp(dir=os.environ.get("TMPDIR")))
        path = directory / "control.sock"
        channel = FakeChannel(path)
        app = q.App.__new__(q.App)
        app._socket, app._socket_buffer, app._next_id = None, b"", 1
        app._stall_captured, app.shots, app.proc = True, None, None
        app._await_typed_input = lambda *a, **k: None
        app._control = lambda args, timeout=10.0: type("R", (), {"stdout": json.dumps({"socket": str(path)})})()
        app.note_latency = lambda *a: None

        self.assertTrue(app._socket_request({"query": "ui"}, 2.0, "first")["ok"])
        with self.assertRaises(q.QaFailure):  # the peer closed after one answer
            for _ in range(3):
                app._socket_request({"query": "ui"}, 2.0, "second")
        self.assertTrue(app._socket_request({"query": "ui"}, 2.0, "third")["ok"])
        self.assertEqual(channel.connections, 2)


if __name__ == "__main__":
    unittest.main()
