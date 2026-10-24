"""Coverage flows: the commands, settings scopes and debugger surfaces the other
flow groups pressed only as a receipt -- each run here for its effect.

Edit transforms (sort, join, block comment, matching bracket, JSON format, line
insert/copy, column select), the save-time transforms and their toggles, user
versus project settings across projects and a restart (VS Code's user/workspace
model), a debugger session driven from the toolbar and the debug pane (step in,
the call stack, variables, a watch, restart), git branches and the stash, more
language-server navigation, and a compare against the clipboard.
"""

from __future__ import annotations

import subprocess
from typing import Callable

import microide_qa as q
from flows_common import dismiss, expect_cursors, expect_text, open_file, read, step
from flows_lsp_debug import _lsp_ready, _require, _state


def palette(app: q.App, query: str, label: str | None = None) -> bool:
    """Runs a command by clicking its row in the command palette (the row whose
    label starts with `label`, default `query`); records a finding when the
    palette does not list it."""
    app.key("ctrl+shift+p")
    step(app, "ctrl+shift+p", 0.3)
    app.type(query)
    step(app, f"palette: {query}", 0.3)
    want = (label or query).lower()
    rows = app.elements("overlay-row")
    row = next((r for r in rows if r.get("label", "").lower().startswith(want)), None)
    if row is None:
        app.expect(False, f"the palette lists {label or query!r} ({[r.get('label') for r in rows][:6]})")
        dismiss(app)
        return False
    app.click(*app.center(row))
    step(app, f"run {row['label']}", 0.4)
    return True


def git(app: q.App, *args: str) -> str:
    assert app.project is not None
    return subprocess.run(["git", *args], cwd=app.project, capture_output=True, text=True,
                          check=False).stdout.strip()


# ---- editing ---------------------------------------------------------------


def flow_edit_transforms(app: q.App) -> None:
    """Sort and join lines, block-comment a selection, jump between brackets,
    format JSON, insert a line above, copy a line up and select a column -- each
    checked against the text and carets (VS Code's commands and chords)."""
    open_file(app, "notes/sort.txt", "cherry\napple\nbanana\n")
    app.key("ctrl+a")
    if palette(app, "sort lines ascending"):
        expect_text(app, "apple\nbanana\ncherry\n", "Sort Lines Ascending sorts the selected lines")
    app.key("ctrl+a")
    if palette(app, "sort lines descending"):
        expect_text(app, "cherry\nbanana\napple\n", "Sort Lines Descending reverses them")
    app.click_text(1, 1)
    if palette(app, "join lines"):
        expect_text(app, "cherry banana\napple\n", "Join Lines joins the next line with one space")
    app.key("ctrl+z")
    step(app, "undo the join")
    expect_text(app, "cherry\nbanana\napple\n", "undo restores the joined lines")
    app.click_text(2, 3)
    app.key("ctrl+shift+Return")
    app.type("top")
    step(app, "ctrl+shift+Enter, type")
    expect_text(app, "cherry\ntop\nbanana\napple\n", "Ctrl+Shift+Enter opens a line above the caret's")
    app.key("shift+alt+Up")
    step(app, "shift+alt+Up")
    expect_text(app, "cherry\ntop\ntop\nbanana\napple\n", "Shift+Alt+Up copies the line up")
    app.expect(app.cursors()[0][0] == 2, f"the caret stays on the upper copy ({app.cursors()})")
    app.click_text(1, 2)
    app.key("ctrl+shift+alt+Down")
    app.key("ctrl+shift+alt+Down")
    step(app, "column select down twice")
    app.expect(len(app.cursors()) == 3 and {c[1] for c in app.cursors()} == {2},
               f"column select puts a caret on each line in the column ({app.cursors()})")
    app.key("Escape")

    open_file(app, "src/brackets.cpp", "int x = (a + b);\n")
    app.click_text(1, 9)
    app.key("ctrl+shift+backslash")
    step(app, "ctrl+shift+\\")
    expect_cursors(app, [(1, 15)], "Ctrl+Shift+\\ jumps to the matching bracket")
    app.key("ctrl+shift+backslash")
    step(app, "ctrl+shift+\\ again")
    expect_cursors(app, [(1, 9)], "and back to its partner")
    app.click_text(1, 1)
    app.key("shift+End")
    app.key("shift+alt+a")
    step(app, "shift+alt+a")
    expect_text(app, "/* int x = (a + b); */\n", "Shift+Alt+A wraps the selection in a block comment")
    app.key("shift+alt+a")
    step(app, "shift+alt+a again")
    expect_text(app, "int x = (a + b);\n", "Shift+Alt+A again removes it")

    open_file(app, "data/config.json", '{"name":"qa","list":[1,2]}\n')
    if palette(app, "format json"):
        app.wait_until(lambda: app.text().count("\n") >= 4, 3.0, "the JSON to be formatted")
        app.expect('"name": "qa"' in app.text(), f"Format JSON spaces the members ({app.text()!r})")


def flow_save_transforms(app: q.App) -> None:
    """Saving trims trailing whitespace and ensures the final newline by default
    (in the buffer and on disk); each toggle turns its transform off."""
    open_file(app, "notes/trim.txt", "keep  \nlast")
    app.click_text(2, 5)
    app.type("!")
    app.key("ctrl+s")
    app.wait_until(lambda: read(app, "notes/trim.txt") == "keep\nlast!\n", 3.0,
                   "the save to trim and add the final newline")
    expect_text(app, "keep\nlast!\n", "the buffer shows what was saved")
    if not (palette(app, "trim trailing whitespace on save")
            and palette(app, "ensure final newline on save")):
        return
    app.key("ctrl+End")
    app.key("BackSpace")
    app.type("  ")
    app.key("ctrl+s")
    step(app, "save with both transforms off", 0.4)
    app.wait_until(lambda: read(app, "notes/trim.txt") == "keep\nlast!  ", 3.0,
                   f"the save to keep the spaces and the missing newline ({read(app, 'notes/trim.txt')!r})")


# ---- settings --------------------------------------------------------------


def _tab_size(app: q.App) -> int:
    app.send("focus", "editor")
    return int(app.buffer().get("tabSize", 0))


def _settings_row(app: q.App, setting: str, query: str) -> bool:
    app.key("ctrl+comma")
    step(app, "ctrl+,", 0.4)
    app.type(query)
    step(app, f"filter {query}", 0.4)
    return app.expect(any(r["name"] == f"setting:{setting}" for r in app.elements("setting")),
                      f"Settings lists {setting}")


def flow_settings_scopes(app: q.App) -> None:
    """A project override stays in its project; the scope chip makes it the user
    default, which the other project follows; both survive a restart (VS Code's
    user and workspace settings)."""
    assert app.project is not None
    open_file(app, "notes/scope.txt", "plain\n")
    base = _tab_size(app)
    if not _settings_row(app, "editor.tab_size", "tab size"):
        dismiss(app)
        return
    app.click_named("button:setting/editor.tab_size/increment")
    step(app, "increment tab size", 0.3)
    app.expect(app.element("setting:editor.tab_size").get("label") == str(base + 1),
               f"the stepper raises the tab size ({app.element('setting:editor.tab_size').get('label')})")
    scope = app.element("button:setting/editor.tab_size/scope") if any(
        e["name"] == "button:setting/editor.tab_size/scope" for e in app.elements("button")) else None
    app.expect(scope is not None, "the row has a scope chip")
    app.key("Escape")
    step(app, "close Settings", 0.3)
    app.expect(_tab_size(app) == base + 1, f"the editor follows the override ({_tab_size(app)})")

    second = app.project.parent / "second"
    second.mkdir(exist_ok=True)
    (second / "other.txt").write_text("other\n")
    app.send("project-open", str(second))
    app.wait_until(lambda: len(app.elements("project-tab")) >= 2, 8.0, "the second project")
    app.send("open", str(second / "other.txt"))
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "other.txt", 5.0, "other.txt")
    step(app, "second project", 0.4)
    app.expect(_tab_size(app) == base,
               f"a project override does not leak into another project ({_tab_size(app)}, want {base})")

    app.send("project-prev")
    step(app, "back to the first project", 0.5)
    if scope is not None and _settings_row(app, "editor.tab_size", "tab size"):
        app.click_named("button:setting/editor.tab_size/scope")
        step(app, "click the scope chip", 0.3)
        app.key("Escape")
        step(app, "close Settings", 0.3)
        app.send("project-next")
        step(app, "the second project again", 0.5)
        app.expect(_tab_size(app) == base + 1,
                   f"the user default reaches the other project ({_tab_size(app)}, want {base + 1})")

    app.restart()
    open_file(app, "notes/scope.txt")
    app.expect(_tab_size(app) == base + 1,
               f"the tab size survives a restart ({_tab_size(app)}, want {base + 1})")


# ---- debugging -------------------------------------------------------------


def _debug_rows(app: q.App, mode: str) -> list[dict]:
    return app.elements("debug-row", prefix=f"debug-row:{mode}/")


def _show_debug_mode(app: q.App, mode: str) -> None:
    app.click_named(f"debug-tab:{mode}")
    step(app, f"debug pane: {mode}", 0.4)
    app.expect(app.element(f"debug-tab:{mode}").get("active") is True, f"clicking the {mode} tab shows it")


def flow_debug_stepping(app: q.App) -> None:
    """A gdb session driven from the debug toolbar and the pane: step into a call,
    walk the call stack by click, expand a variable, add a watch, step out, step
    over, restart onto the breakpoint and stop."""
    if not _require(app, "gdb"):
        return
    assert app.project is not None
    app.send("set-setting", "debug.enabled", "true")
    open_file(app, "src/main.cpp")
    app.click(*app.gutter_point(12, "breakpoint"))
    step(app, "breakpoint on the run() call", 0.3)
    app.send("debug-run", "--type", "gdb", str(app.project / "taskflow"))
    if not app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("line") == 12, 40.0,
                          "the stop on main.cpp:12"):
        return
    step(app, "stopped", 0.5)
    toolbar = {e["name"].split("/", 1)[1]: e for e in app.elements("button", prefix="button:debug-toolbar/")}
    app.expect({"continue", "step-over", "step-into", "step-out", "restart", "stop"} <= set(toolbar),
               f"the debug toolbar names its buttons ({sorted(toolbar)})")
    app.click_named("button:debug-toolbar/step-into")
    app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("file", "").endswith("scheduler.cpp"),
                   15.0, "Step Into to enter Scheduler::run")
    step(app, "step into", 0.5)
    app.expect((app.active_tab() or {}).get("title") == "scheduler.cpp", "the step shows scheduler.cpp")

    _show_debug_mode(app, "callstack")
    rows = _debug_rows(app, "callstack")
    caller = next((r for r in rows if "main" in r.get("label", "")), None)
    app.expect(len(rows) >= 2 and caller is not None,
               f"the call stack lists run and its caller main ({[r.get('label') for r in rows]})")
    if caller:
        app.click(*app.center(caller))
        step(app, "click the main frame", 0.5)
        app.expect((app.active_tab() or {}).get("title") == "main.cpp" and _state(app).get("line") == 12,
                   f"clicking a frame shows its line ({_state(app).get('file')}:{_state(app).get('line')})")

        _show_debug_mode(app, "variables")
        app.wait_until(lambda: any(r.get("label", "").startswith("scheduler") for r in _debug_rows(app, "variables")),
                       8.0, "main's locals to list scheduler")
        local = next((r for r in _debug_rows(app, "variables") if r.get("label", "").startswith("scheduler")), None)
        if local:
            before = len(_debug_rows(app, "variables"))
            app.click(*app.center(local))
            app.wait_until(lambda: len(_debug_rows(app, "variables")) > before, 8.0,
                           "a click on scheduler to expand it")
            step(app, "expand scheduler", 0.4)
            app.expect(any("queue_" in r.get("label", "") for r in _debug_rows(app, "variables")),
                       f"the expanded struct lists its members ({[r.get('label') for r in _debug_rows(app, 'variables')]})")

    app.key("shift+F11")
    app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("file", "").endswith("main.cpp"),
                   15.0, "Shift+F11 to step out to main")
    step(app, "step out", 0.5)
    app.key("F10")
    # gdb stops on the printf's argument line (14) or its first (13): past 12.
    app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("line", 0) in (13, 14), 15.0,
                   "F10 to finish line 12")
    step(app, "step over", 0.5)

    _show_debug_mode(app, "watch")
    pane = app.ui()["surfaces"]["rightPane"]
    app.click(pane[0] + pane[2] / 2, pane[1] + pane[3] - 30)
    step(app, "click the empty watch list", 0.4)
    app.expect((app.ui().get("prompt") or {}).get("kind") == "input",
               f"a click below the watches asks for an expression ({app.ui().get('prompt')})")
    app.type("total")
    app.key("Return")
    app.wait_until(lambda: any(r.get("label", "").startswith("total = 10") for r in _debug_rows(app, "watch")),
                   8.0, "the watch to show total = 10")
    step(app, "watch total", 0.4)

    app.click_named("button:debug-toolbar/restart")
    app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("line") == 12, 40.0,
                   "Restart to stop on the breakpoint again")
    step(app, "restart", 0.5)
    app.click_named("button:debug-toolbar/stop")
    app.wait_until(lambda: not _state(app).get("stopped") and not _state(app).get("frames"), 15.0,
                   "the toolbar's Stop to end the session")
    step(app, "stop", 0.6)
    app.expect(not app.elements("button", prefix="button:debug-toolbar/"), "the toolbar leaves with the session")


# ---- git -------------------------------------------------------------------


def flow_git_branches(app: q.App) -> None:
    """Create a branch, switch back by the picker, stash a change and pop it: the
    repository, the status bar and the open buffer agree at each step."""
    open_file(app, "src/main.cpp")
    original = read(app, "src/main.cpp")
    if palette(app, "git: create branch"):
        app.expect((app.ui().get("prompt") or {}).get("kind") == "input", "Create Branch asks for a name")
        app.type("qa-topic")
        app.key("Return")
        app.wait_until(lambda: git(app, "rev-parse", "--abbrev-ref", "HEAD") == "qa-topic", 8.0,
                       "the new branch to be checked out")
        app.wait_until(lambda: any("qa-topic" in e.get("label", "") for e in app.elements("status")), 8.0,
                       "the status bar to show qa-topic")
    if palette(app, "git: switch branch"):
        app.type("main")
        step(app, "type main", 0.4)
        app.key("Return")
        app.wait_until(lambda: git(app, "rev-parse", "--abbrev-ref", "HEAD") == "main", 8.0,
                       "the switch back to main")

    app.key("ctrl+End")
    app.type("// stashed\n")
    app.key("ctrl+s")
    app.wait_until(lambda: "// stashed" in read(app, "src/main.cpp"), 3.0, "the edit to be saved")
    if palette(app, "git: stash changes"):
        if (app.ui().get("prompt") or {}).get("kind") == "input":
            app.key("Return")
        app.wait_until(lambda: read(app, "src/main.cpp") == original, 8.0, "the stash to revert the file")
        app.wait_until(lambda: "// stashed" not in app.text(), 5.0, "the open buffer to reload the reverted file")
        app.expect(git(app, "stash", "list") != "", "the stash holds the change")
    if palette(app, "git: pop stash"):
        app.wait_until(lambda: "// stashed" in read(app, "src/main.cpp"), 8.0, "the pop to restore the change")
        app.wait_until(lambda: "// stashed" in app.text(), 5.0, "the buffer to show it again")
        app.expect(git(app, "stash", "list") == "", "and the stash is empty")


# ---- language server -------------------------------------------------------


def flow_lsp_more(app: q.App) -> None:
    """Go to Type Definition, workspace symbols, the next problem and the call
    hierarchy through clangd."""
    if not _require(app, "clangd") or not _lsp_ready(app):
        return
    app.click_text(18, 23)  # `task` in `for (const Task& task : queue_)`
    if palette(app, "go to type definition"):
        app.wait_until(lambda: (app.active_tab() or {}).get("title") == "scheduler.hpp", 8.0,
                       "Go to Type Definition to open the header")
        app.expect(app.cursors() and app.cursors()[0][0] == 8,
                   f"it lands on struct Task ({app.cursors()})")
    # Search Workspace Symbols (TD-2026-10-18-356): Ctrl+T is a pick clangd filters
    # as the query is typed; Enter opens the symbol's file there.
    app.send("focus", "editor")
    app.key("ctrl+t")
    app.wait_until(lambda: app.ui()["overlay"].get("kind") == "workspace-symbol", 3.0, "Ctrl+T's pick")
    app.type("Scheduler")
    app.wait_until(lambda: any(e.get("label", "").startswith("Scheduler") for e in app.elements("overlay-row")),
                   10.0, "clangd to answer with Scheduler")
    step(app, "workspace symbols", 0.3)
    app.key("Return")
    app.expect(app.wait_until(lambda: (app.active_tab() or {}).get("title") in ("scheduler.hpp", "scheduler.cpp"),
                              8.0, "Enter to open the symbol's file"),
               f"Enter opens Scheduler's file ({(app.active_tab() or {}).get('title')})")
    app.expect(not app.ui()["overlay"].get("visible"), "the pick closes on Enter")

    open_file(app, "src/broken.cpp", "int ok = 1;\nint bad = missing_name;\nint fine = 2;\n")
    app.wait_until(lambda: app.element("status:problems").get("label", "") not in ("", "0"), 15.0,
                   "clangd to report the error")
    app.click_text(1, 1)
    app.key("alt+F8")
    step(app, "alt+F8", 0.4)
    app.expect(app.cursors()[0][0] == 2, f"Alt+F8 goes to the problem ({app.cursors()})")

    # clangd indexes open files (compile_flags.txt has no project index): open
    # the caller first, so incoming calls can find main.
    open_file(app, "src/main.cpp")
    open_file(app, "src/scheduler.cpp")
    app.click_text(12, 17)  # `run` in `int Scheduler::run()`
    if palette(app, "show call hierarchy"):
        # Incoming calls by default: main is run's one caller.
        app.wait_until(lambda: any("main" in line for line in app.output("lsp.callHierarchy")), 10.0,
                       "the call hierarchy to list main as a caller")
        step(app, "call hierarchy", 0.4)
        app.expect(app.ui()["panel"].get("content") == "output",
                   f"the call hierarchy shows in the panel's output ({app.ui()['panel']})")


# ---- compare ---------------------------------------------------------------


def flow_compare_clipboard(app: q.App) -> None:
    """Copy some text, then Compare with Clipboard opens a compare tab of the file
    against it."""
    open_file(app, "notes/cmp.txt", "one\ntwo\nthree\n")
    app.click_text(2, 1)
    app.key("shift+End")
    app.key("ctrl+c")
    step(app, "copy two")
    tabs_before = len(app.query("tabs")) if isinstance(app.query("tabs"), list) else 0
    if palette(app, "compare with clipboard"):
        app.wait_until(lambda: (app.active_tab() or {}).get("kind") not in (None, "editor"), 5.0,
                       "a compare tab to open")
        step(app, "compare tab", 0.5)
        tab = app.active_tab() or {}
        app.expect("cmp.txt" in tab.get("title", "") or "lipboard" in tab.get("title", ""),
                   f"the compare tab names the file or the clipboard ({tab})")
        tabs = app.query("tabs")
        app.expect(isinstance(tabs, list) and len(tabs) == tabs_before + 1, "one compare tab was added")


# ---- tabs ------------------------------------------------------------------


def flow_reopen_closed_editor(app: q.App) -> None:
    """Close two tabs (by Ctrl+W and by the tab's close button); Ctrl+Shift+T
    reopens the newest first with its caret where it was, then the other; the File
    menu offers it while there is something to reopen. A long file closed folded
    and scrolled away from its caret comes back with both (TD-2026-10-24-390)."""
    body = "void folded() {\n" + "".join(f"  body({i});\n" for i in range(20)) + "}\n"
    open_file(app, "notes/long.c", body + "".join(f"line {i}\n" for i in range(300)))
    app.send("goto", "2:1")
    app.send("fold")
    app.send("goto", "200:3")
    step(app, "fold, caret on line 200", 0.3)
    pane = next((e["rect"] for e in app.ui().get("editors", []) if e.get("focused")), None)
    if app.expect(pane is not None, "the editor pane is on screen") and pane:
        app.wheel(pane[0] + pane[2] / 2, pane[1] + pane[3] / 2, 4)
    step(app, "scroll away from the caret", 0.3)
    left = app.active_tab() or {}
    app.key("ctrl+w")
    step(app, "ctrl+w long.c", 0.3)
    app.key("ctrl+shift+t")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "long.c", 3.0, "long.c to reopen")
    step(app, "reopened long.c", 0.4)
    back = app.active_tab() or {}
    for key in ("cursorLine", "cursorColumn", "scrollLine", "foldedLines"):
        app.expect(back.get(key) == left.get(key),
                   f"long.c reopens with its {key} ({back.get(key)} vs {left.get(key)})")
    app.expect(left.get("foldedLines") == [1], f"the function was folded ({left.get('foldedLines')})")
    open_file(app, "notes/one.txt", "one\ntwo\nthree\n")
    open_file(app, "notes/two.txt", "alpha\nbeta\ngamma\n")
    app.click_text(2, 3)
    app.key("ctrl+w")
    step(app, "ctrl+w two.txt", 0.3)
    close = next((e for e in app.elements("button", prefix="button:close-tab/") if "one.txt" in e["name"]), None)
    app.expect(close is not None, "one.txt has a close button")
    if close:
        app.click(*app.center(close))
        step(app, "close one.txt by its button", 0.3)
    app.key("ctrl+shift+t")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "one.txt", 3.0,
                   "Ctrl+Shift+T to reopen one.txt, the last closed")
    app.key("ctrl+shift+t")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "two.txt", 3.0,
                   "a second Ctrl+Shift+T to reopen two.txt")
    step(app, "reopened", 0.3)
    app.expect(app.cursors() == [(2, 3)], f"two.txt reopens with its caret where it was ({app.cursors()})")
    app.click_named("menu:File")
    step(app, "File menu", 0.3)
    row = next((e for e in app.elements("menu-item") if e["name"] == "menu-item:File/Reopen Closed Editor"), None)
    app.expect(row is not None and row.get("enabled") is False,
               f"with nothing left to reopen, File > Reopen Closed Editor is disabled ({row})")
    dismiss(app)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "edit-transforms": flow_edit_transforms,
    "save-transforms": flow_save_transforms,
    "settings-scopes": flow_settings_scopes,
    "dbg-stepping": flow_debug_stepping,
    "git-branches": flow_git_branches,
    "lsp-more": flow_lsp_more,
    "compare-clipboard": flow_compare_clipboard,
    "tab-reopen": flow_reopen_closed_editor,
}

PLUGINS: dict[str, tuple[str, ...]] = {
    "dbg-stepping": ("gdb-dap",),
    "lsp-more": ("cpp-lsp",),
}
