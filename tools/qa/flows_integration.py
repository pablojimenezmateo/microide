"""Ways in that are not a key or a click: a file dropped on the window from another
application, a cold start from a `--control-spec`. (Function breakpoints are
`dbg-function-breakpoints` in flows_debugger.py.)

A coverage build of the suite never ran the file-drop path (`WorkspaceFileDrop`)
or the spec loader (`ControlSpec`) (TD-2026-10-18-364). Drops
go through the control channel's `pointer drop` on both backends: X has no way to
synthesize one either.
"""

from __future__ import annotations

import json
from typing import Callable

import microide_qa as q
from flows_common import active_root, step


def _editor_center(app: q.App) -> tuple[float, float]:
    x, y, w, h = app.ui()["surfaces"]["editorSurface"]
    return x + w / 2, y + h / 2


def flow_file_drop(app: q.App) -> None:
    """A project file, a file outside the project and a folder, dropped on the editor."""
    assert app.project is not None
    x, y = _editor_center(app)
    app.drop(x, y, app.project / "src" / "main.cpp")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "main.cpp", 5.0,
                   "the dropped project file to open")
    step(app, "drop a project file", 0.3)

    outside = app.work / "dropped-notes.txt"
    outside.write_text("notes from elsewhere\n")
    app.drop(x, y, outside)
    app.wait_until(lambda: app.buffer().get("path", "").endswith("dropped-notes.txt"), 5.0,
                   "a file from outside the project to open in this window (VS Code)")
    step(app, "drop an outside file", 0.3)
    app.expect(app.buffer().get("text") == "notes from elsewhere\n", "the dropped file's text is the buffer")

    folder = app.work / "dropped-project"
    (folder / "src").mkdir(parents=True, exist_ok=True)
    (folder / "src" / "lib.c").write_text("int lib(void) { return 1; }\n")
    projects_before = len(app.query("projects"))
    app.drop(x, y, folder)
    app.wait_until(lambda: active_root(app).endswith("dropped-project"), 8.0,
                   "a dropped folder to open as the project")
    step(app, "drop a folder", 0.5)
    app.expect(len(app.query("projects")) == projects_before + 1,
               "the dropped folder is a project of its own, beside the first")

    gone = app.work / "never-existed.txt"
    tabs_before = len(app.query("tabs"))
    app.drop(x, y, gone)
    step(app, "drop a path that does not exist", 0.3)
    app.expect(len(app.query("tabs")) == tabs_before, "a missing path opens nothing")


def flow_control_spec(app: q.App) -> None:
    """A cold start from `--control --control-spec`: transient settings, files to
    open, a function breakpoint and commands, each reported on the JSONL stream."""
    assert app.project is not None
    spec = app.work / "spec.json"
    spec.write_text(json.dumps({
        "settings": {"editor.font_size": "15", "sidebar.file_icons": "true"},
        "open": ["src/scheduler.cpp", "src/main.cpp"],
        "functionBreakpoints": [{"name": "main"}],
        "commands": ["sidebar-show git", "no-such-command"],
    }))
    extra = list(app._extra_args)
    app._extra_args = extra + ["--control", "--control-spec", str(spec)]
    try:
        app.restart()
    finally:
        app._extra_args = extra
    step(app, "cold start from the spec", 0.8)
    applied = []
    for line in (app.log_path.read_text(errors="replace").splitlines() if app.log_path else []):
        if line.startswith("{") and '"applied"' in line:
            try:
                applied.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    by_command = {a["applied"]: a for a in applied}
    app.expect(len(applied) >= 6, f"one `applied` line per spec entry ({[a['applied'] for a in applied]})")
    app.expect(by_command.get("sidebar-show git", {}).get("ok") is True, "the spec's command ran")
    bad = by_command.get("no-such-command", {})
    app.expect(bad.get("ok") is False and bool(bad.get("error")),
               f"an unknown command is reported, with why ({bad})")
    titles = [t.get("title") for t in app.query("tabs")]
    app.expect("scheduler.cpp" in titles and "main.cpp" in titles, f"the spec's files are open ({titles})")
    app.expect(app.ui()["sidebar"]["view"] == "git", "the spec's command showed the git view")
    editors = app.ui().get("editors") or [{}]
    app.expect(float(editors[0].get("lineHeight", 0)) > 18.0,
               f"the spec's 15pt editor font applied (line height {editors[0].get('lineHeight')}, 17.5 at 13pt)")
    # Transient: the setting is not written to the user's config.
    config = app.work / "config" / "microide"
    written = " ".join(p.read_text(errors="replace") for p in config.rglob("*") if p.is_file())
    app.expect("editor.font_size" not in written, "a spec setting is transient, never persisted")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "file-drop": flow_file_drop,
    "control-spec": flow_control_spec,
}

PLUGINS: dict[str, tuple[str, ...]] = {}
