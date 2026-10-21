"""Editor groups rearranged: Move Editor Group Left/Right/Up/Down, Focus Next
Editor Group and Close Editor Group, from the palette.

`groups-keyboard` splits, focuses and moves TABS between groups; nothing moved a
whole group, so the split tree and the group list (one structure in two
containers: CLAUDE.md, `CheckEditorGroupsMutateWithTheSplitTree`) were never
reordered under a running window. Each group holds one file of its own, so the
group order is read off the files.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, layout_sane, open_file, resize_window, step, write


def _order(app: q.App) -> list[str]:
    """Each group's distinguishing file, in group order."""
    out = []
    for group in app.editor_state().get("groups", []):
        names = [t["title"] for t in group.get("tabs", []) if t["title"].startswith("g-")]
        out.append(names[0] if names else "?")
    return out


def _focused(app: q.App) -> str:
    state = app.editor_state()
    groups = state.get("groups", [])
    index = state.get("focusedGroupIndex", -1)
    if not 0 <= index < len(groups):
        return "?"
    names = [t["title"] for t in groups[index].get("tabs", []) if t["title"].startswith("g-")]
    return names[0] if names else "?"


def _palette(app: q.App, label: str) -> bool:
    app.key("ctrl+shift+p")
    step(app, "palette", 0.3)
    app.type(label)
    step(app, f"type {label}", 0.3)
    row = next((r for r in app.elements("overlay-row") if r.get("label", "").startswith(label)), None)
    if not app.expect(row is not None, f"the palette lists {label!r}"):
        dismiss(app)
        return False
    app.click(*app.center(row))
    step(app, label, 0.4)
    return True


def flow_groups_move(app: q.App) -> None:
    for name in ("a", "b", "c"):
        write(app, f"groups/g-{name}.txt", f"group {name}\n")
    open_file(app, "groups/g-a.txt")
    app.send("split-right", "groups/g-b.txt")
    app.wait_until(lambda: _order(app) == ["g-a.txt", "g-b.txt"], 5.0, "a second group holding g-b")
    app.send("focus", "editor")
    step(app, "two groups", 0.4)
    if not app.expect(_order(app) == ["g-a.txt", "g-b.txt"] and _focused(app) == "g-b.txt",
                      f"two groups, the new one focused ({_order(app)}, focused {_focused(app)})"):
        return

    if _palette(app, "Move Editor Group Left"):
        app.expect(_order(app) == ["g-b.txt", "g-a.txt"] and _focused(app) == "g-b.txt",
                   f"Move Editor Group Left swaps the focused group leftwards and keeps focus on it "
                   f"({_order(app)}, focused {_focused(app)})")
    if _palette(app, "Move Editor Group Left"):
        app.expect(_order(app) == ["g-b.txt", "g-a.txt"],
                   f"at the left edge it stays where it is ({_order(app)})")
    if _palette(app, "Move Editor Group Right"):
        app.expect(_order(app) == ["g-a.txt", "g-b.txt"] and _focused(app) == "g-b.txt",
                   f"Move Editor Group Right puts it back ({_order(app)}, focused {_focused(app)})")
    if _palette(app, "Move Editor Group Up"):
        app.expect(_order(app) == ["g-a.txt", "g-b.txt"],
                   f"with nothing above, Move Editor Group Up changes nothing ({_order(app)})")

    # A third group below the second: up/down within that column.
    app.send("split-down", "groups/g-c.txt")
    app.wait_until(lambda: len(_order(app)) == 3, 5.0, "a third group below")
    app.send("focus", "editor")
    step(app, "three groups", 0.4)
    app.expect(_focused(app) == "g-c.txt", f"the new group below is focused ({_order(app)}, {_focused(app)})")
    if _palette(app, "Move Editor Group Up"):
        app.expect(_order(app).index("g-c.txt") < _order(app).index("g-b.txt") and _focused(app) == "g-c.txt",
                   f"Move Editor Group Up puts it above its neighbour ({_order(app)}, focused {_focused(app)})")
    if _palette(app, "Move Editor Group Down"):
        app.expect(_order(app).index("g-c.txt") > _order(app).index("g-b.txt"),
                   f"Move Editor Group Down puts it back below ({_order(app)})")

    seen = [_focused(app)]
    for _ in range(3):
        if _palette(app, "Focus Next Editor Group"):
            seen.append(_focused(app))
    app.expect(sorted(set(seen)) == ["g-a.txt", "g-b.txt", "g-c.txt"] and seen[0] == seen[-1],
               f"Focus Next Editor Group visits every group and comes round ({seen})")

    before = _order(app)
    closing = _focused(app)
    if _palette(app, "Close Editor Group"):
        app.wait_until(lambda: len(_order(app)) == 2, 5.0, "the group to close")
        after = _order(app)
        app.expect(len(after) == 2 and closing not in after
                   and [g for g in before if g != closing] == after,
                   f"Close Editor Group closes the focused group and keeps the others in order "
                   f"({before} - {closing} -> {after})")
    # Every pane still has its rect (a group without one is never drawn).
    panes = app.ui().get("editors", [])
    app.expect(len(panes) == len(_order(app)), f"each group has a pane on screen ({len(panes)} panes, {_order(app)})")


def _focus_not_stranded(app: q.App, label: str) -> None:
    """Keyboard focus is never on an overlay that is not there."""
    ui = app.ui()
    if ui.get("focus") == "overlay":
        app.expect(ui["overlay"].get("visible") or ui.get("modal") or ui["menu"].get("open")
                   or ui["menu"].get("contextMenu"),
                   f"{label}: focus is on an overlay but none is shown ({ui.get('focus')}, modal {ui.get('modal')})")


# (label, open it, a key that must still reach it after the resizes, what it shows then)
SURFACES: tuple[tuple[str, Callable[[q.App], None], Callable[[q.App], bool]], ...] = (
    ("the command palette", lambda a: a.key("ctrl+shift+p"),
     lambda a: (a.type("zq"), a.settle(0.3), "zq" in str(a.ui()["overlay"].get("query", "")))[-1]),
    ("quick open", lambda a: a.key("ctrl+p"),
     lambda a: (a.type("main"), a.settle(0.3), "main" in str(a.ui()["overlay"].get("query", "")))[-1]),
    ("the find widget", lambda a: a.key("ctrl+f"),
     lambda a: (a.key("ctrl+a"), a.type("total"), a.settle(0.3),
                a.field_text("field:find/search") == "total")[-1]),
    ("Settings", lambda a: a.key("ctrl+comma"),
     lambda a: (a.type("tab size"), a.settle(0.4), any(e["name"] == "setting:editor.tab_size"
                                                        for e in a.elements("setting")))[-1]),
    ("the keyboard shortcuts", lambda a: a.send("keyboard-shortcuts"),
     lambda a: a.ui().get("modal") in ("help", "settings")),
    ("the Edit menu", lambda a: a.click_named("menu:Edit"),
     lambda a: True),
    ("the editor's context menu", lambda a: a.click(*a.text_point(2, 3), button=3),
     lambda a: True),
)


def flow_resize_overlays(app: q.App) -> None:
    """Each overlay, prompt and menu open while the window shrinks to 640x480 and
    grows back: everything stays inside the window, focus never strands on a
    surface that closed, and the surface still takes its keys afterwards."""
    for name in ("src/scheduler.cpp", "src/main.cpp"):
        open_file(app, name)
    app.key("ctrl+grave")
    app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the panel")
    app.send("focus", "editor")
    for label, open_it, still_works in SURFACES:
        resize_window(app, 1440, 900)
        app.send("focus", "editor")
        open_it(app)
        step(app, f"open {label}", 0.4)
        for width, height in ((640, 480), (1024, 640), (1440, 900)):
            resize_window(app, width, height)
            layout_sane(app, f"{label} at {width}x{height}")
            _focus_not_stranded(app, f"{label} at {width}x{height}")
        app.expect(bool(still_works(app)), f"{label} still takes its keys after the resizes ({app.ui()['overlay']}, "
                                           f"modal {app.ui().get('modal')})")
        step(app, f"use {label}", 0.2)
        dismiss(app)
        _focus_not_stranded(app, f"after closing {label}")
    # A prompt: rename from the tree, through a resize, then cancelled.
    app.send("sidebar-show", "tree")
    row = app.wait_for("row:tree/main.cpp", 5.0)
    if row is not None:
        app.click(*app.center(row))
        app.key("F2")
        step(app, "rename prompt", 0.4)
        if app.expect(app.ui().get("modal") == "prompt", f"F2 opens the rename prompt ({app.ui().get('modal')})"):
            for width, height in ((640, 480), (1440, 900)):
                resize_window(app, width, height)
                layout_sane(app, f"the rename prompt at {width}x{height}")
            app.key("End")
            app.type("x")
            app.wait_until(lambda: (app.ui().get("prompt") or {}).get("input", "").endswith("x"), 3.0,
                           "the typed key in the prompt")
            app.expect((app.ui().get("prompt") or {}).get("input", "").endswith("x"),
                       f"the prompt still takes typing ({app.ui().get('prompt')})")
            app.key("Escape")
            step(app, "cancel the rename", 0.3)
            app.expect((app.project / "src" / "main.cpp").exists(), "a cancelled rename renames nothing")
    resize_window(app, 1440, 900)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "groups-move": flow_groups_move,
    "resize-overlays": flow_resize_overlays,
}
