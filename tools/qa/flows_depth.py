"""Depth flows: the paths a coverage build of the suite never executed
(TD-2026-10-18-364) -- the terminal running real full-screen and coloured
programs, the unsaved-changes prompt in front of a project close, a quit and a
rename, and branch review.
"""

from __future__ import annotations

import time
from typing import Callable

import microide_qa as q
from flows_common import open_file, read, step, write


def _terminal_tabs(app: q.App) -> list[dict]:
    terminals = app.query("terminals")
    return terminals if isinstance(terminals, list) else terminals.get("tabs", [])


def _active_pane(app: q.App) -> dict:
    panes = next((t.get("panes", []) for t in _terminal_tabs(app) if t.get("active")), [])
    return next((p for p in panes if p.get("active")), {})


def _screen(app: q.App, lines: int = 200) -> str:
    return app.query("terminal-output", {"lines": lines}).get("text", "")


def _focus_terminal(app: q.App) -> None:
    if not app.ui()["panel"]["visible"] or not _terminal_tabs(app):
        app.key("ctrl+grave")
        app.wait_until(lambda: app.ui()["panel"]["visible"] and bool(_terminal_tabs(app)), 5.0,
                       "a terminal")
    app.send("focus", "panel")
    step(app, "focus the terminal", 0.4)


def _shell(app: q.App, command: str, until: str, label: str, timeout: float = 5.0) -> bool:
    """Types `command` at the prompt and waits for `until` on screen."""
    app.type(command + "\n")
    ok = app.wait_until(lambda: until in _screen(app), timeout, label)
    step(app, label, 0.3)
    return ok


def flow_terminal_programs(app: q.App) -> None:
    """Real programs in the terminal: SGR colours and an OSC title, `less` on the
    alternate screen (page, quit back to the shell), `vim` editing and writing a
    file, and a program that turns on mouse reporting receiving a click."""
    write(app, "notes/long.txt", "".join(f"long line {i}\n" for i in range(1, 301)))
    _focus_terminal(app)
    _shell(app, "cd notes && clear", "$", "cd into notes")

    _shell(app, r"printf '\033[31mqa-red\033[0m \033[1;4mqa-bold\033[0m \033[38;5;208mqa-256\033[0m"
                r" \033[38;2;10;200;30mqa-rgb\033[0m\n'",
           "qa-rgb", "SGR colours (16, bold+underline, 256, truecolour)")
    app.expect("qa-red qa-bold qa-256 qa-rgb" in _screen(app),
               "coloured text reaches the screen without its escapes")

    _shell(app, r"printf '\033]0;qa-title\007'; echo title-set", "title-set", "an OSC 0 title")
    app.wait_until(lambda: "qa-title" in str(_active_pane(app).get("label", "")), 3.0,
                   f"the OSC title to name the pane ({_active_pane(app)})")

    # less: the alternate screen, paging, and the shell's screen back on quit.
    app.type("less long.txt\n")
    app.wait_until(lambda: "long line 1" in _screen(app, 60), 5.0, "less to show the file")
    step(app, "less", 0.4)
    app.key("space")
    app.wait_until(lambda: "long line 1\n" not in _screen(app, 12) and "long line" in _screen(app, 12), 3.0,
                   "Space to page down in less")
    step(app, "less page", 0.3)
    app.key("G")
    app.wait_until(lambda: "long line 300" in _screen(app, 80), 3.0, "G to go to the end in less")
    step(app, "less end", 0.3)
    app.key("q")
    app.wait_until(lambda: "long line 300" not in _screen(app, 40), 3.0,
                   "q to leave less and restore the shell's screen")
    step(app, "less quit", 0.3)

    # vim: insert mode, Escape, an ex command writing the file.
    app.type("vim -u NONE -N -n vimmed.txt\n")
    app.wait_until(lambda: "vimmed.txt" in _screen(app, 60) or "~" in _screen(app, 60), 5.0, "vim to start")
    step(app, "vim", 0.5)
    app.key("i")
    step(app, "vim insert", 0.2)
    app.type("written by vim")
    app.key("Escape")
    step(app, "vim normal", 0.2)
    app.type(":wq\n")
    vimmed = app.project / "notes" / "vimmed.txt"
    app.wait_until(lambda: vimmed.exists() and vimmed.read_text() == "written by vim\n", 5.0,
                   "vim to write the file")
    step(app, "vim quit", 0.4)

    # Mouse reporting (X10 / SGR off): the click reaches the program as ESC [ M.
    # Press and release: two 6-byte reports, read together so neither reaches the prompt.
    app.type("printf '\\033[?1000h'; read -rsn12 m; printf '\\033[?1000l'; printf 'got:%q\\n' \"$m\"\n")
    step(app, "mouse reporting on", 0.6)
    geometry = app.ui().get("terminal") or {}
    if geometry:
        app.click(*app.terminal_point(geometry.get("firstRow", 0) + 2, 5))
        app.wait_until(lambda: "got:" in _screen(app, 20), 3.0, "the click to reach the program")
        app.expect("[M" in _screen(app, 20).split("got:")[-1],
                   f"a click under mouse reporting arrives as ESC [ M ({_screen(app, 5)!r})")
    else:
        app.expect(False, "the ui query has no terminal geometry")
    step(app, "mouse reporting off", 0.3)


def _tabs(app: q.App) -> list[str]:
    return [tab["title"] for group in app.editor_state().get("groups", []) for tab in group.get("tabs", [])]


def _dirty(app: q.App, relative: str, text: str) -> None:
    open_file(app, relative, text)
    app.click_text(1, 1)
    app.type("x")
    step(app, f"dirty {relative}", 0.2)


def _press(app: q.App, choice: str, label: str) -> bool:
    if not app.wait_until(lambda: app.ui().get("modal") == "unsaved-prompt", 3.0, f"the unsaved prompt ({label})"):
        return False
    # "Save" / "Discard" for one dirty tab, "Save all" / "Discard all" for several.
    buttons = [e for e in app.elements("button", prefix="button:prompt/")
               if e["name"].split("/", 1)[1].startswith(choice)]
    if not buttons:
        app.expect(False, f"{label}: no {choice} button ({[e['name'] for e in app.elements('button')]})")
        return False
    app.click(*app.center(buttons[0]))
    step(app, f"{label}: {choice}", 0.4)
    return True


def flow_unsaved_continuations(app: q.App) -> None:
    """The unsaved prompt in front of what it interrupts: renaming a dirty file
    (Cancel keeps the file and returns to the name typed), closing all tabs with two
    dirty (Save all writes both), closing the project with one dirty (Discard closes
    it and leaves the disk alone), and quitting with one dirty (a hot exit: no
    prompt, and the edit is back, unsaved, after a relaunch)."""
    _dirty(app, "notes/rename-me.txt", "orig\n")
    app.click_named("sidebar-view:tree")
    app.wait_until(lambda: any(e["name"] == "row:tree/rename-me.txt" and e.get("selected")
                               for e in app.elements("row")), 3.0, "the tree to select the dirty file")
    app.send("focus", "sidebar")
    step(app, "focus the tree", 0.2)
    app.key("F2")
    step(app, "F2 on a dirty file", 0.3)
    app.key("ctrl+a")
    app.type("renamed.txt")
    app.key("Return")
    step(app, "rename a dirty file", 0.4)
    if _press(app, "Cancel", "rename"):
        app.expect(read(app, "notes/rename-me.txt") == "orig\n", "Cancel leaves the file where it was")
        app.expect(not (app.project / "notes" / "renamed.txt").exists(), "and does not rename it")
        prompt = app.ui().get("prompt") or {}
        app.expect(prompt.get("kind") == "input" and prompt.get("input") == "renamed.txt",
                   f"Cancel returns to the rename field with the name typed ({prompt})")
        app.key("Escape")
        step(app, "leave the rename", 0.3)
        app.expect(app.ui().get("modal") is None, "Escape closes the rename field")

    _dirty(app, "notes/a.txt", "a\n")
    _dirty(app, "notes/b.txt", "b\n")
    app.click_named("menu:File")
    step(app, "File menu", 0.2)
    app.click_named("menu-item:File/Close All Tabs")
    step(app, "close all with dirty tabs", 0.4)
    _press(app, "Save", "close all")
    app.wait_until(lambda: read(app, "notes/a.txt") == "xa\n" and read(app, "notes/b.txt") == "xb\n", 3.0,
                   f"Save to write both ({read(app, 'notes/a.txt')!r}, {read(app, 'notes/b.txt')!r})")
    app.wait_until(lambda: not any(t in _tabs(app) for t in ("a.txt", "b.txt")), 3.0,
                   f"both tabs to close ({_tabs(app)})")

    _dirty(app, "notes/c.txt", "c\n")
    roots_before = len(app.query("projects") or [])
    app.send("project-close")
    step(app, "close the project with a dirty tab", 0.4)
    if _press(app, "Discard", "close project"):
        app.wait_until(lambda: len(app.query("projects") or []) < roots_before or not app.editor_state().get("groups"),
                       3.0, "Don't Save to close the project")
    app.expect(read(app, "notes/c.txt") == "c\n", f"Don't Save leaves the disk alone ({read(app, 'notes/c.txt')!r})")

    # Quit with a dirty buffer is a hot exit, as in VS Code: no prompt, the disk
    # untouched, and the edit back -- still unsaved -- after a relaunch.
    app.send("project-open", str(app.project))
    app.wait_until(lambda: bool(app.editor_state().get("groups")), 5.0, "the project to reopen")
    _dirty(app, "notes/d.txt", "d\n")
    app.restart()
    app.expect(read(app, "notes/d.txt") == "d\n", f"a hot exit leaves the disk alone ({read(app, 'notes/d.txt')!r})")
    app.wait_until(lambda: (app.buffer().get("path") or "").endswith("notes/d.txt"), 5.0, "d.txt restored")
    buffer = app.buffer()
    app.expect(buffer.get("dirty") is True and buffer.get("text") == "xd\n",
               f"the unsaved edit survives the restart ({buffer.get('dirty')}, {buffer.get('text')!r})")
    app.expect(app.ui().get("modal") is None, "the relaunched app asks nothing")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "terminal-programs": flow_terminal_programs,
    "unsaved-continuations": flow_unsaved_continuations,
}
