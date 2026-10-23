"""File format metadata must survive undo, save and process restart together."""
from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import expect_text, open_file, step, write


def flow_format_transactions(app: q.App) -> None:
    """LF/CRLF, with/without a UTF-8 BOM, keep exact bytes through an edit,
    save, undo across the save point, redo, restart and a second edit/save."""
    app.send("set-setting", "editor.save.ensure_final_newline", "false")
    app.send("set-setting", "editor.save.trim_trailing_whitespace", "false")
    cases = (
        ("lf", b"", b"\n"),
        ("crlf", b"", b"\r\n"),
        ("bom-lf", b"\xef\xbb\xbf", b"\n"),
        ("bom-crlf", b"\xef\xbb\xbf", b"\r\n"),
    )
    for name, bom, eol in cases:
        relative = f"format-transactions/{name}.txt"
        path = write(app, relative, "")
        original = bom + eol.join((b"alpha", "café".encode(), b"omega"))
        path.write_bytes(original)
        open_file(app, relative)
        expect_text(app, "alpha\ncafé\nomega", f"{name}: decode format into logical lines")
        app.key("ctrl+Home")
        app.type("X")
        step(app, f"edit {name}")
        app.key("ctrl+s")
        saved = bom + b"X" + original[len(bom):]
        app.wait_until(lambda: not app.buffer().get("dirty") and path.read_bytes() == saved,
                       3.0, f"{name}: first save preserves BOM and line endings")
        app.key("ctrl+z")
        step(app, f"undo saved {name}")
        expect_text(app, "alpha\ncafé\nomega", f"{name}: undo restores text without exposing format bytes")
        app.expect(app.buffer().get("dirty"), f"{name}: undo across save is dirty")
        app.expect(path.read_bytes() == saved, f"{name}: undo never rewrites saved bytes")
        app.key("ctrl+shift+z")
        step(app, f"redo saved {name}")
        expect_text(app, "Xalpha\ncafé\nomega", f"{name}: redo restores edited logical text")
        app.expect(not app.buffer().get("dirty"), f"{name}: redo reaches the save point")
    app.restart()
    step(app, "restore format metadata")
    for name, bom, eol in cases:
        relative = f"format-transactions/{name}.txt"
        open_file(app, relative)
        expect_text(app, "Xalpha\ncafé\nomega", f"{name}: restart restores text")
        app.key("ctrl+End")
        app.key("Return")
        app.type("tail")
        step(app, f"append line after restart {name}")
        app.key("ctrl+s")
        expected = bom + eol.join((b"Xalpha", "café".encode(), b"omega", b"tail"))
        path = app.project / relative
        app.wait_until(lambda: not app.buffer().get("dirty") and path.read_bytes() == expected,
                       3.0, f"{name}: new line after restart uses the original format without adding an EOF newline")


FLOWS: dict[str, Callable[[q.App], None]] = {"file-format-transactions": flow_format_transactions}
