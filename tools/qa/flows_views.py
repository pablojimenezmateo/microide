"""The Problems and Tests sidebar views, driven as a person drives them.

`lsp-diagnostics` checks only that the Problems view lists a row (and needs
clangd); nothing opened the Tests view at all. The coverage build of the suite
ran none of the selection, open or run paths behind either view (TD-2026-10-18-364).
The qa-api plugin publishes the problems, so this runs without a language server.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import active_title, open_file, step


def _rows(app: q.App, view: str) -> list[dict]:
    return [e for e in app.elements("row") if e["name"].startswith(f"row:{view}/")]


def _selected(app: q.App, view: str) -> str:
    row = next((e for e in _rows(app, view) if e.get("selected")), None)
    return row["name"].split("/", 1)[1] if row else ""


def _line(app: q.App) -> int:
    return app.cursors()[0][0] if app.cursors() else 0


def _problem_count(app: q.App) -> str:
    return app.element("status:problems").get("label", "") if any(
        e["name"] == "status:problems" for e in app.elements("status")) else ""


def flow_problems_tests_views(app: q.App) -> None:
    """Problems: three problems in two files list in the view; Down/Up move the
    selection, Enter opens the selected one at its line, a click opens another
    (in the other file), and clearing them empties the view and the status bar.
    Tests: discovery lists the active file's test, Enter opens it at its line, `r`
    runs it and the row shows it passed."""
    open_file(app, "other.qaapi", "other one\nother two\n")
    open_file(app, "notes.qaapi", "line one\nline two\nline three\nline four\n")
    reply = app.send("qa.api.problems")
    app.expect(bool(reply.get("ok")), f"the plugin publishes its problems ({reply})")
    app.wait_until(lambda: _problem_count(app) not in ("", "0"), 5.0, "the status bar to count the problems")
    app.key("ctrl+shift+m")
    step(app, "ctrl+shift+m", 0.4)
    app.expect(app.ui()["sidebar"]["view"] == "problems", f"Ctrl+Shift+M shows the Problems view ({app.ui()['sidebar']})")
    app.wait_until(lambda: len(_rows(app, "problems")) == 3, 5.0, "the three problems to list")
    rows = _rows(app, "problems")
    app.expect(len(rows) == 3, f"the view lists all three problems ({[r['name'] for r in rows]})")
    app.expect(any("notes.qaapi:2:1" in r.get("label", "") for r in rows),
               f"each row names its file and position ({[r.get('label') for r in rows]})")
    if len(rows) != 3:
        return

    # By keyboard: the first row is selected, Down moves, Up moves back.
    app.click(*app.center(rows[0]))
    step(app, "click the first problem", 0.3)
    first = _selected(app, "problems")
    app.send("focus", "sidebar")
    app.key("Down")
    step(app, "Down", 0.2)
    second = _selected(app, "problems")
    app.expect(second and second != first, f"Down moves the selection ({first!r} -> {second!r})")
    app.key("Up")
    step(app, "Up", 0.2)
    app.expect(_selected(app, "problems") == first, f"Up moves it back ({_selected(app, 'problems')!r})")

    # Enter opens the selected problem where it is.
    for target, title, line in (("qa warning on three", "notes.qaapi", 3), ("qa info in other", "other.qaapi", 1)):
        row = next((r for r in _rows(app, "problems") if r["name"] == f"row:problems/{target}"), None)
        if not app.expect(row is not None, f"the {target!r} row is listed"):
            continue
        app.click(*app.center(row))
        step(app, f"select {target}", 0.2)
        app.send("focus", "sidebar")
        app.key("Return")
        app.wait_until(lambda: active_title(app) == title, 3.0, f"Enter to open {title}")
        step(app, f"open {target}", 0.3)
        app.expect(active_title(app) == title and _line(app) == line,
                   f"Enter opens {target!r} at {title}:{line} ({active_title(app)}, {app.cursors()})")
        app.expect(app.ui().get("focus") == "editor", f"and the editor takes the keyboard ({app.ui().get('focus')})")

    reply = app.send("qa.api.problems.clear")
    app.wait_until(lambda: not _rows(app, "problems"), 5.0, "the cleared problems to leave the view")
    step(app, "problems cleared", 0.3)
    app.expect(not _rows(app, "problems"), f"clearing them empties the view ({[r['name'] for r in _rows(app, 'problems')]})")
    app.expect(_problem_count(app) in ("", "0"), f"and the status bar's count ({_problem_count(app)!r})")

    # Tests: discover in the active file, open, run.
    open_file(app, "notes.qaapi")
    reply = app.send("tests-discover")
    app.expect(bool(reply.get("ok")), f"tests-discover runs ({reply})")
    app.send("sidebar-show", "tests")
    app.wait_until(lambda: bool(_rows(app, "tests")), 5.0, "the discovered test to list")
    step(app, "the Tests view", 0.3)
    rows = _rows(app, "tests")
    if not app.expect(rows and rows[0]["name"] == "row:tests/QA case", f"the Tests view lists QA case ({[r['name'] for r in rows]})"):
        return
    app.click_text(4, 1)
    app.click(*app.center(rows[0]))
    step(app, "select QA case", 0.2)
    app.send("focus", "sidebar")
    app.key("Return")
    app.wait_until(lambda: app.ui().get("focus") == "editor", 3.0, "Enter to open the test")
    step(app, "open QA case", 0.3)
    app.expect(active_title(app) == "notes.qaapi" and _line(app) == 1,
               f"Enter opens the test at its line ({active_title(app)}, {app.cursors()})")
    app.click(*app.center(_rows(app, "tests")[0]))
    app.send("focus", "sidebar")
    app.key("r")
    app.wait_until(lambda: "passed" in (_rows(app, "tests") or [{}])[0].get("label", ""), 5.0, "the run to report")
    step(app, "run QA case", 0.3)
    label = (_rows(app, "tests") or [{}])[0].get("label", "")
    app.expect("passed" in label, f"r runs the selected test and its row shows it passed ({label!r})")


FLOWS: dict[str, Callable[[q.App], None]] = {"problems-tests-views": flow_problems_tests_views}
PLUGINS: dict[str, tuple[str, ...]] = {"problems-tests-views": ("qa-api",)}
