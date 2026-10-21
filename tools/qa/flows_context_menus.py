"""The context-menu sweep: right-click every surface that has a menu, and click
every enabled row of it, checking the ui action receipt names the row's command
and the menu closed. The menu-bar sweep (`menu-clicks`) found a row whose press
was taken by the sidebar under it; these menus float over the editor, the tabs,
the terminal and the tree, so each gets the same check. The instance runs with
MICROIDE_QA_ACTION_DRY_RUN=1: a row records the action it reached and runs
nothing (Delete deletes nothing, Close Others closes nothing).
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, open_file, step


def _right_click(app: q.App, point: Callable[[], tuple[float, float]]) -> Callable[[], None]:
    def open_menu() -> None:
        dismiss(app)
        app.click(*point(), button=3)
        app.settle(0.2)
    return open_menu


def _sweep(app: q.App, where: str, open_menu: Callable[[], None]) -> None:
    open_menu()
    ui = app.ui()
    app.expect(ui["menu"]["open"] or ui["menu"]["contextMenu"], f"right-clicking the {where} opens a menu")
    rows = [e for e in app.elements("menu-item") if e.get("enabled") is not False and e.get("command")]
    app.expect(bool(rows), f"the {where} menu has enabled rows")
    dismiss(app)
    for row in rows:
        open_menu()
        try:
            current = app.element(row["name"])
        except q.QaFailure:
            app.expect(False, f"{where}: {row['name']} is not on screen when the menu reopens")
            continue
        before = app.ui()["actions"]["dispatched"]
        app.click(*app.center(current))
        app.settle(0.15)
        app.check(f"{where}: click {row['name']}")
        after = app.ui()
        actions = after["actions"]
        app.expect(actions["dispatched"] == before + 1 and actions["last"] == row["command"],
                   f"{where}: clicking {row['name']} runs {row['command']!r} "
                   f"(ran {actions['last']!r}, {actions['dispatched'] - before} action(s))")
        app.expect(not after["menu"]["open"] and not after["menu"]["contextMenu"],
                   f"{where}: running {row['name']} closes the menu")
    dismiss(app)


def scenario_context_menus(app: q.App) -> None:
    """Right-click the editor text, an editor tab, the terminal, a terminal tab,
    a file / folder / the root / empty space in the tree, a git change and a
    search hit; click every enabled row of each menu."""
    assert app.project is not None
    (app.project / "src" / "main.cpp").write_text((app.project / "src" / "main.cpp").read_text() + "// x\n")
    open_file(app, "src/scheduler.cpp")
    if not app.ui()["panel"]["visible"]:
        app.send("term-toggle")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    # Views are switched by command: under the dry run a click on a view button
    # (a pointer-driven action) records its receipt and switches nothing.
    app.send("sidebar-show", "tree")
    step(app, "tree", 0.3)

    panel = lambda: app.ui()["surfaces"]["bottomPanel"]  # noqa: E731
    sidebar = lambda: app.ui()["surfaces"]["sidebar"]  # noqa: E731
    targets: list[tuple[str, Callable[[], tuple[float, float]]]] = [
        ("editor", lambda: app.text_point(9, 8)),
        ("editor tab", lambda: app.center(app.element("tab:0/scheduler.cpp"))),
        ("terminal", lambda: (panel()[0] + panel()[2] / 3, panel()[1] + panel()[3] / 2)),
        ("terminal tab", lambda: app.center(app.elements("panel-tab")[0])),
        ("tree file", lambda: app.center(app.element("row:tree/main.cpp"))),
        ("tree folder", lambda: app.center(app.element("row:tree/src"))),
        ("tree root", lambda: app.center(app.element("row:tree/taskflow"))),
        ("tree background", lambda: (sidebar()[0] + sidebar()[2] / 2, sidebar()[1] + sidebar()[3] - 30)),
    ]
    for where, point in targets:
        if where.startswith("tree"):
            app.send("sidebar-show", "tree")
            app.settle(0.2)
        _sweep(app, where, _right_click(app, point))

    app.send("sidebar-show", "git")
    app.wait_for("row:git/main.cpp", 8.0)
    _sweep(app, "git change", _right_click(app, lambda: app.center(app.element("row:git/main.cpp"))))

    app.send("project-search", "priority")
    app.wait_until(lambda: any(e["name"].startswith("row:search/src/scheduler.cpp:") for e in app.elements("row")),
                   6.0, "search results")
    _sweep(app, "search hit", _right_click(app, lambda: app.center(next(
        e for e in app.elements("row") if e["name"].startswith("row:search/src/scheduler.cpp:")))))


FLOWS: dict[str, Callable[[q.App], None]] = {"context-menus": scenario_context_menus}
ENV: dict[str, dict[str, str]] = {"context-menus": {"MICROIDE_QA_ACTION_DRY_RUN": "1"}}
