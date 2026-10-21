"""Language servers beyond clangd: the bundled typescript-lsp and dotnet-lsp
plugins against the real typescript-language-server and csharp-ls.

Only cpp-lsp ran in the suite, so the code paths other servers take -- their
own capabilities, their own answer shapes, a plugin that resolves a project-local
binary -- never did (TD-2026-10-18-364). Each flow writes a two-file project into
the fixture: hover a symbol, F12 into the other file, and an error that becomes a
diagnostic.

typescript-language-server is taken from MQA_TS_NODE_MODULES (default
~/.cache/microide-qa/ts/node_modules, where `npm install typescript
typescript-language-server` put it): the plugin finds a project-local
node_modules/.bin first, so the flow links that directory into the project.
"""

from __future__ import annotations

import os
import shutil
import time
from pathlib import Path
from typing import Callable

import microide_qa as q
from flows_common import open_file, step, write

TS_MODULES = Path(os.environ.get("MQA_TS_NODE_MODULES",
                                 str(Path.home() / ".cache" / "microide-qa" / "ts" / "node_modules")))


def _missing(app: q.App, what: str) -> bool:
    if os.environ.get("MQA_ALLOW_MISSING_TOOLS") != "1":
        app.expect(False, f"{what} is not available; this flow cannot run (MQA_ALLOW_MISSING_TOOLS=1 to skip)")
    return True


def _lsp_ready(app: q.App, relative: str, timeout: float = 60.0) -> bool:
    open_file(app, relative)
    return app.wait_until(lambda: "Ready" in app.element("status:lsp").get("label", ""), timeout,
                          f"the language server for {relative} to be ready")


def _hover_until(app: q.App, line: int, column: int, timeout: float,
                 ready: Callable[[str], bool] = lambda text: True) -> dict | None:
    """Hovers until the server answers. A server can say Ready before it has loaded
    the project (csharp-ls loads it through MSBuild after initialize), and a hover
    is asked once per rest of the pointer, as in any editor -- so leave and come
    back, as a person would, rather than wait on an answer that was already empty."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        app.move(*app.text_point(line, column))
        rest_until = time.monotonic() + 3.0
        while time.monotonic() < rest_until:
            hover = app.ui()["popups"]["hover"]
            if hover and hover.get("text"):
                last = hover["text"]
                if ready(hover["text"]):
                    return hover
            time.sleep(0.1)
        # Away from the symbol AND its card (it opens right below the symbol, and a
        # card is kept while the pointer is over it): the sidebar.
        x, y, w, h = app.ui()["surfaces"]["sidebar"]
        app.move(x + w / 2, y + h - 20)
        app.settle(0.3)
    app.expect(False, f"no hover at {line}:{column} within {timeout:.0f}s (last answer: {last!r})")
    return None


def _goes_to(app: q.App, line: int, column: int, title: str, target_line: int) -> None:
    app.click_text(line, column)
    app.key("F12")
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == title, 15.0, f"F12 to open {title}")
    step(app, "F12", 0.3)
    app.expect(bool(app.cursors()) and app.cursors()[0][0] == target_line,
               f"F12 lands on line {target_line} of {title} ({app.cursors()})")


def _diagnosed(app: q.App, relative: str, text: str) -> None:
    open_file(app, relative, text)
    app.send("save")
    app.wait_until(lambda: any("1" in (e.get("label") or "") or "error" in (e.get("label") or "").lower()
                               for e in app.elements("status") if e["name"] == "status:problems"),
                   30.0, f"a diagnostic for {relative}")


def flow_lsp_typescript(app: q.App) -> None:
    """typescript-language-server: hover a function, F12 across files, a type error."""
    if not (TS_MODULES / ".bin" / "typescript-language-server").exists():
        _missing(app, f"typescript-language-server under {TS_MODULES}")
        return
    assert app.project is not None
    web = app.project / "web"
    web.mkdir(exist_ok=True)
    # A real directory, as in a real project: a symlink would resolve outside the
    # project, which the plugin's file sandbox rightly refuses. Hard links make the
    # copy instant (falling back to a copy across filesystems).
    modules = web / "node_modules"
    if not modules.exists():
        try:
            shutil.copytree(TS_MODULES, modules, symlinks=True, copy_function=os.link)
        except OSError:
            shutil.rmtree(modules, ignore_errors=True)
            shutil.copytree(TS_MODULES, modules, symlinks=True)
    write(app, "web/tsconfig.json", '{ "compilerOptions": { "strict": true, "target": "es2020" } }\n')
    write(app, "web/util.ts", "export function twice(value: number): number {\n  return value * 2;\n}\n")
    write(app, "web/main.ts", 'import { twice } from "./util";\n\nconst result: number = twice(21);\nconsole.log(result);\n')
    app.send("project-open", str(web))
    # Files the flow writes from here on belong to the project it opened.
    app.project = web
    if not _lsp_ready(app, "main.ts"):
        return
    # Until tsserver has loaded ./util, `twice` is only the import's alias: its
    # hover says "import twice" and F12 stops at the import line. A loaded suite run
    # got there first; wait for the resolved signature, as a person would see it.
    hover = _hover_until(app, 3, 26, 60.0, lambda text: "number" in text)
    app.expect(hover is not None and "twice" in hover.get("text", "") and "number" in hover.get("text", ""),
               f"hovering twice() shows its signature ({hover})")
    _goes_to(app, 3, 26, "util.ts", 1)
    _diagnosed(app, "bad.ts", 'const n: number = "not a number";\n')


def flow_lsp_csharp(app: q.App) -> None:
    """csharp-ls: hover a method, F12 across files, a compile error."""
    if shutil.which("csharp-ls") is None and not (Path.home() / ".dotnet/tools/csharp-ls").exists():
        _missing(app, "csharp-ls")
        return
    if shutil.which("dotnet") is None:
        _missing(app, "dotnet")
        return
    assert app.project is not None
    # The run's HOME is a throwaway; csharp-ls and its runtime are installed under
    # the real one. Linked in, they are where a user's would be -- and the server
    # sandbox grants ~/.dotnet by name (Landlock follows the link).
    for name in (".dotnet", ".nuget"):
        real, linked = Path.home() / name, app.work / name
        if real.exists() and not linked.exists():
            linked.symlink_to(real)
    cs = app.project / "cs"
    cs.mkdir(exist_ok=True)
    write(app, "cs/qa.csproj",
          '<Project Sdk="Microsoft.NET.Sdk">\n  <PropertyGroup>\n    <OutputType>Exe</OutputType>\n'
          '    <TargetFramework>net8.0</TargetFramework>\n    <Nullable>enable</Nullable>\n'
          '  </PropertyGroup>\n</Project>\n')
    write(app, "cs/Util.cs", "public static class Util\n{\n    public static int Twice(int value) => value * 2;\n}\n")
    write(app, "cs/Program.cs", "var result = Util.Twice(21);\nSystem.Console.WriteLine(result);\n")
    app.send("project-open", str(cs))
    app.project = cs
    # csharp-ls loads the project through MSBuild before it answers: slow the first time.
    if not _lsp_ready(app, "Program.cs", 120.0):
        return
    hover = _hover_until(app, 1, 20, 60.0)
    app.expect(hover is not None and "Twice" in hover.get("text", ""),
               f"hovering Util.Twice shows its signature ({hover})")
    _goes_to(app, 1, 20, "Util.cs", 3)
    _diagnosed(app, "Bad.cs", "public static class Bad\n{\n    public static int N => \"not a number\";\n}\n")


FLOWS: dict[str, Callable[[q.App], None]] = {
    "lsp-typescript": flow_lsp_typescript,
    "lsp-csharp": flow_lsp_csharp,
}

PLUGINS: dict[str, tuple[str, ...]] = {
    "lsp-typescript": ("typescript-lsp",),
    "lsp-csharp": ("dotnet-lsp",),
}
