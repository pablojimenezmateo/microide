"""Workbench flows: quick open, the command palette, the file tree, tabs, editor
groups, the search and git views, Settings, the status bar, the terminal and
window layouts -- each a whole task driven through the real window and checked
against what the app reports (`ui`, `editor`, `buffer`, `terminals`, the files
and the git repository on disk). VS Code's behaviour is the expectation.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import active_title, dismiss, expect_text, git, open_file, read, step, titles, write


def flow_quick_open(app: q.App) -> None:
    """Ctrl+P by keyboard and by mouse; Escape keeps the editor as it was."""
    app.send("focus", "editor")
    app.key("ctrl+p")
    step(app, "ctrl+p", 0.3)
    app.expect(app.ui()["overlay"]["mode"] == "file-finder", "ctrl+p opens Go to File")
    app.type("main")
    step(app, "type main", 0.3)
    app.expect(app.ui()["overlay"]["query"] == "main", "the query reaches the finder")
    app.key("Return")
    step(app, "Enter", 0.3)
    app.wait_until(lambda: active_title(app) == "main.cpp", 3.0, "Enter to open main.cpp")
    app.expect(not app.ui()["overlay"]["visible"], "Enter closes the finder")
    app.expect(app.ui()["focus"] == "editor", "and the editor has the keyboard")
    app.key("ctrl+p")
    app.type("sched")
    step(app, "ctrl+p sched", 0.3)
    rows = app.elements("overlay-row")
    app.expect(len(rows) == 2, f"two files match sched ({[r['name'] for r in rows]})")
    target = next((r for r in rows if r["label"].endswith("scheduler.hpp")), None)
    if target is not None:
        app.click(*app.center(target))
        step(app, "click scheduler.hpp", 0.3)
        app.wait_until(lambda: active_title(app) == "scheduler.hpp", 3.0,
                       "a click on a finder row to open it")
    app.key("ctrl+p")
    app.type("zzzz-nothing")
    step(app, "query with no match", 0.3)
    app.expect(app.ui()["overlay"]["count"] == 0, "a query matching nothing lists nothing")
    app.key("Return")
    step(app, "Enter on nothing")
    app.key("Escape")
    step(app, "Escape")
    app.expect(not app.ui()["overlay"]["visible"], "Escape closes the finder")
    app.expect(active_title(app) == "scheduler.hpp", "and leaves the editor where it was")


def flow_command_palette(app: q.App) -> None:
    """Run commands from the palette by Enter and by click; check each took effect."""
    open_file(app, "src/main.cpp")
    app.key("ctrl+shift+p")
    step(app, "ctrl+shift+p", 0.3)
    app.expect(app.ui()["overlay"]["mode"] == "command-palette", "ctrl+shift+p opens the palette")
    app.type("toggle status bar")
    step(app, "type toggle status bar", 0.3)
    app.key("Return")
    step(app, "Enter", 0.3)
    app.expect(app.ui()["surfaces"]["statusBar"] is None, "Toggle Status Bar hides the status bar")
    app.key("ctrl+shift+p")
    app.type("status bar")
    step(app, "palette again", 0.3)
    row = next((r for r in app.elements("overlay-row") if "Status Bar" in r["label"]), None)
    app.expect(row is not None, "the palette lists Toggle Status Bar")
    if row:
        app.click(*app.center(row))
        step(app, "click Toggle Status Bar", 0.3)
        app.expect(app.ui()["surfaces"]["statusBar"] is not None, "clicking it shows the bar again")
    app.key("ctrl+shift+p")
    app.type("word wrap")
    app.key("Return")
    step(app, "Word Wrap", 0.3)
    app.expect(app.buffer()["softWrap"], "Word Wrap from the palette wraps the editor")
    app.key("ctrl+shift+p")
    app.type("go to line")
    app.key("Return")
    step(app, "Go to Line from the palette", 0.3)
    app.expect(app.ui().get("modal") == "prompt", "Go to Line opens its prompt from the palette")
    app.type("7")
    app.key("Return")
    step(app, "7 Enter", 0.3)
    app.expect(app.cursors()[0][0] == 7, f"the palette's Go to Line jumps ({app.cursors()})")


def flow_tree(app: q.App) -> None:
    """Expand and collapse by click and keys, open by click, create, rename and
    delete through the context menu: the disk, the tree and the tabs agree."""
    app.click_named("sidebar-view:tree")
    step(app, "tree view", 0.3)
    app.click_named("row:tree/include")
    step(app, "click include", 0.3)
    app.expect(app.wait_for("row:tree/scheduler.hpp", 3.0) is not None,
               "clicking a folder expands it")
    app.click_named("row:tree/include")
    step(app, "click include again", 0.3)
    app.wait_until(lambda: not any(e["name"] == "row:tree/scheduler.hpp" for e in app.elements("row")),
                   3.0, "a second click to collapse it")
    if not any(e["name"] == "row:tree/main.cpp" for e in app.elements("row")):
        app.click_named("row:tree/src")
        step(app, "expand src", 0.3)
    app.click_named("row:tree/main.cpp")
    step(app, "click main.cpp", 0.4)
    app.wait_until(lambda: active_title(app) == "main.cpp", 3.0, "a click on a file to open it")
    # Keyboard: Down/Up move the selection, Enter opens, Left collapses.
    # The src folder is expanded now (main.cpp was opened from it); select it with
    # the keyboard, since a click on a folder row also toggles it.
    app.send("focus", "sidebar")
    app.key("Up")
    step(app, "select src", 0.2)
    app.expect([e["name"] for e in app.elements("row") if e.get("selected")] == ["row:tree/src"],
               "Up from main.cpp selects src")
    app.key("Left")
    step(app, "Left on src", 0.2)
    app.expect(not any(e["name"] == "row:tree/main.cpp" for e in app.elements("row")),
               "Left collapses an expanded folder")
    app.key("Right")
    step(app, "Right on src", 0.3)
    app.expect(app.wait_for("row:tree/main.cpp", 2.0) is not None, "Right expands it again")
    app.key("Down")
    step(app, "Down", 0.2)
    selected = [e["name"] for e in app.elements("row") if e.get("selected")]
    app.expect(selected == ["row:tree/main.cpp"], f"Down selects the first child ({selected})")
    app.key("Down")
    app.key("Return")
    step(app, "Down Enter", 0.4)
    app.wait_until(lambda: active_title(app) == "scheduler.cpp", 3.0, "Enter to open the selection")

    # New File through the src folder's context menu.
    src = app.element("row:tree/src")
    app.click(*app.center(src), button=3)
    step(app, "right-click src", 0.3)
    app.expect(app.ui()["menu"]["contextMenu"], "right-click opens the context menu")
    new_file = next((e for e in app.elements("menu-item", prefix="menu-item:context/")
                     if e["label"].startswith("New File")), None)
    app.expect(new_file is not None, "a folder's context menu offers New File")
    if new_file is None:
        dismiss(app)
        return
    app.click(*app.center(new_file))
    step(app, "click New File", 0.3)
    app.expect(app.ui().get("modal") == "prompt", "New File asks for a name")
    app.type("added.cpp")
    app.key("Return")
    step(app, "name added.cpp", 0.5)
    assert app.project is not None
    app.wait_until(lambda: (app.project / "src" / "added.cpp").exists(), 3.0, "the new file on disk")
    app.wait_until(lambda: app.wait_for("row:tree/added.cpp", 0.2) is not None or True, 1.0, "tree")
    app.expect(app.wait_for("row:tree/added.cpp", 3.0) is not None, "the tree shows the new file")
    app.wait_until(lambda: active_title(app) == "added.cpp", 3.0, "the new file to open (VS Code)")

    # Rename it.
    row = app.wait_for("row:tree/added.cpp", 2.0)
    if row:
        app.click(*app.center(row), button=3)
        step(app, "right-click added.cpp", 0.3)
        rename = next((e for e in app.elements("menu-item", prefix="menu-item:context/")
                       if e["label"].startswith("Rename")), None)
        app.expect(rename is not None, "a file's context menu offers Rename")
        if rename:
            app.click(*app.center(rename))
            step(app, "click Rename", 0.3)
            field = app.ui().get("prompt") or {}
            app.expect(field.get("kind") == "input", "Rename asks for the new name")
            app.expect(field.get("input") == "added.cpp", f"Rename starts from the name ({field.get('input')!r})")
            # The new path is relative to the file's folder (VS Code's rename).
            app.key("ctrl+a")
            app.type("renamed.cpp")
            app.key("Return")
            step(app, "rename to renamed.cpp", 0.5)
            app.wait_until(lambda: (app.project / "src" / "renamed.cpp").exists()
                           and not (app.project / "src" / "added.cpp").exists(), 3.0,
                           "the rename on disk")
            app.wait_until(lambda: "renamed.cpp" in titles(app), 3.0,
                           "the open tab to follow the rename")
    # Delete it.
    row = app.wait_for("row:tree/renamed.cpp", 3.0)
    if row:
        app.click(*app.center(row), button=3)
        step(app, "right-click renamed.cpp", 0.3)
        delete = next((e for e in app.elements("menu-item", prefix="menu-item:context/")
                       if e["label"].startswith("Delete")), None)
        if delete:
            app.click(*app.center(delete))
            step(app, "click Delete", 0.3)
            app.expect(app.ui().get("modal") == "prompt", "Delete confirms first")
            confirm = next((e for e in app.elements("button", prefix="button:prompt/")
                            if e["label"] == "Delete"), None)
            if confirm:
                app.click(*app.center(confirm))
                step(app, "confirm Delete", 0.5)
            app.wait_until(lambda: not (app.project / "src" / "renamed.cpp").exists(), 3.0,
                           "the delete on disk")
            app.wait_until(lambda: "renamed.cpp" not in titles(app), 3.0,
                           "the deleted file's tab to close")
            app.wait_until(lambda: not any(e["name"] == "row:tree/renamed.cpp" for e in app.elements("row")),
                           3.0, "the tree to drop the deleted file")
    dismiss(app)


def flow_tabs(app: q.App) -> None:
    """Activate by click and chord, reorder, close by button, middle click and Ctrl+W."""
    for name in ("README.md", "src/main.cpp", "src/scheduler.cpp", "include/scheduler.hpp"):
        open_file(app, name)
    opened = titles(app, 0)
    app.expect(opened[-3:] == ["main.cpp", "scheduler.cpp", "scheduler.hpp"],
               f"tabs open in order ({opened})")
    app.click_named("tab:0/main.cpp")
    step(app, "click the main.cpp tab", 0.2)
    app.expect(active_title(app) == "main.cpp", "clicking a tab activates it")
    app.key("ctrl+Next")
    step(app, "ctrl+PageDown", 0.2)
    app.expect(active_title(app) == "scheduler.cpp", f"ctrl+PageDown goes right ({active_title(app)})")
    app.key("ctrl+Prior")
    step(app, "ctrl+PageUp", 0.2)
    app.expect(active_title(app) == "main.cpp", "ctrl+PageUp goes back")
    app.key("ctrl+shift+Next")
    step(app, "ctrl+shift+PageDown", 0.2)
    opened = titles(app, 0)
    app.expect(opened.index("main.cpp") == opened.index("scheduler.cpp") + 1,
               f"ctrl+shift+PageDown moves the tab right ({opened})")
    # Drag a tab onto the first slot.
    first = app.elements("tab")[0]
    hpp = app.element("tab:0/scheduler.hpp")
    app.drag(*app.center(hpp), first["rect"][0] + 6, first["rect"][1] + first["rect"][3] / 2)
    step(app, "drag scheduler.hpp to the front", 0.3)
    app.expect(titles(app, 0)[0] == "scheduler.hpp", f"dragging a tab reorders it ({titles(app, 0)})")
    # Close: the button, a middle click, Ctrl+W.
    app.click_named("button:close-tab/0/README.md")
    step(app, "click README's close button", 0.3)
    app.expect("README.md" not in titles(app), "the close button closes the tab")
    app.click_named("tab:0/scheduler.cpp", button=2)
    step(app, "middle-click scheduler.cpp", 0.3)
    app.expect("scheduler.cpp" not in titles(app), "a middle click closes the tab")
    app.click_named("tab:0/main.cpp")
    app.key("ctrl+w")
    step(app, "ctrl+w", 0.3)
    app.expect(titles(app) == ["scheduler.hpp"], f"ctrl+w closes the active tab ({titles(app)})")
    app.key("ctrl+w")
    step(app, "ctrl+w the last tab", 0.3)
    app.expect(titles(app) == [], "closing the last tab leaves none")
    app.expect(app.buffer().get("available") is False, "and no buffer")


def flow_editor_groups(app: q.App) -> None:
    """Split, type in both panes of one file, focus by click, drag a tab across,
    close a group."""
    open_file(app, "notes/shared.txt", "shared\n")
    app.key("ctrl+backslash")
    step(app, "ctrl+\\", 0.4)
    state = app.editor_state()
    app.expect(len(state["groups"]) == 2, f"ctrl+\\ splits into two groups ({len(state['groups'])})")
    app.expect(state["focusedGroupIndex"] == 1, "the new group takes focus")
    app.click_text(1, 1, group=1)
    app.type("right ")
    step(app, "type in the right pane", 0.2)
    app.click_text(1, 1, group=0)
    step(app, "click the left pane", 0.2)
    app.expect(app.editor_state()["focusedGroupIndex"] == 0, "clicking a pane focuses its group")
    expect_text(app, "right shared\n", "both panes show one buffer (VS Code's model per file)")
    app.type("left ")
    step(app, "type in the left pane", 0.2)
    app.key("ctrl+s")
    step(app, "save from the left pane", 0.4)
    app.wait_until(lambda: read(app, "notes/shared.txt") == "left right shared\n", 3.0,
                   "one save to write the shared buffer")
    left_rows = app.pane(1)["rows"]
    app.expect(bool(left_rows), "the right pane still shows the file")
    open_file(app, "src/main.cpp")
    tab = app.element("tab:0/main.cpp")
    right_pane = app.pane(1)
    x, y, w, h = right_pane["rect"]
    app.drag(*app.center(tab), x + w / 2, y + h / 2)
    step(app, "drag main.cpp into the right group", 0.4)
    app.expect("main.cpp" in titles(app, 1) and "main.cpp" not in titles(app, 0),
               f"dragging a tab into a pane moves it there ({titles(app, 0)} | {titles(app, 1)})")
    app.send("close-group")
    step(app, "close-group", 0.4)
    app.expect(len(app.editor_state()["groups"]) == 1, "close-group leaves one group")
    app.send("split-down")
    step(app, "split-down", 0.4)
    split = app.editor_state()["split"]
    app.expect(split[0].get("orientation") == "horizontal" or any(n.get("orientation") == "horizontal" for n in split),
               f"split-down stacks the groups ({split})")
    divider = next((e for e in app.elements("divider") if e["name"].startswith("divider:editor/")), None)
    if divider:
        x, y = app.center(divider)
        before = app.pane(0)["rect"][3]
        app.drag(x, y, x, y - 80)
        step(app, "drag the group divider", 0.3)
        app.expect(app.pane(0)["rect"][3] < before, "dragging the divider resizes the groups")


def flow_project_search(app: q.App) -> None:
    """Search by typing in the view, open a hit by click, replace all with confirm."""
    app.key("ctrl+shift+f")
    step(app, "ctrl+shift+f", 0.3)
    app.expect(app.ui()["sidebar"]["view"] == "search", "ctrl+shift+f shows the search view")
    app.type("priority")
    step(app, "type priority", 0.6)
    app.wait_until(lambda: any(e["name"].startswith("row:search/src/scheduler.cpp:")
                               for e in app.elements("row")), 5.0, "results to list scheduler.cpp")
    hit = next((e for e in app.elements("row") if e["name"].startswith("row:search/src/scheduler.cpp:")),
               None)
    if hit:
        line = int(hit["label"].rsplit(":", 1)[1])
        app.click(*app.center(hit))
        step(app, "click a result", 0.4)
        app.wait_until(lambda: active_title(app) == "scheduler.cpp", 3.0, "the hit's file to open")
        app.expect(app.cursors()[0][0] == line, f"the caret lands on the hit's line {line} ({app.cursors()})")
    app.click_named("field:search/replace")
    app.type("rank")
    step(app, "type the replacement", 0.3)
    app.expect(app.field_text("field:search/replace") == "rank", "the replacement lands in its field")
    app.click_named("button:search/replace-all")
    step(app, "click Replace All", 0.3)
    prompt = app.ui().get("prompt") or {}
    app.expect(app.ui().get("modal") == "prompt" and "rank" in prompt.get("message", ""),
               f"Replace All asks first, naming the replacement ({prompt.get('message')})")
    confirm = next((e for e in app.elements("button", prefix="button:prompt/")
                    if e["label"] == "Replace All"), None)
    if confirm:
        app.click(*app.center(confirm))
        step(app, "confirm", 0.6)
        app.wait_until(lambda: "priority" not in read(app, "src/scheduler.cpp"), 5.0,
                       "the replacement to reach the file on disk")
        app.wait_until(lambda: "priority" not in app.text(), 5.0,
                       "the open (clean) editor to reload the replaced file")
    dismiss(app)


def flow_git(app: q.App) -> None:
    """Change a file, see it in the git view, open its diff, stage it, commit it
    from the commit panel: the repository agrees at every step."""
    write(app, "src/main.cpp", read(app, "src/main.cpp") + "// qa change\n")
    app.key("ctrl+shift+g")
    step(app, "ctrl+shift+g", 0.4)
    app.expect(app.ui()["sidebar"]["view"] == "git", "ctrl+shift+g shows the git view")
    row = app.wait_for("row:git/main.cpp", 5.0)
    app.expect(row is not None, "the changed file appears under the git view")
    if not row:
        return
    app.click(*app.center(row))
    step(app, "click the changed file", 0.5)
    tab = app.active_tab() or {}
    app.expect(tab.get("kind") == "compare", f"clicking a change opens its diff (tab {tab.get('kind')})")
    row = app.element("row:git/main.cpp")
    app.click(*app.center(row), button=3)
    step(app, "right-click the change", 0.3)
    stage = next((e for e in app.elements("menu-item", prefix="menu-item:context/")
                  if e["label"].startswith("Stage")), None)
    app.expect(stage is not None, "a change's context menu offers Stage")
    if stage:
        app.click(*app.center(stage))
        step(app, "Stage", 0.5)
    app.wait_until(lambda: git(app, "diff", "--cached", "--name-only").strip() == "src/main.cpp",
                   5.0, "the file to be staged in the index")
    app.wait_until(lambda: any(e["name"].startswith("row:git/Staged") for e in app.elements("row")),
                   5.0, "the git view to show a Staged group")
    app.click_named("button:git/commit")
    step(app, "open the commit panel", 0.4)
    subject = app.wait_for("field:git/subject", 3.0)
    app.expect(subject is not None, "Commit opens the commit message panel")
    if subject:
        app.click(*app.center(subject))
        app.type("qa: commit from the git view")
        step(app, "type the subject", 0.3)
        app.expect(app.field_text("field:git/subject") == "qa: commit from the git view",
                   "the subject lands in its field")
        app.key("ctrl+Return")
        step(app, "ctrl+Enter commits", 0.8)
        app.wait_until(lambda: git(app, "log", "-1", "--format=%s").strip() == "qa: commit from the git view",
                       6.0, "the commit to land in the repository")
        app.wait_until(lambda: not any(e["name"] == "row:git/main.cpp" for e in app.elements("row")),
                       5.0, "the committed file to leave the git view")
        app.wait_until(lambda: "[clean]" in app.element("status:project").get("label", ""), 5.0,
                       "the status bar to say the tree is clean")


def flow_settings(app: q.App) -> None:
    """Open Settings, filter, toggle a checkbox and step a number by click; the
    editor follows and the change persists in the user config."""
    open_file(app, "src/main.cpp")
    app.key("ctrl+comma")
    step(app, "ctrl+,", 0.4)
    app.expect(app.ui().get("modal") == "settings", "ctrl+, opens Settings")
    app.type("word wrap")
    step(app, "filter word wrap", 0.4)
    app.expect(app.field_text("field:settings/filter") == "word wrap", "typing filters Settings")
    rows = app.elements("setting")
    app.expect(any(r["name"] == "setting:editor.wrap" for r in rows),
               f"the filter keeps Word Wrap ({[r['name'] for r in rows][:6]})")
    app.expect(len(rows) < 8, f"and drops most rows ({len(rows)})")
    before = app.element("setting:editor.wrap").get("label")
    # A boolean is a checkbox or an on/off value control, whichever the row draws.
    control = next((e["name"] for e in app.elements("button", prefix="button:setting/editor.wrap/")
                    if e["name"].endswith(("/checkbox", "/value"))), None)
    app.expect(control is not None, "Word Wrap has a control to click")
    if control:
        app.click_named(control)
    step(app, "click the Word Wrap control", 0.3)
    after = app.element("setting:editor.wrap").get("label")
    app.expect(before != after, f"clicking the control toggles Word Wrap ({before} -> {after})")
    # The click moved the keyboard to the rows (where Space steps the selected
    # setting); back to the filter box, as a person would, before typing.
    app.click_named("field:settings/filter")
    app.key("ctrl+a")
    app.type("tab size")
    step(app, "filter tab size", 0.4)
    app.expect(app.field_text("field:settings/filter") == "tab size",
               f"ctrl+a and typing replace the filter ({app.field_text('field:settings/filter')!r})")
    tab_row = next((r for r in app.elements("setting") if "tab_size" in r["name"]), None)
    if tab_row:
        name = tab_row["name"].split(":", 1)[1]
        value = tab_row.get("label")
        try:
            app.click_named(f"button:setting/{name}/increment")
            step(app, "increment tab size", 0.3)
            app.expect(app.element(tab_row["name"]).get("label") != value,
                       f"the increment button steps the value ({value})")
        except q.QaFailure as failure:
            app.expect(False, f"no increment control on {name}: {failure}")
    app.key("Escape")
    step(app, "Escape Settings", 0.3)
    app.expect(app.ui().get("modal") is None, "Escape closes Settings")
    app.send("focus", "editor")
    wrapped = app.buffer()["softWrap"]
    app.expect(wrapped == (after not in ("off", "false")),
               f"the editor follows the Word Wrap setting ({after}, softWrap {wrapped}, "
               f"tab size now {app.buffer()['tabSize']})")
    # Word Wrap is project-scoped: it persists in the project's state, not the user config.
    persisted = " ".join(p.read_text(errors="replace") for p in app.work.rglob("config")
                         if p.is_file())
    app.expect("editor.wrap" in persisted, "the change persists in a settings file")


def flow_status_bar(app: q.App) -> None:
    """Every clickable status segment does something visible, and Escape undoes it."""
    open_file(app, "src/main.cpp")
    for segment in [e for e in app.elements("status") if e.get("enabled")]:
        before = app.ui()
        app.click(*app.center(segment))
        step(app, f"click {segment['name']}", 0.4)
        after = app.ui()
        changed = (after.get("modal") != before.get("modal")
                   or after["overlay"]["visible"] != before["overlay"]["visible"]
                   or after["sidebar"] != before["sidebar"]
                   or after["menu"]["open"] != before["menu"]["open"]
                   or after["panel"] != before["panel"])
        app.expect(changed, f"clicking {segment['name']} ({segment.get('label')!r}) does something")
        dismiss(app)


def flow_terminal(app: q.App) -> None:
    """Run commands and read their output, interrupt one, split, add and switch
    tabs by click, close them."""
    if not app.ui()["panel"]["visible"]:
        app.key("ctrl+grave")
        step(app, "ctrl+`", 0.5)
    app.send("focus", "panel")
    step(app, "focus the terminal", 0.3)

    def output() -> str:
        return app.query("terminal-output").get("text", "")

    app.type("echo qa-$((40+2))\n")
    app.wait_until(lambda: "qa-42" in output(), 5.0, "the shell to run the typed command")
    app.type("sleep 30\n")
    step(app, "start sleep", 0.5)
    app.key("ctrl+c")
    step(app, "ctrl+c", 0.4)
    app.type("echo after-interrupt\n")
    app.wait_until(lambda: "after-interrupt" in output(), 5.0, "ctrl+c to interrupt and the shell to answer")
    terminals = app.query("terminals")
    count = len(terminals) if isinstance(terminals, list) else len(terminals.get("tabs", []))
    app.click_named("button:panel/new-terminal")
    step(app, "click +", 0.6)
    after = app.query("terminals")
    count_after = len(after) if isinstance(after, list) else len(after.get("tabs", []))
    app.expect(count_after == count + 1, f"+ opens a terminal tab ({count} -> {count_after})")
    tabs = app.elements("panel-tab")
    app.expect(len(tabs) >= 2, f"the panel shows both tabs ({[t['name'] for t in tabs]})")
    if len(tabs) >= 2:
        app.click(*app.center(tabs[0]))
        step(app, "click the first terminal tab", 0.3)
        app.expect(app.element(tabs[0]["name"]).get("active") is True, "clicking a panel tab activates it")
        app.wait_until(lambda: "qa-42" in output(), 3.0, "the first tab's scrollback to be back")
    app.key("ctrl+shift+backslash")
    step(app, "split the terminal", 0.6)
    panes = app.query("terminals")
    tab_rows = panes if isinstance(panes, list) else panes.get("tabs", [])
    active = next((t for t in tab_rows if t.get("active")), {})
    app.expect(len(active.get("panes", [])) == 2, f"ctrl+shift+\\ splits the terminal ({active})")
    surface = app.ui()["surfaces"]["bottomPanel"]
    if surface:
        x, y, w, h = surface
        app.wheel(x + w / 4, y + h / 2, -3)
        step(app, "wheel the terminal", 0.2)
    app.click_named("button:panel/maximize")
    step(app, "maximize", 0.4)
    app.expect(app.ui()["panel"]["maximized"], "the maximize button maximizes the panel")
    app.click_named("button:panel/maximize")
    step(app, "restore", 0.4)
    app.expect(not app.ui()["panel"]["maximized"], "and restores it")
    app.type("exit\n")
    step(app, "exit one pane", 0.8)
    app.key("ctrl+grave")
    step(app, "toggle the panel off", 0.4)
    app.expect(not app.ui()["panel"]["visible"], "ctrl+` hides the panel")
    app.key("ctrl+grave")
    step(app, "toggle the panel on", 0.4)
    app.expect(app.ui()["panel"]["visible"], "ctrl+` shows it again")


def _resize(app: q.App, width: int, height: int) -> None:
    if app.window:
        app._xdotool("windowsize", app.window, str(width), str(height))
    app.wait_until(lambda: app.ui()["window"]["width"] == width, 3.0, f"the window to be {width} wide")
    step(app, f"resize to {width}x{height}", 0.4)


def _layout_sane(app: q.App, label: str) -> None:
    """Every element on screen is inside the window, has an area, and siblings of
    one strip (tabs, menu items, status segments) do not overlap."""
    ui = app.ui()
    width, height = ui["window"]["width"], ui["window"]["height"]
    by_kind: dict[str, list[dict]] = {}
    for element in ui["elements"]:
        x, y, w, h = element["rect"]
        app.expect(w > 0 and h > 0, f"{label}: {element['name']} has no area ({element['rect']})")
        app.expect(x >= -1 and y >= -1 and x + w <= width + 1 and y + h <= height + 1,
                   f"{label}: {element['name']} {element['rect']} leaves the {width}x{height} window")
        by_kind.setdefault(element["kind"], []).append(element)
    for kind in ("tab", "menu", "status", "panel-tab", "project-tab", "sidebar-view"):
        rects = by_kind.get(kind, [])
        for i, a in enumerate(rects):
            for b in rects[i + 1:]:
                ax, ay, aw, ah = a["rect"]
                bx, by, bw, bh = b["rect"]
                if ax < bx + bw - 1 and bx < ax + aw - 1 and ay < by + bh - 1 and by < ay + ah - 1:
                    app.expect(False, f"{label}: {a['name']} {a['rect']} overlaps {b['name']} {b['rect']}")


def flow_layouts(app: q.App) -> None:
    """Resize the window through narrow and wide sizes, with tabs, a split and the
    panel open: nothing leaves the window or overlaps a sibling, and the menu bar
    overflows into a menu rather than off the edge."""
    for name in ("README.md", "src/main.cpp", "src/scheduler.cpp", "include/scheduler.hpp"):
        open_file(app, name)
    app.send("split-right")
    step(app, "split-right", 0.3)
    for width, height in ((1440, 900), (1024, 700), (800, 600), (640, 480), (1920, 1080), (1440, 900)):
        _resize(app, width, height)
        _layout_sane(app, f"{width}x{height}")
        menus = app.elements("menu")
        more = [e for e in app.elements("button") if "more" in e["name"]]
        app.expect(menus or more, f"{width}x{height}: the menu bar is still reachable")
        # A click on each menu-bar item still opens its menu at this size.
        for menu in menus[:3]:
            app.click(*app.center(menu))
            step(app, f"{width}: open {menu['name']}", 0.2)
            app.expect(app.ui()["menu"]["open"], f"{width}x{height}: {menu['name']} opens")
            _layout_sane(app, f"{width}x{height} with {menu['name']} open")
            app.key("Escape")
            step(app, "close", 0.15)


def flow_zoom_and_chrome_toggles(app: q.App) -> None:
    """Zoom in and out, toggle the sidebar, the panel and the status bar: the
    layout stays sane and each toggle round-trips."""
    open_file(app, "src/main.cpp")
    for chord in ("ctrl+equal", "ctrl+equal", "ctrl+minus", "ctrl+0"):
        app.key(chord)
        step(app, chord, 0.4)
        _layout_sane(app, chord)
    for chord, key in (("ctrl+b", "sidebar"), ("ctrl+j", "panel")):
        before = app.ui()[key]["visible"]
        app.key(chord)
        step(app, chord, 0.3)
        app.expect(app.ui()[key]["visible"] != before, f"{chord} toggles the {key}")
        app.key(chord)
        step(app, f"{chord} again", 0.3)
        app.expect(app.ui()[key]["visible"] == before, f"{chord} again restores the {key}")
    app.send("status-bar-toggle")
    step(app, "status-bar-toggle", 0.3)
    app.expect(app.ui()["surfaces"]["statusBar"] is None, "status-bar-toggle hides the status bar")
    app.send("status-bar-toggle")
    step(app, "status-bar-toggle again", 0.3)


def flow_help(app: q.App) -> None:
    """Help/About and the keyboard shortcuts open as a modal and close on Escape
    and on a click outside."""
    app.click_named("menu:Help")
    step(app, "open Help", 0.3)
    rows = app.elements("menu-item", prefix="menu-item:Help/")
    about = next((r for r in rows if "About" in r["label"] or r["label"] == "Help"), None)
    if about:
        app.click(*app.center(about))
        step(app, "click About", 0.4)
        app.expect(app.ui().get("modal") == "help", f"{about['label']} opens Help/About")
        app.key("Escape")
        step(app, "Escape", 0.3)
        app.expect(app.ui().get("modal") is None, "Escape closes it")
    app.send("keyboard-shortcuts")
    step(app, "keyboard-shortcuts", 0.4)
    app.expect(app.ui().get("modal") in ("help", "settings"), "the shortcuts list opens")
    # Not near a window edge: those few pixels are the borderless window's resize
    # grips, which SDL takes before the app sees the press.
    app.click(app.ui()["window"]["width"] * 0.08, app.ui()["window"]["height"] * 0.5)
    step(app, "click outside", 0.3)
    app.expect(app.ui().get("modal") is None, "a click outside closes it")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "wb-quick-open": flow_quick_open,
    "wb-command-palette": flow_command_palette,
    "wb-tree": flow_tree,
    "wb-tabs": flow_tabs,
    "wb-editor-groups": flow_editor_groups,
    "wb-project-search": flow_project_search,
    "wb-git": flow_git,
    "wb-settings": flow_settings,
    "wb-status-bar": flow_status_bar,
    "wb-terminal": flow_terminal,
    "wb-layouts": flow_layouts,
    "wb-zoom-chrome": flow_zoom_and_chrome_toggles,
    "wb-help": flow_help,
}
