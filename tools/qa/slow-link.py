#!/usr/bin/env python3
"""A slow network link for ssh, as a ProxyCommand: relays stdin/stdout to host:port
with a fixed one-way delay, so the remote suite can run over a "far" link against a
loopback sshd (tools/qa/local-sshd.sh's `qa-slow` alias).

    ProxyCommand python3 tools/qa/slow-link.py --delay-ms 120 127.0.0.1 22220

Bytes keep their order; each chunk is released `delay` after it arrived, in both
directions, so the round trip is twice the delay. Bandwidth is not limited.
"""

from __future__ import annotations

import argparse
import os
import select
import socket
import sys
import time
from collections import deque


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--delay-ms", type=float, default=100.0)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    args = parser.parse_args()
    delay = args.delay_ms / 1000.0
    sock = socket.create_connection((args.host, args.port))
    sock.setblocking(False)
    stdin, stdout = sys.stdin.fileno(), sys.stdout.fileno()
    os.set_blocking(stdin, False)
    to_net: deque[tuple[float, bytes]] = deque()
    to_local: deque[tuple[float, bytes]] = deque()
    stdin_open = net_open = True
    while stdin_open or net_open or to_net or to_local:
        now = time.monotonic()
        # Release whatever has waited long enough.
        while to_net and to_net[0][0] <= now:
            data = to_net.popleft()[1]
            sock.setblocking(True)
            sock.sendall(data)
            sock.setblocking(False)
        while to_local and to_local[0][0] <= now:
            data = to_local.popleft()[1]
            view = memoryview(data)
            while view:
                try:
                    n = os.write(stdout, view)
                    view = view[n:]
                except BlockingIOError:
                    select.select([], [stdout], [], 0.05)
        if not stdin_open and not to_net and net_open:
            try:
                sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
        if not net_open and not to_local and not stdin_open:
            break
        pending = [d[0] for d in (to_net[0] if to_net else None, to_local[0] if to_local else None) if d]
        timeout = max(0.0, min(pending) - now) if pending else 0.5
        readers = ([stdin] if stdin_open else []) + ([sock] if net_open else [])
        if not readers:
            time.sleep(timeout)
            continue
        ready, _, _ = select.select(readers, [], [], timeout)
        stamp = time.monotonic() + delay
        if stdin in ready:
            try:
                data = os.read(stdin, 65536)
            except BlockingIOError:
                data = None
            if data == b"":
                stdin_open = False
            elif data:
                to_net.append((stamp, data))
        if sock in ready:
            try:
                data = sock.recv(65536)
            except BlockingIOError:
                data = None
            except ConnectionResetError:
                data = b""
            if data == b"":
                net_open = False
            elif data:
                to_local.append((stamp, data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
