"""Depth flows: the paths a coverage build of the suite never executed
(TD-2026-10-18-364) -- the terminal running real full-screen and coloured
programs, the unsaved-changes prompt in front of a project close, a quit and a
rename, and branch review.
"""

from __future__ import annotations

import subprocess
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
    file, a program that turns on mouse reporting receiving a click, and the
    maximized terminal (Ctrl+Shift+J) -- the shell sees its size, `less` fills it,
    and the restore gives the rows back."""
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

    # The maximized ("immersive") terminal: Ctrl+Shift+J gives it the window, and
    # the program inside sees the new size; a full-screen program fills it; the
    # same chord gives the rows back.
    def stty_rows() -> int | None:
        app.type("clear; stty size\n")
        step(app, "stty size", 0.6)
        found = [l.split() for l in _screen(app, 400).splitlines() if l.strip()[:1].isdigit()]
        return int(found[-1][0]) if found and len(found[-1]) == 2 else None

    rows_before = (app.ui().get("terminal") or {}).get("visibleRows")
    app.key("ctrl+shift+j")
    app.wait_until(lambda: app.ui()["panel"].get("maximized") is True, 3.0, "Ctrl+Shift+J to maximize the terminal")
    step(app, "maximized", 0.4)
    rows_max = (app.ui().get("terminal") or {}).get("visibleRows")
    app.expect(rows_max and rows_before and rows_max > rows_before,
               f"the maximized terminal has more rows ({rows_before} -> {rows_max})")
    shell_rows = stty_rows()
    app.expect(shell_rows == rows_max, f"the shell sees the maximized size (stty {shell_rows} vs {rows_max} rows)")
    app.type("less long.txt\n")
    app.wait_until(lambda: f"long line {rows_max - 1}" in _screen(app, 80), 5.0,
                   f"less to fill the maximized terminal ({rows_max} rows)")
    step(app, "less maximized", 0.3)
    app.key("q")
    step(app, "less quit", 0.3)
    app.key("ctrl+shift+j")
    app.wait_until(lambda: app.ui()["panel"].get("maximized") is False, 3.0, "Ctrl+Shift+J to restore")
    app.expect(stty_rows() == rows_before, f"and the shell sees the restored size ({rows_before} rows)")


def flow_terminal_escapes(app: q.App) -> None:
    """Escape sequences a program emits one by one, each read back from the
    screen: cursor motion and overwrite, erase in line and display, save and
    restore cursor, insert and delete line, a scroll region, tab stops, the
    cursor hidden and shown, an OSC 8 hyperlink, and bracketed paste reaching
    `cat -v` as the markers a shell expects."""
    _focus_terminal(app)
    app.type("clear\n")
    app.settle(0.4)
    checks = [
        ("printf 'abc\\033[2Dxy\\n'", "axy", "cursor left overwrites in place"),
        ("printf 'A\\0337BC\\0338X\\n'", "AXC", "DECSC/DECRC restore the cursor"),
        ("printf 'tab\\there\\n'", "tab     here", "a tab stop every eight columns"),
        ("printf 'keep\\033[Kthis\\033[2Kgone\\033[Gline\\n'", "line", "erase in line clears what was typed"),
        ("printf 'up\\ndown\\033[A\\033[Cside\\033[B\\n'", "up   side", "cursor up and right land on the line above"),
        ("printf '1\\n2\\n3\\033[2A\\033[Linserted\\033[B\\033[B\\n'", "inserted", "insert line opens a row"),
        ("printf 'x\\033]8;;http://example.invalid\\033\\\\link\\033]8;;\\033\\\\y\\n'", "xlinky", "an OSC 8 hyperlink shows its text"),
        ("printf '\\033[?25lhidden\\033[?25h\\n'", "hidden", "the cursor hidden and shown"),
    ]
    for command, until, label in checks:
        app.expect(_shell(app, command, until, label), f"{label}: {until!r} on screen")
    _shell(app, "printf 'top\\n\\033[2;3r\\033[2;1Hr1\\nr2\\nr3\\nr4\\033[r\\033[6;1H'", "r4", "a scroll region keeps the top line")
    app.expect("top" in _screen(app), "the line above the scroll region stays")
    _shell(app, "printf '\\033[2J\\033[Hcleared\\n'", "cleared", "erase display")
    app.expect("r4" not in _screen(app), "erase display cleared the earlier lines")
    # Bracketed paste: with ?2004h on, a paste arrives wrapped in ESC[200~ ... ESC[201~.
    if app.wayland:
        return  # the clipboard needs a seat input serial (TD-2026-10-19-366)
    app.send("focus", "editor")
    open_file(app, "notes/paste-src.txt", "pasted-text")
    app.click_text(1, 1)
    app.key("shift+End")
    app.key("ctrl+c")
    _focus_terminal(app)
    app.type("printf '\\033[?2004h'; cat -v\n")
    app.settle(0.4)
    app.key("ctrl+shift+v")
    app.key("Return")
    app.wait_until(lambda: "^[[200~pasted-text^[[201~" in _screen(app), 5.0, "the paste wrapped in bracketed-paste markers")
    step(app, "bracketed paste", 0.3)
    app.expect("^[[200~pasted-text^[[201~" in _screen(app), f"cat -v shows the paste markers ({_screen(app).splitlines()[-3:]})")
    app.key("ctrl+d")
    app.settle(0.3)
    app.type("printf '\\033[?2004l'\n")
    app.settle(0.3)


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


def _git(app: q.App, *args: str) -> str:
    assert app.project is not None
    return subprocess.run(["git", "-C", str(app.project), *args], capture_output=True, text=True).stdout


def _git_labels(app: q.App) -> dict[str, str]:
    return {e["name"]: e.get("label", "") for e in app.elements("row", prefix="row:git/")}


def flow_branch_review(app: q.App) -> None:
    """Branch review from the git view: an Outgoing file's diff marked reviewed
    shows the marker on its row (the ui label is the painted one), survives a
    restart, and clears; the marks are refused, not silently "ok", outside such a
    diff."""
    _git(app, "checkout", "-q", "-b", "qa-review")
    write(app, "notes/r1.txt", "one\n")
    write(app, "notes/r2.txt", "two\n")
    _git(app, "add", "-A")
    _git(app, "commit", "-q", "-m", "qa: review fixture")
    reply = app.send("mark-branch-file-reviewed")
    app.expect(not reply.get("ok"), f"marking with no branch-review diff open is refused ({reply})")
    app.key("ctrl+shift+g")
    if app.wait_for("row:git/r1.txt", 5.0) is None:
        app.expect(False, f"the git view lists the outgoing files ({_git_labels(app)})")
        return
    app.click_named("row:git/r1.txt")
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 5.0, "r1.txt's diff")
    step(app, "outgoing diff", 0.3)
    app.expect(app.send("mark-branch-file-reviewed").get("ok") is True, "marking the outgoing diff runs")
    app.wait_until(lambda: _git_labels(app).get("row:git/r1.txt", "").startswith("["), 3.0,
                   f"the reviewed marker on r1.txt's row ({_git_labels(app)})")
    app.expect(not _git_labels(app).get("row:git/r2.txt", "").startswith("["), "r2.txt stays unmarked")
    app.restart()
    app.key("ctrl+shift+g")
    app.wait_until(lambda: _git_labels(app).get("row:git/r1.txt", "").startswith("["), 5.0,
                   f"the mark survives a restart ({_git_labels(app)})")
    app.click_named("row:git/r1.txt")
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 5.0, "r1.txt's diff again")
    app.send("clear-branch-review-state")
    app.wait_until(lambda: not _git_labels(app).get("row:git/r1.txt", "[").startswith("["), 3.0,
                   f"clearing removes the marker ({_git_labels(app)})")


def flow_selected_ranges(app: q.App) -> None:
    """Partial staging from a working-tree diff, by VS Code's chords: Ctrl+K
    Ctrl+Alt+S stages the change at the caret and nothing else; Ctrl+K Ctrl+R on
    the other change asks first and, confirmed, reverts it in the working tree."""
    head = "".join(f"line {i}\n" for i in range(1, 41))
    write(app, "notes/ranges.txt", head)
    _git(app, "add", "notes/ranges.txt")
    _git(app, "commit", "-q", "-m", "qa: ranges fixture")
    edited = head.replace("line 3\n", "line three\n").replace("line 35\n", "line thirty-five\n")
    write(app, "notes/ranges.txt", edited)
    app.key("ctrl+shift+g")
    row = app.wait_for("row:git/ranges.txt", 5.0)
    if row is None:
        app.expect(False, "the git view lists ranges.txt")
        return
    app.click(*app.center(row))
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 5.0, "the diff")
    app.send("focus", "editor")
    step(app, "diff", 0.4)
    app.key("ctrl+k")
    app.key("ctrl+alt+s")
    app.wait_until(lambda: "line three" in _git(app, "show", ":notes/ranges.txt"), 5.0,
                   "Ctrl+K Ctrl+Alt+S to stage the first change")
    staged = _git(app, "show", ":notes/ranges.txt")
    app.expect("line thirty-five" not in staged, "and only the first change")
    app.key("alt+bracketright")
    step(app, "next change", 0.3)
    app.key("ctrl+k")
    app.key("ctrl+r")
    step(app, "revert asks", 0.4)
    prompt = app.ui().get("prompt") or {}
    app.expect(bool(prompt), f"Ctrl+K Ctrl+R asks before reverting ({prompt})")
    confirm = [e for e in app.elements("button", prefix="button:prompt/")
               if not e["name"].endswith("/Cancel")]
    if confirm:
        app.click(*app.center(confirm[0]))
        if not app.wait_until(lambda: "line thirty-five" not in read(app, "notes/ranges.txt"), 8.0,
                              "the revert in the working tree"):
            # What the app said about it, for a run that cannot be reproduced here
            # (CI's runner reverted nothing where every local run reverts).
            app.expect(False, "revert diagnostics: notifications="
                              f"{[n.get('message') for n in app.query('notifications')]} "
                              f"git={_git(app, '--version')!r} "
                              f"log={[l for l in app.new_log_lines() if 'apply' in l or 'revert' in l or 'error' in l.lower()][-5:]}")
        app.expect("line three" in read(app, "notes/ranges.txt"), "the staged change stays in the file")


def flow_merge_mouse(app: q.App) -> None:
    """The merge editor by mouse: two conflicts, the first resolved with its
    incoming side's accept button, the second with the result pane's Both, then
    the toolbar's Save -- the file holds those choices and no markers."""
    base = "".join(f"line {i}\n" for i in range(1, 13))
    write(app, "notes/merge.txt", base)
    _git(app, "add", "notes/merge.txt")
    _git(app, "commit", "-q", "-m", "qa: merge base")
    _git(app, "checkout", "-q", "-b", "qa-incoming")
    write(app, "notes/merge.txt", base.replace("line 2\n", "line 2 incoming\n").replace("line 10\n", "line 10 incoming\n"))
    _git(app, "commit", "-q", "-am", "qa: incoming side")
    _git(app, "checkout", "-q", "-")
    write(app, "notes/merge.txt", base.replace("line 2\n", "line 2 current\n").replace("line 10\n", "line 10 current\n"))
    _git(app, "commit", "-q", "-am", "qa: current side")
    assert app.project is not None
    merged = subprocess.run(["git", "-C", str(app.project), "merge", "qa-incoming"], capture_output=True, text=True)
    app.expect(merged.returncode != 0, "the merge conflicts (the fixture is built to)")
    app.send("git-refresh")
    app.send("review-conflicts")
    if not app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "merge", 8.0, "a merge tab"):
        return
    step(app, "merge tab", 0.6)

    def press(name: str) -> bool:
        target = app.wait_for(name, 3.0)
        if target is None:
            app.expect(False, f"no {name} on screen ({[e['name'] for e in app.elements('button', prefix='button:merge/')]})")
            return False
        app.hover_named(name)
        step(app, f"hover {name}", 0.2)
        app.click_named(name)
        step(app, f"click {name}", 0.3)
        return True

    press("button:merge/0/accept-incoming")
    press("button:merge/1/result-both")
    press("button:merge/save")
    path = app.project / "notes" / "merge.txt"
    app.wait_until(lambda: "<<<<<<<" not in path.read_text(), 5.0, f"Save to write the resolved file ({path.read_text()!r})")
    text = path.read_text()
    app.expect("line 2 incoming\n" in text and "line 2 current\n" not in text,
               f"the first conflict took the incoming side ({text!r})")
    app.expect("line 10 incoming\n" in text and "line 10 current\n" in text,
               f"the second took both ({text!r})")


def flow_save_pipeline(app: q.App) -> None:
    """Save-time hooks from a plugin (the QA-only `qa-save`): Ctrl+S runs its save
    participant (qa-todo -> qa-done) and, with format on save, its formatter (an
    upper-casing subprocess) -- in that order, onto the disk; without format on
    save only the participant runs."""
    open_file(app, "notes/pipe.qasave", "keep qa-todo here\n")
    app.wait_until(lambda: app.buffer().get("language") == "qasave", 3.0,
                   f"the plugin's syntax to claim .qasave ({app.buffer().get('language')})")
    app.send("set-setting", "editor.format_on_save", "false")
    app.click_text(1, 1)
    app.type("x")
    app.key("ctrl+s")
    app.wait_until(lambda: read(app, "notes/pipe.qasave") == "xkeep qa-done here\n", 5.0,
                   f"the save participant alone ({read(app, 'notes/pipe.qasave')!r})")
    app.send("set-setting", "editor.format_on_save", "true")
    app.click_text(1, 1)
    app.type("y")
    app.key("ctrl+s")
    app.wait_until(lambda: read(app, "notes/pipe.qasave") == "YXKEEP QA-DONE HERE\n", 5.0,
                   f"participant then formatter ({read(app, 'notes/pipe.qasave')!r})")
    app.wait_until(lambda: app.buffer().get("text") == "YXKEEP QA-DONE HERE\n" and not app.buffer().get("dirty"), 3.0,
                   f"the buffer to show what was saved, clean ({app.buffer().get('text')!r})")


PLUGINS: dict[str, tuple[str, ...]] = {"save-pipeline": ("qa-save",)}


def flow_compare_mouse(app: q.App) -> None:
    """A diff by mouse: the divider drags and a double-click restores it; a
    collapsed-context block's buttons reveal the lines they hide; on the working
    side a click places the caret, a double-click takes a word, a triple-click
    the line, and a drag a range."""
    lines = [f"line {i}" for i in range(1, 121)]
    write(app, "notes/cmp.txt", "".join(f"{x}\n" for x in lines))
    _git(app, "add", "notes/cmp.txt")
    _git(app, "commit", "-q", "-m", "qa: compare fixture")
    lines[4] = "line five"
    lines[59] = "line sixty"
    lines[114] = "line one-fifteen"
    write(app, "notes/cmp.txt", "".join(f"{x}\n" for x in lines))
    app.key("ctrl+shift+g")
    step(app, "git view", 0.5)
    row = app.wait_for("row:git/cmp.txt", 10.0)
    if row is None:
        app.expect(False, "the changed file is listed in the git view")
        return
    app.click(*app.center(row))
    if not app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 5.0, "the diff to open"):
        return
    step(app, "compare tab", 0.5)

    # The divider: a drag moves it, a double-click puts it back.
    divider = app.wait_for("divider:compare", 3.0)
    if divider is None:
        app.expect(False, "the compare divider is on screen")
        return
    x0 = divider["rect"][0]
    cx, cy = app.center(divider)
    app.drag(cx, cy, cx - 160, cy)
    step(app, "drag the divider", 0.3)
    moved = (app.element("divider:compare") or {}).get("rect", [x0])[0]
    app.expect(moved < x0 - 100, f"the divider follows the drag ({x0} -> {moved})")
    cx, cy = app.center(app.element("divider:compare") or divider)
    app.click(cx, cy, clicks=2)
    step(app, "double-click the divider", 0.3)
    back = (app.element("divider:compare") or {}).get("rect", [0])[0]
    app.expect(abs(back - x0) < 4, f"a double-click restores the even split ({x0} -> {back})")

    # Collapsed context: "all" reveals a whole block.
    blocks = app.elements("button", prefix="button:compare/context-")
    alls = [e for e in blocks if e["name"].endswith("/all")]
    app.expect(len(alls) >= 1, f"the unchanged stretches collapse into blocks ({[e['name'] for e in blocks]})")
    if alls:
        app.hover_named(alls[0]["name"])
        step(app, "hover a context button", 0.2)
        app.click_named(alls[0]["name"])
        step(app, "show all hidden lines", 0.4)
        after = [e for e in app.elements("button", prefix="button:compare/context-") if e["name"].endswith("/all")]
        first_y = alls[0]["rect"][1]
        app.expect(not any(abs(e["rect"][1] - first_y) < 2 for e in after),
                   f"the block is gone once its lines show ({[(e['name'], e['rect']) for e in after]})")
    # "Show next 20" / "Show previous 20" reveal part of a block: its count drops
    # by 20 (the block stays put on screen, the view scrolls with it).
    for which in ("next", "previous"):
        block = next((e for e in app.elements("button", prefix="button:compare/context-")
                      if e["name"].endswith("/" + which)), None)
        if block is None:
            continue
        all_name = block["name"].rsplit("/", 1)[0] + "/all"
        hidden = int((app.element(all_name)).get("label") or 0)
        app.click_named(block["name"])
        step(app, f"show {which} 20", 0.4)
        now = next((e for e in app.elements("button", prefix="button:compare/context-")
                    if e["name"] == all_name), None)
        left = int(now.get("label") or 0) if now else 0
        app.expect(left == max(hidden - 20, 0), f"Show {which} 20 reveals 20 of {hidden} hidden lines ({left} left)")

    # The working side: click, double-click, triple-click, drag.
    surface = app.ui()["surfaces"]["editorSurface"]
    divider = app.element("divider:compare") or divider
    rx = divider["rect"][0] + divider["rect"][2] + 70
    ry = surface[1] + surface[3] * 0.45
    app.click(rx, ry)
    step(app, "click the working side", 0.3)
    caret = app.cursors()[0] if app.cursors() else (0, 0)
    app.expect(caret[0] > 0, f"a click on the working side places the caret ({app.cursors()})")
    expected = lines[caret[0] - 1] if 0 < caret[0] <= len(lines) else ""
    app.click(rx, ry, clicks=3)
    step(app, "triple-click", 0.3)
    selected = (app.buffer().get("selectedText") or "").rstrip("\n")
    app.expect(selected == expected, f"a triple-click selects the line ({selected!r}, line {caret[0]} is {expected!r})")
    app.click(rx, ry)
    app.click(rx, ry, clicks=2)
    step(app, "double-click", 0.3)
    word = app.buffer().get("selectedText") or ""
    app.expect(bool(word) and word in expected and " " not in word,
               f"a double-click selects a word of the line ({word!r} in {expected!r})")
    app.click(divider["rect"][0] + divider["rect"][2] + 50, ry)
    app.drag(divider["rect"][0] + divider["rect"][2] + 50, ry, rx + 40, ry)
    step(app, "drag-select", 0.3)
    dragged = app.buffer().get("selectedText") or ""
    app.expect(bool(dragged) and "\n" not in dragged and dragged in expected,
               f"a drag selects part of the line ({dragged!r} in {expected!r})")
    app.key("ctrl+a")
    step(app, "select all", 0.2)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "terminal-programs": flow_terminal_programs,
    "terminal-escapes": flow_terminal_escapes,
    "unsaved-continuations": flow_unsaved_continuations,
    "branch-review": flow_branch_review,
    "selected-ranges": flow_selected_ranges,
    "merge-mouse": flow_merge_mouse,
    "compare-mouse": flow_compare_mouse,
    "save-pipeline": flow_save_pipeline,
}
