"""Interactions no command names: the inline blame card, selecting with the mouse
past the editor's edge, a background terminal's activity markers, and the edges
of find and Go to Line -- each checked against what the app reports."""

from __future__ import annotations

import time
from typing import Callable

import microide_qa as q
from flows_common import git, open_file, step, terminal_tabs, write


def _card(app: q.App) -> dict:
    return app.ui()["popups"]["hover"] or {}


def flow_blame_inline(app: q.App) -> None:
    """The caret's line shows who last changed it; hovering that shows the card
    (author, date, commit summary) whose Copy SHA copies the full commit id. An
    edited (dirty) line shows no blame -- it would describe text that is not
    there -- and the setting turns it off."""
    write(app, "blame/lines.txt", "".join(f"line {i}\n" for i in range(1, 40)))
    git(app, "add", "-A")
    git(app, "-c", "user.name=Blame Person", "-c", "user.email=blame@example.com",
        "commit", "-q", "-m", "qa: blame me")
    sha = git(app, "rev-parse", "HEAD")
    open_file(app, "blame/lines.txt")
    app.click_text(3, 2)
    step(app, "caret on line 3", 1.2)

    def card_up() -> bool:
        app.move(*app.text_point(3, 18))
        return _card(app).get("kind") == "blame"
    if not app.wait_until(card_up, 6.0, "the blame card on line 3"):
        return
    card = _card(app)
    lines = card.get("text", "").split("\n")
    app.expect(lines[:1] == ["Blame Person"] and lines[-1:] == ["qa: blame me"],
               f"the card names the author and the commit ({lines})")
    action = card.get("action") or {}
    app.expect(action.get("label") == "Copy SHA", f"the card has Copy SHA ({action})")
    if action:
        x, y, w, h = action["rect"]
        app.click(x + w / 2, y + h / 2)
        step(app, "copy sha", 0.3)
        app.key("ctrl+n")
        app.key("ctrl+v")
        step(app, "paste the sha", 0.3)
        app.expect(app.text() == sha, f"Copy SHA copies the full commit id ({app.text()!r} vs {sha})")
        app.key("ctrl+z")
        app.key("ctrl+w")
        step(app, "close the scratch buffer", 0.3)
        open_file(app, "blame/lines.txt")

    # A dirty line: no blame for text that is not committed (or saved).
    app.click_text(5, 2)
    app.type("edited ")
    step(app, "edit line 5", 1.0)
    app.move(*app.text_point(5, 26))
    time.sleep(1.2)
    app.expect(_card(app).get("kind") != "blame", f"an edited buffer shows no blame card ({_card(app)})")
    app.key("ctrl+z")
    step(app, "undo", 0.5)

    app.send("set-setting", "editor.blame.inline.enabled", "false")
    app.click_text(3, 2)
    step(app, "blame off", 1.0)
    app.move(*app.text_point(3, 18))
    time.sleep(1.2)
    app.expect(_card(app).get("kind") != "blame", f"with the setting off there is no blame ({_card(app)})")


def flow_selection_mouse(app: q.App) -> None:
    """A drag held past the bottom of the editor scrolls and keeps extending the
    selection (VS Code's autoscroll) to the end of the file; Escape collapses the
    selection to its caret; Shift+click extends from the caret; a drag that leaves
    the window and is released outside does not keep selecting afterwards."""
    write(app, "sel/long.txt", "".join(f"line {i}\n" for i in range(1, 201)))
    open_file(app, "sel/long.txt")
    pane = app.pane()
    x0, y0 = app.text_point(5, 1)
    _, ry, _, rh = pane["rect"]
    # Held below the pane, the caret follows the last row on screen as the view
    # scrolls to the end of the file.
    app.drag(x0, y0, x0 + 20, ry + rh + 30, steps=6,
             before_release=lambda: app.wait_until(lambda: app.buffer()["cursors"][0]["line"] >= 195, 8.0,
                                                   "the held drag to scroll the selection to the end"))
    step(app, "release", 0.3)
    caret = app.buffer()["cursors"][0]
    app.expect(caret["anchorLine"] == 5 and caret["line"] >= 195 and app.buffer()["scrollLine"] > 100,
               f"the selection runs from line 5 to the end, scrolled there ({caret})")
    line_after = caret["line"]
    app.move(x0, y0)
    time.sleep(0.5)
    app.expect(app.buffer()["cursors"][0]["line"] == line_after,
               "after the release, moving the pointer selects nothing more")
    app.key("Escape")
    step(app, "escape", 0.2)
    caret = app.buffer()["cursors"]
    app.expect(len(caret) == 1 and caret[0]["empty"] and caret[0]["line"] == line_after,
               f"Escape collapses the selection to the caret ({caret})")
    app.key("ctrl+Home")
    app.click_text(2, 3)
    app.click_text(4, 2, modifiers=("shift",))
    step(app, "shift+click", 0.2)
    caret = app.buffer()["cursors"][0]
    app.expect((caret["anchorLine"], caret["anchorColumn"], caret["line"], caret["column"]) == (2, 3, 4, 2),
               f"Shift+click extends the selection from the caret ({caret})")


def flow_terminal_activity(app: q.App) -> None:
    """A terminal tab that is not shown marks what happened in it: output since it
    was last seen, and a bell rung by its program; showing it clears both."""
    app.send("term")
    app.wait_until(lambda: bool(terminal_tabs(app)), 5.0, "a terminal")
    app.key("ctrl+shift+grave")
    app.wait_until(lambda: len(terminal_tabs(app)) >= 2, 5.0, "a second terminal")
    app.send("focus", "panel")
    busy = next((t["tab"] for t in terminal_tabs(app) if t["active"]), None)
    app.type("sleep 1; echo background-done; printf '\\a'\n")
    step(app, "start the background job", 0.3)
    app.send("term-prev")
    step(app, "show the other terminal", 0.3)
    marked = lambda: next((t for t in terminal_tabs(app) if t["tab"] == busy), {})  # noqa: E731
    app.expect(not marked().get("active"), "the busy terminal is no longer shown")
    app.wait_until(lambda: marked().get("unseenOutput") and marked().get("unseenBell"), 6.0,
                   "the hidden terminal to mark its output and its bell")
    app.send("term-next")
    step(app, "show it", 0.4)
    app.wait_until(lambda: marked().get("active") and not marked().get("unseenOutput")
                   and not marked().get("unseenBell"), 3.0, "showing it to clear both marks")
    app.expect("background-done" in app.query("terminal-output").get("text", ""),
               "and its output is there")


def flow_find_goto_edges(app: q.App) -> None:
    """The find widget with no match counts nothing and leaves the caret; with
    matches, Enter past the last wraps to the first (VS Code); Ctrl+F with a
    selection seeds the query from it; Escape gives the editor the keyboard with
    the match selected. Go to Line past the end lands on the last line and a
    column past the line's end on its end; line 0, a negative line or a word are
    refused with a reason."""
    open_file(app, "edge/words.txt", "alpha one\nbeta\nalpha two\ngamma\nalpha three\n")
    app.send("goto", "2:1")
    app.key("ctrl+f")
    step(app, "find", 0.2)
    app.type("nothing-here")
    step(app, "no match", 0.3)
    app.expect(app.ui()["overlay"].get("count") == 0, f"no match counts zero ({app.ui()['overlay']})")
    before = app.cursors()
    app.key("Return")
    step(app, "enter with no match", 0.2)
    app.expect(app.cursors() == before, f"Enter with no match moves nothing ({before} -> {app.cursors()})")
    app.key("ctrl+a")
    app.type("alpha")
    step(app, "query alpha", 0.3)
    app.expect(app.ui()["overlay"].get("count") == 3, f"three matches ({app.ui()['overlay'].get('count')})")
    # Typing already selected the first match after the caret; Enter steps on from
    # it, one match per press, and past the last wraps to the first.
    lines = [app.cursors()[0][0]]
    for _ in range(3):
        app.key("Return")
        step(app, "next match", 0.15)
        lines.append(app.cursors()[0][0])
    order = [1, 3, 5]
    app.expect(all(order[(order.index(a) + 1) % 3] == b for a, b in zip(lines, lines[1:]) if a in order),
               f"Enter steps through the matches in order and wraps past the last ({lines})")
    app.key("Escape")
    step(app, "escape", 0.2)
    app.expect(app.ui()["focus"] == "editor" and app.buffer().get("selectedText") == "alpha",
               f"Escape gives the editor the keyboard, the match selected ({app.ui()['focus']}, "
               f"{app.buffer().get('selectedText')!r})")
    app.send("goto", "4:1")
    app.key("shift+End")
    app.key("ctrl+f")
    step(app, "ctrl+f with a selection", 0.3)
    app.expect(app.ui()["overlay"].get("query") == "gamma",
               f"Ctrl+F seeds the query from the selection ({app.ui()['overlay'].get('query')!r})")
    app.key("Escape")
    for target, want in (("999", 6), ("2:99", (2, 5))):
        app.send("goto", target)
        step(app, f"goto {target}", 0.2)
        got = app.cursors()[0] if isinstance(want, tuple) else app.cursors()[0][0]
        app.expect(got == want, f"Go to {target} lands on {want} ({app.cursors()})")
    before = app.cursors()
    for bad in ("0", "-3", "abc"):
        reply = app.send("goto", bad)
        app.expect(not reply.get("ok") and reply.get("error") and app.cursors() == before,
                   f"Go to {bad} is refused with a reason and moves nothing ({reply})")


def flow_blame_card_follows_its_line(app: q.App) -> None:
    """The blame card goes when its line stops being the blamed one, with the
    pointer still: focus moving to the other pane of a split (inline blame is the
    active pane's), and the caret leaving the line by a key. Each time the card is
    gone from the ui and the frame that removed it is checked by the redraw
    verifier (the card's pixels once stayed: QA monkey, editor profile)."""
    write(app, "blame2/lines.txt", "".join(f"line {i}\n" for i in range(1, 40)))
    git(app, "add", "-A")
    git(app, "commit", "-q", "-m", "qa: blame two")
    open_file(app, "blame2/lines.txt")
    app.send("split-down")
    step(app, "split down", 0.4)
    app.send("focus-group", "1")
    step(app, "top pane", 0.3)
    app.click_text(3, 2)
    step(app, "caret on line 3", 1.2)

    def card_up() -> bool:
        app.move(*app.text_point(3, 18))
        return _card(app).get("kind") == "blame"
    if not app.wait_until(card_up, 6.0, "the blame card on line 3"):
        return
    app.send("focus-other-group")
    step(app, "focus the other pane", 0.6)
    app.expect(_card(app).get("kind") != "blame", f"the card goes with its pane's focus ({_card(app)})")
    app.send("focus-group", "1")
    step(app, "back to the top pane", 0.3)
    app.click_text(3, 2)
    step(app, "caret on line 3 again", 1.2)
    if not app.wait_until(card_up, 6.0, "the blame card again"):
        return
    app.key("Down")
    step(app, "the caret leaves the line", 0.6)
    app.expect(_card(app).get("kind") != "blame", f"the card goes when the caret leaves its line ({_card(app)})")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "blame-inline": flow_blame_inline,
    "blame-card-follows-its-line": flow_blame_card_follows_its_line,
    "selection-mouse": flow_selection_mouse,
    "terminal-activity": flow_terminal_activity,
    "find-goto-edges": flow_find_goto_edges,
}
