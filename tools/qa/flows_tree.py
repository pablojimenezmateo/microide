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
from flows_common import dismiss, messages, open_file, step, write


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

    before = len(messages(app))
    _context(app, "col", "New Folder…")
    if _prompt_and_type(app, "sub", "new folder over an existing one"):
        app.wait_until(lambda: len(messages(app)) > before, 3.0, "a toast for the refused folder")
        app.expect(any("already exists" in m for m in messages(app)[before:]),
                   f"a new folder over an existing one is refused ({messages(app)[before:]})")
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


FLOWS: dict[str, Callable[[q.App], None]] = {
    "tree-collisions": flow_tree_collisions,
}
