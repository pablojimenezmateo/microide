"""Files as they are on disk: their indentation, line endings, byte-order mark
and bytes; a file deleted or changed under an edit; the save point undo walks
across; untitled buffers.

Every other flow opens a fresh LF, UTF-8, tab-free file the fixture wrote. What
a person opens is anything else -- a two-space Python file, a CRLF config, a
file with a BOM, a binary -- and a save must give back what it took. Each step
here is checked against the bytes on disk, not only against the buffer.
"""

from __future__ import annotations

from typing import Callable

import microide_qa as q
from flows_common import dismiss, open_file, read, step, titles, write


def _status(app: q.App, segment: str) -> str:
    element = next((e for e in app.elements("status") if e["name"] == f"status:{segment}"), None)
    return (element or {}).get("label", "")


def _bytes(app: q.App, relative: str) -> bytes:
    assert app.project is not None
    return (app.project / relative).read_bytes()


def _save(app: q.App, label: str) -> None:
    app.key("ctrl+s")
    app.wait_until(lambda: not app.buffer().get("dirty"), 3.0, f"{label} to save")
    step(app, label, 0.2)


def _reopen(app: q.App, relative: str) -> None:
    """Closes the active tab (clean) and opens `relative` again, so it is read
    from disk with the settings now in force."""
    app.key("ctrl+w")
    step(app, "close", 0.2)
    open_file(app, relative)


def flow_file_formats(app: q.App) -> None:
    """Indentation is detected per file on open (two spaces, tabs, four spaces) and
    shown in the status bar, and turning detection off leaves the editor's own;
    a CRLF file stays CRLF after an edit -- the new line too -- and says so; a
    BOM survives a save; a binary file opens as bytes and a one-byte insertion
    writes back every other byte as it was."""
    # Indentation, detected on open (VS Code's editor.detectIndentation).
    cases = (
        ("fmt/two.py", "def f():\n  if x:\n    return 1\n  return 2\n", True, 2, "Spaces: 2"),
        ("fmt/tabs.c", "int f() {\n\tif (x) {\n\t\treturn 1;\n\t}\n}\n", False, None, "Tabs:"),
        ("fmt/four.js", "function f() {\n    if (x) {\n        return 1;\n    }\n}\n", True, 4, "Spaces: 4"),
    )
    for relative, content, soft, width, label in cases:
        open_file(app, relative, content)
        buffer = app.buffer()
        app.expect(buffer.get("softTabs") == soft and (width is None or buffer.get("tabSize") == width),
                   f"{relative}: indentation detected as {'spaces' if soft else 'tabs'} {width or ''} "
                   f"(softTabs={buffer.get('softTabs')}, tabSize={buffer.get('tabSize')})")
        app.expect(_status(app, "indent").startswith(label),
                   f"{relative}: the status bar says {label!r} ({_status(app, 'indent')!r})")
    # Enter after a detected two-space line indents by two -- after switching away
    # and back, which used to reset the file to the settings' tabs.
    open_file(app, "fmt/two.py")
    app.expect(app.buffer().get("softTabs") and app.buffer().get("tabSize") == 2,
               f"two.py keeps its two spaces after other tabs were active ({app.buffer().get('tabSize')})")
    app.key("ctrl+End")
    app.type("def g():\n")
    app.type("x")
    step(app, "type under def", 0.2)
    app.expect(app.text().endswith("def g():\n  x"),
               f"Enter after `def g():` indents by the detected two spaces ({app.text()[-16:]!r})")
    _save(app, "save two.py")
    # A settings change does not undo a file's detected indentation either.
    app.send("set-setting", "editor.tab_size", "8")
    app.expect(app.buffer().get("tabSize") == 2,
               f"editor.tab_size leaves a detected file alone ({app.buffer().get('tabSize')})")
    # Detection off (Auto-Detect Indent on Open, from its toggle command): the file
    # opens with the editor's own settings, not the file's.
    app.send("toggle-editor-auto-detect-indent")
    app.expect(next(s["value"] for s in app.query("settings") if s["id"] == "editor.indent.detect_on_open") == "false",
               "the toggle turns detection off")
    app.send("set-setting", "editor.tab_size", "8")
    _reopen(app, "fmt/two.py")
    buffer = app.buffer()
    app.expect(buffer.get("tabSize") == 8,
               f"with detection off the file takes editor.tab_size, 8 ({buffer.get('tabSize')})")
    app.send("toggle-editor-auto-detect-indent")
    _reopen(app, "fmt/two.py")
    app.expect(app.buffer().get("tabSize") == 2, f"detection back on reads two again ({app.buffer().get('tabSize')})")

    # CRLF: shown, kept for the old lines and given to the new one.
    write(app, "fmt/crlf.txt", "")
    (app.project / "fmt/crlf.txt").write_bytes(b"one\r\ntwo\r\nthree\r\n")
    open_file(app, "fmt/crlf.txt")
    app.expect("CRLF" in _status(app, "encoding"), f"the status bar says CRLF ({_status(app, 'encoding')!r})")
    app.expect("\r" not in app.text(), f"the buffer holds lines, not carriage returns ({app.text()!r})")
    app.key("ctrl+End")
    app.type("four\n")
    _save(app, "save the CRLF file")
    app.expect(_bytes(app, "fmt/crlf.txt") == b"one\r\ntwo\r\nthree\r\nfour\r\n",
               f"a CRLF file stays CRLF, the typed line too ({_bytes(app, 'fmt/crlf.txt')!r})")

    # A byte-order mark: shown, and kept by a save.
    (app.project / "fmt/bom.txt").write_bytes(b"\xef\xbb\xbfhello\nworld\n")
    open_file(app, "fmt/bom.txt")
    app.expect("BOM" in _status(app, "encoding"), f"the status bar names the BOM ({_status(app, 'encoding')!r})")
    app.expect(app.text().startswith("hello"), f"the BOM is not text in the buffer ({app.text()[:8]!r})")
    app.key("ctrl+End")
    app.type("more")
    _save(app, "save the BOM file")
    app.expect(_bytes(app, "fmt/bom.txt").startswith(b"\xef\xbb\xbfhello\nworld\nmore"),
               f"the BOM survives the save ({_bytes(app, 'fmt/bom.txt')[:16]!r})")

    # A binary file: every byte value, opened, one byte inserted, saved.
    data = bytes(range(256)) * 4
    (app.project / "fmt/blob.bin").write_bytes(data)
    open_file(app, "fmt/blob.bin")
    app.expect("Bytes" in _status(app, "encoding") or "binary" in _status(app, "encoding").lower(),
               f"a binary file says it is shown as bytes ({_status(app, 'encoding')!r})")
    app.key("ctrl+Home")
    app.type("Z")
    _save(app, "save the binary file")
    saved = _bytes(app, "fmt/blob.bin")
    app.expect(saved == b"Z" + data,
               f"one inserted byte, and the other {len(data)} written back exactly "
               f"({len(saved)} bytes, first difference at "
               f"{next((i for i, (a, b) in enumerate(zip(saved, b'Z' + data)) if a != b), '-')})")


def flow_file_vanish(app: q.App) -> None:
    """A file deleted outside the app while its buffer is dirty: Ctrl+S writes it
    back, with no banner to answer (VS Code). A clean file deleted outside keeps
    its tab (VS Code marks it deleted rather than closing what you were reading).
    A file CHANGED outside while dirty is refused, and Overwrite on the banner
    writes the buffer."""
    open_file(app, "vanish/edited.txt", "edited\n")
    app.key("End")
    app.type(" here")
    step(app, "dirty the buffer", 0.2)
    (app.project / "vanish/edited.txt").unlink()
    app.settle(1.5)  # let the watcher see the delete, as a person would not notice it
    app.expect(app.buffer().get("dirty") and app.active_tab().get("title") == "edited.txt",
               f"the dirty buffer of a deleted file stays open and dirty ({app.active_tab()})")
    app.key("ctrl+s")
    app.wait_until(lambda: (app.project / "vanish/edited.txt").exists(), 3.0, "Ctrl+S to write the file back")
    step(app, "save the deleted file", 0.3)
    if (app.project / "vanish/edited.txt").exists():
        app.expect(read(app, "vanish/edited.txt").startswith("edited here"),
                   f"the saved file holds the buffer ({read(app, 'vanish/edited.txt')!r})")
    app.expect(not app.buffer().get("dirty"), "the buffer is clean once saved")
    app.expect(not any(e["name"].startswith("button:banner/") for e in app.elements("button")),
               "no banner asks about a file that is simply gone")

    open_file(app, "vanish/read.txt", "just reading\n")
    (app.project / "vanish/read.txt").unlink()
    app.settle(1.5)
    app.expect("read.txt" in titles(app), f"a clean file deleted outside keeps its tab ({titles(app)})")

    open_file(app, "vanish/clash.txt", "mine\n")
    app.key("End")
    app.type(" edited")
    step(app, "dirty clash.txt", 0.2)
    write(app, "vanish/clash.txt", "theirs\n")
    app.settle(1.5)
    app.key("ctrl+s")
    step(app, "save over an external change", 0.4)
    app.expect(read(app, "vanish/clash.txt") == "theirs\n",
               f"a save over an external change is refused ({read(app, 'vanish/clash.txt')!r})")
    overwrite = app.wait_for("button:banner/Overwrite", 3.0)
    if overwrite:
        app.click(*app.center(overwrite))
        app.wait_until(lambda: read(app, "vanish/clash.txt").startswith("mine edited"), 3.0,
                       "Overwrite to write the buffer")
        step(app, "overwrite", 0.3)


def flow_undo_save_point(app: q.App) -> None:
    """The dirty flag follows the save point through undo and redo (VS Code): undo
    past a save is dirty, redo back to it is clean, and an edit undone back to the
    saved text is clean again; the tab and the unsaved prompt agree."""
    open_file(app, "notes/savepoint.txt", "base\n")
    app.key("End")
    app.type("A")
    _save(app, "save with A")
    app.key("ctrl+z")
    step(app, "undo past the save", 0.2)
    app.expect(app.text() == "base\n" and app.buffer().get("dirty"),
               f"undo past the save point is dirty ({app.text()!r}, dirty={app.buffer().get('dirty')})")
    app.expect(bool((app.active_tab() or {}).get("dirty")), "the tab shows it dirty too")
    app.key("ctrl+shift+z")
    step(app, "redo to the save", 0.2)
    app.expect(app.text() == "baseA\n" and not app.buffer().get("dirty"),
               f"redo back to the save point is clean ({app.text()!r}, dirty={app.buffer().get('dirty')})")
    app.type("B")
    step(app, "type B", 0.1)
    app.expect(app.buffer().get("dirty"), "a new edit is dirty")
    app.key("ctrl+z")
    step(app, "undo B", 0.2)
    app.expect(not app.buffer().get("dirty"), "undoing it back to the saved text is clean")
    app.key("ctrl+w")
    step(app, "close the clean tab", 0.3)
    app.expect(app.ui().get("prompt") is None and "savepoint.txt" not in titles(app),
               f"a buffer back at its save point closes without asking ({app.ui().get('prompt')})")
    app.expect(read(app, "notes/savepoint.txt") == "baseA\n", "the disk holds the save")


def flow_untitled_numbering(app: q.App) -> None:
    """Untitled buffers are numbered (VS Code): Ctrl+N twice gives Untitled-1 and
    Untitled-2, the unsaved prompt names the one it is about, Discard closes it,
    and its
    number is taken by the next new buffer."""
    app.send("focus", "editor")
    app.key("ctrl+n")
    step(app, "ctrl+n", 0.2)
    app.key("ctrl+n")
    step(app, "ctrl+n again", 0.2)
    names = [t for t in titles(app) if t.startswith("Untitled")]
    app.expect(names == ["Untitled-1", "Untitled-2"], f"two new buffers are Untitled-1 and Untitled-2 ({names})")
    app.wait_for("tab:0/Untitled-2", 2.0)
    app.type("draft")
    step(app, "type in Untitled-2", 0.2)
    app.key("ctrl+w")
    step(app, "close the dirty one", 0.3)
    prompt = app.ui().get("prompt") or {}
    app.expect("Untitled-2" in prompt.get("message", ""),
               f"the unsaved prompt names the buffer it is about ({prompt.get('message')!r})")
    discard = app.wait_for("button:prompt/Discard", 2.0)
    if discard:
        app.click(*app.center(discard))
        step(app, "discard", 0.3)
    dismiss(app)
    app.expect("Untitled-2" not in titles(app), f"Discard closes it ({titles(app)})")
    app.key("ctrl+n")
    step(app, "ctrl+n after the close", 0.2)
    app.expect((app.active_tab() or {}).get("title") == "Untitled-2",
               f"the next new buffer takes the free number ({(app.active_tab() or {}).get('title')!r})")
    # Save As (Ctrl+Shift+S) names it: the file is written and the tab takes the name.
    app.type("saved draft")
    app.key("ctrl+shift+s")
    field = app.wait_for("field:prompt", 3.0)
    if field:
        app.key("ctrl+a")
        app.type("untitled/saved.txt")
        app.key("Return")
        app.wait_until(lambda: (app.project / "untitled" / "saved.txt").exists(), 3.0, "Save As to write the file")
        app.expect(read(app, "untitled/saved.txt").startswith("saved draft")
                   and (app.active_tab() or {}).get("title") == "saved.txt",
                   f"the file holds the buffer and the tab takes its name ({(app.active_tab() or {}).get('title')})")


def flow_save_all(app: q.App) -> None:
    """File > Save All (Ctrl+K S, VS Code's chord) writes every unsaved file in
    every group -- one file open in two groups once -- leaves an untitled buffer
    for its Save As and says so, and does not type the chord's S. The menu row is
    greyed out while nothing is unsaved. A file changed on disk since it was
    opened is refused by name, the others still written."""
    open_file(app, "all/one.txt", "one\n")
    app.key("ctrl+End")
    app.type("ONE")
    app.key("ctrl+backslash")
    step(app, "split", 0.4)
    app.key("ctrl+End")
    app.type("!")
    open_file(app, "all/two.txt", "two\n")
    app.key("ctrl+End")
    app.type("TWO")
    app.key("ctrl+n")
    app.type("scratch")
    step(app, "three unsaved buffers", 0.3)
    app.click_named("menu:File")
    step(app, "File menu", 0.2)
    row = app.element("menu-item:File/Save All")
    app.expect(bool(row) and row.get("enabled") is not False, f"Save All is offered ({row})")
    dismiss(app)
    app.key("ctrl+k")
    app.key("s")
    step(app, "ctrl+k s", 0.6)
    app.wait_until(lambda: read(app, "all/one.txt") == "one\nONE!\n" and read(app, "all/two.txt") == "two\nTWO\n",
                   4.0, "both files written")
    app.expect(read(app, "all/one.txt") == "one\nONE!\n" and read(app, "all/two.txt") == "two\nTWO\n",
               f"Save All writes both files ({read(app, 'all/one.txt')!r}, {read(app, 'all/two.txt')!r})")
    app.expect(app.text() == "scratch" and app.buffer()["dirty"],
               f"the untitled buffer keeps its text, unsaved, and no S was typed ({app.text()!r})")
    notes = [n.get("message", "") for n in app.query("notifications")]
    app.expect(any("untitled buffer needs a name" in m for m in notes),
               f"the notice says the untitled buffer needs a Save As ({notes})")
    app.key("ctrl+w")
    step(app, "close the untitled buffer", 0.3)
    if app.ui().get("modal"):
        app.key("d")
        step(app, "discard it", 0.3)
    app.click_named("menu:File")
    step(app, "File menu, nothing unsaved", 0.2)
    row = app.element("menu-item:File/Save All")
    app.expect(row.get("enabled") is False, f"with nothing unsaved the row is greyed out ({row})")
    dismiss(app)
    # A changed-on-disk file is refused by name; the other is still written.
    app.send("open", "all/one.txt")
    step(app, "one.txt", 0.3)
    app.key("ctrl+End")
    app.type("+")
    app.send("open", "all/two.txt")
    step(app, "two.txt", 0.3)
    app.key("ctrl+End")
    app.type("+")
    write(app, "all/one.txt", "changed elsewhere\n")
    step(app, "one.txt changed on disk", 1.2)
    reply = app.send("save-all")
    app.expect(not reply.get("ok") and "one.txt" in str(reply.get("error", "")),
               f"Save All names the file it could not write ({reply})")
    app.expect(read(app, "all/two.txt") == "two\nTWO\n+\n" and read(app, "all/one.txt") == "changed elsewhere\n",
               f"two.txt is written and one.txt's disk change kept ({read(app, 'all/two.txt')!r}, "
               f"{read(app, 'all/one.txt')!r})")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "file-formats": flow_file_formats,
    "file-vanish": flow_file_vanish,
    "undo-save-point": flow_undo_save_point,
    "untitled-numbering": flow_untitled_numbering,
    "save-all": flow_save_all,
}
