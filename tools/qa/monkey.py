"""The seeded monkey: weighted random actions over the running window.

Each step draws one action from a weighted source and runs it through the App,
which records it (the replay file) and then checks every oracle (the redraw
verifier's QA-STALE, sanitizer and allocator reports, the process staying up).
Seeded `random.Random` only, so a seed means the same actions on the same fixture.

Profiles:

  default   ui elements (click/hover) 50%, key chords from the app's own
            `keybindings` 15%, control commands 10%, typed text 10%, drags from
            dividers and tabs 5%, wheel 5%, free pointer clicks 5%
  commands  the former tools/fuzz-control-commands.py, kept seed-for-seed: every
            command verb with hostile arguments (`mixed` generators), on-disk
            churn under the open buffers, a frame between commands
  editor    the fuzzer's `editor` profile: splits, tabs, reopen and typing, with
            the split-and-close burst emitted as a unit (the viewport COPY paths;
            it reproduced the layout-cache use-after-free, TD-2026-09-07-291)

The command generators and the churn are the fuzzer's, moved here verbatim in
behaviour: the fuzzer found TD-2026-09-07-291 and its seeds are referenced from
the TD ledger.
"""

from __future__ import annotations

import random
import shutil
import subprocess
import sys
import threading
from pathlib import Path
from typing import Any, Callable

import microide_qa as q

# Every command name the palette registers, so the sweep cannot silently stop
# covering a verb that was added after it was written. Regenerate with:
#   grep -oE 'ActionId::[A-Za-z]+, "[a-z0-9-]+"' \
#       src/workspace/registries/WorkspaceCommandRegistry.cpp | sed 's/.*, "//;s/"//'
# Deliberately excluded: `quit` (ends the run), `project-open`/`project-close`
# (re-roots the session away from the scratch copy), and the `debug-`/`tests-`
# verbs that launch external processes.
MIXED_VERBS = """
about add-cursor-all-matches add-cursor-next-match branch-review-note breakpoint-clear
close-group code-actions colorscheme command-palette compare compare-clipboard compare-files
completion copy copy-line-down copy-line-up cut delete-line files find find-next find-previous
find-references focus focus-group-down focus-group-left focus-group-right focus-group-up
focus-other-group fold fold-all format-document format-json git-refresh goto goto-declaration
goto-definition goto-implementation goto-type-definition indent-lines indent-width
insert-line-above insert-line-below insert-snippet jump jump-to-matching-bracket
keyboard-shortcuts layout-mode-toggle mark-branch-file-reviewed mark-branch-hunk-reviewed merge
move-group-down move-group-left move-group-right move-group-up move-line-down move-line-up
next-diagnostic open outdent-lines paste plugins-reload previous-diagnostic project-search redo
rename-symbol reopen reveal-in-tree review-branch review-commit review-conflicts save search
select-all settings sidebar-close sidebar-hide sidebar-show sidebar-toggle signature-help
soft-tabs sort-lines-ascending sort-lines-descending split-down split-right status-bar-toggle
tab tabmove tab-size tabswitch term term-close terminal-find toggle-block-comment
toggle-editor-add-cursor-at-match toggle-editor-auto-close toggle-editor-folding
toggle-editor-indent-guides toggle-editor-line-ops toggle-editor-render-whitespace
toggle-editor-save-trim toggle-editor-smart-indent toggle-editor-snippets
toggle-editor-sticky-scroll toggle-editor-surround toggle-fold toggle-line-comment toggle-theme
tree tree-refresh type undo unfold unfold-all unmark-branch-file-reviewed
unmark-branch-hunk-reviewed workspace-symbol wrap
""".split()

# Arguments chosen to hit the parse and clamp paths: empty, out of range, wrong
# type, whitespace-only, quoted, traversal, and a format specifier.
MIXED_ARGS = ["", "0", "-1", "1", "999999", "abc", "on", "off", "HEAD", "HEAD~2", "list",
              "  ", "a b", '"q"', "..", "/etc/passwd", "%s", "\\n"]

# Text that has broken layout before: wide glyphs, combining marks, a long line.
EDITOR_TEXT = ["alpha", "b", "  ", "é", "你好", "\U0001F600", "x" * 200, "\t", "}", "//"]

# What the pointer must never press: it ends the run or opens a native dialog that
# blocks the event loop on a portal nobody answers under Xvfb.
FORBIDDEN_ELEMENTS = ("button:window/close", "button:window/minimize")
FORBIDDEN_LABELS = ("Quit", "Exit", "Open File", "Open Folder", "Save As", "Close Window",
                    "Open Font", "Choose Font")
FORBIDDEN_BINDINGS = ("quit", "open-file", "open-folder", "project-open", "save-as")
# Settings whose rows (and their checkboxes) the pointer must not press: turning
# `control.enabled` off in the Settings overlay stops the very channel the monkey
# drives the app through, and every later query fails (seed 1, 2026-10-25).
FORBIDDEN_SETTINGS = ("control.enabled",)


def project_files(root: Path) -> list[str]:
    """A handful of real files plus paths that do not resolve."""
    tracked = [
        "README.md", "CHANGELOG.md", "AGENTS.md", "CLAUDE.md",
        "src/util/Parse.h", "src/editor/TextLayoutCache.cpp",
        "src/terminal/TerminalSession.cpp", "tests/TextViewportTests.cpp",
        # The taskflow fixture's files, when that is the project.
        "src/scheduler.cpp", "src/main.cpp", "include/scheduler.h", "CMakeLists.txt",
    ]
    present = [name for name in tracked if (root / name).exists()]
    # A directory, a missing file and an empty argument are all reachable from the
    # tree and the palette, so they belong in the stream.
    return present + ["src", "does/not/exist.txt", ""]


def mixed_command(rng: random.Random, files: list[str]) -> str:
    parts = [rng.choice(MIXED_VERBS)]
    for _ in range(rng.choice([0, 0, 1, 1, 2])):
        parts.append(rng.choice(MIXED_ARGS + files) if rng.random() < 0.6 else rng.choice(files))
    return " ".join(part for part in parts if part)


def make_editor_generator() -> Callable[[random.Random, list[str]], str]:
    """Editor/pane commands, with the split-and-close BURST emitted as a unit.

    Purely independent draws do not reach the interesting states often enough.
    Splitting a pane copies the viewport (and its caches) into the new group and
    closing a group destroys viewports, so "split, populate both panes, close one,
    keep drawing the other" is the sequence that exercises copied-cache lifetime --
    and it was the sequence that crashed. At one command per draw a run rarely
    lands the five in order; emitted as a burst it happens several times a run.
    """
    pending: list[str] = []

    def next_command(rng: random.Random, files: list[str]) -> str:
        if pending:
            return pending.pop(0)
        real = [name for name in files if name and ("/" in name or name.endswith(".md"))]
        target = rng.choice(real) if real else "README.md"
        choice = rng.randrange(24)
        if choice < 4:
            return f"open {target}"
        if choice in (4, 5):
            other = rng.choice(real) if real else "README.md"
            pending.extend([
                f"open {other}",                    # populate the new pane's caches
                f"goto {rng.randrange(1, 2000)}",   # ...and lay its visible rows out
                "focus-other-group",
                "close-group",                      # destroy one side's viewports
                f"goto {rng.randrange(1, 2000)}",   # the survivor draws again
            ])
            return "split-right" if choice == 4 else "split-down"
        return {
            6: "close-group", 7: "focus-other-group",
            8: f"tabmove {rng.randrange(-3, 4)}", 9: f"tabswitch {rng.randrange(-3, 4)}",
            10: "reopen", 11: f"goto {rng.randrange(1, 3000)}",
            12: f"type {rng.choice(EDITOR_TEXT)}", 13: "select-all", 14: "undo", 15: "redo",
            16: "wrap", 17: f"tab-size {rng.randrange(1, 17)}", 18: "toggle-editor-folding",
            19: "fold-all", 20: "unfold-all", 21: f"compare {target}",
            22: "add-cursor-all-matches",
        }.get(choice, f"search {rng.choice(['a', 'the', 'void', 'zzz'])}")

    return next_command


def copy_project(source: Path, destination: Path) -> None:
    """A throwaway copy of a project to drive.

    The command stream saves, sorts, comments and deletes lines in whatever it
    opens, so it must never be pointed at a tree anyone cares about. For a git
    worktree this is a local clone, which is both fast and exactly the right
    filter -- it carries the tracked files and a real `.git` (the git verbs need
    one) and leaves the ignored build output behind. `--no-hardlinks` so nothing
    the run does can reach the source repository's objects.
    """
    if (source / ".git").exists():
        result = subprocess.run(
            ["git", "clone", "--local", "--no-hardlinks", "--quiet", str(source),
             str(destination)], capture_output=True, text=True)
        if result.returncode == 0:
            return
        print(f"git clone of {source} failed, falling back to a full copy: "
              f"{result.stderr.strip()}", file=sys.stderr)
    shutil.copytree(source, destination, symlinks=True, ignore_dangling_symlinks=True)


class ExternalChurn:
    """Rewrites files under the project while the run goes on.

    Not decoration: this is what the fuzzer's first real find needed. The
    layout-cache use-after-free lives on the viewport COPY path, and the ordinary
    way a viewport gets copied is a reload -- the file watcher noticing that a file
    changed under an open buffer. A checkout being rewritten under the editor (a
    branch switch, a formatter, a build tree) is the normal case, so the run
    generates it rather than depending on the operator's background noise.
    """

    def __init__(self, project: Path, rng: random.Random,
                 record: Callable[[dict[str, Any]], None] | None = None):
        self._project = project
        self._rng = rng
        # Each rewrite goes into the run's action list as well, so a replay
        # makes the same change between the same two commands: a finding that
        # needs a reload under it does not reproduce from the commands alone.
        self._record = record
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        candidates = ("README.md", "CHANGELOG.md", "AGENTS.md", "CLAUDE.md",
                      "src/scheduler.cpp", "src/main.cpp")
        self._targets = [project / name for name in candidates if (project / name).exists()]

    def __enter__(self) -> "ExternalChurn":
        if self._targets:
            self._thread.start()
        return self

    def __exit__(self, *unused: Any) -> None:
        self._stop.set()
        if self._thread.is_alive():
            self._thread.join(timeout=10)

    def _run(self) -> None:
        counter = 0
        while not self._stop.wait(0.25):
            counter += 1
            target = self._rng.choice(self._targets)
            relative = str(target.relative_to(self._project))
            apply_churn(self._project, relative, counter)
            if self._record is not None:
                self._record({"churn": relative, "counter": counter})


def apply_churn(project: Path, relative: str, counter: int) -> None:
    """One step of ExternalChurn, and what a replay does for its `churn` rows."""
    churn_dir = project / "churn"
    churn_dir.mkdir(exist_ok=True)
    try:
        target = project / relative
        text = target.read_text(errors="replace")
        # A whole-file rewrite, which is what a checkout or a formatter does; an
        # open buffer over it reloads.
        target.write_text(f"churn {counter}\n" + text)
        # New and removed paths as well, so the index watcher is doing more than
        # noticing one mtime.
        (churn_dir / f"file{counter % 32}.txt").write_text(f"{counter}\n")
        stale = churn_dir / f"file{(counter + 16) % 32}.txt"
        if stale.exists():
            stale.unlink()
    except OSError:
        pass


# xdotool key names for the characters and keys SDL names differently.
_XDOTOOL_KEYS = {
    "`": "grave", "=": "equal", "-": "minus", "[": "bracketleft", "]": "bracketright",
    "/": "slash", "\\": "backslash", ";": "semicolon", "'": "apostrophe", ",": "comma",
    ".": "period", "Enter": "Return", "PageUp": "Prior", "PageDown": "Next",
    "Backspace": "BackSpace", "Space": "space", "Keypad +": "KP_Add", "Keypad -": "KP_Subtract",
}


def xdotool_chord(chord: str) -> str | None:
    """`Ctrl+Shift+F` (the app's spelling) as an xdotool chord, or None."""
    parts = chord.split("+")
    # "Ctrl++" splits into a trailing empty pair.
    if chord.endswith("++"):
        parts = parts[:-2] + ["+"]
    *mods, key = parts
    if not key:
        return None
    names = {"Ctrl": "ctrl", "Shift": "shift", "Alt": "alt", "Super": "super"}
    out = [names[m] for m in mods if m in names]
    if len(out) != len(mods):
        return None
    if key == "+":
        key = "plus"
    key = _XDOTOOL_KEYS.get(key, key)
    if len(key) == 1:
        key = key.lower()
    return "+".join(out + [key])


class Monkey:
    def __init__(self, app: q.App, seed: int, profile: str = "default", project: Path | None = None):
        self.app = app
        self.rng = random.Random(seed)
        self.profile = profile
        self.files = project_files(project) if project is not None else []
        self.editor_generator = make_editor_generator()
        self._chords: list[str] | None = None
        self.sources: list[tuple[float, Callable[[], None]]] = [
            (50, self.element_action),
            (15, self.chord_action),
            (10, self.command_action),
            (10, self.type_action),
            (5, self.drag_action),
            (5, self.wheel_action),
            (5, self.point_action),
        ]

    # ---- action sources --------------------------------------------------

    @staticmethod
    def _forbidden(element: dict[str, Any]) -> bool:
        name = element.get("name", "")
        label = element.get("label", "") or name.rsplit("/", 1)[-1]
        return (name in FORBIDDEN_ELEMENTS or any(label.startswith(bad) for bad in FORBIDDEN_LABELS)
                or any(setting in name for setting in FORBIDDEN_SETTINGS))

    def _safe_elements(self) -> list[dict[str, Any]]:
        out = []
        for element in self.app.ui().get("elements", []):
            if self._forbidden(element) or element.get("enabled") is False:
                continue
            out.append(element)
        return out

    def element_action(self) -> None:
        elements = self._safe_elements()
        if not elements:
            return self.point_action()
        element = self.rng.choice(elements)
        x, y = self.app.center(element)
        if self.rng.random() < 0.3:
            self.app.move(x, y)
        else:
            self.app.click(x, y, clicks=2 if self.rng.random() < 0.1 else 1)

    def chord_action(self) -> None:
        if self._chords is None:
            self._chords = []
            for binding in self.app.keybindings():
                if binding.get("id") in FORBIDDEN_BINDINGS or binding.get("plugin"):
                    continue
                chord = xdotool_chord(binding.get("chord", ""))
                if chord:
                    self._chords.append(chord)
            self._chords += ["Escape", "Return", "Tab", "Up", "Down", "Left", "Right"]
        self.app.key(self.rng.choice(self._chords))

    def command_action(self) -> None:
        if self.profile == "editor":
            command = self.editor_generator(self.rng, self.files)
        else:
            command = mixed_command(self.rng, self.files)
        self.app.send_line(command)

    def type_action(self) -> None:
        self.app.type(self.rng.choice(["alpha", "b", "  ", "x" * 40, "}", "//", "\n"]))

    def drag_action(self) -> None:
        handles = [e for e in self._safe_elements() if e["kind"] in ("divider", "tab", "panel-tab")]
        if not handles:
            return self.wheel_action()
        element = self.rng.choice(handles)
        x0, y0 = self.app.center(element)
        dx = self.rng.randint(-120, 120)
        dy = self.rng.randint(-80, 80) if element["kind"] == "divider" else 0
        self.app.drag(x0, y0, x0 + dx, y0 + dy)

    def wheel_action(self) -> None:
        w, h = self.app.size
        self.app.wheel(self.rng.randrange(40, w - 40), self.rng.randrange(60, h - 40),
                       self.rng.choice([-5, -2, -1, 1, 2, 5]))

    def point_action(self) -> None:
        w, h = self.app.size
        # Below the menu bar's window controls: a random click there could close
        # the window. And never on a forbidden element (a menu's Quit row, the
        # control channel's setting) -- a random point used to land on them.
        forbidden = [element.get("rect") for element in self.app.ui().get("elements", [])
                     if self._forbidden(element) and element.get("rect")]

        def blocked(x: int, y: int) -> bool:
            return any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh in forbidden)
        for _ in range(20):
            x, y = self.rng.randrange(0, w - 160), self.rng.randrange(30, h)
            if not blocked(x, y):
                self.app.click(x, y)
                return

    # ---- driving ---------------------------------------------------------

    def step(self) -> None:
        if self.profile in ("commands", "editor"):
            self.command_action()
            return
        total = sum(weight for weight, _ in self.sources)
        pick = self.rng.uniform(0, total)
        for weight, source in self.sources:
            pick -= weight
            if pick <= 0:
                source()
                return
        self.sources[-1][1]()

    def run(self, steps: int, settle: float = 0.12) -> bool:
        """Runs `steps` actions; stops at the first finding. True when clean."""
        for index in range(steps):
            try:
                self.step()
            except q.QaFailure as failure:
                self.app.expect(False, f"step {index}: {failure}")
            self.app.settle(settle)
            if self.app.check(f"monkey step {index}") or not self.app.alive():
                return False
        return not self.app.findings
