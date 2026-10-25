"""The plugin API through the real UI: one of each provider and event (the QA-only
`qa-api` plugin), each driven the way a person reaches it.

A coverage build of the suite ran almost none of the plugin host's provider paths
(PluginHostPublicApi 30%, PluginProviderQueryInterop 12%: TD-2026-10-18-364) -- the
bundled plugins are language servers, a debugger and presentation demos. The
plugin counts every callback the host makes (qa-api-seen.txt in the project), so
each step checks the callback ran, not only that something appeared.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, messages, open_file, step
from flows_lsp_debug import _hover

SOURCE = "fn greet(name) {\n  let inner = 1\n  greet\n}\n"


def _seen(app: q.App) -> dict[str, int]:
    assert app.project is not None
    path = app.project / "qa-api-seen.txt"
    label = path.read_text() if path.exists() else ""
    out: dict[str, int] = {}
    for part in label.split():
        key, _, value = part.partition("=")
        if value.isdigit():
            out[key] = int(value)
    return out


def _bumped(app: q.App, what: str, before: dict[str, int], message: str, timeout: float = 5.0) -> bool:
    ok = app.wait_until(lambda: _seen(app).get(what, 0) > before.get(what, 0), timeout, message)
    if not ok:
        app.expect(False, f"{what}: before {before.get(what, 0)}, now {_seen(app)}; "
                          f"selection {app.buffer().get('selectedText')!r}, tab {(app.active_tab() or {}).get('title')}")
    return ok


def flow_plugin_api(app: q.App) -> None:
    """Commands (palette, menu, keybinding, control), hover, completion, code
    actions, definition, references, signature help, document symbols, a tree
    sidebar (toggle, confirm), tests, and the buffer events."""
    open_file(app, "notes.qaapi", SOURCE)
    app.expect(app.buffer().get("language") == "qaapi", f"the plugin's language is detected ({app.buffer().get('language')})")

    # Commands, four ways in.
    before = _seen(app)
    reply = app.send("qa.api.ping", "control")
    app.expect(bool(reply.get("ok")), f"the plugin command runs from the control channel ({reply})")
    _bumped(app, "ping", before, "the command's callback")
    bindings = [k for k in app.keybindings() if k.get("plugin") and k.get("command") == "qa.api.ping"]
    app.expect(bool(bindings) and bindings[0].get("chord") == "Ctrl+Alt+U",
               f"the plugin's keybinding is registered ({bindings})")
    before = _seen(app)
    app.click_text(1, 1)
    app.key("ctrl+alt+u")
    _bumped(app, "ping", before, "Ctrl+Alt+U to run the plugin command")
    step(app, "plugin keybinding", 0.2)
    before = _seen(app)
    app.click_named("menu:Help")
    step(app, "Help menu", 0.3)
    row = next((e for e in app.elements("menu-item") if e.get("label", "").startswith("QA API Ping")), None)
    app.expect(row is not None, "the plugin's entry is in the Help menu")
    if row:
        app.click(*app.center(row))
        _bumped(app, "ping", before, "the menu entry to run the command")
    step(app, "plugin menu entry", 0.2)
    dismiss(app)

    # Hover.
    before = _seen(app)
    hover = _hover(app, 1, 4)
    app.expect(hover is not None and "qa hover line 1" in hover.get("text", ""),
               f"the plugin's hover shows ({hover})")
    _bumped(app, "hover", before, "the hover provider")
    app.expect(hover is not None and "fn greet(name) {" in hover.get("text", ""),
               f"the provider reads the line through buffer:line ({hover})")
    x, y, w, h = app.ui()["surfaces"]["sidebar"]
    app.move(x + w / 2, y + h - 20)
    # In a buffer with unsaved edits the provider is asked too, and reads the live
    # line (TD-2026-10-21-374: the host used to skip plugins there).
    app.click_text(1, 1)
    app.key("Home")
    app.type("pub ")
    app.expect(app.buffer().get("dirty") is True, "the buffer has an unsaved edit")
    hover = _hover(app, 1, 8)
    app.expect(hover is not None and "pub fn greet(name) {" in hover.get("text", ""),
               f"the hover reads the unsaved line ({hover})")
    step(app, "hover in an unsaved buffer", 0.2)
    app.move(x + w / 2, y + h - 20)
    app.key("ctrl+z")
    app.wait_until(lambda: app.buffer().get("text", "").startswith("fn greet"), 4.0, "the edit undone")

    # Completion: Ctrl+Space lists the provider's two items; Enter inserts the first.
    before = _seen(app)
    app.click_text(3, 8)
    app.key("End")
    app.type(" ")
    app.key("ctrl+space")
    # The list opens ("Loading…") before the plugin thread answers.
    app.wait_until(lambda: app.ui()["overlay"].get("mode") == "completion"
                   and (app.ui()["overlay"].get("count") or 0) > 0, 5.0, "the completion list's items")
    overlay = app.ui()["overlay"]
    app.expect(overlay.get("count") == 2, f"the provider's two items are listed ({overlay})")
    app.key("Return")
    step(app, "accept completion", 0.3)
    app.expect("greet qa_alpha" in app.buffer()["text"], f"Enter inserts the first item ({app.buffer()['text']!r})")
    _bumped(app, "complete", before, "the completion provider")

    # Code actions: Ctrl+. lists the provider's action; Enter runs its command.
    before = _seen(app)
    app.click_text(2, 7)
    app.key("ctrl+period")
    app.wait_until(lambda: app.ui()["overlay"].get("mode") == "code-actions", 5.0, "the code action list")
    app.key("Return")
    _bumped(app, "action", before, "the code action provider")
    _bumped(app, "ping", before, "the chosen action to run its command")
    step(app, "code action", 0.3)
    dismiss(app)

    # Definition and references.
    before = _seen(app)
    app.click_text(3, 4)
    app.key("F12")
    app.wait_until(lambda: app.cursors() and app.cursors()[0] == (1, 4), 5.0, "F12 to the provider's definition")
    _bumped(app, "definition", before, "the definition provider")
    step(app, "go to definition", 0.3)
    before = _seen(app)
    app.key("shift+F12")
    _bumped(app, "references", before, "the references provider")
    # The channel is lsp.references; asking for "references" read an empty one, and
    # the "or the panel shows output" fallback was what passed.
    app.wait_until(lambda: any("notes.qaapi:3" in line for line in app.output("lsp.references")), 5.0,
                   "the references listed")
    step(app, "find references", 0.3)
    # A click on an output entry's `path:line:col` opens it there.
    geometry = app.ui().get("output") or {}
    entries = app.output(geometry.get("channel", "lsp.references"))
    index = next((i for i, line in enumerate(entries) if "notes.qaapi:3" in line), None)
    if app.expect(bool(geometry) and index is not None,
                  f"the references show in the output panel ({geometry}, {entries[:4]})"):
        visible = index - geometry["firstRow"]
        if app.expect(0 <= visible < geometry["visibleRows"], f"the entry is on screen ({index}, {geometry})"):
            app.click(geometry["textX"] + 20, geometry["firstLineY"] + (visible + 0.5) * geometry["lineHeight"])
            app.wait_until(lambda: app.cursors() and app.cursors()[0] == (3, 5), 3.0, "the click to the reference")
            step(app, "click a reference in the output", 0.3)
            app.expect(app.cursors() and app.cursors()[0] == (3, 5) and app.ui().get("focus") == "editor",
                       f"a click on notes.qaapi:3:5 puts the caret there, in the editor ({app.cursors()})")

    # Signature help (explicit: nothing triggers it while typing, TD-2026-10-20-369).
    before = _seen(app)
    app.click_text(3, 8)
    app.key("ctrl+shift+space")
    app.wait_until(lambda: app.ui()["popups"]["signatureHelp"], 5.0, "signature help")
    app.expect("greet(name, times)" in str(app.ui()["popups"]["signatureHelp"]),
               f"the provider's signature shows ({app.ui()['popups']['signatureHelp']})")
    _bumped(app, "signature", before, "the signature help provider")
    app.key("Escape")

    # Document symbols: the outline lists the provider's tree; a row goes there.
    before = _seen(app)
    app.send("sidebar-show", "outline")
    app.wait_until(lambda: any(e["name"] == "row:outline/inner" for e in app.elements("row")), 5.0,
                   "the outline from the provider")
    _bumped(app, "symbols", before, "the document symbols provider")
    app.click_named("row:outline/inner")
    app.wait_until(lambda: app.cursors() and app.cursors()[0][0] == 2, 5.0, "an outline row to go to its line")
    step(app, "outline", 0.3)

    # A tree sidebar: the twisty toggles (the plugin owns the state), a row confirms.
    app.send("sidebar-show", "qa-api")
    app.wait_until(lambda: any(e["name"] == "row:qa-api/Leaf" for e in app.elements("row")), 5.0,
                   "the plugin sidebar's rows")
    before = _seen(app)
    app.click_named("row:qa-api/Leaf")
    _bumped(app, "confirm", before, "a click to confirm the row")
    step(app, "confirm a sidebar row", 0.3)
    before = _seen(app)
    app.send("sidebar-show", "qa-api")
    app.click_named("row:qa-api/Root")
    app.key("Left")
    _bumped(app, "toggle", before, "Left on the root to collapse it")
    app.wait_until(lambda: not any(e["name"] == "row:qa-api/Leaf" for e in app.elements("row")), 5.0,
                   "the leaf to go with its collapsed parent")
    step(app, "toggle a sidebar row", 0.3)

    # Tests: discover in the active file, run what was found.
    open_file(app, "notes.qaapi")
    before = _seen(app)
    reply = app.send("tests-discover")
    app.expect(bool(reply.get("ok")), f"tests-discover runs ({reply})")
    _bumped(app, "discover", before, "the test provider's discover")
    reply = app.send("tests-run")
    app.expect(bool(reply.get("ok")), f"tests-run runs ({reply})")
    _bumped(app, "run", before, "the test provider's run")
    step(app, "tests", 0.3)

    # Buffer events: typing, the caret, a selection, closing the buffer.
    before = _seen(app)
    app.click_text(2, 3)
    app.type("x")
    _bumped(app, "change", before, "on_buffer_change after typing")
    _bumped(app, "cursor", before, "on_cursor_move after the click")
    app.key("shift+End")
    _bumped(app, "selection", before, "on_selection_change after a selection")
    app.key("ctrl+z")
    app.key("ctrl+s")
    step(app, "buffer events", 0.3)
    before = _seen(app)
    app.key("ctrl+w")
    _bumped(app, "close", before, "on_buffer_close when the tab closes")


EDIT_SOURCE = "abc one\nline two\nline three\n"


def flow_plugin_editor_api(app: q.App) -> None:
    """`ctx.editor` from a plugin command: apply_edits makes two edits and moves
    the caret as ONE undo step, an unopened path is refused and changes nothing,
    set_cursor / set_selection move the caret and select; set_ghost_text paints a
    suggestion that Tab accepts (one undo step) and Escape dismisses; notify shows
    a notification. No QA plugin called any of them (TD-364: PluginRuntimeApiInterop
    at 12 %)."""
    open_file(app, "notes.qaapi", EDIT_SOURCE)
    app.send_line("set-setting plugins.ghost_text true")
    app.click_text(3, 1)
    step(app, "focus the buffer", 0.2)

    def run(command: str) -> dict:
        reply = app.send(command, "control")
        step(app, command, 0.2)
        return reply

    run("qa.api.edit")
    edited = "EDITc one\n-- line two\nline three\n"
    app.wait_until(lambda: app.text() == edited, 3.0, "apply_edits to land")
    app.expect(app.text() == edited, f"apply_edits makes both edits ({app.text()!r})")
    app.expect(app.cursors()[:1] == [(2, 4)], f"and puts the caret where it asked ({app.cursors()})")
    app.key("ctrl+z")
    step(app, "undo the plugin edit", 0.2)
    app.expect(app.text() == EDIT_SOURCE, f"one undo takes both edits back ({app.text()!r})")

    reply = run("qa.api.edit.closed")
    app.expect(app.text() == EDIT_SOURCE, f"an edit to an unopened path changes nothing ({app.text()!r}, {reply})")

    run("qa.api.cursor")
    app.expect(app.cursors()[:1] == [(3, 2)], f"set_cursor moves the caret ({app.cursors()})")
    run("qa.api.select")
    selected = app.buffer().get("selectedText")
    app.expect(selected == "abc ", f"set_selection selects columns 1..5 of line 1 ({selected!r})")

    # Ghost text at the caret: Tab accepts it, Escape dismisses it.
    app.click_text(3, 11)
    app.key("End")
    run("qa.api.ghost")
    app.key("Tab")
    step(app, "Tab accepts the suggestion", 0.2)
    app.expect(app.text().splitlines()[2] == "line threeGHOST",
               f"Tab inserts the ghost text at the caret ({app.text().splitlines()[2:3]})")
    app.key("ctrl+z")
    step(app, "undo the accepted suggestion", 0.2)
    app.expect(app.text() == EDIT_SOURCE, f"one undo takes the suggestion back ({app.text()!r})")
    app.key("End")
    run("qa.api.ghost")
    app.key("Escape")
    step(app, "Escape dismisses the suggestion", 0.2)
    app.key("Tab")
    step(app, "Tab after the dismissal", 0.2)
    app.expect("GHOST" not in app.text(), f"after Escape, Tab does not insert it ({app.text().splitlines()[2:3]})")
    app.key("ctrl+z")
    run("qa.api.ghost")
    run("qa.api.ghost.clear")
    app.key("Tab")
    step(app, "Tab after clear_ghost_text", 0.2)
    app.expect("GHOST" not in app.text(), f"clear_ghost_text takes it away ({app.text().splitlines()[2:3]})")
    app.key("ctrl+z")

    run("qa.api.notify")
    app.wait_until(lambda: any("qa-api says hello" in m for m in messages(app)), 3.0, "the notification")
    app.expect(any("qa-api says hello" in m for m in messages(app)),
               f"ctx.notify shows a notification ({messages(app)[-3:]})")
    app.check("plugin editor api")


def flow_plugin_process_api(app: q.App) -> None:
    """`ctx.process` from a plugin command: run returns stdout and the exit code,
    takes stdin, reports a failing program's exit code and stderr, refuses a cwd
    outside the project (an error the plugin can catch), and run_async calls back
    with the result. No QA plugin ran a process (TD-364: PluginProcessInterop at
    0 %)."""
    assert app.project is not None
    open_file(app, "notes.qaapi", EDIT_SOURCE)
    log = app.project / "qa-api-process.txt"

    def lines() -> list[str]:
        return log.read_text().splitlines() if log.exists() else []

    reply = app.send("qa.api.run", "control")
    app.wait_until(lambda: any(line.startswith("async") for line in lines()), 5.0, "run_async to call back")
    got = lines()
    app.expect(bool(reply.get("ok")), f"the command runs ({reply})")
    for expected in ("run ok=true exit=0 out=hello", "stdin out=piped", "fail ok=false exit=4 err=oops",
                     "async exit=3 out=async"):
        app.expect(expected in got, f"{expected!r} in the plugin's record ({got})")
    escape = next((line for line in got if line.startswith("escape")), "")
    app.expect(escape.startswith("escape ok=false") and "escapes" in escape,
               f"a cwd outside the project is refused with an error the plugin catches ({escape!r})")
    app.check("plugin process api")


FLOWS: dict[str, Callable[[q.App], None]] = {"plugin-api": flow_plugin_api,
                                             "plugin-editor-api": flow_plugin_editor_api,
                                             "plugin-process-api": flow_plugin_process_api}
PLUGINS: dict[str, tuple[str, ...]] = {"plugin-api": ("qa-api",), "plugin-editor-api": ("qa-api",),
                                       "plugin-process-api": ("qa-api",)}
