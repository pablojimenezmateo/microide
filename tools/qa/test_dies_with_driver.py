#!/usr/bin/env python3
"""A display server the driver starts dies with the driver (App._dies_with_driver):
a driver SIGKILLed by its caller left its Xvfb running for days."""
from __future__ import annotations

import os
import signal
import subprocess
import sys
import textwrap
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


class DiesWithDriverTests(unittest.TestCase):
    def test_a_child_dies_when_its_driver_is_killed(self) -> None:
        # A stand-in driver: starts a long sleep the way _start_xvfb starts Xvfb,
        # prints its pid, and waits to be killed.
        driver = subprocess.Popen([sys.executable, "-c", textwrap.dedent(f"""
            import subprocess, sys, time
            sys.path.insert(0, {str(HERE)!r})
            import microide_qa as q
            child = subprocess.Popen(["sleep", "60"], preexec_fn=q.App._dies_with_driver())
            print(child.pid, flush=True)
            time.sleep(60)
        """)], stdout=subprocess.PIPE, text=True)
        assert driver.stdout is not None
        child = int(driver.stdout.readline())
        os.kill(driver.pid, signal.SIGKILL)
        driver.wait()
        deadline = time.monotonic() + 5
        alive = True
        while alive and time.monotonic() < deadline:
            try:
                os.kill(child, 0)
                # A zombie still answers kill(0): it is gone once its stat says Z or it is reaped.
                state = Path(f"/proc/{child}/stat").read_text().split()[2]
                alive = state != "Z"
            except (ProcessLookupError, FileNotFoundError):
                alive = False
            time.sleep(0.05)
        self.assertFalse(alive, "the child outlived its SIGKILLed driver")


if __name__ == "__main__":
    unittest.main()
