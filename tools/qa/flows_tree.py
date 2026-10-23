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
from flows_common import active_title, dismiss, git, messages, open_file, read, step, write


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
    before = len(messages(app))
    app.key("F2")
    if _prompt_and_type(app, "b.txt", "rename a.txt onto b.txt"):
        app.wait_until(lambda: len(messages(app)) > before, 3.0, "a toast for the refused rename")
        app.expect(any("already exists" in m for m in messages(app)[before:]),
                   f"a rename onto an existing name is refused and says why ({messages(app)[before:]})")
        app.expect((app.project / "col" / "a.txt").exists() and (app.project / "col" / "b.txt").read_text() == "b\n",
                   "and neither file changed")
    dismiss(app)

    before = len(messages(app))
    _context(app, "col", "New File…")
    if _prompt_and_type(app, "b.txt", "new file over an existing one"):
        app.wait_until(lambda: len(messages(app)) > before, 3.0, "a toast for the refused file")
        app.expect(any("already exists" in m for m in messages(app)[before:]),
                   f"a new file over an existing one is refused ({messages(app)[before:]})")
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


FLOWS: dict[str, Callable[[q.App], None]] = {
    "tree-collisions": flow_tree_collisions,
    "tree-cancel-transactions": flow_tree_cancel_transactions,
    "rename-open-tabs": flow_rename_open_tabs,
}
