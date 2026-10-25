"""Whole journeys across subsystems, as one person's session: a feature branch
from branch to pushed commit, a rename across files with an unsaved buffer in
the way, an editor layout built up and taken back down. Each subsystem has its
own flows; what these find is the seams -- a tree that does not hear about a
branch switch, a search replace that writes under an unsaved buffer, a terminal
that builds a file the editor has not saved."""

from __future__ import annotations

import shutil
import subprocess
from typing import Callable

import microide_qa as q
from flows_common import active_title, dismiss, git, open_file, read, step, titles, tree_rows, write


def _terminal_text(app: q.App) -> str:
    return app.query("terminal-output").get("text", "")


def _run(app: q.App, command: str, marker: str, timeout: float = 20.0) -> bool:
    """Runs `command` in the active terminal and waits for `marker` in its output."""
    app.send("focus", "panel")
    app.type(command + "\n")
    return app.wait_until(lambda: marker in _terminal_text(app), timeout, f"`{command}` to print {marker}")


def flow_journey_feature_branch(app: q.App) -> None:
    """A feature from branch to commit: create a branch, add a source file from the
    tree's context menu, write a function with auto-closed braces and smart indent,
    build it in the terminal (unsaved, the file is still empty on disk; saved, it
    builds), stage and commit from the git view, review the commit, switch
    back to main -- the file leaves the tree and its tab says why -- and return."""
    if shutil.which("g++") is None:
        app.expect(False, "g++ is needed to build in the terminal")
        return
    reply = app.send("git-create-branch", "feature/qa-greet")
    app.expect(bool(reply.get("ok")), f"Git: Create Branch runs ({reply})")
    app.wait_until(lambda: git(app, "rev-parse", "--abbrev-ref", "HEAD") == "feature/qa-greet", 5.0,
                   "the new branch to be checked out")
    app.wait_until(lambda: "feature/qa-greet" in (app.element("status:branch").get("label", "")
                                                   if any(e["name"] == "status:branch" for e in app.elements("status"))
                                                   else app.element("status:project").get("label", "")),
                   5.0, "the status bar to name the branch")

    # A new file from the tree, written with the editor's own help.
    app.send("sidebar-show", "tree")
    src = app.wait_for("row:tree/src", 3.0)
    if not src:
        return
    app.click(*app.center(src), button=3)
    item = app.wait_for("menu-item:context/New File…", 3.0)
    if not item:
        dismiss(app)
        return
    app.click(*app.center(item))
    prompt = app.wait_for("field:prompt", 3.0)
    if not prompt:
        return
    app.key("ctrl+a")
    app.type("greet.cpp")
    app.key("Return")
    app.wait_until(lambda: active_title(app) == "greet.cpp", 5.0, "the new file to open")
    step(app, "new file", 0.3)
    app.type("int greet(int n) {\n")
    app.type("return n * 2;")
    step(app, "write the function", 0.3)
    app.expect(app.text() == "int greet(int n) {\n\treturn n * 2;\n}" or
               app.text() == "int greet(int n) {\n    return n * 2;\n}",
               f"the brace closes and the body is indented ({app.text()!r})")
    # Unsaved: the compiler sees the empty file on disk, not the buffer.
    app.send("term")
    app.wait_until(lambda: bool(app.query("terminals")), 5.0, "a terminal")
    app.expect(_run(app, "test -s src/greet.cpp && echo ON-DISK || echo EMPTY-ON-DISK", "-DISK")
               and "EMPTY-ON-DISK" in _terminal_text(app),
               "the unsaved function is not on disk yet")
    app.send("focus", "editor")
    app.key("ctrl+s")
    app.wait_until(lambda: "greet" in read(app, "src/greet.cpp"), 3.0, "Ctrl+S to write the file")
    step(app, "save", 0.3)
    app.expect(_run(app, "g++ -c src/greet.cpp -o greet.o && echo BUILD-OK", "BUILD-OK"),
               "the saved file builds in the terminal")

    # Stage and commit from the git view.
    app.key("ctrl+shift+g")
    if not app.wait_for("row:git/greet.cpp", 5.0):
        app.expect(False, f"the git view lists the new file ({[e['name'] for e in app.elements('row')]})")
        return
    stage_all = app.wait_for("button:git/stage-all", 2.0)
    if stage_all:
        app.click(*app.center(stage_all))
        app.wait_until(lambda: "src/greet.cpp" in git(app, "diff", "--cached", "--name-only"), 5.0,
                       "Stage All to stage the new file")
    git(app, "reset", "-q", "greet.o")  # Stage All took the build output too: it is not the feature's
    app.click_named("button:git/commit")
    subject = app.wait_for("field:git/subject", 3.0)
    if subject:
        app.click(*app.center(subject))
        app.type("feat: greet")
        app.key("ctrl+Return")
        # The build output is untracked: the commit asks before leaving it out.
        anyway = app.wait_for("button:prompt/Commit Anyway", 3.0)
        app.expect(anyway is not None and "Untracked" in (app.ui().get("prompt") or {}).get("message", ""),
                   f"committing past an untracked file asks first ({app.ui().get('prompt')})")
        if anyway:
            app.click(*app.center(anyway))
        app.wait_until(lambda: git(app, "log", "-1", "--format=%s") == "feat: greet", 6.0, "the commit")
        app.expect("greet.o" not in git(app, "show", "--name-only", "--format=", "HEAD"),
                   "the untracked build output stays out of the commit")
    app.expect("src/greet.cpp" in git(app, "show", "--name-only", "--format=", "HEAD"),
               "the commit holds the new file")

    reply = app.send("review-commit", "HEAD")
    app.expect(bool(reply.get("ok")), f"the commit opens for review ({reply})")
    app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 5.0, "the commit's diff")
    step(app, "review the commit", 0.4)

    # Back to main: the file is gone from the tree; then back to the feature.
    (app.project / "greet.o").unlink(missing_ok=True)
    reply = app.send("git-switch-branch", "main")
    app.expect(bool(reply.get("ok")), f"switching to main runs ({reply})")
    app.wait_until(lambda: not (app.project / "src" / "greet.cpp").exists(), 5.0, "main's tree without greet.cpp")
    app.send("sidebar-show", "tree")
    app.wait_until(lambda: "row:tree/greet.cpp" not in tree_rows(app), 5.0, "the tree to drop greet.cpp on main")
    app.send("git-switch-branch", "feature/qa-greet")
    app.wait_until(lambda: (app.project / "src" / "greet.cpp").exists(), 5.0, "the feature branch's file back")
    app.wait_until(lambda: "row:tree/greet.cpp" in tree_rows(app), 5.0, "the tree to list greet.cpp again")


def flow_journey_rename_refactor(app: q.App) -> None:
    """Rename a type across the project from the search view while one of the files
    is open with an unsaved edit: Replace All changes the files on disk and the
    open, clean ones follow; the dirty one is changed IN its buffer (VS Code) --
    its unsaved edit kept, the disk not written under it -- until it is saved;
    the result builds; Discard All in the git view takes everything back."""
    if shutil.which("g++") is None:
        app.expect(False, "g++ is needed to build in the terminal")
        return
    open_file(app, "src/main.cpp")
    open_file(app, "include/scheduler.hpp")
    app.key("ctrl+End")
    app.type("// unsaved note\n")
    step(app, "dirty the header", 0.2)
    disk_header = read(app, "include/scheduler.hpp")

    app.key("ctrl+shift+f")
    step(app, "search view", 0.3)
    app.key("ctrl+a")
    app.type("Scheduler")
    app.wait_until(lambda: sum(1 for e in app.elements("row") if e["name"].startswith("row:search/")) >= 3, 5.0,
                   "the uses of Scheduler to list")
    app.click_named("field:search/replace")
    app.type("Planner")
    app.click_named("button:search/replace-all")
    confirm = app.wait_for("button:prompt/Replace All", 3.0)
    if not confirm:
        return
    app.click(*app.center(confirm))
    step(app, "replace all", 0.8)
    app.wait_until(lambda: "Scheduler" not in read(app, "src/scheduler.cpp")
                   and "Scheduler" not in read(app, "src/main.cpp"), 5.0, "the files on disk to be replaced")
    app.send("open", "include/scheduler.hpp")
    app.wait_until(lambda: active_title(app) == "scheduler.hpp", 3.0, "the header")
    header = app.text()
    app.expect("Planner" in header and "Scheduler" not in header and "// unsaved note" in header,
               f"the dirty header is replaced in its buffer, its unsaved edit kept ({header[-80:]!r})")
    app.expect(read(app, "include/scheduler.hpp") == disk_header,
               "and the disk is not written under the unsaved buffer")
    app.expect(app.buffer().get("dirty"), "the header stays dirty until it is saved")
    app.send("open", "src/main.cpp")
    app.wait_until(lambda: "Planner" in app.text(), 3.0, "the open clean main.cpp to reload")
    app.send("open", "include/scheduler.hpp")
    app.key("ctrl+s")
    app.wait_until(lambda: "Planner" in read(app, "include/scheduler.hpp"), 3.0, "the header to save")
    app.send("term")
    app.expect(_run(app, "g++ -std=c++20 -Iinclude src/*.cpp -o /tmp/qa-planner-$$ && echo RENAMED-BUILDS",
                    "RENAMED-BUILDS", 40.0), "the renamed project builds")

    app.key("ctrl+shift+g")
    # Enabled, not just on screen: the git view lists the saved changes once its
    # status refresh lands, and until then Discard All has nothing to discard and
    # a click on it does nothing (QA journey-rename-refactor, ASan build).
    discard = None

    def discard_enabled() -> bool:
        nonlocal discard
        discard = next((e for e in app.elements("button", prefix="button:git/discard-all")
                        if e.get("enabled", True)), None)
        return discard is not None
    if app.wait_until(discard_enabled, 5.0, "Discard All to have changes to discard"):
        app.click(*app.center(discard))
        confirm = None

        def discard_prompt() -> bool:
            nonlocal confirm
            confirm = next((e for e in app.elements("button", prefix="button:prompt/")
                            if e.get("label", "").startswith("Discard")), None)
            return confirm is not None
        if app.wait_until(discard_prompt, 3.0, "Discard All to ask first"):
            app.click(*app.center(confirm))
        app.wait_until(lambda: "Scheduler" in read(app, "src/scheduler.cpp")
                       and "Scheduler" in read(app, "include/scheduler.hpp"), 5.0, "Discard All to restore")
        app.wait_until(lambda: not git(app, "status", "--porcelain", "--", "src", "include"), 5.0,
                       "a clean tree")


def flow_journey_layout(app: q.App) -> None:
    """An editor layout built up and taken down: three files in two groups and a
    split down, a terminal split, Settings opened over it all, the sidebar and
    panel toggled, Zen-like with both gone; then every group closed with the
    keyboard until the welcome page, the panel and sidebar back -- every element
    inside its surface at each step (the oracles), the keyboard always somewhere."""
    for name in ("a", "b", "c"):
        open_file(app, f"layout/{name}.txt", f"{name}\n" * 30)
    app.key("ctrl+backslash")
    step(app, "split right", 0.3)
    app.send("split-down")
    step(app, "split down", 0.3)
    groups = app.editor_state()["groups"]
    app.expect(len(groups) == 3, f"three groups ({len(groups)})")
    app.send("term")
    app.send("term-split")
    step(app, "terminal split", 0.4)
    app.key("ctrl+comma")
    step(app, "settings over the layout", 0.4)
    app.expect(app.ui().get("modal") == "settings", "Settings opens as a modal over the layout")
    app.key("Escape")
    step(app, "close settings", 0.3)
    # From the editor: a focused shell takes Ctrl+B (backward-char), as it should.
    app.send("focus", "editor")
    app.key("ctrl+b")
    app.key("ctrl+j")
    step(app, "sidebar and panel away", 0.4)
    ui = app.ui()
    app.expect(not ui["sidebar"]["visible"] and not ui["panel"]["visible"], "both hidden")
    app.expect(ui["focus"] == "editor", f"the keyboard is in the editor with the rest hidden ({ui['focus']})")
    for _ in range(6):
        app.key("ctrl+w")
        step(app, "close", 0.2)
    app.wait_until(lambda: not titles(app), 5.0, "every editor to close")
    groups = app.editor_state()["groups"]
    app.expect(len(groups) == 1, f"closing the last editor of a group closes the group ({len(groups)} left)")
    app.key("ctrl+b")
    app.key("ctrl+j")
    step(app, "sidebar and panel back", 0.4)
    ui = app.ui()
    app.expect(ui["sidebar"]["visible"] and ui["panel"]["visible"], "both back")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "journey-feature-branch": flow_journey_feature_branch,
    "journey-rename-refactor": flow_journey_rename_refactor,
    "journey-layout": flow_journey_layout,
}
