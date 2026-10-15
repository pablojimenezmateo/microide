"""The local GUI QA scenario suite.

Every scenario drives a fresh instance on a fresh copy of the taskflow fixture
(tools/capture-media/make-fixture.sh) by name through the `ui` query, and every
action is followed by the oracles (App.check): the redraw verifier, sanitizer and
allocator reports, the process staying up. Expectations are stated against `ui`
and the control queries, never against reference images.

The focus-sensitive scenarios (menus, overlays, sidebar views, chrome) run once
per focus -- editor, tree, terminal -- because the bugs this harness exists for
depended on what held focus (a workbench shortcut dead from the tree, a caret
frozen while the terminal blinked).
"""

from __future__ import annotations

import time
from typing import Callable

import microide_qa as q

FOCUSES = ("editor", "sidebar", "panel")


def _open_source(app: q.App) -> None:
    app.send("open", "src/scheduler.cpp")
    app.wait_until(lambda: any(e["name"].endswith("/scheduler.cpp")
                               for e in app.elements("tab")), 5.0, "scheduler.cpp's tab")
    app.check("open src/scheduler.cpp")


def _focus(app: q.App, focus: str) -> None:
    if focus == "panel":
        # A live terminal in the panel, so focus means the terminal.
        if not app.ui().get("panel", {}).get("visible"):
            app.key("ctrl+grave")
            app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal panel")
    app.send("focus", focus)
    app.settle(0.2)
    app.check(f"focus {focus}")


def _escape_everything(app: q.App) -> None:
    for _ in range(3):
        ui = app.ui()
        if not (ui["menu"]["open"] or ui["menu"]["contextMenu"] or ui["overlay"]["visible"]
                or ui.get("modal")):
            return
        app.key("Escape")
        app.settle(0.15)
        app.check("escape")


def scenario_menus(app: q.App) -> None:
    """Open every menu-bar menu and hover every enabled row, under each focus."""
    _open_source(app)
    for focus in FOCUSES:
        _focus(app, focus)
        for menu in [e["name"] for e in app.elements("menu")]:
            app.click_named(menu)
            app.settle(0.2)
            app.check(f"{focus}: open {menu}")
            label = menu.split(":", 1)[1]
            app.expect(app.ui()["menu"]["menu"] == label, f"{menu} opens its menu")
            for row in app.elements("menu-item", prefix=f"menu-item:{label}/"):
                if row.get("enabled") is False:
                    continue
                app.hover_named(row["name"])
                app.settle(0.06)
                app.check(f"{focus}: hover {row['name']}")
            app.key("Escape")
            app.settle(0.15)
            app.check(f"{focus}: close {menu}")
        _escape_everything(app)


def scenario_overlays(app: q.App) -> None:
    """Open and dismiss every quick-open overlay and the find widgets."""
    _open_source(app)
    for focus in FOCUSES:
        _focus(app, focus)
        for command in ("files", "command-palette"):
            app.send(command)
            app.settle(0.3)
            app.check(f"{focus}: {command}")
            app.expect(app.ui()["overlay"]["visible"], f"{command} shows an overlay")
            app.type("sched")
            app.settle(0.3)
            app.check(f"{focus}: type in {command}")
            app.key("Down")
            app.settle(0.1)
            app.check(f"{focus}: Down in {command}")
            app.key("Escape")
            app.settle(0.2)
            app.check(f"{focus}: dismiss {command}")
            app.expect(not app.ui()["overlay"]["visible"], f"Escape dismisses {command}")
        # Find in files is the search VIEW (VS Code's), not an overlay.
        app.send("project-search")
        app.settle(0.3)
        app.check(f"{focus}: project-search")
        app.expect(app.ui()["sidebar"]["view"] == "search", "project-search shows the search view")
        app.type("queue")
        app.settle(0.4)
        app.check(f"{focus}: type in the search view")
        app.key("Escape")
        app.settle(0.2)
        app.check(f"{focus}: Escape from the search view")
    _focus(app, "editor")
    for chord in ("ctrl+f", "ctrl+h"):
        app.key(chord)
        app.settle(0.3)
        app.check(chord)
        app.type("queue")
        app.settle(0.3)
        app.check(f"type after {chord}")
        app.key("Escape")
        app.settle(0.2)
        app.check(f"Escape after {chord}")


def scenario_overlay_backdrop(app: q.App) -> None:
    """The compare (commit) picker over a live, modified editor, held open while
    frames go by -- the modal-backdrop stacking repro (formerly
    tools/repro/overlay-backdrop-repro.sh): the editor behind it must stay dimmed
    but readable, which the redraw verifier now checks frame by frame."""
    path = app.project / "src" / "scheduler.cpp" if app.project else None
    if path is not None and path.exists():
        text = path.read_text()
        path.write_text(text.replace("queue_.push_back", "// tweak\n    queue_.push_back", 1))
    _open_source(app)
    for command in (("compare", "src/scheduler.cpp"), ("command-palette",)):
        app.send(*command)
        for index in range(6):
            time.sleep(0.35)
            app.check(f"{' '.join(command)} held {index}")
        app.key("Escape")
        app.settle(0.3)
        app.check(f"dismiss {' '.join(command)}")


def scenario_sidebar_views(app: q.App) -> None:
    """Every sidebar view button, the overflow views, and a scroll of each list."""
    _open_source(app)
    for focus in FOCUSES:
        _focus(app, focus)
        for view in [e["name"] for e in app.elements("sidebar-view")]:
            app.click_named(view)
            app.settle(0.3)
            app.check(f"{focus}: {view}")
            app.expect(app.ui()["sidebar"]["view"] == view.split(":", 1)[1], f"{view} shows")
            surface = app.ui()["surfaces"]["sidebar"]
            if surface:
                x, y, w, h = surface
                app.wheel(x + w / 2, y + h / 2, 3)
                app.settle(0.15)
                app.check(f"{focus}: scroll {view}")
                app.wheel(x + w / 2, y + h / 2, -3)
                app.settle(0.15)
                app.check(f"{focus}: scroll back {view}")
        app.click_named("sidebar-view:tree")
        app.settle(0.2)


def scenario_chrome(app: q.App) -> None:
    """Hover and click every editor tab, panel tab, tree row and status segment."""
    _open_source(app)
    app.send("open", "src/main.cpp")
    app.settle(0.3)
    _focus(app, "panel")
    for focus in FOCUSES:
        _focus(app, focus)
        for kind in ("tab", "panel-tab", "row", "status"):
            for element in app.elements(kind):
                # A click earlier in the walk (a folder row collapsing, a status
                # segment opening a picker) can take the next element off screen.
                try:
                    app.hover_named(element["name"])
                except q.QaFailure:
                    continue
                app.settle(0.06)
                app.check(f"{focus}: hover {element['name']}")
                if kind == "status" and not element.get("enabled"):
                    continue
                try:
                    app.click_named(element["name"])
                except q.QaFailure:
                    continue
                app.settle(0.15)
                app.check(f"{focus}: click {element['name']}")
                _escape_everything(app)


def scenario_dividers(app: q.App) -> None:
    """Drag the sidebar, panel and editor-split dividers both ways."""
    _open_source(app)
    _focus(app, "panel")
    app.send("split-right")
    app.settle(0.3)
    app.check("split-right")
    for divider in [e["name"] for e in app.elements("divider")]:
        element = app.element(divider)
        x, y = app.center(element)
        vertical = element["rect"][2] < element["rect"][3]
        for delta in (60, -100, 40):
            dx, dy = (delta, 0) if vertical else (0, -delta)
            app.drag(x, y, x + dx, y + dy)
            app.settle(0.2)
            app.check(f"drag {divider} by {delta}")
            element = app.element(divider)
            x, y = app.center(element)


def scenario_split_and_close(app: q.App) -> None:
    """Split the editor both ways, type in each pane, close the splits."""
    _open_source(app)
    for split in ("split-right", "split-down"):
        app.send(split)
        app.settle(0.3)
        app.check(split)
        app.expect(any(e["name"].startswith("tab:1/") for e in app.elements("tab")),
                   f"{split} makes a second group")
        app.type("x")
        app.settle(0.15)
        app.check(f"type after {split}")
        app.key("ctrl+z")
        app.settle(0.15)
        app.check(f"undo after {split}")
        app.send("close-group")
        app.settle(0.3)
        app.check(f"close-group after {split}")


def scenario_terminal(app: q.App) -> None:
    """Open the terminal panel, type in it, split it, maximize it, close it."""
    _open_source(app)
    _focus(app, "panel")
    app.type("echo qa-harness\n")
    app.settle(0.6)
    app.check("type in the terminal")
    for step in ("panel/maximize", "panel/maximize", "panel/new-terminal"):
        try:
            app.click_named(f"button:{step}")
        except q.QaFailure:
            continue
        app.settle(0.4)
        app.check(f"click {step}")
    app.send("term-close")
    app.settle(0.4)
    app.check("term-close")
    app.key("ctrl+grave")
    app.settle(0.4)
    app.check("toggle the panel")


def scenario_scrolling(app: q.App) -> None:
    """Scroll the editor and the sidebar by wheel and keyboard."""
    _open_source(app)
    _focus(app, "editor")
    surface = app.ui()["surfaces"]["editorSurface"]
    x, y, w, h = surface
    for notches in (5, 5, -3, -10):
        app.wheel(x + w / 2, y + h / 2, notches)
        app.settle(0.12)
        app.check(f"editor wheel {notches}")
    for chord in ("ctrl+End", "ctrl+Home", "Next", "Prior"):
        app.key(chord)
        app.settle(0.12)
        app.check(f"editor {chord}")


SCENARIOS: dict[str, Callable[[q.App], None]] = {
    "menus": scenario_menus,
    "overlays": scenario_overlays,
    "overlay-backdrop": scenario_overlay_backdrop,
    "sidebar-views": scenario_sidebar_views,
    "chrome": scenario_chrome,
    "dividers": scenario_dividers,
    "split-and-close": scenario_split_and_close,
    "terminal": scenario_terminal,
    "scrolling": scenario_scrolling,
}
