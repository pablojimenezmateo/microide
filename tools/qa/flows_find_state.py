"""The find widget's state as the editor under it changes: another tab, another
group, a reload from disk, the widget closed and F3 pressed anyway. The widget
stays open across a tab switch (VS Code too), so its count, its highlights and
the match F3 steps to must follow the buffer now in front -- they once kept the
previous file's, and F3 then jumped to that file's coordinates in this one."""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import expect_text, open_file, read, step, write


def _count(app: q.App) -> int | None:
    return app.ui()["overlay"].get("count")


def flow_find_across_tabs(app: q.App) -> None:
    """Open the widget over one file, switch to another with it open (by command,
    by Ctrl+PageDown and by clicking the tab): each time the count is the new
    file's and F3 selects the new file's match. Back on the first file, the count
    is its own again."""
    open_file(app, "find/one.txt", "xx foo\nyy foo\nzz foo\n")
    open_file(app, "find/two.txt", "a\nb\nc\nd\nfoo here\n")
    app.send("open", "find/one.txt")
    step(app, "one.txt", 0.3)
    app.key("ctrl+f")
    app.type("foo")
    step(app, "find foo", 0.3)
    app.wait_until(lambda: _count(app) == 3, 2.0, "three matches in one.txt")
    app.send("open", "find/two.txt")
    step(app, "switch to two.txt", 0.3)
    app.expect(app.ui()["overlay"].get("visible") and _count(app) == 1,
               f"the open widget counts two.txt's one match ({app.ui()['overlay']})")
    app.key("ctrl+Home")
    app.key("F3")
    step(app, "F3 in two.txt", 0.3)
    app.expect(app.cursors() == [(5, 4)] and app.buffer().get("selectedText") == "foo",
               f"F3 selects two.txt's foo ({app.cursors()}, {app.buffer().get('selectedText')!r})")
    app.key("ctrl+Prior")
    step(app, "ctrl+pageup back to one.txt", 0.3)
    app.expect((app.active_tab() or {}).get("title") == "one.txt" and _count(app) == 3,
               f"back on one.txt the count is its three ({app.ui()['overlay']})")
    tab = app.element("tab:0/two.txt")
    if app.expect(bool(tab), "two.txt has a tab"):
        app.click(*app.center(tab))
        step(app, "click two.txt's tab", 0.3)
        app.expect(_count(app) == 1, f"a tab click updates the count too ({app.ui()['overlay']})")


def flow_find_next_without_widget(app: q.App) -> None:
    """Escape closes the widget and keeps the query: F3 and Shift+F3 still step
    through its matches in this file and in another one, without reopening the
    widget (VS Code)."""
    open_file(app, "find/a.txt", "alpha x\nbeta alpha\nalpha\n")
    open_file(app, "find/b.txt", "zz alpha\n")
    app.send("open", "find/a.txt")
    step(app, "a.txt", 0.3)
    app.key("ctrl+f")
    app.type("alpha")
    step(app, "find alpha", 0.3)
    app.key("Escape")
    step(app, "close the widget", 0.3)
    start = app.cursors()
    app.key("F3")
    step(app, "F3", 0.3)
    app.expect(app.cursors() != start and app.buffer().get("selectedText") == "alpha"
               and not app.ui()["overlay"].get("visible"),
               f"F3 steps to the next alpha with the widget closed ({start} -> {app.cursors()})")
    app.key("shift+F3")
    step(app, "Shift+F3", 0.3)
    app.expect(app.cursors() == start or app.buffer().get("selectedText") == "alpha",
               f"Shift+F3 steps back ({app.cursors()})")
    app.send("open", "find/b.txt")
    step(app, "b.txt", 0.3)
    app.key("ctrl+Home")
    app.key("F3")
    step(app, "F3 in b.txt", 0.3)
    app.expect(app.cursors() == [(1, 9)] and app.buffer().get("selectedText") == "alpha",
               f"F3 in another file finds the query there ({app.cursors()})")


def flow_find_in_split_groups(app: q.App) -> None:
    """One file in two groups: Replace All typed in one group's widget changes the
    other pane's text too (one document), one undo there takes it back, and the
    other group's own widget counts what is now in the file. A reload from disk
    under an open widget recounts."""
    original = "alpha x\nbeta alpha\nalpha\n"
    open_file(app, "find/split.txt", original)
    app.key("ctrl+backslash")
    step(app, "split right", 0.4)
    app.key("ctrl+h")
    step(app, "replace widget", 0.3)
    app.key("ctrl+a")
    app.type("alpha")
    app.key("Tab")
    app.type("OMEGA")
    step(app, "query and replacement", 0.3)
    app.key("ctrl+Return")
    step(app, "replace all", 0.4)
    replaced = original.replace("alpha", "OMEGA")
    expect_text(app, replaced, "Replace All rewrites every match")
    app.key("Escape")
    app.send("focus-group", "1")
    step(app, "focus the first group", 0.3)
    expect_text(app, replaced, "the other group shows the same document")
    app.key("ctrl+f")
    app.key("ctrl+a")
    app.type("OMEGA")
    step(app, "find in the first group", 0.3)
    app.expect(_count(app) == 3, f"the first group's widget counts the replaced text ({app.ui()['overlay']})")
    app.key("Escape")
    app.key("ctrl+z")
    step(app, "undo in the first group", 0.3)
    expect_text(app, original, "one undo in either group takes Replace All back")
    app.key("ctrl+f")
    app.key("ctrl+a")
    app.type("alpha")
    step(app, "find alpha", 0.3)
    app.send("save")
    step(app, "save", 0.4)
    write(app, "find/split.txt", "alpha alpha alpha alpha\n")
    app.wait_until(lambda: app.text() == "alpha alpha alpha alpha\n", 6.0, "the clean buffer to reload")
    app.expect(app.wait_until(lambda: _count(app) == 4, 3.0, "the widget to recount after the reload"),
               f"a reload under the open widget recounts ({app.ui()['overlay']})")
    app.expect(read(app, "find/split.txt") == "alpha alpha alpha alpha\n", "the disk is what was written")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "find-across-tabs": flow_find_across_tabs,
    "find-next-without-widget": flow_find_next_without_widget,
    "find-in-split-groups": flow_find_in_split_groups,
}
