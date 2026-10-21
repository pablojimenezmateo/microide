"""Advanced editor and tool flows: signature help, code actions, snippets,
sticky scroll, themes, an overflowing tab strip, and the debugger's breakpoint
conditions and watch expressions.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, open_file, step, write
from flows_lsp_debug import _lsp_ready, _require


def flow_signature_help(app: q.App) -> None:
    """Inside a call's parentheses, Ctrl+Shift+Space shows the signature; moving
    out of the call closes it."""
    if not _require(app, "clangd") or not _lsp_ready(app):
        return
    app.click_text(9, 1)
    app.key("End")
    app.key("Return")
    app.type("add(")
    step(app, "type add(", 0.5)
    app.key("ctrl+shift+space")
    app.wait_until(lambda: app.ui()["popups"]["signatureHelp"], 8.0, "signature help")
    step(app, "signature help", 0.3)
    sig = app.ui()["popups"]["signatureHelp"] or ""
    app.expect("priority" in sig and "cost" in sig, f"the signature names the parameters ({sig!r})")
    app.key("Escape")
    app.key("Up")
    step(app, "leave the call", 0.3)
    app.wait_until(lambda: not app.ui()["popups"]["signatureHelp"], 4.0, "signature help to close")


def flow_code_actions(app: q.App) -> None:
    """An unused include draws clangd's warning; Ctrl+. on it offers a fix and
    applying it removes the include."""
    if not _require(app, "clangd"):
        return
    write(app, "compile_flags.txt", "-Iinclude\n-std=c++17\n")
    write(app, "src/unused.cpp", "#include <vector>\n\nint main() { return 0; }\n")
    write(app, ".clangd", "Diagnostics:\n  UnusedIncludes: Strict\n")
    open_file(app, "src/unused.cpp")
    app.wait_until(lambda: "Ready" in app.element("status:lsp").get("label", ""), 30.0, "clangd")
    app.wait_until(lambda: any(e["name"] == "status:problems" and any(c.isdigit() and c != "0"
                                                                      for c in e.get("label", ""))
                               for e in app.elements("status")), 20.0, "the unused-include warning")
    app.click_text(1, 12)
    app.key("ctrl+period")
    app.wait_until(lambda: app.ui()["overlay"].get("mode") == "code-actions"
                   and app.ui()["overlay"].get("count", 0) > 0, 10.0, "code actions")
    step(app, "code actions", 0.3)
    rows = [e for e in app.elements("overlay-row")]
    fix = next((r for r in rows if "remove" in r.get("label", "").lower() or "unused" in r.get("label", "").lower()),
               rows[0] if rows else None)
    if fix:
        app.click(*app.center(fix))
        step(app, "apply the fix", 0.8)
        app.wait_until(lambda: "#include <vector>" not in app.text(), 6.0, "the fix to remove the include")


def flow_snippets(app: q.App) -> None:
    """Insert Snippet lists the language's snippets; choosing one inserts it and
    Tab walks its placeholders."""
    open_file(app, "src/snip.cpp", "\n")
    app.key("ctrl+alt+j")
    step(app, "ctrl+alt+j", 0.4)
    overlay = app.ui()["overlay"]
    if not overlay.get("visible") or not overlay.get("count"):
        app.expect(False, f"Insert Snippet lists C++ snippets (overlay {overlay}, "
                          f"notifications {app.query('notifications')}, language {app.buffer().get('language')})")
        dismiss(app)
        return
    app.key("Return")
    step(app, "choose the first snippet", 0.4)
    # tools/qa/plugins/qa-snippets: the placeholders expand to their defaults.
    app.expect("for (int i = 0; i < count; ++i) {" in app.text(),
               f"choosing the snippet inserts its body ({app.text()!r})")
    app.expect(app.buffer().get("selectedText") == "i",
               f"the first placeholder is selected ({app.buffer().get('selectedText')!r})")
    app.key("Tab")
    step(app, "Tab to the next placeholder", 0.2)
    app.expect(app.buffer().get("selectedText") == "count",
               f"Tab selects the next placeholder ({app.buffer().get('selectedText')!r})")
    app.key("Escape")


def flow_sticky_scroll(app: q.App) -> None:
    """Scrolled inside a long function, its opener stays pinned at the top; a
    click on the pinned line goes to it."""
    body = "int f() {\n" + "".join(f"    int v{i} = {i};\n" for i in range(80)) + "    return 0;\n}\n"
    open_file(app, "src/long.cpp", body)
    surface = app.ui()["surfaces"]["editorSurface"]
    app.wheel(surface[0] + surface[2] / 2, surface[1] + surface[3] / 2, 10)
    step(app, "scroll into the function", 0.4)
    pane = app.pane()
    app.expect(pane.get("stickyRows", 0) >= 1, f"the function's opener is pinned (sticky rows {pane.get('stickyRows')})")
    if pane.get("stickyRows", 0) >= 1:
        x = pane["textX"] + 20
        y = pane["firstLineY"] - pane["lineHeight"] * pane["stickyRows"] + pane["lineHeight"] / 2
        app.click(x, y)
        step(app, "click the pinned line", 0.4)
        app.expect(app.buffer()["scrollLine"] == 1, f"a click on the pinned opener scrolls to it ({app.buffer()['scrollLine']})")


def flow_themes(app: q.App) -> None:
    """Toggle light/dark and step through colour schemes with files, a split and
    the terminal on screen: every frame is clean and the layout holds."""
    open_file(app, "src/scheduler.cpp")
    app.send("split-right", "src/main.cpp")
    for command in (("toggle-theme",), ("toggle-theme",), ("colorscheme", "list")):
        app.send(*command)
        step(app, " ".join(command), 0.5)
    for name in ("default", "light", "dark"):
        app.send("colorscheme", name)
        step(app, f"colorscheme {name}", 0.5)
    app.expect(app.alive(), "the app survives every theme switch")


def flow_tab_overflow(app: q.App) -> None:
    """Twenty tabs overflow the strip: the active one stays visible, the strip
    scrolls by wheel, and Ctrl+PageDown walks into the hidden ones."""
    for i in range(20):
        write(app, f"many/file_{i:02d}.txt", f"file {i}\n")
        app.send("open", f"many/file_{i:02d}.txt")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "file_19.txt", 8.0, "the last tab")
    step(app, "twenty tabs", 0.4)
    visible = [e["name"] for e in app.elements("tab")]
    app.expect("tab:0/file_19.txt" in visible, f"the active tab is on screen ({visible[-3:]})")
    app.expect(len(visible) < 21, "and the strip overflows (not every tab fits)")
    strip = app.ui()["surfaces"]["tabStrip"]
    app.wheel(strip[0] + strip[2] / 2, strip[1] + strip[3] / 2, -10)
    step(app, "wheel the strip", 0.3)
    app.expect([e["name"] for e in app.elements("tab")] != visible, "the wheel scrolls the strip")
    for _ in range(3):
        app.key("ctrl+Next")
        step(app, "ctrl+PageDown", 0.15)
        active = (app.active_tab() or {}).get("title")
        app.expect(any(e["name"].endswith("/" + str(active)) for e in app.elements("tab")),
                   f"the newly active tab {active} is scrolled into view")


def flow_debug_conditions(app: q.App) -> None:
    """A conditional breakpoint set from its gutter menu stops only when it holds;
    a watch expression shows its value at the stop."""
    if not _require(app, "gdb"):
        return
    assert app.project is not None
    app.send("set-setting", "debug.enabled", "true")
    open_file(app, "src/scheduler.cpp")
    app.click(*app.gutter_point(19, "breakpoint"))
    step(app, "breakpoint", 0.3)
    app.click(*app.gutter_point(19, "breakpoint"), button=3)
    step(app, "right-click the breakpoint", 0.3)
    cond = next((e for e in app.elements("menu-item") if e.get("label", "").startswith("Set Condition")), None)
    app.expect(cond is not None, "the breakpoint's menu offers Set Condition")
    if not cond:
        return
    app.click(*app.center(cond))
    step(app, "Set Condition", 0.3)
    app.type("task.priority == 8")
    app.key("Return")
    step(app, "condition typed", 0.3)
    bps = [b for f in app.query("breakpoints") for b in f["breakpoints"]]
    app.expect(bps and bps[0].get("condition") == "task.priority == 8", f"the condition is stored ({bps})")
    app.send("debug-run", "--type", "gdb", str(app.project / "taskflow"))
    app.wait_until(lambda: app.query("debug-state").get("stopped"), 40.0, "the conditional stop")
    step(app, "stopped", 0.5)
    app.send("debug-pane-watch")
    app.send("debug-repl")
    step(app, "debug console", 0.3)
    app.expect(app.ui().get("modal") == "prompt", "debug-repl asks for an expression")
    # A modal takes the pointer: close it before hovering (the hover card used to
    # show through it, which is how this flow passed with the prompt still up).
    app.key("Escape")
    step(app, "close the console prompt", 0.3)
    # The stop is the one the condition allows: hovering `task` shows priority 8.
    hover = None
    app.move(*app.text_point(19, 26))
    app.wait_until(lambda: (app.ui()["popups"]["hover"] or {}).get("kind") == "debug-value", 8.0,
                   "a debug hover on task.cost")
    hover = app.ui()["popups"]["hover"]
    app.expect(hover is not None and "4" in hover.get("text", ""),
               f"the conditional stop is the priority-8 task, whose cost is 4 ({hover})")
    dismiss(app)
    app.send("debug-stop")
    app.wait_until(lambda: not app.query("debug-state").get("stopped"), 10.0, "the session to stop")


def _line(app: q.App, n: int) -> str:
    lines = app.buffer().get("text", "").split("\n")
    return lines[n - 1] if 0 < n <= len(lines) else ""


def _expand(app: q.App, line: int, prefix: str) -> None:
    """Types `prefix` on an empty `line` and presses Tab: a unique exact prefix
    expands in place (no picker)."""
    app.click_text(line, 1)
    app.key("End")
    app.type(prefix)
    if app.ui()["overlay"].get("visible"):
        app.key("Escape")
    app.key("Tab")
    step(app, f"Tab expands {prefix}", 0.25)


def _selected(app: q.App) -> str:
    return app.buffer().get("selectedText") or ""


def flow_snippets_deep(app: q.App) -> None:
    """The snippet engine through the keyboard: Tab on a prefix expands it;
    mirrors follow typing and Backspace; a choice cycles on Tab and Shift+Tab;
    a nested placeholder grows its parent and is dropped when the parent is
    typed over; variables resolve; one undo takes the expansion back; a
    completion item that is a snippet starts a session when accepted."""
    open_file(app, "src/deep.cpp", "\n\n\n\n\n\n\n")

    # Mirrors.
    _expand(app, 1, "mirr")
    app.expect(_line(app, 1) == "name = name + 1;", f"mirr expands with its defaults ({_line(app, 1)!r})")
    app.expect(_selected(app) == "name", f"the first placeholder is selected ({_selected(app)!r})")
    app.type("total")
    step(app, "type into a mirrored placeholder", 0.2)
    app.expect(_line(app, 1) == "total = total + 1;", f"typing updates every mirror ({_line(app, 1)!r})")
    app.key("BackSpace")
    app.key("BackSpace")
    step(app, "Backspace in a mirror", 0.2)
    app.expect(_line(app, 1) == "tot = tot + 1;", f"Backspace edits every mirror ({_line(app, 1)!r})")
    app.key("Tab")
    step(app, "Tab to the second placeholder", 0.2)
    app.expect(_selected(app) == "1", f"Tab selects the second placeholder ({_selected(app)!r})")
    app.type("2")
    app.key("Tab")
    step(app, "Tab to the final stop", 0.2)
    app.expect(_line(app, 1) == "tot = tot + 2;", f"the second placeholder took the typing ({_line(app, 1)!r})")
    app.expect(app.cursors() and app.cursors()[0] == (1, len("tot = tot + 2;") + 1),
               f"the last Tab lands on $0 at the end ({app.cursors()})")
    app.type("x")
    step(app, "type after $0", 0.2)
    app.expect(_line(app, 1) == "tot = tot + 2;x", f"after $0 the session is over: typing is plain ({_line(app, 1)!r})")

    # Choices.
    _expand(app, 2, "pick")
    app.expect(_line(app, 2) == "mode = fast;" and _selected(app) == "fast",
               f"a choice expands to its first option, selected ({_line(app, 2)!r}, {_selected(app)!r})")
    app.key("Tab")
    step(app, "Tab cycles the choice", 0.2)
    app.expect(_line(app, 2) == "mode = safe;" and _selected(app) == "safe",
               f"Tab moves to the next option ({_line(app, 2)!r}, {_selected(app)!r})")
    app.key("Tab")
    app.key("shift+Tab")
    step(app, "Shift+Tab cycles back", 0.2)
    app.expect(_line(app, 2) == "mode = safe;", f"Shift+Tab steps back an option ({_line(app, 2)!r})")
    app.key("Escape")
    step(app, "Escape ends the session", 0.2)
    app.expect(_selected(app) == "safe", f"Escape keeps the chosen option selected ({_selected(app)!r})")
    app.key("Right")
    app.key("Tab")
    step(app, "Tab after Escape", 0.2)
    app.expect(_line(app, 2).startswith("mode = safe") and "fast" not in _line(app, 2)
               and "slow" not in _line(app, 2), f"after Escape Tab does not cycle ({_line(app, 2)!r})")

    # Nesting.
    _expand(app, 3, "nest")
    app.expect(_line(app, 3) == "call(outer(inner));" and _selected(app) == "outer(inner)",
               f"the outer placeholder is selected whole ({_line(app, 3)!r}, {_selected(app)!r})")
    app.key("Tab")
    step(app, "Tab into the nested placeholder", 0.2)
    app.expect(_selected(app) == "inner", f"Tab selects the nested placeholder ({_selected(app)!r})")
    app.type("deep")
    app.key("shift+Tab")
    step(app, "Shift+Tab back to the parent", 0.2)
    app.expect(_selected(app) == "outer(deep)", f"the parent grew with its child ({_selected(app)!r})")
    app.type("flat")
    app.key("Tab")
    step(app, "type over the parent", 0.2)
    app.expect(_line(app, 3) == "call(flat);", f"typing over the parent drops its child ({_line(app, 3)!r})")
    app.expect(app.cursors() and app.cursors()[0] == (3, len("call(flat);") + 1),
               f"Tab goes straight to $0: the dropped child is not a stop ({app.cursors()})")

    # Variables, and one undo for the whole expansion.
    _expand(app, 4, "vars")
    app.expect(_line(app, 4) == "// deep:4 fallback", f"variables resolve; an unknown one takes its default ({_line(app, 4)!r})")
    app.key("ctrl+z")
    step(app, "undo the expansion", 0.2)
    app.expect(_line(app, 4) == "vars", f"one undo restores the prefix ({_line(app, 4)!r})")

    # A caret that leaves the placeholder ends the session.
    _expand(app, 5, "mirr")
    app.click_text(6, 1)
    app.type("q")
    step(app, "click away and type", 0.2)
    app.expect(_line(app, 5) == "name = name + 1;" and _line(app, 6) == "q",
               f"typing elsewhere edits only there ({_line(app, 5)!r}, {_line(app, 6)!r})")
    app.click_text(5, 2)
    app.type("Z")
    step(app, "type into a former mirror", 0.2)
    app.expect(_line(app, 5) == "nZame = name + 1;", f"the mirrors no longer follow ({_line(app, 5)!r})")

    # A snippet completion item.
    app.click_text(7, 1)
    app.type("qs")
    app.key("ctrl+space")
    app.wait_until(lambda: app.ui()["overlay"].get("mode") == "completion", 5.0, "the completion list")
    app.key("Return")
    step(app, "accept a snippet completion", 0.25)
    app.expect(_line(app, 7) == "qs_call(arg, n)" and _selected(app) == "arg",
               f"the item expands as a snippet ({_line(app, 7)!r}, {_selected(app)!r})")
    app.key("Tab")
    step(app, "Tab in the completion's snippet", 0.2)
    app.expect(_selected(app) == "n", f"Tab walks the completion's placeholders ({_selected(app)!r})")
    app.key("Escape")
    app.key("ctrl+s")
    step(app, "save", 0.2)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "lsp-signature-help": flow_signature_help,
    "lsp-code-actions": flow_code_actions,
    "snippets": flow_snippets,
    "snippets-deep": flow_snippets_deep,
    "sticky-scroll": flow_sticky_scroll,
    "themes": flow_themes,
    "tab-overflow": flow_tab_overflow,
    "dbg-conditions": flow_debug_conditions,
}

PLUGINS: dict[str, tuple[str, ...]] = {
    "lsp-signature-help": ("cpp-lsp",),
    "lsp-code-actions": ("cpp-lsp",),
    "dbg-conditions": ("gdb-dap",),
    "snippets": ("qa-snippets",),
    "snippets-deep": ("qa-snippets",),
}
