"""The pointer-shape sweep: every element the ui query reports carries the cursor
the shell would show over its centre (`cursor`), and this walks the app through
its surfaces -- menus, overlays, every sidebar view, Settings and Help, the
editor's find widget and banner, split editors, the terminal with tabs, a split,
its find bar and a program that owns the mouse, the Output panel, the debug pane,
the unsaved-changes prompt, a narrow window's overflow chips -- and holds each
clickable element to the hand, each text field to the I-beam.

The rule a user reads: what lights up and does something when clicked shows a
hand; a disabled control shows the arrow (`enabled: false` on its element). List rows show the hand too, as the
tree always has. The checks name every miss, so a new control that forgets its cursor fails
here rather than in a bug report.
"""

from __future__ import annotations

import microide_qa as q
from flows_common import dismiss, open_file, resize_window, step, write

# Element kinds a click acts on, held to the hand cursor.
POINTER_KINDS = frozenset({
    "menu", "menu-item", "project-tab", "tab", "sidebar-view", "panel-tab", "button",
    "overlay-row", "settings-category", "debug-tab", "notification", "status", "row",
    "setting", "debug-row",
})
# Text inputs, held to the I-beam.
TEXT_KINDS = frozenset({"field"})


def _expected(element: dict) -> str | None:
    kind = element.get("kind")
    if element.get("enabled") is False:
        return "default"
    if kind in POINTER_KINDS:
        return "pointer"
    if kind in TEXT_KINDS:
        return "text"
    return None


def _center(element: dict) -> tuple[float, float]:
    x, y, w, h = element["rect"]
    return x + w / 2, y + h / 2


def _inside(point: tuple[float, float], rect: tuple[float, float, float, float]) -> bool:
    x, y, w, h = rect
    return x <= point[0] < x + w and y <= point[1] < y + h


def _bounds(elements: list[dict]) -> tuple[float, float, float, float] | None:
    if not elements:
        return None
    left = min(e["rect"][0] for e in elements)
    top = min(e["rect"][1] for e in elements)
    right = max(e["rect"][0] + e["rect"][2] for e in elements)
    bottom = max(e["rect"][1] + e["rect"][3] for e in elements)
    return left, top, right - left, bottom - top


def _covered(ui: dict, elements: list[dict]):
    """What floats over the workbench, as a predicate over elements: a modal or a
    list overlay leaves only its own elements reachable, an open menu's popup
    covers what lies under its rows. A covered element rightly shows the arrow."""
    modal = ui.get("modal")
    if modal in ("settings", "prompt", "unsaved-prompt"):
        own = ("prompt/", "setting/", "settings/")
        return lambda e: not (e["kind"] in ("setting", "settings-category", "overlay-row")
                              or e["name"].split(":", 1)[-1].startswith(own))
    if modal:  # Help / About: a card with no element of its own
        return lambda e: True
    overlay = ui.get("overlay") or {}
    if overlay.get("visible") and overlay.get("mode") not in ("buffer-search", "buffer-replace"):
        return lambda e: e["kind"] not in ("overlay-row",) and not e["name"].startswith("field:overlay")
    popup = _bounds([e for e in elements if e["kind"] == "menu-item"])
    if popup is not None:
        pad = 12  # the popup's frame and padding around its rows
        popup = (popup[0] - pad, popup[1] - pad, popup[2] + 2 * pad, popup[3] + 2 * pad)
        return lambda e: e["kind"] != "menu-item" and _inside(_center(e), popup)
    return lambda e: False


def check_cursors(app: q.App, state: str) -> int:
    """Checks every reachable element on screen; returns how many it checked."""
    ui = app.ui()
    elements = ui.get("elements", [])
    covered = _covered(ui, elements)
    checked = 0
    for element in elements:
        want = _expected(element)
        got = element.get("cursor")
        if want is None or got is None or covered(element):
            continue
        checked += 1
        app.expect(got == want,
                   f"[{state}] {element['name']} ({element['kind']}) shows the {got} cursor, not {want}")
    return checked


def _visit(app: q.App, state: str, settle: float = 0.3, expect_elements: bool = True) -> None:
    step(app, state, settle)
    checked = check_cursors(app, state)
    if expect_elements:
        app.expect(checked > 0, f"[{state}] the sweep found no element to check")


def flow_cursor_sweep(app: q.App) -> None:
    """Every clickable element in every surface shows the hand cursor."""
    open_file(app, "notes/a.txt", "alpha\nbeta\ngamma\n")
    open_file(app, "notes/b.txt", "one\ntwo\n")
    _visit(app, "workbench")

    # Each top-level menu, open.
    for menu in [e["name"] for e in app.elements("menu")]:
        app.click_named(menu)
        _visit(app, f"{menu} open", 0.25)
        dismiss(app)

    # Each sidebar view.
    for view in [e["name"] for e in app.elements("sidebar-view")]:
        app.click_named(view)
        _visit(app, view)
    app.send("sidebar-show", "tree")

    # The list overlays.
    for chord, label in (("ctrl+shift+p", "command palette"), ("ctrl+p", "quick open")):
        app.key(chord)
        _visit(app, label)
        dismiss(app)

    # The in-file find widget, in replace mode for all its buttons.
    app.send("focus", "editor")
    app.key("ctrl+h")
    app.type("a")
    _visit(app, "find widget")
    dismiss(app)
    app.key("Escape")

    # Settings and Help.
    app.key("ctrl+comma")
    _visit(app, "settings", 0.4)
    app.key("Escape")
    step(app, "close settings", 0.2)
    app.send("about")
    _visit(app, "about", 0.4, expect_elements=False)  # a card with no controls
    app.key("Escape")
    step(app, "close about", 0.2)

    # Two editor groups.
    app.send("split-right")
    _visit(app, "split editor")

    # The terminal: two tabs, a split pane, the find bar, then a program that
    # owns the mouse (its clickable cells are the program's, not ours).
    app.send("term")
    app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal")
    app.send("term")
    step(app, "second terminal", 0.4)
    app.send("term-split")
    _visit(app, "terminal, split", 0.4)
    app.send("terminal-find")
    app.type("x")
    _visit(app, "terminal find bar")
    app.expect(len(app.elements("button", prefix="button:terminal-find/")) == 5,
               "the terminal find bar's five buttons are on screen to check")
    app.key("Escape")
    app.send("panel-maximize")
    _visit(app, "terminal maximized")
    app.send("panel-maximize")

    # The Output panel.
    app.send("show-output", "microide")
    _visit(app, "output panel")

    # The debug pane.
    app.send("debug-pane-toggle")
    _visit(app, "debug pane")
    app.send("debug-pane-toggle")

    # A toast.
    app.send("focus", "editor")
    app.key("F8")  # no problems anywhere: a toast says so
    app.wait_until(lambda: bool(app.elements("notification")), 3.0, "a toast")
    _visit(app, "notification")

    # The unsaved-changes prompt.
    app.send("focus", "editor")
    app.type("dirty")
    app.key("ctrl+w")
    _visit(app, "unsaved prompt")
    dismiss(app)

    # A narrow window: overflow chips on every strip.
    for i in range(6):
        open_file(app, f"notes/wide-{i}.txt", "x\n")
    resize_window(app, 760, 600)
    _visit(app, "narrow window")
    chips = [e["name"] for e in app.elements("button") if "/more-" in e["name"]]
    app.expect(any(n.startswith("button:tabs/") for n in chips),
               f"a narrow window shows the editor strip's overflow chip ({chips})")


# A stand-in for a TUI that owns the mouse (Claude Code's fullscreen mode): any-
# motion SGR tracking, a "[ Click me ]" control it lights when the pointer rests on
# it and dims when it leaves, a line of text to select, and every key it reads
# echoed as hex -- so the flow can see which keys reach the program.
TUI = r"""
import os, re, sys, termios, tty
fd = sys.stdin.fileno()
saved = termios.tcgetattr(fd)
tty.setraw(fd)
out = sys.stdout
GREY, LIT = "\x1b[38;2;120;120;120m", "\x1b[38;2;255;255;255;48;2;70;70;70m"
BUTTON, ROW, COL = "[ Click me ]", 3, 5
def paint(lit):
    out.write(f"\x1b[{ROW};{COL}H" + (LIT if lit else GREY) + BUTTON + "\x1b[0m")
    out.flush()
out.write("\x1b[?1049h\x1b[?1003h\x1b[?1006h\x1b[2J\x1b[1;1HTUI READY\x1b[5;5HSELECT THIS TEXT")
paint(False)
lit = False
log = []
try:
    while True:
        data = os.read(fd, 1024)
        if not data:
            break
        for m in re.finditer(rb"\x1b\[<(\d+);(\d+);(\d+)([mM])", data):
            b, x, y, kind = int(m[1]), int(m[2]), int(m[3]), m[4]
            over = y == ROW and COL <= x < COL + len(BUTTON)
            if b & 32 and over != lit:
                lit = over
                paint(lit)
            if b == 0 and kind == b"M" and over:
                log.append("clicked")
        keys = re.sub(rb"\x1b\[<[\d;]+[mM]", b"", data)
        if keys:
            if keys == b"q":
                break
            log.append("key:" + keys.hex())
        out.write("\x1b[8;1H\x1b[2K" + " ".join(log[-6:]))
        out.flush()
finally:
    out.write("\x1b[?1003l\x1b[?1006l\x1b[?1049l")
    out.flush()
    termios.tcsetattr(fd, termios.TCSADRAIN, saved)
"""


def flow_terminal_mouse_owner(app: q.App) -> None:
    """A program that owns the mouse: the pane shows the arrow, the hand over the
    control the program lights under the pointer (and the arrow again once it
    dims it), a click reaches the program, Shift+drag selects its text instead,
    Ctrl+J / F5 / Ctrl+P / Escape reach it as keys, and a file dropped on it is
    typed in as its quoted path."""
    write(app, "tui.py", TUI)
    app.send("term", "python3", "tui.py")
    app.send("panel-maximize")

    def screen() -> list[str]:
        return app.query("terminal-output").get("text", "").split("\n")

    app.wait_until(lambda: any("TUI READY" in line for line in screen()), 8.0, "the TUI to start")
    rows = screen()
    button_row = next(i for i, line in enumerate(rows) if "[ Click me ]" in line)
    button_col = rows[button_row].index("[ Click me ]")
    text_row = next(i for i, line in enumerate(rows) if "SELECT THIS TEXT" in line)
    text_col = rows[text_row].index("SELECT THIS TEXT")

    app.move(*app.terminal_point(text_row, text_col + 2))
    app.wait_until(lambda: app.ui().get("cursor") == "default", 3.0,
                   "the arrow over a program that owns the mouse")
    app.move(*app.terminal_point(button_row, button_col + 3))
    app.wait_until(lambda: app.ui().get("cursor") == "pointer", 3.0,
                   "the hand once the program lights the control under the pointer")
    app.move(*app.terminal_point(button_row, button_col + 9))
    app.wait_until(lambda: app.ui().get("cursor") == "pointer", 3.0, "the hand across the whole control")
    app.move(*app.terminal_point(text_row, text_col + 2))
    app.wait_until(lambda: app.ui().get("cursor") == "default", 3.0, "the arrow once it dims it")

    app.click(*app.terminal_point(button_row, button_col + 3))
    app.wait_until(lambda: any("clicked" in line for line in screen()), 3.0, "the click to reach the program")

    for chord, sent in (("ctrl+j", "0a"), ("ctrl+p", "10"), ("F5", "1b5b31357e"), ("Escape", "1b")):
        app.key(chord)
        app.wait_until(lambda: any(f"key:{sent}" in line for line in screen()), 3.0,
                       f"{chord} to reach the program as {sent}")

    # A file dragged in from another application and dropped on the program is
    # typed into it, quoted (how a screenshot reaches Claude Code's prompt).
    dropped = write(app, "drop me.txt", "dropped\n")
    x, y = app.terminal_point(text_row, text_col)
    app.send("pointer", "drop", str(round(x)), str(round(y)), str(dropped))
    typed = f"'{dropped}' ".encode().hex()
    # The hex runs past the pane's width: read the screen joined.
    app.wait_until(lambda: f"key:{typed}" in "".join(screen()), 3.0,
                   "a dropped file's quoted path to reach the program")

    app.drag(*app.terminal_point(text_row, text_col), *app.terminal_point(text_row, text_col + 6),
             modifiers=("shift",))
    step(app, "shift+drag", 0.3)
    app.key("ctrl+shift+c")
    step(app, "copy", 0.2)
    app.key("q")
    app.wait_until(lambda: not any("TUI READY" in line for line in screen()), 5.0, "the TUI to quit")
    app.send("panel-maximize")
    open_file(app, "notes/pasted.txt", "")
    app.send("focus", "editor")
    app.key("ctrl+v")
    step(app, "paste", 0.3)
    app.expect(app.text().startswith("SELECT"),
               f"Shift+drag selected the program's text past its mouse tracking ({app.text()[:40]!r})")


FLOWS = {
    "cursor-sweep": flow_cursor_sweep,
    "terminal-mouse-owner": flow_terminal_mouse_owner,
}
