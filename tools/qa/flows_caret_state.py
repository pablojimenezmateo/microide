"""A caret stays on its character when only the VIEW of the text changes: the tab
size, soft wrap, the editor font's size, a split of the pane. Each is a change
of what a column is worth, and a caret that remembered a visual column for
Up/Down was moved by one: `\\tx|` became `\\t|x` going from tabs of 4 to tabs of
8. The status bar's `Ln, Col` (the visible column) is checked against the caret
after every step by the state oracle; here the carets themselves are."""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import open_file, step


def _carets(app: q.App) -> list[tuple[int, int, int, int]]:
    return [(c["anchorLine"], c["anchorColumn"], c["line"], c["column"]) for c in app.buffer()["cursors"]]


def flow_caret_survives_view_changes(app: q.App) -> None:
    """Carets after tabs (one with a selection, two at once) keep their text
    positions through tab sizes of 8 and 2 (by command and by setting), a soft-wrap
    toggle, a zoom of the editor font and a split; the status bar's Col follows the
    tab size; Up and Down from a caret after a tab land where its column now is."""
    open_file(app, "caret/tabs.txt", "\tx\n\t\tyy\nzzzzzzzzzzzzzzzz\n")
    app.send("goto", "1:3")
    app.key("shift+Left")
    app.key("ctrl+alt+Down")
    step(app, "two carets after tabs, the primary with a selection", 0.2)
    before = _carets(app)
    app.expect(len(before) == 2, f"two carets ({before})")

    def unchanged(label: str) -> None:
        step(app, label, 0.3)
        app.expect(_carets(app) == before, f"{label}: the carets stay on their characters ({before} -> {_carets(app)})")

    app.send("tab-size", "8")
    unchanged("tab-size 8")
    app.send("tab-size", "2")
    unchanged("tab-size 2")
    app.send("set-setting", "editor.tab_size", "6")
    unchanged("editor.tab_size 6")
    app.key("alt+z")
    unchanged("soft wrap on")
    app.key("alt+z")
    unchanged("soft wrap off")
    app.send("set-setting", "editor.font_size", "20")
    unchanged("a bigger editor font")
    app.send("set-setting", "editor.font_size", "14")
    unchanged("the font back")
    app.key("ctrl+backslash")
    step(app, "split right", 0.4)
    app.expect(_carets(app)[0][2:] == before[0][2:],
               f"the new pane opens at the caret ({before[0]} -> {_carets(app)})")
    app.send("focus-group", "1")
    step(app, "back to the first pane", 0.3)
    app.expect(_carets(app) == before, f"the first pane's carets are untouched ({_carets(app)})")
    # One caret now, after the x on line 1 with tabs of 6: Down goes to visible
    # column 8 on `\t\tyy` (tabs at 0-6 and 6-12), which is inside the second tab;
    # Up comes back to the x it left.
    app.key("Escape")
    app.send("goto", "1:3")
    app.send("tab-size", "4")
    step(app, "tabs of 4, one caret after x", 0.2)
    app.key("Down")
    app.key("Up")
    step(app, "down and up", 0.2)
    app.expect(app.cursors() == [(1, 3)], f"Down then Up returns to the x ({app.cursors()})")
    status = app.element("status:line-column").get("label", "")
    app.expect(status == "Ln 1, Col 6", f"the status bar says Col 6 after \\tx with tabs of 4 ({status!r})")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "caret-survives-view-changes": flow_caret_survives_view_changes,
}
