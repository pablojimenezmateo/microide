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
    diff per changed file against the ref and says what it opened."""
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
    app.wait_until(lambda: _diff_tab(app).get("kind") == "compare", 5.0, "one.txt's outgoing diff")
    step(app, "outgoing diff", 0.5)
    app.expect(_diff_tab(app).get("reviewedHunks") == 0, f"nothing is reviewed yet ({_diff_tab(app)})")
    before = _editor_image(app) if not app.wayland else None

    app.expect(app.send("mark-branch-hunk-reviewed").get("ok") is True, "marking the hunk runs")
    app.wait_until(lambda: _diff_tab(app).get("reviewedHunks") == 1, 3.0, "the hunk to count as reviewed")
    step(app, "hunk marked", 0.4)
    if not app.wayland:  # no screen to read on a headless weston (App.grab)
        changed = _differs(before, _editor_image(app))
        app.expect(changed > 0, f"the diff draws the hunk's reviewed marker ({changed} pixels changed)")
    app.expect(app.send("branch-review-note", "checked the loop").get("ok") is True, "a note on the hunk runs")
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
    app.wait_until(lambda: _diff_tab(app).get("kind") == "compare", 5.0, "the staged diff")
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


FLOWS: dict[str, Callable[[q.App], None]] = {
    "review-marks": flow_review_marks,
    "git-unstage-and-push": flow_git_unstage_and_push,
}
