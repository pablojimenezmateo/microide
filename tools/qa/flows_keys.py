"""The shortcut sweep: every resolved keybinding pressed through the real X
keyboard, in every context where it is meant to fire, and checked against the
action receipt (`ui.actions`): the key must reach the action it advertises --
not nothing, not another binding's action.

The unit suite's reachability matrix (KeybindingReachabilityTests) dispatches
SDL events directly; this one goes through xdotool, X keysyms and SDL's own
keymap, which is where a chord like Ctrl+Shift+[ or Ctrl+` can silently arrive
as something else. The instance runs with MICROIDE_QA_ACTION_DRY_RUN=1: a key
records the action it reached and runs nothing, so Ctrl+W closes no tab and F5
starts no debugger.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import open_file

# The `keybindings` query spells keys as the menus show them; xdotool wants keysyms.
_KEYSYMS = {
    "PageDown": "Next", "PageUp": "Prior", "`": "grave", "\\": "backslash",
    "[": "bracketleft", "]": "bracketright", ",": "comma", "-": "minus", "=": "equal",
    "/": "slash", ".": "period", "Space": "space", "Enter": "Return", "Return": "Return",
    "Backspace": "BackSpace", "Esc": "Escape", "Escape": "Escape", "Tab": "Tab",
    "'": "apostrophe", ";": "semicolon", "Delete": "Delete", "Insert": "Insert",
    "Home": "Home", "End": "End", "Up": "Up", "Down": "Down", "Left": "Left", "Right": "Right",
}
_MODIFIERS = {"Ctrl": "ctrl", "Shift": "shift", "Alt": "alt", "Super": "super", "Cmd": "super"}


def xdotool_chord(chord: str) -> str | None:
    """'Ctrl+Shift+[' -> 'ctrl+shift+bracketleft', and a two-step 'Ctrl+K Ctrl+O'
    -> 'ctrl+k ctrl+o'; None for a chord this cannot type."""
    if " " in chord:
        steps = [xdotool_chord(step) for step in chord.split(" ")]
        return None if None in steps else " ".join(steps)  # type: ignore[arg-type]
    parts = chord.split("+")
    if chord.endswith("++"):
        parts = chord[:-2].split("+") + ["+"]
    *mods, key = parts
    names = []
    for mod in mods:
        if mod not in _MODIFIERS:
            return None
        names.append(_MODIFIERS[mod])
    if key in _KEYSYMS:
        names.append(_KEYSYMS[key])
    elif len(key) == 1 and key.isalnum():
        names.append(key.lower())
    elif key.startswith("F") and key[1:].isdigit():
        names.append(key)
    else:
        return None
    return "+".join(names)


def _enter(app: q.App, context: str) -> None:
    """Put the keyboard in `context` (editor, sidebar, terminal)."""
    if context == "terminal":
        if not app.ui()["panel"]["visible"] or app.ui()["panel"]["content"] != "terminal":
            app.send("term-toggle")
            app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal panel")
        app.send("focus", "panel")
    elif context == "sidebar":
        if app.ui()["sidebar"]["view"] != "tree" or not app.ui()["sidebar"]["visible"]:
            app.send("sidebar-show", "tree")
        app.send("focus", "sidebar")
    else:
        app.send("focus", "editor")


def _active_in(binding: dict, where: str) -> bool:
    """Whether `binding` is meant to fire with the keyboard in `where`."""
    context = binding["context"]
    if context == "global":
        return where != "terminal" or binding["skipsTerminal"]
    return context == where


def _runs(bindings: list[dict], binding: dict, where: str) -> dict:
    """The binding the chord of `binding` runs in `where`. Bindings that share a
    chord (F6: debug-pause while a program runs, focus-next-part otherwise) run the
    first ENABLED one in table order, or the first when none is; any other chord
    has the one binding."""
    if not any(b["sharesChord"] for b in bindings if b["chord"] == binding["chord"]):
        return binding
    sharing = [b for b in bindings if b["chord"] == binding["chord"] and _active_in(b, where)]
    return next((b for b in sharing if b["enabled"]), sharing[0] if sharing else binding)


def scenario_keys(app: q.App) -> None:
    """Press every binding in each context it serves; the receipt must name its
    command -- or, for a chord several bindings share, the first enabled one's. A
    Global binding that does not skip the shell must NOT fire from a focused
    terminal (the terminal owns the key)."""
    open_file(app, "src/scheduler.cpp")
    bindings = [b for b in app.keybindings() if not b.get("plugin")]
    app.expect(len(bindings) > 50, f"the keybinding table lists the bindings ({len(bindings)})")
    # A chord a terminal-context binding claims runs THAT binding from a focused
    # terminal (Ctrl+PageDown is next-tab elsewhere, term-next there).
    terminal_owner = {b["chord"]: b["command"] for b in bindings if b["context"] == "terminal"}
    unreachable: list[str] = []
    for binding in bindings:
        chord = xdotool_chord(binding["chord"])
        if chord is None:
            app.expect(False, f"the sweep cannot type {binding['chord']!r} ({binding['id']})")
            continue
        context = binding["context"]
        targets = {"global": ["editor", "sidebar"] + (["terminal"] if binding["skipsTerminal"] else []),
                   "editor": ["editor"], "sidebar": ["sidebar"], "terminal": ["terminal"]}[context]
        refusals = ["terminal"] if context == "global" and not binding["skipsTerminal"] else []
        for where in targets + refusals:
            _enter(app, where)
            app.settle(0.05)
            before = app.ui()["actions"]
            app.key(chord)
            app.settle(0.08)
            after = app.ui()["actions"]
            ran = after["dispatched"] > before["dispatched"]
            label = f"{binding['chord']} ({binding['id']}) in the {where}"
            if where in refusals:
                owner = terminal_owner.get(binding["chord"])
                if ran and after["last"] != owner:
                    app.expect(False, f"{label}: the terminal owns this key, but {after['last']} ran")
                if not ran and owner is not None:
                    app.expect(False, f"{label}: the terminal's own {owner} should have run")
                continue
            expected = _runs(bindings, binding, where)
            if not ran:
                unreachable.append(label)
            elif after["last"] != expected["command"]:
                shared = "" if expected is binding else f" (it shares the chord: {expected['id']} is the enabled one)"
                app.expect(False, f"{label}: ran {after['last']!r}, expected {expected['command']!r}{shared}")
            # A key a focused surface consumed can still have moved state (Escape
            # closing a menu); put the window back before the next press.
            if app.ui()["overlay"]["visible"] or app.ui().get("modal") or app.ui()["menu"]["open"]:
                app.key("Escape")
                app.settle(0.1)
        app.check(binding["id"])
    for label in unreachable:
        app.expect(False, f"{label}: the key reached no action")


FLOWS: dict[str, Callable[[q.App], None]] = {"keys": scenario_keys}
ENV: dict[str, dict[str, str]] = {"keys": {"MICROIDE_QA_ACTION_DRY_RUN": "1"}}
