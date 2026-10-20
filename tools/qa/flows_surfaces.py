"""Surfaces the sweeps touch only by hover: commands with their arguments, the
search view by keyboard, the bottom panel by mouse, the overview ruler, a diff
with soft wrap.

The command sweep runs every verb bare, so the argument parsers
(`WorkspaceCommandParsing`, the command-line coordinator) ran at a fifth in the
coverage build (TD-2026-10-18-364); `commands-args` runs each verb that takes an
argument with one, and reads the effect back. The others drive what a coverage
build showed untouched: the search view's keys, the panel's tabs, dividers and
maximize, the ruler's click, `DiffWrapLayout`.
"""

from __future__ import annotations

import subprocess
from typing import Callable

import microide_qa as q
from flows_common import dismiss, open_file, read, step, write


def _cursor(app: q.App) -> tuple[int, int]:
    cursors = app.cursors()
    return cursors[0] if cursors else (0, 0)


def _setting(app: q.App, setting: str) -> str | None:
    return next((r["value"] for r in app.query("settings") if r["id"] == setting), None)


def _group_count(app: q.App) -> int:
    return len({t.get("group") for t in app.query("tabs")})


def _run(app: q.App, line: str, check: Callable[[], bool] | None, label: str, timeout: float = 3.0) -> None:
    reply = app.send_line(line)
    if not app.expect(bool(reply.get("ok")), f"`{line}` runs ({reply})"):
        return
    if check is not None:
        app.wait_until(check, timeout, f"`{line}`: {label}")
    step(app, line, 0.15)


def flow_commands_args(app: q.App) -> None:
    """Each argument-taking command with an argument, and its effect."""
    assert app.project is not None
    open_file(app, "src/scheduler.cpp")
    _run(app, "goto 8:6", lambda: _cursor(app) == (8, 6), "the caret at 8:6")
    _run(app, "jump +2", lambda: _cursor(app)[0] == 10, "two lines down")
    _run(app, "jump -3", lambda: _cursor(app)[0] == 7, "three lines up")
    _run(app, "jump 2:1", lambda: _cursor(app) == (9, 1), "a relative jump with a column")
    reply = app.send_line("jump abc")
    app.expect(not reply.get("ok") and bool(reply.get("error")), f"`jump abc` is refused with why ({reply})")
    _run(app, "tab-size 8", lambda: app.buffer().get("tabSize") == 8, "a tab size of 8")
    _run(app, "tab-size 4", lambda: app.buffer().get("tabSize") == 4, "back to 4")
    _run(app, "soft-tabs on", lambda: app.buffer().get("softTabs") is True, "soft tabs")
    _run(app, "soft-tabs off", lambda: app.buffer().get("softTabs") is False, "hard tabs")
    _run(app, "wrap on", lambda: app.buffer().get("softWrap") is True, "soft wrap")
    _run(app, "wrap off", lambda: app.buffer().get("softWrap") is False, "no wrap")
    _run(app, "indent-width 2", lambda: _setting(app, "editor.indent_width") == "2", "an indent width of 2")
    _run(app, "indent-width 4", None, "")
    _run(app, "type hello-args", lambda: "hello-args" in app.text(), "the typed text in the buffer")
    _run(app, "key ctrl+z 1", lambda: "hello-args" not in app.text(), "the typing undone")
    _run(app, "set-setting editor.tab_size 6", lambda: _setting(app, "editor.tab_size") == "6", "the setting")
    _run(app, "set-setting editor.tab_size 4", None, "")
    _run(app, "colorscheme light", None, "")
    reply = app.send_line("colorscheme no-such-scheme")
    app.expect(not reply.get("ok") and "no-such-scheme" in str(reply.get("error", "")),
               f"an unknown colour scheme is refused by name ({reply})")
    _run(app, "colorscheme default", None, "")

    _run(app, "open src/main.cpp", lambda: (app.active_tab() or {}).get("title") == "main.cpp", "main.cpp active")
    _run(app, "tab notes/fresh.txt", lambda: (app.active_tab() or {}).get("title") == "fresh.txt"
         and app.buffer().get("text") == "", "an empty buffer bound to the new path")
    _run(app, "tabswitch main.cpp", lambda: (app.active_tab() or {}).get("title") == "main.cpp", "main.cpp by name")
    _run(app, "tabswitch +1", lambda: (app.active_tab() or {}).get("title") != "main.cpp", "the next tab")
    _run(app, "tabswitch -1", lambda: (app.active_tab() or {}).get("title") == "main.cpp", "back")
    _run(app, "tabmove 1", lambda: [t["title"] for t in sorted(app.query("tabs"), key=lambda t: t.get("index", 0))][0]
         == "main.cpp", "the active tab moved to the first slot")
    _run(app, "tabmove +1", lambda: [t["title"] for t in sorted(app.query("tabs"), key=lambda t: t.get("index", 0))][1]
         == "main.cpp", "and one slot right")
    _run(app, "split-right src/scheduler.cpp", lambda: _group_count(app) == 2
         and (app.active_tab() or {}).get("title") == "scheduler.cpp", "a second group showing scheduler.cpp")
    _run(app, "focus-group 1", lambda: app.pane().get("group") == 0, "the first group focused (groups count from 1)")
    _run(app, "tab-to-group 2", lambda: any(t.get("group") == 1 and t["title"] == "main.cpp" for t in app.query("tabs")),
         "main.cpp moved to the second group")
    _run(app, "close-group", lambda: _group_count(app) == 1, "one group again")
    _run(app, "reveal src/main.cpp 3:2", lambda: (app.active_tab() or {}).get("title") == "main.cpp"
         and _cursor(app) == (3, 2), "main.cpp at 3:2")

    _run(app, "sidebar-show git", lambda: app.ui()["sidebar"]["view"] == "git", "the git view")
    _run(app, "sidebar-toggle search", lambda: app.ui()["sidebar"]["view"] == "search", "the search view")
    _run(app, "sidebar-show tree", lambda: app.ui()["sidebar"]["view"] == "tree", "the tree")
    before = app.ui()["surfaces"]["sidebar"][2]
    _run(app, "sidebar-width 340", lambda: abs(app.ui()["surfaces"]["sidebar"][2] - 340) <= 2, "a 340 wide sidebar")
    _run(app, f"sidebar-width {int(before)}", None, "")
    _run(app, "find sched", lambda: app.ui()["overlay"]["visible"] and app.ui()["overlay"].get("mode") == "file-finder"
         and app.ui()["overlay"].get("query") == "sched", "the file finder seeded")
    app.key("Escape")
    _run(app, "project-search queue", lambda: app.ui()["sidebar"]["view"] == "search"
         and app.field_text("field:search/query") == "queue", "the search view seeded")
    app.key("Escape")
    # (`files <root>` opens that root as a project: the project flows own it.)

    if not app.ui()["panel"]["visible"]:
        app.key("ctrl+grave")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    _run(app, "focus panel", lambda: app.ui()["focus"] == "panel", "the panel focused")
    _run(app, "term echo args-term-$((40+2))", lambda: "args-term-42" in app.query("terminal-output").get("text", ""),
         "the command's output", 8.0)
    _run(app, "focus editor", lambda: app.ui()["focus"] == "editor", "the editor focused")
    channels = app.query("output").get("channels", [])
    if channels:
        _run(app, f"show-output {channels[0]['id']}", lambda: app.ui()["panel"]["content"] == "output", "the output panel")
    _run(app, "ui-scale up", lambda: app.ui()["window"]["scale"][0] > 1.0, "a larger scale")
    _run(app, "ui-scale reset", lambda: app.ui()["window"]["scale"][0] == 1.0, "scale 1")

    write(app, "notes/base.txt", "a\nb\nc\n")
    write(app, "notes/incoming.txt", "a\nb2\nc\n")
    write(app, "notes/current.txt", "a\nb\nc3\n")
    _run(app, "compare-files notes/base.txt notes/incoming.txt",
         lambda: (app.active_tab() or {}).get("kind") == "compare", "a compare tab", 6.0)
    _run(app, "merge notes/base.txt notes/incoming.txt notes/current.txt notes/merged.txt",
         lambda: (app.active_tab() or {}).get("kind") == "merge", "a merge tab", 6.0)
    _run(app, "review-commit HEAD", lambda: (app.active_tab() or {}).get("kind") == "compare", "a commit review", 8.0)
    open_file(app, "notes/tosave.txt", "saved elsewhere\n")
    _run(app, "save notes/copy-of.txt", lambda: (app.project / "notes" / "copy-of.txt").exists()
         and read(app, "notes/copy-of.txt") == "saved elsewhere\n", "the buffer written under the given path")
    open_file(app, "notes/data.json", '{"a":1,"b":[1,2]}')
    _run(app, "format-json notes/data.json", lambda: "\n" in app.text() and '"a": 1' in app.text(), "the JSON formatted")

    readme = app.project / "README.md"
    readme.write_text(readme.read_text() + "\nstash me\n")
    _run(app, "git-stash wip-args", lambda: "stash me" not in readme.read_text(), "the change stashed", 8.0)
    stashes = subprocess.run(["git", "stash", "list"], cwd=app.project, capture_output=True, text=True).stdout
    app.expect("wip-args" in stashes, f"the stash carries the message ({stashes.strip()!r})")
    _run(app, "git-switch-branch feature/stable-sort",
         lambda: "feature/stable-sort" in next((e.get("label", "") for e in app.elements("status")
                                                if e["name"] == "status:project"), ""), "the branch switched", 8.0)
    _run(app, "git-switch-branch main", None, "", 8.0)
    app.send("set-setting", "debug.enabled", "true")
    def breakpoint_lines() -> list[int]:
        return [b["line"] for f in app.query("breakpoints") for b in f.get("breakpoints", [])]

    _run(app, "breakpoint-set src/main.cpp 3", lambda: 3 in breakpoint_lines(), "a breakpoint on line 3")
    _run(app, "breakpoint-clear src/main.cpp", lambda: not breakpoint_lines(), "no breakpoints")
    dismiss(app)


# ---- the search view by keyboard ----------------------------------------------

def _search_rows(app: q.App) -> list[dict]:
    return app.elements("row", prefix="row:search/")


def _selected_search_row(app: q.App) -> str | None:
    return next((r["name"] for r in _search_rows(app) if r.get("selected")), None)


def _active_field(app: q.App) -> str | None:
    return next((f["name"] for f in app.elements("field") if f.get("active")), None)


def flow_search_view_keys(app: q.App) -> None:
    """The search view driven by keys: Down/Up and j/k over the results, Enter
    opening one at its line, `/` `=` `i` `x` jumping to a field, Tab cycling the
    fields, `c` toggling the exact count, Escape leaving the view."""
    open_file(app, "src/scheduler.cpp")
    app.send("project-search", "queue")
    app.wait_until(lambda: len(_search_rows(app)) >= 2, 8.0, "search results")
    step(app, "project-search queue", 0.3)
    app.key("Escape")  # leave the query field for the list
    app.settle(0.15)
    app.expect(app.ui()["focus"] == "sidebar", f"Escape from the field keeps the view ({app.ui()['focus']})")
    first = _selected_search_row(app)
    app.key("Down")
    step(app, "Down", 0.15)
    second = _selected_search_row(app)
    app.expect(second is not None and second != first, f"Down moves the selection ({first} -> {second})")
    app.key("k")
    step(app, "k", 0.15)
    app.expect(_selected_search_row(app) == first, f"k moves it back up ({_selected_search_row(app)})")
    app.key("j")
    step(app, "j", 0.15)
    app.expect(_selected_search_row(app) == second, "j moves down")
    app.key("Up")
    step(app, "Up", 0.15)
    app.expect(_selected_search_row(app) == first, "Up moves up")
    app.key("End")
    step(app, "End", 0.15)
    last = _selected_search_row(app)
    app.expect(last == _search_rows(app)[-1]["name"], f"End selects the last visible result ({last})")
    app.key("Home")
    step(app, "Home", 0.15)
    app.expect(_selected_search_row(app) == first, "Home selects the first")
    app.key("Down")
    app.settle(0.1)
    row = _selected_search_row(app) or ""
    wanted_line = int(row.rsplit(":", 1)[1].split("#")[0]) if ":" in row.rsplit("/", 1)[1] else 0
    app.key("Return")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == row.split("/")[-1].split(":")[0], 3.0,
                   "Enter to open the result's file")
    step(app, "Enter", 0.3)
    app.expect(_cursor(app)[0] == wanted_line, f"Enter opens the result at its line ({row!r}: {_cursor(app)})")
    line_text = app.text().splitlines()[_cursor(app)[0] - 1] if app.text() and _cursor(app)[0] else ""
    app.expect("queue" in line_text.lower(), f"and that line holds the match ({line_text!r})")
    # VS Code seeds the editor's find widget from the search: it opens, focused.
    app.expect(app.field_text("field:find/search") == "queue" and app.ui()["focus"] == "overlay",
               f"the find widget is seeded from the search and focused ({app.ui()['focus']})")
    app.key("Escape")

    app.send("sidebar-show", "search")
    app.send("focus", "sidebar")
    app.key("Escape")
    app.settle(0.15)
    for key, field in (("slash", "field:search/query"), ("equal", "field:search/replace"),
                       ("i", "field:search/include"), ("x", "field:search/exclude")):
        app.key(key)
        step(app, f"{key} in the list", 0.2)
        app.expect(_active_field(app) == field, f"{key} edits {field} ({_active_field(app)})")
        # The key opens the field; it must not also type into it.
        text = app.field_text(field)
        app.expect(not any(c in text for c in "/=ix"), f"{key} is not typed into the field it opened ({text!r})")
        app.key("Escape")
        app.settle(0.1)
    app.expect(app.field_text("field:search/query") == "queue", f"the query is intact ({app.field_text('field:search/query')!r})")
    app.key("slash")
    app.settle(0.1)
    seen = []
    for _ in range(4):
        app.key("Tab")
        app.settle(0.1)
        seen.append(_active_field(app))
    # The scope fields were opened by i and x above, so all four cycle.
    app.expect(len(set(seen)) == 4 and all(s for s in seen), f"Tab cycles the four fields ({seen})")
    app.key("shift+Tab")
    app.settle(0.1)
    app.expect(_active_field(app) == seen[-2], f"Shift+Tab cycles back ({_active_field(app)})")
    app.key("Escape")
    app.settle(0.1)
    app.expect(_active_field(app) is None, f"Escape leaves the field for the list ({_active_field(app)})")
    app.key("c")
    step(app, "c toggles the exact count", 0.4)
    app.wait_until(lambda: len(_search_rows(app)) >= 2, 5.0, "the results listed again after the recount")
    app.key("c")
    app.wait_until(lambda: len(_search_rows(app)) >= 2, 5.0, "and after toggling back")
    dismiss(app)


# ---- the bottom panel by mouse ------------------------------------------------------

def _panel_height(app: q.App) -> float:
    surface = app.ui()["surfaces"]["bottomPanel"]
    return surface[3] if surface else 0.0


def _terminal_tabs(app: q.App) -> list[dict]:
    return app.query("terminals")


def flow_panel_mouse(app: q.App) -> None:
    """The panel's tabs clicked, closed by their button and by middle-click, the
    panel divider dragged and double-clicked back, a split's divider dragged and
    double-clicked back, maximize on and off."""
    open_file(app, "src/scheduler.cpp")
    if not app.ui()["panel"]["visible"]:
        app.key("ctrl+grave")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    step(app, "the panel", 0.3)
    app.click_named("button:panel/new-terminal")
    app.wait_until(lambda: len(_terminal_tabs(app)) == 2, 5.0, "a second terminal tab")
    step(app, "new terminal", 0.3)
    tabs = app.elements("panel-tab")
    app.expect(len(tabs) == 2, f"two panel tabs ({[t['name'] for t in tabs]})")
    app.click(*app.center(tabs[0]))
    step(app, "click the first tab", 0.3)
    app.expect(_terminal_tabs(app)[0].get("active"), "clicking a panel tab activates it")
    app.expect(app.element(tabs[0]["name"]).get("active") is True, "and the strip shows it active")
    close = next((e for e in app.elements("button", prefix="button:close-panel-tab/")), None)
    if app.expect(close is not None, f"a panel tab has a close button ({[e['name'] for e in app.elements('button')]})"):
        app.click(*app.center(close))
        app.wait_until(lambda: len(_terminal_tabs(app)) == 1, 5.0, "the tab to close")
        step(app, "close a tab by its button", 0.3)
    app.click_named("button:panel/new-terminal")
    app.wait_until(lambda: len(_terminal_tabs(app)) == 2, 5.0, "a second terminal tab again")
    tab = app.elements("panel-tab")[1]
    app.click(*app.center(tab), button=2)
    app.wait_until(lambda: len(_terminal_tabs(app)) == 1, 5.0, "a middle-click to close the tab")
    step(app, "middle-click a tab", 0.3)
    app.expect(len(_terminal_tabs(app)) == 1, "a middle-click closes a panel tab")

    divider = app.element("divider:panel")
    x, y = app.center(divider)
    before = _panel_height(app)
    app.drag(x, y, x, y - 120)
    step(app, "drag the panel divider up", 0.3)
    app.expect(_panel_height(app) > before + 60, f"the panel grows with the drag ({before} -> {_panel_height(app)})")
    divider = app.element("divider:panel")
    app.click(*app.center(divider), clicks=2)
    step(app, "double-click the divider", 0.3)
    app.expect(abs(_panel_height(app) - before) <= 2, f"a double-click restores the default height ({_panel_height(app)}, was {before})")

    app.send("term-split")
    app.wait_until(lambda: any(e["name"].startswith("divider:terminal/") for e in app.elements("divider")), 5.0,
                   "a pane divider from the split")
    step(app, "split the terminal", 0.3)
    pane_divider = next((e for e in app.elements("divider", prefix="divider:terminal/")), None)
    if app.expect(pane_divider is not None, "the split shows its pane divider"):
        x, y = app.center(pane_divider)
        app.drag(x, y, x - 150, y)
        step(app, "drag the pane divider", 0.3)
        moved = app.element(pane_divider["name"])
        app.expect(abs(app.center(moved)[0] - (x - 150)) <= 12, f"the divider follows the drag ({app.center(moved)[0]} vs {x - 150})")
        app.click(*app.center(moved), clicks=2)
        step(app, "double-click the pane divider", 0.3)
        reset = app.element(pane_divider["name"])
        app.expect(abs(app.center(reset)[0] - x) <= 4, f"a double-click evens the panes again ({app.center(reset)[0]} vs {x})")
    app.click_named("button:panel/maximize")
    step(app, "maximize", 0.3)
    app.expect(app.ui()["panel"]["maximized"] and app.ui()["surfaces"]["editorSurface"] is None,
               "maximize takes the editor's room")
    app.click_named("button:panel/maximize")
    step(app, "restore", 0.3)
    app.expect(not app.ui()["panel"]["maximized"] and app.ui()["surfaces"]["editorSurface"] is not None,
               "the second click gives it back")
    app.send("term-close")
    dismiss(app)


# ---- the overview ruler ------------------------------------------------------------

def flow_overview_ruler(app: q.App) -> None:
    """The overview ruler's marks (find matches, the caret) through scrolling,
    with the ruler turned off and on again; every frame against a full repaint.
    The ruler is not a click target (nor is VS Code's), so this is paint."""
    open_file(app, "notes/long.txt", "".join(f"line {i:03d} {'mark' if i % 17 == 0 else ''}\n" for i in range(1, 301)))
    app.key("ctrl+f")
    app.type("mark")
    step(app, "find marks", 0.4)
    app.key("Escape")
    surface = app.ui()["surfaces"]["editorSurface"]
    for notches in (10, 10, -5):
        app.wheel(surface[0] + surface[2] / 2, surface[1] + surface[3] / 2, notches)
        step(app, f"wheel {notches}", 0.2)
    app.key("ctrl+End")
    step(app, "ctrl+End", 0.3)
    app.send("set-setting", "editor.view.overview_ruler.enabled", "false")
    step(app, "ruler off", 0.3)
    app.key("ctrl+Home")
    step(app, "ctrl+Home without the ruler", 0.3)
    app.send("set-setting", "editor.view.overview_ruler.enabled", "true")
    step(app, "ruler on", 0.3)
    app.expect(app.alive(), "the ruler toggles and paints cleanly")


# ---- a diff with soft wrap -----------------------------------------------------------

def flow_diff_wrap(app: q.App) -> None:
    """A diff whose lines are longer than the pane, wrapped and unwrapped, scrolled."""
    assert app.project is not None
    readme = app.project / "README.md"
    long_line = "wrapped " * 60
    readme.write_text(readme.read_text() + long_line + "\n" + long_line + " changed\n")
    app.key("ctrl+shift+g")
    row = app.wait_for("row:git/README.md", 8.0)
    if row is None:
        return
    app.click(*app.center(row))
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 8.0, "the working-tree diff")
    step(app, "the README.md diff", 0.4)
    app.send("focus", "editor")
    app.send("wrap", "on")
    app.wait_until(lambda: app.buffer().get("softWrap") is True, 3.0, "soft wrap on in the diff")
    step(app, "wrap on", 0.4)
    app.key("ctrl+End")
    step(app, "ctrl+End with wrap", 0.3)
    surface = app.ui()["surfaces"]["editorSurface"]
    for notches in (-5, 3, -8):
        app.wheel(surface[0] + surface[2] / 2, surface[1] + surface[3] / 2, notches)
        step(app, f"wheel {notches}", 0.2)
    app.send("wrap", "off")
    app.wait_until(lambda: app.buffer().get("softWrap") is False, 3.0, "soft wrap off")
    step(app, "wrap off", 0.4)
    app.key("ctrl+Home")
    step(app, "ctrl+Home", 0.2)
    app.expect(app.alive(), "the diff survives wrapping and unwrapping")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "commands-args": flow_commands_args,
    "search-view-keys": flow_search_view_keys,
    "panel-mouse": flow_panel_mouse,
    "overview-ruler": flow_overview_ruler,
    "diff-wrap": flow_diff_wrap,
}
