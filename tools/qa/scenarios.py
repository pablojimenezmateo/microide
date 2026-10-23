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
                # The row under the pointer is the one the menu highlights: a hit
                # test that disagreed with the painted rows would show here.
                app.expect(app.element(row["name"]).get("active") is True,
                           f"{focus}: hovering {row['name']} highlights it")
            app.key("Escape")
            app.settle(0.15)
            app.check(f"{focus}: close {menu}")
        _escape_everything(app)


# Layouts the click sweep repeats in, each with the sidebar views it walks: each
# moves the chrome under different menu rows (with the project strip shown, File's
# fourth row lies over the Search button -- the click-through fixed in 923aecee
# needed exactly that one non-default setting). Every layout sets every key the
# matrix uses, so no layout inherits another's.
_MATRIX_DEFAULTS = (("chrome.project_tabs.hide_when_single", "true"), ("ui.layout_mode", "auto"),
                    ("ui.scale", "1"), ("ui.show_status_bar", "true"), ("sidebar.file_icons", "false"))


def _layout(**overrides: str) -> tuple[tuple[str, str], ...]:
    return tuple((key, overrides.get(key.replace(".", "_"), value)) for key, value in _MATRIX_DEFAULTS)


MENU_CLICK_LAYOUTS: tuple[tuple[str, tuple[tuple[str, str], ...], tuple[str, ...]], ...] = (
    ("default", _layout(), ("tree", "search", "git")),
    ("project strip", _layout(chrome_project_tabs_hide_when_single="false"), ("tree", "search", "git")),
    ("compact", _layout(ui_layout_mode="compact"), ("tree",)),
    ("scaled, no status bar, file icons",
     _layout(ui_scale="1.5", ui_show_status_bar="false", sidebar_file_icons="true"), ("tree", "git")),
)


def _click_rows(app: q.App, where: str, reopen: Callable[[], None], owner_prefix: str) -> None:
    """Reopen the menu, click one row, and check the receipt names that row, for
    every enabled row the menu shows."""
    reopen()
    rows = [(row["name"], row["label"]) for row in app.elements("menu-item", prefix=owner_prefix)
            if row.get("enabled") is not False]
    app.key("Escape")
    app.settle(0.1)
    for name, label in rows:
        reopen()
        try:
            row = app.element(name)
        except q.QaFailure:
            app.expect(False, f"{where}: {name} is not on screen when its menu reopens")
            _escape_everything(app)
            continue
        before = app.ui()["menu"]["rowsRun"]
        app.click(*app.center(row))
        app.settle(0.12)
        app.check(f"{where}: click {name}")
        menu = app.ui()["menu"]
        if menu["open"] and menu.get("submenu"):
            # A submenu row opens its submenu; its own rows get the same sweep.
            submenu = menu["submenu"]
            app.key("Escape")
            app.settle(0.1)
            _click_rows(app, f"{where} > {label}",
                        lambda: (reopen(), app.hover_named(name), app.settle(0.15)),
                        f"menu-item:{submenu}/")
            continue
        locator = name.split(":", 1)[1]
        app.expect(menu["rowsRun"] == before + 1 and menu["lastRun"] == locator,
                   f"{where}: clicking {name} runs it (ran {menu['lastRun']!r}, "
                   f"{menu['rowsRun'] - before} row(s))")
        app.expect(not menu["open"], f"{where}: running {name} closes its menu")
        _escape_everything(app)


def scenario_menu_clicks(app: q.App) -> None:
    """Click every row of every menu with the pointer, in each layout, and check
    the click ran that row (the `ui` receipt) and not a surface beneath the menu.
    The app runs with MICROIDE_QA_MENU_DRY_RUN=1 (SCENARIO_ENV), so a row records
    its run and closes the menu without quitting, closing the project or opening a
    native picker."""
    _open_source(app)
    for layout, settings, views in MENU_CLICK_LAYOUTS:
        for key, value in settings:
            reply = app.send("set-setting", key, value)
            app.expect(bool(reply.get("ok")), f"{layout}: set-setting {key} {value} ({reply})")
        app.settle(0.3)
        app.check(f"layout {layout}")
        for view in views:
            if not any(e["name"] == f"sidebar-view:{view}" for e in app.elements("sidebar-view")):
                app.expect(False, f"{layout}: no {view} view button ({[e['name'] for e in app.elements('sidebar-view')]})")
                continue
            app.click_named(f"sidebar-view:{view}")
            app.settle(0.2)
            menus = [e["name"] for e in app.elements("menu")]
            for menu in menus:
                label = menu.split(":", 1)[1]
                _click_rows(app, f"{layout}, {view} view, {label}",
                            lambda: (app.click_named(menu), app.settle(0.15)),
                            f"menu-item:{label}/")
            # The menus the bar could not fit sit behind its chevron (every menu, in
            # compact mode): open the popup, pick the menu, then sweep its rows the
            # same way. A layout with neither menus nor a chevron swept nothing and
            # passed, which is what the compact layout did until 2026-10-20.
            chevron = any(e["name"] == "button:menu-bar/more" for e in app.elements("button"))
            app.expect(menus or chevron, f"{layout}: the menu bar shows its menus or a chevron to them")
            if chevron:
                app.click_named("button:menu-bar/more")
                app.settle(0.2)
                app.check(f"{layout}: open the menu chevron")
                app.expect(app.ui()["menu"]["overflow"], f"{layout}: the chevron opens the overflow popup")
                folded = [e["label"] for e in app.elements("menu-item", prefix="menu-item:more/")]
                app.key("Escape")
                app.settle(0.1)
                app.expect(len(folded) + len(menus) >= 8, f"{layout}: every menu is reachable ({menus} + {folded})")
                for label in folded:
                    def reopen(label: str = label) -> None:
                        app.click_named("button:menu-bar/more")
                        app.settle(0.15)
                        app.click_named(f"menu-item:more/{label}")
                        app.settle(0.15)
                    reopen()
                    app.expect(app.ui()["menu"]["menu"] == label, f"{layout}: the popup's {label} row opens that menu")
                    app.key("Escape")
                    app.settle(0.1)
                    _click_rows(app, f"{layout}, {view} view, {label} (via chevron)", reopen, f"menu-item:{label}/")
            # The sidebar's views menu, anchored on its button over the tree.
            if any(e["name"] == "button:sidebar/more" for e in app.elements("button")):
                _click_rows(app, f"{layout}, {view} view, sidebar views menu",
                            lambda: (app.click_named("button:sidebar/more"), app.settle(0.15)),
                            "menu-item:")
        app.click_named("sidebar-view:tree")
        app.settle(0.2)


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


import importlib  # noqa: E402
from pathlib import Path  # noqa: E402

# The flow modules, in suite order. Each exports FLOWS ({name: f(app)}) and may
# export PLUGINS ({name: plugins its instance starts with: bundled, plugins/<name>,
# or QA-only, tools/qa/plugins/<name>}) and ENV ({name: environment beyond
# App.launch's}). A flows_*.py missing here, or a scenario name two modules
# define, fails the import instead of silently dropping a flow.
FLOW_MODULES: tuple[str, ...] = (
    "flows_editing", "flows_workbench", "flows_lsp_debug", "flows_keys", "flows_context_menus",
    "flows_more", "flows_advanced", "flows_coverage", "flows_ux", "flows_depth", "flows_resilience",
    "flows_matrix", "flows_languages", "flows_integration", "flows_plugin_api", "flows_commands",
    "flows_settings", "flows_inputs", "flows_surfaces", "flows_panes", "flows_tree", "flows_effects", "flows_lifecycle", "flows_diff", "flows_debugger", "flows_layout",
    "flows_views", "flows_scrollbars", "flows_files", "flows_tabs", "flows_editor_views", "flows_review", "flows_actions", "flows_interactions", "flows_journeys", "flows_edit_transactions", "flows_replace_matrix", "flows_format_transactions",
)

_on_disk = {path.stem for path in Path(__file__).resolve().parent.glob("flows_*.py")} - {"flows_common"}
if _on_disk != set(FLOW_MODULES):
    raise ImportError(f"scenarios.FLOW_MODULES and tools/qa/flows_*.py disagree: "
                      f"unlisted {sorted(_on_disk - set(FLOW_MODULES))}, missing {sorted(set(FLOW_MODULES) - _on_disk)}")

SCENARIOS: dict[str, Callable[[q.App], None]] = {
    "menus": scenario_menus,
    "menu-clicks": scenario_menu_clicks,
    "overlays": scenario_overlays,
    "overlay-backdrop": scenario_overlay_backdrop,
    "sidebar-views": scenario_sidebar_views,
    "chrome": scenario_chrome,
    "dividers": scenario_dividers,
    "split-and-close": scenario_split_and_close,
    "terminal": scenario_terminal,
    "scrolling": scenario_scrolling,
}
SCENARIO_PLUGINS: dict[str, tuple[str, ...]] = {}
SCENARIO_ENV: dict[str, dict[str, str]] = {"menu-clicks": {"MICROIDE_QA_MENU_DRY_RUN": "1"}}

def _redefined_top_level_names(path: Path) -> list[str]:
    """Functions or classes a module defines twice at its top level: the second
    silently replaces the first for every caller (a `_palette` helper returning
    None replaced one returning bool, and `if _palette(...)` in another flow
    stopped adding its breakpoint)."""
    import ast
    seen: set[str] = set()
    twice: list[str] = []
    for node in ast.parse(path.read_text()).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            if node.name in seen:
                twice.append(node.name)
            seen.add(node.name)
    return twice


for _module_name in FLOW_MODULES:
    _twice = _redefined_top_level_names(Path(__file__).resolve().parent / f"{_module_name}.py")
    if _twice:
        raise ImportError(f"{_module_name} defines {', '.join(_twice)} twice at its top level")
    _module = importlib.import_module(_module_name)
    for _name, _flow in _module.FLOWS.items():
        if _name in SCENARIOS:
            raise ImportError(f"scenario {_name!r} is defined twice ({_module_name})")
        SCENARIOS[_name] = _flow
    SCENARIO_PLUGINS.update(getattr(_module, "PLUGINS", {}))
    SCENARIO_ENV.update(getattr(_module, "ENV", {}))

# Scenarios a backend cannot drive, with why. The Wayland backend delivers input
# through the control channel at SDL's event level, not the seat: no input serial
# reaches the compositor, so it refuses the clipboard selection (copy and paste
# between the editor, the shell and the compare view), and kiosk weston owns the
# window size. The suite reports these as skipped, not as passed.
UNSUPPORTED: dict[str, dict[str, str]] = {
    "wayland": {
        name: "the clipboard needs a seat input serial, which injected input has none of"
        for name in ("edit-selection-clipboard", "compare-clipboard", "terminal-clipboard", "undo-semantics",
                     "terminal-selection")
    } | {name: "kiosk weston owns the window size" for name in ("wb-layouts", "resize-overlays")},
}

