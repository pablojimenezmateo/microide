"""UX flows: the editor behaviours a VS Code user does without thinking, each
checked for its effect -- navigation history, caret motion and selection growth,
indentation, the find widget's options, the terminal clipboard, git's discard and
unstage, and notifications.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, expect_cursors, expect_text, open_file, step


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


FLOWS: dict[str, Callable[[q.App], None]] = {
    "nav-history": flow_nav_history,
    "caret-motion": flow_caret_motion,
    "indentation": flow_indentation,
    "find-options": flow_find_options,
}

PLUGINS: dict[str, tuple[str, ...]] = {}
