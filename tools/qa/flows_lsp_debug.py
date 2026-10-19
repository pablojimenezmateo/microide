"""Language-server and debugger flows against the real tools: clangd (through
plugins/cpp-lsp) and gdb (plugins/gdb-dap) on the taskflow fixture. A flow skips
itself, loudly, when the tool is not installed -- not running is not a pass, so
it records a finding unless MQA_ALLOW_MISSING_TOOLS=1.
"""

from __future__ import annotations

import json
import os
import shutil
import time
from typing import Callable

import microide_qa as q
from flows_common import expect_text, open_file, read, step, write


def _require(app: q.App, tool: str) -> bool:
    if shutil.which(tool):
        return True
    if os.environ.get("MQA_ALLOW_MISSING_TOOLS") != "1":
        app.expect(False, f"{tool} is not installed; this flow cannot run (MQA_ALLOW_MISSING_TOOLS=1 to skip)")
    return False


def _lsp_ready(app: q.App, relative: str = "src/scheduler.cpp") -> bool:
    write(app, "compile_flags.txt", "-Iinclude\n-std=c++17\n")
    open_file(app, relative)
    return app.wait_until(lambda: "Ready" in app.element("status:lsp").get("label", ""), 30.0,
                          "clangd to be ready")


def _hover(app: q.App, line: int, column: int, timeout: float = 8.0) -> dict | None:
    app.move(*app.text_point(line, column))
    found: dict = {}

    def shown() -> bool:
        hover = app.ui()["popups"]["hover"]
        if hover and hover.get("text"):
            found.update(hover)
            return True
        return False
    return found if app.wait_until(shown, timeout, f"a hover at {line}:{column}") else None


def flow_lsp_hover(app: q.App) -> None:
    """Hover a member and a type: the card shows clangd's answer as text."""
    if not _require(app, "clangd") or not _lsp_ready(app):
        return
    hover = _hover(app, 9, 8)  # queue_
    step(app, "hover queue_", 0.3)
    if hover:
        text = hover["text"]
        app.expect("queue_" in text and "std::vector<Task>" in text,
                   f"the hover names the member and its type ({text!r})")
        app.expect("###" not in text and "`" not in text,
                   f"the hover is text, not markdown source ({text!r})")
        x, y, w, h = hover["rect"]
        app.expect(w > 0 and h > 0 and x >= 0 and y >= 0, "the card has a rect on screen")
    # Moving away dismisses it.
    surface = app.ui()["surfaces"]["sidebar"]
    app.move(surface[0] + 20, surface[1] + surface[3] - 30)
    app.wait_until(lambda: app.ui()["popups"]["hover"] is None, 4.0, "the hover to go away")
    step(app, "move away")


def flow_lsp_show_hover(app: q.App) -> None:
    """Show Hover by keyboard (Ctrl+K Ctrl+I): the card for the caret's symbol with
    the pointer elsewhere; Escape puts it away, and so does moving the caret."""
    if not _require(app, "clangd") or not _lsp_ready(app):
        return
    app.click_text(9, 8)
    step(app, "caret on queue_", 0.3)
    surface = app.ui()["surfaces"]["sidebar"]
    app.move(surface[0] + 20, surface[1] + surface[3] - 30)  # the pointer well away
    app.wait_until(lambda: app.ui()["popups"]["hover"] is None, 4.0, "no card under the pointer")
    shown: dict = {}

    def card() -> bool:
        hover = app.ui()["popups"]["hover"]
        if hover and hover.get("text"):
            shown.update(hover)
            return True
        return False
    app.key("ctrl+k")
    app.key("ctrl+i")
    app.wait_until(card, 8.0, "Ctrl+K Ctrl+I to show the hover at the caret")
    step(app, "show hover", 0.3)
    app.expect("queue_" in shown.get("text", ""), f"the card is the caret's symbol ({shown.get('text')!r})")
    app.key("Escape")
    app.wait_until(lambda: app.ui()["popups"]["hover"] is None, 4.0, "Escape to put the card away")
    step(app, "escape", 0.2)
    app.expect(app.buffer().get("cursors", [{}])[0].get("line") == 9, "Escape left the caret where it was")
    app.key("ctrl+k")
    app.key("ctrl+i")
    app.wait_until(card, 8.0, "the card again")
    app.key("Down")
    app.wait_until(lambda: app.ui()["popups"]["hover"] is None, 4.0, "a caret move to put the card away")
    step(app, "caret moved", 0.2)


def flow_lsp_navigation(app: q.App) -> None:
    """F12 and Ctrl+click go to a definition; Shift+F12 lists references."""
    if not _require(app, "clangd") or not _lsp_ready(app, "src/main.cpp"):
        return
    # `scheduler.add(` on line 7 of main.cpp: the method is defined in scheduler.cpp.
    # Without a project index (no compile_commands.json) clangd answers with the
    # declaration in the header, as it does for VS Code; with one, the source.
    targets = {"scheduler.hpp": 17, "scheduler.cpp": 8}
    app.click_text(7, 16)
    app.key("F12")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") in targets, 10.0,
                   "F12 to open the definition's file")
    step(app, "F12", 0.3)
    title = (app.active_tab() or {}).get("title")
    app.expect(app.cursors()[0][0] == targets.get(title),
               f"F12 lands on Scheduler::add in {title} ({app.cursors()})")
    open_file(app, "src/main.cpp")
    app.wait_until(lambda: "Ready" in app.element("status:lsp").get("label", ""), 15.0, "clangd")
    # The pointer still rests where F12 was pressed, so add()'s hover is back up
    # (below the line: there is no room above line 7) and covers line 12. A press
    # on a hover card is the card's, as in VS Code; move off it first.
    surface = app.ui()["surfaces"]["sidebar"]
    app.move(surface[0] + 20, surface[1] + surface[3] - 30)
    app.wait_until(lambda: app.ui()["popups"]["hover"] is None, 4.0, "the hover to close")
    app.click_text(12, 34, modifiers=("ctrl",))  # the `run` of scheduler.run()
    run_targets = {"scheduler.hpp": 18, "scheduler.cpp": 12}
    app.wait_until(lambda: (app.active_tab() or {}).get("title") in run_targets, 10.0,
                   "Ctrl+click to go to the definition")
    step(app, "ctrl+click", 0.3)
    title = (app.active_tab() or {}).get("title")
    app.expect(app.cursors()[0][0] == run_targets.get(title),
               f"Ctrl+click lands on Scheduler::run in {title} ({app.cursors()})")
    app.key("shift+F12")

    app.wait_until(lambda: app.ui()["panel"]["content"] == "output" and any(
        "References" in e.get("label", "") and e.get("active") for e in app.elements("panel-tab")),
        10.0, "Shift+F12 to show the references")
    step(app, "shift+F12", 0.3)


def flow_lsp_completion(app: q.App) -> None:
    """Member completion narrows as you type and Enter accepts the selection."""
    if not _require(app, "clangd") or not _lsp_ready(app):
        return
    app.click_text(19, 9)
    app.key("End")
    app.key("Return")
    app.type("queue_.")
    app.wait_until(lambda: app.ui()["overlay"].get("mode") == "completion"
                   and app.ui()["overlay"].get("count", 0) > 5, 10.0, "member completion after '.'")
    step(app, "completion up", 0.3)
    before = app.ui()["overlay"].get("count", 0)
    app.type("emp")
    step(app, "type emp", 0.8)
    labels = [e["label"] for e in app.elements("overlay-row")]
    app.expect(app.ui()["overlay"].get("count", 0) < before and all("emp" in l for l in labels),
               f"typing narrows the list ({labels})")
    target = next((e for e in app.elements("overlay-row") if "empty" in e["label"]), None)
    if target:
        app.click(*app.center(target))
        step(app, "click empty()", 0.5)
        line = app.text().split("\n")[19]
        app.expect("queue_.empty" in line, f"clicking a completion accepts it ({line!r})")
    app.expect(not app.ui()["overlay"]["visible"], "accepting closes the list")
    app.key("ctrl+z")
    step(app, "undo the completion")
    app.key("ctrl+space")
    step(app, "ctrl+space", 1.0)
    app.expect(app.ui()["overlay"].get("mode") == "completion", "ctrl+space asks for completions")
    app.key("Escape")
    step(app, "Escape")
    app.expect(not app.ui()["overlay"]["visible"], "Escape closes the list")


def flow_lsp_diagnostics(app: q.App) -> None:
    """Break the code: the problem shows in the status bar, the problems view and
    a hover; Alt+F8 jumps to it; fixing it clears everything."""
    if not _require(app, "clangd") or not _lsp_ready(app):
        return
    app.click_text(16, 19)  # after "int total = 0"
    app.key("End")
    app.type(" int broken = ;")
    step(app, "introduce an error", 0.3)
    app.wait_until(lambda: any(ch.isdigit() and ch != "0"
                               for ch in app.element("status:problems").get("label", "0")
                               if True) if any(e["name"] == "status:problems" for e in app.elements("status")) else False,
                   15.0, "the status bar to count the problem")
    app.key("ctrl+Home")
    app.key("alt+F8")
    step(app, "alt+F8", 0.5)
    app.expect(app.cursors()[0][0] == 16, f"Alt+F8 jumps to the problem's line ({app.cursors()})")
    app.key("ctrl+shift+m")
    step(app, "ctrl+shift+m", 0.5)
    app.expect(app.ui()["sidebar"]["view"] == "problems", "ctrl+shift+m shows the problems view")
    rows = [e for e in app.elements("row") if e["name"].startswith("row:problems/")]
    app.expect(bool(rows), "the problems view lists the error")
    hover = _hover(app, 16, 33)
    app.expect(hover is not None and hover.get("kind") == "diagnostic",
               f"hovering the error shows the diagnostic ({hover})")
    app.send("focus", "editor")
    for _ in range(14):
        app.key("BackSpace")
    step(app, "remove the error", 0.3)
    app.wait_until(lambda: not any(e["name"] == "status:problems" and any(c.isdigit() and c != "0" for c in e.get("label", ""))
                                   for e in app.elements("status")), 15.0, "the problem to clear")


def flow_lsp_rename(app: q.App) -> None:
    """F2 renames a local everywhere it is used."""
    if not _require(app, "clangd") or not _lsp_ready(app):
        return
    app.click_text(17, 10)  # `ran`
    app.key("F2")
    step(app, "F2", 0.5)
    prompt = app.ui().get("prompt") or {}
    app.expect(prompt.get("kind") == "input", f"F2 asks for the new name ({prompt})")
    app.key("ctrl+a")
    app.type("count")
    app.key("Return")
    app.wait_until(lambda: "++count" in app.text() and "int count = 0" in app.text(), 10.0,
                   "the rename to reach every use")
    step(app, "renamed", 0.3)
    app.expect(" ran" not in app.text(), "no use of the old name is left")


def flow_lsp_format(app: q.App) -> None:
    """Format Document rewrites badly formatted code."""
    if not _require(app, "clangd") or not _lsp_ready(app):
        return
    write(app, "src/ugly.cpp", "int   f( int a ){return a+1 ;}\n")
    open_file(app, "src/ugly.cpp")
    app.wait_until(lambda: "Ready" in app.element("status:lsp").get("label", ""), 15.0, "clangd")
    app.key("ctrl+shift+i")
    app.wait_until(lambda: app.text() != "int   f( int a ){return a+1 ;}\n", 10.0,
                   "Format Document to change the text")
    step(app, "formatted", 0.3)
    app.expect("return a + 1;" in app.text(), f"the code is formatted ({app.text()!r})")


def _state(app: q.App) -> dict:
    return app.query("debug-state")


def flow_debug_session(app: q.App) -> None:
    """A whole gdb session: breakpoint by gutter click, launch, stop, step
    over/in/out, read a variable by hover and in the Variables pane, continue
    back onto the breakpoint, stop with Shift+F5, remove the breakpoint."""
    if not _require(app, "gdb"):
        return
    assert app.project is not None
    app.send("set-setting", "debug.enabled", "true")
    open_file(app, "src/scheduler.cpp")
    app.click(*app.gutter_point(19, "breakpoint"))
    step(app, "click the breakpoint gutter", 0.3)
    files = app.query("breakpoints")
    lines = [b["line"] for f in files for b in f["breakpoints"]]
    app.expect(lines == [19], f"a gutter click sets a breakpoint on line 19 ({lines})")
    app.send("debug-run", "--type", "gdb", str(app.project / "taskflow"))
    app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("line") == 19, 40.0,
                   "the program to stop at the breakpoint")
    step(app, "stopped", 0.5)
    app.expect(app.ui()["surfaces"]["rightPane"] is not None, "the debug pane opens on the first stop")
    app.expect((app.active_tab() or {}).get("title") == "scheduler.cpp", "the stop shows its file")
    app.key("F10")
    app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("line") != 19, 15.0,
                   "F10 to step over")
    step(app, "F10", 0.5)
    hover = _hover(app, 19, 10)  # total
    app.expect(hover is not None and hover.get("kind") == "debug-value" and hover["text"].strip().isdigit(),
               f"hovering a variable while stopped shows its value ({hover})")
    app.send("debug-pane-variables")
    step(app, "variables pane", 0.8)
    app.key("F5")
    app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("line") == 19, 15.0,
                   "F5 to continue back onto the breakpoint")
    step(app, "F5 back onto the breakpoint", 0.5)
    app.key("F11")
    app.wait_until(lambda: _state(app).get("stopped") and _state(app).get("line") != 19, 15.0,
                   "F11 to step")
    step(app, "F11", 0.5)
    app.key("shift+F5")
    app.wait_until(lambda: not _state(app).get("stopped") and not _state(app).get("frames"), 15.0,
                   "Shift+F5 to stop the session")
    step(app, "shift+F5", 0.8)
    app.click(*app.gutter_point(19, "breakpoint"))
    step(app, "click the breakpoint again", 0.3)
    files = app.query("breakpoints")
    app.expect(not [b for f in files for b in f["breakpoints"]],
               f"a second gutter click removes the breakpoint ({files})")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "lsp-hover": flow_lsp_hover,
    "lsp-show-hover": flow_lsp_show_hover,
    "lsp-navigation": flow_lsp_navigation,
    "lsp-completion": flow_lsp_completion,
    "lsp-diagnostics": flow_lsp_diagnostics,
    "lsp-rename": flow_lsp_rename,
    "lsp-format": flow_lsp_format,
    "dbg-session": flow_debug_session,
}

PLUGINS: dict[str, tuple[str, ...]] = {
    **{name: ("cpp-lsp",) for name in FLOWS if name.startswith("lsp-")},
    "dbg-session": ("gdb-dap",),
}
