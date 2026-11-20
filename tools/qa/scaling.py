"""The scaling oracle: an operation timed at a size N and at 2N.

Why it exists: a defect whose cost grows faster than the input is invisible to
every other oracle until the input is large enough to hit a timeout, and then it
reads as one slow answer with no cause attached. One Backspace at 16,000 carets
took 15 s (the piece tree had become a chain: f1c8498ed) and was found only by
the latency oracle's 2.5 s budget, after the soak had typed that many carets by
chance. Timing the same operation at N and 2N says it directly: a linear
operation takes about twice as long, a quadratic one four times.

Each probe builds its input in the project (under `qa-scale/`), opens it the way
a person would, and times one operation from the command to the app's next idle
point -- so the frame that shows the result is in the time, as it is in what the
user waits for. Each size is measured `REPEATS` times and the fastest kept (the
noise on a loaded machine is all upward). A probe is reported when

    t(2N) >= MIN_SECONDS (x the slow factor)   and   t(2N) / t(N) >= RATIO

and the same holds when both sizes are measured again: one slow sample on a busy
machine is not a finding. RATIO is 3, between linear (2) and quadratic (4), with
room for the n log n of a sort and a hash table's growth.

    tools/qa/run.py monkey --profile scaling --steps 7    every probe once
    tools/qa/run.py monkey --profile scaling --steps 1    one, least-run first

A probe that fails to set up (a refused command, a missing tab) is skipped, not
reported: this oracle measures time, and the other oracles own correctness.
"""

from __future__ import annotations

import contextlib
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import microide_qa as q

RATIO = 3.0
MIN_SECONDS = 0.3
REPEATS = 2


@dataclass
class Probe:
    name: str
    size: int                                      # N; 2N is measured too
    what: str                                      # the operation, as a finding names it
    setup: Callable[[q.App, Path, int], bool]      # builds and opens the input of size n
    operation: Callable[[q.App], None]             # the timed part
    teardown: Callable[[q.App], None]


def _write(path: Path, lines: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(("\n".join(lines) + "\n").encode())


def _ok(reply: dict) -> bool:
    return bool(reply) and reply.get("ok", True) is not False


def _open(app: q.App, path: Path) -> bool:
    return _ok(app.send_line(f"open {q.quote_arg(str(path))}", record=False))


def _close(app: q.App) -> None:
    # Saved first: a dirty tab's close asks, and nothing would answer.
    with contextlib.suppress(Exception):
        app.send_line("save", record=False)
        app.send_line("close-tab", record=False)


def _lines_file(root: Path, n: int, tag: str, line: Callable[[int], str]) -> Path:
    path = root / "qa-scale" / f"{tag}-{n}.txt"
    _write(path, [line(i) for i in range(n)])
    return path


def _carets(tag: str) -> Callable[[q.App, Path, int], bool]:
    """A caret at the end of every one of n lines."""
    def setup(app: q.App, root: Path, n: int) -> bool:
        path = _lines_file(root, n, tag, lambda i: f"line {i} of the caret probe")
        return (_open(app, path) and _ok(app.send_line("select-all", record=False))
                and _ok(app.send_line("add-cursors-to-line-ends", record=False)))
    return setup


def _plain(tag: str, line: Callable[[int], str], then: tuple[str, ...] = ()) -> Callable[[q.App, Path, int], bool]:
    """n lines, open; then each of `then` with {half} = n / 2."""
    def setup(app: q.App, root: Path, n: int) -> bool:
        if not _open(app, _lines_file(root, n, tag, line)):
            return False
        return all(_ok(app.send_line(command.format(half=n // 2), record=False)) for command in then)
    return setup


# What a probe's setup leaves for its operation and teardown (one probe runs at
# a time).
_state: dict[str, object] = {}


def _tabs_setup(app: q.App, root: Path, n: int) -> bool:
    """n small files open, each in its own tab; the timed open is the n+1st."""
    folder = root / "qa-scale" / f"tabs-{n}"
    for i in range(n + 1):
        _write(folder / f"f{i}.txt", [f"file {i}"])
    _state["next_tab"] = folder / f"f{n}.txt"
    _state["tabs"] = n + 1
    return all(_open(app, folder / f"f{i}.txt") for i in range(n))


def _tabs_teardown(app: q.App) -> None:
    for _ in range(int(_state.get("tabs", 0))):
        with contextlib.suppress(Exception):
            app.send_line("close-tab", record=False)


def _compare_setup(app: q.App, root: Path, n: int) -> bool:
    """Two files of n lines differing at every seventh: written, not yet shown."""
    folder = root / "qa-scale"
    _write(folder / f"cmp-{n}.left.txt", [f"row {i}" for i in range(n)])
    _write(folder / f"cmp-{n}.right.txt", [f"row {i}" + (" changed" if i % 7 == 0 else "") for i in range(n)])
    _state["compare"] = (folder / f"cmp-{n}.left.txt", folder / f"cmp-{n}.right.txt")
    return True


def _compare_open(app: q.App) -> None:
    left, right = _state["compare"]  # type: ignore[misc]
    app.send_line(f"compare-files {q.quote_arg(str(left))} {q.quote_arg(str(right))}", record=False)
    # The model is built off the main thread: the idle point does not cover it.
    deadline = time.monotonic() + 60 * q.SLOW_FACTOR
    while time.monotonic() < deadline:
        with contextlib.suppress(Exception):
            shown = app.query("compare")
            if shown.get("rightPath") == str(right) and len(shown.get("rows") or []) > 1:
                return
        time.sleep(0.01)


def _command(line: str) -> Callable[[q.App], None]:
    return lambda app: app.send_line(line, record=False)


PROBES: list[Probe] = [
    Probe("carets-type", 3000, "typing one character at every caret",
          _carets("type"), _command("type x"), _close),
    Probe("carets-keystroke", 3000, "one typed key at every caret",
          _carets("keystroke"), lambda app: app.type("x"), _close),
    Probe("carets-backspace", 3000, "one Backspace at every caret",
          _carets("backspace"), lambda app: app.key("BackSpace"), _close),
    Probe("lines-type", 60000, "typing one character in the middle of a file",
          _plain("lines", lambda i: f"{i:08d} a line of an ordinary length for a source file", ("goto {half}:1",)),
          _command("type x"), _close),
    Probe("lines-sort", 30000, "sorting every line of a file",
          _plain("sort", lambda i: f"{(i * 7919) % 100003:06d} sort me", ("select-all",)),
          _command("sort-lines-ascending"), _close),
    Probe("search-matches", 20000, "a search with a match on every line",
          _plain("search", lambda i: f"alpha {i} beta"), _command("search alpha"), _close),
    Probe("tabs-open", 120, "opening one more file beside the open tabs",
          _tabs_setup, lambda app: _open(app, Path(str(_state["next_tab"]))), _tabs_teardown),
    Probe("compare-rows", 20000, "opening a diff of two files",
          _compare_setup, _compare_open, _close),
]


def _measure(app: q.App, root: Path, probe: Probe, n: int) -> float | None:
    """The fastest of REPEATS timings at size n; None when the setup failed."""
    best: float | None = None
    for _ in range(REPEATS):
        try:
            if not probe.setup(app, root, n):
                probe.teardown(app)
                return None
            app.settle_quiet(most=10.0)
            started = time.monotonic()
            probe.operation(app)
            app.settle_quiet(most=60.0)
            elapsed = time.monotonic() - started
        except q.QaFailure:
            probe.teardown(app)
            return None
        probe.teardown(app)
        app.settle_quiet(most=10.0)
        best = elapsed if best is None else min(best, elapsed)
    return best


def _seconds(value: float | None) -> str:
    return "skipped" if value is None else f"{value:.3f}s"


def superlinear(small: float, large: float) -> bool:
    """Whether t(2N) = `large` against t(N) = `small` grew clearly faster than
    linear.

    >>> superlinear(0.10, 0.21)    # linear
    False
    >>> superlinear(0.10, 0.45)    # quadratic, and slow enough to trust
    True
    >>> superlinear(0.01, 0.05)    # quadratic, but in the noise
    False
    """
    return large >= MIN_SECONDS * q.SLOW_FACTOR and large >= RATIO * max(small, 1e-3)


def check(app: q.App, root: Path, probe: Probe) -> list[tuple[str, str]]:
    """The probe at N and 2N; a finding when it grew faster than linear twice."""
    n = probe.size
    small = _measure(app, root, probe, n)
    large = _measure(app, root, probe, 2 * n)
    # Printed whatever the verdict: the run's log then holds every probe's curve.
    print(f"scaling: {probe.name} {n}: {_seconds(small)}, {2 * n}: {_seconds(large)}", flush=True)
    if small is None or large is None or not superlinear(small, large):
        return []
    # Confirmed on a second pair: a slow sample on a loaded machine is not a finding.
    small_again = _measure(app, root, probe, n)
    large_again = _measure(app, root, probe, 2 * n)
    if small_again is None or large_again is None or not superlinear(small_again, large_again):
        return []
    small, large = min(small, small_again), min(large, large_again)
    return [("scaling", f"{probe.what} ({probe.name}) took {large:.2f}s at {2 * n} against {small:.2f}s at {n}: "
                        f"{large / max(small, 1e-3):.1f}x for twice the input (budget {RATIO:.0f}x)")]
