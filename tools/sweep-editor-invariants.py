#!/usr/bin/env python3
"""Drive the real binary and check ALGEBRAIC invariants of the editing commands.

Why this exists, and why it is not a second unit suite: the unit tests assert
what one command does to one fixture, so they only catch what someone thought to
write down. This asserts properties that must hold for EVERY command and every
caret position, with no model of the expected output at all -- so it needs no
oracle to be right, and it cannot go stale when a command's behaviour is
deliberately changed:

    undo        run a command, then undo it: the file is byte-identical again
    redo        replaying those undos lands back on exactly what it produced
    inverse     a command that DID change the file is undone exactly by its
                inverse: move-line-down/up, indent/outdent, cut/paste, and the
                comment toggles applied twice (conditioned on the first one
                applying, because at a buffer edge it correctly does nothing)
    idempotent  sorting an already-sorted buffer changes nothing
    non-vacuous every command must have moved a byte in at least one probe, or
                the properties above passed without testing anything

A green unit suite is compatible with `move-line-down` at the last line eating a
line, with an undo entry that restores the text but not the trailing newline, or
with a comment toggle that cannot uncomment what it just wrote. Those are the
shapes this finds, and they need the whole shell -- real settings, real save
path, real undo coalescing -- so they are invisible from a viewport fixture.

Everything runs through the control channel against the shipped binary, and every
comparison is made against the bytes ON DISK after a `save`, never against the
app's own report of its state.

    tools/sweep-editor-invariants.py --binary build/microide-asan/microide/microide

Exit code is the number of failing cases; each failure prints the case, the
command, and a unified diff of what changed.

Environment: needs `xvfb` and a SHORT `XDG_RUNTIME_DIR` (AF_UNIX caps the socket
path at 108 bytes) -- same requirements as tools/fuzz-control-commands.py.
"""

from __future__ import annotations

import argparse
import contextlib
import difflib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Fixtures. Each case gets its OWN copy of one of these, so the tab under test
# has exactly one edit in its undo history and an `undo` cannot reach across
# cases.
FIXTURES: dict[str, str] = {
    # Plain text: no comment syntax, no indentation rules.
    "plain.txt": "delta\nalpha\ncharlie\nbravo\necho\n",
    # A C++ buffer: line comments (`//`), block comments, braces, indentation.
    "code.cpp": (
        "#include <vector>\n"
        "\n"
        "int Sum(const std::vector<int>& values) {\n"
        "  int total = 0;\n"
        "  for (int value : values) {\n"
        "    total += value;\n"
        "  }\n"
        "  return total;\n"
        "}\n"
    ),
    # Python: a different line-comment token and significant indentation.
    "code.py": (
        "import sys\n"
        "\n"
        "def main(argv):\n"
        "    total = 0\n"
        "    for arg in argv:\n"
        "        total += len(arg)\n"
        "    return total\n"
    ),
    # Text the layout and the byte paths disagree about: wide glyphs, a
    # combining mark, a tab, and trailing whitespace.
    "wide.txt": "你好 world\ncafé latte\n\tindented\ntrailing   \nplain\n",
    # One line and NO final newline: the phantom-line edge every line op has to
    # get right.
    "single.txt": "only line",
    # Two lines, the second empty: the "last line is empty" boundary.
    "twoline.txt": "first\n",
}

# Caret positions probed per fixture, as 1-based (line, column). `None` means
# "select the whole buffer first" -- the other half of every line op's contract.
CARETS: list[tuple[int, int] | None] = [(1, 1), (2, 1), (3, 3), (99, 1), None]


class Failure(Exception):
    pass


class Effect:
    """How many probed cases actually changed the file, per command.

    Load-bearing, not a statistic: every property here ("undo restores it",
    "the inverse restores it") is trivially TRUE when the command did nothing at
    all. A broken `open`, a `save` that never fires, or a verb renamed out from
    under this file would make the whole sweep pass while probing nothing. The
    run fails when a command never once moved a byte.
    """

    def __init__(self) -> None:
        self.applied: dict[str, int] = {}
        self.probed: dict[str, int] = {}
        # Which probes did nothing, so a low count can be explained rather than
        # assumed to be the boundary cases.
        self.inert: list[str] = []

    def record(self, command: str, before: str, after: str, where: str) -> None:
        self.probed[command] = self.probed.get(command, 0) + 1
        if before != after:
            self.applied[command] = self.applied.get(command, 0) + 1
        else:
            self.inert.append(where)

    def inert_commands(self) -> list[str]:
        return sorted(name for name in self.probed if not self.applied.get(name))


class Driver:
    """One connection to a running instance, request/response in lock step."""

    def __init__(self, sock_path: str, timeout: float = 30.0):
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        self._sock.connect(sock_path)
        self._buffer = b""
        self._next_id = 1

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass

    def _readline(self) -> dict:
        while b"\n" not in self._buffer:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise Failure("the shell closed the connection")
            self._buffer += chunk
        line, _, self._buffer = self._buffer.partition(b"\n")
        try:
            return json.loads(line.decode(errors="replace"))
        except ValueError:
            return {}

    def request(self, payload: dict) -> dict:
        request_id = self._next_id
        self._next_id += 1
        payload = dict(payload, id=request_id)
        self._sock.sendall(json.dumps(payload).encode() + b"\n")
        # Unsolicited events share the stream; the reply is the object carrying
        # our id.
        while True:
            message = self._readline()
            if message.get("id") == request_id:
                return message

    def command(self, text: str) -> dict:
        return self.request({"command": text})


def run_case(driver: Driver, path: Path, original: str, steps: list[str]) -> list[str]:
    """Apply `steps` to a freshly opened `path`, returning the file after each.

    Each entry is the file's bytes on disk after that step's `save`, so nothing
    here trusts the app's own account of its buffer.
    """
    path.write_text(original)
    reply = driver.command(f"open {path}")
    # Never assume the open landed. A refused open used to answer ok=true and
    # leave the previously-active tab in place, so every command after it edited
    # and saved SOMEONE ELSE'S file while this case's file sat untouched -- and
    # every property here passed, because an untouched file undoes to itself.
    # That is what a whole fixture going quietly inert looks like from in here.
    if not reply.get("ok"):
        raise Failure(f"`open {path.name}` was refused: "
                      f"{reply.get('error') or reply.get('feedback')!r}. Every "
                      f"property after this would be measured against the wrong "
                      f"buffer, so the run stops here.")
    contents = []
    for step in steps:
        driver.command(step)
        driver.command("save")
        contents.append(path.read_text())
    return contents


def diff(expected: str, actual: str) -> str:
    return "".join(difflib.unified_diff(expected.splitlines(keepends=True),
                                        actual.splitlines(keepends=True),
                                        "expected", "actual"))


def caret_steps(caret: tuple[int, int] | None) -> list[str]:
    if caret is None:
        return ["select-all"]
    return [f"goto {caret[0]}:{caret[1]}"]


# (label, command, inverse-command-or-None, inverse-needs-a-selection).
#
# A None inverse means the command is only checked for the undo/redo property.
# The last field marks a pair that is an inverse ONLY over a selection:
#
#   `cut` with nothing selected takes the whole LINE and remembers that it did, so
#   the matching `paste` re-inserts it as a line ABOVE the caret (VS Code's
#   line-clipboard rule). Cut the empty line after a file's final newline and the
#   caret has nowhere to go back to, so the paste lands the line above its old
#   home -- correct behaviour, not a round trip. Over a selection, cut and paste
#   move exactly the same bytes and the pair is a true inverse.
#
# `outdent-lines` is deliberately NOT paired with `indent-lines`. Outdent is not
# injective: it cannot give back an indent a line never had, so on any block with
# a line already at column 0 (every real code fixture) outdent-then-indent shifts
# that line right by one unit. The pair that IS an inverse -- indent then outdent,
# because indent adds exactly one unit to every line it touches -- is listed.
EDIT_COMMANDS: list[tuple[str, str, str | None, bool]] = [
    ("move-line-down/up", "move-line-down", "move-line-up", False),
    ("move-line-up/down", "move-line-up", "move-line-down", False),
    ("indent/outdent", "indent-lines", "outdent-lines", False),
    ("line-comment twice", "toggle-line-comment", "toggle-line-comment", False),
    ("block-comment twice", "toggle-block-comment", "toggle-block-comment", False),
    ("delete-line", "delete-line", None, False),
    ("copy-line-down", "copy-line-down", None, False),
    ("copy-line-up", "copy-line-up", None, False),
    ("insert-line-below", "insert-line-below", None, False),
    ("insert-line-above", "insert-line-above", None, False),
    ("type", "type xyz", None, False),
    ("sort-ascending", "sort-lines-ascending", None, False),
    ("sort-descending", "sort-lines-descending", None, False),
    ("cut/paste", "cut", "paste", True),
    ("outdent", "outdent-lines", None, False),
]


def check_undo(driver: Driver, path: Path, original: str, prefix: list[str],
               command: str, label: str, failures: list[str], effect: Effect,
               max_undo: int = 8) -> None:
    """A command followed by enough undos restores the file byte for byte."""
    contents = run_case(driver, path, original, prefix + [command])
    after = contents[-1]
    effect.record(command, original, after, f"{command} | {path.name} | {prefix}")
    undos = 0
    while undos < max_undo:
        driver.command("undo")
        driver.command("save")
        undos += 1
        if path.read_text() == original:
            break
    else:
        failures.append(
            f"UNDO {label}: `{command}` on {path.name} after {prefix} did not restore "
            f"the file within {max_undo} undos\n"
            f"  after the command:\n{diff(original, after)}"
            f"  after the undos:\n{diff(original, path.read_text())}")
        return
    # Redo is the other half of the same contract and nothing else here covers it:
    # replaying the undos must land back on exactly what the command produced.
    for _ in range(undos):
        driver.command("redo")
    driver.command("save")
    redone = path.read_text()
    if redone != after:
        failures.append(
            f"REDO {label}: `{command}` on {path.name} after {prefix} did not come "
            f"back after {undos} undo(s) and {undos} redo(s)\n{diff(after, redone)}")


def check_inverse(driver: Driver, path: Path, original: str, prefix: list[str],
                  command: str, inverse: str, label: str, failures: list[str]) -> None:
    """If the command changed the file, its inverse must change it back exactly.

    Conditioned on the command having applied, because at a buffer edge it
    legitimately does nothing -- `move-line-up` on line 1, `move-line-down` with
    the last line selected -- and then the "inverse" is just a first move in the
    other direction, which of course does not restore anything. Asserting the
    pair unconditionally would report every edge as a failure and drown the one
    case that matters.
    """
    after_first = run_case(driver, path, original, prefix + [command])[-1]
    if after_first == original:
        return
    # The caret is deliberately NOT re-placed between the two: the first command
    # moved it, and the inverse acting where the caret now is IS the pairing an
    # editor promises (press Alt+Down then Alt+Up).
    driver.command(inverse)
    driver.command("save")
    restored = path.read_text()
    if restored == original:
        return
    failures.append(
        f"INVERSE {label}: `{command}` then `{inverse}` on {path.name} after "
        f"{prefix} did not restore the file\n"
        f"  after `{command}`:\n{diff(original, after_first)}"
        f"  after `{inverse}`:\n{diff(original, restored)}")


def check_idempotent(driver: Driver, path: Path, original: str, prefix: list[str],
                     command: str, label: str, failures: list[str]) -> None:
    """Running the command twice equals running it once."""
    once = run_case(driver, path, original, prefix + [command])[-1]
    driver.command(command)
    driver.command("save")
    twice = path.read_text()
    if once == twice:
        return
    failures.append(
        f"IDEMPOTENT {label}: `{command}` twice on {path.name} after {prefix} "
        f"differs from once\n{diff(once, twice)}")


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


def start_app(binary: Path, project: Path, scratch: Path, log_path: Path,
              startup_timeout: int):
    home = scratch / "xdg"
    for part in ("config", "state", "data", "cache"):
        (home / part).mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment.update({
        "SDL_AUDIODRIVER": "dummy",
        "XDG_CONFIG_HOME": str(home / "config"),
        "XDG_STATE_HOME": str(home / "state"),
        "XDG_DATA_HOME": str(home / "data"),
        "XDG_CACHE_HOME": str(home / "cache"),
        "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
    })
    # The save path is part of the contract under test, but trimming and the
    # final-newline rule rewrite the buffer on the way out and would show up as
    # a phantom failure of every other property. Turn both off so a save writes
    # the buffer verbatim; they get their own dedicated cases below.
    # Indent detection reads the file being opened, so it would make the
    # indent/outdent width depend on the fixture -- pin it instead.
    settings = [
        "--set", "editor.save.trim_trailing_whitespace", "false",
        "--set", "editor.save.ensure_final_newline", "false",
        "--set", "editor.indent.detect_on_open", "false",
        "--set", "editor.editorconfig.enabled", "false",
        "--set", "editor.autosave", "false",
        "--set", "editor.format_on_save", "false",
        "--set", "editor.soft_tabs", "true",
        "--set", "editor.indent_width", "2",
        "--set", "editor.tab_size", "2",
    ]
    with log_path.open("wb") as log:
        app = subprocess.Popen(
            ["xvfb-run", "-a", str(binary), str(project), "--control", *settings],
            stdout=log, stderr=subprocess.STDOUT, env=environment, start_new_session=True)
    sock_path = wait_for_socket(log_path, startup_timeout)
    if sock_path is None:
        try:
            os.killpg(os.getpgid(app.pid), 9)
        except (ProcessLookupError, PermissionError):
            app.kill()
        raise Failure(f"the app never announced a socket:\n"
                      f"{log_path.read_text(errors='replace')[-2000:]}")
    return app, sock_path


def sweep(open_session, project: Path, only: str | None,
          verbose: bool = False) -> list[str]:
    failures: list[str] = []
    effect = Effect()
    case_index = 0
    for fixture_name, original in FIXTURES.items():
        # A fresh instance per fixture. One case is one `open`, and a full run is
        # well past `kMaxOpenTabsPerGroup` (512) -- past which the group has no
        # room and every later case would be measured against a stale tab.
        with open_session() as driver:
          for caret in CARETS:
              prefix = caret_steps(caret)
              for label, command, inverse, needs_selection in EDIT_COMMANDS:
                  if only and only not in command:
                      continue
                  case_index += 1
                  stem = Path(fixture_name).stem
                  suffix = Path(fixture_name).suffix
                  path = project / f"case{case_index:04d}_{stem}{suffix}"
                  check_undo(driver, path, original, prefix, command, label, failures,
                             effect)
                  if inverse is not None and not (needs_selection and caret is not None):
                      path2 = project / f"case{case_index:04d}b_{stem}{suffix}"
                      check_inverse(driver, path2, original, prefix, command, inverse,
                                    label, failures)
                  if command.startswith("sort-lines"):
                      path3 = project / f"case{case_index:04d}c_{stem}{suffix}"
                      check_idempotent(driver, path3, original, prefix, command, label,
                                       failures)
    for command in effect.inert_commands():
        failures.append(
            f"VACUOUS: `{command}` never changed the file in "
            f"{effect.probed[command]} probes, so every invariant checked on it "
            f"passed without testing anything (open/save/verb wiring?)")
    if verbose:
        print("inert probes (the command changed nothing):")
        for where in effect.inert:
            print("  " + where)
    print("applied: " + ", ".join(
        f"{name} {effect.applied.get(name, 0)}/{effect.probed[name]}"
        for name in sorted(effect.probed)))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path,
                        default=REPO_ROOT / "build" / "microide" / "microide")
    parser.add_argument("--only", help="restrict to commands containing this substring")
    parser.add_argument("--startup-timeout", type=int, default=180)
    parser.add_argument("--verbose", action="store_true",
                        help="list every probe in which the command changed nothing")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"no such binary: {args.binary}", file=sys.stderr)
        return 2
    if not os.environ.get("XDG_RUNTIME_DIR"):
        print("XDG_RUNTIME_DIR must be set to a SHORT path (AF_UNIX caps the socket "
              "path at 108 bytes); use the real /run/user/<uid>.", file=sys.stderr)
        return 2

    scratch = Path(tempfile.mkdtemp(prefix="microide-sweep-"))
    project = scratch / "project"
    project.mkdir()
    # One log per session. A shared log would break startup detection outright:
    # wait_for_socket returns the FIRST "ready" line it finds, so session 2 would
    # connect to session 1's dead socket.
    session_logs: list[Path] = []
    session_count = 0

    @contextlib.contextmanager
    def open_session():
        """One instance, torn down when the caller is done with it.

        Per fixture rather than per run, so a session never approaches
        `kMaxOpenTabsPerGroup` (512): each case is one `open`, past the ceiling
        the group has no room, and every later case would then be measured
        against whatever tab happened to stay active.
        """
        nonlocal session_count
        session_count += 1
        log_path = scratch / f"app{session_count}.log"
        session_logs.append(log_path)
        app, sock_path = start_app(args.binary, project, scratch, log_path,
                                   args.startup_timeout)
        driver = Driver(sock_path)
        try:
            yield driver
        finally:
            driver.close()
            try:
                os.killpg(os.getpgid(app.pid), 9)
            except (ProcessLookupError, PermissionError):
                app.kill()
            app.wait(timeout=30)

    try:
        failures = sweep(open_session, project, args.only, args.verbose)
    except Failure as error:
        print(f"sweep aborted: {error}", file=sys.stderr)
        return 2

    for log in session_logs:
        text = log.read_text(errors="replace")
        if "ERROR: AddressSanitizer" in text:
            marker = text.find("ERROR: AddressSanitizer")
            failures.append(f"SANITIZER report during the sweep ({log.name}):\n" +
                            text[marker:marker + 4000])

    for failure in failures:
        print(failure)
        print("-" * 72)
    if failures or args.keep:
        print(f"scratch kept at {scratch}")
    else:
        shutil.rmtree(scratch, ignore_errors=True)
    print(f"{len(failures)} failing invariant(s)")
    return len(failures)


if __name__ == "__main__":
    sys.exit(main())
