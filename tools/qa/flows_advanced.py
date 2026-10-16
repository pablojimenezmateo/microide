"""Advanced editor and tool flows: signature help, code actions, snippets,
sticky scroll, themes, an overflowing tab strip, and the debugger's breakpoint
conditions and watch expressions.
"""

from __future__ import annotations

import shutil
from typing import Callable

import microide_qa as q
from flows_common import dismiss, expect_text, open_file, step, write
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


FLOWS: dict[str, Callable[[q.App], None]] = {
    "lsp-signature-help": flow_signature_help,
    "lsp-code-actions": flow_code_actions,
    "snippets": flow_snippets,
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
}
