"""The control channel's own buttons and a few focus commands -- what the coverage
ledger found only the smoke pass had run: a toast's button and an editor
banner's choices pressed by name (`notification-action`, `banner-action`, what
an agent driving the app uses instead of a click), Show Tree, focus between
stacked editor groups, the previous project, and relaunching a terminal."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Callable

import microide_qa as q
from flows_common import active_root, dismiss, open_file, read, step, terminal_tabs, write


def _banner(app: q.App) -> list[str]:
    return [e["name"].split("/", 1)[1] for e in app.elements("button") if e["name"].startswith("button:banner/")]


def flow_notification_banner_commands(app: q.App) -> None:
    """A plugin's failed setup toasts with a Show Errors button; notification-action
    presses it by label: the Plugin Errors channel shows and the toast is gone. A
    file changed on disk under an edit raises the banner; banner-action Keep keeps
    the edit and drops the banner, Compare opens the disk against the buffer,
    Reload takes the disk's text, Overwrite writes the buffer; a banner-action
    with no banner up is refused."""
    app.wait_until(lambda: any(n.get("key") == "plugins.load-errors" for n in app.query("notifications")),
                   5.0, "the qa-broken plugin's error toast")
    toast = next((n for n in app.query("notifications") if n.get("key") == "plugins.load-errors"), None)
    if toast:
        labels = [a.get("label") for a in toast.get("actions", [])]
        app.expect("Show Errors" in labels, f"the toast offers Show Errors ({labels})")
        reply = app.send("notification-action", toast["key"], "show errors")
        app.expect(bool(reply.get("ok")), f"notification-action presses it, label case-insensitive ({reply})")
        step(app, "show errors", 0.3)
        app.expect(app.ui()["panel"]["visible"] and app.query("output").get("shown") == "plugins.error",
                   f"the Plugin Errors channel shows ({app.query('output').get('shown')})")
        app.expect(not any(n.get("key") == "plugins.load-errors" for n in app.query("notifications")),
                   "the toast is dismissed by its button")
    reply = app.send("notification-action", "no-such-toast", "0")
    app.expect(not reply.get("ok"), f"a button on a toast that is not there is refused ({reply})")

    def clash(content: str) -> bool:
        write(app, "banner/note.txt", content)
        return app.wait_until(lambda: "Keep" in _banner(app), 5.0, "the external-change banner")

    open_file(app, "banner/note.txt", "mine\n")
    app.key("End")
    app.type(" edited")
    step(app, "edit", 0.2)
    if clash("theirs one\n"):
        app.send("banner-action", "keep")
        step(app, "keep", 0.3)
        app.expect(not _banner(app) and app.buffer().get("dirty") and "mine edited" in app.text(),
                   f"Keep drops the banner and keeps the edit ({_banner(app)}, {app.text()!r})")
    if clash("theirs two\n"):
        app.send("banner-action", "Compare")
        app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 3.0, "Compare to open a diff")
        step(app, "compare", 0.4)
        app.key("ctrl+w")
        step(app, "close the diff", 0.3)
        open_file(app, "banner/note.txt")
    if "Reload" in _banner(app) or clash("theirs three\n"):
        app.send("banner-action", "Reload")
        app.wait_until(lambda: app.text().startswith("theirs"), 3.0, "Reload to take the disk's text")
        app.expect(not app.buffer().get("dirty"), "a reloaded buffer is clean")
    app.key("End")
    app.type(" again")
    if clash("theirs four\n"):
        app.send("banner-action", "Overwrite")
        app.wait_until(lambda: read(app, "banner/note.txt").endswith(" again\n"), 3.0, "Overwrite to write the buffer")
        app.expect(not _banner(app), "the banner goes with the overwrite")
    reply = app.send("banner-action", "Keep")
    app.expect(not reply.get("ok"), f"banner-action with no banner up is refused ({reply})")


def flow_focus_commands(app: q.App) -> None:
    """Show Tree brings the file tree from another view; with a split down,
    focus-group-up and -down move the keyboard between the stacked groups (and
    typing lands in the one focused); Previous Project cycles back through two
    projects; Relaunch Terminal starts a fresh shell after the old one exits."""
    app.send("sidebar-show", "search")
    app.send("tree")
    step(app, "show tree", 0.3)
    app.expect(app.ui()["sidebar"]["view"] == "tree", f"Show Tree shows the file tree ({app.ui()['sidebar']})")

    open_file(app, "focus/top.txt", "top\n")
    app.send("split-down")
    step(app, "split down", 0.3)
    open_file(app, "focus/bottom.txt", "bottom\n")
    focused = lambda: next((g["index"] for g in app.editor_state()["groups"] if g.get("focused")), -1)  # noqa: E731
    app.expect(focused() == 1, f"the new group below is focused ({focused()})")
    app.send("focus-group-up")
    step(app, "focus up", 0.2)
    app.expect(focused() == 0, f"focus-group-up moves to the group above ({focused()})")
    app.send("focus", "editor")
    app.key("ctrl+End")
    app.type("UP")
    step(app, "type above", 0.2)
    app.expect(read(app, "focus/top.txt") == "top\n" and "UP" in app.text() and app.buffer()["path"].endswith("top.txt"),
               f"typing lands in the group above ({app.buffer().get('path')})")
    app.key("ctrl+z")
    app.send("focus-group-down")
    step(app, "focus down", 0.2)
    app.expect(focused() == 1, f"focus-group-down moves back below ({focused()})")

    assert app.project is not None
    second = app.project.parent / "second-project"
    second.mkdir(exist_ok=True)
    (second / "readme.txt").write_text("second\n")
    first_root = active_root(app)
    app.send("project-open", str(second))
    app.wait_until(lambda: active_root(app) == str(second), 5.0, "the second project")
    app.send("project-prev")
    app.wait_until(lambda: active_root(app) == first_root, 5.0, "Previous Project to go back to the first")
    step(app, "previous project", 0.4)
    app.send("project-prev")
    app.wait_until(lambda: active_root(app) == str(second), 5.0, "and wrap round to the second")
    app.send("project-prev")
    app.wait_until(lambda: active_root(app) == first_root, 5.0, "the first again")

    app.send("term")
    app.send("focus", "panel")
    app.wait_until(lambda: bool(terminal_tabs(app)), 5.0, "a terminal")
    app.type("exit\n")
    app.wait_until(lambda: not any(p.get("running") for t in terminal_tabs(app) for p in t.get("panes", [])
                                   if t.get("active")), 5.0, "the shell to exit")
    app.send("term-relaunch")
    app.wait_until(lambda: any(p.get("running") for t in terminal_tabs(app) for p in t.get("panes", [])
                               if t.get("active")), 5.0, "Relaunch Terminal to start a shell")
    app.type("echo relaunched-$((6*7))\n")
    app.wait_until(lambda: "relaunched-42" in app.query("terminal-output").get("text", ""), 5.0,
                   "the relaunched shell to run a command")


def _opened(app: q.App) -> list[str]:
    path = app.work / "run" / "xdg-open-args"
    return path.read_text().splitlines() if path.exists() else []


def flow_desktop_opener(app: q.App) -> None:
    """What the app hands the desktop opener (`tools/qa/fakebin/xdg-open` records
    it): Open URL passes an http(s) URL through exactly and refuses a file: or
    javascript: one without starting anything; Show in File Explorer on a tree
    file or folder hands over the folder that contains it (VS Code's Reveal in
    File Explorer on Linux)."""
    url = "https://example.com/qa?x=1&y=two"
    reply = app.send("open-url", url)
    app.expect(bool(reply.get("ok")), f"Open URL runs for an https URL ({reply})")
    app.wait_until(lambda: url in _opened(app), 3.0, "the opener to get the URL as given")
    for bad in ("file:///etc/passwd", "javascript:alert(1)"):
        before = len(_opened(app))
        reply = app.send("open-url", bad)
        app.expect(not reply.get("ok"), f"Open URL refuses {bad} ({reply})")
        app.expect(len(_opened(app)) == before, f"and starts no opener for it ({_opened(app)[before:]})")

    assert app.project is not None
    app.send("sidebar-show", "tree")
    for row, row_label, expected in (("row:tree/src", "src", app.project),
                                     ("row:tree/README.md", "README.md", app.project)):
        target = app.wait_for(row, 3.0)
        if not target:
            continue
        app.click(*app.center(target), button=3)
        item = app.wait_for("menu-item:context/Show in File Explorer", 3.0)
        if not app.expect(item is not None, f"{row_label}'s menu has Show in File Explorer"):
            dismiss(app)
            continue
        before = len(_opened(app))
        app.click(*app.center(item))
        app.wait_until(lambda: len(_opened(app)) > before, 3.0, f"Show in File Explorer for {row_label}")
        handed = _opened(app)[before:]
        app.expect(handed[:1] == [str(expected)], f"{row_label}: the opener gets {expected} ({handed})")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "notification-banner-commands": flow_notification_banner_commands,
    "focus-commands": flow_focus_commands,
    "desktop-opener": flow_desktop_opener,
}
PLUGINS: dict[str, tuple[str, ...]] = {"notification-banner-commands": ("qa-broken",)}
FAKEBIN = Path(__file__).resolve().parent / "fakebin"
ENV: dict[str, dict[str, str]] = {
    "desktop-opener": {"PATH": f"{FAKEBIN}:{os.environ.get('PATH', '/usr/bin:/bin')}"},
}
