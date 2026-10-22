"""Every scrollbar, dragged and clicked as a person does.

No flow touched a scrollbar: the editor's two, the sidebar's, the terminal's
scrollback, the command palette's list and the Settings pane's all went
unclicked, and the thumb drag paths behind them were unmeasured (TD-2026-10-18-364).
The `ui` query reports each one as `scrollbar:<surface>` (the track as its rect,
plus its `thumb`), from the geometry its paint and its drag both use.
"""

from __future__ import annotations

import time
from typing import Any, Callable

import microide_qa as q
from flows_common import dismiss, open_file, step, write


def _bar(app: q.App, name: str, timeout: float = 3.0) -> dict[str, Any] | None:
    """`scrollbar:<name>` once it is on screen, None if it is not within
    `timeout` (no finding: the caller's expect says what was missing)."""
    deadline = time.monotonic() + timeout
    while True:
        found = next((e for e in app.elements("scrollbar") if e["name"] == f"scrollbar:{name}"), None)
        if found is not None or time.monotonic() >= deadline:
            return found
        app.settle(0.1)


def _drag_thumb(app: q.App, name: str, to_end: bool) -> bool:
    """Drags `name`'s thumb to the end (or the start) of its track."""
    bar = _bar(app, name)
    if bar is None or "thumb" not in bar:
        return False
    x, y, w, h = bar["rect"]
    tx, ty, tw, th = bar["thumb"]
    x0, y0 = tx + tw / 2, ty + th / 2
    if bar.get("label") == "horizontal":
        app.drag(x0, y0, (x + w + tw) if to_end else (x - tw), y0)
    else:
        app.drag(x0, y0, x0, (y + h + th) if to_end else (y - th))
    step(app, f"drag {name} to the {'end' if to_end else 'start'}", 0.3)
    return True


def _editor(app: q.App) -> dict[str, Any]:
    return (app.ui().get("editors") or [{}])[0]


def _first_line(app: q.App) -> int:
    rows = _editor(app).get("rows") or [{}]
    return rows[0].get("line", 0)


def _last_line(app: q.App) -> int:
    rows = _editor(app).get("rows") or [{}]
    return rows[-1].get("line", 0)


def flow_scrollbars(app: q.App) -> None:
    """The editor: a click in the track below the thumb scrolls down, the thumb
    dragged to the end shows the last line and back to the start the first, the
    horizontal thumb scrolls sideways -- and none of it moves the caret, as in
    VS Code. Then the sidebar's, the terminal's scrollback, the command palette's
    and the Settings pane's, each dragged to its end and back."""
    assert app.project is not None
    lines = [f"row {i}" for i in range(1, 401)]
    lines[6] = "wide " + "w" * 400
    open_file(app, "notes/scroll.txt", "\n".join(lines) + "\n")
    app.key("ctrl+Home")
    step(app, "open a long, wide file", 0.3)

    bar = _bar(app, "editor/0")
    if app.expect(bar is not None and "thumb" in bar, "a long file has a vertical scrollbar"):
        x, y, w, h = bar["rect"]
        app.click(x + w / 2, y + h - 4)
        app.wait_until(lambda: _first_line(app) > 1, 3.0, "a click in the track to scroll")
        step(app, "click the track below the thumb", 0.3)
        app.expect(_first_line(app) > 1, f"a click below the thumb scrolls down (first line {_first_line(app)})")
        _drag_thumb(app, "editor/0", to_end=True)
        app.expect(_last_line(app) >= 399, f"the thumb at the end shows the last line (last {_last_line(app)})")
        _drag_thumb(app, "editor/0", to_end=False)
        app.expect(_first_line(app) == 1, f"the thumb at the start shows the first line (first {_first_line(app)})")
        app.expect(app.cursors()[:1] == [(1, 1)], f"scrolling moves the view, not the caret ({app.cursors()[:1]})")

    if app.expect(_bar(app, "editor/0/h") is not None, "a wide line gives a horizontal scrollbar"):
        _drag_thumb(app, "editor/0/h", to_end=True)
        column = _editor(app).get("scrollColumn", 0)
        app.expect(column > 0, f"the horizontal thumb dragged right scrolls sideways (column {column})")
        _drag_thumb(app, "editor/0/h", to_end=False)
        column = _editor(app).get("scrollColumn", 0)
        app.expect(column == 0, f"and dragged back shows column 1 again (column {column})")

    # The sidebar: enough files at the root to overflow the tree.
    for i in range(80):
        write(app, f"many_{i:02d}.txt", f"{i}\n")
    app.key("ctrl+shift+e")
    if app.expect(_bar(app, "sidebar", 8.0) is not None, "a long tree has a scrollbar"):
        def first_row() -> str:
            rows = app.elements("row")
            return rows[0]["name"] if rows else ""
        top = first_row()
        _drag_thumb(app, "sidebar", to_end=True)
        bottom = first_row()
        app.expect(bottom != top, f"the tree's thumb at the end scrolls the rows ({top} -> {bottom})")
        _drag_thumb(app, "sidebar", to_end=False)
        app.expect(first_row() == top, f"and back at the start shows the first row ({first_row()})")

    # The terminal's scrollback.
    if not app.ui()["panel"]["visible"]:
        app.key("ctrl+grave")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    app.send("focus", "panel")
    app.type("clear; seq 1 400\n")
    app.wait_until(lambda: "400" in app.query("terminal-output").get("text", ""), 5.0, "seq to print")
    step(app, "fill the scrollback", 0.4)
    if app.expect(_bar(app, "terminal/0") is not None, "a full scrollback has a scrollbar"):
        def first_row() -> int:
            return (app.ui().get("terminal") or {}).get("firstRow", -1)
        live = first_row()
        _drag_thumb(app, "terminal/0", to_end=False)
        app.expect(first_row() == 0, f"the thumb at the start shows the scrollback's top (row {first_row()})")
        _drag_thumb(app, "terminal/0", to_end=True)
        app.expect(first_row() == live, f"and at the end the live screen again (row {first_row()}, was {live})")

    # The command palette's list.
    app.send("focus", "editor")
    app.key("ctrl+shift+p")
    if app.expect(_bar(app, "overlay") is not None, "the command palette's long list has a scrollbar"):
        def first_item() -> str:
            rows = app.elements("overlay-row")
            return rows[0]["name"] if rows else ""
        top = first_item()
        _drag_thumb(app, "overlay", to_end=True)
        app.expect(first_item() != top, f"the palette's thumb at the end scrolls the list ({top} -> {first_item()})")
        _drag_thumb(app, "overlay", to_end=False)
        app.expect(first_item() == top, f"and back at the start shows the first command ({first_item()})")
    dismiss(app)

    # The Settings pane.
    app.key("ctrl+comma")
    if app.expect(_bar(app, "settings") is not None, "the Settings pane's long list has a scrollbar"):
        def first_setting() -> str:
            rows = app.elements("setting")
            return rows[0]["name"] if rows else ""
        top = first_setting()
        _drag_thumb(app, "settings", to_end=True)
        app.expect(first_setting() != top, f"the Settings thumb at the end scrolls the rows ({top} -> {first_setting()})")
        _drag_thumb(app, "settings", to_end=False)
        app.expect(first_setting() == top, f"and back at the start shows the first row ({first_setting()})")
    dismiss(app)
    app.check("scrollbars")


FLOWS: dict[str, Callable[[q.App], None]] = {"scrollbars": flow_scrollbars}
