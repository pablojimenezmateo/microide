#!/usr/bin/env python3
"""Drive the real binary with randomized control-channel command sequences.

Why this exists: the unit suites drive coordinators and viewports directly, so a
defect that needs the whole shell -- a real render loop, real background threads,
a viewport that gets COPIED because a tab vector grew -- is invisible to them. A
few hundred random commands through the shipped binary is a cheap way to reach
those states. The first run of this found a heap-use-after-free in the editor's
visible-line layout cache (TD-2026-09-07-291): a copied cache kept an intrusive
recency list pointing into the SOURCE's map nodes, and the next repaint wrote
through it once the source died.

Run it against the ASAN build to get an answer instead of a coin flip:
unsanitized, that bug surfaced as `malloc(): unsorted double linked list
corrupted` in two runs out of ten and as nothing at all in the rest.

    cmake --preset microide-asan && cmake --build build/microide-asan --target microide -j8
    tools/fuzz-control-commands.py --binary build/microide-asan/microide/microide

Options:
    --binary PATH     the microide executable to drive (default: the release build)
    --project PATH    project to open; COPIED to a scratch dir first, because the
                      command stream saves, sorts, comments and deletes lines in
                      whatever it opens (default: this repository)
    --seeds N[-M]     seeds to run (default 1-8); one process per seed
    --rounds N        commands per seed (default 400)
    --profile P       `mixed` (every command verb, hostile arguments) or `editor`
                      (splits, tabs, reopen, typing -- the viewport COPY paths)
    --no-churn        do not rewrite project files under the editor while driving;
                      the churn is what reaches the reload (viewport copy) paths
    --keep            keep the scratch project and logs of a failing seed

Exit code is the number of seeds that died, hung, or reported a sanitizer error.
Each failure prints the log tail; a sanitizer report names the site directly.

Two environment requirements, both documented in
`dev-docs/control/control-channel.md`: it needs `xvfb` (with SDL_VIDEODRIVER=dummy
and no X display the app exits right after the sandbox line), and XDG_RUNTIME_DIR
must be a SHORT path because AF_UNIX caps the socket path at 108 bytes.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

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
EDITOR_TEXT = ["alpha", "b", "  ", "é", "你好", "\U0001F600", "x" * 200, "\t", "}", "//"]


def project_files(root: Path) -> list[str]:
    """A handful of real files plus paths that do not resolve."""
    tracked = [
        "README.md", "CHANGELOG.md", "AGENTS.md", "CLAUDE.md",
        "src/util/Parse.h", "src/editor/TextLayoutCache.cpp",
        "src/terminal/TerminalSession.cpp", "tests/TextViewportTests.cpp",
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


def make_editor_generator():
    """Editor/pane commands, with the split-and-close BURST emitted as a unit.

    Purely independent draws do not reach the interesting states often enough.
    Splitting a pane copies the viewport (and its caches) into the new group and
    closing a group destroys viewports, so "split, populate both panes, close one,
    keep drawing the other" is the sequence that exercises copied-cache lifetime —
    and it was the sequence that crashed. At one command per draw a run rarely
    lands the five in order; emitted as a burst it happens several times a run.
    Verified: with the layout-cache fix reverted, this profile reproduces the
    use-after-free; drawing the same commands independently did not.
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
                f"goto {rng.randrange(1, 2000)}",   # …and lay its visible rows out
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
    """A throwaway copy of the project to drive.

    The command stream saves, sorts, comments and deletes lines in whatever it
    opens, so it must never be pointed at a tree anyone cares about. For a git
    worktree this is a local clone, which is both fast and exactly the right
    filter -- it carries the tracked files and a real `.git` (the git verbs need
    one) and leaves the ignored build output behind. `--no-hardlinks` so nothing
    the run does can reach the source repository's objects.
    """
    if (source / ".git").exists():
        result = subprocess.run(
            ["git", "clone", "--local", "--no-hardlinks", "--quiet",
             str(source), str(destination)],
            capture_output=True, text=True)
        if result.returncode == 0:
            return
        print(f"git clone of {source} failed, falling back to a full copy: "
              f"{result.stderr.strip()}", file=sys.stderr)
    shutil.copytree(source, destination, symlinks=True, ignore_dangling_symlinks=True)


class ExternalChurn:
    """Rewrites files under the project while the command stream runs.

    Not decoration: this is what the first real find needed. The layout-cache
    use-after-free lives on the viewport COPY path, and the ordinary way a
    viewport gets copied is a reload -- the file watcher noticing that a file
    changed under an open buffer. Driving a project nothing else writes to never
    reaches it, and the bug only reproduced against a working copy that a build
    happened to be churning. A checkout being rewritten under the editor (a
    branch switch, a formatter, a build tree) is the normal case, so the sweep
    generates it rather than depending on the operator's background noise.
    """

    def __init__(self, project: Path, rng: random.Random):
        self._project = project
        self._rng = rng
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._targets = [path for path in
                         (project / "README.md", project / "CHANGELOG.md",
                          project / "AGENTS.md", project / "CLAUDE.md")
                         if path.exists()]

    def __enter__(self) -> "ExternalChurn":
        if self._targets:
            self._thread.start()
        return self

    def __exit__(self, *unused) -> None:
        self._stop.set()
        if self._thread.is_alive():
            self._thread.join(timeout=10)

    def _run(self) -> None:
        churn_dir = self._project / "churn"
        churn_dir.mkdir(exist_ok=True)
        counter = 0
        while not self._stop.wait(0.25):
            counter += 1
            target = self._rng.choice(self._targets)
            try:
                text = target.read_text(errors="replace")
                # A whole-file rewrite, which is what a checkout or a formatter
                # does; an open buffer over it reloads.
                target.write_text(f"churn {counter}\n" + text)
                # New and removed paths as well, so the index watcher is doing
                # more than noticing one mtime.
                (churn_dir / f"file{counter % 32}.txt").write_text(f"{counter}\n")
                stale = churn_dir / f"file{(counter + 16) % 32}.txt"
                if stale.exists():
                    stale.unlink()
            except OSError:
                pass


def wait_for_socket(log_path: Path, timeout_s: int) -> str | None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if log_path.exists():
            for line in log_path.read_text(errors="replace").splitlines():
                if '"event":"ready"' in line:
                    try:
                        return json.loads(line)["socket"]
                    except (ValueError, KeyError):
                        return None
        time.sleep(0.5)
    return None


def drive(sock_path: str, rng: random.Random, rounds: int, make_command, files,
          delay_s: float) -> str:
    """Send `rounds` commands, then prove the shell still answers a query."""
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(30)
    client.connect(sock_path)
    for index in range(rounds):
        line = json.dumps({"id": index + 1, "command": make_command(rng, files)})
        try:
            client.sendall(line.encode() + b"\n")
        except OSError:
            return "the shell closed the connection mid-stream"
        # Drain so the reply pipe cannot fill and block the shell; a periodic
        # blocking read keeps the stream roughly in step with the app.
        try:
            client.settimeout(6 if index % 20 == 0 else 0.05)
            if not client.recv(65536):
                return "the shell closed the connection mid-stream"
        except socket.timeout:
            pass
        except OSError:
            return "the shell closed the connection mid-stream"
        # Let a FRAME happen between commands. This is not politeness: the states
        # worth reaching are the ones a repaint reads, and a back-to-back stream
        # lands several commands in one event-loop turn and repaints once at the
        # end. The layout-cache use-after-free needs the surviving pane to DRAW
        # after the split and again after the close; at full speed the same
        # command sequence produced nothing.
        if delay_s > 0:
            time.sleep(delay_s)

    client.settimeout(45)
    client.sendall(json.dumps({"id": 999999, "query": "status"}).encode() + b"\n")
    buffered = b""
    try:
        while b'"id":999999' not in buffered.replace(b" ", b""):
            chunk = client.recv(65536)
            if not chunk:
                return "the shell closed the connection before answering"
            buffered += chunk
    except socket.timeout:
        return "the shell stopped answering (hung or died)"
    return ""


def run_seed(args, seed: int, scratch: Path) -> bool:
    """True when the seed survived. Prints the diagnosis when it did not."""
    home = scratch / f"xdg{seed}"
    for part in ("config", "state", "data", "cache"):
        (home / part).mkdir(parents=True, exist_ok=True)
    project = scratch / f"project{seed}"
    if project.exists():
        shutil.rmtree(project)
    copy_project(args.project, project)

    log_path = scratch / f"seed{seed}.log"
    environment = dict(os.environ)
    environment.update({
        "SDL_AUDIODRIVER": "dummy",
        "XDG_CONFIG_HOME": str(home / "config"),
        "XDG_STATE_HOME": str(home / "state"),
        "XDG_DATA_HOME": str(home / "data"),
        "XDG_CACHE_HOME": str(home / "cache"),
        # halt_on_error so the first report is the one attributable to the last
        # commands, rather than the cascade after a corrupted heap.
        "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
    })
    with log_path.open("wb") as log:
        # Its own session, so the teardown below can signal the whole group.
        # Killing `xvfb-run` alone leaves the app it exec'd running, and a leaked
        # instance makes every later `control-send` ambiguous ("multiple
        # instances are running") until someone finds and kills it by hand.
        app = subprocess.Popen(
            ["xvfb-run", "-a", str(args.binary), str(project), "--control"],
            stdout=log, stderr=subprocess.STDOUT, env=environment, start_new_session=True)

    try:
        sock_path = wait_for_socket(log_path, timeout_s=args.startup_timeout)
        if sock_path is None:
            print(f"seed {seed}: FAILED to start (no ready handshake)")
            print(log_path.read_text(errors="replace")[-2000:])
            return False
        make_command = make_editor_generator() if args.profile == "editor" else mixed_command
        if args.no_churn:
            failure = drive(sock_path, random.Random(seed), args.rounds, make_command,
                            project_files(project), args.delay)
        else:
            with ExternalChurn(project, random.Random(seed ^ 0x5f5f)):
                failure = drive(sock_path, random.Random(seed), args.rounds, make_command,
                                project_files(project), args.delay)
    finally:
        try:
            os.killpg(os.getpgid(app.pid), 9)
        except (ProcessLookupError, PermissionError):
            app.kill()
        app.wait(timeout=30)

    text = log_path.read_text(errors="replace")
    sanitizer = "ERROR: AddressSanitizer" in text or "ERROR: ThreadSanitizer" in text
    corruption = any(marker in text for marker in
                     ("malloc():", "free():", "double free", "corrupted", "Assertion"))
    if not failure and not sanitizer and not corruption:
        return True

    print(f"seed {seed}: {failure or 'sanitizer/allocator report'}  (log: {log_path})")
    marker = max(text.find("ERROR: AddressSanitizer"), text.find("ERROR: ThreadSanitizer"))
    print(text[marker:marker + 4000] if marker >= 0 else text[-3000:])
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path,
                        default=REPO_ROOT / "build" / "microide" / "microide")
    parser.add_argument("--project", type=Path, default=REPO_ROOT)
    parser.add_argument("--seeds", default="1-8")
    parser.add_argument("--rounds", type=int, default=400)
    parser.add_argument("--profile", choices=("mixed", "editor"), default="mixed")
    parser.add_argument("--delay", type=float, default=0.05,
                        help="seconds between commands, so a frame is painted between "
                             "them (see drive(); at full speed the interesting states "
                             "are never drawn)")
    parser.add_argument("--startup-timeout", type=int, default=120)
    parser.add_argument("--no-churn", action="store_true",
                        help="do not rewrite project files while driving (see ExternalChurn: "
                             "the churn is what reaches the reload/copy paths)")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"no such binary: {args.binary}", file=sys.stderr)
        return 2
    if not os.environ.get("XDG_RUNTIME_DIR"):
        print("XDG_RUNTIME_DIR must be set to a SHORT path (AF_UNIX caps the socket "
              "path at 108 bytes); use the real /run/user/<uid>.", file=sys.stderr)
        return 2

    start, _, end = args.seeds.partition("-")
    seeds = range(int(start), int(end or start) + 1)

    scratch = Path(tempfile.mkdtemp(prefix="microide-fuzz-"))
    failures = 0
    try:
        for seed in seeds:
            if run_seed(args, seed, scratch):
                print(f"seed {seed}: survived {args.rounds} commands")
            else:
                failures += 1
    finally:
        if args.keep or failures:
            print(f"scratch kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)
    print(f"{len(list(seeds)) - failures}/{len(list(seeds))} seeds survived")
    return failures


if __name__ == "__main__":
    sys.exit(main())
