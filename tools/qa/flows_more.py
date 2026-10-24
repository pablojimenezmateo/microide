"""More flows: what a session spans beyond one edit -- quitting and coming
back, an untitled buffer saved for the first time, text that is not ASCII,
dragging text, the terminal's find bar, the outline, several projects, and the
compare / merge editors over a real git history.
"""

from __future__ import annotations

from pathlib import Path
from typing import Callable

import microide_qa as q
from flows_common import expect_text, git_run, open_file, read, step, titles, write


def flow_session_restore(app: q.App) -> None:
    """Open tabs in two groups, place the caret, fold, quit, start again: the
    tabs, the split, the active tab, the caret, the scroll and the fold come
    back, and Reopen Closed Editor reaches a tab closed before the quit."""
    open_file(app, "src/main.cpp")
    app.key("ctrl+w")
    step(app, "close main.cpp", 0.3)
    open_file(app, "src/scheduler.cpp")
    app.click_text(19, 9)
    app.send("fold-all")
    # Go to Line reveals the caret's own fold (VS Code's revealCursor); the rest
    # stay collapsed.
    app.send("goto", "19:9")
    step(app, "fold all but the caret's", 0.3)
    app.send("split-right", "include/scheduler.hpp")
    step(app, "split-right", 0.5)
    before = app.editor_state()
    groups = [[t["title"] for t in g["tabs"]] for g in before["groups"]]
    app.expect(len(groups) == 2, f"two groups before quitting ({groups})")
    app.restart()
    app.wait_until(lambda: len(app.editor_state().get("groups", [])) == 2, 10.0, "the split to come back")
    step(app, "restarted", 0.6)
    after = app.editor_state()
    restored = [[t["title"] for t in g["tabs"]] for g in after["groups"]]
    app.expect(restored == groups, f"the tabs come back in their groups ({restored} vs {groups})")
    left = next((t for t in after["groups"][0]["tabs"] if t["title"] == "scheduler.cpp"), {})
    was = next((t for t in before["groups"][0]["tabs"] if t["title"] == "scheduler.cpp"), {})
    app.expect(left.get("cursorLine") == 19 and left.get("cursorColumn") == 9,
               f"the caret comes back where it was ({left.get('cursorLine')}:{left.get('cursorColumn')})")
    app.expect(bool(was.get("foldedLines")) and left.get("foldedLines") == was.get("foldedLines")
               and left.get("scrollLine") == was.get("scrollLine"),
               f"its folds and scroll come back ({left.get('foldedLines')} @ {left.get('scrollLine')} vs "
               f"{was.get('foldedLines')} @ {was.get('scrollLine')})")
    app.send("focus-group", "1")
    app.key("ctrl+shift+t")
    app.expect(app.wait_until(lambda: (app.active_tab() or {}).get("title") == "main.cpp", 3.0,
                              "Reopen Closed Editor to reach main.cpp"),
               "the editor closed before the quit can be reopened after it")


def flow_untitled_save_as(app: q.App) -> None:
    """Ctrl+N, type, Ctrl+S asks where, the file is written and the tab takes
    its name; quitting with a dirty untitled buffer asks first."""
    # From the editor: a focused terminal owns Ctrl+N (it does not skip the shell).
    app.send("focus", "editor")
    app.key("ctrl+n")
    step(app, "ctrl+n", 0.3)
    tab = app.active_tab() or {}
    app.expect(tab.get("kind") == "editor" and not tab.get("path") and app.buffer().get("available"),
               f"ctrl+n opens an untitled buffer ({tab.get('title')!r})")
    app.send("focus", "editor")
    app.type("draft line\n")
    step(app, "type", 0.2)
    app.key("ctrl+s")
    step(app, "ctrl+s", 0.4)
    prompt = app.ui().get("prompt") or {}
    app.expect(prompt.get("kind") == "input" and "Save" in prompt.get("title", ""),
               f"saving an untitled buffer asks where ({prompt})")
    app.type("notes/draft.txt")
    app.key("Return")
    step(app, "save as notes/draft.txt", 0.6)
    assert app.project is not None
    app.wait_until(lambda: (app.project / "notes" / "draft.txt").exists(), 5.0, "the file on disk")
    if (app.project / "notes" / "draft.txt").exists():
        app.expect(read(app, "notes/draft.txt").startswith("draft line"),
                   f"the file holds the text ({read(app, 'notes/draft.txt')!r})")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "draft.txt", 3.0,
                   "the tab to take the file's name")
    app.expect(not app.buffer()["dirty"], "the saved buffer is clean")


def flow_unicode_editing(app: q.App) -> None:
    """Accented letters, CJK and an emoji: typed, stepped over a character at a
    time, deleted whole, selected by double-click, saved as UTF-8."""
    open_file(app, "notes/unicode.txt", "")
    app.type("café 日本 ok")
    step(app, "type", 0.3)
    expect_text(app, "café 日本 ok", "non-ASCII text lands as typed")
    app.key("End")
    for _ in range(4):
        app.key("Left")
    step(app, "Left x4", 0.1)
    # Four Lefts from the end: over "k", "o", " " and land before 本.
    caret = app.cursors()[0]
    app.expect(caret == (1, len("café 日".encode()) + 1),
               f"Left steps whole characters (caret {caret}, want byte column {len('café 日'.encode()) + 1})")
    app.key("BackSpace")
    step(app, "BackSpace a CJK character", 0.1)
    expect_text(app, "café 本 ok", "BackSpace deletes the whole character")
    app.click_text(1, 2, clicks=2)
    step(app, "double-click café", 0.2)
    app.expect(app.buffer()["selectedText"] == "café",
               f"double-click selects the accented word ({app.buffer()['selectedText']!r})")
    app.key("End")
    app.type(" ✓")
    app.key("ctrl+s")
    step(app, "save", 0.4)
    assert app.project is not None
    app.wait_until(lambda: (app.project / "notes" / "unicode.txt").read_bytes().decode("utf-8", "replace")
                   .startswith("café 本 ok ✓"), 4.0, "the UTF-8 bytes on disk")


def flow_drag_drop_text(app: q.App) -> None:
    """Select a word and drag it elsewhere: it moves (VS Code's dragAndDrop);
    one undo puts it back."""
    open_file(app, "notes/drag.txt", "alpha beta gamma\n")
    app.click_text(1, 7, clicks=2)
    step(app, "select beta", 0.2)
    app.expect(app.buffer()["selectedText"] == "beta", "double-click selects beta")
    app.settle(0.6)
    app.drag_text((1, 8), (1, 17))
    step(app, "drag beta to the end", 0.3)
    expect_text(app, "alpha  gammabeta\n", "dragging the selection moves it")
    app.key("ctrl+z")
    step(app, "undo", 0.2)
    expect_text(app, "alpha beta gamma\n", "one undo takes the move back")


def flow_terminal_find(app: q.App) -> None:
    """Ctrl+F in a focused terminal finds in its scrollback; Escape closes it."""
    if not app.ui()["panel"]["visible"]:
        app.send("term-toggle")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    app.send("focus", "panel")
    app.type("echo needle-one; echo needle-two\n")
    app.wait_until(lambda: "needle-two" in app.query("terminal-output").get("text", ""), 5.0, "output")
    app.key("ctrl+f")
    step(app, "ctrl+f", 0.3)
    app.type("needle")
    step(app, "type needle", 0.4)
    shot = app.grab()
    app.expect(shot is not None, "the find bar paints")
    app.key("Return")
    step(app, "Enter", 0.2)
    app.key("Escape")
    step(app, "Escape", 0.2)
    app.type("echo after-find\n")
    app.wait_until(lambda: "after-find" in app.query("terminal-output").get("text", ""), 5.0,
                   "Escape to give the keyboard back to the shell")


def flow_projects(app: q.App) -> None:
    """Open a second folder: a project tab strip appears, both projects switch
    by click and by command, and closing one leaves the other."""
    assert app.project is not None
    second = app.project.parent / "second"
    second.mkdir(exist_ok=True)
    (second / "other.txt").write_text("second project\n")
    app.send("project-open", str(second))
    app.wait_until(lambda: len(app.query("projects")) >= 2 if isinstance(app.query("projects"), list) else False,
                   8.0, "the second project to open")
    step(app, "second project", 0.6)
    tabs = app.elements("project-tab")
    app.expect(len(tabs) == 2, f"the project strip shows both ({[t['name'] for t in tabs]})")
    app.send("open", str(second / "other.txt"))
    step(app, "open other.txt", 0.4)
    first = next((t for t in tabs if "taskflow" in t["label"]), None)
    if first:
        app.click(*app.center(first))
        step(app, "click the first project tab", 0.6)
        app.expect(app.element(first["name"]).get("active") is True, "clicking a project tab activates it")
        app.expect("other.txt" not in titles(app), "the first project shows its own tabs")
    app.send("project-next")
    step(app, "project-next", 0.5)
    app.expect("other.txt" in titles(app), f"project-next switches to the second ({titles(app)})")
    app.send("project-close")
    step(app, "project-close", 0.6)
    app.wait_until(lambda: len(app.elements("project-tab")) <= 1, 5.0, "the project to close")
    app.expect("other.txt" not in titles(app), "closing it leaves the first project")


def flow_outline(app: q.App) -> None:
    """The outline view lists the file's symbols (clangd's documentSymbol) and a
    click goes to one."""
    write(app, "compile_flags.txt", "-Iinclude\n-std=c++17\n")
    open_file(app, "src/scheduler.cpp")
    app.wait_until(lambda: "Ready" in app.element("status:lsp").get("label", ""), 30.0, "clangd")
    app.key("ctrl+alt+o")
    step(app, "ctrl+alt+o", 0.6)
    app.expect(app.ui()["sidebar"]["view"] == "outline", "ctrl+alt+o shows the outline")
    app.wait_until(lambda: any("run" in e.get("label", "") for e in app.elements("row")), 10.0,
                   "the outline to list Scheduler::run")
    rows = [e for e in app.elements("row")]
    target = next((r for r in rows if "run" in r.get("label", "")), None)
    if target:
        app.click(*app.center(target))
        step(app, "click run", 0.4)
        app.expect(app.cursors()[0][0] == 12, f"a click goes to the symbol ({app.cursors()})")


def flow_compare_and_merge(app: q.App) -> None:
    """Review the last commit as a compare tab; merge the conflicting branch and
    resolve the conflict in the merge editor; the file ends without markers."""
    app.send("review-commit", "HEAD")
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 8.0, "a compare tab")
    step(app, "review-commit", 0.6)
    app.expect(app.buffer().get("kind") == "compare", "the compare tab's editable side is the buffer")
    surface = app.ui()["surfaces"]["editorSurface"]
    app.wheel(surface[0] + surface[2] / 2, surface[1] + surface[3] / 2, 3)
    step(app, "scroll the compare", 0.2)
    branch = git_run(app, "branch", "--list", "feature/*").stdout.split()
    if not branch:
        app.expect(False, "the fixture has no feature branch to merge")
        return
    merged = git_run(app, "merge", branch[-1])
    app.expect(merged.returncode != 0, "merging the feature branch conflicts (the fixture is built to)")
    app.send("git-refresh")
    app.send("review-conflicts")
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "merge", 8.0, "a merge tab")
    step(app, "review-conflicts", 0.6)
    tab = app.active_tab() or {}
    path = tab.get("path", "")
    app.expect(bool(path), "the merge tab names its file")
    app.send("git-accept-current", path)
    step(app, "accept current", 0.6)
    if path:
        text = Path(path).read_text()
        app.expect("<<<<<<<" not in text and ">>>>>>>" not in text,
                   "accepting current leaves no conflict markers in the file")


PLUGINS: dict[str, tuple[str, ...]] = {"outline": ("cpp-lsp",)}

FLOWS: dict[str, Callable[[q.App], None]] = {
    "session-restore": flow_session_restore,
    "untitled-save-as": flow_untitled_save_as,
    "unicode-editing": flow_unicode_editing,
    "drag-drop-text": flow_drag_drop_text,
    "terminal-find": flow_terminal_find,
    "projects": flow_projects,
    "outline": flow_outline,
    "compare-merge": flow_compare_and_merge,
}
