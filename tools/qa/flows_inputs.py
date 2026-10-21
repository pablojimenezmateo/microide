"""Ways in that the sweeps do not take: the desktop's file dialogs, the menus
and prompts by keyboard, a tab dragged, a file renamed or deleted under an open
editor, the terminal's word and line selection, and a `.editorconfig`.

A coverage build of the suite ran none of these (TD-2026-10-18-364): the dialog
coordinator at 9 %, the modal key handlers at 16 %, the single-line fields at
27 %, the tab retargeting on a rename at 27 %, `.editorconfig` at 25 %. The
native dialogs go through SDL's zenity driver with `tools/qa/fakebin/zenity`
first on PATH: the flow writes the answer the dialog would give, so the app's
own result handling -- a selection, a cancel, a failure -- runs for real.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Callable

import microide_qa as q
from flows_common import active_root, active_title, dismiss, expect_text, open_file, read, step, tree_rows, write

FAKEBIN = Path(__file__).resolve().parent / "fakebin"


# ---- native dialogs ---------------------------------------------------------

def _answer_dialog(app: q.App, verb: str, path: Path | str = "") -> None:
    """What the next dialog answers: `select <path>`, `cancel` or `fail`."""
    (app.work / "run" / "zenity-result").write_text(f"{verb} {path}\n")
    args = app.work / "run" / "zenity-args"
    if args.exists():
        args.unlink()


def _dialog_args(app: q.App) -> list[str]:
    args = app.work / "run" / "zenity-args"
    return args.read_text().splitlines() if args.exists() else []


def _menu_row(app: q.App, menu: str, row: str) -> None:
    app.click_named(f"menu:{menu}")
    app.settle(0.15)
    app.click_named(f"menu-item:{menu}/{row}")


def flow_native_dialogs(app: q.App) -> None:
    """File > Open Folder… and Open File… through the desktop dialog (zenity,
    faked): a folder opens as a project, a file from outside the project opens
    in a tab, Cancel and a failed dialog change nothing, the picker asks for the
    right thing (a directory, the project as the default location, a font
    filter), and with no dialog program at all the menu row falls back to the
    command palette seeded with the verb."""
    assert app.project is not None
    open_file(app, "src/scheduler.cpp")

    second = app.work / "second-project"
    (second / "src").mkdir(parents=True, exist_ok=True)
    (second / "src" / "lib.c").write_text("int lib(void) { return 1; }\n")
    projects_before = len(app.query("projects"))
    _answer_dialog(app, "select", second)
    _menu_row(app, "File", "Open Folder…")
    app.wait_until(lambda: active_root(app).endswith("second-project"), 10.0,
                   "the folder picked in the dialog to open as the project")
    step(app, "Open Folder… picks a folder", 0.5)
    app.expect(len(app.query("projects")) == projects_before + 1, "the picked folder is a project beside the first")
    args = _dialog_args(app)
    app.expect("--directory" in args, f"the folder picker asks for a directory ({args})")
    app.expect("--filename" in args and str(app.project) in " ".join(args),
               f"the folder picker opens at the current project ({args})")

    _answer_dialog(app, "cancel")
    count = len(app.query("projects"))
    _menu_row(app, "File", "Open Folder…")
    app.wait_until(lambda: bool(_dialog_args(app)), 5.0, "the dialog to run")
    step(app, "Open Folder… cancelled", 0.5)
    app.expect(len(app.query("projects")) == count and active_root(app).endswith("second-project"),
               "Cancel opens nothing and keeps the project")

    outside = app.work / "elsewhere.txt"
    outside.write_text("picked from outside\n")
    _answer_dialog(app, "select", outside)
    _menu_row(app, "File", "Open File…")
    app.wait_until(lambda: app.buffer().get("path", "").endswith("elsewhere.txt"), 8.0,
                   "the file picked in the dialog to open")
    step(app, "Open File… picks a file", 0.4)
    app.expect(app.buffer().get("text") == "picked from outside\n", "the picked file's text is the buffer")
    args = _dialog_args(app)
    app.expect("--directory" not in args, f"the file picker asks for a file ({args})")

    _answer_dialog(app, "fail")
    tabs = len(app.query("tabs"))
    _menu_row(app, "File", "Open File…")
    app.wait_until(lambda: bool(_dialog_args(app)), 5.0, "the dialog to run")
    step(app, "Open File… with the dialog failing", 0.5)
    app.expect(app.alive() and len(app.query("tabs")) == tabs, "a failed dialog opens nothing")
    # A second dialog after a failure must open: the failure released the slot.
    _answer_dialog(app, "select", outside)
    _menu_row(app, "File", "Open File…")
    app.wait_until(lambda: bool(_dialog_args(app)), 5.0, "the dialog to run again after a failure")
    step(app, "Open File… after a failure", 0.4)

    # The font row's "Choose file…" in Settings: a picker with a font filter whose
    # answer becomes the setting.
    font = Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")
    if font.exists():
        app.key("ctrl+comma")
        app.wait_for("field:settings/filter", 3.0)
        app.type("editor.font_family")
        app.settle(0.2)
        value = app.wait_for("button:setting/editor.font_family/value", 2.0)
        if value is not None:
            _answer_dialog(app, "select", font)
            app.click(*app.center(value))
            app.settle(0.3)
            choose = next((e for e in app.elements("overlay-row") if "choose" in e.get("label", "").lower()), None)
            if app.expect(choose is not None, "the font picker offers Choose file…"):
                app.click(*app.center(choose))
                app.wait_until(lambda: any(r["id"] == "editor.font_family" and r["value"] == str(font)
                                           for r in app.query("settings")), 8.0,
                               "the chosen font file to become the setting")
                step(app, "Choose file… picks a font", 0.5)
                args = _dialog_args(app)
                app.expect(any(a.startswith("--file-filter=") and "ttf" in a for a in args),
                           f"the font picker filters for font files ({args})")
                app.send("set-setting", "editor.font_family", "")
        app.key("Escape")
        step(app, "close Settings", 0.3)
    dismiss(app)

    # No dialog program at all: the menu row seeds the palette with the verb so
    # the path can be typed (the fallback the executor promises).
    (app.work / "run" / "zenity-absent").write_text("")
    app.restart()
    step(app, "relaunched without zenity", 0.5)
    _menu_row(app, "File", "Open Folder…")
    app.wait_until(lambda: app.ui()["overlay"]["visible"], 5.0, "the command palette as the fallback")
    step(app, "Open Folder… with no dialog program", 0.3)
    query = app.ui()["overlay"].get("query") or ""
    app.expect(query.startswith("project-open"), f"the palette is seeded with the verb ({query!r})")
    app.key("Escape")
    _menu_row(app, "File", "Open File…")
    app.wait_until(lambda: app.ui()["overlay"]["visible"], 5.0, "the command palette as the fallback")
    query = app.ui()["overlay"].get("query") or ""
    app.expect(query.startswith("open"), f"Open File… seeds the palette with its verb ({query!r})")
    app.key("Escape")
    step(app, "dismiss the palette", 0.2)


# ---- menus and prompts by keyboard ---------------------------------------------

def _active_row(app: q.App, menu: str) -> dict | None:
    return next((e for e in app.elements("menu-item", prefix=f"menu-item:{menu}/") if e.get("active")), None)


def flow_menu_keyboard(app: q.App) -> None:
    """An open menu driven by keys: Left/Right and Tab between menus, Down/Up,
    Home/End over the rows (skipping disabled ones), Enter running the row
    (receipt), Escape; the compact layout's overflow popup by keys; the tree's
    context menu by keys. Runs with the menu dry run so Enter records a row
    without quitting."""
    open_file(app, "src/scheduler.cpp")
    app.click_named("menu:File")
    step(app, "open File", 0.2)
    menus = [e["name"].split(":", 1)[1] for e in app.elements("menu")]
    app.key("Right")
    step(app, "Right", 0.15)
    app.expect(app.ui()["menu"]["menu"] == menus[1], f"Right moves to the next menu ({app.ui()['menu']['menu']})")
    app.key("Left")
    app.key("Left")
    step(app, "Left twice", 0.15)
    app.expect(app.ui()["menu"]["menu"] == menus[-1], f"Left from the first menu wraps to the last ({app.ui()['menu']['menu']})")
    app.key("Tab")
    step(app, "Tab", 0.15)
    app.expect(app.ui()["menu"]["menu"] == menus[0], f"Tab wraps forward to the first menu ({app.ui()['menu']['menu']})")
    app.key("shift+Tab")
    app.key("Tab")
    step(app, "Shift+Tab, Tab", 0.15)
    app.expect(app.ui()["menu"]["menu"] == menus[0], "Shift+Tab then Tab is back where it started")

    rows = [e for e in app.elements("menu-item", prefix="menu-item:File/")]
    enabled = [e["name"] for e in rows if e.get("enabled") is not False]
    start = _active_row(app, "File")
    app.key("Down")
    step(app, "Down", 0.15)
    active = _active_row(app, "File")
    want = enabled[0] if start is None else enabled[(enabled.index(start["name"]) + 1) % len(enabled)]
    app.expect(active is not None and active["name"] == want,
               f"Down selects the next enabled row ({active and active['name']}, want {want})")
    app.key("End")
    step(app, "End", 0.15)
    active = _active_row(app, "File")
    app.expect(active is not None and active["name"] == enabled[-1],
               f"End selects the last enabled row ({active and active['name']})")
    app.key("Up")
    step(app, "Up", 0.15)
    active = _active_row(app, "File")
    app.expect(active is not None and active["name"] == enabled[-2],
               f"Up steps to the previous enabled row ({active and active['name']})")
    app.key("Home")
    step(app, "Home", 0.15)
    active = _active_row(app, "File")
    app.expect(active is not None and active["name"] == enabled[0], "Home selects the first row again")
    disabled = [e["name"] for e in rows if e.get("enabled") is False]
    if disabled:
        seen = []
        for _ in range(len(rows)):
            app.key("Down")
            app.settle(0.05)
            active = _active_row(app, "File")
            seen.append(active["name"] if active else None)
        app.expect(not set(seen) & set(disabled), f"Down never lands on a disabled row ({disabled})")
        app.key("Home")
        app.settle(0.1)
    # Enter runs the selected row: the receipt names it and the menu closes.
    app.key("Down")
    app.settle(0.1)
    target = _active_row(app, "File")
    before = app.ui()["menu"]["rowsRun"]
    app.key("Return")
    step(app, "Enter", 0.2)
    menu = app.ui()["menu"]
    app.expect(target is not None and menu["rowsRun"] == before + 1
               and menu["lastRun"] == target["name"].split(":", 1)[1],
               f"Enter runs the selected row ({menu['lastRun']!r}, want {target and target['name']})")
    app.expect(not menu["open"], "and closes the menu")
    app.click_named("menu:Edit")
    app.key("Escape")
    step(app, "Escape", 0.15)
    app.expect(not app.ui()["menu"]["open"], "Escape closes the menu")

    # The compact layout: every menu behind the chevron, walked by keys.
    app.send("set-setting", "ui.layout_mode", "compact")
    step(app, "compact layout", 0.4)
    if app.wait_for("button:menu-bar/more", 3.0) is not None:
        app.click_named("button:menu-bar/more")
        step(app, "open the chevron", 0.2)
        app.expect(app.ui()["menu"]["overflow"], "the chevron opens the overflow popup")
        folded = [e["label"] for e in app.elements("menu-item", prefix="menu-item:more/")]
        app.key("Down")
        app.key("Down")
        step(app, "Down twice in the popup", 0.15)
        active = next((e for e in app.elements("menu-item", prefix="menu-item:more/") if e.get("active")), None)
        app.expect(active is not None and active["label"] == folded[1],
                   f"Down walks the popup's rows ({active and active['label']})")
        app.key("Return")
        step(app, "Enter in the popup", 0.2)
        app.expect(app.ui()["menu"]["menu"] == folded[1], f"Enter opens that menu ({app.ui()['menu']['menu']})")
        app.key("Escape")
        app.settle(0.1)
        app.click_named("button:menu-bar/more")
        app.key("Escape")
        step(app, "Escape from the popup", 0.15)
        app.expect(not app.ui()["menu"]["overflow"] and not app.ui()["menu"]["open"], "Escape closes the popup")
    app.send("set-setting", "ui.layout_mode", "auto")
    step(app, "regular layout", 0.4)

    # The tree's context menu by keys: Down/Up/Home/End over its rows, Enter runs.
    app.click_named("sidebar-view:tree")
    app.settle(0.2)
    row = app.element("row:tree/README.md")
    app.click(*app.center(row), button=3)
    step(app, "right-click README.md", 0.25)
    app.expect(app.ui()["menu"]["contextMenu"], "the right-click opens the context menu")
    rows = app.elements("menu-item", prefix="menu-item:context/")
    enabled = [e["name"] for e in rows if e.get("enabled") is not False]
    start = _active_row(app, "context")  # the pointer rests on the row it opened over
    app.key("Down")
    app.settle(0.1)
    active = _active_row(app, "context")
    want = enabled[0] if start is None else enabled[(enabled.index(start["name"]) + 1) % len(enabled)]
    app.expect(active is not None and active["name"] == want,
               f"Down selects the next context row ({active and active['name']}, want {want})")
    app.key("End")
    app.settle(0.1)
    active = _active_row(app, "context")
    app.expect(active is not None and active["name"] == enabled[-1], "End selects the last context row")
    app.key("Home")
    app.settle(0.1)
    dispatched = app.ui()["actions"]["dispatched"]
    app.key("Return")
    step(app, "Enter on the first context row", 0.3)
    actions = app.ui()["actions"]
    app.expect(actions["dispatched"] == dispatched + 1 and not app.ui()["menu"]["contextMenu"],
               f"Enter runs the row and closes the menu ({actions})")
    app.expect((app.active_tab() or {}).get("title") == "README.md", "the first row (Open) opened README.md")
    dismiss(app)


def _prompt_buttons(app: q.App) -> list[str]:
    return [e["name"].split("/", 1)[1] for e in app.elements("button", prefix="button:prompt/")]


def _prompt_selected(app: q.App) -> str | None:
    return next((e["name"].split("/", 1)[1] for e in app.elements("button", prefix="button:prompt/")
                 if e.get("active")), None)


def flow_prompts_keyboard(app: q.App) -> None:
    """The unsaved prompt by keys (S/D/C, arrows and Enter, Escape), a confirm
    prompt (Delete: Escape keeps the file, Enter deletes), and a text prompt's
    field with the single-line keys and the mouse: Home/End, word motion,
    Shift selection, Ctrl+A/C/X/V, Ctrl+Backspace, a click placing the caret, a
    double-click selecting a word, a triple-click selecting all, a drag."""
    assert app.project is not None
    open_file(app, "notes/prompt.txt", "kept\n")
    app.click_text(1, 1)
    app.key("ctrl+End")
    app.type("edit")
    step(app, "dirty the file", 0.2)

    def close_and_wait(label: str) -> bool:
        app.key("ctrl+w")
        return app.wait_until(lambda: app.ui().get("modal") == "unsaved-prompt", 3.0, f"the unsaved prompt ({label})")

    if close_and_wait("c"):
        app.key("c")
        step(app, "C", 0.3)
        app.expect(app.ui().get("modal") is None and (app.active_tab() or {}).get("title") == "prompt.txt"
                   and app.buffer().get("dirty"), "C cancels: the tab stays, still dirty")
    if close_and_wait("Escape"):
        app.key("Escape")
        step(app, "Escape", 0.3)
        app.expect(app.ui().get("modal") is None and app.buffer().get("dirty"), "Escape cancels too")
    if close_and_wait("arrows"):
        buttons = _prompt_buttons(app)
        first = _prompt_selected(app)
        app.key("Right")
        app.settle(0.1)
        app.expect(_prompt_selected(app) != first, f"Right moves the selection ({first} -> {_prompt_selected(app)})")
        app.key("Right")
        app.key("Right")
        app.settle(0.1)
        app.expect(_prompt_selected(app) == buttons[-1], f"Right stops at the last button ({_prompt_selected(app)})")
        app.key("Left")
        app.settle(0.1)
        discard = next((b for b in buttons if b.startswith("Discard")), None)
        app.expect(_prompt_selected(app) == discard, f"Left from the last selects {discard} ({_prompt_selected(app)})")
        app.key("Return")
        step(app, "Enter on Discard", 0.4)
        app.expect(app.ui().get("modal") is None and (app.active_tab() or {}).get("title") != "prompt.txt",
                   "Enter runs the selected button: the tab is closed")
        app.expect(read(app, "notes/prompt.txt") == "kept\n", "Discard left the disk alone")
    open_file(app, "notes/prompt.txt")
    app.click_text(1, 1)
    app.key("ctrl+End")
    app.type("saved")
    step(app, "dirty again", 0.2)
    wanted = app.text()
    if close_and_wait("s"):
        app.key("s")
        # The save adds the final newline the buffer lacks (editor.save.ensure_final_newline).
        app.wait_until(lambda: read(app, "notes/prompt.txt") in (wanted, wanted + "\n"), 3.0, "S to save the file")
        step(app, "S", 0.3)
        app.expect((app.active_tab() or {}).get("title") != "prompt.txt", "S saves and closes the tab")

    # A confirm prompt: Delete… on a tree row. Escape keeps the file; Enter on
    # the selected button deletes it.
    write(app, "notes/doomed.txt", "x\n")
    app.send("tree-refresh")
    app.click_named("sidebar-view:tree")
    app.wait_until(lambda: any(e["name"] == "row:tree/doomed.txt" for e in app.elements("row")), 5.0, "doomed.txt in the tree")
    app.click_named("row:tree/doomed.txt")
    app.key("Delete")
    if app.wait_until(lambda: app.ui().get("modal") == "prompt", 3.0, "the delete prompt"):
        step(app, "Delete asks", 0.2)
        app.expect("Delete" in _prompt_buttons(app), f"the prompt offers Delete ({_prompt_buttons(app)})")
        app.key("Escape")
        step(app, "Escape", 0.3)
        app.expect(app.ui().get("modal") is None and (app.project / "notes" / "doomed.txt").exists(),
                   "Escape keeps the file")
        app.click_named("row:tree/doomed.txt")
        app.key("Delete")
        app.wait_until(lambda: app.ui().get("modal") == "prompt", 3.0, "the delete prompt again")
        selected = _prompt_selected(app)
        if selected != "Delete":
            app.key("Left")
            app.settle(0.1)
        app.key("Return")
        app.wait_until(lambda: not (app.project / "notes" / "doomed.txt").exists(), 3.0, "Enter to delete the file")
        step(app, "Enter deletes", 0.3)
        app.expect(not (app.project / "notes" / "doomed.txt").exists(), "Enter on Delete deletes the file")

    # A text prompt (New File…) and its field.
    row = app.element("row:tree/notes")
    app.click(*app.center(row), button=3)
    app.settle(0.2)
    app.click_named("menu-item:context/New File…")
    if not app.wait_until(lambda: app.ui().get("modal") == "prompt"
                          and any(e["name"] == "field:prompt" for e in app.elements("field")),
                          3.0, "the New File prompt"):
        dismiss(app)
        return
    step(app, "New File…", 0.2)
    app.key("ctrl+a")
    app.key("BackSpace")

    def field() -> str:
        return app.field_text("field:prompt")

    def check(label: str, want: str) -> None:
        app.settle(0.08)
        app.expect(field() == want, f"{label}: the field reads {field()!r}, want {want!r}")

    app.type("alpha beta.txt")
    check("typed", "alpha beta.txt")
    app.key("Home")
    app.type("x")
    check("Home then a key", "xalpha beta.txt")
    app.key("End")
    app.type("y")
    check("End then a key", "xalpha beta.txty")
    app.key("ctrl+Left")
    app.type("_")
    check("Ctrl+Left to the word start (a dot ends a word, as in VS Code)", "xalpha beta._txty")
    app.key("ctrl+Right")
    app.type("+")
    check("Ctrl+Right to the word end", "xalpha beta._txty+")
    app.key("ctrl+BackSpace")
    check("Ctrl+Backspace deletes the word before (the + alone)", "xalpha beta._txty")
    app.key("ctrl+BackSpace")
    check("Ctrl+Backspace again takes the word (an underscore is part of it)", "xalpha beta.")
    app.key("Home")
    app.key("ctrl+Delete")
    check("Ctrl+Delete deletes the word after", " beta.")
    app.key("shift+End")
    app.type("new.txt")
    check("Shift+End selects to the end; typing replaces it", "new.txt")
    if not app.wayland:  # the clipboard needs a seat input serial (TD-2026-10-19-366)
        app.key("Home")
        app.key("shift+ctrl+Right")
        app.key("ctrl+c")
        app.key("End")
        app.key("ctrl+v")
        check("Ctrl+C a word selection, Ctrl+V at the end", "new.txtnew")
        app.key("shift+Home")
        app.key("ctrl+x")
        check("Ctrl+X cuts the selection", "")
        app.key("ctrl+v")
        check("Ctrl+V pastes it back", "new.txtnew")
    app.key("ctrl+a")
    app.type("one two three")
    check("Ctrl+A then typing replaces everything", "one two three")
    # The mouse: a click places the caret, a double-click selects the word, a
    # triple-click selects all, a drag selects a range.
    rect = app.element("field:prompt")["rect"]
    app.click(rect[0] + 4, rect[1] + rect[3] / 2)
    app.type("<")
    check("a click at the left edge puts the caret first", "<one two three")
    app.click(rect[0] + rect[2] - 4, rect[1] + rect[3] / 2)
    app.type(">")
    check("a click at the right edge puts the caret last", "<one two three>")
    app.key("ctrl+a")
    app.type("one two three")
    # The first word starts at the field's left padding: a double-click just
    # inside the edge is on it.
    app.click(rect[0] + 10, rect[1] + rect[3] / 2, clicks=2)
    app.type("1")
    check("a double-click selects the word under it and typing replaces it", "1 two three")
    app.click(rect[0] + rect[2] / 2, rect[1] + rect[3] / 2, clicks=3)
    app.type("all")
    check("a triple-click selects everything", "all")
    app.key("ctrl+a")
    app.type("drag me.txt")
    app.drag(rect[0] + 4, rect[1] + rect[3] / 2, rect[0] + rect[2] - 4, rect[1] + rect[3] / 2)
    app.type("dragged.txt")
    check("a drag across the field selects it all; typing replaces", "dragged.txt")
    app.key("Escape")
    step(app, "Escape from the prompt", 0.3)
    app.expect(app.ui().get("modal") is None and not (app.project / "notes" / "dragged.txt").exists(),
               "Escape creates nothing")
    dismiss(app)


# ---- tabs by mouse ----------------------------------------------------------------

def _order(app: q.App, group: int = 0) -> list[str]:
    return [t["title"] for t in sorted((t for t in app.query("tabs") if t.get("group") == group),
                                       key=lambda t: t.get("index", 0))]


def flow_tab_drag(app: q.App) -> None:
    """A tab dragged along its strip reorders it (the drop keyed on the tab's
    box, not the grab point); a press without travel is a click; a tab dropped
    on the editor's edge splits (VS Code's drag to split); a tab dragged onto the
    other group's strip moves there; the last tab leaving a group closes it."""
    # The fixture restores README.md; the strip starts with the three files alone.
    for _ in range(4):
        if not app.query("tabs"):
            break
        app.send("focus", "editor")
        app.key("ctrl+w")
        app.settle(0.2)
        dismiss(app)
    for name in ("a", "b", "c"):
        open_file(app, f"drag/{name}.txt", f"{name}\n")
    app.expect(_order(app) == ["a.txt", "b.txt", "c.txt"], f"three tabs in order ({_order(app)})")
    c = app.element("tab:0/c.txt")
    a = app.element("tab:0/a.txt")
    y = app.center(c)[1]
    app.drag(app.center(c)[0], y, a["rect"][0] + 4, y, steps=14)
    step(app, "drag c before a", 0.4)
    app.expect(_order(app) == ["c.txt", "a.txt", "b.txt"], f"the tab lands first ({_order(app)})")
    app.expect((app.active_tab() or {}).get("title") == "c.txt", "the dragged tab is the active one")
    b = app.element("tab:0/b.txt")
    app.drag(*app.center(b), app.element("tab:0/c.txt")["rect"][0] + 10, y, steps=14)
    step(app, "drag b to the front", 0.4)
    app.expect(_order(app) == ["b.txt", "c.txt", "a.txt"], f"and again ({_order(app)})")
    # A press with no travel is a click: it activates, it does not reorder.
    a = app.element("tab:0/a.txt")
    app.drag(*app.center(a), app.center(a)[0] + 2, y, steps=2)
    step(app, "a two-pixel press on a", 0.3)
    app.expect(_order(app) == ["b.txt", "c.txt", "a.txt"] and (app.active_tab() or {}).get("title") == "a.txt",
               f"a press without travel activates and does not move ({_order(app)})")

    # Drop on the editor's right edge: a split with that tab.
    surface = app.ui()["surfaces"]["editorSurface"]
    a = app.element("tab:0/a.txt")
    app.drag(*app.center(a), surface[0] + surface[2] - 20, surface[1] + surface[3] / 2, steps=16)
    app.wait_until(lambda: any(t.get("group") == 1 for t in app.query("tabs")), 3.0, "a second group from the drop")
    step(app, "drop a on the right edge", 0.5)
    app.expect(_order(app, 1) == ["a.txt"] and _order(app, 0) == ["b.txt", "c.txt"],
               f"the drop splits right with that tab ({_order(app, 0)} | {_order(app, 1)})")
    # Onto the other group's strip: the tab moves there.
    c = app.element("tab:0/c.txt")
    a1 = app.element("tab:1/a.txt")
    app.drag(*app.center(c), a1["rect"][0] + a1["rect"][2] + 10, app.center(a1)[1], steps=16)
    app.wait_until(lambda: _order(app, 1) == ["a.txt", "c.txt"], 3.0, "c to join group 1")
    step(app, "drag c onto the other strip", 0.4)
    app.expect(_order(app, 1) == ["a.txt", "c.txt"] and _order(app, 0) == ["b.txt"],
               f"the tab moved to the other group after a ({_order(app, 0)} | {_order(app, 1)})")
    # The last tab leaving its group closes the group.
    b = app.element("tab:0/b.txt")
    a1 = app.element("tab:1/a.txt")
    app.drag(*app.center(b), a1["rect"][0] + 4, app.center(a1)[1], steps=16)
    app.wait_until(lambda: not any(t.get("group") == 1 for t in app.query("tabs")), 3.0, "the empty group to close")
    step(app, "drag the last tab out of group 0", 0.5)
    app.expect(_order(app, 0) == ["b.txt", "a.txt", "c.txt"], f"one group again, in drop order ({_order(app, 0)})")
    app.expect(len(app.ui().get("editors") or []) == 1, "one pane is drawn")


# ---- files under open editors -----------------------------------------------------

def _rename_row(app: q.App, row: str, new_name: str) -> bool:
    app.click_named("sidebar-view:tree")
    app.wait_until(lambda: f"row:tree/{row}" in tree_rows(app), 5.0, f"{row} in the tree")
    app.click_named(f"row:tree/{row}")
    app.key("F2")
    if not app.wait_until(lambda: app.ui().get("modal") == "prompt", 3.0, f"the rename prompt for {row}"):
        return False
    app.key("ctrl+a")
    app.type(new_name)
    app.key("Return")
    return app.wait_until(lambda: f"row:tree/{new_name}" in tree_rows(app), 5.0, f"{new_name} in the tree")


def flow_tree_file_ops(app: q.App) -> None:
    """A rename or delete under an open editor: renaming an open file retargets
    its tab (title, path, the next save); renaming a folder retargets the tabs
    inside it; deleting an open file closes its tab; a file renamed or deleted
    outside the app reaches the tree and the tab."""
    assert app.project is not None
    open_file(app, "ops/one.txt", "one\n")
    if _rename_row(app, "one.txt", "uno.txt"):
        step(app, "rename one.txt to uno.txt", 0.4)
        app.send("focus", "editor")
        app.expect((app.active_tab() or {}).get("title") == "uno.txt", f"the tab follows the rename ({app.active_tab()})")
        app.expect(app.buffer().get("path", "").endswith("ops/uno.txt"), f"the buffer's path follows ({app.buffer().get('path')})")
        app.click_text(1, 1)
        app.key("ctrl+End")
        app.type("more")
        app.key("ctrl+s")
        app.wait_until(lambda: (app.project / "ops" / "uno.txt").read_text() == "one\nmore\n", 3.0, "the save under the new name")
        app.expect(not (app.project / "ops" / "one.txt").exists(), "nothing is written under the old name")

    open_file(app, "ops/sub/two.txt", "two\n")
    app.click_named("sidebar-view:tree")
    if "row:tree/sub" not in tree_rows(app):
        app.click_named("row:tree/ops")
        app.settle(0.3)
    if _rename_row(app, "sub", "folder"):
        step(app, "rename the folder", 0.4)
        app.send("focus", "editor")
        app.expect(app.buffer().get("path", "").endswith("ops/folder/two.txt"),
                   f"a tab inside a renamed folder follows ({app.buffer().get('path')})")
        app.click_text(1, 1)
        app.key("ctrl+End")
        app.type("!")
        app.key("ctrl+s")
        app.wait_until(lambda: (app.project / "ops" / "folder" / "two.txt").read_text() == "two\n!\n", 3.0,
                       "the save inside the renamed folder")

    open_file(app, "ops/three.txt", "three\n")
    app.click_named("sidebar-view:tree")
    app.wait_until(lambda: "row:tree/three.txt" in tree_rows(app), 5.0, "three.txt in the tree")
    app.click_named("row:tree/three.txt")
    app.key("Delete")
    if app.wait_until(lambda: app.ui().get("modal") == "prompt", 3.0, "the delete prompt"):
        delete = next((e for e in app.elements("button", prefix="button:prompt/") if e["name"].endswith("/Delete")), None)
        if delete is not None:
            app.click(*app.center(delete))
        app.wait_until(lambda: not (app.project / "ops" / "three.txt").exists(), 3.0, "the delete")
        app.wait_until(lambda: all(t.get("title") != "three.txt" for t in app.query("tabs")), 3.0,
                       "the deleted file's tab to close")
        step(app, "delete an open file", 0.4)
        app.expect(all(t.get("title") != "three.txt" for t in app.query("tabs")), "the tab of a deleted file closes")

    # Outside the app: a rename and a delete on disk.
    open_file(app, "ops/four.txt", "four\n")
    os.rename(app.project / "ops" / "four.txt", app.project / "ops" / "cuatro.txt")
    app.click_named("sidebar-view:tree")
    app.wait_until(lambda: "row:tree/cuatro.txt" in tree_rows(app) and "row:tree/four.txt" not in tree_rows(app),
                   8.0, "the tree to show the outside rename")
    step(app, "renamed outside the app", 0.4)
    app.send("focus", "editor")
    app.expect(app.alive(), "the app survives a file renamed under its editor")
    (app.project / "ops" / "cuatro.txt").unlink()
    app.wait_until(lambda: "row:tree/cuatro.txt" not in tree_rows(app), 8.0, "the tree to drop the deleted file")
    step(app, "deleted outside the app", 0.4)
    app.expect(app.alive(), "the app survives a file deleted under its editor")
    dismiss(app)


# ---- the terminal's selection ---------------------------------------------------

def flow_terminal_selection(app: q.App) -> None:
    """A double-click selects a word of the shell's output, a triple-click its
    line, Shift+click extends; each copied with Ctrl+Shift+C and pasted into an
    editor to read it back."""
    open_file(app, "notes/sel.txt", "")
    if not app.ui()["panel"]["visible"]:
        app.key("ctrl+grave")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    app.send("focus", "panel")
    step(app, "focus the terminal", 0.3)

    def output() -> str:
        return app.query("terminal-output").get("text", "")

    app.type("clear; echo alpha beta gamma; echo second line here\n")
    app.wait_until(lambda: "second line here" in output(), 5.0, "the output")
    step(app, "echo two lines", 0.3)
    lines = output().split("\n")
    row = next((i for i, line in enumerate(lines) if line.strip() == "alpha beta gamma"), None)
    if not app.expect(row is not None, f"the output row is in the transcript ({lines[-6:]})"):
        return

    def copy_and_read(label: str) -> str:
        app.key("ctrl+shift+c")
        app.settle(0.15)
        app.send("focus", "editor")
        app.key("ctrl+a")
        app.key("ctrl+v")
        app.settle(0.2)
        got = app.text()
        app.key("ctrl+a")
        app.key("BackSpace")
        app.send("focus", "panel")
        step(app, label, 0.2)
        return got

    app.click(*app.terminal_point(row, 7), clicks=2)
    step(app, "double-click beta", 0.2)
    got = copy_and_read("copy the word")
    app.expect(got.strip() == "beta", f"a double-click selects the word under it ({got!r})")
    app.click(*app.terminal_point(row, 7), clicks=3)
    step(app, "triple-click the line", 0.2)
    got = copy_and_read("copy the line")
    app.expect(got.strip() == "alpha beta gamma", f"a triple-click selects the line ({got!r})")
    # A drag from one line into the next selects across them.
    app.drag(*app.terminal_point(row, 6), *app.terminal_point(row + 1, 6))
    step(app, "drag into the next line", 0.2)
    got = copy_and_read("copy the two-line selection")
    app.expect(got.startswith("beta gamma") and "second" in got,
               f"a drag across lines selects across them ({got!r})")
    # A click collapses the selection: with nothing selected, Ctrl+Shift+C leaves
    # the clipboard alone (a sentinel put there first says so).
    app.send("focus", "editor")
    app.type("sentinel")
    app.key("ctrl+a")
    app.key("ctrl+c")
    app.key("BackSpace")
    app.send("focus", "panel")
    app.click(*app.terminal_point(row + 1, 0))
    step(app, "a click collapses the selection", 0.2)
    got = copy_and_read("copy with nothing selected")
    app.expect(got.strip() == "sentinel", f"a click dropped the selection, so the copy took nothing ({got!r})")


# ---- .editorconfig ------------------------------------------------------------------

def _indent_label(app: q.App) -> str:
    return next((e.get("label", "") for e in app.elements("status") if e["name"] == "status:indent"), "")


def flow_editorconfig(app: q.App) -> None:
    """A `.editorconfig`'s sections pick each file's indentation, line ending and
    save behaviour over the settings (VS Code's precedence); a nested root file
    wins below it; `unset` restores the default; the master switch turns it off."""
    assert app.project is not None
    write(app, ".editorconfig", "\n".join([
        "root = true", "",
        "[*]", "indent_style = space", "indent_size = 2", "end_of_line = lf",
        "trim_trailing_whitespace = true", "insert_final_newline = true", "",
        "[*.md]", "indent_style = tab", "tab_width = 8", "trim_trailing_whitespace = false",
        "insert_final_newline = false", "",
        "[win/*]", "end_of_line = crlf", "",
        "[plain/*]", "indent_size = unset", "indent_style = unset", "",
    ]))
    write(app, "nested/.editorconfig", "root = true\n[*]\nindent_style = space\nindent_size = 3\n")
    open_file(app, "cfg/code.py", "def f():\npass\n")
    app.expect(app.buffer().get("softTabs") is True and app.buffer().get("tabSize") == 2,
               f"[*] gives spaces of 2 ({app.buffer().get('softTabs')}, {app.buffer().get('tabSize')})")
    app.expect(_indent_label(app) == "Spaces: 2", f"the status bar says so ({_indent_label(app)!r})")
    app.click_text(2, 1)
    app.key("Tab")
    app.key("End")
    app.type("  ")
    app.key("ctrl+s")
    app.wait_until(lambda: read(app, "cfg/code.py") == "def f():\n  pass\n", 3.0, "the save")
    step(app, "Tab and save in a 2-space file", 0.3)
    expect_text(app, "def f():\n  pass\n", "Tab inserts two spaces; the save trims the trailing ones")

    open_file(app, "cfg/notes.md", "# title  ")
    app.expect(app.buffer().get("softTabs") is False and app.buffer().get("tabSize") == 8,
               f"[*.md] gives tabs of 8 ({app.buffer().get('softTabs')}, {app.buffer().get('tabSize')})")
    app.expect(_indent_label(app) == "Tabs: 8", f"the status bar follows the section ({_indent_label(app)!r})")
    app.click_text(1, 1)
    app.key("End")
    app.type("!")
    app.key("ctrl+s")
    app.wait_until(lambda: read(app, "cfg/notes.md") == "# title  !", 3.0, "the markdown save")
    step(app, "save a markdown file", 0.3)
    app.expect(read(app, "cfg/notes.md") == "# title  !",
               f"the section keeps trailing whitespace and adds no final newline ({read(app, 'cfg/notes.md')!r})")

    open_file(app, "win/dos.txt", "a\nb\n")
    app.click_text(1, 1)
    app.key("End")
    app.type("!")
    app.key("ctrl+s")
    app.wait_until(lambda: "!" in (app.project / "win" / "dos.txt").read_bytes().decode(), 3.0, "the save")
    step(app, "save under [win/*]", 0.3)
    raw = (app.project / "win" / "dos.txt").read_bytes()
    app.expect(raw == b"a!\r\nb\r\n", f"end_of_line = crlf writes CRLF ({raw!r})")

    open_file(app, "plain/p.txt", "p\n")
    app.expect(app.buffer().get("tabSize") == 4 and app.buffer().get("softTabs") is False,
               f"`unset` restores the settings' defaults ({app.buffer().get('tabSize')}, {app.buffer().get('softTabs')})")
    open_file(app, "nested/deep.py", "n\n")
    app.expect(app.buffer().get("tabSize") == 3 and app.buffer().get("softTabs") is True,
               f"a nested root file wins below it ({app.buffer().get('tabSize')})")

    app.send("set-setting", "editor.editorconfig.enabled", "false")
    open_file(app, "cfg/code.py")
    step(app, "editorconfig off", 0.3)
    app.expect(app.buffer().get("tabSize") == 4 and app.buffer().get("softTabs") is False,
               f"with the switch off the settings rule again ({app.buffer().get('tabSize')}, {app.buffer().get('softTabs')})")
    app.send("set-setting", "editor.editorconfig.enabled", "true")
    open_file(app, "cfg/notes.md")
    app.expect(app.buffer().get("tabSize") == 8, "and back on, the file's section applies again")


def flow_terminal_links(app: q.App) -> None:
    """The terminal's links and mouse: a click on a printed `path:line:col`
    opens the file at that place, a click on a `file://` URL opens the file; the
    middle button pastes the primary selection at the prompt; a program that
    turned mouse reporting on gets the wheel as escape sequences (`cat -v` shows
    them) instead of the terminal scrolling.

    None of this was clicked by a flow (TD-364: the panel's mouse path at 64 %)."""
    assert app.project is not None
    open_file(app, "notes/links.txt", "")
    if not app.ui()["panel"]["visible"]:
        app.key("ctrl+grave")
        app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    app.send("focus", "panel")
    step(app, "focus the terminal", 0.3)

    def output() -> str:
        return app.query("terminal-output").get("text", "")

    def row_of(text: str) -> int | None:
        lines = output().split("\n")
        return next((i for i in range(len(lines) - 1, -1, -1) if lines[i].startswith(text)), None)

    app.type("clear; echo see src/main.cpp:3:5 here; echo file://$PWD/README.md\n")
    app.wait_until(lambda: row_of("file://") is not None, 5.0, "the two links printed")
    step(app, "print two links", 0.3)
    row = row_of("see src/main.cpp")
    if app.expect(row is not None, "the path:line:col line is on screen"):
        app.click(*app.terminal_point(row, 8))
        app.wait_until(lambda: active_title(app) == "main.cpp", 3.0, "the click to open main.cpp")
        step(app, "click path:line:col", 0.3)
        app.expect(active_title(app) == "main.cpp" and app.cursors() and app.cursors()[0] == (3, 5),
                   f"a click on src/main.cpp:3:5 opens it at line 3, column 5 ({active_title(app)}, {app.cursors()})")
    app.send("focus", "panel")
    row = row_of("file://")
    if app.expect(row is not None, "the file:// line is on screen"):
        app.click(*app.terminal_point(row, 12))
        app.wait_until(lambda: active_title(app) == "README.md", 3.0, "the click to open README.md")
        step(app, "click file://", 0.3)
        app.expect(active_title(app) == "README.md", f"a click on a file:// URL opens the file ({active_title(app)})")

    # The middle button pastes the primary selection (X11 only).
    app.send("focus", "panel")
    if not app.wayland:
        app.type("clear; echo primaryword\n")
        app.wait_until(lambda: row_of("primaryword") is not None, 5.0, "the word printed")
        row = row_of("primaryword")
        if row is not None:
            app.click(*app.terminal_point(row, 3), clicks=2)
            step(app, "double-click a word", 0.2)
            app.click(*app.terminal_point(row, 3), button=2)
            app.wait_until(lambda: output().rstrip().endswith("primaryword"), 3.0, "the middle-click paste at the prompt")
            step(app, "middle-click paste", 0.3)
            app.expect(output().rstrip().endswith("primaryword"),
                       f"a middle-click pastes the selection at the prompt ({output().rstrip()[-40:]!r})")
            app.key("ctrl+u")

    # Mouse reporting: the wheel goes to the program, not the scrollback.
    app.type("clear; printf '\\033[?1000h\\033[?1006h'; cat -v\n")
    app.settle(0.6)
    geometry = app.ui().get("terminal") or {}
    if app.expect(bool(geometry), "the terminal reports its geometry"):
        x = geometry["textX"] + 10 * geometry["charWidth"]
        y = geometry["firstLineY"] + 2 * geometry["lineHeight"]
        app.wheel(x, y, 2)
        app.key("Return")
        app.wait_until(lambda: "^[[<65;" in output(), 3.0, "the wheel's report")
        step(app, "wheel with mouse reporting on", 0.3)
        app.expect("^[[<65;" in output(), f"the wheel reaches the program as SGR reports ({output().rstrip()[-60:]!r})")
    app.key("ctrl+c")
    app.type("printf '\\033[?1000l\\033[?1006l'; clear\n")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "native-dialogs": flow_native_dialogs,
    "terminal-links": flow_terminal_links,
    "menu-keyboard": flow_menu_keyboard,
    "prompts-keyboard": flow_prompts_keyboard,
    "tab-drag": flow_tab_drag,
    "tree-file-ops": flow_tree_file_ops,
    "terminal-selection": flow_terminal_selection,
    "editorconfig": flow_editorconfig,
}

ENV: dict[str, dict[str, str]] = {
    "native-dialogs": {"SDL_FILE_DIALOG_DRIVER": "zenity",
                       "PATH": f"{FAKEBIN}:{os.environ.get('PATH', '/usr/bin:/bin')}"},
    "menu-keyboard": {"MICROIDE_QA_MENU_DRY_RUN": "1"},
}
