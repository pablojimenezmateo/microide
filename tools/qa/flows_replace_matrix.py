"""Find/replace boundary cases through the widget, with exact edit receipts."""
from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import expect_text, open_file, read, step


def flow_replace_matrix(app: q.App) -> None:
    """Literal punctuation, adjacent and overlapping matches, deletion, capture
    groups, word/case filters, empty buffers and absent matches. Reset all widget
    options per case; they deliberately survive closing and reopening Find."""
    # name, input, query, replacement, enabled options, count, output
    cases = (
        ("literal", "a.b a+b a.b", "a.b", "$1", (), 2, "$1 a+b $1"),
        ("adjacent", "aaaa", "a", "xy", (), 4, "xyxyxyxy"),
        ("overlap", "aaaaa", "aa", "X", (), 2, "XXa"),
        ("delete", "one two one", "one", "", (), 2, " two "),
        ("capture", "a=12 b=34", r"([a-z])=(\d+)", "$2:$1", ("regex",), 2, "12:a 34:b"),
        ("whole-word", "cat catalog scat cat", "cat", "dog", ("whole-word",), 2, "dog catalog scat dog"),
        ("case", "Cat cat CAT", "cat", "dog", ("match-case",), 1, "Cat dog CAT"),
        ("absent", "keep this", "missing", "lost", (), 0, "keep this"),
        ("empty", "", "missing", "lost", (), 0, ""),
    )
    app.send("set-setting", "editor.save.ensure_final_newline", "false")
    app.send("set-setting", "editor.save.trim_trailing_whitespace", "false")
    for name, original, query, replacement, enabled, count, expected in cases:
        path = f"replace-matrix/{name}.txt"
        open_file(app, path, original)
        app.key("ctrl+Home")
        app.key("ctrl+h")
        if not app.wait_for("field:find/search", 3.0):
            return
        for option in ("regex", "whole-word", "match-case"):
            button = f"button:find/{option}"
            if bool(app.element(button).get("active")) != (option in enabled):
                app.click_named(button)
        app.click_named("field:find/search")
        app.key("ctrl+a")
        app.type(query)
        app.wait_until(lambda: app.ui()["overlay"].get("count") == count, 3.0,
                       f"{name}: the query has {count} matches")
        app.click_named("field:find/replace")
        app.key("ctrl+a")
        # Backspace clears a previous replacement even when the new one is empty.
        app.key("BackSpace")
        if replacement:
            app.type(replacement)
        app.click_named("button:find/replace-all")
        step(app, f"replace-all {name}")
        expect_text(app, expected, f"{name}: Replace All produces the exact expected text")
        app.expect(read(app, path) == original, f"{name}: Replace All edits the buffer, not disk")
        app.key("Escape")
        step(app, f"close replace {name}")
        app.expect(app.ui()["focus"] == "editor", f"{name}: Escape restores editor focus")
        if count:
            app.expect(app.buffer().get("dirty"), f"{name}: replacement is unsaved")
            app.key("ctrl+z")
            step(app, f"undo replace-all {name}")
            expect_text(app, original, f"{name}: one undo restores every replacement")
            app.expect(not app.buffer().get("dirty"), f"{name}: undo reaches the original save point")
            app.key("ctrl+shift+z")
            step(app, f"redo replace-all {name}")
            expect_text(app, expected, f"{name}: one redo reapplies every replacement")
            app.key("ctrl+s")
            app.wait_until(lambda: read(app, path) == expected and not app.buffer().get("dirty"),
                           3.0, f"{name}: save writes exactly the replacement result")
        else:
            app.expect(not app.buffer().get("dirty"), f"{name}: replacing no matches leaves a clean buffer")


FLOWS: dict[str, Callable[[q.App], None]] = {"edit-replace-matrix": flow_replace_matrix}
