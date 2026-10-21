"""Shared helpers for the flow scenarios (flows_*.py).

A flow drives a whole user task -- type and save, find and replace, stage and
commit -- with real input, and checks each step against what the app reports
(`buffer`, `editor`, `ui`, the files on disk), not against pixels. The redraw
verifier and the crash/sanitizer oracles still run after every step (App.check).
"""

from __future__ import annotations

import subprocess
import time
from pathlib import Path

import microide_qa as q


def write(app: q.App, relative: str, content: str) -> Path:
    """Writes a fixture file (creating its directory)."""
    assert app.project is not None
    path = app.project / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return path


def read(app: q.App, relative: str) -> str:
    assert app.project is not None
    return (app.project / relative).read_text()


def open_file(app: q.App, relative: str, content: str | None = None) -> None:
    """Opens `relative` (writing `content` first when given) and waits until it
    is the active tab with its text loaded, then focuses the editor."""
    if content is not None:
        write(app, relative, content)
    app.send("open", relative)
    name = Path(relative).name
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == name
                   and app.buffer().get("available")
                   and app.buffer().get("path", "").endswith(relative),
                   5.0, f"{relative} to open")
    app.send("focus", "editor")
    app.settle(0.15)
    app.check(f"open {relative}")


def expect_text(app: q.App, expected: str, label: str, timeout: float = 2.0) -> bool:
    """Waits for the buffer to hold `expected`; records a finding naming the
    first difference otherwise."""
    deadline = time.monotonic() + timeout
    actual = ""
    while time.monotonic() < deadline:
        actual = app.text()
        if actual == expected:
            return True
        time.sleep(0.05)
    index = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                 min(len(actual), len(expected)))
    app.expect(False, f"{label}: buffer differs at byte {index}: "
                      f"got {actual[max(0, index - 20):index + 30]!r}, "
                      f"want {expected[max(0, index - 20):index + 30]!r}")
    return False


def expect_cursors(app: q.App, expected: list[tuple[int, int]], label: str) -> bool:
    actual = app.cursors()
    ok = actual == expected
    app.expect(ok, f"{label}: carets are {actual}, want {expected}")
    return ok


def indent_unit(app: q.App) -> str:
    """One indentation level in the active buffer (its soft-tabs setting)."""
    buffer = app.buffer()
    return " " * buffer.get("tabSize", 4) if buffer.get("softTabs") else "\t"


def step(app: q.App, label: str, settle: float = 0.12) -> None:
    """Lets the frame land, then runs the oracles."""
    app.settle(settle)
    app.check(label)


def dismiss(app: q.App) -> None:
    """Escapes anything modal (menu, overlay, prompt, settings)."""
    for _ in range(4):
        ui = app.ui()
        if not (ui["menu"]["open"] or ui["menu"]["contextMenu"] or ui["overlay"]["visible"]
                or ui.get("modal")):
            return
        app.key("Escape")
        step(app, "escape", 0.15)


# ---- shared readers ----------------------------------------------------------
# One copy each: these were redefined, byte for byte or nearly, in up to five
# flow modules.

def git_run(app: q.App, *args: str) -> subprocess.CompletedProcess:
    """`git` in the project, for a caller that needs the exit status."""
    assert app.project is not None
    return subprocess.run(["git", "-C", str(app.project), *args], capture_output=True, text=True, check=False)


def git(app: q.App, *args: str) -> str:
    """`git` in the project; its stdout without the trailing newline (and only
    that: a porcelain line's leading space is part of its status)."""
    return git_run(app, *args).stdout.rstrip("\n")


def titles(app: q.App, group: int | None = None) -> list[str]:
    """Every editor tab's title, in group order (or one group's)."""
    groups = app.editor_state()["groups"]
    return [tab["title"] for g in groups if group is None or g["index"] == group for tab in g["tabs"]]


def active_title(app: q.App) -> str | None:
    return (app.active_tab() or {}).get("title")


def tree_rows(app: q.App) -> list[str]:
    """The file tree's visible rows by element name (`row:tree/<label>`)."""
    return [e["name"] for e in app.elements("row", prefix="row:tree/")]


def terminal_tabs(app: q.App) -> list[dict]:
    terminals = app.query("terminals")
    return terminals if isinstance(terminals, list) else terminals.get("tabs", [])


def active_root(app: q.App) -> str:
    """The active project's root."""
    return next((p["root"] for p in app.query("projects") if p.get("active")), "")


def messages(app: q.App) -> list[str]:
    """Every notification's message, oldest first."""
    return [n.get("message", "") for n in app.query("notifications")]
