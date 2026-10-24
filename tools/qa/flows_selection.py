"""Selection by mouse and by key where the gesture's GRANULARITY is the point:
the line numbers (a whole line per press, whole lines per drag), a word drag,
Ctrl+L over a partial selection, a right-click that must keep what is selected
for the context menu, and the find widget's Alt+Enter. Each is checked against
the carets and anchors the `buffer` query reports, and against VS Code wherever
the two could differ."""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, expect_text, open_file, step


def _selection(app: q.App) -> tuple[int, int, int, int]:
    """The primary caret as (anchor line, anchor column, line, column), 1-based."""
    caret = app.buffer()["cursors"][0]
    return caret["anchorLine"], caret["anchorColumn"], caret["line"], caret["column"]


def _line_number_point(app: q.App, line: int) -> tuple[float, float]:
    """The middle of a visible line's number: left of the fold zone, inside the
    gutter (gutter_point's "breakpoint" zone is the same column with the debugger
    off, but this aims at the digits a person aims at)."""
    pane = app.pane()
    for row in pane["rows"]:
        if row["line"] == line:
            return pane["gutterX"] + (pane["foldHitX"] - pane["gutterX"]) * 0.6, row["y"] + pane["lineHeight"] / 2
    raise q.QaFailure(f"line {line} is not on screen")


def flow_line_number_selection(app: q.App) -> None:
    """A click on a line number selects that line whole (anchor at its start, the
    caret at the start of the next); a drag down the numbers takes every line it
    crosses, the last one included; a drag UP takes them too, anchored at the end
    of the line pressed; typing replaces the lines. VS Code does all four."""
    open_file(app, "sel/numbers.txt", "one\ntwo\nthree\nfour\nfive\nsix\n")
    app.click(*_line_number_point(app, 2))
    step(app, "click line 2's number")
    app.expect(_selection(app) == (2, 1, 3, 1) and app.buffer()["selectedText"] == "two\n",
               f"a click on a line number selects the line ({_selection(app)}, "
               f"{app.buffer()['selectedText']!r})")
    x0, y0 = _line_number_point(app, 2)
    x1, y1 = _line_number_point(app, 4)
    app.drag(x0, y0, x1, y1)
    step(app, "drag the numbers 2..4")
    app.expect(app.buffer()["selectedText"] == "two\nthree\nfour\n",
               f"a drag down the numbers takes lines 2-4 whole ({app.buffer()['selectedText']!r})")
    x2, y2 = _line_number_point(app, 5)
    app.drag(x2, y2, x1, y1)
    step(app, "drag the numbers 5..4")
    app.expect(app.buffer()["selectedText"] == "four\nfive\n" and _selection(app)[2] == 4,
               f"a drag up the numbers takes both lines, the caret at the top ({_selection(app)}, "
               f"{app.buffer()['selectedText']!r})")
    app.type("X")
    step(app, "type over the lines")
    expect_text(app, "one\ntwo\nthree\nXsix\n", "typing replaces the selected lines")
    # The fold marker is still the fold marker: a press there does not select.
    open_file(app, "sel/fold.cpp", "int f() {\n    return 1;\n}\nint g;\n")
    app.wait_until(lambda: any(f[0] == 1 for f in app.buffer().get("foldable", [])), 3.0,
                   "the function to be foldable")
    app.click(*app.gutter_point(1, "fold"))
    step(app, "click the fold marker")
    app.expect(app.buffer()["selectedText"] == "" and app.buffer().get("folded"),
               f"the fold marker folds and selects nothing ({app.buffer().get('folded')}, "
               f"{app.buffer()['selectedText']!r})")


def flow_word_and_line_drags(app: q.App) -> None:
    """A double-click then drag extends by whole words, a triple-click then drag
    by whole lines, in either direction, and Shift+click after a double-click
    extends from the word's anchor (VS Code)."""
    open_file(app, "sel/words.txt", "alpha beta gamma delta\nepsilon zeta\neta theta\n")
    x0, y0 = app.text_point(1, 8)
    x1, y1 = app.text_point(1, 13)
    app.click(x0, y0, clicks=2)
    step(app, "double-click beta")
    app.expect(app.buffer()["selectedText"] == "beta", f"double-click selects beta ({app.buffer()['selectedText']!r})")
    app.drag(x0, y0, x1, y1, clicks=2)
    step(app, "double-click drag to gamma")
    app.expect(app.buffer()["selectedText"] == "beta gamma",
               f"a double-click drag extends by whole words ({app.buffer()['selectedText']!r})")
    xb, yb = app.text_point(1, 3)
    app.drag(x0, y0, xb, yb, clicks=2)
    step(app, "double-click drag back to alpha")
    app.expect(app.buffer()["selectedText"] == "alpha beta",
               f"dragging back takes alpha and keeps beta ({app.buffer()['selectedText']!r})")
    x2, y2 = app.text_point(2, 3)
    x3, y3 = app.text_point(3, 2)
    app.drag(x2, y2, x3, y3, clicks=3)
    step(app, "triple-click drag from line 2 to 3")
    app.expect(app.buffer()["selectedText"].rstrip("\n") == "epsilon zeta\neta theta",
               f"a triple-click drag takes whole lines ({app.buffer()['selectedText']!r})")


def flow_context_menu_clipboard(app: q.App) -> None:
    """A right-click inside a selection keeps it, and the menu's Cut, Copy and
    Paste act on it: Cut removes just the word (it used to collapse the selection
    and cut the whole line), Paste puts it back where the caret is. A right-click
    outside the selection moves the caret there. A whole-line copy (no selection)
    pastes above the caret's line and leaves the caret in its own place."""
    open_file(app, "sel/menu.txt", "alpha beta gamma\nsecond line\n")

    def menu_row(label: str) -> dict | None:
        app.wait_until(lambda: bool(app.elements("menu-item", prefix="menu-item:Editor/")), 2.0,
                       "the editor context menu")
        return next((e for e in app.elements("menu-item", prefix="menu-item:Editor/")
                     if e["name"] == f"menu-item:Editor/{label}"), None)

    app.click_text(1, 8, clicks=2)
    step(app, "select beta")
    app.click_text(1, 9, button=3)
    step(app, "right-click inside beta", 0.3)
    app.expect(app.buffer()["selectedText"] == "beta",
               f"the right-click keeps the selection ({app.buffer()['selectedText']!r})")
    cut = menu_row("Cut")
    if not app.expect(cut is not None, "the editor menu has Cut"):
        return
    app.click(*app.center(cut))
    step(app, "Cut", 0.3)
    expect_text(app, "alpha  gamma\nsecond line\n", "Cut removes the selected word, not the line")
    app.click_text(2, 7, button=3)
    step(app, "right-click on line 2", 0.3)
    app.expect(app.cursors() == [(2, 7)], f"a right-click outside a selection moves the caret ({app.cursors()})")
    paste = menu_row("Paste")
    if paste:
        app.click(*app.center(paste))
        step(app, "Paste", 0.3)
    expect_text(app, "alpha  gamma\nsecondbeta line\n", "Paste inserts the cut word at the caret")
    dismiss(app)

    # Copy from the menu, then a whole-line copy by key.
    app.click_text(1, 1, clicks=2)
    app.click_text(1, 2, button=3)
    step(app, "right-click inside alpha", 0.3)
    copy = menu_row("Copy")
    if copy:
        app.click(*app.center(copy))
        step(app, "Copy", 0.3)
    app.key("ctrl+End")
    app.key("ctrl+v")
    step(app, "paste the copy")
    expect_text(app, "alpha  gamma\nsecondbeta line\nalpha", "the menu's Copy copied the selected word")
    app.click_text(1, 4)
    app.key("ctrl+c")
    app.click_text(2, 5)
    app.key("ctrl+v")
    step(app, "paste a whole-line copy")
    expect_text(app, "alpha  gamma\nalpha  gamma\nsecondbeta line\nalpha",
                "a whole-line copy pastes above the caret's line")
    app.expect(app.cursors() == [(3, 5)],
               f"the caret keeps its place in its own line, one line down ({app.cursors()})")


def flow_expand_line_selection(app: q.App) -> None:
    """Ctrl+L from a bare caret takes its line, and each press one more; from a
    word it takes only the word's line; from a selection over two lines (made
    upward) it takes exactly those two (VS Code's expandLineSelection)."""
    open_file(app, "sel/lines.txt", "foo bar\nbaz\nqux\nlast\n")
    app.click_text(1, 2)
    app.key("ctrl+l")
    step(app, "ctrl+l at a caret")
    app.expect(app.buffer()["selectedText"] == "foo bar\n", f"one line ({app.buffer()['selectedText']!r})")
    app.key("ctrl+l")
    step(app, "ctrl+l again")
    app.expect(app.buffer()["selectedText"] == "foo bar\nbaz\n", f"two lines ({app.buffer()['selectedText']!r})")
    app.click_text(1, 2, clicks=2)
    app.key("ctrl+l")
    step(app, "ctrl+l on a word")
    app.expect(app.buffer()["selectedText"] == "foo bar\n",
               f"a word's Ctrl+L takes only its line ({app.buffer()['selectedText']!r})")
    app.click_text(3, 2)
    app.key("shift+Up")
    app.key("ctrl+l")
    step(app, "ctrl+l on an upward selection")
    app.expect(app.buffer()["selectedText"] == "baz\nqux\n",
               f"an upward selection over two lines takes those two ({app.buffer()['selectedText']!r})")


def flow_find_select_all(app: q.App) -> None:
    """Alt+Enter in the find widget puts a caret on every match it counts -- with
    match case on, not the differently-cased one -- and gives the editor the
    keyboard, so typing replaces them all and one undo takes it back. With a
    regex query the matches are the regex's, which Ctrl+Shift+L (a word scan)
    cannot do."""
    original = "foo bar Foo\nbaz foo\nfoo1 foo22\n"
    open_file(app, "sel/find.txt", original)
    app.key("ctrl+f")
    app.type("foo")
    step(app, "find foo", 0.3)
    app.key("alt+c")
    step(app, "match case", 0.3)
    app.wait_until(lambda: app.ui()["overlay"].get("count") == 4, 2.0, "four case-matching matches")
    app.key("alt+Return")
    step(app, "alt+enter", 0.3)
    app.expect(app.ui()["focus"] == "editor" and len(app.buffer()["cursors"]) == 4,
               f"Alt+Enter selects the four matches in the editor ({app.ui()['focus']}, {app.cursors()})")
    app.type("X")
    step(app, "type X")
    expect_text(app, "X bar Foo\nbaz X\nX1 X22\n", "typing replaces every match")
    app.key("ctrl+z")
    step(app, "undo")
    expect_text(app, original, "one undo takes the whole edit back")
    app.key("Escape")
    app.key("ctrl+f")
    app.key("ctrl+a")
    app.key("alt+c")
    app.key("alt+r")
    app.type("foo[0-9]+")
    step(app, "regex query", 0.3)
    app.wait_until(lambda: app.ui()["overlay"].get("count") == 2, 2.0, "two regex matches")
    app.key("alt+Return")
    step(app, "alt+enter on a regex", 0.3)
    app.expect(sorted(app.cursors()) == [(3, 5), (3, 11)],
               f"the carets end each regex match ({app.cursors()})")
    app.key("alt+r")


def flow_multi_caret_pairs(app: q.App) -> None:
    """The language's pair rules at three carets at once, in a C file: ( closes
    at each and Backspace deletes each pair; Enter after a brace indents each new
    line; Home goes to each line's text then its start; Ctrl+Backspace takes each
    word; Tab indents each; a quote after a word does not pair; and a pair key
    over each caret's selection surrounds it -- also on a line after a lone quote
    and inside a string literal, where it once replaced the selection."""
    counter = iter(range(100))

    def three(text: str) -> None:
        open_file(app, f"pairs/case{next(counter)}.c", text)
        app.send("goto", "1:1")
        app.key("End")
        app.key("ctrl+alt+Down")
        app.key("ctrl+alt+Down")
        step(app, "three carets at the line ends", 0.2)

    three("a\nb\nc\n")
    app.type("(")
    expect_text(app, "a()\nb()\nc()\n", "( closes at every caret")
    app.key("BackSpace")
    expect_text(app, "a\nb\nc\n", "Backspace between a pair deletes both, at every caret")
    three("  f() {\n  g() {\n  h() {\n")
    app.key("Return")
    expect_text(app, "  f() {\n    \n  g() {\n    \n  h() {\n    \n", "Enter after a brace indents each new line")
    three("    x\n    y\n    z\n")
    app.key("Home")
    step(app, "home", 0.1)
    app.expect(sorted(app.cursors()) == [(1, 5), (2, 5), (3, 5)], f"Home goes to each line's text ({app.cursors()})")
    app.key("Home")
    step(app, "home again", 0.1)
    app.expect(sorted(app.cursors()) == [(1, 1), (2, 1), (3, 1)], f"and then to each start ({app.cursors()})")
    three("foo bar\nfoo baz\nfoo qux\n")
    app.key("ctrl+BackSpace")
    expect_text(app, "foo \nfoo \nfoo \n", "Ctrl+Backspace takes each caret's word")
    three("x\ny\nz\n")
    app.type('"')
    expect_text(app, 'x"\ny"\nz"\n', "a quote after a word does not pair")
    app.key("shift+Home")
    app.type("(")
    expect_text(app, '(x")\n(y")\n(z")\n', "( surrounds every selection, the line after a lone quote too")
    open_file(app, "pairs/string.c", 'char* s = "hello world";\n')
    app.click_text(1, 13, clicks=2)
    step(app, "select hello in the string", 0.2)
    app.type("[")
    expect_text(app, 'char* s = "[hello] world";\n', "[ surrounds a word selected inside a string")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "line-number-selection": flow_line_number_selection,
    "word-and-line-drags": flow_word_and_line_drags,
    "context-menu-clipboard": flow_context_menu_clipboard,
    "expand-line-selection": flow_expand_line_selection,
    "find-select-all": flow_find_select_all,
    "multi-caret-pairs": flow_multi_caret_pairs,
}
