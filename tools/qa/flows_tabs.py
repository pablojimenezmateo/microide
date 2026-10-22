"""The editor tab's own menu, for real, and a file opened twice.

`context-menus` presses every row of the tab's context menu under the action
dry run: it proves the row reaches its command, not what the command does. Here
each row runs and its effect is read back -- which tabs a Close row closes
(relative to the tab right-clicked, not the active one, as in VS Code), that a
dirty tab among them asks first, what Copy Path put on the clipboard, where
Reveal in File Tree left the tree, what Compare with Selected compares.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import active_title, dismiss, open_file, step, titles, tree_rows


def _tab_menu(app: q.App, title: str, row: str) -> bool:
    """Right-clicks `title`'s tab in group 0 and clicks `row` in its menu."""
    tab = app.wait_for(f"tab:0/{title}", 3.0)
    if not tab:
        return False
    app.click(*app.center(tab), button=3)
    item = app.wait_for(f"menu-item:Tabs/{row}", 3.0)
    if not app.expect(item is not None, f"the tab menu of {title} has {row!r}"):
        dismiss(app)
        return False
    if not app.expect(item.get("enabled", True), f"{row!r} is enabled on {title}"):
        dismiss(app)
        return False
    app.click(*app.center(item))
    step(app, f"{title}: {row}", 0.3)
    return True


def _clipboard(app: q.App) -> str:
    """What the clipboard holds, read back by pasting into a fresh buffer that is
    then discarded."""
    app.send("focus", "editor")
    app.key("ctrl+n")
    step(app, "scratch buffer", 0.2)
    app.key("ctrl+v")
    step(app, "paste", 0.2)
    pasted = app.text()
    app.key("ctrl+z")
    app.key("ctrl+w")
    step(app, "close the scratch buffer", 0.3)
    discard = next((e for e in app.elements("button") if e["name"] == "button:prompt/Discard"), None)
    if discard:
        app.click(*app.center(discard))
        step(app, "discard", 0.2)
    return pasted


def flow_tab_context_menu(app: q.App) -> None:
    """Every row of the editor tab's menu, run: Close Tabs to the Right and to the
    Left relative to the tab right-clicked; Close Other Tabs asking for a dirty one
    (Cancel keeps it, Discard closes it); Copy Relative / Absolute Path pasted
    back; Reveal in File Tree showing the file in a collapsed tree; Select for
    Compare then Compare with Selected opening their diff; Close All Tabs."""
    for name in ("a", "b", "c", "d", "e"):
        open_file(app, f"tabs/{name}.txt", f"{name}\n")
    start = [t for t in titles(app) if t.endswith(".txt")]
    app.expect(start == ["a.txt", "b.txt", "c.txt", "d.txt", "e.txt"], f"five tabs ({start})")

    # Relative to the right-clicked tab (c), not to the active one (e).
    if _tab_menu(app, "c.txt", "Close Tabs to the Right"):
        app.expect(not {"d.txt", "e.txt"} & set(titles(app)) and "c.txt" in titles(app),
                   f"Close Tabs to the Right closes d and e, keeps c ({titles(app)})")
    if _tab_menu(app, "b.txt", "Close Tabs to the Left"):
        app.expect(titles(app)[:1] == ["b.txt"],
                   f"Close Tabs to the Left closes everything before b ({titles(app)})")

    # A dirty tab among those a Close row closes asks first (VS Code).
    open_file(app, "tabs/c.txt")
    app.key("ctrl+End")
    app.type("dirty")
    step(app, "dirty c", 0.2)
    if _tab_menu(app, "b.txt", "Close Other Tabs"):
        prompt = app.ui().get("prompt") or {}
        app.expect(prompt.get("kind") == "unsaved" and "c.txt" in prompt.get("message", ""),
                   f"closing a dirty c.txt asks first ({prompt})")
        cancel = app.wait_for("button:prompt/Cancel", 2.0)
        if cancel:
            app.click(*app.center(cancel))
            step(app, "cancel", 0.3)
        app.expect("c.txt" in titles(app), f"Cancel keeps the dirty tab ({titles(app)})")
    if _tab_menu(app, "b.txt", "Close Other Tabs"):
        discard = app.wait_for("button:prompt/Discard", 2.0)
        if discard:
            app.click(*app.center(discard))
            step(app, "discard", 0.3)
        app.wait_until(lambda: titles(app) == ["b.txt"], 3.0, "Close Other Tabs to leave only b")
        app.expect((app.project / "tabs/c.txt").read_text() == "c\n", "Discard leaves c.txt on disk as it was")

    # The paths, read back from the clipboard.
    if _tab_menu(app, "b.txt", "Copy Relative Path"):
        pasted = _clipboard(app)
        app.expect(pasted == "tabs/b.txt", f"Copy Relative Path copies tabs/b.txt ({pasted!r})")
    if _tab_menu(app, "b.txt", "Copy Absolute Path"):
        pasted = _clipboard(app)
        app.expect(pasted == str(app.project / "tabs" / "b.txt"),
                   f"Copy Absolute Path copies the absolute path ({pasted!r})")

    # Reveal in File Tree, with the tree collapsed and on another view.
    app.send("sidebar-show", "tree")
    collapse = next((e for e in app.elements("button") if e["name"] == "button:sidebar/Collapse"), None)
    if collapse:
        app.click(*app.center(collapse))
        step(app, "collapse the tree", 0.3)
    app.expect("row:tree/b.txt" not in tree_rows(app), f"the collapsed tree hides b.txt ({tree_rows(app)})")
    app.send("sidebar-show", "search")
    if _tab_menu(app, "b.txt", "Reveal in File Tree"):
        ui = app.ui()
        app.expect(ui["sidebar"]["visible"] and ui["sidebar"]["view"] == "tree",
                   f"Reveal shows the file tree ({ui['sidebar']})")
        row = next((e for e in app.elements("row", prefix="row:tree/b.txt")), None)
        app.expect(row is not None and row.get("selected"),
                   f"the tree expands to b.txt and selects it ({row})")

    # Select for Compare, then Compare with Selected on another tab.
    open_file(app, "tabs/a.txt", "a\nshared\n")
    open_file(app, "tabs/b.txt", "b\nshared\n")
    if _tab_menu(app, "a.txt", "Select for Compare") and _tab_menu(app, "b.txt", "Compare with Selected"):
        tab = app.active_tab() or {}
        app.expect(tab.get("kind") == "compare", f"Compare with Selected opens a diff ({tab})")
        editor = next((t for g in app.editor_state()["groups"] for t in g["tabs"] if t.get("active") and t.get("kind") == "compare"), {})
        labels = (editor.get("leftLabel", ""), editor.get("rightLabel", ""))
        app.expect("a.txt" in labels[0] and "b.txt" in labels[1],
                   f"the selected file is the left side, the clicked one the right ({labels})")

    # Close All Tabs.
    first = next((t for t in titles(app)), None)
    if first and _tab_menu(app, first, "Close All Tabs"):
        app.wait_until(lambda: titles(app) == [], 3.0, "Close All Tabs to close every tab")


def flow_tab_open_twice(app: q.App) -> None:
    """Opening a file that is open activates its tab rather than adding another;
    with two groups, Quick Open opens into the focused group (VS Code), and the
    same file then shows in both, one buffer: typing in one is in the other."""
    open_file(app, "twice/one.txt", "one\n")
    open_file(app, "twice/two.txt", "two\n")
    open_file(app, "twice/one.txt")
    app.expect(titles(app).count("one.txt") == 1 and active_title(app) == "one.txt",
               f"a second open activates the open tab ({titles(app)}, active {active_title(app)})")
    app.key("ctrl+backslash")
    step(app, "split right", 0.3)
    groups = app.editor_state()["groups"]
    app.expect(len(groups) == 2 and groups[1].get("focused"), f"the split is focused ({len(groups)} groups)")
    app.key("ctrl+p")
    step(app, "quick open", 0.2)
    app.type("two.txt")
    app.wait_until(lambda: (app.ui()["overlay"].get("count") or 0) > 0, 3.0, "Quick Open to find two.txt")
    app.key("Return")
    app.wait_until(lambda: active_title(app) == "two.txt", 3.0, "two.txt to open")
    step(app, "open from quick open", 0.3)
    focused = next((g for g in app.editor_state()["groups"] if g.get("focused")), {})
    app.expect(focused.get("index") == 1 and any(t["title"] == "two.txt" for t in focused.get("tabs", [])),
               f"Quick Open opens into the focused group ({focused.get('index')}: "
               f"{[t['title'] for t in focused.get('tabs', [])]})")
    app.send("focus", "editor")
    app.key("ctrl+End")
    app.type("typed in group 2")
    step(app, "type in the right pane", 0.2)
    app.key("ctrl+1")
    step(app, "focus group 1", 0.2)
    app.send("open", "twice/two.txt")
    app.wait_until(lambda: active_title(app) == "two.txt", 3.0, "two.txt in group 1")
    app.expect("typed in group 2" in app.text(),
               f"the file is one buffer in both groups ({app.text()!r})")
    app.key("ctrl+z")
    step(app, "undo in group 1", 0.2)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "tab-context-menu": flow_tab_context_menu,
    "tab-open-twice": flow_tab_open_twice,
}
