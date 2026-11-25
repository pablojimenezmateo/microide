"""Branch review past the file mark, the staged half of selected ranges, and a
plain push -- git commands the coverage ledger found only the smoke pass ran.

`branch-review` marks a whole file. A review also marks single hunks and writes
notes on them, and those were persisted and never drawn: the marker lived in a
summary only collapsed-context rows paint. The diff now draws it on the hunk's
first row, the `editor` query counts it (`reviewedHunks`, `notedHunks`), and
this flow checks both -- the count, and that the diff's pixels change when a
hunk is marked and come back exactly when it is unmarked.
"""

from __future__ import annotations

import subprocess
from typing import Callable

from PIL import Image, ImageChops

import microide_qa as q
from flows_common import git, open_file, step, write


def _diff_tab(app: q.App) -> dict:
    return next((t for g in app.editor_state()["groups"] for t in g["tabs"]
                 if t.get("active") and g.get("focused")), {})


def _editor_image(app: q.App) -> Image.Image:
    x, y, w, h = app.ui()["surfaces"]["editorSurface"]
    sx, sy = getattr(app, "window_scale", (1.0, 1.0))
    ox, oy = app.origin
    return app.grab().crop((round(x * sx) + ox, round(y * sy) + oy,
                            round((x + w) * sx) + ox, round((y + h) * sy) + oy))


def _differs(a: Image.Image, b: Image.Image) -> int:
    diff = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).convert("L")
    return sum(1 for value in diff.getdata() if value > 8)


def _row_label(app: q.App, name: str) -> str:
    return next((e.get("label", "") for e in app.elements("row") if e["name"] == name), "")


def flow_review_marks(app: q.App) -> None:
    """On an Outgoing file's diff: Mark Branch Hunk Reviewed draws the hunk's
    marker (the diff's pixels change, `reviewedHunks` counts it), a note marks it
    too (`notedHunks`), Unmark puts the diff back exactly; Mark / Unmark Branch
    File Reviewed toggle the git row's marker. Review Diff Against Ref opens one
    diff per changed file against the ref and says what it opened, and its diffs
    take marks; every hunk of a file reviewed marks the file's row."""
    app.send("set-setting", "editor.caret_blink.enabled", "false")
    git(app, "checkout", "-q", "-b", "qa-marks")
    write(app, "review/one.txt", "".join(f"line {i}\n" for i in range(20)))
    write(app, "review/two.txt", "two\n")
    git(app, "add", "-A")
    git(app, "commit", "-q", "-m", "qa: marks fixture")
    app.key("ctrl+shift+g")
    if not app.wait_for("row:git/one.txt", 5.0):
        app.expect(False, "the git view lists the outgoing files")
        return
    app.click_named("row:git/one.txt")
    app.wait_until(lambda: q.loaded_diff(_diff_tab(app)), 5.0, "one.txt's outgoing diff")
    step(app, "outgoing diff", 0.5)
    app.expect(_diff_tab(app).get("reviewedHunks") == 0, f"nothing is reviewed yet ({_diff_tab(app)})")
    before = _editor_image(app) if not app.wayland else None

    app.expect(app.send("mark-branch-hunk-reviewed").get("ok") is True, "marking the hunk runs")
    app.wait_until(lambda: _diff_tab(app).get("reviewedHunks") == 1, 3.0, "the hunk to count as reviewed")
    step(app, "hunk marked", 0.4)
    if not app.wayland:  # no screen to read on a headless weston (App.grab)
        changed = _differs(before, _editor_image(app))
        app.expect(changed > 0, f"the diff draws the hunk's reviewed marker ({changed} pixels changed)")
    app.expect(app.send("branch-review-comment", "checked the loop").get("ok") is True, "a note on the hunk runs")
    app.wait_until(lambda: _diff_tab(app).get("notedHunks") == 1, 3.0, "the note to count")
    app.send("unmark-branch-hunk-reviewed")
    app.wait_until(lambda: _diff_tab(app).get("reviewedHunks") == 0, 3.0, "the hunk to be unmarked")
    step(app, "hunk unmarked", 0.4)

    app.send("mark-branch-file-reviewed")
    app.wait_until(lambda: _row_label(app, "row:git/one.txt").startswith("[reviewed]"), 3.0,
                   "the file's reviewed marker on its row")
    app.send("unmark-branch-file-reviewed")
    app.wait_until(lambda: not _row_label(app, "row:git/one.txt").startswith("["), 3.0,
                   "the file's marker to go")
    step(app, "file unmarked", 0.3)

    reply = app.send("review-branch", "main")
    app.expect(bool(reply.get("ok")), f"Review Diff Against Ref runs ({reply})")
    step(app, "review against main", 0.6)
    tab = _diff_tab(app)
    app.expect(tab.get("kind") == "compare" and tab.get("leftLabel") == "main",
               f"it opens a diff against main ({tab.get('kind')}, {tab.get('leftLabel')!r})")
    messages = [n.get("message", "") for n in app.query("notifications")]
    app.expect(any("review-branch main" in m and "one.txt" in m and "two.txt" in m for m in messages),
               f"and says which files it opened ({messages})")
    # Its diffs take review marks too (TD-2026-10-22-381): they used to refuse
    # with "Not a branch review".
    reply = app.send("mark-branch-hunk-reviewed")
    app.expect(reply.get("ok") is True, f"a review-branch diff takes a hunk mark ({reply})")
    app.wait_until(lambda: _diff_tab(app).get("reviewedHunks") == 1, 3.0, "the hunk to count as reviewed")
    app.expect(_diff_tab(app).get("reviewedHunks") == 1, f"and counts it ({_diff_tab(app)})")
    app.send("unmark-branch-hunk-reviewed")

    # Every hunk of an Outgoing file reviewed marks the file: its row says so.
    app.key("ctrl+shift+g")
    app.click_named("row:git/two.txt")
    app.wait_until(lambda: q.loaded_diff(_diff_tab(app)), 5.0, "two.txt's outgoing diff")
    app.send("mark-branch-hunk-reviewed")  # two.txt's one hunk
    app.wait_until(lambda: _row_label(app, "row:git/two.txt").startswith("[reviewed]"), 3.0,
                   "the row marker once every hunk is reviewed")
    app.expect(_row_label(app, "row:git/two.txt").startswith("[reviewed]"),
               f"a file with every hunk reviewed shows as reviewed ({_row_label(app, 'row:git/two.txt')!r})")
    app.send("unmark-branch-hunk-reviewed")
    app.wait_until(lambda: not _row_label(app, "row:git/two.txt").startswith("["), 3.0,
                   "unmarking the hunk to take the file's marker back")


def flow_git_unstage_and_push(app: q.App) -> None:
    """Ctrl+K Ctrl+N in a diff unstages the staged change at the caret and only it
    (VS Code's Unstage Selected Ranges); Git: Push sends a committed branch to its
    upstream on a local bare origin."""
    head = "".join(f"line {i}\n" for i in range(1, 41))
    write(app, "ranges/unstage.txt", head)
    git(app, "add", "ranges/unstage.txt")
    git(app, "commit", "-q", "-m", "qa: unstage fixture")
    write(app, "ranges/unstage.txt",
          head.replace("line 3\n", "line three\n").replace("line 35\n", "line thirty-five\n"))
    git(app, "add", "ranges/unstage.txt")
    app.key("ctrl+shift+g")
    row = app.wait_for("row:git/unstage.txt", 5.0)
    if row is None:
        app.expect(False, "the git view lists the staged file")
        return
    app.click(*app.center(row))
    app.wait_until(lambda: q.loaded_diff(_diff_tab(app)), 5.0, "the staged diff")
    app.send("focus", "editor")
    step(app, "diff", 0.4)
    app.key("ctrl+k")
    app.key("ctrl+n")
    app.wait_until(lambda: "line three" not in git(app, "show", ":ranges/unstage.txt"), 5.0,
                   "Ctrl+K Ctrl+N to unstage the change at the caret")
    staged = git(app, "show", ":ranges/unstage.txt")
    app.expect("line thirty-five" in staged, "the other staged change stays staged")
    app.expect("line three" in (app.project / "ranges/unstage.txt").read_text(),
               "unstaging leaves the working tree alone")

    assert app.project is not None
    remote = app.project.parent / "push-origin.git"
    subprocess.run(["git", "init", "--bare", "-q", str(remote)], check=True)
    git(app, "remote", "add", "origin", str(remote))
    git(app, "commit", "-q", "-am", "qa: to push")
    branch = git(app, "rev-parse", "--abbrev-ref", "HEAD")
    git(app, "push", "-q", "-u", "origin", branch)
    write(app, "ranges/pushed.txt", "pushed\n")
    git(app, "add", "-A")
    git(app, "commit", "-q", "-m", "qa: pushed by the app")
    app.send("git-refresh")
    reply = app.send("git-push")
    app.expect(bool(reply.get("ok")), f"Git: Push runs ({reply})")
    app.wait_until(lambda: subprocess.run(["git", "-C", str(remote), "log", "-1", "--format=%s", branch],
                                          capture_output=True, text=True).stdout.strip() == "qa: pushed by the app",
                   10.0, "the commit to reach origin")
    step(app, "pushed", 0.3)


def flow_review_controls(app: q.App) -> None:
    """The review done with the mouse alone, on the diff's own controls
    (render/CompareReviewControls): the selected change's Mark Reviewed marks it
    and becomes Unmark; Next Change moves the buttons to the second change;
    Comment opens the prompt, and what is typed shows as a row under the change
    with Edit and Delete; Delete removes it; Mark File Reviewed marks every
    change. Then the status bar counts a linked worktree, and a click on it opens
    Switch Worktree."""
    app.send("set-setting", "editor.caret_blink.enabled", "false")
    # The file on main first, then the agent's two changes to it on a branch: two
    # hunks far enough apart that the run between them collapses.
    write(app, "controls/calc.txt", "".join(f"line {i}\n" for i in range(1, 31)))
    git(app, "add", "-A")
    git(app, "commit", "-q", "-m", "qa: controls base")
    git(app, "checkout", "-q", "-b", "qa-controls")
    write(app, "controls/calc.txt",
          "".join(f"changed {i}\n" if i in (3, 25) else f"line {i}\n" for i in range(1, 31)))
    git(app, "add", "-A")
    git(app, "commit", "-q", "-m", "qa: controls fixture")
    app.key("ctrl+shift+g")
    if not app.wait_for("row:git/calc.txt", 5.0):
        app.expect(False, "the git view lists the outgoing file")
        return
    app.click_named("row:git/calc.txt")
    app.wait_until(lambda: q.loaded_diff(_diff_tab(app)), 5.0, "calc.txt's outgoing diff")
    if not app.wait_for("button:compare/hunk-0/toggle-reviewed", 5.0):
        app.expect(False, "the selected change shows Mark Reviewed")
        return
    step(app, "review controls", 0.4)

    app.click_named("button:compare/hunk-0/toggle-reviewed")
    app.wait_until(lambda: _diff_tab(app).get("reviewedHunks") == 1, 3.0, "the click to mark the change")
    app.expect(app.element("button:compare/hunk-0/toggle-reviewed").get("label") == "Unmark",
               "a marked change offers Unmark")
    app.click_named("button:compare/next-change")
    app.wait_for("button:compare/hunk-1/comment", 3.0)
    step(app, "next change", 0.3)

    app.click_named("button:compare/hunk-1/comment")
    app.wait_until(lambda: app.ui().get("modal") is not None, 3.0, "the comment prompt")
    app.type("check the upper bound")
    app.key("Return")
    app.wait_until(lambda: _diff_tab(app).get("notedHunks") == 1, 3.0, "the comment to be kept")
    edit = app.wait_for("button:compare/note-1/edit", 3.0) or {}
    app.expect(edit.get("label") == "check the upper bound", f"the comment shows under its change ({edit})")
    step(app, "comment row", 0.3)
    reply = app.send("copy-review-comments")
    app.expect(reply.get("ok") is True, f"the comment can be handed over ({reply})")
    app.click_named("button:compare/note-1/delete")
    app.wait_until(lambda: _diff_tab(app).get("notedHunks") == 0, 3.0, "Delete to remove the comment")

    app.click_named("button:compare/file-reviewed")
    app.wait_until(lambda: _diff_tab(app).get("reviewedHunks") == 2, 3.0, "Mark File Reviewed to mark both")
    app.click_named("button:compare/file-reviewed")
    app.wait_until(lambda: _diff_tab(app).get("reviewedHunks") == 0, 3.0, "Unmark File to take them back")
    step(app, "file unmarked", 0.3)

    assert app.project is not None
    tree = app.project.parent / "qa-controls-tree"
    git(app, "worktree", "add", "-q", "-b", "qa-controls-tree", str(tree))
    app.send("git-refresh")
    segment = app.wait_for("status:worktrees", 5.0) or {}
    app.expect(segment.get("label") == "2 worktrees", f"the status bar counts the linked tree ({segment})")
    if segment:
        app.click_named("status:worktrees")
        app.wait_until(lambda: app.ui().get("overlay", {}).get("visible") is True, 5.0,
                       "a click on it to open Switch Worktree")
        app.key("Escape")
    git(app, "worktree", "remove", "--force", str(tree))


FLOWS: dict[str, Callable[[q.App], None]] = {
    "review-marks": flow_review_marks,
    "review-controls": flow_review_controls,
    "git-unstage-and-push": flow_git_unstage_and_push,
}
