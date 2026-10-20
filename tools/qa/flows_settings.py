"""Every setting, every value.

Eighty of the registered settings were never set by any flow (TD-2026-10-18-364:
the configuration dimension), and the click-through fixed in 923aecee needed one
non-default setting to show. The sweep takes the catalogue from the `settings`
query -- id, type, scope, range, enum values, current value -- so a setting added
to the registry is swept the day it lands, with nothing to update here.

Three flows: `settings-sweep` sets every setting to every value it can take
through `set-setting` (the deterministic path a `--set` or a control spec uses)
and checks the value reads back, the Settings overlay shows it, the app survives
and every frame is clean, plus a receipt for each setting with a cheap observable;
`settings-controls` drives each kind of row control with the pointer (checkbox,
stepper at its bounds, segmented enum, text value, the font picker, the filter);
`settings-persist` writes a batch of user- and project-scope values and reads
them back after a restart, from the app and from the files.
"""

from __future__ import annotations

import re
from typing import Callable

import microide_qa as q
from flows_common import dismiss, open_file, step

# Settings the sweep leaves alone, and why: a value here changes what the harness
# itself stands on, or spawns something.
SKIP: dict[str, str] = {
    "control.enabled": "turning the control channel off cuts the driver's own line",
    "terminal.shell": "a different shell for every terminal the suite opens",
    "remote.ssh_command": "no remote host in the local suite",
    "remote.server_command": "no remote host in the local suite",
    "remote.server_socket_dir": "no remote host in the local suite",
    "remote.server_install": "no remote host in the local suite",
    "session.restore_on_launch": "the restart flows own it",
}

# One harmless non-default value for each string setting the sweep sets (a
# string has no catalogue of values); the parsers these feed are what runs.
STRING_VALUES: dict[str, str] = {
    "editor.font_family": "DejaVu Sans Mono",
    "terminal.font_family": "DejaVu Sans Mono",
    "terminal.commands_to_skip_shell": "ls, cat",
    "project.files_exclude": "**/build/**, *.o",
    "editor.brackets.user_pairs": "<>",
    "editor.brackets.user_disabled": "'",
    "editor.comments.user_line": "#",
    "editor.indents.user_open_patterns": "then$",
    "editor.snippets.user_disabled": "main",
}


def _catalogue(app: q.App) -> list[dict]:
    rows = app.query("settings")
    app.expect(len(rows) >= 90, f"the settings query lists the catalogue ({len(rows)})")
    return rows


def _values_for(row: dict) -> list[str]:
    """Every value the sweep sets for one setting, the default last so the
    sweep leaves the app as it found it."""
    kind, default = row["type"], row["default"]
    if kind == "bool":
        values = ["false" if default == "true" else "true"]
    elif kind == "enum":
        values = [v for v in row.get("enumValues", []) if v != default]
    elif kind in ("int", "float"):
        values = []
        for bound in ("min", "max"):
            if bound in row and str(row[bound]) != default:
                values.append(_number_text(row[bound]))
    else:
        values = [STRING_VALUES[row["id"]]] if row["id"] in STRING_VALUES else []
    return values + [default]


def _number_text(value: object) -> str:
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def _same_value(kind: str, got: str | None, want: str) -> bool:
    if got is None:
        return False
    if kind in ("int", "float"):
        try:
            return abs(float(got) - float(want)) < 1e-6
        except ValueError:
            return False
    return got == want


def _display_matches(kind: str, label: str, value: str) -> bool:
    """The Settings row paints an enum by its label ("On focus change" for
    on_focus_change) and a number as text; a checkbox as on/off."""
    if kind == "bool":
        return label == ("on" if value == "true" else "off")
    if kind in ("int", "float"):
        return _same_value(kind, label, value)
    if kind == "enum":
        return re.sub(r"[^a-z0-9]+", "_", label.lower()).strip("_") == value
    return label == value


def _value(app: q.App, setting: str) -> str | None:
    return next((r["value"] for r in app.query("settings") if r["id"] == setting), None)


# ---- receipts: what a value must change on screen -----------------------------

def _editor(app: q.App) -> dict:
    editors = app.ui().get("editors") or [{}]
    return editors[0]


def _line_height(app: q.App) -> float:
    return float(_editor(app).get("lineHeight", 0))


def _text_x(app: q.App) -> float:
    return float(_editor(app).get("textX", 0))


def _surface(app: q.App, key: str) -> list | None:
    return app.ui()["surfaces"].get(key)


def _menus(app: q.App) -> int:
    return len(app.elements("menu"))


def _foldable(app: q.App) -> int:
    return len(app.buffer().get("foldable", []))


def _type_and_read(app: q.App, text: str) -> str:
    """Types at a fresh last line and returns the buffer's last line."""
    app.send("focus", "editor")
    app.key("ctrl+End")
    app.key("Return")
    app.type(text)
    app.settle(0.15)
    return app.text().splitlines()[-1] if app.text().strip() else ""


# (setting, value) -> (label, probe) where probe(app) returns something that must
# differ between the default and the value. Probes read the app, never pixels.
RECEIPTS: dict[tuple[str, str], tuple[str, Callable[[q.App], object]]] = {
    ("ui.show_status_bar", "false"): ("the status bar leaves the layout", lambda a: _surface(a, "statusBar")),
    ("ui.layout_mode", "compact"): ("compact mode folds the menu bar", _menus),
    ("chrome.project_tabs.hide_when_single", "false"): ("the project strip shows", lambda a: _surface(a, "projectTabStrip")),
    ("editor.line_numbers", "false"): ("the text moves left without numbers", _text_x),
    ("editor.font_size", "32"): ("the line height grows", _line_height),
    ("editor.font_size", "8"): ("the line height shrinks", _line_height),
    ("ui.scale", "1.5"): ("the window scale follows", lambda a: a.ui()["window"]["scale"]),
    ("editor.tab_size", "16"): ("the buffer's tab size", lambda a: a.buffer().get("tabSize")),
    ("editor.soft_tabs", "true"): ("the buffer's soft tabs", lambda a: a.buffer().get("softTabs")),
    ("editor.fold.enabled", "false"): ("no foldable ranges", _foldable),
    ("editor.brackets.auto_close.enabled", "false"): ("( no longer closes itself", lambda a: _type_and_read(a, "(")),
    ("terminal.font_size", "32"): ("the terminal cell height grows", lambda a: (a.ui().get("terminal") or {}).get("lineHeight")),
}


def flow_settings_sweep(app: q.App) -> None:
    """Every setting to every value it can take, each read back and shown."""
    open_file(app, "src/scheduler.cpp")
    app.send("open", "src/main.cpp")
    app.key("ctrl+grave")
    app.wait_until(lambda: app.ui()["panel"]["visible"], 5.0, "the terminal panel")
    app.send("focus", "editor")
    rows = _catalogue(app)
    swept = 0
    for row in rows:
        setting, kind = row["id"], row["type"]
        if setting in SKIP or row.get("plugin"):
            continue
        if setting in ("editor.font_size", "terminal.font_size", "ui.scale") and app.wayland:
            pass  # the same values; kiosk weston keeps the window, the content rescales
        baseline: dict[str, object] = {}
        for value in _values_for(row):
            receipt = RECEIPTS.get((setting, value))
            if receipt is not None:
                baseline[value] = receipt[1](app)
            reply = app.send("set-setting", setting, value)
            if not app.expect(bool(reply.get("ok")), f"set-setting {setting} {value} ({reply})"):
                continue
            step(app, f"{setting} = {value}", 0.25)
            swept += 1
            got = _value(app, setting)
            app.expect(_same_value(kind, got, value), f"{setting} reads back {value!r} (got {got!r})")
            if receipt is not None and value != row["default"]:
                label, probe = receipt
                after = probe(app)
                app.expect(after != baseline[value], f"{setting}={value}: {label} ({baseline[value]!r} -> {after!r})")
            if not app.alive():
                return
        # What a wrong value gets: a refusal, not a crash, and the value kept.
        if kind in ("int", "float", "bool", "enum"):
            kept = _value(app, setting)
            reply = app.send("set-setting", setting, "not-a-" + kind)
            app.expect(not reply.get("ok") and bool(reply.get("error")),
                       f"set-setting {setting} not-a-{kind} is refused with why ({reply})")
            app.expect(_value(app, setting) == kept, f"a refused value leaves {setting} at {kept!r}")
        if kind == "int" and "max" in row:
            reply = app.send("set-setting", setting, str(int(row["max"]) + 1000))
            step(app, f"{setting} past its maximum", 0.1)
            got = _value(app, setting)
            app.expect(bool(reply.get("ok")) and _same_value(kind, got, str(row["max"])),
                       f"{setting} past its maximum clamps to {row['max']} ({reply}, got {got!r})")
            app.send("set-setting", setting, row["default"])
            app.settle(0.1)
    print(f"settings-sweep: {swept} values set", flush=True)
    app.expect(swept > 120, f"the sweep set the catalogue ({swept} values)")
    app.send("focus", "editor")
    leftovers = [(r["id"], r["value"]) for r in app.query("settings")
                 if r["id"] not in SKIP and not r.get("plugin") and not _same_value(r["type"], r["value"], r["default"])]
    app.expect(not leftovers, f"every setting is back at its default after the sweep ({leftovers})")

    # The overlay paints the current value of every row: filter for each id.
    app.key("ctrl+comma")
    app.wait_for("field:settings/filter", 3.0)
    for row in rows:
        if row.get("plugin"):
            continue
        app.click_named("field:settings/filter")
        app.key("ctrl+a")
        app.type(row["id"])
        app.settle(0.15)
        element = next((e for e in app.elements("setting") if e["name"] == f"setting:{row['id']}"), None)
        if not app.expect(element is not None, f"Settings lists {row['id']} when filtered by its id"):
            continue
        app.expect(_display_matches(row["type"], str(element.get("label", "")), str(row["value"])),
                   f"the {row['id']} row shows its value ({element.get('label')!r} for {row['value']!r})")
    app.check("every row filtered")
    app.key("Escape")
    step(app, "close Settings", 0.3)


def _row(app: q.App, setting: str) -> dict | None:
    app.click_named("field:settings/filter")
    app.key("ctrl+a")
    app.type(setting)
    app.settle(0.2)
    return next((e for e in app.elements("setting") if e["name"] == f"setting:{setting}"), None)


def _control(app: q.App, setting: str, part: str) -> dict | None:
    return next((e for e in app.elements("button") if e["name"] == f"button:setting/{setting}/{part}"), None)


def flow_settings_controls(app: q.App) -> None:
    """Each kind of row control by pointer: a checkbox, a stepper held at both
    bounds, a segmented enum, a text value edited in place, the font picker's
    list and its Choose file row, Reset, and the filter by description."""
    open_file(app, "src/scheduler.cpp")
    app.key("ctrl+comma")
    app.wait_for("field:settings/filter", 3.0)
    step(app, "Settings", 0.3)

    # A checkbox: click flips it, the catalogue agrees, Reset puts it back.
    row = _row(app, "editor.line_numbers")
    if app.expect(row is not None, "editor.line_numbers is listed"):
        box = _control(app, "editor.line_numbers", "checkbox")
        app.expect(box is not None, "a bool row has a checkbox")
        if box is not None:
            app.click(*app.center(box))
            step(app, "click the checkbox", 0.25)
            app.expect(_value(app, "editor.line_numbers") == "false", "the checkbox click turns line numbers off")
            app.expect(app.element("setting:editor.line_numbers").get("label") == "off", "the row shows off")
            reset = _control(app, "editor.line_numbers", "reset")
            app.expect(reset is not None, "a changed row offers Reset")
            if reset is not None:
                app.click(*app.center(reset))
                step(app, "Reset", 0.25)
                app.expect(_value(app, "editor.line_numbers") == "true", "Reset turns them back on")

    # A stepper: increments stop at the maximum, decrements at the minimum.
    row = _row(app, "editor.tab_size")
    if app.expect(row is not None, "editor.tab_size is listed"):
        spec = next(r for r in app.query("settings") if r["id"] == "editor.tab_size")
        inc = _control(app, "editor.tab_size", "increment")
        dec = _control(app, "editor.tab_size", "decrement")
        if app.expect(inc is not None and dec is not None, "an int row has a stepper"):
            for _ in range(int(spec["max"]) - int(spec["default"]) + 2):
                app.click(*app.center(inc))
                app.settle(0.05)
            step(app, "increment past the maximum", 0.2)
            app.expect(_value(app, "editor.tab_size") == str(spec["max"]),
                       f"the stepper stops at the maximum ({_value(app, 'editor.tab_size')})")
            for _ in range(int(spec["max"]) - int(spec["min"]) + 2):
                app.click(*app.center(dec))
                app.settle(0.05)
            step(app, "decrement past the minimum", 0.2)
            app.expect(_value(app, "editor.tab_size") == str(spec["min"]),
                       f"the stepper stops at the minimum ({_value(app, 'editor.tab_size')})")
            app.send("set-setting", "editor.tab_size", spec["default"])

    # A segmented enum: clicking the value steps to the next option and wraps.
    row = _row(app, "editor.autosave")
    if app.expect(row is not None, "editor.autosave is listed"):
        spec = next(r for r in app.query("settings") if r["id"] == "editor.autosave")
        value = _control(app, "editor.autosave", "value")
        if app.expect(value is not None, "an enum row has a value control"):
            seen = []
            for _ in range(len(spec["enumValues"])):
                app.click(*app.center(value))
                step(app, "click the enum value", 0.2)
                seen.append(_value(app, "editor.autosave"))
                value = _control(app, "editor.autosave", "value") or value
            app.expect(seen == spec["enumValues"][1:] + [spec["default"]],
                       f"clicking the value walks every option in order and wraps ({seen})")
            app.send("set-setting", "editor.autosave", spec["default"])

    # A text value: click, type, Enter writes it; Escape abandons an edit.
    row = _row(app, "project.files_exclude")
    if app.expect(row is not None, "project.files_exclude is listed"):
        value = _control(app, "project.files_exclude", "value")
        if app.expect(value is not None, "a string row has a value control"):
            app.click(*app.center(value))
            step(app, "click the text value", 0.2)
            app.type("**/build/**")
            app.key("Return")
            step(app, "type a value and Enter", 0.25)
            app.expect(_value(app, "project.files_exclude") == "**/build/**",
                       f"Enter writes the typed value ({_value(app, 'project.files_exclude')!r})")
            value = _control(app, "project.files_exclude", "value") or value
            app.click(*app.center(value))
            app.key("ctrl+a")
            app.type("abandoned")
            app.key("Escape")
            step(app, "Escape from an edit", 0.25)
            app.expect(_value(app, "project.files_exclude") == "**/build/**",
                       "Escape leaves the value as it was")
            app.expect(app.ui().get("modal") == "settings", "Escape from the edit keeps Settings open")
            app.send("set-setting", "project.files_exclude", "")

    # The font picker: a list of families filtered by typing, Enter picks one.
    row = _row(app, "editor.font_family")
    if app.expect(row is not None, "editor.font_family is listed"):
        value = _control(app, "editor.font_family", "value")
        if app.expect(value is not None, "the font row has a value control"):
            app.click(*app.center(value))
            step(app, "open the font picker", 0.3)
            rows = app.elements("overlay-row")
            app.expect(len(rows) >= 2, f"the font picker lists installed families ({len(rows)})")
            app.type("mono")
            step(app, "filter the families", 0.3)
            filtered = app.elements("overlay-row")
            app.expect(0 < len(filtered) <= len(rows) and all("mono" in r.get("label", "").lower()
                                                            or "choose" in r.get("label", "").lower()
                                                            for r in filtered),
                       f"typing filters the list ({[r.get('label') for r in filtered][:4]})")
            app.key("Down")
            app.key("Return")
            step(app, "pick a family", 0.3)
            chosen = _value(app, "editor.font_family") or ""
            app.expect("mono" in chosen.lower(), f"Enter writes the highlighted family ({chosen!r})")
            app.send("set-setting", "editor.font_family", "")

    # The filter matches descriptions and labels, not only ids.
    app.click_named("field:settings/filter")
    app.key("ctrl+a")
    app.type("whitespace")
    step(app, "filter by a word of a description", 0.3)
    names = [e["name"] for e in app.elements("setting")]
    app.expect("setting:editor.save.trim_trailing_whitespace" in names and "setting:editor.view.render_whitespace" in names,
               f"the filter matches by label and description ({names})")
    app.key("ctrl+a")
    app.type("zzzz-no-such-setting")
    step(app, "a filter that matches nothing", 0.3)
    app.expect(not app.elements("setting"), "an unmatched filter shows no rows")
    app.key("ctrl+a")
    app.key("BackSpace")
    step(app, "clear the filter", 0.3)
    categories = [e["name"] for e in app.elements("settings-category")]
    app.expect(len(categories) >= 5, f"categories are back ({categories})")
    # The overlay scrolls by wheel and the rows follow.
    rows_before = [(e["name"], e["rect"][1]) for e in app.elements("setting")]
    first = app.elements("setting")[0]
    app.wheel(*app.center(first), 8)
    step(app, "wheel the rows", 0.3)
    rows_after = [(e["name"], e["rect"][1]) for e in app.elements("setting")]
    app.expect(rows_after and rows_after[0] != rows_before[0], f"the wheel scrolls the rows ({rows_before[:1]} -> {rows_after[:1]})")
    app.expect(all(0 <= y <= app.ui()["window"]["height"] for _, y in rows_after),
               "every listed row is inside the window")
    app.key("Escape")
    step(app, "close Settings", 0.3)
    dismiss(app)


def flow_settings_persist(app: q.App) -> None:
    """Values of both scopes written, read back after a restart from the app and
    from the config files; a project value stays in its project's file."""
    assert app.project is not None
    open_file(app, "src/scheduler.cpp")
    wanted = {
        "editor.font_size": "15", "editor.tab_size": "8", "editor.autosave": "after_delay",
        "ui.show_status_bar": "false", "sidebar.file_icons": "true", "editor.line_numbers": "false",
        "terminal.scrollback_lines": "5000", "project.files_exclude": "**/build/**",
    }
    scopes = {r["id"]: r["scope"] for r in app.query("settings")}
    for setting, value in wanted.items():
        reply = app.send("set-setting", setting, value)
        app.expect(bool(reply.get("ok")), f"set-setting {setting} {value} ({reply})")
    step(app, "a batch of values", 0.4)
    app.restart()
    open_file(app, "src/scheduler.cpp")
    step(app, "after a restart", 0.4)
    after = {r["id"]: r["value"] for r in app.query("settings")}
    for setting, value in wanted.items():
        app.expect(after.get(setting) == value, f"{setting} survives a restart ({after.get(setting)!r}, want {value!r})")
    app.expect(_surface(app, "statusBar") is None, "the restored status-bar setting applies")
    app.expect(_line_height(app) > 18.0, f"the restored font size applies (line height {_line_height(app)})")
    config_dir = app.work / "config" / "microide"
    user_text = " ".join(p.read_text(errors="replace") for p in config_dir.rglob("*") if p.is_file())
    # A project's config lives under the state directory, not in the project.
    project_text = " ".join(p.read_text(errors="replace")
                            for p in (app.work / "state" / "microide" / "projects").rglob("config") if p.is_file())
    for setting, value in wanted.items():
        in_user, in_project = setting in user_text, setting in project_text
        if scopes.get(setting) == "project":
            app.expect(in_project and not in_user, f"{setting} (project scope) lives in the project's config "
                                                   f"(project {in_project}, user {in_user})")
        else:
            app.expect(in_user and not in_project, f"{setting} (user scope) lives in the user's config "
                                                   f"(user {in_user}, project {in_project})")
    for setting in wanted:
        app.send("set-setting", setting, next(r["default"] for r in app.query("settings") if r["id"] == setting))
    step(app, "defaults again", 0.3)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "settings-sweep": flow_settings_sweep,
    "settings-controls": flow_settings_controls,
    "settings-persist": flow_settings_persist,
}
