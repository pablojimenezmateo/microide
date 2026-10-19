"""Resilience flows: what happens when things go wrong around the editor -- the app
killed mid-session, a project much bigger than the fixture.
"""

from __future__ import annotations

import subprocess
import time
from pathlib import Path
from typing import Callable

import microide_qa as q
from flows_common import open_file, read, step


def flow_crash_recovery(app: q.App) -> None:
    """Killed with SIGKILL a few seconds after an edit (past the session flush's
    2.5 s debounce), the app comes back with the edit unsaved in its tab and the
    file on disk untouched -- VS Code's hot-exit backup, for a crash."""
    open_file(app, "notes/crash.txt", "before\n")
    app.click_text(1, 1)
    app.type("edited ")
    step(app, "edit", 0.3)
    app.expect(app.buffer().get("dirty") is True, "the edit makes the tab dirty")
    time.sleep(4.0)  # past the 2.5 s session-flush debounce
    app.crash_and_relaunch()
    app.expect(read(app, "notes/crash.txt") == "before\n",
               f"the crash leaves the file on disk alone ({read(app, 'notes/crash.txt')!r})")
    app.wait_until(lambda: (app.buffer().get("path") or "").endswith("notes/crash.txt"), 8.0,
                   "the crashed session's tab to come back")
    buffer = app.buffer()
    app.expect(buffer.get("dirty") is True and buffer.get("text") == "edited before\n",
               f"the unsaved edit survives the crash ({buffer.get('dirty')}, {buffer.get('text')!r})")


REPO = Path(__file__).resolve().parents[2]


def _timed(app: q.App, predicate, budget: float, label: str) -> float | None:
    """Waits for `predicate` up to 4x `budget`; a finding when it took longer than
    `budget` (slow) or never happened (broken). Returns the seconds it took."""
    start = time.monotonic()
    ok = app.wait_until(predicate, budget * 4, label)
    took = time.monotonic() - start
    if ok:
        app.expect(took <= budget, f"{label} took {took:.2f}s (budget {budget:.1f}s)")
        return took
    return None


def flow_big_repo(app: q.App) -> None:
    """This repository (~4.6k files) as the project, from a shared clone: the tree
    lists it, Go to File finds a deep file, Find in Files lists hits, and the git
    view takes 300 changed files and Stage All -- each within a budget."""
    clone = app.work / "big-repo"
    subprocess.run(["git", "clone", "-q", "--shared", str(REPO), str(clone)], check=True)
    app.send("project-open", str(clone))
    _timed(app, lambda: any(e["name"] == "row:tree/src" for e in app.elements("row")), 3.0,
           "the tree to list the big project")
    app.send("focus", "editor")
    app.key("ctrl+p")
    step(app, "ctrl+p", 0.2)
    app.type("RemoteHostSession.cpp")
    _timed(app, lambda: (app.ui()["overlay"].get("count") or 0) > 0, 2.0, "Go to File to find a deep file")
    app.key("Return")
    _timed(app, lambda: (app.active_tab() or {}).get("title") == "RemoteHostSession.cpp", 2.0,
           "the deep file to open")
    app.key("ctrl+shift+f")
    step(app, "find in files", 0.3)
    app.type("ProcessLauncher")
    _timed(app, lambda: sum(1 for e in app.elements("row") if e["name"].startswith("row:search/")) > 5, 5.0,
           "Find in Files to list hits across the project")
    app.key("Escape")
    # 300 changed files: the git view lists them, and Stage All stages them.
    sources = sorted(p for p in (clone / "src").rglob("*.cpp"))[:300]
    app.expect(len(sources) == 300, f"the clone has 300 sources to change ({len(sources)})")
    for path in sources:
        with path.open("a") as handle:
            handle.write("// qa\n")
    app.key("ctrl+shift+g")
    app.send("git-refresh")
    app.send("focus", "sidebar")

    def header_counts_300() -> bool:
        app.key("Home")  # the section header sits above the rows the view may be scrolled to
        return any("(300)" in (e.get("label") or "") for e in app.elements("row", prefix="row:git/"))
    _timed(app, header_counts_300, 5.0, "the git view to count 300 changes")
    stage = next((e for e in app.elements("button") if e["name"] == "button:git/stage-all"), None)
    if stage is None:
        app.expect(False, "no Stage All button")
        return
    app.click(*app.center(stage))
    _timed(app, lambda: subprocess.run(["git", "-C", str(clone), "diff", "--cached", "--name-only"],
                                       capture_output=True, text=True).stdout.count("\n") >= 300, 5.0,
           "Stage All to stage the 300 files")


def flow_huge_file(app: q.App) -> None:
    """A 64 MiB log opens without freezing the window, goes to its end, takes an
    edit at the last line and undoes it; a 2 MiB single line takes End and a typed
    character."""
    assert app.project is not None
    huge = app.project / "notes" / "huge.log"
    huge.parent.mkdir(parents=True, exist_ok=True)
    line = "2026-10-19T12:00:00Z INFO microide qa huge-file line with some padding text {:08d}\n"
    with huge.open("w") as handle:
        for i in range(800_000):
            handle.write(line.format(i))
    app.send("open", "notes/huge.log")
    _timed(app, lambda: (app.buffer().get("lineCount") or 0) >= 800_000, 8.0, "the 64 MiB file to load")
    app.expect(app.alive() and bool(app.ui()), "the window answers while it loads")
    app.send("focus", "editor")
    app.key("ctrl+End")
    _timed(app, lambda: (app.cursors() or [(0, 0)])[0][0] >= 800_000, 2.0, "Ctrl+End to reach the last line")
    # `buffer` omits the text past 1 MiB: the dirty flag and the caret say it.
    app.type("tail-edit")
    _timed(app, lambda: app.buffer().get("dirty") is True and (app.cursors() or [(0, 0)])[0][1] >= 10, 2.0,
           "an edit at the end of a 64 MiB buffer")
    app.key("ctrl+z")
    _timed(app, lambda: app.buffer().get("dirty") is False, 2.0, "undo of it (the buffer clean again)")

    wide = app.project / "notes" / "wide.txt"
    wide.write_text("x" * (2 * 1024 * 1024) + "\n")
    app.send("open", "notes/wide.txt")
    _timed(app, lambda: (app.active_tab() or {}).get("title") == "wide.txt", 4.0, "the 2 MiB line to open")
    app.send("focus", "editor")
    app.key("End")
    app.type("Z")
    _timed(app, lambda: (app.cursors() or [(0, 0)])[0] == (1, 2 * 1024 * 1024 + 2), 2.0,
           f"a character typed at the end of a 2 MiB line (caret {app.cursors()[:1]})")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "big-repo": flow_big_repo,
    "huge-file": flow_huge_file,
    "crash-recovery": flow_crash_recovery,
}
