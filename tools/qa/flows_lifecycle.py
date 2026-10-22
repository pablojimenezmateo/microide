"""Things that come and go while the app runs: plugins reloaded.

A plugin's contributions are registered once at start; Reload Plugins tears the
host down and registers them again. Nothing in the suite reloaded, so a
contribution registered twice (a second keybinding, menu row, completion item or
status item) or lost (a sidebar view, a language) after a reload went unseen --
and editing a plugin and reloading it is the plugin author's whole loop.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, open_file, step
from flows_lsp_debug import _hover
from flows_plugin_api import SOURCE, _seen


def _contributions(app: q.App) -> dict[str, object]:
    """What the qa-api plugin contributed, as the app reports it."""
    app.click_named("menu:Help")
    step(app, "Help menu", 0.3)
    menu_rows = [e["label"] for e in app.elements("menu-item") if e.get("label", "").startswith("QA API Ping")]
    dismiss(app)
    app.click_named("button:sidebar/more")
    step(app, "the sidebar's more views", 0.3)
    views = sorted(e.get("label", "") for e in app.elements("menu-item") if "QA" in e.get("label", ""))
    dismiss(app)
    return {
        "keybindings": sorted(k.get("chord", "") for k in app.keybindings()
                              if k.get("plugin") and k.get("command") == "qa.api.ping"),
        "menu rows": menu_rows,
        "status items": sorted(e["name"] for e in app.elements("status") if e["name"].startswith("status:plugin/qa-api/")),
        "sidebar views": views,
        "commands": sorted(c["command"] for c in app.query("commands") if c["command"].startswith("qa.api.")),
    }


def _completion_count(app: q.App) -> int:
    """The completion list's length after a space (the word before the caret
    would filter the provider's items); the space is taken back after."""
    app.click_text(3, 8)
    app.key("End")
    app.type(" ")
    app.key("ctrl+space")
    # The list opens ("Loading…") before the plugin thread answers.
    app.wait_until(lambda: app.ui()["overlay"].get("mode") == "completion"
                   and (app.ui()["overlay"].get("count") or 0) > 0, 5.0, "the completion list's items")
    count = int(app.ui()["overlay"].get("count") or 0)
    app.key("Escape")
    app.key("BackSpace")
    # Saved, so the buffer is clean again: a plugin's hover is not asked for a
    # buffer with unsaved edits (it would read the disk; TD-2026-10-21-374).
    app.key("ctrl+s")
    step(app, "close completion", 0.2)
    app.expect(app.ui()["focus"] == "editor",
               f"Escape on the completion list gives focus back to the editor ({app.ui()['focus']})")
    return count


def _reload(app: q.App) -> bool:
    app.key("ctrl+shift+p")
    step(app, "palette", 0.3)
    app.type("reload plugins")
    step(app, "type reload plugins", 0.3)
    row = next((r for r in app.elements("overlay-row") if r.get("label", "").startswith("Reload Plugins")), None)
    if not app.expect(row is not None, "the palette lists Reload Plugins"):
        dismiss(app)
        return False
    app.click(*app.center(row))
    step(app, "Reload Plugins", 1.0)
    return app.wait_until(lambda: any(e["name"].startswith("status:plugin/qa-api/") for e in app.elements("status")),
                          10.0, "the plugin's status item back after the reload")


def flow_plugins_reload(app: q.App) -> None:
    """Reload Plugins twice: every contribution there exactly once after each, the
    language still detected, the providers answering; then the plugin's source
    edited on disk and reloaded, and the edit in effect."""
    open_file(app, "notes.qaapi", SOURCE)
    baseline = _contributions(app)
    app.expect(all(baseline.values()), f"the plugin contributed everything this flow counts ({baseline})")
    completions = _completion_count(app)
    app.expect(completions == 2, f"the provider lists its two completion items ({completions})")

    for round_ in (1, 2):
        if not _reload(app):
            return
        open_file(app, "notes.qaapi")
        after = _contributions(app)
        for key, value in baseline.items():
            app.expect(after[key] == value, f"after reload {round_}, the plugin's {key} are as before "
                                            f"({value} -> {after[key]})")
        app.expect(app.buffer().get("language") == "qaapi",
                   f"after reload {round_}, the plugin's language is still detected ({app.buffer().get('language')})")
        count = _completion_count(app)
        app.expect(count == completions, f"after reload {round_}, the completion list holds {completions} items, "
                                         f"not {count}")
        before = _seen(app)
        app.click_text(1, 1)
        app.key("ctrl+alt+u")
        app.wait_until(lambda: _seen(app).get("ping", 0) != before.get("ping", 0), 5.0,
                       f"after reload {round_}, the keybinding to run the reloaded command")
        step(app, f"ping after reload {round_}", 0.2)

    # The author's loop: edit the plugin, reload, see the edit.
    source = app.work / "config" / "microide" / "plugins" / "qa-api" / "init.lua"
    if not app.expect(source.exists(), f"the seeded plugin is at {source}"):
        return
    source.write_text(source.read_text().replace('"qa hover line "', '"qa hover (edited) line "'))
    if not _reload(app):
        return
    open_file(app, "notes.qaapi")
    hover = _hover(app, 1, 4)
    app.expect(hover is not None and "qa hover (edited) line 1" in hover.get("text", ""),
               f"the edited plugin's hover shows after a reload ({hover})")
    x, y, w, h = app.ui()["surfaces"]["sidebar"]
    app.move(x + w / 2, y + h - 20)


def flow_plugin_errors(app: q.App) -> None:
    """A plugin that gets its registrations wrong (the qa-broken plugin): each
    malformed one is refused with a message naming what is missing, and a setup
    that raises -- at startup, not on a reload the user asked for -- is shown to
    the user as a warning naming the plugin and why, with a button to the Plugin
    Errors channel. It used to be written to that channel and nowhere else."""
    assert app.project is not None
    path = app.project / "qa-broken-errors.txt"
    app.wait_until(path.exists, 10.0, "the plugin to record its refusals")
    lines = dict(line.split(": ", 1) for line in path.read_text().splitlines() if ": " in line) \
        if path.exists() else {}
    expected = {
        "completion": "language_id",
        "menu": "Toolz",
        "keybinding": "key",
        "setting": "type",
        "status": "id",
        "formatter": "empty",
        "code_action": "language_id",
    }
    for name, needle in expected.items():
        got = lines.get(name, "(not attempted)")
        app.expect(got != "accepted" and needle in got,
                   f"a {name} registration without what it needs is refused, saying so ({got!r})")

    # A setup that raises leaves nothing of the plugin registered (its good
    # command included) -- and says so, as a warning naming plugin and reason,
    # with a button to the Plugin Errors channel.
    reply = app.send("qa.broken.ok")
    app.expect(not reply.get("ok"), f"a plugin whose setup failed registers nothing ({reply})")

    def warning() -> dict | None:
        return next((n for n in app.query("notifications")
                     if "qa-broken" in n.get("message", "") and "Nowhere" in n.get("message", "")), None)
    app.wait_until(lambda: warning() is not None, 10.0, "the failed setup to be reported")
    note = warning()
    if app.expect(note is not None, f"a warning names the plugin and the reason "
                                    f"({[n.get('message') for n in app.query('notifications')]})"):
        button = next((e for e in app.elements("button", prefix="button:notification/")
                       if e["name"].endswith("/Show Errors")), None)
        if app.expect(button is not None, "the warning offers Show Errors"):
            app.click(*app.center(button))
            app.wait_until(lambda: (app.ui().get("output") or {}).get("channel") == "plugins.error", 3.0,
                           "Show Errors to open the Plugin Errors channel")
            app.expect((app.ui().get("output") or {}).get("channel") == "plugins.error",
                       f"Show Errors opens the Plugin Errors channel ({app.ui().get('output')})")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "plugins-reload": flow_plugins_reload,
    "plugin-errors": flow_plugin_errors,
}
PLUGINS: dict[str, tuple[str, ...]] = {"plugins-reload": ("qa-api",), "plugin-errors": ("qa-broken",)}
