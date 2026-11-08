"""What the editor's view switches show, columns of carets, and the backward
navigation keys -- the commands the coverage ledger found only the smoke pass
had ever run.

Most view switches change paint and nothing a query reports, so their receipt
is the editor's own pixels: toggled, the pane must look different, and toggled
back it must look exactly as before (the caret blink is off, so nothing else
moves). That is a weak oracle on purpose -- it cannot say the paint is RIGHT --
but a switch that changes nothing on screen, or does not undo, is caught.
"""

from __future__ import annotations

from typing import Callable

from PIL import Image, ImageChops

import microide_qa as q
from flows_common import open_file, step


def _pane_image(app: q.App) -> Image.Image:
    """The focused editor pane's text area, as the screen shows it."""
    pane = app.pane()
    x, y, w, h = pane["rect"]
    sx, sy = getattr(app, "window_scale", (1.0, 1.0))
    ox, oy = app.origin
    box = (round(x * sx) + ox, round(y * sy) + oy, round((x + w) * sx) + ox, round((y + h) * sy) + oy)
    return app.grab().crop(box)


def _differs(a: Image.Image, b: Image.Image) -> int:
    """How many pixels differ between two crops of one size."""
    diff = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).convert("L")
    return sum(1 for value in diff.getdata() if value > 8)


def _paint_toggle(app: q.App, command: str, label: str) -> None:
    if app.wayland:
        # No screen to read on a headless weston (App.grab): the redraw verifier
        # still checks the paint; the toggle is run both ways all the same.
        app.send(command)
        step(app, f"{label} toggled", 0.3)
        app.send(command)
        step(app, f"{label} toggled back", 0.3)
        return
    before = _pane_image(app)
    app.send(command)
    step(app, f"{label} toggled", 0.5)
    toggled = _pane_image(app)
    changed = _differs(before, toggled)
    app.expect(changed > 0, f"{label}: toggling it changes what the editor shows ({changed} pixels)")
    app.send(command)
    step(app, f"{label} toggled back", 0.5)
    back = _pane_image(app)
    restored = _differs(before, back)
    app.expect(restored == 0, f"{label}: toggling it back restores the editor exactly ({restored} pixels differ)")


def flow_view_toggles(app: q.App) -> None:
    """Render Whitespace, Indent Guides, Bracket Match Highlight and Occurrences
    Highlight each change the editor's paint and undo it; Sticky Scroll pins the
    function header while scrolled inside it, and off pins nothing; Code Folding
    off leaves nothing foldable, and Toggle Fold at Cursor folds and unfolds the
    block the caret is in."""
    app.send("set-setting", "editor.caret_blink.enabled", "false")
    body = "".join(f"    total = total + {i};  // step {i}\n" for i in range(60))
    open_file(app, "views/sample.c",
              "int sum(int count) {\n    int total = 0;\n\tif (count) {\n\t\ttotal = count;\n\t}\n"
              + body + "    return total;\n}\n")
    # A word that occurs many times, with the caret on it (occurrences), next to a
    # bracket (bracket match), over indented lines (guides) and spaces (whitespace).
    app.click_text(1, 20)
    step(app, "caret after the opening brace", 0.6)
    _paint_toggle(app, "toggle-editor-bracket-match-highlight", "Bracket Match Highlight")
    app.click_text(2, 10)
    step(app, "caret on `total`", 0.8)
    _paint_toggle(app, "toggle-editor-occurrences-highlight", "Occurrences Highlight")
    _paint_toggle(app, "toggle-editor-render-whitespace", "Render Whitespace")
    _paint_toggle(app, "toggle-editor-indent-guides", "Indent Guides")

    # Sticky scroll: scrolled inside sum(), its header stays pinned.
    app.send("goto", "40")
    step(app, "inside the function", 0.4)
    app.wheel(*app.text_point(40, 5), 3)
    step(app, "scroll", 0.4)
    sticky = app.pane().get("stickyRows", 0)
    app.expect(sticky > 0, f"scrolled inside sum(), sticky scroll pins its header ({sticky} rows)")
    app.send("toggle-editor-sticky-scroll")
    step(app, "sticky scroll off", 0.4)
    app.expect(app.pane().get("stickyRows", 0) == 0, f"off, nothing is pinned ({app.pane().get('stickyRows')})")
    app.send("toggle-editor-sticky-scroll")
    step(app, "sticky scroll on", 0.4)
    app.expect(app.pane().get("stickyRows", 0) > 0, "back on, the header is pinned again")

    # Folding: Toggle Fold at Cursor, then the whole feature off and on.
    app.send("goto", "4")
    step(app, "inside the if block", 0.2)
    app.send("toggle-fold")
    step(app, "toggle fold", 0.3)
    folded = app.buffer().get("folded", [])
    app.expect(any(start == 3 for start, _ in folded), f"Toggle Fold at Cursor folds the `if` block ({folded})")
    app.send("toggle-fold")
    step(app, "toggle fold again", 0.3)
    app.expect(not any(start == 3 for start, _ in app.buffer().get("folded", [])),
               f"and again unfolds it ({app.buffer().get('folded')})")
    app.expect(bool(app.buffer().get("foldable")), "with folding on the function is foldable")
    app.send("toggle-editor-folding")
    step(app, "folding off", 0.3)
    app.expect(not app.buffer().get("foldable"), f"folding off: nothing is foldable ({app.buffer().get('foldable')})")
    app.send("toggle-editor-folding")
    step(app, "folding on", 0.3)
    app.expect(bool(app.buffer().get("foldable")), "folding back on: the function is foldable again")


def flow_column_select(app: q.App) -> None:
    """Column selection by keys (VS Code's Ctrl+Shift+Alt+arrows): down twice
    gives three carets in one column, right twice selects two characters on each
    line, typing replaces all three, up drops the last line; Ctrl+Alt+Up adds a
    caret above; Escape leaves one."""
    open_file(app, "cols/grid.txt", "abcdef\nghijkl\nmnopqr\nstuvwx\n")
    app.click_text(2, 3)
    step(app, "caret at 2:3", 0.2)
    app.key("ctrl+shift+alt+Down")
    app.key("ctrl+shift+alt+Down")
    step(app, "column down twice", 0.2)
    lines = sorted(line for line, _ in app.cursors())
    app.expect(lines == [2, 3, 4], f"three carets in one column on lines 2-4 ({app.cursors()})")
    app.key("ctrl+shift+alt+Right")
    app.key("ctrl+shift+alt+Right")
    step(app, "column right twice", 0.2)
    spans = sorted((c["line"], c["anchorColumn"], c["column"]) for c in app.buffer().get("cursors", []))
    app.expect(spans == [(2, 3, 5), (3, 3, 5), (4, 3, 5)],
               f"columns 3-4 selected on each of lines 2-4 ({spans})")
    app.key("ctrl+shift+alt+Left")
    step(app, "column left", 0.2)
    spans = sorted((c["line"], c["anchorColumn"], c["column"]) for c in app.buffer().get("cursors", []))
    app.expect(spans == [(2, 3, 4), (3, 3, 4), (4, 3, 4)], f"column left narrows each line's selection ({spans})")
    app.key("ctrl+shift+alt+Right")
    app.key("ctrl+shift+alt+Up")
    step(app, "column up", 0.2)
    app.expect(sorted(line for line, _ in app.cursors()) == [2, 3],
               f"column up drops the bottom line ({app.cursors()})")
    app.type("X")
    step(app, "type over the column", 0.2)
    app.expect(app.text() == "abcdef\nghXkl\nmnXqr\nstuvwx\n",
               f"typing replaces each line's selection ({app.text()!r})")
    app.key("Escape")
    step(app, "escape", 0.1)
    app.click_text(3, 2)
    app.key("ctrl+alt+Up")
    step(app, "add cursor above", 0.2)
    app.expect(sorted(app.cursors()) == [(2, 2), (3, 2)], f"Ctrl+Alt+Up adds a caret above ({app.cursors()})")
    app.type("_")
    step(app, "type at both", 0.2)
    app.expect(app.text().split("\n")[1:3] == ["g_hXkl", "m_nXqr"], f"both carets type ({app.text()!r})")
    app.key("Escape")
    step(app, "escape", 0.1)
    app.expect(len(app.cursors()) == 1, f"Escape leaves one caret ({app.cursors()})")


def flow_previous_navigation(app: q.App) -> None:
    """The backward half of each navigation pair: Shift+F3 steps to the previous
    match of the last search and wraps from the first to the last (VS Code);
    Shift+F8 / Shift+Alt+F8 walk the problems backwards, across files for the
    first and within the file for the second."""
    open_file(app, "nav/words.txt", "alpha\nbeta alpha\ngamma\nalpha delta\n")
    app.key("ctrl+f")
    step(app, "find", 0.2)
    app.type("alpha")
    step(app, "query", 0.3)
    app.key("Escape")
    app.send("goto", "4:1")
    step(app, "caret on line 4", 0.2)
    app.key("shift+F3")
    step(app, "shift+F3", 0.2)
    app.expect(app.cursors()[0][0] == 2, f"Shift+F3 goes to the previous match, line 2 ({app.cursors()})")
    app.key("shift+F3")
    app.key("shift+F3")
    step(app, "shift+F3 twice", 0.2)
    app.expect(app.cursors()[0][0] == 4, f"past the first match it wraps to the last, line 4 ({app.cursors()})")

    open_file(app, "other.qaapi", "other one\nother two\n")
    open_file(app, "notes.qaapi", "line one\nline two\nline three\nline four\n")
    reply = app.send("qa.api.problems")
    app.expect(bool(reply.get("ok")), f"the plugin publishes its problems ({reply})")
    app.wait_until(lambda: app.element("status:problems").get("label", "") not in ("", "0"), 5.0,
                   "the status bar to count the problems")
    app.send("goto", "4:1")
    app.key("shift+alt+F8")
    step(app, "previous diagnostic in the file", 0.3)
    app.expect(app.cursors()[0][0] == 3 and (app.active_tab() or {}).get("title") == "notes.qaapi",
               f"Shift+Alt+F8 goes to this file's previous problem, line 3 ({app.cursors()})")
    app.key("shift+alt+F8")
    step(app, "and again", 0.3)
    app.expect(app.cursors()[0][0] == 2, f"then line 2 ({app.cursors()})")
    app.key("shift+F8")
    step(app, "previous problem across files", 0.4)
    app.expect((app.active_tab() or {}).get("title") == "other.qaapi",
               f"Shift+F8 before the first problem of this file goes to the other file's "
               f"({(app.active_tab() or {}).get('title')})")


def _row_lines(app: q.App, group: int) -> list[int]:
    """Which line each visible row of a pane shows: its soft-wrapped row map."""
    return [row["line"] for row in app.pane(group)["rows"]]


def flow_split_wrap_edits(app: q.App) -> None:
    """One file soft-wrapped in two split panes of different widths, edited from
    each pane in turn. Each view keeps its own line-width and wrapped-row tables,
    spliced by the edits made through it, so an edit through one pane leaves the
    other's describing the old text. An edit through that other pane once spliced
    its change into the stale tables and stamped them current -- a line kept its
    old width (QA monkey seeds 69 and 601; a debug build aborted on the
    cross-check, which makes a recurrence a crash here). For any build, each
    pane's rows must be what a fresh wrap of the same text gives: wrap off and on
    again rebuilds the tables from the buffer."""
    long = "Jobs run highest-priority first; ties keep insertion order. The trace prints a"
    open_file(app, "wrap/split.md",
              "# taskflow\n\nshort line\n" + long + "\nrunning cost total.\n\n## End\n")
    if not app.buffer().get("softWrap"):
        app.key("alt+z")
    step(app, "soft wrap in the first pane", 0.3)
    app.key("ctrl+backslash")
    step(app, "split right", 0.4)
    if not app.buffer().get("softWrap"):
        app.key("alt+z")
    step(app, "soft wrap in the second pane", 0.3)
    app.expect(len(app.ui().get("editors", [])) == 2, "two panes show the file")

    def fresh_rows(group: int, label: str) -> None:
        app.send("focus-group", str(group + 1))
        step(app, f"{label}: pane {group + 1} focused", 0.2)
        before = _row_lines(app, group)
        app.key("alt+z")
        step(app, f"{label}: wrap off", 0.2)
        app.key("alt+z")
        step(app, f"{label}: wrap on again", 0.3)
        after = _row_lines(app, group)
        app.expect(before == after,
                   f"{label}: pane {group + 1} wraps the text as a fresh wrap does ({before} != {after})")

    # Through the second pane: line 3 grows past every other line.
    app.send("goto", "3:6")
    app.type("x" * 70)
    step(app, "the second pane widens line 3", 0.3)
    # Through the first pane: another line, the line count unchanged.
    app.send("focus-group", "1")
    step(app, "the first pane focused", 0.2)
    app.send("goto", "5:1")
    app.type("}")
    step(app, "the first pane types on line 5", 0.3)
    # And back through the second: a line removed and one added.
    app.send("focus-group", "2")
    app.send("goto", "4:10")
    app.key("Return")
    app.key("ctrl+z")
    app.key("ctrl+shift+z")
    step(app, "the second pane splits line 4, undoes and redoes it", 0.3)
    app.send("focus-group", "1")
    app.send("goto", "3:2")
    app.type("yy")
    step(app, "the first pane types on the widened line", 0.3)
    fresh_rows(0, "after the edits")
    fresh_rows(1, "after the edits")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "view-toggles": flow_view_toggles,
    "column-select": flow_column_select,
    "previous-navigation": flow_previous_navigation,
    "split-wrap-edits": flow_split_wrap_edits,
}
PLUGINS: dict[str, tuple[str, ...]] = {"previous-navigation": ("qa-api",)}
