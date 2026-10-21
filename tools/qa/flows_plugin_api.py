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
from flows_common import dismiss, open_file, step
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
    x, y, w, h = app.ui()["surfaces"]["sidebar"]
    app.move(x + w / 2, y + h - 20)

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
    app.wait_until(lambda: "notes.qaapi:3" in app.query("output", {"channel": "references"}).get("text", "")
                   or app.ui()["panel"].get("content") == "output", 5.0, "the references listed")
    step(app, "find references", 0.3)

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


FLOWS: dict[str, Callable[[q.App], None]] = {"plugin-api": flow_plugin_api}
PLUGINS: dict[str, tuple[str, ...]] = {"plugin-api": ("qa-api",)}
