"""Editing transactions across selections, save points, tabs and a restart.

Use exact text and disk receipts: a correct repaint alone cannot establish that
an edit hit the intended buffer or that undo restored the selected newline.
"""
from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import expect_text, open_file, read, step


def _save(app: q.App, path: str, expected: str) -> None:
    app.key("ctrl+s")
    app.wait_until(lambda: not app.buffer().get("dirty") and read(app, path) == expected,
                   3.0, f"{path}: save clears dirty and writes the expected text")
    step(app, f"save {path}")


def flow_selection_transactions(app: q.App) -> None:
    """Replace and delete empty, whole-file and newline-spanning selections;
    undo must restore text AND selection, redo must repeat the same transaction."""
    app.send("set-setting", "editor.save.ensure_final_newline", "false")
    original = "alpha beta\ngamma delta\nomega"
    cases = (
        ("word", ("ctrl+Home",) + ("Right",) * 6 + ("shift+Right",) * 4, "X", "alpha X\ngamma delta\nomega"),
        ("newline", ("ctrl+Home", "End", "shift+Right"), "X", "alpha betaXgamma delta\nomega"),
        ("cross-line", ("ctrl+Home", "End", "shift+Down"), "X", "alpha betaXa\nomega"),
        ("all", ("ctrl+a",), "X", "X"),
        ("delete-all", ("ctrl+a",), "", ""),
    )
    for name, keys, replacement, expected in cases:
        path = f"transactions/{name}.txt"
        open_file(app, path, original)
        for key in keys:
            app.key(key)
        selected = app.buffer().get("selectedText", "")
        if not app.expect(bool(selected), f"{name}: keyboard creates a nonempty selection"):
            continue
        # Check navigation separately so a misplaced selection cannot validate itself.
        if name == "word":
            app.expect(selected == "beta", f"Shift+Right selects four characters ({selected!r})")
        elif name == "newline":
            app.expect(selected == "\n", f"Shift+Right at EOL selects the newline ({selected!r})")
        elif name == "cross-line":
            app.expect(selected == "\ngamma delt", f"Shift+Down keeps the preferred column ({selected!r})")
        else:
            app.expect(selected == original, "Select All includes every character")
        if replacement:
            app.type(replacement)
        else:
            app.key("BackSpace")
        step(app, f"replace {name}")
        expect_text(app, expected, f"{name}: replacement splices the selected range")
        app.expect(read(app, path) == original, f"{name}: editing leaves disk untouched")
        app.key("ctrl+z")
        step(app, f"undo {name}")
        expect_text(app, original, f"{name}: one undo restores the original")
        app.expect(app.buffer().get("selectedText") == selected, f"{name}: undo restores the selection")
        app.expect(not app.buffer().get("dirty"), f"{name}: undo returns to the save point")
        app.key("ctrl+shift+z")
        step(app, f"redo {name}")
        expect_text(app, expected, f"{name}: redo repeats the replacement")
        _save(app, path, expected)


def flow_tab_save_transactions(app: q.App) -> None:
    """Two dirty files keep independent history, including an undo across save;
    closing a clean tab and reopening it must read its own saved content."""
    app.send("set-setting", "editor.save.ensure_final_newline", "false")
    paths = ("transactions/left.txt", "transactions/right.txt")
    for path, text in zip(paths, ("left", "right")):
        open_file(app, path, text)
        app.key("ctrl+End")
        app.type("CHANGED")
        step(app, f"dirty {path}")
    for path, text in zip(paths, ("left", "right")):
        open_file(app, path)
        expect_text(app, text + "CHANGED", f"{path}: switching preserves the unsaved buffer")
        app.expect(read(app, path) == text, f"{path}: the other tab's edits never write this file")
        _save(app, path, text + "CHANGED")
        app.key("ctrl+z")
        step(app, f"undo saved edit in {path}")
        expect_text(app, text, f"{path}: undo is local to this tab")
        app.expect(app.buffer().get("dirty"), f"{path}: undo across save is dirty")
        app.expect(read(app, path) == text + "CHANGED", f"{path}: undo does not alter disk")
        app.key("ctrl+shift+z")
        step(app, f"redo to save point in {path}")
        expect_text(app, text + "CHANGED", f"{path}: redo restores saved content")
        app.expect(not app.buffer().get("dirty"), f"{path}: redo reaches the save point")
        app.key("ctrl+w")
        step(app, f"close clean {path}")
        app.expect(not app.ui().get("prompt"), f"{path}: a clean tab closes without a prompt")
        open_file(app, path)
        expect_text(app, text + "CHANGED", f"{path}: reopen reads saved text")


def flow_path_identity_restart(app: q.App) -> None:
    """Same basenames, spaces, Unicode and punctuation must retain distinct
    buffers and disk identities through save, hot exit and a second save."""
    app.send("set-setting", "editor.save.ensure_final_newline", "false")
    paths = ("identity/one/shared name.txt", "identity/two/shared name.txt",
             "identity/café [draft] #1.txt")
    for index, path in enumerate(paths):
        open_file(app, path, f"file {index}\n")
        app.key("ctrl+End")
        app.type(f"saved {index}")
        _save(app, path, f"file {index}\nsaved {index}")
        app.type(f" pending {index}")
        step(app, f"unsaved tail {index}")
    app.restart()
    step(app, "restore distinct path identities")
    for index, path in enumerate(paths):
        open_file(app, path)
        expected = f"file {index}\nsaved {index} pending {index}"
        expect_text(app, expected, f"{path}: hot exit restores the correct unsaved buffer")
        app.expect(app.buffer().get("dirty"), f"{path}: restored unsaved content stays dirty")
        app.expect(read(app, path) == f"file {index}\nsaved {index}", f"{path}: hot exit leaves disk alone")
        _save(app, path, expected)
    for index, path in enumerate(paths):
        app.expect(read(app, path) == f"file {index}\nsaved {index} pending {index}",
                   f"{path}: saving another same-named tab never overwrites this file")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "edit-selection-transactions": flow_selection_transactions,
    "edit-tab-save-transactions": flow_tab_save_transactions,
    "file-path-identity-restart": flow_path_identity_restart,
}
