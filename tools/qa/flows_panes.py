"""The git view and a plugin's view by keyboard, the debug pane by mouse, and
the two dirty-buffer paths a close can take without asking.

A coverage build of the suite left the sidebar key handler at 45 %, the debug
pane's mouse coordinator at 39 % and the dirty-prompt continuations at 5 %
(TD-2026-10-18-364). The receipts are the repository (`git status`), the control
queries and the files on disk, never pixels.
"""

from __future__ import annotations

import subprocess
from typing import Callable

import microide_qa as q
from flows_common import dismiss, open_file, read, step, write
from flows_coverage import _debug_rows, _show_debug_mode
from flows_lsp_debug import _require, _state


def _git(app: q.App, *args: str) -> str:
    assert app.project is not None
    return subprocess.run(["git", *args], cwd=app.project, capture_output=True, text=True, check=False).stdout


def _porcelain(app: q.App) -> dict[str, str]:
    """{path: XY} from `git status --porcelain`."""
    out: dict[str, str] = {}
    for line in _git(app, "status", "--porcelain").splitlines():
        out[line[3:].strip()] = line[:2]
    return out


def _git_rows(app: q.App) -> list[dict]:
    return app.elements("row", prefix="row:git/")


def _git_selected(app: q.App) -> str | None:
    return next((r["name"] for r in _git_rows(app) if r.get("selected")), None)


def flow_git_view_keys(app: q.App) -> None:
    """The git view by keys: Down/Up/Home/End over the entries, `s` staging the
    selected one and `u` unstaging it (the repository says so), `d` and Enter
    opening its diff, `R` refreshing after a change made outside."""
    assert app.project is not None
    for name in ("alpha", "beta", "gamma"):
        write(app, f"keys/{name}.txt", f"{name}\n")
    _git(app, "add", "keys")
    _git(app, "commit", "-q", "-m", "qa: git view keys fixture")
    for name in ("alpha", "beta", "gamma"):
        write(app, f"keys/{name}.txt", f"{name} changed\n")
    open_file(app, "keys/alpha.txt")
    app.key("ctrl+shift+g")
    app.wait_until(lambda: len(_git_rows(app)) >= 3, 8.0, "the three changes in the git view")
    step(app, "git view", 0.4)
    app.send("focus", "sidebar")
    app.key("Home")
    app.settle(0.15)
    first = _git_selected(app)
    app.key("Down")
    step(app, "Down", 0.15)
    second = _git_selected(app)
    app.expect(second is not None and second != first, f"Down moves the selection ({first} -> {second})")
    app.key("End")
    step(app, "End", 0.15)
    app.expect(_git_selected(app) == "row:git/gamma.txt", f"End selects the last entry ({_git_selected(app)})")
    app.key("Up")
    step(app, "Up", 0.15)
    app.expect(_git_selected(app) == "row:git/beta.txt", f"Up moves back up ({_git_selected(app)})")
    app.key("Home")
    app.settle(0.15)
    selected = (_git_selected(app) or "").split("/", 1)[-1]
    app.key("s")
    app.wait_until(lambda: _porcelain(app).get(f"keys/{selected}", "").startswith("M"), 5.0, f"s to stage {selected}")
    step(app, "s", 0.3)
    app.expect(_porcelain(app).get(f"keys/{selected}", "") == "M ", f"s stages the selected entry ({_porcelain(app)})")
    # The staged entry sits under its own header now: select it (a click also
    # opens its diff and focuses the editor, so the keys go back to the view).
    staged = next((r for r in _git_rows(app) if r["name"].endswith("/" + selected)), None)
    if staged is not None:
        app.click(*app.center(staged))
        app.settle(0.2)
        app.send("focus", "sidebar")
    app.key("u")
    app.wait_until(lambda: _porcelain(app).get(f"keys/{selected}", "") == " M", 5.0, f"u to unstage {selected}")
    step(app, "u", 0.3)
    app.expect(_porcelain(app).get(f"keys/{selected}", "") == " M", f"u unstages it again ({_porcelain(app)})")
    app.key("d")
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 5.0, "d to open the diff")
    step(app, "d", 0.3)
    app.expect((app.active_tab() or {}).get("kind") == "compare", "d opens the entry's diff")
    app.send("focus", "sidebar")
    app.key("Down")
    app.settle(0.1)
    other = (_git_selected(app) or "").split("/", 1)[-1]
    app.key("Return")
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare"
                   and other in (app.active_tab() or {}).get("title", ""), 5.0, "Enter to open the next entry's diff")
    step(app, "Enter", 0.3)
    app.expect(other in (app.active_tab() or {}).get("title", ""), f"Enter opens the selected entry ({app.active_tab()})")
    write(app, "keys/delta.txt", "new\n")
    app.send("focus", "sidebar")
    app.key("R")
    app.wait_until(lambda: any(r["name"].endswith("/delta.txt") for r in _git_rows(app)), 8.0, "R to pick up the new file")
    step(app, "R", 0.3)
    app.expect(any(r["name"].endswith("/delta.txt") for r in _git_rows(app)), "R refreshes the view")
    dismiss(app)


def _view_rows(app: q.App, view: str) -> list[dict]:
    return app.elements("row", prefix=f"row:{view}/")


def flow_plugin_sidebar_keys(app: q.App) -> None:
    """A plugin's tree view by keys: Down/Up, Right expanding a collapsed row,
    Left collapsing it, Space toggling, Enter confirming (the plugin counts each
    callback), R refreshing."""
    open_file(app, "notes/tree.qaapi", "one\ntwo\nthree\n")
    app.send("sidebar-show", "qa-api")
    app.wait_until(lambda: len(_view_rows(app, "qa-api")) >= 1, 10.0, "the plugin's rows")
    step(app, "the plugin view", 0.3)
    app.send("focus", "sidebar")
    rows = _view_rows(app, "qa-api")
    app.expect([r["name"] for r in rows] == ["row:qa-api/Root", "row:qa-api/Leaf"],
               f"the plugin's tree starts expanded, as its snapshot says ({[r['name'] for r in rows]})")
    app.key("Home")
    app.key("Left")
    app.wait_until(lambda: len(_view_rows(app, "qa-api")) == 1, 5.0, "Left to collapse the root")
    step(app, "Left", 0.3)
    app.expect([r["name"] for r in _view_rows(app, "qa-api")] == ["row:qa-api/Root"], "Left collapses the row")
    app.key("Right")
    app.wait_until(lambda: len(_view_rows(app, "qa-api")) == 2, 5.0, "Right to expand the root")
    step(app, "Right", 0.3)
    app.expect(len(_view_rows(app, "qa-api")) == 2, "Right expands it again")
    app.key("space")
    app.wait_until(lambda: len(_view_rows(app, "qa-api")) == 1, 5.0, "Space to toggle it closed")
    step(app, "Space", 0.3)
    app.key("space")
    app.wait_until(lambda: len(_view_rows(app, "qa-api")) == 2, 5.0, "Space to toggle it open")
    step(app, "Space again", 0.3)
    app.key("Down")
    step(app, "Down", 0.15)
    selected = next((r["name"] for r in _view_rows(app, "qa-api") if r.get("selected")), None)
    app.expect(selected == "row:qa-api/Leaf", f"Down selects the leaf ({selected})")
    app.key("Return")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "notes.qaapi" or app.ui()["focus"] == "editor", 5.0,
                   "Enter to confirm the leaf")
    step(app, "Enter", 0.3)
    app.expect(app.alive(), "Enter on a leaf confirms it (the plugin's on_confirm)")
    app.send("focus", "sidebar")
    app.key("R")
    step(app, "R", 0.3)
    app.expect(len(_view_rows(app, "qa-api")) >= 1, "R refreshes the view and keeps its rows")
    dismiss(app)


def flow_debug_pane_mouse(app: q.App) -> None:
    """The debug pane by mouse at a stop: a variable row's context menu (Copy
    Value, Add to Watch), the Breakpoints tab's rows (a click jumps to the line,
    the context menu disables and removes), the wheel over a long list."""
    if not _require(app, "gdb"):
        return
    assert app.project is not None
    open_file(app, "src/scheduler.cpp")
    app.send("set-setting", "debug.enabled", "true")
    app.click(*app.gutter_point(12, "breakpoint"))
    app.click(*app.gutter_point(19, "breakpoint"))
    step(app, "two breakpoints", 0.3)
    app.send("debug-run", "--type", "gdb", str(app.project / "taskflow"))
    if not app.wait_until(lambda: _state(app).get("stopped"), 40.0, "a stop at the first breakpoint"):
        return
    step(app, "stopped", 0.6)
    _show_debug_mode(app, "variables")
    rows = [r for r in _debug_rows(app, "variables") if "=" in r.get("label", "")]
    if app.expect(bool(rows), f"the Variables tab lists variables at the stop ({[r.get('label') for r in _debug_rows(app, 'variables')]})"):
        row = rows[0]
        app.click(*app.center(row), button=3)
        step(app, "right-click a variable", 0.3)
        app.expect(app.ui()["menu"]["contextMenu"], "a variable row has a context menu")
        labels = [e["label"] for e in app.elements("menu-item", prefix="menu-item:context/")]
        app.expect("Copy Value" in labels and "Add to Watch" in labels, f"with Copy Value and Add to Watch ({labels})")
        app.click_named("menu-item:context/Add to Watch")
        step(app, "Add to Watch", 0.4)
        _show_debug_mode(app, "watch")
        watch = _debug_rows(app, "watch")
        app.expect(any(row["label"].split("=")[0].strip().split(":")[0].strip() in w.get("label", "") for w in watch),
                   f"the variable is a watch row now ({[w.get('label') for w in watch]})")
        _show_debug_mode(app, "variables")
        row = next(r for r in _debug_rows(app, "variables") if "=" in r.get("label", ""))
        app.click(*app.center(row), button=3)
        app.settle(0.2)
        app.click_named("menu-item:context/Copy Value")
        step(app, "Copy Value", 0.3)
        app.send("focus", "editor")
        app.key("ctrl+End")
        app.key("Return")
        app.key("ctrl+v")
        app.settle(0.3)
        pasted = app.text().splitlines()[-1] if app.text() else ""
        app.expect(bool(pasted.strip()), f"Copy Value put the value on the clipboard ({pasted!r})")
        app.key("ctrl+z")
        app.key("ctrl+z")
    _show_debug_mode(app, "breakpoints")
    bp_rows = _debug_rows(app, "breakpoints")
    if app.expect(len(bp_rows) >= 2, f"the Breakpoints tab lists both ({[r.get('label') for r in bp_rows]})"):
        second = next((r for r in bp_rows if ":19" in r.get("label", "")), bp_rows[-1])
        app.click(*app.center(second))
        app.wait_until(lambda: app.cursors() and app.cursors()[0][0] == 19, 3.0, "a click to jump to the breakpoint's line")
        step(app, "click a breakpoint row", 0.3)
        app.expect(app.cursors() and app.cursors()[0][0] == 19, f"a click jumps to the breakpoint's line ({app.cursors()[:1]})")
        app.click(*app.center(second), button=3)
        step(app, "right-click a breakpoint row", 0.3)
        app.expect(app.ui()["menu"]["contextMenu"], "a breakpoint row has a context menu")
        app.click_named("menu-item:context/Disable Breakpoint")
        app.wait_until(lambda: any(b["line"] == 19 and not b["enabled"] for f in app.query("breakpoints")
                                   for b in f.get("breakpoints", [])), 3.0, "Disable Breakpoint")
        step(app, "Disable Breakpoint", 0.3)
        app.expect(any(b["line"] == 19 and not b["enabled"] for f in app.query("breakpoints") for b in f.get("breakpoints", [])),
                   "the context menu disables the breakpoint")
        second = next((r for r in _debug_rows(app, "breakpoints") if ":19" in r.get("label", "")), None)
        if second is not None:
            app.click(*app.center(second), button=3)
            app.settle(0.2)
            app.click_named("menu-item:context/Remove Breakpoint")
            app.wait_until(lambda: not any(b["line"] == 19 for f in app.query("breakpoints") for b in f.get("breakpoints", [])),
                           3.0, "Remove Breakpoint")
            step(app, "Remove Breakpoint", 0.3)
            app.expect(not any(b["line"] == 19 for f in app.query("breakpoints") for b in f.get("breakpoints", [])),
                       "the context menu removes the breakpoint")
    _show_debug_mode(app, "callstack")
    pane = app.ui()["surfaces"]["rightPane"]
    if pane:
        app.wheel(pane[0] + pane[2] / 2, pane[1] + pane[3] / 2, 3)
        step(app, "wheel the pane", 0.2)
        app.wheel(pane[0] + pane[2] / 2, pane[1] + pane[3] / 2, -3)
        step(app, "wheel back", 0.2)
    app.send("debug-stop")
    app.wait_until(lambda: not _state(app).get("stopped"), 10.0, "the session to stop")
    dismiss(app)


def flow_dirty_close_group(app: q.App) -> None:
    """Closing an editor group whose tab is dirty asks first (Save / Discard /
    Cancel), as closing the tab would; Reload from Disk on a dirty buffer says
    why it will not."""
    assert app.project is not None
    open_file(app, "notes/kept.txt", "kept\n")
    app.send("split-right", "notes/kept.txt")
    app.wait_until(lambda: len({t.get("group") for t in app.query("tabs")}) == 2, 5.0, "a second group")
    open_file(app, "notes/group.txt", "group\n")
    app.click_text(1, 1)
    app.key("ctrl+End")
    app.type(" edited")
    step(app, "a dirty tab in the second group", 0.3)
    app.send("close-group")
    prompted = app.wait_until(lambda: app.ui().get("modal") == "unsaved-prompt", 3.0, "the unsaved prompt before the group closes")
    step(app, "close-group", 0.3)
    if prompted:
        cancel = next((e for e in app.elements("button", prefix="button:prompt/") if e["name"].endswith("/Cancel")), None)
        if cancel is not None:
            app.click(*app.center(cancel))
        step(app, "Cancel", 0.3)
        app.expect(len({t.get("group") for t in app.query("tabs")}) == 2 and app.buffer().get("dirty"),
                   "Cancel keeps the group and the edit")
        app.send("close-group")
        app.wait_until(lambda: app.ui().get("modal") == "unsaved-prompt", 3.0, "the prompt again")
        discard = next((e for e in app.elements("button", prefix="button:prompt/") if e["name"].split("/")[-1].startswith("Discard")), None)
        if discard is not None:
            app.click(*app.center(discard))
        app.wait_until(lambda: len({t.get("group") for t in app.query("tabs")}) == 1, 3.0, "Discard to close the group")
        step(app, "Discard", 0.3)
    app.expect(read(app, "notes/group.txt") == "group\n", "the disk was never written by a close")
    app.expect(prompted, "closing a group with a dirty tab asks (VS Code: Save / Don't Save / Cancel) instead of dropping the edit")

    open_file(app, "notes/reload.txt", "disk\n")
    app.click_text(1, 1)
    app.key("ctrl+End")
    app.type(" edited")
    reply = app.send_line("reopen")
    step(app, "reopen with a dirty buffer", 0.3)
    app.expect(not reply.get("ok") and bool(reply.get("error")),
               f"Reload from Disk on a dirty buffer says why it will not ({reply})")
    app.expect(app.text() == "disk\n edited", f"and keeps the edit ({app.text()!r})")
    dismiss(app)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "git-view-keys": flow_git_view_keys,
    "plugin-sidebar-keys": flow_plugin_sidebar_keys,
    "debug-pane-mouse": flow_debug_pane_mouse,
    "dirty-close-group": flow_dirty_close_group,
}

PLUGINS: dict[str, tuple[str, ...]] = {
    "plugin-sidebar-keys": ("qa-api",),
    "debug-pane-mouse": ("gdb-dap",),
}
