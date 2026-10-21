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


# ---- window size ---------------------------------------------------------------

def resize_window(app: q.App, width: int, height: int) -> None:
    """Resizes the X window (the Wayland backend's kiosk weston owns the size)."""
    if app.window:
        app._xdotool("windowsize", app.window, str(width), str(height))
    app.wait_until(lambda: app.ui()["window"]["width"] == width, 3.0, f"the window to be {width} wide")
    step(app, f"resize to {width}x{height}", 0.4)


def layout_sane(app: q.App, label: str) -> None:
    """Every element on screen is inside the window, has an area, and siblings of
    one strip (tabs, menu items, status segments) do not overlap."""
    ui = app.ui()
    width, height = ui["window"]["width"], ui["window"]["height"]
    by_kind: dict[str, list[dict]] = {}
    for element in ui["elements"]:
        x, y, w, h = element["rect"]
        app.expect(w > 0 and h > 0, f"{label}: {element['name']} has no area ({element['rect']})")
        app.expect(x >= -1 and y >= -1 and x + w <= width + 1 and y + h <= height + 1,
                   f"{label}: {element['name']} {element['rect']} leaves the {width}x{height} window")
        by_kind.setdefault(element["kind"], []).append(element)
    for kind in ("tab", "menu", "status", "panel-tab", "project-tab", "sidebar-view"):
        rects = by_kind.get(kind, [])
        for i, a in enumerate(rects):
            for b in rects[i + 1:]:
                ax, ay, aw, ah = a["rect"]
                bx, by, bw, bh = b["rect"]
                if ax < bx + bw - 1 and bx < ax + aw - 1 and ay < by + bh - 1 and by < ay + ah - 1:
                    app.expect(False, f"{label}: {a['name']} {a['rect']} overlaps {b['name']} {b['rect']}")


# ---- a merge conflict ------------------------------------------------------------

def open_conflict(app: q.App, relative: str = "notes/merge.txt") -> bool:
    """A file with two conflicts (lines 2 and 10: "line N incoming" against "line N
    current") from a real `git merge`, opened in the merge editor by
    review-conflicts. False (and a finding) when no merge tab opens."""
    base = "".join(f"line {i}\n" for i in range(1, 13))
    write(app, relative, base)
    git(app, "add", relative)
    git(app, "commit", "-q", "-m", "qa: merge base")
    git(app, "checkout", "-q", "-b", "qa-incoming")
    write(app, relative, base.replace("line 2\n", "line 2 incoming\n").replace("line 10\n", "line 10 incoming\n"))
    git(app, "commit", "-q", "-am", "qa: incoming side")
    git(app, "checkout", "-q", "-")
    write(app, relative, base.replace("line 2\n", "line 2 current\n").replace("line 10\n", "line 10 current\n"))
    git(app, "commit", "-q", "-am", "qa: current side")
    merged = git_run(app, "merge", "qa-incoming")
    app.expect(merged.returncode != 0, "the merge conflicts (the fixture is built to)")
    app.send("git-refresh")
    app.send("review-conflicts")
    if not app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "merge", 8.0, "a merge tab"):
        return False
    step(app, "merge tab", 0.6)
    return True
