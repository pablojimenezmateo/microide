"""The seeded monkey: coverage-steered random actions over the running window.

Each step draws an action, runs it through the App (which records it for the
replay file), settles until the app's log is quiet, and checks every oracle: the
App's always-on ones (crash, the redraw verifier's QA-STALE, sanitizer and
allocator reports, focus/bounds/overlap, the chrome-vs-buffer state oracle) and
the monkey's own (monkey_oracles: bytes on disk around saves and kill -9s, undo
algebra, leaks, control latency). See dev-docs/project/safety-plan.md W1.

**Reach.** Actions come from weighted sources (a profile sets the weights):

  element   a named on-screen element: click, hover, double/triple, right,
            middle and modifier clicks
  text      clicks and drags inside the editor's text: shift/alt/ctrl clicks,
            word/line selection, drag-select, column select, text drag and drop
  chord     the app's own keybindings (`keybindings` query) and navigation keys
  command   control commands from the live catalogue with hostile arguments
  type      typing: words, code, Unicode (CJK, emoji, combining marks, RTL),
            tabs; long lines and pastes go through the paste path
  burst     key repeat (the same key 5-40 times) or several actions with no
            settle and no check between (timing stress)
  drag      dividers, tabs and panel tabs
  wheel     scrolling, with Ctrl (zoom) and Shift (horizontal)
  point     a free click anywhere safe
  setting   `set-setting` over the live catalogue (ui.scale, theme, wrap, ...)
  window    resize from tiny to the screen, focus loss
  terminal  shell commands in the panel's terminal (output floods, alternate
            screen, titles, a pager, Ctrl+C)
  file      open or drop the odd files seeded under qa-weird/ (CRLF, mixed,
            Latin-1, binary, a long line, Unicode names)
  external  the outside world: rewrites, creates, renames, deletes, chmods,
            symlinks (loops, dangling) and git (commit, branch switch, stash,
            a conflicting merge), recorded so a replay redoes them
  kill9     SIGKILL and relaunch on the same profile (the data-loss oracle)

**Steering.** A state signature per step (monkey_model.state_signature) feeds a
coverage model: within a source, an action already tried in this state is drawn
less often; a source whose actions keep reaching unseen states or transitions is
drawn more. With `--coverage` the model is the soak runner's, merged across
runs; with `--corpus` a run may start by replaying a saved prefix that reached a
rare state, and saves the prefixes of the states it reaches first.

**Findings.** A finding is recorded and exploration continues; a crash is
recorded and the app relaunched on the same profile (which exercises session
restore). Findings are deduplicated per run by signature (findings.py) and
listed in `findings.json` beside the evidence directories.

The `commands` and `editor` profiles are the former command fuzzer's, kept as
they were: TD-2026-09-07-291 and its seeds are referenced from the TD ledger.
"""

from __future__ import annotations

import contextlib
import json
import os
import random
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Callable

import findings as fnd
import microide_qa as q
import monkey_model as mm
import diff_variants as dv
import monkey_oracles as mo
import remote_host as rh

# Every command name the palette registers, so the sweep cannot silently stop
# covering a verb that was added after it was written. Regenerate with:
#   grep -oE 'ActionId::[A-Za-z]+, "[a-z0-9-]+"' \
#       src/workspace/registries/WorkspaceCommandRegistry.cpp | sed 's/.*, "//;s/"//'
# Deliberately excluded: `quit` (ends the run), `project-open`/`project-close`
# (re-roots the session away from the scratch copy), and the `debug-`/`tests-`
# verbs that launch external processes. The default profile reads the live
# catalogue instead (the `commands` query) and excludes FORBIDDEN_COMMANDS.
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
tab tabmove tab-size tabswitch term term-close term-select-all terminal-find toggle-block-comment
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

# What the monkey types, by class (the class is part of the action key, so the
# steering spreads over classes per state).
TEXT_CLASSES: dict[str, list[str]] = {
    "word": ["alpha", "b", "foo bar", "  ", "x" * 40, "}", "{", "//", "\n", "(", "\"", "'", "<", ">",
             "#include", "int main() {\n", "return 0;", "\n\n", "    ", ";", "[]", "TODO"],
    "unicode": ["é", "ñandú", "Grüße", "你好世界", "日本語のテキスト", "\U0001F600", "👍🏽", "👨‍👩‍👧",
                "e\u0301", "a\u0308\u0304", "مرحبا بالعالم", "שלום", "\u200b", "Ω≈ç√∫", "ﬁ", "𝔘𝔫𝔦"],
    "control": ["\t", "\t\t", "\n", "\n\n\n"],
}
# Pasted through the control channel's `type` (the paste path): too long to type
# a key at a time, or carrying characters no key does (a CR).
PASTE_TEXTS = [
    "x" * 3000,
    "word " * 800,
    "".join(f"line {i}\n" for i in range(400)),
    "one\r\ntwo\r\nthree\r\n",
    "mixed\r\nends\nhere\r",
    "\t".join(["col"] * 60),
    "你好" * 500,
    "a\u0300" * 300,
    "\U0001F600" * 200,
]
# What only another application leaves on the clipboard: what the app's own copy
# never produces (App.foreign_clipboard; surface-extra.toml).
FOREIGN_CLIPBOARD_TEXTS = [
    "",
    "\r\n",
    "a\rb\rc",
    "x" * 200_000,
    "path/with spaces/and\ttab.txt\n",
    "\u00a0non-breaking\u00a0spaces\u00a0",
    "trailing whitespace   \n\t\n",
    "\ufeffbom at the start",
    "line\u2028separator\u2029paragraph",
]

# What the pointer must never press: it ends the run or opens a native dialog that
# blocks the event loop on a portal nobody answers under Xvfb.
# Maximize too: the harness, not a window manager, owns the window's size, and
# Maximize sits beside Close -- a press aimed at it from a layout read before a
# resize or a scale change settled landed on Close and ended the run.
FORBIDDEN_ELEMENTS = ("button:window/close", "button:window/minimize", "button:window/maximize")
FORBIDDEN_LABELS = ("Quit", "Exit", "Open File", "Open Folder", "Save As", "Close Window",
                    "Open Font", "Choose Font")
FORBIDDEN_BINDINGS = ("quit", "open-file", "open-folder", "project-open", "save-as")
# Settings whose rows (and their checkboxes) the pointer must not press: turning
# `control.enabled` off in the Settings overlay stops the very channel the monkey
# drives the app through, and every later query fails (seed 1, 2026-10-25).
FORBIDDEN_SETTINGS = ("control.enabled",)
# Commands the default profile's command source never sends: they end the run,
# re-root the session away from the scratch copy, open a native dialog, or start
# an external debugger/test process the fixture has no adapter for.
FORBIDDEN_COMMANDS = {"quit", "project-open", "project-close", "open-file", "open-folder", "save-as",
                      "set-setting", "key", "pointer", "restart"}
FORBIDDEN_COMMAND_PREFIXES = ("debug-", "tests-", "remote-", "breakpoint-")
# `set-setting` never touches these: the control channel, the session restore the
# kill -9 oracle relies on, and the remote settings (no host here).
FORBIDDEN_SETTING_PREFIXES = ("control.", "session.restore_on_launch", "remote.")
# Settings drawn more often: the ones whose changes reshape layout and painting.
INTERESTING_SETTINGS = ("ui.scale", "editor.wrap", "editor.font_size", "editor.colorscheme",
                        "editor.fold.sticky_scroll.enabled", "editor.tab_size", "ui.layout_mode",
                        "editor.line_numbers", "editor.view.render_whitespace", "plugins.code_lens_above",
                        "ui.show_status_bar", "terminal.font_size", "editor.inlay_hints.enabled",
                        "editor.autosave", "editor.save.trim_trailing_whitespace",
                        "editor.save.ensure_final_newline", "editor.line_endings")
STRING_VALUES = ["", "x", "  ", "/nonexistent", "monospace", "DejaVu Sans Mono", "*.md", "src", "%s", "a,b"]
SCALE_VALUES = ["1", "1.25", "1.5", "1.75", "2", "0.5", "3", "1.1"]

# Shell lines for the terminal source.
TERMINAL_LINES = [
    "ls -la\n", "echo héllo wörld 你好\n", "printf '\\033[31mred\\033[0m \\033[1mbold\\033[0m\\n'\n",
    "seq 1 400\n", "cat README.md\n", "yes | head -n 3000\n", "clear\n", "tput cols; tput lines\n",
    "printf '\\033]0;title-%s\\007' $RANDOM\n", "printf '\\033[?1049h\\033[2J alt screen'; sleep 0.4; "
    "printf '\\033[?1049l'\n", "less README.md\n", "q", "printf '%0.s-' $(seq 1 500); echo\n",
    "printf 'no newline at the end'\n", "for i in $(seq 1 30); do echo $i; sleep 0.01; done\n",
    "git status\n", "git log --oneline | head\n", "printf '\\t|\\ttabs\\n'\n", "echo $COLUMNS\n",
]


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


def mixed_command(rng: random.Random, files: list[str], verbs: list[str] | None = None) -> str:
    parts = [rng.choice(verbs or MIXED_VERBS)]
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


def git_env(project: Path | None = None) -> dict[str, str]:
    """The environment for git run against a scratch project: never one a hook or
    bisect exported (GIT_DIR and friends would point every call at the source
    repository).

    With `project`, git is also fenced to it: a ceiling at its parent stops git's
    upward search at the project, and only local transports are allowed. Scratch
    projects live under build/, inside the microide checkout, so a project whose
    `.git` the run broke (the app deletes it, a stash directory is created in its
    place) otherwise resolves to the CHECKOUT -- which is how a soak run once
    committed "qa commit 29" to main, switched the checkout's branch and pushed a
    qa-branch to GitHub."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update({"GIT_AUTHOR_NAME": "QA Monkey", "GIT_AUTHOR_EMAIL": "qa@example.com",
                "GIT_COMMITTER_NAME": "QA Monkey", "GIT_COMMITTER_EMAIL": "qa@example.com",
                "GIT_TERMINAL_PROMPT": "0"})
    if project is not None:
        env.update(q.git_fence_env(project))
    return env


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
             str(destination)], capture_output=True, text=True, env=git_env())
        if result.returncode == 0:
            detach_from_source(destination)
            return
        print(f"git clone of {source} failed, falling back to a full copy: "
              f"{result.stderr.strip()}", file=sys.stderr)
    shutil.copytree(source, destination, symlinks=True, ignore_dangling_symlinks=True)


def detach_from_source(clone: Path) -> None:
    """Point the clone's `origin` at an empty bare repository beside it.

    A clone's origin is the repository it came from, and for the commands profile
    that is the microide checkout: the app's Push from the scratch project would
    write refs straight into it (and a worktree's refs are the main checkout's).
    The bare stand-in keeps push, fetch and upstream tracking exercisable."""
    remote = clone.parent / (clone.name + "-origin.git")
    env = git_env(clone)
    subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True,
                   capture_output=True, env=env)
    subprocess.run(["git", "-C", str(clone), "remote", "set-url", "origin", str(remote)],
                   check=True, capture_output=True, env=env)


# ---- the odd files under qa-weird/ ---------------------------------------------

WEIRD_DIR = "qa-weird"
WEIRD_OPENABLE = ["crlf.txt", "mixed.txt", "latin1.txt", "bom.txt", "binary.bin", "nul-in-text.txt",
                  "long-line.txt", "no-final-newline.txt", "empty.txt", "many-lines.txt", "wide-chars.txt",
                  "rtl.txt", "tabs.txt", "trailing-ws.txt", "ünï cødé/文件夹/名前 with spaces.txt",
                  "code.py", "data.json", "link-to-crlf.txt", "unreadable.txt", "dangling", "loop"]


def seed_weird_files(project: Path) -> list[str]:
    """Files no fixture has: line-ending and encoding cases, a binary, a long
    line, Unicode names, a deep tree, symlinks (one a loop, one dangling) and an
    unreadable file. Deterministic, so a corpus prefix means the same thing on
    every fixture. Returns the openable paths, relative to the project."""
    root = project / WEIRD_DIR
    if not root.exists():
        root.mkdir()
        (root / "crlf.txt").write_bytes(b"first line\r\nsecond line\r\n\tindented\r\n")
        (root / "mixed.txt").write_bytes(b"unix\nwindows\r\nold mac\rend\n")
        (root / "latin1.txt").write_bytes("caf\xe9 na\xefve r\xe9sum\xe9\n".encode("latin-1"))
        (root / "bom.txt").write_bytes(b"\xef\xbb\xbfwith a byte order mark\n")
        (root / "binary.bin").write_bytes(bytes(range(256)) * 8)
        (root / "nul-in-text.txt").write_bytes(b"text\x00with a nul\nsecond\n")
        (root / "long-line.txt").write_text("y" * 300_000 + "\n")
        (root / "no-final-newline.txt").write_text("no newline at the end")
        (root / "empty.txt").write_text("")
        (root / "many-lines.txt").write_text("".join(f"{i:06d} the quick brown fox\n" for i in range(60_000)))
        (root / "wide-chars.txt").write_text("你好世界 " * 40 + "\n" + "\U0001F600" * 80 + "\n" + "e\u0301" * 50 + "\n")
        (root / "rtl.txt").write_text("مرحبا بالعالم\nשלום עולם\nmixed English عربي text\n")
        (root / "tabs.txt").write_text("\tone\n\t\ttwo\n  \t mixed\n")
        (root / "trailing-ws.txt").write_text("trailing   \nspaces \t\n")
        unicode_dir = root / "ünï cødé" / "文件夹"
        unicode_dir.mkdir(parents=True)
        (unicode_dir / "名前 with spaces.txt").write_text("unicode path\n")
        deep = root
        for depth in range(14):
            deep = deep / f"d{depth}"
        deep.mkdir(parents=True)
        (deep / "deep.txt").write_text("deep\n")
        (root / "code.py").write_text("def f(x):\n    if x:\n        return [i for i in range(x)]\n    return None\n")
        (root / "data.json").write_text('{"a": [1, 2, {"b": null}], "c": "\\u00e9"}\n')
        for target, link in (("crlf.txt", "link-to-crlf.txt"), (".", "loop"), ("does-not-exist", "dangling")):
            try:
                os.symlink(target, root / link)
            except OSError:
                pass
        unreadable = root / "unreadable.txt"
        unreadable.write_text("secret\n")
        try:
            os.chmod(unreadable, 0)
        except OSError:
            pass
    return [f"{WEIRD_DIR}/{name}" for name in WEIRD_OPENABLE]


# ---- the outside world ----------------------------------------------------------

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


EXTERNAL_OPS = ("rewrite", "append", "truncate", "crlf", "create-tree", "delete-tree", "rename-file",
                "rename-back", "chmod-ro", "chmod-none", "chmod-restore", "symlink", "symlink-loop",
                "binary", "git-commit", "git-switch", "git-switch-back", "git-stash", "git-stash-pop",
                "git-merge-conflict", "git-merge-abort", "git-checkout-file", "delete-file", "restore-file")


def apply_external(project: Path, op: str, target: str, counter: int) -> None:
    """One outside-world change, deterministic in (op, target, counter) so a
    replay redoes it. `target` is a project-relative file path."""
    path = project / target
    env = git_env(project)
    stash = project / ".git" / "qa-deleted"

    def git(*args: str) -> None:
        subprocess.run(["git", "-C", str(project), *args], capture_output=True, env=env, timeout=30)

    try:
        if op == "compile-flags":
            # What clangd needs to resolve the fixture's includes (flows_lsp_debug._lsp_ready).
            (project / "compile_flags.txt").write_text("-Iinclude\n-std=c++17\n")
        elif op == "diff-variant":
            # Two versions of one generated source, for compare-files: the
            # structural diff's paths (diff_variants.py). Six slots, reused.
            language, left, right = dv.variant_pair(counter)
            folder = project / "qa-diff"
            folder.mkdir(parents=True, exist_ok=True)
            (folder / f"v{counter % 6}.left.{language}").write_text(left)
            (folder / f"v{counter % 6}.right.{language}").write_text(right)
        elif op == "rewrite" and path.is_file():
            path.write_text(f"rewritten {counter}\n" + path.read_text(errors="replace"))
        elif op == "append" and path.is_file():
            with open(path, "a") as out:
                out.write(f"appended {counter}\n")
        elif op == "truncate" and path.is_file():
            path.write_text("")
        elif op == "crlf" and path.is_file():
            path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        elif op == "binary" and path.parent.is_dir():
            path.write_bytes(bytes((counter * 7 + i) % 256 for i in range(4096)))
        elif op == "create-tree":
            tree = project / "qa-ext" / f"t{counter % 8}"
            for depth in range(counter % 5 + 1):
                tree = tree / f"n{depth}"
            tree.mkdir(parents=True, exist_ok=True)
            (tree / f"file{counter}.txt").write_text(f"{counter}\n")
        elif op == "delete-tree":
            shutil.rmtree(project / "qa-ext" / f"t{counter % 8}", ignore_errors=True)
        elif op == "rename-file" and path.is_file():
            path.rename(path.with_name(path.name + ".renamed"))
        elif op == "rename-back":
            renamed = path.with_name(path.name + ".renamed")
            if renamed.is_file() and not path.exists():
                renamed.rename(path)
        elif op == "delete-file" and path.is_file():
            stash.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, stash / path.name)
            path.unlink()
        elif op == "restore-file":
            if (stash / path.name).is_file() and not path.exists():
                shutil.copy2(stash / path.name, path)
        elif op == "chmod-ro" and path.is_file():
            os.chmod(path, 0o444)
        elif op == "chmod-none" and path.is_file():
            os.chmod(path, 0)
        elif op == "chmod-restore" and path.exists():
            os.chmod(path, 0o644)
        elif op == "symlink":
            link = project / "qa-ext" / f"link{counter % 4}"
            link.parent.mkdir(parents=True, exist_ok=True)
            if not link.is_symlink():
                os.symlink(path, link)
        elif op == "symlink-loop":
            loop = project / "qa-ext" / f"loop{counter % 3}"
            loop.parent.mkdir(parents=True, exist_ok=True)
            if not loop.is_symlink():
                os.symlink("..", loop)
        elif op == "git-commit":
            git("add", "-A")
            git("commit", "-q", "-m", f"qa commit {counter}", "--no-verify")
        elif op == "git-switch":
            git("switch", "-q", "-c", f"qa-branch-{counter % 4}")
            git("switch", "-q", f"qa-branch-{counter % 4}")
        elif op == "git-switch-back":
            git("switch", "-q", "main")
        elif op == "git-stash":
            git("stash", "-q", "-u")
        elif op == "git-stash-pop":
            git("stash", "pop", "-q")
        elif op == "git-merge-conflict":
            # The taskflow fixture's branch rewrites the same lines as main.
            git("merge", "-q", "--no-edit", "feature/stable-sort")
        elif op == "git-merge-abort":
            git("merge", "--abort")
        elif op == "git-checkout-file" and path.is_file():
            git("checkout", "-q", "--", target)
    except (OSError, subprocess.SubprocessError, ValueError):
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


# ---- profiles --------------------------------------------------------------------

# Base weights per source. A profile is a focus, not a filter: a focused run
# still wanders through every source it lists.
PROFILES: dict[str, dict[str, float]] = {
    "default": {"element": 28, "text": 8, "chord": 12, "command": 7, "type": 10, "burst": 5, "drag": 4,
                "wheel": 4, "point": 3, "setting": 3, "window": 2, "terminal": 2, "file": 3,
                "external": 2, "kill9": 0.4},
    # Editing: text, keys, typing and saves; the data-loss oracles get the most to check.
    "editing": {"element": 8, "text": 20, "chord": 20, "type": 22, "burst": 10, "command": 6, "file": 4,
                "wheel": 3, "external": 3, "kill9": 0.6, "setting": 2},
    # Layout and painting: splits, drags, resizes, zoom and fractional scales (W7).
    "layout": {"element": 20, "drag": 12, "window": 8, "setting": 10, "wheel": 8, "command": 8,
               "chord": 8, "text": 6, "type": 4, "point": 4, "burst": 4},
    # The outside world: git, renames, permissions, churn under open buffers.
    "world": {"element": 16, "external": 18, "file": 8, "command": 8, "chord": 8, "type": 8, "text": 6,
              "kill9": 1.0, "terminal": 3},
    "terminal": {"terminal": 30, "element": 14, "chord": 12, "type": 10, "burst": 6, "window": 4,
                 "command": 6, "wheel": 6, "setting": 2},
    "settings": {"setting": 25, "element": 25, "chord": 10, "type": 8, "text": 6, "window": 4, "command": 6},
    # Language servers and the debugger, through their plugins (PROFILE_PLUGINS):
    # clangd answering at random positions, gdb running the fixture's program.
    "tooling": {"tooling": 18, "type": 12, "text": 8, "chord": 12, "element": 16, "burst": 4, "wheel": 4,
                "command": 6, "point": 4, "file": 3},
    # A remote project over a loopback ssh host of the run's own (remote_host.py):
    # editing the mirror while the host's files change, the link is cut and the
    # host's server killed; the convergence oracle compares the two trees.
    "remote": {"remote": 8, "external": 8, "type": 14, "text": 8, "chord": 12, "element": 14, "burst": 4,
               "wheel": 4, "command": 6, "file": 6, "terminal": 3},
    # Diffs: structural refinement over generated code, typed into on the editable side.
    "diff": {"diff": 14, "type": 16, "text": 10, "chord": 12, "element": 14, "burst": 6, "wheel": 6,
             "command": 4, "setting": 2, "window": 2},
}
# Plugins a profile's runs load (tools/qa/microide_qa.py App.launch copies them
# in; the replay header records them, so a replay loads the same). Every other
# profile runs with none -- which is why no soak run had ever had a language
# server, a debug adapter or the Lua plugin host behind its commands.
PROFILE_PLUGINS: dict[str, tuple[str, ...]] = {
    "tooling": ("cpp-lsp", "gdb-dap", "todo-highlight", "eol-annotations", "language-tools", "qa-tests"),
}
# Where the taskflow fixture's code is, for language-server and debugger actions:
# (file, lines it has) -- breakpoints land on real lines, positions inside them.
TOOLING_FILES = (("src/scheduler.cpp", 26), ("src/main.cpp", 16), ("include/scheduler.hpp", 26))
TOOLING_FUNCTIONS = ("taskflow::Scheduler::run", "taskflow::Scheduler::add", "main", "nosuchfunction")
LSP_COMMANDS = ("goto-definition", "goto-declaration", "goto-type-definition", "goto-implementation",
                "find-references", "completion", "signature-help", "show-hover", "code-actions",
                "format-document", "next-problem", "goto-symbol")
DEBUG_COMMANDS = ("debug-continue", "debug-step-over", "debug-step-in", "debug-step-out", "debug-pause",
                  "debug-stop", "debug-restart", "debug-step-back", "debug-reverse-continue",
                  "debug-switch-session", "debug-stop-all", "debug-pane-callstack", "debug-pane-variables",
                  "debug-pane-watch", "debug-repl", "debug-show-output", "debug-start-or-continue")
# The remote profile's commands, with arguments that resolve (`qa-local` is the
# run's host; a path argument is added from the project's files).
REMOTE_COMMANDS = ("remote-conflicts", "remote-resolve-all local", "remote-resolve-all remote",
                   "remote-show-log", "remote-status", "remote-disconnect", "remote-reconnect",
                   "remote-reconnect qa-local", "remote-changes", "remote-clear-changes",
                   "remote-apply-deletes", "remote-local-terminal", "remote-forward-port 18080",
                   "remote-stop-server")
COMMAND_PROFILES = ("commands", "editor")
ALL_PROFILES = tuple(PROFILES) + COMMAND_PROFILES


class Monkey:
    def __init__(self, app: q.App, seed: int, profile: str = "default", project: Path | None = None,
                 coverage: mm.Coverage | None = None, corpus: mm.Corpus | None = None,
                 weird: bool = True, max_findings: int = 25, keep_going: bool = True,
                 min_logical: tuple[int, int] | None = None, host: rh.LoopbackHost | None = None):
        self.app = app
        # A remote project's run: `project` is the mirror the editor edits and the
        # host's directory is the outside world -- external changes, churn and the
        # weird files go there and reach the editor through the connection.
        self.host = host
        self.world = host.root if host is not None else project
        # The smallest window, in layout units (pixels / ui.scale), the run may
        # leave the app at; None: any. A resize, a ui.scale or a `ui-scale 999999`
        # that went under it is undone after the step (_keep_window_usable).
        self.min_logical = min_logical
        self.rng = random.Random(seed)
        self.seed = seed
        self.profile = profile
        self.project = project
        self.files = project_files(project) if project is not None else []
        self.weird = seed_weird_files(self.world) if (weird and self.world is not None
                                                      and profile not in COMMAND_PROFILES) else []
        self.editor_generator = make_editor_generator()
        self.coverage = coverage or mm.Coverage()
        self.corpus = corpus or mm.Corpus(None)
        self.max_findings = max_findings
        self.keep_going = keep_going
        self._chords: list[str] | None = None
        self._commands: list[str] | None = None
        self._settings: list[dict[str, Any]] | None = None
        self.counter = 0
        self.state = "start"
        self.signatures: dict[str, dict[str, Any]] = {}
        self.relaunches = 0
        self.step_index = 0
        self.weights = dict(PROFILES.get(profile, PROFILES["default"]))
        self.reward: dict[str, float] = {name: 0.5 for name in self.weights}
        self.sources: dict[str, Callable[[], str]] = {
            "element": self.element_action, "text": self.text_action, "chord": self.chord_action,
            "command": self.command_action, "type": self.type_action, "burst": self.burst_action,
            "drag": self.drag_action, "wheel": self.wheel_action, "point": self.point_action,
            "setting": self.setting_action, "window": self.window_action,
            "terminal": self.terminal_action, "file": self.file_action,
            "external": self.external_action, "kill9": self.kill9_action, "diff": self.diff_action,
            "tooling": self.tooling_action, "remote": self.remote_action,
        }
        self._tooling_ready = False
        self.data = mo.DataOracle(app)
        self.undo = mo.UndoAlgebra(app, random.Random(seed ^ 0x0DD))
        self.resources = mo.ResourceOracle(app)
        self.liveness = mo.Liveness(app)
        self.idle = mo.IdleOracle(app)
        self.structural = mo.StructuralDiffOracle(app)
        self.converge = mo.RemoteConvergence(app, project, host.root, host.open_line()) \
            if host is not None and project is not None else None
        self.new_states = 0
        self.new_transitions = 0

    # ---- choosing -----------------------------------------------------------

    def _pick(self, candidates: list[tuple[str, Any]]) -> tuple[str, Any]:
        """One of (key, payload), an action less tried in this state more likely:
        weight 1 / (1 + times (state, key) was tried)."""
        weights = [1.0 / (1 + self.coverage.pair_count(self.state, key)) for key, _ in candidates]
        total = sum(weights)
        pick = self.rng.uniform(0, total)
        for weight, candidate in zip(weights, candidates):
            pick -= weight
            if pick <= 0:
                return candidate
        return candidates[-1]

    def _menu_open(self) -> bool:
        menu = (self.app.last_ui or {}).get("menu") or {}
        return bool(menu.get("open") or menu.get("contextMenu") or menu.get("overflow"))

    def _choose_source(self) -> str:
        """A source by its profile weight times how well it has paid lately
        (0.5 + 2 x its reward average: never zero, so nothing starves). With a
        menu open the element source is drawn four times as often: a menu the
        next action closes is a menu whose rows are never pressed (a 12-run hunt
        pressed 23 of 167 menu rows)."""
        names = list(self.weights)
        menu_open = self._menu_open()
        weights = [self.weights[name] * (0.5 + 2 * self.reward.get(name, 0.5))
                   * (4.0 if menu_open and name == "element" else 1.0) for name in names]
        total = sum(weights)
        pick = self.rng.uniform(0, total)
        for weight, name in zip(weights, names):
            pick -= weight
            if pick <= 0:
                return name
        return names[-1]

    # ---- action sources -----------------------------------------------------

    @staticmethod
    def _forbidden(element: dict[str, Any]) -> bool:
        name = element.get("name", "")
        label = element.get("label", "") or name.rsplit("/", 1)[-1]
        return (name in FORBIDDEN_ELEMENTS or any(label.startswith(bad) for bad in FORBIDDEN_LABELS)
                or any(setting in name for setting in FORBIDDEN_SETTINGS))

    @classmethod
    def _forbidden_rects(cls, ui: dict[str, Any]) -> list[list[float]]:
        return [element["rect"] for element in ui.get("elements", [])
                if element.get("rect") and cls._forbidden(element)]

    @staticmethod
    def _inside(rects: list[list[float]], x: float, y: float) -> bool:
        return any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh in rects)

    def _press_lands_on_forbidden(self, x: float, y: float) -> bool:
        """App.press_guard while the monkey runs. Every source aims from SOME read
        of the layout -- the text source from the last check's, the element source
        from one taken before it chose -- and the layout can move in between (a
        resize or ui.scale still settling). Read it again at the press, so no
        source can land on Close, whatever it aimed at."""
        try:
            ui = self.app.ui()
        except q.QaFailure:
            return False
        return self._inside(self._forbidden_rects(ui), x, y)

    def _activation_chooses_forbidden(self) -> bool:
        """App.activation_guard while the monkey runs: an open menu (the menu bar's,
        a context menu, the overflow popup) whose keyboard selection is a row the
        run must not choose."""
        try:
            ui = self.app.ui()
        except q.QaFailure:
            return False
        menu = ui.get("menu") or {}
        if not (menu.get("open") or menu.get("contextMenu") or menu.get("overflow")):
            return False
        return any(element.get("kind") == "menu-item" and element.get("active") and self._forbidden(element)
                   for element in ui.get("elements", []))

    def _ui(self) -> dict[str, Any]:
        # The check after the last action read it, and nothing has been sent since:
        # the same screen. A second `ui` query per step was ~5 ms of each
        # (profiled 2026-11-13). The press and activation guards still read afresh.
        if self.app.last_ui is not None and self.app.last_ui_actions == len(self.app.actions):
            return self.app.last_ui
        ui = self.app.ui()
        self.app.last_ui = ui
        self.app.last_ui_actions = len(self.app.actions)
        return ui

    def _safe_elements(self) -> list[dict[str, Any]]:
        out = []
        for element in self._ui().get("elements", []):
            if self._forbidden(element) or element.get("enabled") is False:
                continue
            if element["rect"][2] <= 0 or element["rect"][3] <= 0:
                continue
            out.append(element)
        return out

    def element_action(self) -> str:
        elements = self._safe_elements()
        if not elements:
            return self.point_action()
        if self._menu_open() and self.rng.random() < 0.75:
            rows = [e for e in elements if e.get("kind") == "menu-item"]
            elements = rows or elements
        key, element = self._pick([(mm.action_key({"how": "el"}, e), e) for e in elements])
        x, y = self.app.center(element)
        how = self.rng.random()
        self.coverage.note("elements", element.get("name", ""))
        if how < 0.2:
            self.app.move(x, y)
            return f"hover:{key}"
        if how < 0.27:
            self.app.click(x, y, button=3)
            return f"right:{key}"
        if how < 0.32:
            self.app.click(x, y, clicks=2)
            return f"double:{key}"
        if how < 0.34:
            self.app.click(x, y, clicks=3)
            return f"triple:{key}"
        if how < 0.35:
            self.app.click(x, y, button=2)
            return f"middle:{key}"
        if how < 0.40:
            modifier = self.rng.choice(["shift", "ctrl", "alt"])
            self.app.click(x, y, modifiers=(modifier,))
            return f"{modifier}:{key}"
        self.app.click(x, y)
        return f"click:{key}"

    def _text_point(self) -> tuple[float, float] | None:
        """A point on a visible text row of a random editor pane."""
        ui = self.app.last_ui or self._ui()
        panes = ui.get("editors") or []
        if not panes:
            return None
        pane = self.rng.choice(panes)
        rows = pane.get("rows") or []
        if not rows:
            return None
        row = self.rng.choice(rows)
        x = pane["textX"] + self.rng.uniform(0, 60) * pane.get("charWidth", 8)
        rect = pane.get("rect") or [0, 0, 1e9, 1e9]
        x = min(x, rect[0] + rect[2] - 4)
        return x, row["y"] + pane.get("lineHeight", 18) / 2

    def text_action(self) -> str:
        start = self._text_point()
        if start is None:
            return self.chord_action()
        x, y = start
        how = self.rng.choice(["click", "shift", "alt", "ctrl", "double", "triple", "drag", "drag",
                               "alt-drag", "double-drag", "drop-text", "right"])
        if how == "click":
            self.app.click(x, y)
        elif how in ("shift", "alt", "ctrl"):
            self.app.click(x, y, modifiers=(how,))
        elif how == "double":
            self.app.click(x, y, clicks=2)
        elif how == "triple":
            self.app.click(x, y, clicks=3)
        elif how == "right":
            self.app.click(x, y, button=3)
        else:
            end = self._text_point() or (x + 80, y + 40)
            if how == "drop-text":
                # Select a word, then drag the selection elsewhere (text drag and drop).
                self.app.click(x, y, clicks=2)
                self.app.settle_quiet(0.2)
                self.app.drag(x, y, *end)
            else:
                self.app.drag(x, y, *end, modifiers=("alt",) if how == "alt-drag" else (),
                              clicks=2 if how == "double-drag" else 1)
        return f"text:{how}"

    def _step_off_a_forbidden_setting(self) -> None:
        """Before a key or typed text: with a forbidden setting's row selected in the
        Settings overlay, Enter or Space would toggle it -- seed 1 switched
        `control.enabled` off that way, after a click that selected the row from
        empty space beside it. Down first, so the keys land on the next row."""
        if any(element.get("selected") and any(setting in element.get("name", "") for setting in FORBIDDEN_SETTINGS)
               for element in self._ui().get("elements", [])):
            self.app.key("Down")

    def _chord_list(self) -> list[str]:
        if self._chords is None:
            self._chords = []
            for binding in self.app.keybindings():
                if binding.get("id") in FORBIDDEN_BINDINGS or binding.get("plugin"):
                    continue
                chord = xdotool_chord(binding.get("chord", ""))
                if chord:
                    self._chords.append(chord)
            self._chords += ["Escape", "Return", "Tab", "shift+Tab", "Up", "Down", "Left", "Right",
                             "Home", "End", "Prior", "Next", "BackSpace", "Delete", "shift+Up", "shift+Down",
                             "shift+Left", "shift+Right", "ctrl+Left", "ctrl+Right", "shift+End", "shift+Home",
                             "ctrl+shift+Left", "ctrl+shift+Right", "ctrl+Home", "ctrl+End", "F2", "F3",
                             "shift+F10", "Menu", "alt+Left", "alt+Right"]
        return self._chords

    def chord_action(self) -> str:
        self._step_off_a_forbidden_setting()
        key, chord = self._pick([(f"key:{c}", c) for c in self._chord_list()])
        self.app.key(chord)
        return key

    def _command_list(self) -> list[str]:
        if self._commands is None:
            try:
                catalogue = [c["command"] for c in self.app.query("commands")
                             if c.get("command") and not c.get("plugin")]
            except (q.QaFailure, TypeError):
                catalogue = []
            self._commands = [c for c in catalogue if c not in FORBIDDEN_COMMANDS
                              and not c.startswith(FORBIDDEN_COMMAND_PREFIXES)] or list(MIXED_VERBS)
        return self._commands

    def command_action(self) -> str:
        if self.profile == "editor":
            command = self.editor_generator(self.rng, self.files)
        elif self.profile == "commands":
            command = mixed_command(self.rng, self.files)
        else:
            _, verb = self._pick([(f"cmd:{v}", v) for v in self._command_list()])
            command = mixed_command(self.rng, self.files + self.weird, [verb])
        self.app.send_line(command)
        return f"cmd:{command.split(' ', 1)[0]}"

    def type_action(self) -> str:
        self._step_off_a_forbidden_setting()
        if self.rng.random() < 0.12:
            self.app.send_line("type " + q.quote_arg(self.rng.choice(PASTE_TEXTS)))
            return "type:paste"
        _, (name, texts) = self._pick([(f"type:{name}", (name, texts)) for name, texts in TEXT_CLASSES.items()])
        self.app.type(self.rng.choice(texts))
        return f"type:{name}"

    def burst_action(self) -> str:
        """Key repeat or several actions back to back, with no settle and no
        check between: input faster than frames."""
        self.app.in_burst = True
        try:
            if self.rng.random() < 0.6:
                chord = self.rng.choice(["Down", "Up", "BackSpace", "Delete", "Return", "Tab", "Next", "Prior",
                                         "ctrl+z", "ctrl+y", "ctrl+shift+z", "ctrl+d", "alt+Up", "alt+Down",
                                         "shift+Down", "ctrl+Right", "Right", "Left", "ctrl+slash",
                                         "ctrl+shift+bracketleft", "ctrl+shift+bracketright", "ctrl+equal",
                                         "ctrl+minus", "Escape", "ctrl+Tab"])
                for _ in range(self.rng.randint(5, 40)):
                    self.app.key(chord)
                return f"burst:{chord}"
            names = ("element", "text", "chord", "type", "wheel", "command")
            for _ in range(self.rng.randint(3, 8)):
                self.sources[self.rng.choice(names)]()
            return "burst:mixed"
        finally:
            self.app.in_burst = False

    def drag_action(self) -> str:
        handles = [e for e in self._safe_elements() if e["kind"] in ("divider", "tab", "panel-tab")]
        if not handles:
            return self.wheel_action()
        _, element = self._pick([(f"drag:{e['kind']}", e) for e in handles])
        x0, y0 = self.app.center(element)
        w, h = self.app.current_size()
        dx = self.rng.choice([self.rng.randint(-120, 120), self.rng.randint(-w, w)])
        dy = self.rng.choice([self.rng.randint(-80, 80), self.rng.randint(-h, h)]) if element["kind"] == "divider" \
            else self.rng.choice([0, 0, self.rng.randint(-200, 300)])
        self.app.drag(x0, y0, max(1, min(w - 2, x0 + dx)), max(1, min(h - 2, y0 + dy)))
        return f"drag:{element['kind']}"

    def wheel_action(self) -> str:
        w, h = self.app.current_size()
        modifiers = self.rng.choice([(), (), (), ("shift",), ("ctrl",)])
        self.app.wheel(self.rng.randrange(40, max(41, w - 40)), self.rng.randrange(60, max(61, h - 40)),
                       self.rng.choice([-10, -5, -2, -1, 1, 2, 5, 10]), modifiers)
        return f"wheel:{'+'.join(modifiers)}"

    def point_action(self) -> str:
        w, h = self.app.current_size()
        # Below the menu bar's window controls: a random click there could close
        # the window. And never on a forbidden element (a menu's Quit row, the
        # control channel's setting) -- a random point used to land on them.
        forbidden = self._forbidden_rects(self._ui())
        for _ in range(20):
            x, y = self.rng.randrange(0, max(1, w - 160)), self.rng.randrange(30, max(31, h))
            if not self._inside(forbidden, x, y):
                self.app.click(x, y, button=3 if self.rng.random() < 0.15 else 1)
                return "point"
        return "point"

    def _setting_list(self) -> list[dict[str, Any]]:
        if self._settings is None:
            try:
                rows = self.app.query("settings")
            except q.QaFailure:
                rows = []
            self._settings = [row for row in (rows if isinstance(rows, list) else [])
                              if not str(row.get("id", "")).startswith(FORBIDDEN_SETTING_PREFIXES)]
        return self._settings

    def _setting_value(self, row: dict[str, Any]) -> str:
        kind = row.get("type")
        if row.get("id") == "ui.scale":
            return self.rng.choice(self._scales_that_fit() or ["1"])
        if kind == "bool":
            return self.rng.choice(["true", "false"])
        if kind == "enum" and row.get("enumValues"):
            return self.rng.choice(row["enumValues"])
        if kind == "int":
            low, high = int(row.get("min", 0)), int(row.get("max", 100))
            default = row.get("default")
            return str(self.rng.choice([low, high, self.rng.randint(low, high),
                                        int(default) if str(default).lstrip("-").isdigit() else low]))
        if kind == "float":
            return self.rng.choice(["0", "0.5", "1", "1.5", "2", "-1", "100"])
        return self.rng.choice(STRING_VALUES + [str(row.get("default", ""))])

    def setting_action(self) -> str:
        rows = self._setting_list()
        if not rows:
            return self.chord_action()
        interesting = [row for row in rows if row.get("id") in INTERESTING_SETTINGS]
        pool = interesting if interesting and self.rng.random() < 0.6 else rows
        key, row = self._pick([(f"set:{row['id']}", row) for row in pool])
        value = self._setting_value(row)
        self.coverage.note("settings", f"{row['id']}={value}")
        self.app.send_line(f"set-setting {row['id']} {q.quote_arg(value) if value else chr(34) * 2}")
        return key

    def window_action(self) -> str:
        if self.app.wayland:
            return self.chord_action()
        if self.rng.random() < 0.3:
            self.app.focus_out()
            return "window:focus-out"
        screen_w, screen_h = self.app.size
        widths = [320, 640, 800, 1024, self.rng.randint(320, screen_w), screen_w]
        heights = [240, 400, 600, self.rng.randint(240, screen_h), screen_h]
        if self.min_logical is not None:
            scale = self._window_scale()
            widths = [w for w in widths if w >= self.min_logical[0] * scale] or [screen_w]
            heights = [h for h in heights if h >= self.min_logical[1] * scale] or [screen_h]
        w = self.rng.choice(widths)
        h = self.rng.choice(heights)
        self.app.resize(min(w, screen_w), min(h, screen_h))
        return "window:resize"

    def _window_scale(self) -> float:
        """Window pixels per layout unit, as the last `ui` answer has it."""
        scale = ((self.app.last_ui or {}).get("window") or {}).get("scale") or [1.0]
        try:
            return max(0.1, float(scale[0]))
        except (TypeError, ValueError, IndexError):
            return 1.0

    def _scales_that_fit(self, whole_screen: bool = False) -> list[str]:
        """The SCALE_VALUES at which the window as it is now (or, with
        `whole_screen`, the window resized to the screen) is at least min_logical."""
        if self.min_logical is None:
            return list(SCALE_VALUES)
        width, height = self.app.size
        window = (self.app.last_ui or {}).get("window") or {}
        if not whole_screen and window.get("width") and window.get("height"):
            scale = self._window_scale()
            width, height = window["width"] * scale, window["height"] * scale
        return [v for v in SCALE_VALUES
                if width / float(v) >= self.min_logical[0] and height / float(v) >= self.min_logical[1]]

    def _keep_window_usable(self) -> None:
        """Undoes a step that left the window under min_logical: back to a scale
        that fits, at the screen's size. Recorded like any action, so a replay
        does the same."""
        if self.min_logical is None or self.app.wayland or not self.app.alive():
            return
        window = (self.app.last_ui or {}).get("window") or {}
        width, height = window.get("width"), window.get("height")
        if not width or not height or (width >= self.min_logical[0] and height >= self.min_logical[1]):
            return
        scale = self.rng.choice(self._scales_that_fit(whole_screen=True) or ["1"])
        self.app.send_line(f"set-setting ui.scale {scale}")
        self.app.resize(*self.app.size)
        self.app.settle_quiet(most=0.3)
        with contextlib.suppress(q.QaFailure):
            self._ui()

    def terminal_action(self) -> str:
        ui = self.app.last_ui or {}
        if ui.get("focus") != "terminal" or self.rng.random() < 0.15:
            self.app.send_line("term")
            self.app.settle_quiet(0.3)
        if self.rng.random() < 0.15:
            self.app.key(self.rng.choice(["ctrl+c", "ctrl+d", "ctrl+l", "ctrl+z", "q", "Up", "Prior"]))
            return "terminal:key"
        _, line = self._pick([(f"terminal:{line[:12]}", line) for line in TERMINAL_LINES])
        self.app.type(line)
        return "terminal:line"

    def file_action(self) -> str:
        files = self.weird or self.files
        if not files:
            return self.command_action()
        target = self.rng.choice([name for name in files if name] or ["README.md"])
        if self.rng.random() < 0.25 and self.project is not None:
            w, h = self.app.current_size()
            self.app.drop(self.rng.randrange(300, max(301, w - 100)), self.rng.randrange(100, max(101, h - 100)),
                          str(self.project / target))
            return "file:drop"
        self.app.send_line(f"open {q.quote_arg(target)}")
        return "file:open"

    def external_action(self) -> str:
        if self.project is None:
            return self.chord_action()
        self.counter += 1
        if self.rng.random() < 0.15:
            # Another application's clipboard: the app's paste reads text it never
            # copied (CRLF, an enormous line, wide characters), and its own next
            # copy takes the selection back (App.foreign_clipboard).
            self.app.foreign_clipboard(self.rng.choice(PASTE_TEXTS + FOREIGN_CLIPBOARD_TEXTS))
            return "ext:foreign-clipboard"
        op = self.rng.choice(EXTERNAL_OPS)
        candidates = [name for name in self.files + self.weird
                      if name and not name.endswith(("loop", "dangling")) and "." in Path(name).name]
        target = self.rng.choice(candidates) if candidates else "README.md"
        self.app.record_external({"external": op, "target": target, "counter": self.counter})
        apply_external(self.world, op, target, self.counter)
        if op.startswith("git-"):
            self.app.outside_change_pending = True
        return f"ext:{op}"

    def diff_action(self) -> str:
        """Two versions of a generated source in a diff, then the structural
        oracle on what it shows. The editable right side takes the typing that
        follows, which rebuilds the refinement per keystroke."""
        if self.project is None:
            return self.command_action()
        self.counter += 1
        self.app.record_external({"external": "diff-variant", "target": "", "counter": self.counter})
        apply_external(self.project, "diff-variant", "", self.counter)
        language = dv.LANGUAGES[self.counter % len(dv.LANGUAGES)]
        folder = self.project / "qa-diff"
        left, right = (folder / f"v{self.counter % 6}.{side}.{language}" for side in ("left", "right"))
        self.app.send_line(f"compare-files {q.quote_arg(str(left))} {q.quote_arg(str(right))}")
        # The tab builds its model after the command answers: checked before, the
        # oracle reads the previous tab or a one-row placeholder and checks nothing.
        deadline = time.monotonic() + 3.0 * q.SLOW_FACTOR
        while time.monotonic() < deadline:
            with contextlib.suppress(Exception):
                shown = self.app.query("compare")
                if shown.get("rightPath") == str(right) and len(shown.get("rows") or []) > 1:
                    break
            time.sleep(0.1)
        self._check_structural(f"monkey step {self.step_index}")
        return "diff:open"

    def tooling_action(self) -> str:
        """A language-server or debugger action with real arguments on the
        fixture's code. The first one sets up what they need (a compile_flags.txt
        for clangd, the debugger on), as recorded actions a replay repeats."""
        if self.project is None:
            return self.command_action()
        if not self._tooling_ready:
            self._tooling_ready = True
            self.counter += 1
            self.app.record_external({"external": "compile-flags", "target": "", "counter": self.counter})
            apply_external(self.project, "compile-flags", "", self.counter)
            self.app.send_line("set-setting debug.enabled true")
            self.app.send_line("open src/scheduler.cpp")
            return "tooling:setup"
        path, lines = self.rng.choice(TOOLING_FILES)
        line = self.rng.randrange(1, lines + 1)
        roll = self.rng.random()
        if roll < 0.45:
            # At a position in the code, then the request: most land on a symbol.
            self.app.send_line(f"open {path}")
            self.app.send_line(f"goto {line}:{self.rng.randrange(1, 40)}")
            command = self.rng.choice(LSP_COMMANDS)
            if command == "goto-symbol":
                command += " " + self.rng.choice(("Scheduler", "run", "add", "zz"))
            self.app.send_line(command)
            return f"tooling:{command.split()[0]}"
        if roll < 0.55:
            self.app.send_line(f"open {path}")
            self.app.send_line(f"goto {line}:{self.rng.randrange(1, 30)}")
            self.app.send_line(f"rename-symbol {self.rng.choice(('task', 'total', 'renamed', 'queue_'))}")
            return "tooling:rename"
        if roll < 0.70:
            absolute = q.quote_arg(str(self.project / path))
            command = self.rng.choice((
                f"breakpoint-set {absolute} {line}",
                f"breakpoint-set {absolute} {line} total>{self.rng.randrange(10)}",
                f"breakpoint-condition {absolute} {line} task.cost=={self.rng.randrange(5)}",
                f"breakpoint-hit-condition {absolute} {line} {self.rng.randrange(1, 4)}",
                f"breakpoint-logmessage {absolute} {line} \"total is {{total}}\"",
                f"breakpoint-enable {absolute} {line}", f"breakpoint-disable {absolute} {line}",
                f"breakpoint-remove {absolute} {line}", f"breakpoint-clear {absolute}", "breakpoint-clear",
                f"breakpoint-function-add {self.rng.choice(TOOLING_FUNCTIONS)}",
                f"breakpoint-function-toggle {self.rng.choice(TOOLING_FUNCTIONS)}",
                f"breakpoint-function-condition {self.rng.choice(TOOLING_FUNCTIONS)} total>1",
                f"breakpoint-function-remove {self.rng.choice(TOOLING_FUNCTIONS)}",
                f"breakpoint-exception-condition {self.rng.choice(('throw', 'catch', 'all'))} 1",
                "breakpoint-toggle"))
            self.app.send_line(command)
            return f"tooling:{command.split()[0]}"
        if roll < 0.80:
            # The Tests sidebar, through tools/qa/plugins/qa-tests: discovery on a
            # file, a run of everything or of a few ids it reports (and one it never
            # did), then the sidebar is where the monkey's clicks land.
            choice = self.rng.random()
            if choice < 0.4:
                self.app.send_line(f"open {path}")
                command = "tests-discover"
            elif choice < 0.7:
                command = "tests-run"
            else:
                suite = f"qa-tests/{path}"
                ids = self.rng.sample((suite, f"{suite}::wide", f"{suite}::long", f"{suite}::nosuch",
                                       "qa-tests/elsewhere::x"), self.rng.randint(1, 3))
                command = "tests-run " + " ".join(ids)
            self.app.send_line(command)
            return f"tooling:{command.split()[0]}"
        if roll < 0.87:
            self.app.send_line(f"debug-run --type gdb {q.quote_arg(str(self.project / 'taskflow'))}")
            return "tooling:debug-run"
        command = self.rng.choice(DEBUG_COMMANDS)
        if command == "debug-switch-session":
            command += f" {self.rng.randrange(0, 3)}"
        self.app.send_line(command)
        return f"tooling:{command}"

    def remote_action(self) -> str:
        """The connection under a remote project: the link cut or the host's server
        killed (recorded, so a replay cuts it at the same point), or one of the
        remote commands -- conflicts listed and resolved, deletes applied, a
        disconnect and a reconnect, the server stopped."""
        if self.host is None:
            return self.command_action()
        if self.rng.random() < 0.3:
            kind = self.rng.choice(rh.FAULTS)
            self.counter += 1
            self.app.record_external({"external": "remote-fault", "target": kind, "counter": self.counter})
            self.host.fault(kind)
            return f"remote:fault-{kind}"
        named = [name for name in self.files if name]
        if self.rng.random() < 0.15 and self.project is not None:
            # Blocked saves (a dangling link or an unreadable file where the save
            # lands, both of which the outside world makes): every one at once.
            command = f"remote-blocked {q.quote_arg(str(self.project))} {self.rng.choice(('retry', 'replace'))}"
        elif self.rng.random() < 0.2 and named:
            command = (f"remote-resolve {q.quote_arg(self.rng.choice(named))} "
                       f"{self.rng.choice(('local', 'remote', 'compare', 'merge'))}")
        else:
            command = self.rng.choice(REMOTE_COMMANDS)
        self.app.send_line(command)
        return f"remote:{command.split()[0]}"

    def _check_converged(self, label: str) -> None:
        if self.converge is None:
            return
        self.app.record_external({"oracle": "converge"})
        self._report_with_evidence(label, self.converge.check(), self.converge.evidence)
        self.converge.evidence = {}

    def _check_structural(self, label: str) -> None:
        self.app.record_external({"oracle": "structural-diff"})
        self._report_with_evidence(label, self.structural.check(), self.structural.evidence)
        self.structural.evidence = {}

    def kill9_action(self) -> str:
        if self.relaunches >= 3:
            return self.chord_action()
        self.relaunches += 1
        self._report_kill(f"kill -9 at step {self.step_index}", self.data.kill9_and_check(self.rng))
        return "kill9"

    # ---- replaying the outside world -------------------------------------------

    def replay_external(self, row: dict[str, Any]) -> None:
        """What q.replay calls for an `external` or `kill9` row."""
        if "kill9" in row:
            # The same kill and the same comparison the run made: a replay that only
            # killed and relaunched could never reproduce a lost buffer, which is why
            # every kill -9 finding measured 0% (soak 48a294e57a19).
            self._report_kill("kill -9 (replayed)",
                              self.data.kill9_and_check(self.rng, waited=bool(row.get("waited"))))
        elif row.get("external") == "remote-fault":
            if self.host is not None:
                self.host.fault(row.get("target", ""))
        elif self.world is not None:
            apply_external(self.world, row["external"], row.get("target", ""), int(row.get("counter", 0)))

    # ---- findings ----------------------------------------------------------------

    def _report_kill(self, label: str, found: list[tuple[str, str]]) -> None:
        """A kill -9 finding, plus what the oracle compared: the documents query
        before the kill and after the relaunch, beside the finding's evidence."""
        before = len(self.app.findings)
        self._report(label, found)
        recorded = self.app.findings[before:]
        shot = next((f.screenshot for f in recorded if f.screenshot), None)
        if shot and getattr(self.data, "last_kill", None) is not None:
            (Path(shot).parent / "kill-documents.json").write_text(json.dumps(self.data.last_kill, indent=1))
        state = getattr(self.data, "last_kill_state", None)
        if shot and state is not None and state.is_dir():
            # The session records the relaunch read, as the killed process left them.
            shutil.make_archive(str(Path(shot).parent / "kill-state"), "gztar", root_dir=state)

    def _report(self, label: str, found: list[tuple[str, str]]) -> None:
        if found:
            before = len(self.app.findings)
            self.app.report(label, found)
            self._dedupe(before)

    def _dedupe(self, since: int) -> None:
        """Drops findings since index `since` whose signature this run has seen,
        counting them; the first of each keeps its evidence directory."""
        kept = self.app.findings[:since]
        ui = self.app.last_ui or {}
        for finding in self.app.findings[since:]:
            context = fnd.stale_context(finding.detail, ui.get("surfaces"), ui.get("elements"))
            signature = fnd.signature(finding.kind, finding.detail,
                                      self.app.log_tail() if finding.kind in ("crash", "log") else "",
                                      context)
            entry = self.signatures.get(signature)
            evidence = Path(finding.screenshot).parent if finding.screenshot else None
            if entry is not None:
                entry["count"] += 1
                if evidence is not None and evidence.name.startswith("finding-") and \
                        str(evidence) != entry.get("evidence"):
                    shutil.rmtree(evidence, ignore_errors=True)
                continue
            self.signatures[signature] = {
                "signature": signature, "kind": finding.kind, "detail": finding.detail[:2000],
                "label": fnd.describe(finding.kind, finding.detail, context), "count": 1,
                "step": self.step_index, "evidence": str(evidence or ""), "state": self.state,
                "actions": len(self.app.actions),
            }
            kept.append(finding)
        self.app.findings[:] = kept

    # ---- driving -------------------------------------------------------------------

    def step(self) -> tuple[str, str]:
        """One action; returns (source, action key)."""
        if self.profile in COMMAND_PROFILES:
            return "command", self.command_action()
        source = self._choose_source()
        return source, self.sources[source]()

    def _observe(self, source: str, key: str) -> None:
        """The state now, its coverage, the corpus, and the source's reward."""
        after = mm.state_signature(self.app.last_ui, self.app.last_buffer, self.app.last_editor)
        new_state, new_transition = self.coverage.record(self.state, key, after)
        self.new_states += new_state
        self.new_transitions += new_transition
        if new_state:
            self.corpus.save(after, self.app.replay_header, self.app.actions)
        reward = 1.0 if new_state else (0.5 if new_transition else 0.0)
        self.reward[source] = 0.9 * self.reward.get(source, 0.5) + 0.1 * reward
        self.state = after

    def _start_from_corpus(self, probability: float) -> None:
        if self.corpus.directory is None or self.rng.random() >= probability:
            return
        record = self.corpus.pick(self.rng, self.coverage, self.app.replay_header)
        if record is None:
            return
        self.app.record_external({"corpus": record.get("state", "")})
        actions = [a for a in record.get("actions", []) if "corpus" not in a]
        project = self.project
        q.replay(self.app, actions, pause=0.12,
                 churn=(lambda row: apply_churn(project, row["churn"], row["counter"])) if project else None,
                 external=self.replay_external)
        # The counter continues past the prefix's, so new external rows differ.
        self.counter = max([int(a.get("counter", 0)) for a in actions if "external" in a] + [self.counter])

    def _report_data(self, label: str, found: list[tuple[str, str]]) -> None:
        """A data finding, with the oracle's byte evidence beside it."""
        self._report_with_evidence(label, found, self.data.evidence)
        self.data.evidence = {}

    def _report_with_evidence(self, label: str, found: list[tuple[str, str]],
                              evidence: dict[str, bytes]) -> None:
        """A finding, with the files its oracle kept written beside it."""
        before = len(self.app.findings)
        self.app.report(label, found)
        if len(self.app.findings) > before and evidence:
            shot = self.app.findings[-1].screenshot
            if shot:
                folder = Path(shot).parent
                for name, data in evidence.items():
                    with contextlib.suppress(OSError):
                        (folder / name).write_bytes(data)
        if found:
            self._dedupe(before)

    def replay_oracle(self, row: dict[str, Any]) -> None:
        """What q.replay calls for an {"oracle": ...} row: the oracle the run ran
        there, run again. A replay that only re-sent the undo oracle's undos and
        redos never compared the text, so every undo finding measured 0%; a data
        finding compared once, at the end, long after the race it caught."""
        label = "replayed oracle"
        name = row.get("oracle")
        if name == "data-clean":
            self._report_data(label, self.data.check_clean())
        elif name == "structural-diff":
            self._report_with_evidence(label, self.structural.check(), self.structural.evidence)
            self.structural.evidence = {}
        elif name == "converge" and self.converge is not None:
            self._report_with_evidence(label, self.converge.check(), self.converge.evidence)
            self.converge.evidence = {}
        elif name == "undo":
            self.app.in_oracle = True
            try:
                found = self.undo.check(int(row.get("asked", 1)))
            finally:
                self.app.in_oracle = False
            self._report_with_evidence(label, found, self.undo.evidence)

    def _periodic_oracles(self, index: int) -> None:
        label = f"monkey step {index}"
        self._report(label, self.liveness.check())
        if index % 20 == 19:
            self.app.record_external({"oracle": "data-clean"})
            self._report_data(label, self.data.check_clean())
        if index % 40 == 39:
            asked = self.undo.pick()
            self.app.record_external({"oracle": "undo", "asked": asked})
            self.app.in_oracle = True
            try:
                found = self.undo.check(asked)
            finally:
                self.app.in_oracle = False
            self._report_with_evidence(label, found, self.undo.evidence)
        if index % 5 == 4 and "t=compare" in self.state:
            self._check_structural(label)
        if index % 60 == 59:
            self._check_converged(label)
        if index % 50 == 0:
            self._report(label, self.resources.check())
        if index % 150 == 149:
            before = len(self.app.findings)
            self._report(label, self.idle.check())
            shot = next((f.screenshot for f in self.app.findings[before:] if f.screenshot), None)
            if shot and self.idle.evidence:
                (Path(shot).parent / "idle-wakes.txt").write_text(self.idle.evidence)

    def _final_oracles(self) -> None:
        if not self.app.alive():
            return
        label = "end of run"
        self._report_data(label, self.data.check_clean())
        if self.converge is not None:
            self._report_with_evidence(label, self.converge.check(), self.converge.evidence)
            self.converge.evidence = {}
        self._report(label, self.resources.check())
        pid = self.app.proc.pid if self.app.proc else None
        left = self.data.staging_leftovers(pid)
        if left:
            self._report(label, [("data", f"the app left {len(left)} save temporaries of its own "
                                          f"(pid {pid}) behind while idle: {left[0].name}")])

    def run(self, steps: int, settle: float = 0.12, corpus_probability: float = 0.5,
            time_budget: float | None = None) -> bool:
        """Runs `steps` actions (or until `time_budget` seconds, or max_findings
        distinct findings). Returns True when clean."""
        started = time.monotonic()
        self.app.press_guard = self._press_lands_on_forbidden
        self.app.activation_guard = self._activation_chooses_forbidden
        self.resources.sample()
        self._start_from_corpus(corpus_probability)
        self._keep_window_usable()
        for index in range(steps):
            self.step_index = index
            before = len(self.app.findings)
            source, key = "chord", "other"
            try:
                source, key = self.step()
            except q.QaFailure as failure:
                self.app.expect(False, f"step {index}: {failure}")
            self.app.settle_quiet(most=settle)
            self.app.check(f"monkey step {index}")
            self._dedupe(before)
            if self.app.alive():
                self._observe(source, key)
                self._periodic_oracles(index)
                self._keep_window_usable()
            if not self.app.alive():
                if not self.keep_going or self.relaunches >= 3:
                    break
                self.relaunches += 1
                try:
                    self.app.crash_and_relaunch()
                    self.state = "relaunched"
                except Exception as error:  # noqa: BLE001 - a relaunch that fails ends the run
                    self.app.expect(False, f"relaunch after the crash failed: {error}")
                    break
            elif self.app.findings and not self.keep_going:
                break
            if len(self.signatures) >= self.max_findings:
                break
            if time_budget is not None and time.monotonic() - started > time_budget:
                break
        self._final_oracles()
        return not self.app.findings

    def summary(self) -> dict[str, Any]:
        """What the run did and found: the soak runner reads it (findings.json)."""
        commands: dict[str, int] = {}
        try:
            commands = dict(self.app.action_counts().get("commands", {}))
        except Exception:  # noqa: BLE001 - a dead app has no counts to give
            pass
        for name, count in commands.items():
            self.coverage.note("commands", name, count)
        return {"seed": self.seed, "profile": self.profile, "steps": self.step_index + 1,
                "states": len(self.coverage.states), "new_states": self.new_states,
                "new_transitions": self.new_transitions, "relaunches": self.relaunches,
                "commands": commands, "header": self.app.replay_header,
                "findings": list(self.signatures.values()),
                "resources": self.resources.samples[-1] if self.resources.samples else {},
                **({"converge": dict(self.converge.outcomes)} if self.converge is not None else {})}
