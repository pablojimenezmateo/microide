"""The settings matrix for the sweeps: every keybinding and every context-menu row,
again under each layout the default run never sees.

The menu click-through (923aecee) needed one non-default setting to show, and the
first scaled run of `menu-clicks` found the ui query's unscaled coordinates
(aec389eb): a bug that depends on layout is invisible to a suite that only ever
runs the default one. `menu-clicks` walks its own layouts; this walks the same
ones -- plus the light theme and a larger editor font -- for `keys` and
`context-menus`, in the dry-run mode they already use (every key and row records
its receipt and runs nothing).
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, step
from flows_context_menus import scenario_context_menus
from flows_keys import scenario_keys

# (name, settings, extra commands) -- every layout sets every key it uses, so no
# layout inherits another's; the extras are commands (a colour scheme is one).
_MATRIX_KEYS = (("chrome.project_tabs.hide_when_single", "true"), ("ui.layout_mode", "auto"),
                ("ui.scale", "1"), ("ui.show_status_bar", "true"), ("sidebar.file_icons", "false"),
                ("editor.font_size", "13"))


def _layout(**overrides: str) -> tuple[tuple[str, str], ...]:
    return tuple((key, overrides.get(key.replace(".", "_"), value)) for key, value in _MATRIX_KEYS)


MATRIX: tuple[tuple[str, tuple[tuple[str, str], ...], tuple[tuple[str, ...], ...]], ...] = (
    ("project strip", _layout(chrome_project_tabs_hide_when_single="false"), ()),
    ("compact", _layout(ui_layout_mode="compact"), ()),
    ("scaled, no status bar, file icons",
     _layout(ui_scale="1.5", ui_show_status_bar="false", sidebar_file_icons="true"), ()),
    ("light theme, large font", _layout(editor_font_size="20"), (("colorscheme", "light"),)),
)


def _apply(app: q.App, name: str, settings: tuple[tuple[str, str], ...],
           commands: tuple[tuple[str, ...], ...]) -> None:
    dismiss(app)
    for key, value in settings:
        reply = app.send("set-setting", key, value)
        app.expect(bool(reply.get("ok")), f"{name}: set-setting {key} {value} ({reply})")
    for command in commands:
        reply = app.send(*command)
        app.expect(bool(reply.get("ok")), f"{name}: {' '.join(command)} ({reply})")
    step(app, f"layout {name}", 0.4)


def scenario_sweeps_matrix(app: q.App) -> None:
    """`keys` and `context-menus` once per non-default layout."""
    for name, settings, commands in MATRIX:
        _apply(app, name, settings, commands)
        scenario_keys(app)
        dismiss(app)
        scenario_context_menus(app)
        dismiss(app)
    _apply(app, "default", _layout(), (("colorscheme", "default"),))


SCENARIOS: dict[str, Callable[[q.App], None]] = {"sweeps-matrix": scenario_sweeps_matrix}
ENV: dict[str, dict[str, str]] = {"sweeps-matrix": {"MICROIDE_QA_ACTION_DRY_RUN": "1"}}
