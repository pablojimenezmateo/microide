"""Editing flows: what a person does to text, checked against the buffer.

Each flow types, clicks and presses keys through the real window and states what
the buffer, its carets, its folds and the file on disk must be afterwards. The
expected behaviour is VS Code's wherever the two could differ.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import (dismiss, expect_cursors, expect_text, indent_unit, open_file, read,
                          step, write)


def flow_type_and_save(app: q.App) -> None:
    """Type, delete by character and word, undo to the start, redo to the end,
    save: the buffer and the file agree at every point that should."""
    original = "alpha\nbeta\n"
    open_file(app, "notes/typing.txt", original)
    app.key("ctrl+End")
    step(app, "ctrl+End")
    expect_cursors(app, [(3, 1)], "ctrl+End goes to the last line")
    app.type("gamma")
    step(app, "type gamma")
    expect_text(app, "alpha\nbeta\ngamma", "typing appends")
    app.expect(app.buffer()["dirty"], "typing makes the buffer dirty")
    app.expect((app.active_tab() or {}).get("dirty") is True, "typing marks the tab dirty")
    app.key("Return")
    app.type("delta")
    step(app, "Return delta")
    expect_text(app, "alpha\nbeta\ngamma\ndelta", "Return starts a line")
    app.key("BackSpace")
    app.key("BackSpace")
    step(app, "BackSpace x2")
    expect_text(app, "alpha\nbeta\ngamma\ndel", "BackSpace deletes a character")
    app.key("ctrl+BackSpace")
    step(app, "ctrl+BackSpace")
    expect_text(app, "alpha\nbeta\ngamma\n", "ctrl+BackSpace deletes the word")
    final = app.text()
    for _ in range(30):
        if app.text() == original:
            break
        app.key("ctrl+z")
        step(app, "ctrl+z", 0.06)
    expect_text(app, original, "undo returns to the opened text")
    app.expect(not app.buffer()["dirty"], "undo to the saved text clears dirty (VS Code)")
    for _ in range(30):
        if app.text() == final:
            break
        app.key("ctrl+shift+z")
        step(app, "ctrl+shift+z", 0.06)
    expect_text(app, final, "redo returns to the last edit")
    app.key("ctrl+s")
    step(app, "ctrl+s", 0.4)
    app.wait_until(lambda: not app.buffer()["dirty"], 3.0, "save to clear dirty")
    app.expect(read(app, "notes/typing.txt") == final,
               f"save writes the buffer (disk has {read(app, 'notes/typing.txt')!r})")
    # Ctrl+Y is the other redo.
    app.key("ctrl+z")
    step(app, "ctrl+z after save")
    app.key("ctrl+y")
    step(app, "ctrl+y")
    expect_text(app, final, "ctrl+y redoes")


def flow_selection_and_clipboard(app: q.App) -> None:
    """Select by double/triple click, drag and shift+click; copy, cut, paste."""
    open_file(app, "notes/clip.txt", "one two three\nfour five six\nseven\n")
    app.click_text(1, 6, clicks=2)
    step(app, "double-click two")
    app.expect(app.buffer()["selectedText"] == "two",
               f"double-click selects the word (got {app.buffer()['selectedText']!r})")
    app.key("ctrl+c")
    app.key("ctrl+End")
    app.key("ctrl+v")
    step(app, "copy, ctrl+End, paste")
    expect_text(app, "one two three\nfour five six\nseven\ntwo", "paste inserts the copy")
    app.click_text(2, 3, clicks=3)
    step(app, "triple-click line 2")
    selected = app.buffer()["selectedText"]
    app.expect(selected.rstrip("\n") == "four five six",
               f"triple-click selects the line (got {selected!r})")
    app.drag_text((1, 1), (1, 4))
    step(app, "drag over one")
    app.expect(app.buffer()["selectedText"] == "one",
               f"a drag selects what it covers (got {app.buffer()['selectedText']!r})")
    app.click_text(1, 9, modifiers=("shift",))
    step(app, "shift+click")
    app.expect(app.buffer()["selectedText"] == "one two ",
               f"shift+click extends the selection (got {app.buffer()['selectedText']!r})")
    # Cut with no selection cuts the whole line (VS Code).
    app.click_text(3, 2)
    app.key("ctrl+x")
    step(app, "ctrl+x with no selection")
    expect_text(app, "one two three\nfour five six\ntwo", "ctrl+x cuts the caret's line")
    app.key("ctrl+Home")
    app.key("ctrl+v")
    step(app, "paste the cut line at the top")
    expect_text(app, "seven\none two three\nfour five six\ntwo",
                "pasting a cut line inserts it above the caret's line")
    app.key("ctrl+a")
    step(app, "ctrl+a")
    app.expect(app.buffer()["selectedText"] == app.text(), "ctrl+a selects everything")
    app.type("z")
    step(app, "type over everything")
    expect_text(app, "z", "typing replaces the selection")
    app.key("shift+Home")
    app.key("Delete")
    step(app, "shift+Home Delete")
    expect_text(app, "", "Delete removes the selection")


def flow_multi_cursor(app: q.App) -> None:
    """Ctrl+D, Ctrl+Shift+L, Alt+click, Ctrl+Alt+Down, Shift+Alt+I and a box
    drag: every caret edits, and one undo takes a multi-caret edit back."""
    text = "foo = 1\nfoo = 2\nfoo = 3\n"
    open_file(app, "notes/multi.txt", text)
    app.click_text(1, 2)
    for count in (1, 2, 3):
        app.key("ctrl+d")
        step(app, f"ctrl+d {count}")
    app.expect(len(app.cursors()) == 3, f"three ctrl+d select three foo (carets {app.cursors()})")
    app.type("bar")
    step(app, "type over three selections")
    expect_text(app, "bar = 1\nbar = 2\nbar = 3\n", "typing replaces every selection")
    app.key("ctrl+z")
    step(app, "undo the multi-caret edit")
    expect_text(app, text, "one undo takes the whole multi-caret edit back")
    app.key("Escape")
    step(app, "Escape")
    app.expect(len(app.cursors()) == 1, "Escape leaves one caret")
    app.click_text(2, 1)
    app.key("ctrl+shift+l")
    step(app, "ctrl+shift+l")
    app.expect(len(app.cursors()) == 3, f"ctrl+shift+l selects every foo (carets {app.cursors()})")
    app.type("q")
    step(app, "type at all matches")
    expect_text(app, "q = 1\nq = 2\nq = 3\n", "ctrl+shift+l edits every match")
    app.key("Escape")
    app.click_text(1, 5)
    app.click_text(2, 5, modifiers=("alt",))
    app.click_text(3, 5, modifiers=("alt",))
    step(app, "alt+click two more carets")
    app.expect(len(app.cursors()) == 3, f"alt+click adds carets (carets {app.cursors()})")
    app.type("-")
    step(app, "type at three carets")
    expect_text(app, "q = -1\nq = -2\nq = -3\n", "alt+click carets each type")
    app.key("Escape")
    app.click_text(1, 1)
    app.key("ctrl+alt+Down")
    app.key("ctrl+alt+Down")
    step(app, "ctrl+alt+Down x2")
    app.expect(len(app.cursors()) == 3, f"ctrl+alt+Down adds carets below ({app.cursors()})")
    app.type("> ")
    step(app, "type at column carets")
    expect_text(app, "> q = -1\n> q = -2\n> q = -3\n", "carets added below each type")
    app.key("Escape")
    app.key("ctrl+a")
    app.key("shift+alt+i")
    step(app, "shift+alt+i")
    app.expect(len(app.cursors()) >= 3, f"shift+alt+i puts a caret on every line end ({app.cursors()})")
    app.type(";")
    step(app, "type at line ends")
    app.expect(app.text().startswith("> q = -1;\n> q = -2;\n> q = -3;"),
               f"shift+alt+i carets type at line ends (got {app.text()!r})")
    app.key("Escape")
    # A box (column) selection: shift+alt drag from line 1 col 3 to line 3 col 4.
    app.click_text(1, 3)
    app.drag_text((1, 3), (3, 4), modifiers=("shift", "alt"))
    step(app, "shift+alt box drag")
    app.expect(len(app.cursors()) == 3, f"a box drag makes one selection per line ({app.cursors()})")
    app.key("Delete")
    step(app, "delete the box")
    # Columns 3..4 of "> q = -1;" are the "q".
    app.expect(app.text().startswith(">  = -1;\n>  = -2;\n>  = -3;"),
               f"deleting a box removes the column (got {app.text()!r})")


def flow_line_operations(app: q.App) -> None:
    """Move, duplicate, delete, insert, indent, comment lines (VS Code chords)."""
    open_file(app, "src/lines.cpp", "int a;\nint b;\nint c;\n")
    app.click_text(1, 1)
    app.key("alt+Down")
    step(app, "alt+Down")
    expect_text(app, "int b;\nint a;\nint c;\n", "alt+Down moves the line down")
    expect_cursors(app, [(2, 1)], "the caret moves with the line")
    app.key("alt+Up")
    step(app, "alt+Up")
    expect_text(app, "int a;\nint b;\nint c;\n", "alt+Up moves it back")
    app.key("shift+alt+Down")
    step(app, "shift+alt+Down")
    expect_text(app, "int a;\nint a;\nint b;\nint c;\n", "shift+alt+Down duplicates the line")
    app.key("ctrl+shift+k")
    step(app, "ctrl+shift+k")
    expect_text(app, "int a;\nint b;\nint c;\n", "ctrl+shift+k deletes the line")
    # shift+alt+Down left the caret on the copy (VS Code), so ctrl+shift+k
    # deleted that copy and the caret is on "int b;" now.
    expect_cursors(app, [(2, 1)], "after deleting the copy the caret is on the next line")
    app.click_text(1, 3)
    app.key("ctrl+Return")
    app.type("x")
    step(app, "ctrl+Return x")
    lines = app.text().split("\n")
    app.expect(lines[:3] == ["int a;", "x", "int b;"],
               f"ctrl+Return opens a line below the caret's, wherever the caret is (got {lines[:3]})")
    app.key("ctrl+z")
    app.key("ctrl+z")
    step(app, "undo the insert")
    app.click_text(2, 1)
    app.key("ctrl+slash")
    step(app, "ctrl+/")
    app.expect(app.text().split("\n")[1].startswith("//"),
               f"ctrl+/ comments the line (got {app.text().split(chr(10))[1]!r})")
    app.key("ctrl+slash")
    step(app, "ctrl+/ again")
    app.expect(app.text().split("\n")[1] == "int b;",
               f"ctrl+/ again uncomments (got {app.text().split(chr(10))[1]!r})")
    unit = indent_unit(app)
    app.key("ctrl+bracketright")
    step(app, "ctrl+]")
    app.expect(app.text().split("\n")[1] == unit + "int b;",
               f"ctrl+] indents by one unit {unit!r} (got {app.text().split(chr(10))[1]!r})")
    app.key("ctrl+bracketleft")
    step(app, "ctrl+[")
    app.expect(app.text().split("\n")[1] == "int b;", "ctrl+[ outdents")
    # Tab with a multi-line selection indents every line; shift+Tab outdents.
    app.drag_text((1, 1), (3, 3))
    app.key("Tab")
    step(app, "Tab over a selection")
    app.expect(all(line.startswith(unit) for line in app.text().split("\n")[:3]),
               f"Tab indents every selected line (got {app.text()!r})")
    app.key("shift+Tab")
    step(app, "shift+Tab")
    expect_text(app, "int a;\nint b;\nint c;\n", "shift+Tab outdents them back")


def flow_auto_close_and_indent(app: q.App) -> None:
    """Brackets and quotes close, typing the closer steps over it, Enter between
    braces indents (VS Code's defaults)."""
    open_file(app, "src/auto.cpp", "")
    app.type("f(")
    step(app, "type f(")
    expect_text(app, "f()", "( auto-closes")
    app.type("x)")
    step(app, "type x)")
    expect_text(app, "f(x)", "typing ) steps over the auto-closed one")
    app.type(" {")
    app.key("Return")
    step(app, "{ Return")
    unit = indent_unit(app)
    app.expect(app.text() == f"f(x) {{\n{unit}\n}}",
               f"Enter between braces indents and pushes the brace down (got {app.text()!r})")
    expect_cursors(app, [(2, len(unit) + 1)], "the caret sits on the indented line")
    app.type('s = "')
    step(app, 'type s = "')
    app.expect('s = ""' in app.text(), f'" auto-closes (got {app.text()!r})')
    app.key("BackSpace")
    step(app, "BackSpace inside an empty pair")
    app.expect('s = \n' in app.text() or app.text().split("\n")[1].endswith("s = "),
               f"BackSpace in an empty pair deletes both (got {app.text()!r})")


def flow_folding(app: q.App) -> None:
    """Fold by gutter click and by chord, fold/unfold all, and caret motion
    across a collapsed region."""
    open_file(app, "src/scheduler.cpp")
    foldable = app.buffer()["foldable"]
    app.expect([12, 24] in foldable, f"Scheduler::run is foldable ({foldable})")
    app.click(*app.gutter_point(12))
    step(app, "click the fold marker of line 12")
    app.expect(app.buffer()["folded"] == [[12, 24]], f"the click folds 12-24 ({app.buffer()['folded']})")
    lines = [row["line"] for row in app.pane()["rows"]]
    app.expect(13 not in lines and 25 in lines, f"the folded body is off screen (rows {lines[:16]})")
    app.click_text(12, 1)
    app.key("Down")
    step(app, "Down over the fold")
    app.expect(app.cursors()[0][0] == 25, f"Down from the opener skips the fold ({app.cursors()})")
    app.key("Up")
    step(app, "Up over the fold")
    app.expect(app.cursors()[0][0] == 12, f"Up lands back on the opener ({app.cursors()})")
    app.click(*app.gutter_point(12))
    step(app, "click the marker again")
    app.expect(app.buffer()["folded"] == [], "a second click unfolds")
    app.click_text(9, 5)
    app.key("ctrl+shift+bracketleft")
    step(app, "ctrl+shift+[")
    app.expect([8, 10] in app.buffer()["folded"], f"ctrl+shift+[ folds the caret's block ({app.buffer()['folded']})")
    app.key("ctrl+shift+bracketright")
    step(app, "ctrl+shift+]")
    app.expect(app.buffer()["folded"] == [], "ctrl+shift+] unfolds it")
    app.send("fold-all")
    step(app, "fold-all")
    app.expect(len(app.buffer()["folded"]) >= 2, f"fold-all folds every block ({app.buffer()['folded']})")
    app.send("unfold-all")
    step(app, "unfold-all")
    app.expect(app.buffer()["folded"] == [], "unfold-all unfolds")
    # Editing the opener line of a collapsed fold keeps the fold.
    app.click(*app.gutter_point(8))
    app.click_text(8, 1)
    app.key("End")
    app.type(" ")
    step(app, "type at the end of a folded opener")
    app.expect([8, 10] in app.buffer()["folded"], f"typing on the opener keeps the fold ({app.buffer()['folded']})")
    app.key("ctrl+z")
    step(app, "undo")


def flow_find_replace(app: q.App) -> None:
    """Find: type a query, step matches by Enter, F3, Shift+F3 and the widget
    buttons, toggle match case by click; replace one and all; Escape returns
    to the editor."""
    original = "Total total TOTAL\ntotal = 0\n"
    open_file(app, "notes/find.txt", original)
    app.key("ctrl+f")
    step(app, "ctrl+f", 0.3)
    app.expect(app.ui()["overlay"]["mode"] == "buffer-search", "ctrl+f opens the find widget")
    app.type("total")
    step(app, "type total", 0.3)
    app.expect(app.field_text("field:find/search") == "total", "the query lands in the find field")
    app.expect(app.ui()["overlay"]["count"] == 4, f"four case-insensitive matches ({app.ui()['overlay']['count']})")
    first = app.cursors()[0]
    app.key("Return")
    step(app, "Enter")
    second = app.cursors()[0]
    app.expect(second != first, f"Enter moves to the next match ({first} -> {second})")
    app.key("shift+Return")
    step(app, "shift+Enter")
    app.expect(app.cursors()[0] == first, f"shift+Enter goes back ({app.cursors()[0]} vs {first})")
    app.click_named("button:find/next")
    step(app, "click next")
    app.expect(app.cursors()[0] == second, "the next button moves forward")
    app.click_named("button:find/previous")
    step(app, "click previous")
    app.expect(app.cursors()[0] == first, "the previous button moves back")
    app.click_named("button:find/match-case")
    step(app, "toggle match case", 0.3)
    app.expect(app.element("button:find/match-case").get("active") is True, "match case shows on")
    app.expect(app.ui()["overlay"]["count"] == 2, f"match case leaves two matches ({app.ui()['overlay']['count']})")
    app.click_named("button:find/match-case")
    step(app, "toggle match case off", 0.3)
    app.key("Escape")
    step(app, "Escape the find widget")
    app.expect(not app.ui()["overlay"]["visible"], "Escape closes the find widget")
    app.expect(app.ui()["focus"] == "editor", "and the editor has the keyboard again")
    app.key("F3")
    step(app, "F3 with the widget closed")
    app.key("ctrl+h")
    step(app, "ctrl+h", 0.3)
    app.expect(app.ui()["overlay"]["mode"] == "buffer-replace", "ctrl+h opens find and replace")
    app.click_named("field:find/replace")
    app.type("sum")
    step(app, "type the replacement", 0.2)
    app.expect(app.field_text("field:find/replace") == "sum", "the replacement lands in its field")
    app.click_named("button:find/replace")
    step(app, "replace one", 0.3)
    app.expect(app.text().count("sum") == 1, f"replace changes one match (got {app.text()!r})")
    app.click_named("button:find/replace-all")
    step(app, "replace all", 0.3)
    expect_text(app, "sum sum sum\nsum = 0\n", "replace all changes every match")
    app.key("Escape")
    app.key("ctrl+z")
    step(app, "undo replace all")
    app.expect(app.text().count("sum") == 1, f"one undo takes replace-all back (got {app.text()!r})")


def flow_goto_line(app: q.App) -> None:
    open_file(app, "src/scheduler.cpp")
    app.key("ctrl+g")
    step(app, "ctrl+g", 0.25)
    ui = app.ui()
    app.expect(ui.get("modal") == "prompt" and (ui.get("prompt") or {}).get("kind") == "input",
               f"ctrl+g opens a line prompt (modal {ui.get('modal')})")
    app.type("20")
    app.key("Return")
    step(app, "20 Enter", 0.25)
    app.expect(app.cursors()[0][0] == 20, f"go to line 20 puts the caret there ({app.cursors()})")
    app.expect(app.ui().get("modal") is None, "the prompt closes")
    app.key("ctrl+g")
    app.type("5:9")
    app.key("Return")
    step(app, "5:9 Enter", 0.25)
    app.expect(app.cursors()[0] == (5, 9) or app.cursors()[0][0] == 5,
               f"line:column goes to the column ({app.cursors()})")


def flow_unsaved_prompt(app: q.App) -> None:
    """Close a dirty tab: Cancel keeps it, Don't Save discards, Save writes."""
    open_file(app, "notes/dirty.txt", "keep\n")
    for choice, expect_open, expect_disk in (("Cancel", True, "keep\n"),
                                              ("Discard", False, "keep\n"),
                                              ("Save", False, "xkeep\n")):
        if not (app.active_tab() or {}).get("title") == "dirty.txt":
            open_file(app, "notes/dirty.txt")
        app.click_text(1, 1)
        if not app.buffer()["dirty"]:
            app.type("x")
        step(app, f"dirty before {choice}")
        app.key("ctrl+w")
        step(app, f"ctrl+w before {choice}", 0.25)
        app.expect(app.ui().get("modal") == "unsaved-prompt", f"closing a dirty tab asks ({choice})")
        try:
            app.click_named(f"button:prompt/{choice}")
        except q.QaFailure as failure:
            app.expect(False, f"no {choice} button: {failure}")
            dismiss(app)
            continue
        step(app, f"click {choice}", 0.3)
        titles = [tab["title"] for group in app.editor_state()["groups"] for tab in group["tabs"]]
        app.expect(("dirty.txt" in titles) == expect_open,
                   f"{choice}: the tab is {'open' if 'dirty.txt' in titles else 'closed'}")
        app.expect(read(app, "notes/dirty.txt") == expect_disk,
                   f"{choice}: disk has {read(app, 'notes/dirty.txt')!r}, want {expect_disk!r}")
        app.expect(app.ui().get("modal") is None, f"{choice} dismisses the prompt")
        if choice == "Cancel":
            app.expect(app.buffer()["dirty"], "Cancel keeps the edit")
            app.key("ctrl+z")
            step(app, "undo the edit")
            app.type("x")


def flow_external_change(app: q.App) -> None:
    """A file changed on disk: a clean buffer reloads; a dirty one shows the
    banner, whose Reload, Keep and Overwrite do what they say."""
    open_file(app, "notes/disk.txt", "one\n")
    write(app, "notes/disk.txt", "two\n")
    app.wait_until(lambda: app.text() == "two\n", 6.0, "a clean buffer to reload from disk")
    step(app, "reloaded")
    app.click_text(1, 1)
    app.type("local ")
    step(app, "dirty the buffer")
    write(app, "notes/disk.txt", "three\n")
    banner = app.wait_for("button:banner/Reload", 6.0)
    step(app, "banner up")
    if banner:
        app.click_named("button:banner/Reload")
        step(app, "click Reload", 0.3)
        expect_text(app, "three\n", "Reload takes the disk's text")
        app.expect(not app.buffer()["dirty"], "Reload leaves the buffer clean")
    # No trailing space: saving trims trailing whitespace by default.
    app.type("mine")
    step(app, "dirty it again")
    mine = app.text()
    write(app, "notes/disk.txt", "four\n")
    if app.wait_for("button:banner/Overwrite", 6.0):
        app.click_named("button:banner/Overwrite")
        step(app, "click Overwrite", 0.4)
        app.wait_until(lambda: read(app, "notes/disk.txt") == mine, 3.0,
                       "Overwrite to write the buffer")
        app.expect(not app.buffer()["dirty"], "Overwrite leaves the buffer clean")
    app.type("x")
    step(app, "dirty a third time")
    kept = app.text()
    write(app, "notes/disk.txt", "five\n")
    if app.wait_for("button:banner/dismiss", 6.0):
        app.click_named("button:banner/dismiss")
        step(app, "dismiss the banner", 0.3)
        app.expect(app.text() == kept, "dismissing keeps the buffer")
        app.expect(app.buffer()["dirty"], "and leaves it dirty")
        app.expect(not any(e["name"].startswith("button:banner/") for e in app.elements("button")),
                   "and the banner goes")


def flow_wrap_and_scroll(app: q.App) -> None:
    """Soft wrap a long line; scroll by wheel and keys; the caret stays on
    screen after every jump."""
    long_line = " ".join(f"word{i}" for i in range(120))
    body = "\n".join([long_line] + [f"line {i}" for i in range(2, 201)]) + "\n"
    open_file(app, "notes/long.txt", body)
    app.key("alt+z")
    step(app, "alt+z", 0.25)
    app.expect(app.buffer()["softWrap"], "alt+z turns soft wrap on")
    rows = [row["line"] for row in app.pane()["rows"]]
    app.expect(rows.count(1) > 1, f"the long line wraps onto several rows ({rows[:8]})")
    app.click_text(1, 1)
    app.key("End")
    step(app, "End on a wrapped line")
    app.expect(app.cursors()[0][0] == 1, "End stays on the logical line")
    app.key("alt+z")
    step(app, "alt+z off", 0.25)
    app.expect(not app.buffer()["softWrap"], "alt+z turns it off")
    app.key("ctrl+Home")
    surface = app.ui()["surfaces"]["editorSurface"]
    x, y = surface[0] + surface[2] / 2, surface[1] + surface[3] / 2
    app.wheel(x, y, 5)
    step(app, "wheel down", 0.2)
    scroll = app.buffer()["scrollLine"]
    app.expect(scroll > 1, f"the wheel scrolls the editor (scrollLine {scroll})")
    app.wheel(x, y, -10)
    step(app, "wheel up", 0.2)
    app.expect(app.buffer()["scrollLine"] == 1, "wheeling back up returns to the top")
    for chord in ("ctrl+End", "Prior", "Prior", "Next", "ctrl+Home"):
        app.key(chord)
        step(app, chord)
        buffer = app.buffer()
        line = buffer["cursors"][0]["line"]
        top = buffer["scrollLine"]
        app.expect(top <= line < top + buffer["visibleCount"],
                   f"after {chord} the caret (line {line}) is on screen ({top}..{top + buffer['visibleCount'] - 1})")
    app.key("ctrl+End")
    app.expect(app.cursors()[0][0] == 201, f"ctrl+End reaches the last line ({app.cursors()})")


def flow_multi_caret_lines(app: q.App) -> None:
    """Line operations over more than one caret, as VS Code does them: Insert
    Line Above at two carets (each new line taking its own line's indent), Delete
    Line over a selection that spans lines (every line it touches), Tab with one
    cursor selecting a whole line and another a point (the line shifts, the point
    gets an indent at its column) and Toggle Block Comment over two selections
    (each wrapped, then unwrapped).

    `edit-multi-cursor` types and deletes at several carets; the line operations
    over them ran at a fraction in the coverage build (TD-364: ShapingActions at
    60 %)."""
    source = "int a = 1;\n    int b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
    open_file(app, "notes/shape.cpp", source)
    unit = indent_unit(app)

    # Insert Line Above at two carets.
    app.click_text(1, 1)
    app.click_text(2, 7, modifiers=("alt",))
    app.wait_until(lambda: len(app.cursors()) == 2, 2.0, "Alt+click to add a second caret")
    app.key("ctrl+shift+Return")
    step(app, "Insert Line Above at two carets", 0.2)
    expect_text(app, "\nint a = 1;\n    \n    int b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n",
                "Insert Line Above opens a line above each caret, with that line's indent")
    app.expect(sorted(app.cursors()) == [(1, 1), (3, 5)],
               f"and puts each caret on its new line ({app.cursors()})")
    app.key("Escape")
    app.key("ctrl+z")
    expect_text(app, source, "one undo takes both new lines back")

    # Delete Line over a selection spanning lines 2-4.
    app.click_text(2, 7)
    app.click_text(4, 3, modifiers=("shift",))
    app.key("ctrl+shift+k")
    step(app, "Delete Line over three lines", 0.2)
    expect_text(app, "int a = 1;\nint e = 5;\n", "Delete Line removes every line the selection touches")
    app.key("ctrl+z")
    expect_text(app, source, "and one undo puts them back")

    # Tab with one cursor on a whole line and one at a point.
    app.click_text(3, 1)
    app.key("shift+End")
    app.click_text(5, 5, modifiers=("alt",))
    app.wait_until(lambda: len(app.cursors()) == 2, 2.0, "Alt+click to add the point caret")
    app.key("Tab")
    step(app, "Tab with a line and a point", 0.2)
    expect_text(app, f"int a = 1;\n    int b = 2;\n{unit}int c = 3;\nint d = 4;\nint {unit}e = 5;\n",
                "Tab shifts the selected line and indents at the point")
    app.key("Escape")
    app.key("ctrl+z")
    expect_text(app, source, "one undo takes both back")

    # Toggle Block Comment over two selections.
    app.click_text(3, 5)
    app.key("ctrl+d")  # selects the word `c`
    app.click_text(4, 5, modifiers=("alt",))
    app.key("ctrl+d")
    app.wait_until(lambda: len(app.cursors()) == 2, 2.0, "two selections")
    app.key("shift+alt+a")
    step(app, "Toggle Block Comment over two selections", 0.2)
    expect_text(app, "int a = 1;\n    int b = 2;\nint /* c */ = 3;\nint /* d */ = 4;\nint e = 5;\n",
                "each selection is wrapped in its own block comment")
    app.key("shift+alt+a")
    expect_text(app, source, "the second toggle unwraps them")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "edit-type-save": flow_type_and_save,
    "edit-multi-caret-lines": flow_multi_caret_lines,
    "edit-selection-clipboard": flow_selection_and_clipboard,
    "edit-multi-cursor": flow_multi_cursor,
    "edit-line-ops": flow_line_operations,
    "edit-auto-close": flow_auto_close_and_indent,
    "edit-folding": flow_folding,
    "edit-find-replace": flow_find_replace,
    "edit-goto-line": flow_goto_line,
    "edit-unsaved-prompt": flow_unsaved_prompt,
    "edit-external-change": flow_external_change,
    "edit-wrap-scroll": flow_wrap_and_scroll,
}
