"""Recording the app under rr (https://rr-project.org): what the QA driver and
the soak runner both need to know about it, and nothing that needs a window.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

# MQA_RR=1 (or `chaos`) records every launch of the app under `rr record`: a run
# that shows a finding then keeps an execution trace that replays the same
# instructions every time, with reverse execution -- the one piece of evidence
# that turns a finding no replay of its ACTIONS reproduces (a race, a kill -9 that
# landed at the wrong instant) into one that reproduces always. `chaos` is rr's
# chaos mode: randomized scheduling, to make races show more often. Measured on
# the monkey (2026-11-16): 20% more wall time, about 80 KB of trace a step.
RR_MODES = ("1", "chaos")


def rr_mode() -> str:
    """"" (not recording), "1" or "chaos", from $MQA_RR."""
    mode = os.environ.get("MQA_RR", "")
    if mode in ("", "0"):
        return ""
    if mode not in RR_MODES:
        raise ValueError(f"MQA_RR={mode!r}: expected one of {', '.join(RR_MODES)}")
    return mode


def is_rr_line(line: str) -> bool:
    """A line rr wrote into the app's log (they share stderr) about its own
    failure: its FATAL header and its "Assertion ... failed to hold" -- which is
    rr's assertion, not the app's, and not an app finding."""
    return line.startswith("[FATAL src/") or "' failed to hold." in line


def rr_failure(log_text: str) -> str | None:
    """Why rr gave up recording, when the log shows it did: its assertion's
    message, else its FATAL line. rr does not implement every system call
    (`tee`, seen from a command a terminal ran), and stops the whole recording,
    app included, at the first it does not know."""
    lines = log_text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("[FATAL src/"):
            for following in lines[index + 1:index + 4]:
                if "' failed to hold." in following:
                    return following.strip().lstrip("-> ").strip()[:300]
            return line.strip()[:300]
    return None


def rr_unusable_reason() -> str | None:
    """Why `rr record` cannot run here, or None: no rr, or the kernel refuses an
    unprivileged user the performance counters it counts instructions with."""
    if shutil.which("rr") is None:
        return "rr is not installed"
    try:
        paranoid = int(Path("/proc/sys/kernel/perf_event_paranoid").read_text())
    except (OSError, ValueError):
        return None
    if paranoid > 1:
        return f"kernel.perf_event_paranoid is {paranoid}; rr needs 1 or less"
    return None


def finish_recordings(directory: Path, keep: bool) -> list[Path]:
    """Ends a run's rr traces under `directory` (App.recording_dir): deleted when
    the run found nothing, else left as they are and returned.

    Not packed. A trace is its event log (about 80 KB a monkey step) and a hard
    link to the app binary, so it costs little and outlives the soak's pruning
    of that build. `rr pack` would add a copy of every system library the app
    maps -- 450 MB a trace, measured -- to survive an upgrade of those
    libraries; pack by hand a trace worth keeping that long."""
    traces = sorted(p for p in directory.glob("launch-*") if p.is_dir()) if directory.is_dir() else []
    if not keep:
        shutil.rmtree(directory, ignore_errors=True)
        return []
    return traces
