"""The file tree's operations where they collide: a rename onto a name that
exists, a new file or folder over one that exists, a reserved name, a file
made inside directories that do not exist yet, a folder deleted with a file
open from it. Each refusal is a toast that says why; each success is on disk.

`LocalTreeOps` ran at a third in the coverage build (TD-2026-10-18-364): the
happy paths are the tree-keys and tree-file-ops flows', the refusals were
reached by nothing.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import active_title, dismiss, git, messages, open_file, read, step, wait_toasts_clear, write


def _rows(app: q.App) -> list[str]:
    return [e["name"] for e in app.elements("row", prefix="row:tree/")]


def _select(app: q.App, row: str) -> bool:
    app.click_named("sidebar-view:tree")
    if not app.wait_until(lambda: f"row:tree/{row}" in _rows(app), 5.0, f"{row} in the tree"):
        return False
    app.click_named(f"row:tree/{row}")
    app.settle(0.15)
    return True


def _prompt_and_type(app: q.App, text: str, label: str) -> bool:
    if not app.wait_until(lambda: app.ui().get("modal") == "prompt" and (app.ui().get("prompt") or {}).get("kind") == "input",
                          3.0, f"the prompt ({label})"):
        return False
    app.key("ctrl+a")
    app.type(text)
    app.key("Return")
    step(app, label, 0.3)
    return True


def _context(app: q.App, row: str, item: str) -> None:
    element = app.element(f"row:tree/{row}")
    app.click(*app.center(element), button=3)
    app.settle(0.2)
    app.click_named(f"menu-item:context/{item}")


def flow_tree_collisions(app: q.App) -> None:
    """Refusals that say why, and creations that make their directories."""
    assert app.project is not None
    write(app, "col/a.txt", "a\n")
    write(app, "col/b.txt", "b\n")
    write(app, "col/sub/inner.txt", "inner\n")
    open_file(app, "col/a.txt")
    app.send("tree-refresh")
    if not _select(app, "a.txt"):
        return
    wait_toasts_clear(app)
    app.key("F2")
    if _prompt_and_type(app, "b.txt", "rename a.txt onto b.txt"):
        app.wait_until(lambda: bool(messages(app)), 3.0, "a toast for the refused rename")
        app.expect(any("already exists" in m for m in messages(app)),
                   f"a rename onto an existing name is refused and says why ({messages(app)})")
        app.expect((app.project / "col" / "a.txt").exists() and (app.project / "col" / "b.txt").read_text() == "b\n",
                   "and neither file changed")
    dismiss(app)

    wait_toasts_clear(app)
    _context(app, "col", "New File…")
    if _prompt_and_type(app, "b.txt", "new file over an existing one"):
        app.wait_until(lambda: bool(messages(app)), 3.0, "a toast for the refused file")
        app.expect(any("already exists" in m for m in messages(app)),
                   f"a new file over an existing one is refused ({messages(app)})")
        app.expect((app.project / "col" / "b.txt").read_text() == "b\n", "and the existing file is untouched")
    dismiss(app)

    _context(app, "col", "New Folder…")
    if _prompt_and_type(app, "sub", "new folder over an existing one"):
        # By message, not by count: the refusal can replace the file refusal's toast
        # while that one is still up (one toast per source), leaving the count alone.
        app.wait_until(lambda: any("create folder" in m.lower() and "already exists" in m
                                   for m in messages(app)), 3.0, "a toast for the refused folder")
        app.expect(any("create folder" in m.lower() and "already exists" in m for m in messages(app)),
                   f"a new folder over an existing one is refused ({messages(app)})")
        app.expect((app.project / "col" / "sub" / "inner.txt").exists(), "and the folder keeps its contents")
    dismiss(app)

    # A reserved name: the prompt keeps the name for correction and makes nothing
    # (it says nothing, which VS Code's inline validation would; TD-2026-10-18-364).
    tabs = len(app.query("tabs"))
    _context(app, "col", "New File…")
    if _prompt_and_type(app, "..", "a reserved name"):
        app.expect(app.ui().get("modal") == "prompt" and (app.ui().get("prompt") or {}).get("input") == "..",
                   f"a reserved name is refused and the prompt stays for correction ({app.ui().get('modal')})")
        app.expect(len(app.query("tabs")) == tabs, "and nothing was made")
    dismiss(app)

    # A name that climbs out of the folder (a `..` component): the same refusal,
    # and nothing lands outside.
    _context(app, "col", "New File…")
    if _prompt_and_type(app, "x/../../../escaped.txt", "a name that escapes the folder"):
        app.expect(app.ui().get("modal") == "prompt", "a name with `..` in it is refused and the prompt stays")
        app.expect(not (app.project.parent / "escaped.txt").exists() and not (app.project / "escaped.txt").exists(),
                   "and nothing was written outside the folder")
    dismiss(app)

    _context(app, "col", "New File…")
    if _prompt_and_type(app, "deep/er/new.txt", "a file inside directories that do not exist"):
        app.wait_until(lambda: (app.project / "col" / "deep" / "er" / "new.txt").exists(), 3.0, "the file and its directories")
        app.expect((app.project / "col" / "deep" / "er" / "new.txt").exists(), "the directories are made on the way")
        app.wait_until(lambda: (app.active_tab() or {}).get("title") == "new.txt", 3.0, "the new file opened")
        app.expect((app.active_tab() or {}).get("title") == "new.txt", "and the new file is the active tab")
    dismiss(app)

    open_file(app, "col/sub/inner.txt")
    if _select(app, "sub"):
        app.key("Delete")
        if app.wait_until(lambda: (app.ui().get("prompt") or {}).get("kind") == "confirm", 3.0, "the delete prompt"):
            delete = next((e for e in app.elements("button", prefix="button:prompt/") if e["name"].endswith("/Delete")), None)
            if delete is not None:
                app.click(*app.center(delete))
            app.wait_until(lambda: not (app.project / "col" / "sub").exists(), 5.0, "the folder to go")
            app.wait_until(lambda: all(t.get("title") != "inner.txt" for t in app.query("tabs")), 3.0,
                           "the open file's tab to close with its folder")
            step(app, "delete a folder with an open file", 0.4)
            app.expect(not (app.project / "col" / "sub").exists(), "the folder is gone")
            app.expect(all(t.get("title") != "inner.txt" for t in app.query("tabs")), "the tab of the file inside it closed")
    dismiss(app)


def _rename(app: q.App, row: str, new_name: str) -> bool:
    app.click_named("sidebar-view:tree")
    app.settle(0.2)
    if f"row:tree/{row}" not in _rows(app) and "row:tree/ren" in _rows(app):
        app.click_named("row:tree/ren")  # expand the fixture's folder
        app.settle(0.3)
    if not _select(app, row):
        return False
    app.key("F2")
    if not app.wait_until(lambda: app.ui().get("modal") == "prompt", 3.0, f"the rename prompt for {row}"):
        return False
    app.key("ctrl+a")
    app.type(new_name)
    app.key("Return")
    return True


def _tab_titles(app: q.App) -> list[tuple[int, str]]:
    return [(gi, t.get("title", "")) for gi, g in enumerate(app.editor_state().get("groups", []))
            for t in g.get("tabs", [])]


def flow_rename_open_tabs(app: q.App) -> None:
    """A rename under every kind of open tab: a file's diff follows it; a dirty
    file open in two editor groups asks first, and Save writes the edit under the
    new name with both tabs following; Discard renames the file as it is on disk
    and reopens the tab there, the edit dropped.

    `tree-file-ops` renames clean files in one group; the retargeting of a diff
    tab, of a second group's tab and of a discarded buffer ran at a quarter in the
    coverage build (TD-2026-10-18-364)."""
    assert app.project is not None
    write(app, "ren/diffme.txt", "one\ntwo\n")
    git(app, "add", "ren/diffme.txt")
    git(app, "commit", "-q", "-m", "qa: rename fixture")
    write(app, "ren/diffme.txt", "one\nTWO\n")
    app.key("ctrl+shift+g")
    row = app.wait_for("row:git/diffme.txt", 5.0)
    if app.expect(row is not None, "the git view lists diffme.txt"):
        app.click(*app.center(row))
        app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 5.0, "the diff to open")
        step(app, "the diff of diffme.txt", 0.3)
        if app.expect(_rename(app, "diffme.txt", "diffed.txt"), "renaming diffme.txt from the tree"):
            app.wait_until(lambda: (app.project / "ren" / "diffed.txt").exists(), 3.0, "the rename on disk")
            step(app, "rename under the diff", 0.4)
            compare = [t for g in app.editor_state().get("groups", []) for t in g.get("tabs", [])
                       if t.get("kind") == "compare"]
            app.expect(len(compare) == 1 and "diffed.txt" in compare[0].get("title", "")
                       and "diffme.txt" not in compare[0].get("title", ""),
                       f"the diff tab follows the rename ({[t.get('title') for t in compare]})")

    # A dirty file in two groups: Save, then rename.
    open_file(app, "ren/split.txt", "split\n")
    app.send("split-right")
    app.wait_until(lambda: sum(1 for _, t in _tab_titles(app) if t == "split.txt") == 2, 3.0,
                   "split.txt in both groups")
    app.send("focus", "editor")
    app.click_text(1, 1)
    app.key("End")
    app.type(" edited")
    app.wait_until(lambda: app.buffer().get("dirty"), 3.0, "the edit to dirty the buffer")
    if app.expect(_rename(app, "split.txt", "split2.txt"), "renaming split.txt from the tree"):
        if app.wait_until(lambda: app.ui().get("modal") == "unsaved-prompt", 3.0, "the unsaved prompt before the rename"):
            step(app, "rename a dirty file", 0.3)
            # "Save all": the file is open in two groups, two dirty editors.
            save = next((e for e in app.elements("button", prefix="button:prompt/")
                         if e["name"].split("/")[-1].startswith("Save")), None)
            if not app.expect(save is not None, "the prompt offers Save"):
                return
            app.click(*app.center(save))
            app.wait_until(lambda: (app.project / "ren" / "split2.txt").exists(), 3.0, "the rename after the save")
            step(app, "saved, then renamed", 0.4)
            titles = _tab_titles(app)
            app.expect(sum(1 for _, t in titles if t == "split2.txt") == 2 and not any(t == "split.txt" for _, t in titles),
                       f"both groups' tabs follow the rename ({titles})")
            app.expect(read(app, "ren/split2.txt") == "split edited\n" and not (app.project / "ren" / "split.txt").exists(),
                       f"the edit is saved under the new name ({read(app, 'ren/split2.txt')!r})")
            app.expect(not app.buffer().get("dirty"), "and the buffer is clean")

    # A dirty file: Discard, then rename.
    app.send("focus-group", "1")
    open_file(app, "ren/drop.txt", "keep me\n")
    app.send("focus", "editor")
    app.click_text(1, 1)
    app.key("End")
    app.type(" dropped")
    app.wait_until(lambda: app.buffer().get("dirty"), 3.0, "the edit to dirty the buffer")
    if app.expect(_rename(app, "drop.txt", "drop2.txt"), "renaming drop.txt from the tree"):
        if app.wait_until(lambda: app.ui().get("modal") == "unsaved-prompt", 3.0, "the unsaved prompt before the rename"):
            app.click_named("button:prompt/Discard")
            app.wait_until(lambda: (app.project / "ren" / "drop2.txt").exists(), 3.0, "the rename after the discard")
            step(app, "discarded, then renamed", 0.4)
            titles = _tab_titles(app)
            app.expect(any(t == "drop2.txt" for _, t in titles) and not any(t == "drop.txt" for _, t in titles),
                       f"the tab follows the rename ({titles})")
            app.expect(read(app, "ren/drop2.txt") == "keep me\n", f"the file is renamed as it was on disk ({read(app, 'ren/drop2.txt')!r})")
            app.send("focus", "editor")
            app.expect(active_title(app) == "drop2.txt" and app.text() == "keep me\n" and not app.buffer().get("dirty"),
                       f"the tab shows the file as it is, the edit dropped ({active_title(app)}, {app.text()!r})")


def flow_tree_cancel_transactions(app: q.App) -> None:
    """Cancel rename, creation and deletion after entering a real name: neither
    disk nor the open dirty buffer changes, and its undo history still works."""
    assert app.project is not None
    path = "cancel-tree/keep.txt"
    original = "keep me\n"
    open_file(app, path, original)
    app.key("End")
    app.type("CHANGED")
    edited = "keep meCHANGED\n"
    app.send("tree-refresh")
    if not _select(app, "keep.txt"):
        return
    for operation in ("rename", "new-file", "new-folder", "delete"):
        if operation == "rename":
            app.key("F2")
        elif operation == "delete":
            app.key("Delete")
        else:
            _context(app, "cancel-tree", "New File…" if operation == "new-file" else "New Folder…")
        if not app.wait_until(lambda: app.ui().get("modal") == "prompt", 3.0,
                              f"{operation}: prompt opens"):
            return
        if operation != "delete":
            app.key("ctrl+a")
            app.type("abandoned.txt" if operation != "new-folder" else "abandoned-folder")
        app.key("Escape")
        step(app, f"cancel tree {operation}")
        app.expect(not app.ui().get("modal"), f"{operation}: Escape closes the prompt")
        app.expect(read(app, path) == original, f"{operation}: cancel leaves disk untouched")
        app.expect(not (app.project / "cancel-tree/abandoned.txt").exists()
                   and not (app.project / "cancel-tree/abandoned-folder").exists(),
                   f"{operation}: the abandoned name creates nothing")
        app.expect(app.text() == edited and app.buffer().get("dirty"),
                   f"{operation}: the dirty buffer keeps its unsaved edit")
        app.expect(active_title(app) == "keep.txt", f"{operation}: the open tab retains its identity")
        if not _select(app, "keep.txt"):
            return
    app.send("focus", "editor")
    app.key("ctrl+z")
    step(app, "undo after canceled tree operations")
    app.expect(app.text() == original and not app.buffer().get("dirty"),
               "canceling tree operations preserves the editor's undo/save point")
    app.key("ctrl+shift+z")
    app.key("ctrl+s")
    app.wait_until(lambda: read(app, path) == edited and not app.buffer().get("dirty"),
                   3.0, "the same buffer saves normally after canceled operations")


def _maybe_read(app: q.App, relative: str) -> str | None:
    """`relative`'s text, or None while it does not exist."""
    assert app.project is not None
    path = app.project / relative
    return path.read_text() if path.is_file() else None


def flow_tree_clipboard(app: q.App) -> None:
    """The tree's Cut / Copy / Paste (TD-2026-10-18-359), by Ctrl+C/X/V and by the
    row's menu: a copy pasted beside its source is "name copy.ext", then "name
    copy 2.ext"; pasted into a folder it keeps its name; a cut file moves, its open
    dirty tab following with the unsaved text; a cut pastes once; moving a folder
    into itself is refused with a toast."""
    write(app, "clip/a.txt", "alpha\n")
    write(app, "clip/dest/keep.txt", "keep\n")
    app.send("tree-refresh")
    app.send("sidebar-show", "tree")
    _select(app, "clip")  # a click on a folder expands it
    app.key("Right")
    if not _select(app, "a.txt"):
        app.expect(False, "the tree lists clip/a.txt")
        return
    app.key("ctrl+c")
    app.key("ctrl+v")
    app.wait_until(lambda: _maybe_read(app, "clip/a copy.txt") == "alpha\n", 3.0, "Ctrl+C, Ctrl+V to copy a.txt beside itself")
    app.expect(_maybe_read(app, "clip/a copy.txt") == "alpha\n", "a copy beside its source is 'a copy.txt'")
    _context(app, "a.txt", "Copy")
    _context(app, "a.txt", "Paste")
    app.wait_until(lambda: _maybe_read(app, "clip/a copy 2.txt") == "alpha\n", 3.0, "the menu's Copy, Paste to copy again")
    app.expect(_maybe_read(app, "clip/a copy 2.txt") == "alpha\n", "the next copy is 'a copy 2.txt'")
    _context(app, "dest", "Paste")
    app.wait_until(lambda: _maybe_read(app, "clip/dest/a.txt") == "alpha\n", 3.0, "Paste on a folder")
    app.expect(_maybe_read(app, "clip/dest/a.txt") == "alpha\n", "pasted into a folder, a copy keeps its name")

    # Cut a file with unsaved edits open, paste it into dest.
    open_file(app, "clip/a copy.txt")
    app.key("ctrl+End")
    app.type("unsaved")
    step(app, "edit a copy.txt", 0.2)
    if not _select(app, "a copy.txt"):
        return
    app.key("ctrl+x")
    app.settle(0.2)
    app.expect(_maybe_read(app, "clip/a copy.txt") == "alpha\n", "cutting moves nothing yet")
    _select(app, "dest")
    app.key("ctrl+v")
    app.wait_until(lambda: _maybe_read(app, "clip/dest/a copy.txt") == "alpha\n", 3.0, "Ctrl+X, Ctrl+V to move the file")
    step(app, "move", 0.3)
    app.expect(_maybe_read(app, "clip/dest/a copy.txt") == "alpha\n" and _maybe_read(app, "clip/a copy.txt") is None,
               "the cut file moved, its saved bytes with it")
    tab = app.active_tab() or {}
    app.expect(tab.get("title") == "a copy.txt" and tab.get("dirty"), f"its open tab follows it, still dirty ({tab})")
    app.expect("unsaved" in (app.text() or ""), "with its unsaved text")
    app.key("ctrl+z")

    # A folder cannot be moved into itself.
    app.send("sidebar-show", "tree")
    _select(app, "dest")
    app.key("ctrl+x")
    app.key("Right")  # expand dest to reach a file inside it
    _select(app, "keep.txt")
    app.key("ctrl+v")
    app.wait_until(lambda: any("into itself" in m for m in messages(app)), 3.0, "the refusal toast")
    app.expect(any("into itself" in m for m in messages(app)), f"moving a folder into itself says why ({messages(app)})")
    app.expect(_maybe_read(app, "clip/dest/keep.txt") == "keep\n", "and moves nothing")
    dismiss(app)


def flow_tree_drag(app: q.App) -> None:
    """A tree row dragged (TD-2026-10-24-386), as VS Code's explorer drags: onto a
    pane's right edge it opens in a new pane carved from it; onto a pane's tab
    strip it opens in that pane; onto a folder it moves there (its open tab
    following) and with Ctrl held copies; onto the terminal it types its quoted
    path. A click on a row still opens it."""
    write(app, "drag/a.txt", "alpha\n")
    write(app, "drag/b c.txt", "beta\n")
    write(app, "drag/dest/keep.txt", "keep\n")
    open_file(app, "README.md")
    app.send("tree-refresh")
    app.send("sidebar-show", "tree")
    _select(app, "drag")
    app.key("Right")
    app.settle(0.2)
    if not app.wait_until(lambda: "row:tree/a.txt" in _rows(app), 5.0, "a.txt in the tree"):
        return

    def center(name: str) -> tuple[float, float]:
        return app.center(app.element(f"row:tree/{name}"))

    pane = app.ui()["editors"][0]["rect"]
    # Onto the pane's right edge: a new pane on the right holds a.txt.
    x0, y0 = center("a.txt")
    app.drag(x0, y0, pane[0] + pane[2] - 10, pane[1] + pane[3] / 2, steps=10)
    app.wait_until(lambda: len(app.editor_state()["groups"]) == 2, 3.0, "a drop on the right edge to split")
    groups = app.editor_state()["groups"]
    app.expect(len(groups) == 2 and [t["title"] for t in groups[1]["tabs"]] == ["a.txt"],
               f"a drop on a pane's right edge opens the file in a new pane ({[[t['title'] for t in g['tabs']] for g in groups]})")

    # Onto the first pane's tab strip: opened in that pane.
    strip = app.ui()["surfaces"]["tabStrip"]
    x0, y0 = center("b c.txt")
    app.drag(x0, y0, strip[0] + 40, strip[1] + strip[3] / 2, steps=10)
    app.wait_until(lambda: any(t["title"] == "b c.txt" for t in app.editor_state()["groups"][0]["tabs"]), 3.0,
                   "a drop on the tab strip to open the file there")
    app.expect(any(t["title"] == "b c.txt" for t in app.editor_state()["groups"][0]["tabs"]),
               "a drop on a pane's tab strip opens the file in that pane")

    # Onto a folder with Ctrl: copied; without: moved, its open tab following.
    app.send("sidebar-show", "tree")
    app.settle(0.2)
    x0, y0 = center("b c.txt")
    dx, dy = center("dest")
    app.drag(x0, y0, dx, dy, steps=10, modifiers=("ctrl",))
    app.wait_until(lambda: _maybe_read(app, "drag/dest/b c.txt") == "beta\n", 3.0, "a Ctrl drop to copy into dest")
    app.expect(_maybe_read(app, "drag/dest/b c.txt") == "beta\n" and _maybe_read(app, "drag/b c.txt") == "beta\n",
               "a Ctrl drop on a folder copies the file there")
    x0, y0 = center("a.txt")
    dx, dy = center("dest")
    app.drag(x0, y0, dx, dy, steps=10)
    app.wait_until(lambda: _maybe_read(app, "drag/dest/a.txt") == "alpha\n", 3.0, "a drop to move into dest")
    app.expect(_maybe_read(app, "drag/dest/a.txt") == "alpha\n" and _maybe_read(app, "drag/a.txt") is None,
               "a drop on a folder moves the file there")
    app.expect(any(t["title"] == "a.txt" for t in app.editor_state()["groups"][1]["tabs"]),
               "and its open tab follows it")

    # Onto the terminal: its quoted path is typed at the prompt.
    if not app.ui()["panel"]["visible"]:
        app.send("term-toggle")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    app.send("sidebar-show", "tree")
    app.settle(0.3)
    panel = app.ui()["surfaces"]["bottomPanel"]
    x0, y0 = center("b c.txt")
    app.drag(x0, y0, panel[0] + panel[2] / 2, panel[1] + panel[3] / 2, steps=10)
    app.wait_until(lambda: "b c.txt'" in app.query("terminal-output").get("text", ""), 5.0,
                   "the quoted path typed into the terminal")
    app.expect("b c.txt'" in app.query("terminal-output").get("text", ""),
               "a drop on the terminal types the file's quoted path")
    app.key("ctrl+u")  # leave the shell's line empty
    dismiss(app)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "tree-collisions": flow_tree_collisions,
    "tree-cancel-transactions": flow_tree_cancel_transactions,
    "rename-open-tabs": flow_rename_open_tabs,
    "tree-clipboard": flow_tree_clipboard,
    "tree-drag": flow_tree_drag,
}
