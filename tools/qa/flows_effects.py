"""What a setting or a toggle DOES, not only that it reads back.

`settings-sweep` sets every registered setting to every value and reads it back;
a receipt checks the effect for a dozen of them. The editor's behaviour switches
(auto-close, surround, smart indent, the comment and line-operation actions, Add
Cursor at Match, sorting, snippets, indentation detection) and the language
contract's user overrides (extra and disabled bracket pairs, the line-comment
marker, extra smart-indent patterns, disabled snippets) gate whole behaviours
that the sweep never exercised: a switch that flips its setting and leaves the
behaviour on is green there. Each case here runs the behaviour with the switch
on and off, through the command palette as a person would, and reads the buffer.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, expect_text, indent_unit, open_file, step


def _setting(app: q.App, setting: str) -> object:
    rows = app.query("settings")
    rows = rows if isinstance(rows, list) else rows.get("settings", [])
    return next((row.get("value") for row in rows if row.get("id") == setting), None)


def _toggle(app: q.App, command: str, setting: str) -> bool:
    """Runs a toggle command from the palette (typed by its command name, Enter on
    the first row) and checks that its setting flipped."""
    before = _setting(app, setting)
    app.key("ctrl+shift+p")
    step(app, "palette", 0.3)
    app.type(command)
    step(app, f"type {command}", 0.3)
    rows = app.elements("overlay-row")
    if not app.expect(bool(rows), f"the palette lists {command}"):
        dismiss(app)
        return False
    app.key("Return")
    step(app, f"run {command}", 0.3)
    after = _setting(app, setting)
    app.send("focus", "editor")
    return app.expect(after != before, f"{command} flips {setting} ({before!r} -> {after!r})")


_serial = [0]


def _fresh(app: q.App, text: str, line: int = 1, column: int = 1, suffix: str = ".cpp") -> None:
    """Opens a new file holding `text` (from disk: typing it would run the very
    behaviours under test) with the caret at (line, column)."""
    _serial[0] += 1
    open_file(app, f"effects/case{_serial[0]}{suffix}", text)
    app.send("goto", f"{line}:{column}")
    app.settle(0.1)


# (command, setting, setup text, (line, col), action(app), expected as the
# instance starts, expected after the toggle). `{I}` is one indentation level.
Case = tuple[str, str, str, tuple[int, int], Callable[[q.App], None], str, str]


def _select_word(app: q.App) -> None:
    app.key("ctrl+shift+Right")


def _keys(*chords: str) -> Callable[[q.App], None]:
    def press(app: q.App) -> None:
        for chord in chords:
            app.key(chord)
    return press


def _typed(text: str, before: Callable[[q.App], None] | None = None, *after: str) -> Callable[[q.App], None]:
    def act(app: q.App) -> None:
        if before:
            before(app)
        app.type(text)
        for chord in after:
            app.key(chord)
    return act


CASES: tuple[Case, ...] = (
    ("toggle-editor-auto-close", "editor.brackets.auto_close.enabled", "x", (1, 2),
     _typed("("), "x()", "x("),
    ("toggle-editor-surround", "editor.brackets.surround.enabled", "word", (1, 1),
     _typed("(", _select_word), "(word)", "("),
    ("toggle-editor-smart-indent", "editor.indent.smart.enabled", "int f() {", (1, 10),
     _keys("Return"), "int f() {\n{I}", "int f() {\n"),
    ("toggle-editor-toggle-comment", "editor.shaping.toggle_comment.enabled", "int a;", (1, 1),
     _keys("ctrl+slash"), "// int a;", "int a;"),
    ("toggle-editor-line-ops", "editor.shaping.line_ops.enabled", "one\ntwo", (1, 1),
     _keys("alt+Down"), "two\none", "one\ntwo"),
    ("toggle-editor-line-ops", "editor.shaping.line_ops.enabled", "one\ntwo", (1, 1),
     _keys("shift+alt+Down"), "one\none\ntwo", "one\ntwo"),
    ("toggle-editor-line-ops", "editor.shaping.line_ops.enabled", "one\ntwo", (1, 1),
     _keys("ctrl+shift+k"), "two", "one\ntwo"),
    ("toggle-editor-sort-lines", "editor.shaping.sort_lines.enabled", "b\na\nc", (1, 1),
     lambda app: (app.key("ctrl+a"), app.send("sort-lines-ascending")), "a\nb\nc", "b\na\nc"),
    ("toggle-editor-add-cursor-at-match", "editor.multicursor.add_at_match.enabled", "ab ab", (1, 1),
     _typed("X", _keys("ctrl+d", "ctrl+d")), "X X", "Xab ab"),
    # Off by default: the seed matches any case; on, only its own.
    ("toggle-editor-search-case-sensitive", "editor.search.case_sensitive", "ab Ab", (1, 1),
     _typed("X", _keys("ctrl+shift+l")), "X X", "X Ab"),
    ("toggle-editor-snippets", "editor.snippets.enabled", "", (1, 1),
     _typed("nest", None, "Tab"), "call(outer(inner));", "nest{I}"),
)


def flow_editor_toggles(app: q.App) -> None:
    """Each behaviour switch: its behaviour as the instance starts, then toggled,
    then toggled back for the next case."""
    for command, setting, text, (line, column), act, on, off in CASES:
        for state, want in (("default", on), ("toggled", off)):
            if state == "toggled" and not _toggle(app, command, setting):
                break
            _fresh(app, text, line, column)
            act(app)
            step(app, f"{command} {state}", 0.15)
            expect_text(app, want.replace("{I}", indent_unit(app)), f"{command} {state}: {text!r} at {line}:{column}")
        else:
            _toggle(app, command, setting)


def _set(app: q.App, setting: str, value: str) -> bool:
    reply = app.send("set-setting", setting, value)
    app.send("focus", "editor")
    return app.expect(bool(reply.get("ok")), f"set-setting {setting} {value!r} ({reply})")


def flow_language_overrides(app: q.App) -> None:
    """The language contract's user overrides, each with a behaviour that shows
    it: an extra pair auto-closes and surrounds, a disabled pair does neither, the
    line-comment marker replaces the language's, an extra indent pattern indents
    after it, a disabled snippet prefix no longer expands -- and each set back to
    empty restores the language's own."""
    cases: tuple[tuple[str, str, str, tuple[int, int], Callable[[q.App], None], str, str], ...] = (
        ("editor.brackets.user_pairs", "<|>", "x", (1, 2), _typed("<"), "x<", "x<>"),
        ("editor.brackets.user_pairs", "<|>", "word", (1, 1), _typed("<", _select_word), "<", "<word>"),
        ("editor.brackets.user_disabled", "(|)", "x", (1, 2), _typed("("), "x()", "x("),
        ("editor.brackets.user_disabled", "(|)", "word", (1, 1), _typed("(", _select_word), "(word)", "("),
        ("editor.comments.user_line", "#", "int a;", (1, 1), _keys("ctrl+slash"), "// int a;", "# int a;"),
        ("editor.indents.user_open_patterns", "then", "if x then", (1, 10), _keys("Return"),
         "if x then\n", "if x then\n{I}"),
        ("editor.snippets.user_disabled", "nest", "", (1, 1), _typed("nest", None, "Tab"),
         "call(outer(inner));", "nest{I}"),
    )
    for setting, value, text, (line, column), act, before, after in cases:
        for label, want in (("default", before), (f"= {value}", after)):
            if label != "default" and not _set(app, setting, value):
                break
            _fresh(app, text, line, column)
            act(app)
            step(app, f"{setting} {label}", 0.15)
            expect_text(app, want.replace("{I}", indent_unit(app)), f"{setting} {label}: {text!r} at {line}:{column}")
        _set(app, setting, "")
    # Back to empty: the language's own pairs again.
    _fresh(app, "x", 1, 2)
    app.type("(")
    expect_text(app, "x()", "user_disabled cleared: ( closes itself again")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "editor-toggles": flow_editor_toggles,
    "language-overrides": flow_language_overrides,
}

PLUGINS: dict[str, tuple[str, ...]] = {
    "editor-toggles": ("qa-snippets",),
    "language-overrides": ("qa-snippets",),
}
