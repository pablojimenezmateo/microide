"""The diff view's own commands, each with a receipt.

`commands-run` runs every command once as a smoke pass and the Git menu sweep is
a dry run, so the diff's toggles and copies were never checked for what they DO:
Ignore Whitespace hiding a whitespace-only change, Show Whitespace, Copy Change /
Copy File as Patch (the patch read back by pasting it, then proven exact with
`git apply --check -R` against the working tree), Copy Path, and stepping
between the files of a commit under review.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, git, git_run, open_conflict, open_file, step, write

BASE = "".join(f"line {i}\n" for i in range(1, 31))


def _diff_tab(app: q.App) -> dict:
    """The focused group's active tab as the `editor` query reports it."""
    state = app.editor_state()
    for group in state.get("groups", []):
        for tab in group.get("tabs", []):
            if tab.get("active") and group.get("focused"):
                return tab
    return {}


def _run(app: q.App, label: str) -> bool:
    """Runs a palette row by its label (typed, the row clicked)."""
    app.key("ctrl+shift+p")
    step(app, "palette", 0.3)
    app.type(label)
    step(app, f"type {label}", 0.3)
    row = next((r for r in app.elements("overlay-row") if r.get("label", "").startswith(label)), None)
    if not app.expect(row is not None, f"the palette lists {label!r}"):
        dismiss(app)
        return False
    app.click(*app.center(row))
    step(app, label, 0.4)
    return True


def _paste_into_scratch(app: q.App, back_to: str, kind: str = "compare") -> str:
    """The clipboard, read by pasting it into a scratch file (the real paste
    path), then the diff tab `back_to` activated again."""
    open_file(app, "notes/scratch.txt", "")
    app.key("ctrl+a")
    app.key("ctrl+v")
    step(app, "paste", 0.3)
    text = app.text()
    app.key("ctrl+a")
    app.key("Delete")
    app.key("ctrl+s")
    app.click_named(back_to)
    app.wait_until(lambda: _diff_tab(app).get("kind") == kind, 5.0, f"the {kind} tab again")
    app.send("focus", "editor")
    step(app, "back to the diff", 0.3)
    return text


def flow_diff_commands(app: q.App) -> None:
    """Toggles and copies on a working-tree diff with one whitespace-only change
    and one real change."""
    assert app.project is not None
    write(app, "notes/ws.txt", BASE)
    git(app, "add", "notes/ws.txt")
    git(app, "commit", "-q", "-m", "qa: whitespace fixture")
    changed = BASE.replace("line 5\n", "line 5   \n").replace("line 20\n", "line twenty\n")
    write(app, "notes/ws.txt", changed)
    app.key("ctrl+shift+g")
    step(app, "git view", 0.5)
    row = app.wait_for("row:git/ws.txt", 5.0)
    if row is None:
        return
    app.click(*app.center(row))
    if not app.wait_until(lambda: _diff_tab(app).get("kind") == "compare", 5.0, "the diff to open"):
        return
    app.send("focus", "editor")
    step(app, "the diff", 0.5)
    tab_name = next((e["name"] for e in app.elements("tab") if e.get("active")), None) \
        or next((e["name"] for e in app.elements("tab") if "ws.txt" in e["name"]), "")
    app.expect(_diff_tab(app).get("changes") == 2,
               f"the diff shows the whitespace change and the real one ({_diff_tab(app)})")

    if _run(app, "Diff: Toggle Ignore Whitespace"):
        app.wait_until(lambda: _diff_tab(app).get("ignoreWhitespace") is True, 3.0, "ignore whitespace on")
        app.expect(_diff_tab(app).get("changes") == 1,
                   f"ignoring whitespace hides the whitespace-only change ({_diff_tab(app)})")
    if _run(app, "Diff: Toggle Ignore Whitespace"):
        app.wait_until(lambda: _diff_tab(app).get("ignoreWhitespace") is False, 3.0, "ignore whitespace off")
        app.expect(_diff_tab(app).get("changes") == 2, f"and it comes back ({_diff_tab(app)})")
    for state in (True, False):
        if _run(app, "Diff: Toggle Show Whitespace"):
            app.wait_until(lambda: _diff_tab(app).get("showWhitespace") is state, 3.0,
                           f"show whitespace {'on' if state else 'off'}")
            app.expect(_diff_tab(app).get("showWhitespace") is state,
                       f"Show Whitespace toggles to {state} ({_diff_tab(app)})")

    # The copies go through the clipboard, which the Wayland backend's injected
    # input cannot reach (TD-2026-10-19-366).
    if app.wayland:
        return
    if _run(app, "Diff: Copy Path"):
        pasted = _paste_into_scratch(app, tab_name)
        app.expect(pasted.strip().endswith("notes/ws.txt"), f"Copy Path copies the file's path ({pasted!r})")

    # The caret on the real change: Copy Change as Patch takes that hunk.
    app.key("ctrl+Home")
    app.key("alt+bracketright")
    app.key("alt+bracketright")
    step(app, "the second change", 0.3)
    if _run(app, "Diff: Copy Change as Patch"):
        pasted = _paste_into_scratch(app, tab_name)
        app.expect("@@" in pasted and "-line 20" in pasted and "+line twenty" in pasted
                   and "line 5   " not in pasted,
                   f"Copy Change as Patch copies the change at the caret, alone ({pasted!r})")

    if _run(app, "Diff: Copy File as Patch"):
        pasted = _paste_into_scratch(app, tab_name)
        app.expect("+line twenty" in pasted and "+line 5   " in pasted,
                   f"Copy File as Patch copies both changes ({pasted!r})")
        patch = app.project.parent / "qa-file.patch"
        patch.write_text(pasted if pasted.endswith("\n") else pasted + "\n")
        verify = git_run(app, "apply", "--check", "-R", str(patch))
        app.expect(verify.returncode == 0,
                   f"the copied patch is exact: it reverse-applies to the working tree ({verify.stderr!r})")


def flow_diff_review_files(app: q.App) -> None:
    """A commit touching three files, reviewed: Next/Previous File in Review step
    through them in order and stop at the ends."""
    assert app.project is not None
    for name in ("a", "b", "c"):
        write(app, f"notes/review-{name}.txt", f"{name} one\n{name} two\n")
    git(app, "add", "notes")
    git(app, "commit", "-q", "-m", "qa: review base")
    for name in ("a", "b", "c"):
        write(app, f"notes/review-{name}.txt", f"{name} one\n{name} TWO\n")
    git(app, "commit", "-q", "-am", "qa: review three files")
    reply = app.send("review-commit", "HEAD")
    if not app.expect(bool(reply.get("ok")), f"review-commit HEAD runs ({reply})"):
        return
    if not app.wait_until(lambda: _diff_tab(app).get("reviewFiles") == 3, 8.0, "a review of three files"):
        app.expect(False, f"the review names its three files ({_diff_tab(app)})")
        return
    app.send("focus", "editor")
    step(app, "the review", 0.5)
    seen = [(_diff_tab(app).get("reviewFileIndex"), _diff_tab(app).get("title"))]
    for _ in range(3):
        _run(app, "Diff: Next File in Review")
        seen.append((_diff_tab(app).get("reviewFileIndex"), _diff_tab(app).get("title")))
    app.expect([index for index, _ in seen] == [1, 2, 3, 3],
               f"Next File steps 1 -> 2 -> 3 and stops at the last ({seen})")
    app.expect(len({title for _, title in seen[:3]}) == 3, f"each step shows another file ({seen})")
    back = []
    for _ in range(3):
        _run(app, "Diff: Previous File in Review")
        back.append(_diff_tab(app).get("reviewFileIndex"))
    app.expect(back == [2, 1, 1], f"Previous File steps back and stops at the first ({back})")


def flow_merge_commands(app: q.App) -> None:
    """The merge editor's commands from the palette: Copy Incoming / Copy Current
    Side of the conflict (read back by pasting), then Git: Accept Incoming taking
    the whole file's incoming side, staged."""
    assert app.project is not None
    if not open_conflict(app):
        return
    merge_tab = next((e["name"] for e in app.elements("tab") if e.get("active")), "")
    # The copies go through the clipboard (not on the Wayland backend: TD-2026-10-19-366).
    for label, want, not_want in () if app.wayland else (("Merge: Copy Incoming Side", "line 2 incoming", "line 2 current"),
                                  ("Merge: Copy Current Side", "line 2 current", "line 2 incoming")):
        if _run(app, label):
            pasted = _paste_into_scratch(app, merge_tab, "merge")
            app.expect(want in pasted and not_want not in pasted,
                       f"{label} copies that side of the conflict alone ({pasted!r})")
    if _run(app, "Git: Accept Incoming"):
        path = app.project / "notes" / "merge.txt"
        app.wait_until(lambda: "<<<<<<<" not in path.read_text(), 8.0, "the file to take the incoming side")
        text = path.read_text()
        app.expect("line 2 incoming\n" in text and "line 10 incoming\n" in text and "current" not in text,
                   f"Accept Incoming takes the incoming side of every conflict ({text!r})")
        app.wait_until(lambda: "UU" not in git(app, "status", "--porcelain"), 8.0, "the file to be resolved")
        status = git(app, "status", "--porcelain", "notes/merge.txt")
        app.expect(status.startswith("M ") or status == "",
                   f"and marks it resolved in the index ({status!r})")


def _merge(app: q.App) -> dict:
    return next((t for g in app.editor_state().get("groups", []) for t in g.get("tabs", [])
                 if t.get("kind") == "merge" and t.get("active")), {})


def flow_merge_toolbar(app: q.App) -> None:
    """The merge editor's toolbar and its keys: Next / Previous step the selected
    conflict; Toggle Base shows and hides the ancestor pane; Unresolved jumps to
    the next conflict left; Alt+R resets a resolved conflict; Mark Resolved
    refuses while a conflict is open, and once both are resolved and saved it
    stages the file.

    `merge-mouse` accepts sides and saves; the toolbar's other buttons, the reset
    (reachable from nothing until this flow looked for it) and Mark Resolved's
    refusal and staging ran at under half in the coverage build (TD-364)."""
    assert app.project is not None
    if not open_conflict(app):
        return
    merge = _merge(app)
    app.expect(merge.get("conflicts") == 2 and merge.get("unresolved") == 2 and merge.get("selectedConflict") == 1,
               f"the merge opens on the first of two open conflicts ({merge})")

    def press(name: str, label: str) -> bool:
        button = app.element(f"button:merge/{name}")
        if not app.expect(bool(button), f"the toolbar shows {name} ({[e['name'] for e in app.elements('button', prefix='button:merge/')]})"):
            return False
        app.click(*app.center(button))
        step(app, label, 0.3)
        return True

    if press("next", "Next"):
        app.expect(_merge(app).get("selectedConflict") == 2, f"Next selects the second conflict ({_merge(app)})")
    if press("prev", "Previous"):
        app.expect(_merge(app).get("selectedConflict") == 1, f"Previous goes back to the first ({_merge(app)})")
    if press("Toggle Base", "Toggle Base"):
        app.expect(_merge(app).get("basePane") is True, f"Toggle Base shows the ancestor pane ({_merge(app)})")
        app.check("the base pane out")
        press("Toggle Base", "Toggle Base again")
        app.expect(_merge(app).get("basePane") is False, f"and hides it again ({_merge(app)})")

    # Resolve the first, reset it with Alt+R, resolve it again.
    app.send("focus", "editor")
    app.key("alt+i")
    app.wait_until(lambda: _merge(app).get("unresolved") == 1, 3.0, "Alt+I to resolve the first conflict")
    app.key("alt+r")
    app.wait_until(lambda: _merge(app).get("unresolved") == 2, 3.0, "Alt+R to reset it")
    step(app, "Alt+R", 0.3)
    app.expect(_merge(app).get("unresolved") == 2, f"Alt+R puts the conflict back ({_merge(app)})")
    app.key("alt+i")
    app.wait_until(lambda: _merge(app).get("unresolved") == 1, 3.0, "Alt+I again")

    if press("Unresolved", "Unresolved"):
        app.expect(_merge(app).get("selectedConflict") == 2, f"Unresolved jumps to the conflict left ({_merge(app)})")
    if press("Mark Resolved", "Mark Resolved with a conflict open"):
        merge = _merge(app)
        app.expect(not merge.get("markedResolved") and merge.get("status"),
                   f"Mark Resolved refuses while a conflict is open, and says why ({merge})")
        app.expect("UU" in git(app, "status", "--porcelain", "notes/merge.txt"),
                   "and leaves the file conflicted in the index")
    app.send("focus", "editor")
    app.key("alt+c")
    app.wait_until(lambda: _merge(app).get("unresolved") == 0, 3.0, "Alt+C to resolve the second")
    press("save", "Save")
    app.wait_until(lambda: not _merge(app).get("dirty"), 3.0, "the result to save")
    if press("Mark Resolved", "Mark Resolved"):
        app.wait_until(lambda: "UU" not in git(app, "status", "--porcelain", "notes/merge.txt"), 5.0, "the file to be staged")
        text = (app.project / "notes" / "merge.txt").read_text()
        app.expect("line 2 incoming\n" in text and "line 10 current\n" in text and "<<<<<<<" not in text,
                   f"the result holds each conflict's choice ({text!r})")
        status = git(app, "status", "--porcelain", "notes/merge.txt")
        app.expect(status.startswith("M ") or status == "", f"Mark Resolved stages it ({status!r})")


def _outgoing(app: q.App) -> set[str]:
    """The git view's Outgoing files (the rows after the Outgoing header)."""
    rows = [e for e in app.elements("row", prefix="row:git/")]
    names, inside = set(), False
    for row in rows:
        label = row.get("label", "")
        if label.startswith("Outgoing"):
            inside = True
            continue
        if inside and row["name"].endswith(".txt"):
            names.add(row["name"].split("/", 1)[1])
    return names


def flow_git_outgoing_base(app: q.App) -> None:
    """The Outgoing section's base, from its dropdown: Auto lists the branch's
    files against main; Previous commit (HEAD~1) narrows to the last commit's;
    "Branch or commit…" opens a picker that Up/Down walk and Enter takes, and
    picking main lists the branch's files again.

    The dropdown and the base picker were reached by nothing (TD-364)."""
    git(app, "checkout", "-q", "-b", "qa-base")
    write(app, "notes/first.txt", "first\n")
    git(app, "add", "-A")
    git(app, "commit", "-q", "-m", "qa: first")
    write(app, "notes/second.txt", "second\n")
    git(app, "add", "-A")
    git(app, "commit", "-q", "-m", "qa: second")
    app.send("git-refresh")
    app.key("ctrl+shift+g")
    if not app.wait_until(lambda: _outgoing(app) == {"first.txt", "second.txt"}, 8.0,
                          "the branch's two files under Outgoing"):
        app.expect(False, f"Outgoing lists both files ({_outgoing(app)})")
        return
    step(app, "outgoing against main", 0.3)

    def choose(label_prefix: str) -> bool:
        button = app.element("button:git/outgoing-base")
        if not app.expect(bool(button), "the Outgoing header has its base dropdown"):
            return False
        app.click(*app.center(button))
        app.wait_until(lambda: any(e.get("label", "").startswith(label_prefix) for e in app.elements("menu-item")),
                       3.0, "the base menu")
        item = next((e for e in app.elements("menu-item") if e.get("label", "").startswith(label_prefix)), None)
        if not app.expect(item is not None, f"the base menu offers {label_prefix!r} "
                                            f"({[e.get('label') for e in app.elements('menu-item')]})"):
            return False
        app.click(*app.center(item))
        step(app, f"base: {label_prefix}", 0.3)
        return True

    if choose("Previous commit"):
        app.wait_until(lambda: _outgoing(app) == {"second.txt"}, 8.0, "Outgoing to narrow to the last commit")
        app.expect(_outgoing(app) == {"second.txt"}, f"against HEAD~1 only the last commit's file is outgoing ({_outgoing(app)})")
    if choose("Branch or commit"):
        app.wait_until(lambda: app.ui()["overlay"].get("visible") and (app.ui()["overlay"].get("count") or 0) > 0, 5.0,
                       "the base picker to list refs")
        overlay = app.ui()["overlay"]
        app.expect(overlay.get("visible") and (overlay.get("count") or 0) > 1, f"the picker lists branches and commits ({overlay})")
        app.key("Down")
        moved = app.ui()["overlay"].get("selected")
        app.key("Up")
        app.expect(moved == 1 and app.ui()["overlay"].get("selected") == 0,
                   f"Down and Up walk the picker ({moved}, {app.ui()['overlay'].get('selected')})")
        app.type("main")
        app.wait_until(lambda: (app.ui()["overlay"].get("count") or 0) >= 1, 3.0, "main in the picker")
        app.key("Return")
        app.wait_until(lambda: _outgoing(app) == {"first.txt", "second.txt"}, 8.0, "Outgoing against main again")
        step(app, "base: main from the picker", 0.3)
        app.expect(_outgoing(app) == {"first.txt", "second.txt"}, f"picking main lists both again ({_outgoing(app)})")
    if choose("Auto"):
        app.wait_until(lambda: _outgoing(app) == {"first.txt", "second.txt"}, 8.0, "Outgoing back on Auto")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "merge-commands": flow_merge_commands,
    "git-outgoing-base": flow_git_outgoing_base,
    "merge-toolbar": flow_merge_toolbar,
    "diff-commands": flow_diff_commands,
    "diff-review-files": flow_diff_review_files,
}
