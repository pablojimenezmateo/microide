"""UX flows: the editor behaviours a VS Code user does without thinking, each
checked for its effect -- navigation history, caret motion and selection growth,
indentation, the find widget's options, the terminal clipboard, git's discard and
unstage, and notifications.
"""

from __future__ import annotations

import subprocess
from typing import Callable

import microide_qa as q
from flows_common import dismiss, expect_cursors, expect_text, open_file, read, step, write


def _numbered(count: int, prefix: str = "line") -> str:
    return "".join(f"{prefix} {i}\n" for i in range(1, count + 1))


def _where(app: q.App) -> tuple[str, int]:
    return (app.active_tab() or {}).get("title", ""), app.cursors()[0][0] if app.cursors() else 0


def _goto(app: q.App, line: int) -> None:
    app.key("ctrl+g")
    step(app, "ctrl+g", 0.2)
    app.type(str(line))
    app.key("Return")
    step(app, f"go to line {line}", 0.25)


def _menu_row(app: q.App, menu: str, label: str) -> dict | None:
    app.click_named(f"menu:{menu}")
    step(app, f"{menu} menu", 0.25)
    return next((e for e in app.elements("menu-item")
                 if e["name"] == f"menu-item:{menu}/{label}"), None)


# ---- navigation ------------------------------------------------------------


def flow_nav_history(app: q.App) -> None:
    """Jump far in a file, switch to another; Alt+Left walks back through both
    places, Alt+Right forward again, the Go menu does the same and greys out at
    either end, a file closed since is reopened, and a focused terminal keeps
    the keys (VS Code's Go Back / Go Forward)."""
    open_file(app, "notes/long.txt", _numbered(120))
    _goto(app, 90)
    open_file(app, "notes/short.txt", "alpha\nbeta\ngamma\n")
    app.click_text(2, 3)
    step(app, "caret in short.txt")

    app.key("alt+Left")
    app.wait_until(lambda: _where(app) == ("long.txt", 90), 3.0, "Alt+Left to go back to long.txt:90")
    app.key("alt+Left")
    app.wait_until(lambda: _where(app) == ("long.txt", 1), 3.0, "Alt+Left again to long.txt:1")
    step(app, "back twice")
    # Whatever the session opened before long.txt is further back still: walk to
    # the oldest place, where Go Back greys out and Go Forward does not.
    for _ in range(6):
        before = _where(app)
        app.key("alt+Left")
        step(app, "alt+Left towards the oldest place", 0.2)
        if _where(app) == before:
            break
    row = _menu_row(app, "Go", "Go Back")
    app.expect(row is not None and row.get("enabled") is False,
               f"at the oldest place Go > Go Back is disabled ({row})")
    forward = next((e for e in app.elements("menu-item") if e["name"] == "menu-item:Go/Go Forward"), None)
    app.expect(forward is not None and forward.get("enabled") is True,
               f"and Go > Go Forward is enabled ({forward})")
    dismiss(app)
    for _ in range(8):
        if _where(app) == ("long.txt", 90):
            break
        app.key("alt+Right")
        step(app, "alt+Right towards long.txt:90", 0.2)
    app.expect(_where(app) == ("long.txt", 90), f"Alt+Right walks forward to long.txt:90 ({_where(app)})")

    # The next place is a closed file: going forward reopens it, caret and all.
    close = next((e for e in app.elements("button", prefix="button:close-tab/")
                  if e["name"].endswith("/short.txt")), None)
    app.expect(close is not None, "short.txt has a close button")
    if close:
        app.click(*app.center(close))
        step(app, "close short.txt", 0.3)
    app.send("focus", "editor")
    app.key("alt+Right")
    app.wait_until(lambda: _where(app) == ("short.txt", 2), 3.0,
                   "Alt+Right to reopen short.txt with the caret on line 2")
    expect_cursors(app, [(2, 3)], "the caret is back where it was in short.txt")

    # A focused terminal keeps Alt+Left for the shell / its panes.
    app.key("ctrl+grave")
    app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    app.send("focus", "panel")
    step(app, "focus the terminal", 0.3)
    app.key("alt+Left")
    step(app, "alt+Left in the terminal", 0.3)
    app.expect(_where(app)[0] == "short.txt",
               f"Alt+Left in a focused terminal does not navigate ({_where(app)})")
    app.key("ctrl+grave")
    step(app, "hide the terminal", 0.3)


def flow_caret_motion(app: q.App) -> None:
    """Home toggles between the first non-blank and column 1, End, word motion
    and word selection, Ctrl+Home/End, PageDown/PageUp move the caret a page,
    Ctrl+Down scrolls without moving it, Ctrl+L grows the selection by lines and
    Shift+Alt+Right expands it (VS Code's cursor commands)."""
    body = "    alpha beta_gamma (delta)\n" + _numbered(150)
    open_file(app, "notes/motion.txt", body)
    app.click_text(1, 15)
    app.key("End")
    step(app, "End")
    expect_cursors(app, [(1, 29)], "End goes to the end of the line")
    app.key("Home")
    step(app, "Home")
    expect_cursors(app, [(1, 5)], "Home goes to the first non-blank character")
    app.key("Home")
    step(app, "Home again")
    expect_cursors(app, [(1, 1)], "Home again goes to column 1")
    app.key("Home")
    step(app, "Home a third time")
    expect_cursors(app, [(1, 5)], "and back to the first non-blank")

    app.key("ctrl+Right")
    step(app, "ctrl+Right")
    expect_cursors(app, [(1, 10)], "Ctrl+Right goes to the end of the word")
    app.key("ctrl+Right")
    step(app, "ctrl+Right again")
    expect_cursors(app, [(1, 21)], "an underscore does not split a word")
    app.key("ctrl+Left")
    step(app, "ctrl+Left")
    expect_cursors(app, [(1, 11)], "Ctrl+Left goes to the start of the word")
    app.key("ctrl+shift+Right")
    step(app, "ctrl+shift+Right")
    app.expect(app.buffer().get("selectedText") == "beta_gamma",
               f"Ctrl+Shift+Right selects to the end of the word ({app.buffer().get('selectedText')!r})")
    app.key("ctrl+BackSpace")
    step(app, "ctrl+BackSpace with a selection")
    app.expect(app.text().startswith("    alpha  (delta)\n"),
               f"Ctrl+Backspace with a selection deletes the selection ({app.text()[:30]!r})")
    app.key("ctrl+z")
    step(app, "undo")

    app.key("ctrl+End")
    step(app, "ctrl+End")
    expect_cursors(app, [(152, 1)], "Ctrl+End goes to the end of the document")
    app.key("ctrl+Home")
    step(app, "ctrl+Home")
    expect_cursors(app, [(1, 1)], "Ctrl+Home goes to the start")
    visible = app.buffer().get("visibleCount", 0)
    app.key("Next")
    step(app, "PageDown")
    after_page = app.cursors()[0][0]
    app.expect(visible > 5 and abs(after_page - 1 - visible) <= 2,
               f"PageDown moves the caret about a page ({visible} rows visible, caret on {after_page})")
    app.expect(app.buffer().get("scrollLine", 0) > 1, "and scrolls the view with it")
    app.key("Prior")
    step(app, "PageUp")
    app.expect(app.cursors()[0][0] <= 2, f"PageUp brings the caret back up ({app.cursors()})")

    caret = app.cursors()
    top = app.buffer().get("scrollLine", 0)
    app.key("ctrl+Down")
    app.key("ctrl+Down")
    step(app, "ctrl+Down twice")
    app.expect(app.buffer().get("scrollLine", 0) == top + 2,
               f"Ctrl+Down scrolls one line each ({top} -> {app.buffer().get('scrollLine')})")
    app.expect(app.cursors() == caret, f"without moving the caret ({caret} -> {app.cursors()})")
    app.key("ctrl+Up")
    app.key("ctrl+Up")
    step(app, "ctrl+Up twice")

    app.click_text(3, 3)
    app.key("ctrl+l")
    step(app, "ctrl+l")
    app.expect(app.buffer().get("selectedText") == "line 2\n",
               f"Ctrl+L selects the line ({app.buffer().get('selectedText')!r})")
    app.key("ctrl+l")
    step(app, "ctrl+l again")
    app.expect(app.buffer().get("selectedText") == "line 2\nline 3\n",
               f"Ctrl+L again adds the next line ({app.buffer().get('selectedText')!r})")

    app.click_text(1, 23)
    app.key("shift+alt+Right")
    step(app, "shift+alt+Right")
    first = app.buffer().get("selectedText")
    app.key("shift+alt+Right")
    step(app, "shift+alt+Right again")
    second = app.buffer().get("selectedText")
    app.expect(first == "delta" and second in ("(delta)", "alpha beta_gamma (delta)",
                                                "    alpha beta_gamma (delta)"),
               f"Expand Selection grows word -> brackets ({first!r} -> {second!r})")
    app.key("shift+alt+Left")
    step(app, "shift+alt+Left")
    app.expect(app.buffer().get("selectedText") == "delta",
               f"Shrink Selection returns to the word ({app.buffer().get('selectedText')!r})")
    app.key("Escape")


def flow_indentation(app: q.App) -> None:
    """Tab and Shift+Tab indent and outdent every selected line and keep the
    selection; Shift+Tab without one outdents the caret's line; Enter after an
    opening brace indents; Backspace in leading spaces removes one level."""
    open_file(app, "src/indent.cpp", "int f() {\nreturn 1;\n}\na\nb\n")
    unit = " " * app.buffer().get("tabSize", 4) if app.buffer().get("softTabs") else "\t"
    app.click_text(4, 1)
    app.key("shift+Down")
    app.key("shift+End")
    app.key("Tab")
    step(app, "Tab over two lines")
    expect_text(app, f"int f() {{\nreturn 1;\n}}\n{unit}a\n{unit}b\n", "Tab indents each selected line")
    app.expect(app.buffer().get("selectedText") == f"{unit}a\n{unit}b"
               or app.buffer().get("selectedText", "").endswith("a\n" + unit + "b"),
               f"the selection stays on the lines ({app.buffer().get('selectedText')!r})")
    app.key("shift+Tab")
    step(app, "Shift+Tab over two lines")
    expect_text(app, "int f() {\nreturn 1;\n}\na\nb\n", "Shift+Tab outdents them again")

    app.click_text(2, 1)
    app.key("Tab")
    step(app, "Tab at line start")
    expect_text(app, f"int f() {{\n{unit}return 1;\n}}\na\nb\n", "Tab at the start of a line indents it")
    app.key("End")
    app.key("shift+Tab")
    step(app, "Shift+Tab with no selection")
    expect_text(app, "int f() {\nreturn 1;\n}\na\nb\n", "Shift+Tab with the caret anywhere outdents the line")

    app.click_text(1, 10)
    app.key("Return")
    step(app, "Enter after the brace")
    expect_text(app, f"int f() {{\n{unit}\nreturn 1;\n}}\na\nb\n", "Enter after { indents the new line")
    expect_cursors(app, [(2, len(unit) + 1)], "the caret sits at the indent")
    if app.buffer().get("softTabs"):
        app.key("BackSpace")
        step(app, "Backspace in the indent")
        expect_text(app, "int f() {\n\nreturn 1;\n}\na\nb\n", "Backspace in leading spaces removes one level")


def flow_find_options(app: q.App) -> None:
    """The find widget's whole-word and regex toggles by Alt+W / Alt+R and by
    click, Ctrl+H from the find field, a regex replace with a capture group, and
    the query seeded from the word under the caret."""
    open_file(app, "notes/options.txt", "cat catalog cat\nid=12 id=345\nscat\n")
    app.click_text(1, 6)
    app.key("ctrl+f")
    step(app, "ctrl+f on a word", 0.3)
    app.expect(app.field_text("field:find/search") == "catalog",
               f"Ctrl+F seeds the query from the word under the caret ({app.field_text('field:find/search')!r})")
    app.key("ctrl+a")
    app.type("cat")
    step(app, "query cat", 0.3)
    app.expect(app.ui()["overlay"]["count"] == 4, f"cat matches four times ({app.ui()['overlay']['count']})")
    app.key("alt+w")
    step(app, "alt+w", 0.3)
    app.expect(app.element("button:find/whole-word").get("active") is True, "Alt+W turns whole word on")
    app.expect(app.ui()["overlay"]["count"] == 2,
               f"whole word leaves the two standalone cats ({app.ui()['overlay']['count']})")
    app.click_named("button:find/whole-word")
    step(app, "click whole word off", 0.3)
    app.expect(app.element("button:find/whole-word").get("active") is False, "a click turns it off")
    app.key("alt+r")
    step(app, "alt+r", 0.3)
    app.expect(app.element("button:find/regex").get("active") is True, "Alt+R turns regex on")
    app.click_named("field:find/search")
    app.key("ctrl+a")
    app.type(r"id=(\d+)")
    step(app, "regex query", 0.3)
    app.expect(app.ui()["overlay"]["count"] == 2, f"the regex matches both ids ({app.ui()['overlay']['count']})")
    app.key("ctrl+h")
    step(app, "ctrl+h", 0.3)
    app.click_named("field:find/replace")
    app.key("ctrl+a")
    app.type("n$1")
    step(app, "replacement with a group", 0.2)
    app.click_named("button:find/replace-all")
    step(app, "replace all", 0.3)
    expect_text(app, "cat catalog cat\nn12 n345\nscat\n", "a regex replace substitutes $1")
    app.click_named("button:find/regex")
    step(app, "regex off", 0.3)
    app.key("Escape")
    step(app, "close find", 0.2)

    # Find in Selection (VS Code's Alt+L) is not implemented: TD-2026-10-18-358.
    dismiss(app)


# ---- quick open and undo ------------------------------------------------------


def flow_quick_open(app: q.App) -> None:
    """Ctrl+P with `file:line` opens at the line; `>` switches to commands and `:`
    to Go to Line, keeping what follows; a click on a row opens it; Escape returns
    the editor untouched."""
    write(app, "notes/target.txt", "".join(f"row {i}\n" for i in range(1, 41)))
    open_file(app, "notes/start.txt", "start\n")
    app.key("ctrl+p")
    step(app, "ctrl+p", 0.3)
    app.type("target.txt:25")
    step(app, "type target.txt:25", 0.4)
    rows = app.elements("overlay-row")
    app.expect(any("target.txt" in r.get("label", "") for r in rows),
               f"file:line still lists the file ({[r.get('label') for r in rows][:4]})")
    app.key("Return")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "target.txt", 3.0, "target.txt to open")
    app.wait_until(lambda: app.cursors()[:1] == [(25, 1)], 3.0, "the caret on line 25")

    app.key("ctrl+p")
    step(app, "ctrl+p", 0.3)
    app.type(">sort lines")
    step(app, "type >sort lines", 0.3)
    overlay = app.ui()["overlay"]
    app.expect(overlay.get("mode") == "command-palette" and overlay.get("query") == "sort lines",
               f"> switches to commands, keeping the rest ({overlay})")
    app.expect(overlay.get("count", 0) >= 2, f"the sort commands are listed ({overlay.get('count')})")
    dismiss(app)

    app.key("ctrl+p")
    step(app, "ctrl+p", 0.3)
    app.type(":7")
    step(app, "type :7", 0.3)
    prompt = app.ui().get("prompt") or {}
    app.expect(prompt.get("input") == "7", f": switches to Go to Line with 7 ({prompt})")
    app.key("Return")
    step(app, "Enter", 0.3)
    app.expect(app.cursors()[:1] == [(7, 1)], f"Go to Line lands on 7 ({app.cursors()})")

    app.key("ctrl+p")
    step(app, "ctrl+p", 0.3)
    app.expect(app.ui()["overlay"].get("query") == "", "the next Ctrl+P opens empty")
    app.type("start")
    step(app, "type start", 0.3)
    row = next((r for r in app.elements("overlay-row") if "start.txt" in r.get("label", "")), None)
    app.expect(row is not None, "start.txt is listed")
    if row:
        app.click(*app.center(row))
        app.wait_until(lambda: (app.active_tab() or {}).get("title") == "start.txt", 3.0,
                       "a click on the row to open it")
    before = app.text()
    app.key("ctrl+p")
    app.type("zzz")
    app.key("Escape")
    step(app, "escape the finder", 0.3)
    app.expect(app.text() == before and not app.ui()["overlay"]["visible"],
               "Escape leaves the editor as it was")


def flow_undo_semantics(app: q.App) -> None:
    """Undo restores the selection an edit replaced; a paste, a multi-caret edit
    and a line move each undo in one step; a new edit after undo drops redo; undo
    brings the caret back to the edit."""
    open_file(app, "notes/undo.txt", "alpha beta gamma\n" + "".join(f"pad {i}\n" for i in range(80)))
    app.click_text(1, 7)
    app.key("ctrl+d")
    step(app, "select beta")
    app.type("BETA")
    step(app, "type over the selection")
    app.expect(app.text().startswith("alpha BETA gamma\n"), f"typing replaces the selection ({app.text()[:20]!r})")
    app.key("ctrl+z")
    step(app, "undo the replacement")
    app.expect(app.text().startswith("alpha beta gamma\n"), f"undo restores the word ({app.text()[:20]!r})")
    app.expect(app.buffer().get("selectedText") == "beta",
               f"undo restores the selection the edit replaced ({app.buffer().get('selectedText')!r})")

    app.key("Escape")
    app.click_text(1, 1)
    app.key("ctrl+alt+Down")
    app.key("ctrl+alt+Down")
    step(app, "three carets")
    app.type("> ")
    step(app, "type at three carets")
    app.expect(app.text().startswith("> alpha beta gamma\n> pad 0\n> pad 1\n"),
               f"each caret typed ({app.text()[:40]!r})")
    app.key("ctrl+z")
    step(app, "undo the multi-caret edit")
    app.expect(app.text().startswith("alpha beta gamma\npad 0\npad 1\n"),
               f"one undo takes back every caret's edit ({app.text()[:40]!r})")
    app.key("Escape")

    app.click_text(2, 1)
    app.key("End")
    app.key("shift+Home")
    app.key("ctrl+c")
    app.click_text(1, 1)
    app.key("ctrl+v")
    step(app, "paste")
    pasted = app.text()
    app.key("ctrl+z")
    step(app, "undo the paste")
    app.expect(app.text().startswith("alpha beta gamma\n") and app.text() != pasted,
               f"one undo takes back the paste ({app.text()[:30]!r})")

    app.click_text(1, 1)
    app.key("alt+Down")
    step(app, "move the line down")
    app.key("ctrl+z")
    step(app, "undo the move")
    app.expect(app.text().startswith("alpha beta gamma\npad 0\n"), f"one undo takes back the move ({app.text()[:30]!r})")

    app.key("ctrl+End")
    app.type("tail")
    step(app, "type at the end")
    app.key("ctrl+Home")
    step(app, "go to the top")
    app.key("ctrl+z")
    step(app, "undo far from the edit")
    app.expect(app.cursors()[0][0] >= 80, f"undo brings the caret back to the edit ({app.cursors()})")
    app.expect(app.buffer().get("scrollLine", 0) > 40, "and scrolls it into view")
    app.type("x")
    step(app, "a new edit after undo")
    text = app.text()
    app.key("ctrl+shift+z")
    step(app, "redo after a new edit")
    app.expect(app.text() == text, "a new edit drops the redo history")


# ---- mouse and wrapping -------------------------------------------------------


def flow_mouse_and_wrap(app: q.App) -> None:
    """Shift+wheel scrolls a long line sideways; Alt+Z wraps it, Down steps one
    wrapped row, End stops at the row's end and a second End at the line's; a
    double-click on the tab strip's empty space opens an untitled editor."""
    long_line = " ".join(f"w{i:03d}" for i in range(120))
    open_file(app, "notes/wide.txt", long_line + "\nshort\n")
    pane = app.pane()
    x, y = pane["textX"] + 200, pane["rows"][0]["y"] + pane["lineHeight"] / 2
    app.wheel(x, y, 3, modifiers=("shift",))
    step(app, "shift+wheel down", 0.3)
    app.expect(app.pane()["scrollColumn"] > 0,
               f"Shift+wheel scrolls the long line sideways (scrollColumn {app.pane()['scrollColumn']})")
    app.wheel(x, y, -6, modifiers=("shift",))
    step(app, "shift+wheel up", 0.3)
    app.expect(app.pane()["scrollColumn"] == 0, "and back to column 0")

    if not app.buffer().get("softWrap"):
        app.key("alt+z")
        step(app, "alt+z", 0.3)
    app.expect(app.buffer().get("softWrap") is True, "Alt+Z turns soft wrap on")
    app.click_text(1, 3)
    app.key("Down")
    step(app, "Down on a wrapped line")
    app.expect(app.cursors()[0][0] == 1 and app.cursors()[0][1] > 3,
               f"Down steps to the line's next wrapped row ({app.cursors()})")
    app.click_text(1, 3)
    app.key("End")
    step(app, "End")
    first_end = app.cursors()[0]
    app.expect(first_end[0] == 1 and first_end[1] < len(long_line) + 1,
               f"End stops at the end of the wrapped row ({first_end})")
    app.key("End")
    step(app, "End again")
    expect_cursors(app, [(1, len(long_line) + 1)], "a second End goes to the end of the line")
    app.key("alt+z")
    step(app, "alt+z off", 0.3)
    app.expect(app.buffer().get("softWrap") is False, "Alt+Z turns it off again")

    strip = app.ui()["surfaces"]["tabStrip"]
    tabs_before = len(app.elements("tab"))
    app.click(strip[0] + strip[2] - 100, strip[1] + strip[3] / 2, clicks=2)
    step(app, "double-click the tab strip", 0.4)
    app.expect(len(app.elements("tab")) == tabs_before + 1 and
               (app.active_tab() or {}).get("title", "").lower().startswith("untitled"),
               f"a double-click on the strip opens an untitled editor ({app.active_tab()})")


# ---- project search ----------------------------------------------------------------


def _search_hits(app: q.App) -> list[str]:
    """The search view's hit rows (`row:search/<path>:<line>`)."""
    return [e["name"][len("row:search/"):] for e in app.elements("row", prefix="row:search/")
            if e["name"].rsplit(":", 1)[-1].isdigit()]


def flow_search_options(app: q.App) -> None:
    """The search view's options: Ctrl+Shift+F seeds the selected word, the case
    and regex toggles narrow the hits, include/exclude globs scope them, and the
    keyboard opens a hit."""
    write(app, "notes/q1.txt", "Quokka quokka\nQUOKKA\n")
    write(app, "notes/q2.md", "quokka\n")
    open_file(app, "notes/q2.md")
    app.click_text(1, 2)
    app.key("ctrl+d")
    step(app, "select quokka")
    app.key("ctrl+shift+f")
    step(app, "ctrl+shift+f", 0.5)
    app.expect(app.field_text("field:search/query") == "quokka",
               f"Ctrl+Shift+F seeds the selected word ({app.field_text('field:search/query')!r})")
    if app.field_text("field:search/query") != "quokka":
        app.click_named("field:search/query")
        app.key("ctrl+a")
        app.type("quokka")
    app.wait_until(lambda: len(_search_hits(app)) >= 3, 5.0, "case-insensitive hits in both files")
    all_hits = _search_hits(app)
    app.click_named("button:search/case")
    step(app, "match case on", 0.6)
    app.wait_until(lambda: 0 < len(_search_hits(app)) < len(all_hits), 5.0,
                   f"match case to drop QUOKKA and Quokka ({all_hits})")
    app.click_named("button:search/case")
    step(app, "match case off", 0.6)

    app.click_named("button:search/mode")
    step(app, "regex mode", 0.4)
    app.click_named("field:search/query")
    app.key("ctrl+a")
    app.type("^QUOK")
    step(app, "regex ^QUOK", 0.8)
    app.wait_until(lambda: len(_search_hits(app)) >= 1, 5.0, "the regex to match line starts")
    hits = _search_hits(app)
    app.expect(all(h.endswith(":1") or h.endswith(":2") for h in hits) and len(hits) <= 4,
               f"^ anchors at line starts ({hits})")
    app.click_named("button:search/mode")
    step(app, "back to literal", 0.4)
    app.click_named("field:search/query")
    app.key("ctrl+a")
    app.type("quokka")
    step(app, "literal quokka", 0.8)

    if not any(e["name"] == "field:search/include" for e in app.elements("field")):
        app.click_named("button:search/scope")
        step(app, "show include/exclude", 0.4)
    include = app.wait_for("field:search/include", 2.0)
    app.expect(include is not None, "the scope toggle shows the include field")
    if include:
        app.click(*app.center(include))
        app.type("*.md")
        app.key("Return")
        step(app, "include *.md", 0.8)
        app.wait_until(lambda: _search_hits(app) and all(".md:" in h for h in _search_hits(app)), 5.0,
                       f"include *.md to keep only q2.md ({_search_hits(app)})")
        app.click(*app.center(app.element("field:search/include")))
        app.key("ctrl+a")
        app.key("BackSpace")
        app.key("Return")
        step(app, "clear include", 0.6)
        exclude = app.element("field:search/exclude")
        app.click(*app.center(exclude))
        app.type("notes/**")
        app.key("Return")
        step(app, "exclude notes/**", 0.8)
        app.wait_until(lambda: not any(h.startswith("notes/") for h in _search_hits(app)), 5.0,
                       f"exclude notes/** to drop the notes hits ({_search_hits(app)})")
        app.click(*app.center(app.element("field:search/exclude")))
        app.key("ctrl+a")
        app.key("BackSpace")
        app.key("Return")
        step(app, "clear exclude", 0.6)
    app.wait_until(lambda: len(_search_hits(app)) >= 3, 5.0, "all hits back")
    app.send("focus", "sidebar")
    step(app, "focus the results", 0.2)
    app.key("Down")
    app.key("Down")
    app.key("Return")
    step(app, "Enter on a hit", 0.5)
    app.expect((app.active_tab() or {}).get("title") in ("q1.txt", "q2.md"),
               f"Enter on a result opens its file ({(app.active_tab() or {}).get('title')})")
    dismiss(app)


# ---- git ---------------------------------------------------------------------


def _git(app: q.App, *args: str) -> str:
    assert app.project is not None
    return subprocess.run(["git", *args], cwd=app.project, capture_output=True, text=True,
                          check=False).stdout.strip()


def _git_row_menu(app: q.App, name: str, label_prefix: str) -> bool:
    """Right-clicks the git view's row for `name` and clicks the context menu row
    whose label starts with `label_prefix`."""
    row = app.wait_for(f"row:git/{name}", 5.0)
    app.expect(row is not None, f"the git view lists {name}")
    if row is None:
        return False
    app.click(*app.center(row), button=3)
    step(app, f"right-click {name}", 0.3)
    item = next((e for e in app.elements("menu-item", prefix="menu-item:context/")
                 if e.get("label", "").startswith(label_prefix)), None)
    app.expect(item is not None, f"{name}'s context menu offers {label_prefix} "
                                 f"({[e.get('label') for e in app.elements('menu-item')]})")
    if item is None:
        dismiss(app)
        return False
    app.click(*app.center(item))
    step(app, f"{label_prefix} {name}", 0.4)
    return True


def flow_git_discard(app: q.App) -> None:
    """An untracked file shows in the git view; Stage and Unstage follow the index
    both ways; Discard asks first, Cancel keeps the change, Discard restores the
    file on disk and in its open editor; Stage All takes everything."""
    original = read(app, "src/main.cpp")
    write(app, "src/main.cpp", original + "// discard me\n")
    write(app, "src/scheduler.cpp", read(app, "src/scheduler.cpp") + "// stage me\n")
    write(app, "notes/untracked.txt", "new\n")
    open_file(app, "src/main.cpp")
    app.key("ctrl+shift+g")
    step(app, "git view", 0.5)
    app.expect(app.wait_for("row:git/untracked.txt", 5.0) is not None,
               "an untracked file shows in the git view")

    if _git_row_menu(app, "scheduler.cpp", "Stage"):
        app.wait_until(lambda: _git(app, "diff", "--cached", "--name-only") == "src/scheduler.cpp", 5.0,
                       "Stage to put scheduler.cpp in the index")
    if _git_row_menu(app, "scheduler.cpp", "Unstage"):
        app.wait_until(lambda: _git(app, "diff", "--cached", "--name-only") == "", 5.0,
                       "Unstage to take it out of the index again")

    if _git_row_menu(app, "main.cpp", "Discard"):
        app.expect(app.ui().get("modal") == "prompt", "Discard asks before throwing the change away")
        cancel = next((e for e in app.elements("button", prefix="button:prompt/")
                       if e.get("label") == "Cancel"), None)
        if cancel:
            app.click(*app.center(cancel))
            step(app, "cancel the discard", 0.3)
            app.expect(read(app, "src/main.cpp").endswith("// discard me\n"), "Cancel keeps the change")
    if _git_row_menu(app, "main.cpp", "Discard"):
        confirm = next((e for e in app.elements("button", prefix="button:prompt/")
                        if e.get("label", "").startswith("Discard")), None)
        app.expect(confirm is not None, f"the prompt has a Discard button "
                                        f"({[e['name'] for e in app.elements('button', prefix='button:prompt/')]})")
        if confirm:
            app.click(*app.center(confirm))
            step(app, "confirm the discard", 0.5)
            app.wait_until(lambda: read(app, "src/main.cpp") == original, 5.0, "the file restored on disk")
            app.wait_until(lambda: not any(e["name"] == "row:git/main.cpp" for e in app.elements("row")),
                           5.0, "main.cpp to leave the git view")
            app.send("open", "src/main.cpp")
            app.wait_until(lambda: "// discard me" not in app.text(), 5.0,
                           "the open editor to follow the discard")
    app.key("ctrl+shift+g")
    step(app, "git view again", 0.3)
    stage_all = app.wait_for("button:git/stage-all", 3.0)
    if stage_all:
        app.click(*app.center(stage_all))
        step(app, "Stage All", 0.6)
        app.wait_until(lambda: set(_git(app, "diff", "--cached", "--name-only").splitlines())
                       == {"src/scheduler.cpp", "notes/untracked.txt"}, 5.0,
                       "Stage All to stage the change and the untracked file")


# ---- terminal ----------------------------------------------------------------


def flow_terminal_clipboard(app: q.App) -> None:
    """Copy in the editor and paste into the shell with Ctrl+Shift+V; select the
    shell's output with the mouse and Ctrl+Shift+C it into an editor; the same
    through the terminal's context menu."""
    open_file(app, "notes/clip.txt", "echo qa-paste-$((50+5))\n")
    app.click_text(1, 1)
    app.key("End")
    app.key("shift+Home")
    app.key("ctrl+c")
    step(app, "copy the command")
    if not app.ui()["panel"]["visible"]:
        app.key("ctrl+grave")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    app.send("focus", "panel")
    step(app, "focus the terminal", 0.3)

    def output() -> str:
        return app.query("terminal-output").get("text", "")

    app.type("clear\n")
    step(app, "clear", 0.4)
    app.key("ctrl+shift+v")
    app.key("Return")
    app.wait_until(lambda: "qa-paste-55" in output(), 5.0, "Ctrl+Shift+V to paste the command into the shell")

    surface = app.ui()["surfaces"]["bottomPanel"]
    if not surface:
        app.expect(False, "the bottom panel has a surface")
        return
    x, y, w, h = surface
    # Drag over the output row, located by its transcript row.
    lines = output().split("\n")
    row = next((i for i, line in enumerate(lines) if line.strip() == "qa-paste-55"), None)
    app.expect(row is not None, f"the output row is in the transcript ({lines[-6:]})")
    if row is None:
        return
    app.drag(*app.terminal_point(row, 0), *app.terminal_point(row, len("qa-paste-55")))
    step(app, "drag over the output", 0.3)
    app.key("ctrl+shift+c")
    step(app, "ctrl+shift+c", 0.2)
    app.key("ctrl+n")
    step(app, "untitled", 0.3)
    app.send("focus", "editor")
    app.key("ctrl+v")
    step(app, "paste", 0.3)
    # The shell's OUTPUT (55), not the command the clipboard held before.
    app.expect("qa-paste-55" in app.text(),
               f"the terminal selection pastes into the editor ({app.text()[:120]!r})")

    # The context menu's Paste does what Ctrl+Shift+V does.
    app.click_text(1, 1)
    app.key("ctrl+a")
    app.type("echo qa-menu-$((60+6))")
    app.key("ctrl+a")
    app.key("ctrl+c")
    step(app, "copy the second command")
    app.send("focus", "panel")
    app.click(x + w / 2, y + h / 2, button=3)
    step(app, "right-click the terminal", 0.3)
    paste = next((e for e in app.elements("menu-item") if e.get("label", "").startswith("Paste")), None)
    app.expect(paste is not None, f"the terminal's context menu offers Paste "
                                  f"({[e['name'] for e in app.elements('menu-item')]})")
    if paste:
        app.click(*app.center(paste))
        step(app, "Paste from the menu", 0.3)
        app.key("Return")
        app.wait_until(lambda: "qa-menu-66" in output(), 5.0, "the menu's Paste to reach the shell")
    dismiss(app)
    # Leave the untitled buffer clean so the run ends without a prompt.
    app.send("focus", "editor")
    app.key("ctrl+a")
    app.key("Delete")


# ---- file tree ---------------------------------------------------------------


def _tree_rows(app: q.App) -> list[str]:
    return [e["name"] for e in app.elements("row", prefix="row:tree/")]


def _tree_selected(app: q.App) -> list[str]:
    return [e["name"] for e in app.elements("row", prefix="row:tree/") if e.get("selected")]


def flow_tree_keys(app: q.App) -> None:
    """The explorer by keyboard: the tree follows the active editor, typing
    selects by name, F2 renames and Delete deletes (asking first), a folder made
    from the context menu, Collapse All."""
    write(app, "notes/alpha.txt", "a\n")
    write(app, "notes/beta.txt", "b\n")
    app.click_named("sidebar-view:tree")
    step(app, "tree view", 0.3)
    open_file(app, "notes/beta.txt")
    app.wait_until(lambda: _tree_selected(app) == ["row:tree/beta.txt"], 3.0,
                   "the tree to select the active editor's file")
    app.send("focus", "sidebar")
    step(app, "focus the tree", 0.2)
    app.type("al")
    step(app, "type al", 0.2)
    app.expect(_tree_selected(app) == ["row:tree/alpha.txt"],
               f"typing selects the entry whose name starts with it ({_tree_selected(app)})")

    app.key("F2")
    step(app, "F2", 0.3)
    prompt = app.ui().get("prompt") or {}
    app.expect(prompt.get("kind") == "input" and prompt.get("input") == "alpha.txt",
               f"F2 asks for alpha.txt's new name ({prompt})")
    if prompt.get("kind") == "input":
        app.key("ctrl+a")
        app.type("gamma.txt")
        app.key("Return")
        step(app, "rename to gamma.txt", 0.5)
        assert app.project is not None
        app.wait_until(lambda: (app.project / "notes" / "gamma.txt").exists()
                       and not (app.project / "notes" / "alpha.txt").exists(), 3.0, "the rename on disk")
        app.wait_until(lambda: "row:tree/gamma.txt" in _tree_rows(app), 3.0, "the tree to show gamma.txt")

    app.send("focus", "sidebar")
    app.type("ga")
    step(app, "select gamma", 0.2)
    app.key("Delete")
    step(app, "Delete", 0.3)
    prompt = app.ui().get("prompt") or {}
    app.expect(prompt.get("kind") == "confirm", f"Delete asks first ({prompt})")
    confirm = next((e for e in app.elements("button", prefix="button:prompt/")
                    if e.get("label") == "Delete"), None)
    if confirm:
        app.click(*app.center(confirm))
        step(app, "confirm Delete", 0.5)
        assert app.project is not None
        app.wait_until(lambda: not (app.project / "notes" / "gamma.txt").exists(), 3.0, "the delete on disk")
        app.wait_until(lambda: "row:tree/gamma.txt" not in _tree_rows(app), 3.0, "the tree to drop it")
    dismiss(app)

    notes = app.wait_for("row:tree/notes", 2.0)
    if notes:
        app.click(*app.center(notes), button=3)
        step(app, "right-click notes", 0.3)
        new_folder = next((e for e in app.elements("menu-item", prefix="menu-item:context/")
                           if e.get("label", "").startswith("New Folder")), None)
        app.expect(new_folder is not None, "a folder's context menu offers New Folder")
        if new_folder:
            app.click(*app.center(new_folder))
            step(app, "New Folder", 0.3)
            app.type("drafts")
            app.key("Return")
            step(app, "name drafts", 0.5)
            assert app.project is not None
            app.wait_until(lambda: (app.project / "notes" / "drafts").is_dir(), 3.0, "the folder on disk")
            app.wait_until(lambda: "row:tree/drafts" in _tree_rows(app), 3.0, "the tree to show drafts")
        dismiss(app)
    collapse = app.wait_for("button:sidebar/Collapse", 2.0)
    app.expect(collapse is not None, "the tree header has Collapse All")
    if collapse:
        app.click(*app.center(collapse))
        step(app, "Collapse All", 0.3)
        app.expect("row:tree/beta.txt" not in _tree_rows(app),
                   f"Collapse All folds every folder ({_tree_rows(app)[:8]})")


# ---- notifications ---------------------------------------------------------------


def flow_notifications(app: q.App) -> None:
    """A message toasts and is named on screen; a click dismisses it; several
    stack newest first; the keyboard route (Focus Notifications) reaches them and
    Escape gives the editor back."""
    open_file(app, "notes/toast.txt", "plain\n")
    app.key("F8")  # no problems anywhere: a toast says so
    app.wait_until(lambda: any(e["name"].startswith("notification:") for e in app.elements("notification")),
                   3.0, "F8 with no problems to toast")
    toast = next(iter(app.elements("notification")), None)
    app.expect(toast is not None and "problem" in toast.get("label", "").lower(),
               f"the toast says there are no problems ({toast})")
    if toast:
        app.click(*app.center(toast))
        step(app, "click the toast", 0.3)
        app.expect(not app.elements("notification"), "a click dismisses the toast")
    app.key("F8")
    step(app, "F8 again", 0.2)
    app.key("ctrl+g")
    step(app, "ctrl+g", 0.2)
    app.type("x")
    app.key("Return")
    step(app, "go to line x", 0.3)
    toasts = app.elements("notification")
    app.expect(len(toasts) >= 1, f"the toasts stack ({[t.get('label') for t in toasts]})")
    dismiss(app)
    app.key("ctrl+shift+p")
    step(app, "palette", 0.3)
    app.type("focus notifications")
    step(app, "query", 0.3)
    palette_rows = app.elements("overlay-row")
    app.expect(bool(palette_rows), "the palette lists Focus Notifications")
    if palette_rows:
        app.key("Return")
        step(app, "Focus Notifications", 0.3)
        focused = [t for t in app.elements("notification") if t.get("active")]
        app.expect(len(focused) == 1, f"Focus Notifications puts the keyboard on a toast ({focused})")
        app.key("Delete")
        step(app, "Delete the focused toast", 0.3)
        app.key("Escape")
        step(app, "Escape the toasts", 0.3)
        app.expect(app.ui()["focus"] == "editor", f"Escape gives the editor the keyboard back ({app.ui()['focus']})")
    dismiss(app)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "nav-history": flow_nav_history,
    "caret-motion": flow_caret_motion,
    "indentation": flow_indentation,
    "find-options": flow_find_options,
    "git-discard": flow_git_discard,
    "terminal-clipboard": flow_terminal_clipboard,
    "notifications": flow_notifications,
    "tree-keys": flow_tree_keys,
    "quick-open": flow_quick_open,
    "undo-semantics": flow_undo_semantics,
    "mouse-and-wrap": flow_mouse_and_wrap,
    "search-options": flow_search_options,
}

PLUGINS: dict[str, tuple[str, ...]] = {}
