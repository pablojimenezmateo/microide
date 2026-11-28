#!/usr/bin/env python3
"""microide_qa — drive a real microide window the way a person does, and check it.

The Playwright of this repo. There is no DOM, so the pieces are:

  * a private Xvfb display and an isolated XDG tree per run (the user's real
    config, session and sockets are never touched) -- the recipe
    tools/capture-media/lib.sh proved, reused here rather than copied;
  * real input through xdotool: mouse moves, clicks, drags, wheel, key chords,
    typing -- the same events SDL gets from a person;
  * the control channel for commands and state (`--query`), including the `ui`
    query that names on-screen elements (menu items, tabs, rows, buttons) by
    their rects, which is what makes "click the Edit menu" a locator rather
    than a coordinate;
  * oracles that need no expected screenshot: the app's own redraw verifier
    (MICROIDE_QA_VERIFY_REDRAW=1 renders every partial frame a second time in
    full and logs any pixel that differs -- the stale-paint class), sanitizer
    reports, and the process staying alive;
  * evidence: the app's per-frame redraw trace is on for every run, and a
    finding is written as a directory with the actions up to it, the frame lines
    since the previous action, the last screenshots and its own. A frame that
    shows once and never again (TD-2026-10-15-343) is lost without this.

Library use:

    with App.launch(project=fixture) as app:
        app.click_named("menu:Edit")
        app.expect(app.ui()["menu"]["open"], "the Edit menu opens")
        app.check()            # raises on any oracle finding so far

See tools/qa/run.py for the scenario suite and the seeded monkey.
"""

from __future__ import annotations

import collections
import contextlib
import ctypes
import gzip
import importlib.util
import json
import re
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterator

from PIL import Image

import xtest
from recording import (RR_MODES, finish_recordings, is_rr_line, rr_failure, rr_mode,  # noqa: F401
                       rr_unusable_reason)

REPO = Path(__file__).resolve().parents[2]
CAPTURE = REPO / "tools" / "capture-media"


def _load_xwd2png():
    spec = importlib.util.spec_from_file_location("xwd2png", CAPTURE / "xwd2png.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


_XWD = _load_xwd2png()

# Log lines that are a finding whatever the scenario was doing.
FINDING_MARKERS = (
    "QA-STALE",                  # the redraw verifier: a partial frame left stale pixels
    "QA-OVERRUN",                # a draw outside its element (render::PaintElement)
    "ERROR: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "runtime error:",            # UBSan
    "WARNING: ThreadSanitizer",
    "Assertion",                 # SDL_assert / assert()
    "terminate called",
    # glibc's allocator noticing corruption: how an unsanitized binary reports the
    # heap bugs the command fuzzer found (TD-2026-09-07-291).
    "malloc():",
    "free():",
    "double free",
    "corrupted size",
    # A signal a sanitizer caught (a segfault under ASan prints this, then its
    # report), and the C++ runtime's own last words.
    "DEADLYSIGNAL",
    "pure virtual method called",
    "std::bad_alloc",
    # The app's notices for states its code says cannot happen.
    "should not happen",
    # A persisted record (the session a kill -9 restores from) that did not land,
    # or that landed and its own reader rejects: a restore that goes back in time.
    "persistence: could not write",
    "persistence: could not encode",
    "read but did not decode",
)


_INJECTED_WRITE_FAILURE = re.compile(r"microide crash-point: failed \S+ \((.+)\) with errno")
_STATE_WRITE_FAILED = re.compile(r"persistence: could not write (.+?) \(")
_STAGING_NAME = re.compile(r"^\.(.+)\.microide-staging\.\d+\.\d+$")


def finding_lines(lines: list[str], injected: set[str] | None = None) -> list[str]:
    """The log lines the oracles report. A state write that failed because a
    driver armed a fault at it (`crash-points` fail=, logged by the app as
    `crash-point: failed <site> (<path>)` just before) is the injection, not a
    finding; `injected` carries those paths across calls."""
    injected = set() if injected is None else injected
    found: list[str] = []
    for line in lines:
        if (failed := _INJECTED_WRITE_FAILURE.search(line)) is not None:
            path = Path(failed.group(1))
            staging = _STAGING_NAME.match(path.name)
            injected.add(str(path.parent / (staging.group(1) if staging else path.name)))
            continue
        if not is_finding_line(line):
            continue
        if (state := _STATE_WRITE_FAILED.search(line)) is not None and state.group(1) in injected:
            continue
        found.append(line)
    return found


_FRAME_MONO = re.compile(r"^microide redraw frame: .* mono=([0-9.]+)")


def interleave_timeline(actions: list[dict[str, Any]], log_lines: list[str], keep: int = 400) -> str:
    """The run's last actions and the app's frame lines in one time order: the
    driver stamps each replay row with time.monotonic() (`t`), and the app each
    frame line with the same CLOCK_MONOTONIC (`mono=`), so the frames an action
    caused stand right after it."""
    events: list[tuple[float, int, str]] = []
    for index, action in enumerate(actions):
        if isinstance(action.get("t"), (int, float)):
            row = json.dumps({k: v for k, v in action.items() if k != "t"}, ensure_ascii=False)
            events.append((float(action["t"]), 0, f"action {index}: {row[:300]}"))
    for line in log_lines:
        if (match := _FRAME_MONO.match(line)) is not None:
            events.append((float(match.group(1)), 1, line))
    events.sort(key=lambda event: (event[0], event[1]))
    return "".join(f"{when:.3f}  {text}\n" for when, _, text in events[-keep:])


def loaded_diff(tab: dict[str, Any] | None) -> bool:
    """A diff tab whose sides have been read: a diff opens as a stand-in and
    loads on the reader thread, and commands on its changes wait for that."""
    return bool(tab) and tab.get("kind") == "compare" and not tab.get("loading", False)


def is_finding_line(line: str) -> bool:
    """A log line an oracle reports. The input trace echoes what was typed, so a
    typed "Assertion" is not one."""
    return ("microide input: " not in line and not is_rr_line(line)
            and any(marker in line for marker in FINDING_MARKERS))

# Sanitizer runtime defaults for a QA run (a sanitizer build reads them; others
# ignore them). halt_on_error so the first report is the one attributable to the
# last action, not the cascade after a corrupted heap; leaks off unless asked
# (MQA_DETECT_LEAKS=1), since a killed GUI process "leaks" everything it held.
SANITIZER_ENV = {
    # handle_abort: an assertion's abort prints its stack as a segfault does (the
    # app's own crash handler stands down under a sanitizer).
    "ASAN_OPTIONS": "halt_on_error=1:handle_abort=1:detect_leaks=" + os.environ.get("MQA_DETECT_LEAKS", "0"),
    "LSAN_OPTIONS": f"suppressions={REPO / 'tests' / 'lsan.supp'}",
    "UBSAN_OPTIONS": "print_stacktrace=1",
    "TSAN_OPTIONS": f"suppressions={REPO / 'tests' / 'tsan.supp'}",
}


def process_children(pid: int) -> list[int]:
    """The direct children of every thread of `pid` (empty when it is gone)."""
    out: list[int] = []
    with contextlib.suppress(OSError, ValueError):
        for task in Path(f"/proc/{pid}/task").iterdir():
            with contextlib.suppress(OSError, ValueError):
                out += [int(child) for child in (task / "children").read_text().split()]
    return out


def machine_pressure() -> str:
    """The machine's load as a finding sees it: the load averages and the kernel's
    pressure-stall figures. A latency or timed-out finding from a starved machine
    is the machine's; without this the two looked the same in the ledger."""
    lines = [f"cores: {os.cpu_count()}"]
    for name in ("loadavg", "pressure/cpu", "pressure/io", "pressure/memory"):
        with contextlib.suppress(OSError):
            for line in Path(f"/proc/{name}").read_text().splitlines():
                lines.append(f"{name}: {line}")
    return "\n".join(lines) + "\n"


# How long a main-loop turn runs before the app prints where its main thread is
# (MICROIDE_TRACE_STALL_MS, stretched by the slow factor): under the latency
# oracle's budget, so a slow answer's stack is in the log by the time it is one.
STALL_TRACE_SECONDS = 1.0
# A late reply's wait: after MAIN_THREAD_SAMPLE_AFTER_S the request loop samples
# the main thread every MAIN_THREAD_SAMPLE_S (App.slowest_waits).
MAIN_THREAD_SAMPLE_AFTER_S = 1.0
MAIN_THREAD_SAMPLE_S = 0.2


# The desktop's input-method selection, which the app under test must not inherit
# (TD-2026-10-27-395). With `XMODIFIERS=@im=ibus` from a developer's session, SDL
# routed the private display's keys through that desktop's IBus, which committed a
# Latin-1 keysym double-encoded (`é` -> `Ã©`): unicode-editing failed at every desk
# with IBus running and passed on CI, which has none. A run must not depend on the
# desk it runs at, as its own HOME and XDG tree already say.
DESKTOP_INPUT_METHOD_ENV = ("XMODIFIERS", "GTK_IM_MODULE", "QT_IM_MODULE", "SDL_IM_MODULE")

# What a microide host terminal gives its shell (RemoteServer::SetShellEnvironment,
# TerminalTable): the server it belongs to, its handle there, and that server's URL
# forwarder. A run started from such a shell must not hand them to the app under
# test -- its shells would open pages on the developer's own client and reach the
# developer's own server.
HOST_TERMINAL_ENV = ("MICROIDE_SERVER_SOCKET_DIR", "MICROIDE_TERMINAL_HANDLE", "BROWSER")

# First on every app's PATH: stand-ins for the tools that would reach the desktop
# of whoever runs the suite (see desktopbin/notify-send).
DESKTOPBIN = Path(__file__).resolve().parent / "desktopbin"


def git_fence_env(project: Path) -> dict[str, str]:
    """What keeps every git a run starts -- the driver's and the app's -- inside
    `project`: no upward repository search past it, no network transport.

    Scratch projects live under build/, inside the microide checkout. A project
    whose `.git` a run broke otherwise resolves to the CHECKOUT, which is how a
    soak run once committed to main, switched the checkout's branch and pushed a
    branch to GitHub (TD-2026-11-09-436)."""
    return {"GIT_CEILING_DIRECTORIES": str(Path(project).resolve().parent),
            "GIT_ALLOW_PROTOCOL": "file"}


def app_base_env(environ: dict[str, str]) -> dict[str, str]:
    """The environment an app under test starts from: the caller's, less what
    ties the run to the desktop, or the microide host terminal, it was started on."""
    dropped = DESKTOP_INPUT_METHOD_ENV + HOST_TERMINAL_ENV
    return {key: value for key, value in environ.items() if key not in dropped}


# util::kMinimumLayoutWidth / kMinimumLayoutHeight: the smallest layout (in layout
# units) the workbench is laid out for.
LAYOUT_FLOOR = (640, 400)

# MQA_UI_SCALE runs everything at that ui.scale (the qa-scale lane). Window sizes
# a flow names are layout units, as the `ui` query reports them: the driver makes
# the window that many units at the scale, so a flow written for scale 1 lays out
# the same workbench (a 1280x720 window at 1.75 was 731x411 units, below the
# layout floor, and every size-sensitive flow failed on that alone).
UI_SCALE = float(os.environ.get("MQA_UI_SCALE") or 1.0)


def device_size(width: int, height: int) -> tuple[int, int]:
    """The window size, in X pixels, that holds `width` x `height` layout units."""
    return round(width * UI_SCALE), round(height * UI_SCALE)

# SDL's defaults (SDL_HINT_MOUSE_DOUBLE_CLICK_TIME / _RADIUS), with margin.
DOUBLE_CLICK_SECONDS = 0.55

# The window system a run drives: "x11" (Xvfb + xdotool, the default) or
# "wayland" (a headless weston; input goes through the control channel's `pointer`
# and `key` commands, since a Wayland compositor takes input from no other client).
BACKEND = os.environ.get("MQA_BACKEND", "x11")
# A window manager on the run's Xvfb ("openbox"; empty: none, the default). With
# one, the app's own maximize and fullscreen reach a real window manager and
# come back as the configure sequences a desktop sends.
WINDOW_MANAGER = os.environ.get("MQA_WM", "")
# How much slower than a release build the binary under test is; every control
# timeout is multiplied by it (a coverage build ran `control-list` past the 3 s a
# fast build answers in 50 ms).
SLOW_FACTOR = float(os.environ.get("MQA_SLOW_FACTOR", "1"))
# The git view's elements: their rows move whenever a git refresh lands.
GIT_VIEW_ELEMENTS = ("row:git/", "button:git/", "field:git/")
# Integer output scale for the Wayland backend: 2 is a HiDPI screen.
WAYLAND_SCALE = int(os.environ.get("MQA_WAYLAND_SCALE", "1"))
# weston: on PATH, or unpacked by tools/qa/weston.sh under this root.
WESTON_ROOT = Path(os.environ.get("MQA_WESTON_ROOT", str(Path.home() / ".cache" / "microide-qa" / "weston" / "root")))
# The X keymap Xvfb starts with. xkeyboard-config gives F1-F12 the CTRL+ALT key
# type (its fifth level switches virtual terminals), and xdotool 3.20160805 then
# resolves a plain F5 to a level it reaches by holding Alt: every F-key press
# arrived as Alt+F<n> (keys, debug-keys, lsp-rename, menus' F10 ... all failed on
# a machine with xkb-data 2.46). The server refuses a keymap uploaded after start
# (BadValue), so each Xvfb starts from a copy of the XKB tree whose `pc` symbols
# take the plain two-level F-keys (srvr_ctrl(no_srvr_keys)), built once.
XKB_SOURCE = Path("/usr/share/X11/xkb")
XKB_CACHE = Path.home() / ".cache" / "microide-qa" / "xkb"


def plain_fkey_xkb_dir() -> Path | None:
    """An XKB directory whose F-keys have no VT-switch level, or None when the
    system's needs no change (or there is none to copy)."""
    pc = XKB_SOURCE / "symbols" / "pc"
    try:
        text = pc.read_text()
    except OSError:
        return None
    if 'include "srvr_ctrl(fkey2vt)"' not in text:
        return None
    stamp = str(int(pc.stat().st_mtime))
    target = XKB_CACHE / stamp
    if (target / "symbols" / "pc").exists():
        return target
    XKB_CACHE.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="build-", dir=XKB_CACHE))
    shutil.copytree(XKB_SOURCE.resolve(), scratch / "xkb", symlinks=False)
    patched = scratch / "xkb" / "symbols" / "pc"
    patched.write_text(text.replace('include "srvr_ctrl(fkey2vt)"', 'include "srvr_ctrl(no_srvr_keys)"'))
    try:
        (scratch / "xkb").rename(target)  # atomic: a parallel start sees all or nothing
    except OSError:
        pass  # another driver got there first; its copy is the same
    shutil.rmtree(scratch, ignore_errors=True)
    return target

# xdotool keysyms as the app's `key` command spells them.
_KEYSYM_TO_CHORD = {
    "Return": "enter", "Escape": "escape", "BackSpace": "backspace", "Delete": "delete",
    "Prior": "pageup", "Next": "pagedown", "Home": "home", "End": "end", "Tab": "tab",
    "Up": "up", "Down": "down", "Left": "left", "Right": "right", "space": "space",
    "Insert": "insert", "grave": "`", "comma": ",", "period": ".", "slash": "/",
    "backslash": "\\", "bracketleft": "[", "bracketright": "]", "minus": "-", "equal": "=",
    "semicolon": ";", "apostrophe": "'",
}


def chord_for_key_command(chord: str) -> str:
    """An xdotool chord ("ctrl+shift+Prior", "G") as the `key` command spells it."""
    *mods, key = chord.split("+") if chord != "+" else ["+"]
    key = _KEYSYM_TO_CHORD.get(key, key)
    if len(key) == 1 and key.isalpha() and key.isupper():
        mods.append("shift")
        key = key.lower()
    return "+".join([m.lower().replace("control", "ctrl").replace("super", "meta") for m in mods] + [key])
DOUBLE_CLICK_RADIUS = 40


WORK_OWNER_FILE = "owner.pid"
_swept_orphans = False
# Set to the run's work tree in the app's environment, so every process the app
# starts carries it -- and keeps it, whatever process group or session it moves
# to. See kill_run_processes.
RUN_MARKER = "MQA_RUN"


def kill_run_processes(work: Path) -> int:
    """SIGKILLs every process left from the run whose work tree is `work`: each
    one whose environment carries RUN_MARKER for it. Returns how many.

    Killing the app's process group is not enough. What the app starts can leave
    the group -- gdb runs a program in a group of its own, a terminal's shell in
    a session of its own -- and a debug session's processes outlived every run:
    the soak machine held 999 gdb processes, orphaned, when they were found
    (2026-11-13). Twice, since killing a process can strand a child it had not
    reaped yet."""
    needle = f"\0{RUN_MARKER}={work}\0".encode()
    own = os.getpid()
    killed = 0
    for _ in range(2):
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit() or int(entry.name) == own:
                continue
            try:
                environ = b"\0" + (entry / "environ").read_bytes()
            except OSError:
                continue  # gone, or not ours
            if needle in environ:
                with contextlib.suppress(ProcessLookupError, PermissionError):
                    os.kill(int(entry.name), signal.SIGKILL)
                    killed += 1
    return killed


def sweep_orphaned_work_trees(root: Path = Path("/tmp"), legacy_age_s: float = 24 * 3600) -> list[Path]:
    """Removes this user's /tmp/mqa-* work trees whose driver process is gone.

    A tree is removed by App.stop() in a `finally`, which a killed driver never
    runs: a remote run left its whole mirror behind each time (~100 MB), and dozens
    of them filled the user's tmpfs quota until every test failed with EDQUOT
    (2026-11-02). A tree records its driver's pid (WORK_OWNER_FILE); one with no
    record (made before it existed) goes once it is a day old. MQA_KEEP_WORK=1
    keeps them, as it keeps a tree at stop. Once per process; returns what went."""
    global _swept_orphans
    if _swept_orphans or os.environ.get("MQA_KEEP_WORK") == "1":
        return []
    _swept_orphans = True
    removed: list[Path] = []
    for tree in root.glob("mqa-*"):
        try:
            if not tree.is_dir() or tree.is_symlink() or tree.stat().st_uid != os.getuid():
                continue
            owner = tree / WORK_OWNER_FILE
            if owner.exists():
                pid = int(owner.read_text().strip() or "0")
                try:
                    os.kill(pid, 0)
                    continue  # its driver still runs
                except ProcessLookupError:
                    pass
                except PermissionError:
                    continue  # a live process of someone else's: not ours to judge
            elif time.time() - tree.stat().st_mtime < legacy_age_s:
                continue
        except (OSError, ValueError):
            continue
        kill_run_processes(tree)  # what its app left running
        shutil.rmtree(tree, ignore_errors=True)
        removed.append(tree)
    return removed


class QaFailure(AssertionError):
    pass


class ReplayStopped(Exception):
    """A replay's `stop` asked it to end before its next action (the soak's
    minimizer, on its budget or a pause). Not a QaFailure: nothing went wrong
    with the app, and the replay says nothing about the finding."""


class AppGone(QaFailure):
    """The request failed because the app is dead or dying -- not a refusal, not
    a stall. The crash oracle (App.check) reports the death with its status and
    stack; a caller that files failures as findings leaves this one to it."""


# ---- what the app prints when it dies or stalls (platform/CrashDiagnostics.h) ----

CRASH_BLOCK = re.compile(r"^microide crash: signal .*?^microide crash: end of backtrace$", re.M | re.S)
STALL_BLOCK = re.compile(r"^microide stall: the main loop has not turned .*?"
                         r"^microide stall: (?:end of backtrace|ended after [^\n]*)$", re.M | re.S)
# A frame as glibc's backtrace_symbols prints it: `module(symbol+0xoff) [0xaddr]`,
# `module(+0xoff) [0xaddr]` for a function a PIE does not export.
_RAW_FRAME = re.compile(r"^(?P<module>[^\s(]+)\((?P<symbol>[^+)]*)\+?(?P<offset>0x[0-9a-f]+)?\) \[0x[0-9a-f]+\]$")
# The handler's own frames and the kernel's signal trampoline above them.
_DIAGNOSTIC_FRAMES = ("CrashDiagnostics.h", "crash_diagnostics_detail", "__restore_rt")


def damage_requests_before(log: str, frame: int) -> list[str]:
    """The repaint requests the app logged for frame `frame` (MICROIDE_TRACE_DAMAGE):
    each `microide damage:` block between the previous frame's line and the
    `QA-STALE frame=<frame>` line, frames included.

    >>> log = "\\n".join(["microide redraw frame: frame=6 x", "microide damage: 1,2,3,4", "  #1 a(+0x1) [0x9]",
    ...                    "noise", "microide damage: full", "  #1 b(+0x2) [0x8]", "QA-STALE frame=7 y"])
    >>> damage_requests_before(log, 7)
    ['microide damage: 1,2,3,4\\n  #1 a(+0x1) [0x9]', 'microide damage: full\\n  #1 b(+0x2) [0x8]']
    >>> damage_requests_before(log, 8)
    []
    """
    lines = log.splitlines()
    stale = next((i for i, line in enumerate(lines) if f"QA-STALE frame={frame} " in line + " "), None)
    if stale is None:
        return []
    start = 0
    for i in range(stale - 1, -1, -1):
        if "microide redraw frame: frame=" in lines[i]:
            start = i + 1
            break
    blocks: list[list[str]] = []
    taking = False
    for line in lines[start:stale]:
        if line.startswith("microide damage:"):
            blocks.append([line])
            taking = True
        elif taking and line.startswith("  #"):
            blocks[-1].append(line)
        else:
            taking = False
    return ["\n".join(block) for block in blocks]


def symbolize_backtrace(block: str, binary: Path, timeout: float = 120.0) -> str:
    """A crash or stall block of the app's log with its frames resolved: one
    `#N 0xOFF in function file:line` per frame (the shape findings.stack_key
    reads), inlined frames included, the handler's own frames dropped.

    Frames of the app's binary are offsets (`microide(+0x1a2b3c)`), resolved with
    addr2line against `binary` -- the soak keeps each commit's with its debug
    info; without debug info the function name alone comes from the symbol
    table. A frame of a shared library keeps the symbol backtrace printed."""
    lines = block.splitlines()
    frames = [(index, match) for index, line in enumerate(lines) if (match := _RAW_FRAME.match(line.strip()))]
    name = Path(binary).name
    ours = [(index, match) for index, match in frames
            if Path(match["module"]).name == name and not match["symbol"] and match["offset"]]
    resolved: dict[int, list[str]] = {}
    tool = shutil.which("addr2line")
    if ours and tool is not None:
        # A return address is the instruction AFTER the call: one byte back is
        # inside the call, on the caller's line.
        addresses = [hex(max(0, int(match["offset"], 16) - 1)) for _, match in ours]
        try:
            out = subprocess.run([tool, "-e", str(binary), "-f", "-C", "-i", "-a", *addresses],
                                 capture_output=True, text=True, timeout=timeout, check=False).stdout
        except (OSError, subprocess.TimeoutExpired):
            out = ""
        blocks: list[list[str]] = []
        for line in out.splitlines():
            if line.startswith("0x"):
                blocks.append([])
            elif blocks:
                blocks[-1].append(line)
        for (index, _), parts in zip(ours, blocks):
            resolved[index] = [f"{parts[i]} {parts[i + 1].split(' (discriminator')[0]}"
                               for i in range(0, len(parts) - 1, 2)]
    header = [line for index, line in enumerate(lines) if not any(index == i for i, _ in frames)]
    out_lines = header[:1]
    number = 0
    for index, match in frames:
        offset = match["offset"] or "0x0"
        for text in resolved.get(index) or [None]:
            if text is None:
                where = match["symbol"] or "??"
                text = f"{where} ?? [{Path(match['module']).name}+{offset}]"
            elif text.endswith("??:0") or text.endswith("??:?"):
                text = f"{text.rsplit(' ', 1)[0]} ?? [{name}+{offset}]"
            if any(marker in text for marker in _DIAGNOSTIC_FRAMES):
                continue
            out_lines.append(f"#{number} {offset} in {text}")
            number += 1
    return "\n".join(out_lines) + "\n"


def signal_name(code: int | None) -> str:
    """ "SIGABRT" for an exit status of -6; empty for an ordinary exit.

    >>> signal_name(-6), signal_name(-11), signal_name(1), signal_name(None)
    ('SIGABRT', 'SIGSEGV', '', '')
    """
    if code is None or code >= 0:
        return ""
    try:
        return signal.Signals(-code).name
    except ValueError:
        return f"signal {-code}"


@dataclass
class Finding:
    kind: str
    detail: str
    screenshot: str | None = None


class MainThreadProfiler:
    """`perf record` of the app's main thread while a control reply is late, and
    the report of where its CPU went. Not under rr (the recorder owns the
    tracee's counters, and the recording replays the turn anyway), not without
    perf, and never a failure of the request it watches."""

    REPORT_BYTES = 48 * 1024
    # One thread's samples need little buffer, and perf's default (per CPU,
    # charged to the user's perf_event_mlock_kb, 516 KB here) let two or three
    # sessions -- the soak's jobs profile at once -- take it all, and the next
    # `perf record` failed with nothing to report.
    MMAP_PAGES = 16

    def __init__(self, process: subprocess.Popen[bytes], data: Path, binary: Path) -> None:
        self.process, self.data, self.binary = process, data, binary

    @classmethod
    def start(cls, app: "App") -> "MainThreadProfiler | None":
        perf = shutil.which("perf")
        work = getattr(app, "work", None)
        if perf is None or work is None or getattr(app, "recording", False) or app.proc is None:
            return None
        data = Path(work) / "latency.perf.data"
        try:
            process = subprocess.Popen(
                [perf, "record", "--quiet", "-F", "497", "--call-graph", "dwarf,16384", "-m", str(cls.MMAP_PAGES),
                 "-t", str(app.pid), "-o", str(data)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            return None
        return cls(process, data, app.binary)

    def _stop(self) -> bool:
        with contextlib.suppress(ProcessLookupError):
            self.process.send_signal(signal.SIGINT)
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            return False
        return self.data.exists()

    def discard(self) -> None:
        self._stop()
        with contextlib.suppress(OSError):
            self.data.unlink()

    def finish(self) -> str:
        """The main thread's hottest functions (self time) and their callers."""
        try:
            if not self._stop():
                return ""
            report = subprocess.run(
                ["perf", "report", "-i", str(self.data), "--stdio", "--no-children", "--sort", "dso,symbol",
                 "--percent-limit", "2", "-g", "caller,2,callee,function,percent", "--max-stack", "16"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=120, check=False)
            text = report.stdout.decode("utf-8", "replace")
            return text[:self.REPORT_BYTES]
        except (OSError, subprocess.TimeoutExpired):
            return ""
        finally:
            with contextlib.suppress(OSError):
                self.data.unlink()


@dataclass
class App:
    binary: Path
    work: Path
    display: str
    size: tuple[int, int]
    env: dict[str, str]
    xvfb: subprocess.Popen | None = None
    wm: subprocess.Popen | None = None
    window_manager_name: str = ""
    proc: subprocess.Popen | None = None
    log_path: Path | None = None
    # The project the run edits, which a replay has at a different path (a fresh
    # fixture, a remote project's mirror): drops under it are recorded relative
    # to it and replayed against the replay's own. Set by the monkey's runner.
    replay_root: Path | None = None
    log_offset: int = 0
    findings: list[Finding] = field(default_factory=list)
    actions: list[dict[str, Any]] = field(default_factory=list)
    shots: Path | None = None
    window: str | None = None
    # The window's top-left on the X root: ui rects are window coordinates and
    # xdotool moves in root coordinates.
    origin: tuple[int, int] = (0, 0)
    # The project the instance opened (a fresh fixture copy for local runs).
    project: Path | None = None
    # Evidence for a finding: the window after each of the last few actions, and
    # the app's frame-trace lines since the previous check.
    recent_shots: collections.deque = field(default_factory=lambda: collections.deque(maxlen=3))
    recent_log: list[str] = field(default_factory=list)
    injected_write_failures: set[str] = field(default_factory=set)
    # What a replay needs to rebuild the run: fixture kind, profile, seed.
    replay_header: dict[str, Any] = field(default_factory=dict)
    # action-counts of earlier processes of this run (restart, crash_and_relaunch).
    banked_action_counts: dict[str, dict[str, int]] = field(default_factory=dict)
    # A persistent connection for send_line (the command fuzzer's hot path).
    _socket: socket.socket | None = None
    _socket_buffer: bytes = b""
    _next_id: int = 1
    # (time, x, y) of the last press, for the double-click guard in click().
    _last_click: tuple[float, float, float] = (0.0, -1e9, -1e9)
    _launches: int = 0
    _reported_bad_utf8: bool = False
    _stall_captured: bool = False
    # Recording under rr ($MQA_RR, read once per App): the mode, the app's own pid
    # (rr's child; None until looked up, again after each launch) and the trace
    # directory of the current launch.
    _rr_mode: str = field(default_factory=rr_mode)
    _app_pid: int | None = None
    _trace_dir: Path | None = None
    _extra_args: list[str] = field(default_factory=list)
    # In-process XTEST input on the run's Xvfb (None on Wayland or without libXtst).
    _xt: Any = None
    # A flow changed the project or ran `git` outside the app (flows_common's
    # write/git_run) since the git view last settled: the watcher may not have
    # seen it yet, so wait_git_settled asks for the refresh itself.
    outside_change_pending: bool = False
    # The slowest control answer since the monkey last looked: (what, seconds).
    slowest_answer: tuple[str, float] = ("", 0.0)
    # Where that answer's time went, for the app's main thread: (seconds on a
    # CPU, seconds runnable and waiting for one) -- None when /proc could not
    # say -- and what the app's stall watchdog printed meanwhile.
    slowest_sched: tuple[float, float] | None = None
    slowest_stall: str = ""
    # Where the main thread sat while the slowest answer was owed: "wchan syscall"
    # sampled every MAIN_THREAD_SAMPLE_S once a reply is late, counted. The
    # stall watchdog sees one long loop turn; this sees a wait spread over many
    # short ones, or the loop asleep with the request queued (soak 049ca74b0cee:
    # 3.01 s "mostly blocked", the watchdog silent).
    slowest_waits: str = ""
    # The app's OTHER threads while the slowest answer was late: (seconds they spent
    # runnable and waiting for a CPU, summed, over a window of seconds). A main
    # thread blocked on a renderer whose rasterizer threads the machine starved
    # (lavapipe, the soak's CPU Vulkan, under a load of 25) was not slow itself.
    slowest_helpers: tuple[float, float] | None = None
    # Where the main thread's CPU went while the slowest answer was late: a `perf`
    # profile of that thread alone, started once the reply is MAIN_THREAD_SAMPLE_AFTER_S
    # late and stopped when it lands. The waits above say "running" and no more;
    # the watchdog sees only a single long turn (soak 32325094bfef: 7.76 s on a CPU
    # over many short turns, nothing to say what it computed).
    slowest_profile: str = ""
    # The crashing thread's stack, resolved, once a check has found the app dead
    # (symbolize_backtrace); empty until then and again after a relaunch.
    crash_stack: str = ""
    # The answers the last check read (ui, buffer, editor): the monkey's state
    # signature is built from them, so steering costs no extra query.
    last_ui: dict[str, Any] | None = None
    # How many actions were recorded when last_ui was read: it still describes
    # the screen while no action has been recorded since (Monkey._ui reuses it).
    last_ui_actions: int = -1
    last_buffer: dict[str, Any] | None = None
    # What the state oracles saw when one disagreed, written beside a state
    # finding as state-evidence.txt: the caret line's text, the buffer's tab size
    # and carets, and per re-read whether a frame was prepared in between. The
    # Ln/Col finding (soak 5a2cb8ba7f6b) never replayed, and its evidence could not
    # tell a frame that never came from a label that stayed stale across frames.
    state_evidence: list[str] = field(default_factory=list)
    last_editor: dict[str, Any] | None = None
    # Set while the monkey runs a burst: actions recorded then carry "b": 1, and a
    # replay sends them with no settle or check between (the timing they had).
    in_burst: bool = False
    # Whether the app was settled since the last recorded row: a row recorded with
    # no settle since the one before marks that one "n" (no settle after it), so a
    # replay sends the two back to back as the run did -- the rows of one monkey
    # step, an action's own several sends.
    settled_since_record: bool = True
    # Set while an oracle drives the app (the undo algebra's undos and redos):
    # what it sends is recorded with "o", after an {"oracle": ...} row, so a
    # replay re-runs the oracle there instead of replaying its commands blind.
    in_oracle: bool = False
    # Set by the monkey while it generates actions: given a press point, True when
    # the press must not happen (it would land on a window control and end the
    # run). click() and drag() then send and record nothing.
    press_guard: Callable[[float, float], bool] | None = None
    # Whether the app answers `idle` (settle_quiet): None until asked, False once
    # an app without it refused.
    _idle_query: bool | None = None
    # Its keyboard half: True when a key that ACTIVATES (Return, keypad Enter,
    # Space) would choose a row the run must not choose -- an open menu's
    # keyboard selection on Quit/Exit. key() and type() then close the menu with
    # Escape (recorded, so a replay does the same) before sending what they were
    # asked to. The pointer never pressed Quit; arrow keys walked onto it and a
    # typed newline chose it, and the app quit cleanly: an "exit (0)" filed as a
    # crash (soak 7ab9a5476905, 43 sightings).
    # Asked before a key that would choose the highlighted row of an open menu or
    # setting (True: send Escape first). Its argument is True for a key that only
    # STEPS a selected setting (Left/Right), which no menu row reacts to.
    activation_guard: Callable[[bool], bool] | None = None
    # A replay's own verdicts (run.py replay_record): whether it ran to the end
    # of its actions, and where it left the run's path (monkey_model.Divergence).
    replay_completed: bool = False
    divergence: Any = None
    _in_snapshot: bool = False

    # ---- lifecycle -------------------------------------------------------

    @classmethod
    @contextlib.contextmanager
    def launch(cls,
               project: Path | None = None,
               binary: Path | None = None,
               size: tuple[int, int] = (1440, 900),
               display: str | None = None,
               extra_args: list[str] | None = None,
               extra_env: dict[str, str] | None = None,
               shots: Path | None = None,
               keep_home: bool = False,
               plugins: tuple[str, ...] = (),
               window_manager: str | None = None) -> Iterator["App"]:
        binary = Path(binary or os.environ.get("MICROIDE_BIN") or REPO / "build/microide/microide")
        size = device_size(*size)
        if not binary.exists():
            raise FileNotFoundError(f"no microide binary at {binary} (set MICROIDE_BIN)")
        # Short path: the control socket lives under XDG_RUNTIME_DIR and AF_UNIX
        # paths stop at 108 bytes.
        sweep_orphaned_work_trees()
        work = Path(tempfile.mkdtemp(prefix="mqa-", dir="/tmp"))
        # Whose it is: a driver killed before its `finally` (a CI timeout, a SIGKILL,
        # a suite interrupted) leaves the tree, and the next launch removes it.
        (work / WORK_OWNER_FILE).write_text(str(os.getpid()))
        env = app_base_env(dict(os.environ))
        env.update({
            RUN_MARKER: str(work),
            "SDL_VIDEODRIVER": "wayland" if BACKEND == "wayland" else "x11",
            # One SDL_Log line per frame and per clip under an open menu: the frame
            # sequence a finding is read against.
            "MICROIDE_TRACE_REDRAW_VERBOSE": "1",
            "MICROIDE_TRACE_MENU_HOVER": "1",
            # One line per key and text event with the focus that took it: where a
            # keystroke that never reached the buffer went (TD-2026-10-20-368).
            "MICROIDE_TRACE_INPUT": "1",
            # One line per read, write, adopted baseline and undo-to-clean of a
            # buffer: the history a data finding's evidence quotes for its file.
            "MICROIDE_TRACE_BUFFER_IO": "1",
            # One line per sync-engine decision about a path holding something of
            # the user's (a save, a blocked save, a conflict): the history a remote
            # finding's evidence quotes for its file (diff-<file>.sync.txt).
            "MICROIDE_TRACE_MIRROR_SYNC": "1",
            # One line per terminal session start (with the shell's pid and the
            # code that started it), stop and end: which path made a leaked shell
            # (children-origin.txt in a leak finding).
            "MICROIDE_TRACE_TERMINAL_LIFE": "1",
            # The first caret a viewport leaves outside its document, with the
            # stack of the code that left it (TextViewport::TraceCaretBounds): the
            # invariants query only finds it steps later (soak 872c8984d39b).
            "MICROIDE_TRACE_CARET_BOUNDS": "1",
            # One line per repaint request with the stack that asked for it
            # (WorkspaceShell::RequestRedrawRect / RequestFullRedraw): a stale
            # frame's evidence names the code whose damage came instead of the
            # damage that was owed (damage_requests_before).
            "MICROIDE_TRACE_DAMAGE": "1",
            # A main-loop turn that runs this long prints the main thread's stack
            # (platform/CrashDiagnostics.h): where the loop was when an answer
            # was slow, which the latency oracle and a stall's evidence quote.
            "MICROIDE_TRACE_STALL_MS": str(round(STALL_TRACE_SECONDS * 1000 * SLOW_FACTOR)),
            # The durable-write boundaries, counted and armable (the `crash-points`
            # query, util/CrashPointControl.h): where the kill oracle kills.
            "MICROIDE_QA_CRASH_POINTS": "count",
            # Lets this driver's gdb attach when the app stops answering: yama's
            # ptrace scope 1 refuses a sibling otherwise (capture_stall).
            "MICROIDE_QA_PARENT_MAY_TRACE": "1",
            # The debug adapter's traffic, for a debugger flow's evidence.
            "MICROIDE_DAP_LOG": str(work / "dap.log"),
            "XDG_CONFIG_HOME": str(work / "config"),
            "XDG_STATE_HOME": str(work / "state"),
            "XDG_DATA_HOME": str(work / "data"),
            "XDG_RUNTIME_DIR": str(work / "run"),
            "MICROIDE_QA_VERIFY_REDRAW": "1",
            # The verifier saves the retained and reference images of the first
            # stale frames here; a finding copies the ones it names.
            "MICROIDE_QA_STALE_DUMP_DIR": str(work / "stale"),
            "SDL_AUDIODRIVER": "dummy",
        })
        for key, value in SANITIZER_ENV.items():
            env.setdefault(key, value)
        if not keep_home:
            # A remote run needs the real HOME for ssh keys and config; anything
            # else gets a throwaway one so the embedded shell is predictable.
            env["HOME"] = str(work)
            if project is not None:
                env.update(git_fence_env(project))
        env.update(extra_env or {})
        env["PATH"] = f"{DESKTOPBIN}:{env.get('PATH', '/usr/bin:/bin')}"
        for sub in ("config", "state", "data", "run", "stale"):
            (work / sub).mkdir(parents=True, exist_ok=True)
        os.chmod(work / "run", 0o700)
        # Bundled plugins a scenario needs (a language server, a debug adapter),
        # seeded as tools/capture-media/lib.sh seeds them for the media scenes.
        for plugin in plugins:
            # A bundled plugin, or a QA-only one (tools/qa/plugins: test content no
            # bundled plugin ships, such as snippets).
            source = REPO / "plugins" / plugin
            if not source.is_dir():
                source = REPO / "tools" / "qa" / "plugins" / plugin
            if not source.is_dir():
                raise FileNotFoundError(f"no plugin {plugin!r} under plugins/ or tools/qa/plugins/")
            shutil.copytree(source, work / "config" / "microide" / "plugins" / plugin)
        extra_args = list(extra_args or [])
        # MQA_UI_SCALE runs everything at that ui.scale (the qa-scale lane: the
        # fractional scales whose device grid is not the layout's, safety-plan W7).
        if os.environ.get("MQA_UI_SCALE") and "ui.scale" not in extra_args:
            extra_args += ["--set", "ui.scale", os.environ["MQA_UI_SCALE"]]
        app = cls(binary=binary, work=work, display=display or "", size=size, env=env, shots=shots,
                  project=project)
        app.window_manager_name = WINDOW_MANAGER if window_manager is None else window_manager
        try:
            try:
                app._start(project, extra_args)
            except Exception:
                # stop() removes the private work tree: keep startup evidence
                # before cleanup, including compositor and sanitizer output.
                if shots is not None:
                    startup = shots / "startup"
                    startup.mkdir(parents=True, exist_ok=True)
                    for log in work.glob("*.log"):
                        shutil.copy2(log, startup / log.name)
                raise
            yield app
        finally:
            app.stop()

    @property
    def wayland(self) -> bool:
        return BACKEND == "wayland"

    def _start(self, project: Path | None, extra_args: list[str]) -> None:
        w, h = self.size
        if self.wayland:
            self._start_weston(w, h)
            self.env.pop("DISPLAY", None)
            self.env["WAYLAND_DISPLAY"] = "mqa-wl"
        else:
            self._start_xvfb(w, h)
            if self.window_manager_name:
                self._start_window_manager()
            self.env["DISPLAY"] = self.display
            self._xt = xtest.XTest(self.display)
        self._extra_args = list(extra_args)
        self._spawn(project, extra_args)

    def _bank_action_counts(self) -> None:
        """Adds this process's `action-counts` to the run's running total before it
        exits: the coverage ledger reads the counts at the end of a scenario, and a
        restart would otherwise drop everything the first process ran."""
        if not self.alive():
            return
        with contextlib.suppress(Exception):
            counts = self.query("action-counts")
            for kind in ("commands", "plugins"):
                banked = self.banked_action_counts.setdefault(kind, {})
                for name, count in (counts.get(kind) or {}).items():
                    banked[name] = banked.get(name, 0) + int(count)

    def action_counts(self) -> dict[str, dict[str, int]]:
        """Every command run in this scenario, across restarts: the live process's
        `action-counts` plus what earlier processes banked."""
        total = {kind: dict(names) for kind, names in self.banked_action_counts.items()}
        with contextlib.suppress(Exception):
            counts = self.query("action-counts") if self.alive() else {}
            for kind in ("commands", "plugins"):
                bucket = total.setdefault(kind, {})
                for name, count in (counts.get(kind) or {}).items():
                    bucket[name] = bucket.get(name, 0) + int(count)
        return total

    def restart(self, project: Path | None = None, quit_timeout: float = 15.0,
                between: Callable[[], None] | None = None) -> None:
        """Quits the app the way a person does (the `quit` command, so session state
        is written as on a real exit) and starts it again on the same display, XDG
        tree and project -- the session-restore path. An unsaved buffer makes quit
        ask first; a scenario that wants that prompt drives it before calling this.
        `between` runs once the old process is gone and before the new one starts
        (what happens while the app is closed: a host server restarted, a wait)."""
        self._bank_action_counts()
        if self.alive():
            self.send("quit")
            deadline = time.monotonic() + quit_timeout
            while self.alive() and time.monotonic() < deadline:
                time.sleep(0.1)
            if self.alive():
                self.expect(False, "quit did not exit the app")
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(timeout=5)
        # The oracles over what the first run logged up to its exit; the exit
        # itself is the point, not a crash (check() would report it as one).
        for line in finding_lines(self.new_log_lines(), self.injected_write_failures):
            self.findings.append(Finding("log", line.strip()))
        if self.proc is not None and self.proc.returncode not in (0, None):
            self.expect(False, f"quit exited with status {self.proc.returncode}")
        elif not self.alive():
            # The `quit` above ran (the process left cleanly) but cannot be read back
            # from a process that is gone: the ledger counts it here.
            commands = self.banked_action_counts.setdefault("commands", {})
            commands["quit"] = commands.get("quit", 0) + 1
        if self._socket is not None:
            with contextlib.suppress(OSError):
                self._socket.close()
            self._socket = None
        self.log_offset = 0
        if between is not None:
            between()
        self._spawn(project if project is not None else self.project, self._extra_args)

    def crash_and_relaunch(self, project: Path | None = None,
                           between: Callable[[], None] | None = None,
                           restore_workspace: bool = False) -> None:
        """Kills the app with SIGKILL -- no quit, no session save, the crash path --
        and starts it again on the same display, XDG tree and project. `between`
        runs while it is down: what the killed process left on disk, before the
        next one reads (and rewrites) it.

        `restore_workspace` starts it with no project on its command line, as a
        user reopening the app after a crash does: the saved workspace comes back,
        every project of it. Naming a project opens THAT project instead of the
        workspace (`code <folder>`), so a second project's unsaved buffers were not
        in the relaunched app at all (soak e946ec9333d7)."""
        self._bank_action_counts()
        if self.alive():
            self._sigkill()
        for line in finding_lines(self.new_log_lines(), self.injected_write_failures):
            self.findings.append(Finding("log", line.strip()))
        if self._socket is not None:
            with contextlib.suppress(OSError):
                self._socket.close()
            self._socket = None
        self.log_offset = 0
        if between is not None:
            between()
        if restore_workspace:
            self._spawn(None, self._extra_args)
        else:
            self._spawn(project if project is not None else self.project, self._extra_args)

    def _spawn(self, project: Path | None, extra_args: list[str]) -> None:
        self.log_path = self.work / f"microide-{self._launches}.log" if self._launches else self.work / "microide.log"
        self._launches += 1
        # Input sent through X that the app has not been seen to take yet -- the
        # characters typed, the count of key presses, of button presses and
        # releases and of wheel notches, and the point a pointer move lands on --
        # and where in its log to look for them: see _await_typed_input.
        self._typed_pending = ""
        self._keys_pending = 0
        self._buttons_pending = 0
        self._wheels_pending = 0
        self._motion_pending: tuple[int, int] | None = None
        self._pointer_at: tuple[int, int] | None = None  # window coordinates, as last sent
        # Latin-1 characters bound to the display's free keycodes (_map_latin1), oldest
        # first. Per launch: a restart keeps the display, whose bound keycodes a fresh
        # listing then sees as taken.
        self._latin1_keycodes: dict[str, int] = {}
        self._free_keycodes: list[int] | None = None
        self._keys_sent: list[str] = []  # what the presses were, for a timeout's note
        self._typed_log_offset = 0
        # Absolute: the app starts in its work tree, not the caller's directory.
        self._app_pid = None
        self.crash_stack = ""
        args = [*self._recorder_args(), str(Path(self.binary).absolute())]
        if project is not None:
            args.append(str(Path(project).absolute()))
        args += ["--set", "control.enabled", "true", *extra_args]
        with open(self.log_path, "wb") as log:
            # Started in its private work tree, not the driver's directory (the
            # checkout, for the soak): a relative path the app resolves -- a file
            # saved as "1" -- must not land in a repository anyone cares about.
            self.proc = subprocess.Popen(args, env=self.env, stdout=log, stderr=subprocess.STDOUT,
                                         start_new_session=True, cwd=self.work)
        _wait(lambda: self._control_ready() or not self.alive(), 20.0,
              "the control channel never came up")
        if not self.alive():
            raise QaFailure(f"microide exited at startup; log: {self.log_path}")
        # Ready means its first frame is on screen (MICROIDE_TRACE_REDRAW_VERBOSE
        # logs each one): until the startup present returns, the main loop answers
        # nothing. A fixed settle here was a guess, and four apps starting at once
        # under software-rendered weston (scale 2) blocked in that present for
        # longer than it and the 5 s wait after -- `open`, then no frame and no tab
        # (qa-wayland: overlays, menus, overlay-backdrop, menu-clicks on one run).
        _wait(lambda: self._first_frame_presented() or not self.alive(), 20.0,
              "the window never presented its first frame")
        if not self.alive():
            raise QaFailure(f"microide exited at startup; log: {self.log_path}")
        # Looked up once the first frame is presented, when the window surely exists.
        # Looked up before it, on a fixed 5 s budget, a loaded machine (four apps
        # starting beside three sanitizer builds) once found none: every resize the
        # resize-overlays flow sent then went nowhere, and only its waits noticed.
        self.window = self._find_window()
        if not self.wayland and self.window is None:
            raise QaFailure(f"no X window found for microide (pid {self.pid}); log: {self.log_path}")
        self.origin = self._window_origin()
        self._apply_run_size()

    def _apply_run_size(self) -> None:
        """Makes the window the run's size. The app opens at its own initial size
        (Application.cpp kInitialWindowWidth x Height, 1440x900) whatever the
        screen, so a run at any other size -- the monkey's `--size`, a scaled run
        (device_size) -- drove a 1440x900 window on a larger screen. Waits for the
        `ui` query to report the size (layout units times the window's scale).
        The app comes up already at 1440x900 on a 1440x900 screen, the default
        run, and then this is a no-op."""
        if self.wayland or self.window is None:
            return
        width, height = self.size
        # At the screen's corner first: the app centres its window on the screen,
        # and grown in place there it ran off the screen's edge, where a click
        # lands on nothing.
        if (xt := self._xt_or_none()) is not None:
            xt.place(int(self.window), 0, 0)
        else:
            self._xdotool("windowmove", self.window, "0", "0")
        self._resize_window(width, height)

        def applied() -> bool:
            window = self.ui().get("window") or {}
            scale = (window.get("scale") or [1.0])[0] or 1.0
            return abs(window.get("width", 0) * scale - width) <= 2 and abs(window.get("height", 0) * scale - height) <= 2
        _wait(applied, 5.0, f"the window to take the run's {width}x{height} size")
        self.origin = self._window_origin()

    def _frames_logged(self) -> int:
        """How many frames this launch has logged (MICROIDE_TRACE_REDRAW_VERBOSE):
        read twice, it says whether the app prepared a frame in between."""
        with contextlib.suppress(OSError):
            return self.log_path.read_bytes().count(b"microide redraw frame:")
        return -1

    def _first_frame_presented(self) -> bool:
        with contextlib.suppress(OSError):
            return b"microide redraw frame: frame=1 " in self.log_path.read_bytes()
        return False

    @staticmethod
    def _dies_with_driver() -> Callable[[], None]:
        """A preexec_fn: the child is sent SIGTERM when this driver dies.

        Xvfb is not in the app's session, and a driver killed by anything that
        signals only its own process -- the soak's minimizer and verifier under
        subprocess.run(timeout=...), a crash, a ^C in the wrong place -- left it
        behind: 16 Xvfb servers sat orphaned for 3 to 13 days on the soak
        machine. The parent's pid is taken before the fork, so a driver that died
        between the fork and the prctl is caught too."""
        parent = os.getpid()

        def arm() -> None:
            libc = ctypes.CDLL(None, use_errno=True)
            libc.prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
            if os.getppid() != parent:
                os.kill(os.getpid(), signal.SIGTERM)
        return arm

    def _start_xvfb(self, w: int, h: int) -> None:
        """Xvfb picks a free display and tells us (-displayfd); scanning lock
        files raced when two drivers started at once on one machine."""
        xkb = plain_fkey_xkb_dir()
        xkb_args = ["-xkbdir", str(xkb)] if xkb is not None else []
        if self.display:
            self.xvfb = subprocess.Popen(
                ["Xvfb", self.display, *xkb_args, "-screen", "0", f"{w}x{h}x24", "-nolisten", "tcp"],
                env=self.tool_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                preexec_fn=self._dies_with_driver())
        else:
            read_fd, write_fd = os.pipe()
            self.xvfb = subprocess.Popen(
                ["Xvfb", "-displayfd", str(write_fd), *xkb_args, "-screen", "0", f"{w}x{h}x24",
                 "-nolisten", "tcp"],
                env=self.tool_env, pass_fds=(write_fd,), stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, preexec_fn=self._dies_with_driver())
            os.close(write_fd)
            with os.fdopen(read_fd) as reader:
                number = reader.readline().strip()
            if not number.isdigit():
                raise QaFailure("Xvfb did not report a display number")
            self.display = f":{number}"
        _wait(lambda: subprocess.run(["xdpyinfo", "-display", self.display], env=self.tool_env,
                                     capture_output=True).returncode == 0,
              5.0, "Xvfb never accepted connections")

    @property
    def window_manager(self) -> bool:
        """A window manager runs on this run's X display (MQA_WM)."""
        return self.wm is not None

    def _start_window_manager(self) -> None:
        """The MQA_WM window manager on the run's Xvfb, up once it has claimed the
        screen (_NET_SUPPORTING_WM_CHECK on the root window)."""
        if self.window_manager_name != "openbox" or shutil.which("openbox") is None:
            raise QaFailure(f"MQA_WM={self.window_manager_name!r}: only an installed openbox is supported")
        env = {**self.tool_env, "DISPLAY": self.display}
        self.wm = subprocess.Popen(["openbox", "--sm-disable"], env=env, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, preexec_fn=self._dies_with_driver())
        _wait(lambda: b"window id" in subprocess.run(
                  ["xprop", "-display", self.display, "-root", "_NET_SUPPORTING_WM_CHECK"],
                  env=env, capture_output=True).stdout or self.wm.poll() is not None,
              5.0, "openbox never claimed the screen")
        if self.wm.poll() is not None:
            raise QaFailure("openbox exited at startup")

    def _start_weston(self, w: int, h: int) -> None:
        """A headless weston of its own (kiosk shell: the one window fills the
        output, as a maximized window does) on a socket under this run's
        XDG_RUNTIME_DIR. Nothing reaches the user's own session."""
        weston = WESTON_ROOT / "usr" / "bin" / "weston"
        env = dict(self.env)
        if weston.exists():
            lib = WESTON_ROOT / "usr" / "lib" / "x86_64-linux-gnu"
            env["LD_LIBRARY_PATH"] = f"{lib}:{lib / 'weston'}"
            # Weston loads its modules from the absolute path it was built with.
            env["WESTON_MODULE_MAP"] = ";".join(
                f"{module.name}={module}" for module in
                sorted(lib.glob("libweston-*/*.so")) + sorted((lib / "weston").glob("*.so")))
        elif shutil.which("weston"):
            weston = Path(shutil.which("weston"))
        else:
            raise FileNotFoundError("no weston: install it, or run tools/qa/weston.sh to unpack one")
        # weston's --width/--height are the output's logical size and --scale its
        # pixel density, so the window is the run's size in layout units at any scale.
        self.xvfb = subprocess.Popen(
            [str(weston), "--backend=headless", "--shell=kiosk", "--socket=mqa-wl", "--no-config",
             f"--width={w}", f"--height={h}", f"--scale={WAYLAND_SCALE}"],
            env=env, stdout=open(self.work / "weston.log", "wb"), stderr=subprocess.STDOUT,
            start_new_session=True)
        socket_path = Path(self.env["XDG_RUNTIME_DIR"]) / "mqa-wl"
        _wait(lambda: socket_path.exists() or self.xvfb.poll() is not None, 10.0,
              "weston never opened its socket")
        if self.xvfb.poll() is not None:
            raise QaFailure(f"weston exited at startup; log: {self.work / 'weston.log'}")
        self.display = "wayland:mqa-wl"

    def stop(self) -> None:
        self._end_clipboard_owner()
        if (xt := getattr(self, "_xt", None)) is not None:
            xt.close()
            self._xt = None
        if self._socket is not None:
            with contextlib.suppress(OSError):
                self._socket.close()
            self._socket = None
        for proc in (self.proc, self.wm, self.xvfb):
            if proc is not None and proc.poll() is None:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(proc.pid, signal.SIGTERM) if proc is self.proc else proc.terminate()
                # rr closes its trace on SIGTERM, after its tracees have gone.
                with contextlib.suppress(subprocess.TimeoutExpired):
                    proc.wait(timeout=20 if proc is self.proc and self.recording else 5)
                if proc.poll() is None:
                    proc.kill()
        kill_run_processes(self.work)
        if os.environ.get("MQA_KEEP_WORK") != "1":
            shutil.rmtree(self.work, ignore_errors=True)

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    @property
    def recording(self) -> bool:
        """This launch runs under `rr record` (MQA_RR)."""
        return bool(self._rr_mode)

    @property
    def pid(self) -> int | None:
        """The app's own pid: what /proc, a signal or a window search is about.
        Recorded, `self.proc` is rr and the app is the child of it that runs the
        binary; the oracles read the app's descriptors and threads, not rr's."""
        if self.proc is None:
            return None
        if not self.recording:
            return self.proc.pid
        if self._app_pid is None:
            binary = os.path.realpath(self.binary)
            for child in process_children(self.proc.pid):
                with contextlib.suppress(OSError):
                    if os.path.realpath(f"/proc/{child}/exe") == binary:
                        self._app_pid = child
                        break
        return self._app_pid if self._app_pid is not None else self.proc.pid

    def recording_dir(self) -> Path | None:
        """Where this run's rr traces go: beside the screenshots, so they outlive
        the private work tree, else inside it."""
        if not self.recording:
            return None
        return (self.shots if self.shots is not None else self.work) / "rr"

    def _recorder_args(self) -> list[str]:
        """The `rr record` prefix of this launch's command line, or nothing."""
        if not self.recording:
            return []
        reason = rr_unusable_reason()
        if reason is not None:
            raise QaFailure(f"MQA_RR is set but rr cannot record here: {reason}")
        traces = self.recording_dir()
        traces.mkdir(parents=True, exist_ok=True)
        self._trace_dir = traces / f"launch-{self._launches - 1}"
        shutil.rmtree(self._trace_dir, ignore_errors=True)  # rr refuses an existing one
        chaos = ["--chaos"] if self._rr_mode == "chaos" else []
        return ["rr", "record", *chaos, "-o", str(self._trace_dir)]

    def _sigkill(self) -> None:
        """SIGKILL, the crash path. Recorded, the kill goes to the app alone: rr
        sees its tracee die, writes the trace's end and exits, and the trace of
        the killed process replays to the instant of the kill. Killing rr with it
        left a trace with no end, which `rr replay` refuses."""
        if self.proc is None:
            return
        if self.alive() and (getattr(self, "env", None) or {}).get("LLVM_PROFILE_FILE"):
            # A source-coverage build: its counters go with the process unless it
            # writes them first (the `code-coverage` query, util/CoverageProfile.h).
            with contextlib.suppress(Exception):
                self._socket_request({"query": "code-coverage", "args": {"flush": True}}, 10.0,
                                     "query 'code-coverage'")
        if self.recording and self.alive():
            app = self.pid
            if app is not None and app != self.proc.pid:
                with contextlib.suppress(ProcessLookupError):
                    os.kill(app, signal.SIGKILL)
                # rr leaves once every tracee has: the app's terminals hang up and
                # its servers read end-of-file when it dies. One that lingers is
                # ended by rr's own SIGTERM handling, which still closes the trace.
                with contextlib.suppress(subprocess.TimeoutExpired):
                    self.proc.wait(timeout=10 * SLOW_FACTOR)
                if self.proc.poll() is None:
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(self.proc.pid, signal.SIGTERM)
                    with contextlib.suppress(subprocess.TimeoutExpired):
                        self.proc.wait(timeout=10 * SLOW_FACTOR)
        with contextlib.suppress(ProcessLookupError):
            os.killpg(self.proc.pid, signal.SIGKILL)
        self.proc.wait(timeout=5)

    def save_run_logs(self, destination: Path) -> None:
        """The whole log of every launch of this run, compressed, into
        `destination`. A finding keeps the log's last 64 KB -- the last tenth of
        a 1500-step run -- and the work tree with the rest is deleted at stop();
        what set a late symptom up (a save 600 actions earlier) was not in it."""
        destination.mkdir(parents=True, exist_ok=True)
        for log in sorted(self.work.glob("microide*.log")):
            with contextlib.suppress(OSError):
                with open(log, "rb") as source, gzip.open(destination / f"{log.name}.gz", "wb", 6) as out:
                    shutil.copyfileobj(source, out)

    # ---- control channel -------------------------------------------------

    def _control(self, args: list[str], timeout: float = 10.0) -> subprocess.CompletedProcess:
        # errors="replace": a malformed byte in an answer is the app's bug to report,
        # not a reason for the run to die mid-scenario (the monkey did, seed 12).
        # MQA_SLOW_FACTOR stretches every control timeout for an instrumented
        # build (coverage, the sanitizers), whose control client alone can take
        # longer than a fast build's whole query.
        self._await_typed_input()
        return subprocess.run([str(self.binary), *args], env=self.tool_env, capture_output=True,
                              text=True, errors="replace", timeout=timeout * SLOW_FACTOR)

    def _control_ready(self) -> bool:
        result = self._control(["control-list"], timeout=3.0)
        return result.returncode == 0 and bool(result.stdout.strip())

    def send(self, *words: str) -> dict[str, Any]:
        """Runs a control command; returns the reply. Records it for replay."""
        self._record({"send": list(words)})
        result = self._control(["control-send", *words])
        return _last_json(result.stdout) or {"ok": False, "raw": result.stdout + result.stderr}

    def send_line(self, line: str, timeout: float = 15.0, record: bool = True) -> dict[str, Any]:
        """Sends one command line VERBATIM over the control socket (no argv
        re-quoting, so a hostile argument reaches the parser as written) and
        waits for its reply. Records it for replay unless `record` is False."""
        if record:
            self._record({"line": line})
        return self._socket_request({"command": line}, timeout, repr(line))

    def _socket_request(self, payload: dict[str, Any], timeout: float, what: str) -> dict[str, Any]:
        """One request (`command` or `query`) over the persistent control socket,
        after the app has taken the input sent before it; returns the reply."""
        self._await_typed_input()
        if self._socket is None:
            listing = _last_json(self._control(["control-list"]).stdout) or {}
            path = listing.get("socket")
            if not path:
                # A dead app has no socket, and neither has one the kernel is still
                # taking apart (its core piped to the crash reporter): that is a
                # crash, which check() reports with its status and stack. Filed as
                # "no control socket", it was a second finding per crash that said
                # nothing of it (soak e02c374fd3ee: the main thread a zombie).
                if self.dying():
                    raise AppGone("the app is gone: no control socket to connect to")
                raise QaFailure("no control socket to connect to")
            self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                self._socket.connect(path)
            except OSError as error:
                self._socket = None
                if self.dying():
                    raise AppGone(f"the app is gone: {error}") from error
                raise QaFailure(f"the control socket refused the connection: {error}") from error
        request_id = self._next_id
        self._next_id += 1
        self._socket.settimeout(timeout)
        # From here on the time is the app's: the wait for typed input is not.
        started = time.monotonic()
        sched_before = self._main_thread_sched()
        log_before = self._log_size()
        waits: collections.Counter[str] = collections.Counter()
        helpers_from: tuple[float, float] | None = None  # (their queued seconds, when) once late
        profiler: MainThreadProfiler | None = None
        deadline = started + timeout
        try:
            self._socket.sendall(json.dumps({"id": request_id, **payload}).encode() + b"\n")
            while True:
                newline = self._socket_buffer.find(b"\n")
                if newline < 0:
                    # Short receives once the reply is late, so the main thread's
                    # wait can be sampled between them; the deadline is the same.
                    late = time.monotonic() - started >= MAIN_THREAD_SAMPLE_AFTER_S
                    if late:
                        waits[self._main_thread_wait()] += 1
                        if helpers_from is None:
                            helpers_from = (self._helper_threads_queued(), time.monotonic())
                            profiler = MainThreadProfiler.start(self)
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise socket.timeout("timed out")
                    self._socket.settimeout(min(remaining, MAIN_THREAD_SAMPLE_S) if late
                                            else min(remaining, MAIN_THREAD_SAMPLE_AFTER_S))
                    try:
                        chunk = self._socket.recv(65536)
                    except socket.timeout:
                        continue
                    if not chunk:
                        if self.dying():
                            raise AppGone("the app is gone: it closed the control connection")
                        raise QaFailure("the app closed the control connection")
                    self._socket_buffer += chunk
                    continue
                raw, self._socket_buffer = (self._socket_buffer[:newline],
                                            self._socket_buffer[newline + 1:])
                # errors="replace": a malformed byte in an answer is the app's bug
                # to report (the ui query's chrome check), not a dead run.
                text = raw.decode("utf-8", "replace")
                with contextlib.suppress(json.JSONDecodeError):
                    reply = json.loads(text)
                    if isinstance(reply, dict) and reply.get("id") == request_id:
                        if "\ufffd" in text:
                            reply["_replaced"] = True
                        helpers = None
                        if helpers_from is not None:
                            helpers = (max(0.0, self._helper_threads_queued() - helpers_from[0]),
                                       time.monotonic() - helpers_from[1])
                        seconds = time.monotonic() - started
                        profile = profiler.finish() if profiler is not None else ""
                        profiler = None
                        self.note_latency(what, seconds, sched_before, log_before, waits, helpers, profile)
                        return reply
        except (OSError, socket.timeout) as error:
            if self.dying():
                if profiler is not None:
                    profiler.discard()
                    profiler = None
                with contextlib.suppress(OSError):
                    self._socket.close()
                self._socket = None
                self._socket_buffer = b""
                raise AppGone(f"the app is gone: {what} got no reply: {error}") from error
            if profiler is not None:
                profiler.discard()
                profiler = None
            self.capture_stall(f"{what} got no reply", sched_before, time.monotonic() - started)
            # The next request connects afresh. The app may have shed this
            # connection (it logs why), and keeping the dead socket made every
            # later step of the run fail the same way: 155 broken pipes in one
            # soak run, each a finding, with no sign of whether the channel
            # itself still worked.
            with contextlib.suppress(OSError):
                self._socket.close()
            self._socket = None
            self._socket_buffer = b""
            raise QaFailure(f"{what} got no reply: {error}") from error

    def dying(self) -> bool:
        """The app has exited, or is on its way out: its main thread is gone
        while the process still stands (the kernel writing its core to the crash
        reporter, threads being torn down). `alive()` says True for those
        seconds, and nothing answers."""
        if not self.alive():
            return True
        pid = self.pid
        try:
            stat = Path(f"/proc/{pid}/task/{pid}/stat").read_text()
        except OSError:
            # No such process. Recorded, `self.proc` is rr and the app its child:
            # when rr gives up (a system call it cannot record) it kills and
            # reaps the app and is itself still on its way out. The request in
            # flight then failed against a live `proc`, and was filed as "the
            # app closed the control connection" -- two of three recent
            # sightings of that finding were rr's own FATAL, which check()
            # counts as a lost recorder once it reads the log.
            if self.recording and self._app_pid is not None and pid == self._app_pid:
                return True
            return not self.alive()
        return stat[stat.rfind(")") + 2:stat.rfind(")") + 3] in ("Z", "X")

    def wait_gone(self, seconds: float) -> bool:
        """Waits up to `seconds` for a dying app to finish exiting (so its exit
        status can be read); True when it has."""
        deadline = time.monotonic() + seconds
        while self.alive() and time.monotonic() < deadline:
            time.sleep(0.05)
        return not self.alive()

    def _main_thread_sched(self) -> tuple[float, float] | None:
        """The app's main thread's (seconds on a CPU, seconds runnable and waiting
        for one) so far, from /proc's schedstat."""
        pid = self.pid if self.proc is not None else None
        if pid is None:
            return None
        try:
            ran, waited = Path(f"/proc/{pid}/task/{pid}/schedstat").read_text().split()[:2]
            return int(ran) / 1e9, int(waited) / 1e9
        except (OSError, ValueError):
            return None

    def _helper_threads_queued(self) -> float:
        """Seconds the app's threads other than the main one have spent runnable and
        waiting for a CPU, summed (/proc schedstat's second field)."""
        pid = self.pid if self.proc is not None else None
        if pid is None:
            return 0.0
        total = 0
        try:
            tasks = list(Path(f"/proc/{pid}/task").iterdir())
        except OSError:
            return 0.0
        for task in tasks:
            if task.name == str(pid):
                continue
            with contextlib.suppress(OSError, ValueError, IndexError):
                total += int((task / "schedstat").read_text().split()[1])
        return total / 1e9

    def _main_thread_wait(self) -> str:
        """Where the app's main thread is right now: its kernel wait channel and
        the syscall it is in (number, first argument), from /proc."""
        pid = self.pid if self.proc is not None else None
        if pid is None:
            return "?"
        task = Path(f"/proc/{pid}/task/{pid}")
        try:
            wchan = (task / "wchan").read_text().strip() or "-"
            syscall = (task / "syscall").read_text().split()
        except OSError:
            return "?"
        call = "running" if syscall[:1] == ["running"] else " ".join(syscall[:2])
        return f"wchan={wchan} syscall={call}"

    def _log_size(self) -> int:
        try:
            return self.log_path.stat().st_size if self.log_path is not None else 0
        except OSError:
            return 0

    def log_since(self, offset: int, max_bytes: int = 256 * 1024) -> str:
        """The app's log from byte `offset` on (at most `max_bytes` of it)."""
        if self.log_path is None:
            return ""
        try:
            with open(self.log_path, "rb") as log:
                log.seek(offset)
                return log.read(max_bytes).decode("utf-8", "replace")
        except OSError:
            return ""

    def stall_reports(self, text: str) -> str:
        """The stall watchdog's reports in `text`, their stacks resolved."""
        blocks = [match.group(0) for match in STALL_BLOCK.finditer(text)]
        return "\n".join(symbolize_backtrace(block, self.binary) if "the main thread is at:" in block
                         else block + "\n" for block in blocks[:3])

    def capture_stall(self, why: str, sched_before: tuple[float, float] | None = None,
                      waited: float = 0.0) -> None:
        """When the app stops answering the control channel, records where each of
        its threads is: name, state, kernel wait channel and current syscall
        (`/proc`, readable for our own child without ptrace, which yama's scope 1
        refuses to a sibling gdb). Once per app; the file sits beside the
        screenshots, so the finding carries it (TD-2026-11-07-433)."""
        if self._stall_captured or self.shots is None or self.proc is None or self.proc.poll() is not None:
            return
        self._stall_captured = True
        lines = [f"stall: {why}", f"pid {self.pid}", ""]
        # Whose time it was: a main thread that was runnable and not run is a
        # starved machine, one that ran is the app computing, one that did
        # neither was blocked -- on I/O, a lock, a child.
        sched_now = self._main_thread_sched()
        if sched_before is not None and sched_now is not None and waited > 0:
            lines[2:2] = [sched_account(waited, sched_now[0] - sched_before[0], sched_now[1] - sched_before[1]), ""]
        task_dir = Path(f"/proc/{self.pid}/task")
        try:
            tids = sorted(task_dir.iterdir(), key=lambda entry: int(entry.name))
        except OSError as error:
            tids = []
            lines.append(f"cannot list threads: {error}")
        for tid in tids:
            fields: list[str] = [tid.name]
            for name in ("comm", "wchan", "syscall"):
                with contextlib.suppress(OSError):
                    fields.append(f"{name}={(tid / name).read_text(errors='replace').strip()}")
            with contextlib.suppress(OSError):
                for line in (tid / "status").read_text().splitlines():
                    if line.startswith("State:"):
                        fields.append(line.split(None, 1)[1])
            lines.append("  ".join(fields))
        self.shots.mkdir(parents=True, exist_ok=True)
        # What the app's own watchdog printed while it did not answer: the main
        # thread's stack when the turn passed the threshold (the gdb attach below
        # comes seconds later, and on a loaded machine after the stall is over).
        reported = self.stall_reports(self.log_tail(256 * 1024))
        if reported:
            lines += ["", "the app's stall watchdog (MICROIDE_TRACE_STALL_MS), newest reports:", reported]
        (self.shots / "stall-threads.txt").write_text("\n".join(lines) + "\n")
        # Where each thread is in the code, not only that it sleeps: every thread of
        # soak d3f7c53a3cd7 was asleep on a futex, and the states alone cannot say
        # which wait. The app lets its parent's descendants trace it
        # (MICROIDE_QA_PARENT_MAY_TRACE); bounded, and skipped without gdb.
        # Not under rr: a traced process takes no second tracer, and the recording
        # holds every thread's stack at the stall anyway (`rr replay`, then `bt`).
        gdb = shutil.which("gdb") if not self.recording else None
        if gdb is not None:
            try:
                traced = subprocess.run(
                    [gdb, "-p", str(self.pid), "-batch", "-nx", "-ex", "set pagination off",
                     "-ex", "thread apply all bt 40"],
                    env=self.tool_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60, check=False)
                (self.shots / "stall-backtrace.txt").write_bytes(traced.stdout[-512 * 1024:])
            except (OSError, subprocess.TimeoutExpired) as error:
                (self.shots / "stall-backtrace.txt").write_text(f"gdb could not attach: {error}\n")

    def query(self, verb: str, args: dict[str, Any] | None = None) -> dict[str, Any]:
        """A query over the persistent control connection (a spawned control
        client per query was ~6 ms, and the oracles ask several per step)."""
        payload: dict[str, Any] = {"query": verb}
        if args is not None:
            payload["args"] = args
        reply = self._socket_request(payload, 10.0, f"query {verb!r}")
        if not reply.get("ok"):
            raise QaFailure(f"query {verb!r} failed: {json.dumps(reply)[:600]}")
        result = reply.get("result", {})
        if verb == "ui" and reply.get("_replaced"):
            self._check_chrome_utf8(result)
        return result

    def note_latency(self, what: str, seconds: float, sched_before: tuple[float, float] | None = None,
                     log_before: int | None = None, waits: collections.Counter[str] | None = None,
                     helpers: tuple[float, float] | None = None, profile: str = "") -> None:
        """The liveness oracle's input: the slowest control answer per step
        (the monkey reads and resets `slowest_answer`), with where its time went
        for the main thread and what the stall watchdog printed during it."""
        if seconds > self.slowest_answer[1]:
            self.slowest_answer = (what, seconds)
            sched_now = self._main_thread_sched() if sched_before is not None else None
            self.slowest_sched = ((sched_now[0] - sched_before[0], sched_now[1] - sched_before[1])
                                  if sched_now is not None and sched_before is not None else None)
            self.slowest_stall = ""
            self.slowest_helpers = helpers
            self.slowest_profile = profile
            self.slowest_waits = "\n".join(f"{count:4d} x {where}" for where, count in waits.most_common(8)) \
                if waits else ""
            if seconds >= STALL_TRACE_SECONDS * SLOW_FACTOR and log_before is not None:
                text = self.log_since(log_before)
                # The app's own account of a wait that slept through a wake
                # (DescribeLateWake): whether the lost-wake relay nudged.
                late = [line for line in text.splitlines() if "microide idle-wait:" in line][:4]
                self.slowest_stall = self.stall_reports(text) + "".join(line + "\n" for line in late)

    # Element kinds whose labels are chrome the app composes itself (menus, the
    # status bar, buttons, settings), not text copied from a file or typed into a
    # field -- the channel is byte-transparent, and a binary file opened as text
    # legitimately puts raw bytes into a find query or a tab title.
    _CHROME_KINDS = ("menu", "menu-item", "status", "button", "setting", "settings-category",
                     "debug-tab", "panel-tab", "sidebar-view")

    def _check_chrome_utf8(self, ui: dict[str, Any]) -> None:
        if self._reported_bad_utf8:
            return
        for element in ui.get("elements", []):
            if element.get("kind") in self._CHROME_KINDS and "\ufffd" in element.get("label", ""):
                self._reported_bad_utf8 = True
                self.expect(False, f"{element['name']} draws a label cut through a character: "
                                   f"{element.get('label')!r}")
                return

    def output(self, channel: str | None = None) -> list[str]:
        """The lines of an output channel (references, call hierarchy, workspace
        symbols, the debug console, logs) -- by default the one the Output tab shows."""
        return self.query("output", {"channel": channel} if channel else None).get("text", [])

    def ui(self) -> dict[str, Any]:
        """Named on-screen elements and focus/overlay/menu state (the `ui` query)."""
        answer = self.query("ui")
        # Rects and geometry are in layout units; `window.scale` is window pixels
        # per unit (ui.scale, HiDPI). Every point a flow takes from an answer goes
        # through _root, which applies the latest one.
        scale = (answer.get("window") or {}).get("scale")
        if isinstance(scale, list) and len(scale) == 2:
            self.window_scale = (float(scale[0]), float(scale[1]))
        return answer

    def element(self, name: str) -> dict[str, Any]:
        """The element called `name` in the ui query (e.g. "menu:Edit", "tab:0/main.cpp")."""
        for element in self.ui().get("elements", []):
            if element.get("name") == name:
                return element
        raise QaFailure(f"no element named {name!r} on screen")

    def elements(self, kind: str | None = None, prefix: str = "") -> list[dict[str, Any]]:
        """The on-screen elements, optionally of one kind and/or with a name prefix."""
        return [element for element in self.ui().get("elements", [])
                if (kind is None or element.get("kind") == kind)
                and element.get("name", "").startswith(prefix)]

    def buffer(self) -> dict[str, Any]:
        """The buffer the caret is in (the `buffer` query): text, every caret with
        its selection (primary first, 1-based), folds, flags."""
        return self.query("buffer")

    def text(self) -> str:
        return self.buffer().get("text", "")

    def editor_state(self) -> dict[str, Any]:
        """Every editor group and tab (the `editor` query)."""
        return self.query("editor")

    def active_tab(self) -> dict[str, Any] | None:
        state = self.editor_state()
        for group in state.get("groups", []):
            if group.get("focused"):
                for tab in group.get("tabs", []):
                    if tab.get("active"):
                        return tab
        return None

    def field_text(self, name: str) -> str:
        """A text field's contents (`field:` elements carry their text as label)."""
        return self.element(name).get("label", "")

    def pane(self, group: int | None = None, ui: dict[str, Any] | None = None) -> dict[str, Any]:
        """One editor pane's text geometry from `ui.editors` (the focused one by
        default)."""
        editors = (ui or self.ui()).get("editors", [])
        for pane in editors:
            if (group is None and pane.get("focused")) or pane.get("group") == group:
                return pane
        if group is None and editors:
            return editors[0]
        raise QaFailure(f"no editor pane {group if group is not None else '(focused)'} on screen")

    def text_point(self, line: int, column: int, group: int | None = None) -> tuple[float, float]:
        """The window point a click at 1-based (line, column) resolves to: the
        editor maps x to a column with lround(offset / charWidth), so aim a fifth
        of a cell past the column's left edge."""
        pane = self.pane(group)
        for row in pane["rows"]:
            if row["line"] == line:
                x = pane["textX"] + (column - 1 - pane["scrollColumn"] + 0.2) * pane["charWidth"]
                return x, row["y"] + pane["lineHeight"] / 2
        raise QaFailure(f"line {line} is not on screen in pane {pane.get('group')}")

    def terminal_point(self, row: int, column: int) -> tuple[float, float]:
        """The window point of the active terminal pane's cell at 0-based transcript
        `row` (the `terminal-output` line) and `column`, from `ui.terminal`."""
        geometry = self.ui().get("terminal")
        if not geometry:
            raise QaFailure("no terminal pane on screen")
        visible = row - geometry["firstRow"]
        if not 0 <= visible < geometry["visibleRows"]:
            raise QaFailure(f"terminal row {row} is not on screen (first {geometry['firstRow']})")
        return (geometry["textX"] + (column + 0.3) * geometry["charWidth"],
                geometry["firstLineY"] + (visible + 0.5) * geometry["lineHeight"])

    def click_text(self, line: int, column: int, clicks: int = 1,
                   modifiers: tuple[str, ...] = (), group: int | None = None,
                   button: int = 1) -> None:
        self.click(*self.text_point(line, column, group), clicks=clicks, modifiers=modifiers,
                   button=button)

    def drag_text(self, start: tuple[int, int], end: tuple[int, int],
                  modifiers: tuple[str, ...] = (), group: int | None = None) -> None:
        x0, y0 = self.text_point(*start, group=group)
        x1, y1 = self.text_point(*end, group=group)
        self.drag(x0, y0, x1, y1, modifiers=modifiers)

    def gutter_point(self, line: int, part: str = "fold", group: int | None = None) -> tuple[float, float]:
        """The fold marker (`part="fold"`) or breakpoint zone (`"breakpoint"`) of a
        visible line's gutter."""
        pane = self.pane(group)
        for row in pane["rows"]:
            if row["line"] == line:
                y = row["y"] + pane["lineHeight"] / 2
                if part == "fold":
                    return (pane["foldHitX"] + pane["gutterX"] + pane["gutterWidth"]) / 2, y
                return pane["gutterX"] + 6, y
        raise QaFailure(f"line {line} is not on screen in pane {pane.get('group')}")

    def cursors(self) -> list[tuple[int, int]]:
        """Every caret as (line, column), primary first."""
        return [(c["line"], c["column"]) for c in self.buffer().get("cursors", [])]

    def wait_for(self, name: str, timeout: float = 5.0) -> dict[str, Any] | None:
        """Waits for the element called `name` to be on screen. A git view element
        is read once that view has settled (wait_git_settled), so its rect is the
        one a click made next will find."""
        if name.startswith(GIT_VIEW_ELEMENTS):
            self.wait_git_settled(timeout)
        found: dict[str, Any] = {}

        def present() -> bool:
            try:
                found.update(self.element(name))
                return True
            except QaFailure:
                return False
        return found if self.wait_until(present, timeout, f"{name} on screen") else None

    def wait_text(self, predicate, timeout: float = 5.0, message: str = "the buffer") -> bool:
        return self.wait_until(lambda: predicate(self.text()), timeout, message)

    def keybindings(self) -> list[dict[str, Any]]:
        """The resolved keybindings (`keybindings` query), dispatch order."""
        result = self._control(["control-send", "--query", "keybindings"])
        reply = _last_json(result.stdout) or {}
        return reply.get("result", []) if reply.get("ok") else []

    def wait_until(self, predicate, timeout: float = 5.0, message: str = "condition") -> bool:
        """Polls `predicate` (a callable over nothing) until true; records an
        expectation finding and returns False on timeout -- never sleeps a fixed
        time for a state the app reports."""
        deadline = time.monotonic() + timeout * SLOW_FACTOR
        while time.monotonic() < deadline:
            try:
                if predicate():
                    return True
            except QaFailure:
                pass
            time.sleep(0.05)
        self.expect(False, f"timed out waiting for {message}")
        return False

    # ---- input -----------------------------------------------------------

    @property
    def tool_env(self) -> dict[str, str]:
        """The environment for the driver's own tools (xdotool, xwd, xdpyinfo, the
        control client): the app's -- DISPLAY, the XDG tree -- with the driver's
        PATH. A scenario's env may put a stand-in program first on the app's PATH
        or drop PATH to nothing (`native-dialogs`); the tools must not notice."""
        return {**self.env, "PATH": os.environ.get("PATH", "/usr/bin:/bin")}

    def _xdotool(self, *args: str) -> None:
        subprocess.run(["timeout", "5", "xdotool", *args], env=self.tool_env, capture_output=True)

    # X input: in-process XTEST (tools/qa/xtest.py) when libXtst is there, else
    # xdotool. The events are the same; a spawned `timeout 5 xdotool` cost ~130 ms.

    def _xt_or_none(self) -> "xtest.XTest | None":
        xt = getattr(self, "_xt", None)
        return xt if xt is not None and xt.available() else None

    def _x_move(self, x: float, y: float) -> None:
        rx, ry = self._root(x, y)
        if xt := self._xt_or_none():
            xt.move(int(rx), int(ry))
        else:
            self._xdotool("mousemove", rx, ry)

    def _x_click(self, button: int, repeat: int = 1) -> None:
        if xt := self._xt_or_none():
            xt.click(button, repeat)
        else:
            self._xdotool("click", "--repeat", str(repeat), "--delay", "60", str(button))

    def _x_button(self, button: int, down: bool) -> None:
        if xt := self._xt_or_none():
            xt.button(button, down)
        else:
            self._xdotool("mousedown" if down else "mouseup", str(button))

    def _x_modifier(self, name: str, down: bool) -> None:
        if (xt := self._xt_or_none()) and xt.modifier(name, down):
            return
        self._xdotool("keydown" if down else "keyup", name)

    def _key_gap(self) -> None:
        """Keeps two key presses at least xtest.KEY_GAP apart, across calls: SDL
        reads a release and a press of one key closer than 2 ms as auto-repeat
        (a burst of one key, or "\n\n" typed as two Returns)."""
        last = getattr(self, "_last_key_at", 0.0)
        wait = xtest.KEY_GAP - (time.monotonic() - last)
        if wait > 0:
            time.sleep(wait)

    def _x_keys(self, *chords: str) -> None:
        xt = self._xt_or_none()
        for chord in chords:
            self._key_gap()
            if not (xt and xt.chord(chord)):
                self._xdotool("key", "--clearmodifiers", chord)
            self._last_key_at = time.monotonic()

    def _x_type_ascii(self, text: str) -> None:
        xt = self._xt_or_none()
        self._key_gap()
        missed = xt.type_ascii(text) if xt else text
        self._last_key_at = time.monotonic()
        if missed:
            self._xdotool("type", "--clearmodifiers", "--delay", "25", "--", missed)

    def _find_window(self) -> str | None:
        if self.wayland:
            return None
        # By the app's pid (its _NET_WM_PID), else its WM_CLASS -- never by title:
        # the title names the active file ("● main.cpp — taskflow — microide"), and
        # xdotool, in the C locale, cannot even match a UTF-8 one.
        searches = [["--class", "microide"]]
        if self.proc is not None:
            searches.insert(0, ["--pid", str(self.pid)])
        deadline = time.monotonic() + 20.0
        while True:
            for search in searches:
                result = subprocess.run(["xdotool", "search", *search], env=self.tool_env,
                                        capture_output=True, text=True)
                ids = result.stdout.split()
                if ids:
                    return ids[-1]
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.2)

    def _window_origin(self) -> tuple[int, int]:
        if self.wayland or not self.window:
            return (0, 0)
        result = subprocess.run(["xdotool", "getwindowgeometry", "--shell", self.window],
                                env=self.tool_env, capture_output=True, text=True)
        fields = dict(line.split("=", 1) for line in result.stdout.split() if "=" in line)
        try:
            return (int(fields.get("X", 0)), int(fields.get("Y", 0)))
        except ValueError:
            return (0, 0)

    def _root(self, x: float, y: float) -> tuple[str, str]:
        sx, sy = getattr(self, "window_scale", (1.0, 1.0))
        return str(round(x * sx) + self.origin[0]), str(round(y * sy) + self.origin[1])

    def _focus(self) -> None:
        """Gives the window the X input focus and waits until it has it. Keys then
        go through XTEST, as a keyboard's do. Passing `--window` to xdotool instead
        made libxdo fall back to XSendEvent whenever the focus was not (yet) on the
        window -- synthetic core events SDL3, reading the keyboard through XInput2,
        never sees: a loaded run once lost every key of a flow with no trace in the
        app's input log (TD-2026-10-20-368)."""
        if not self.window:
            return
        if (xt := self._xt_or_none()) and xt.focus(int(self.window)):
            return
        self._xdotool("windowfocus", "--sync", self.window)

    def _input(self, *words: str) -> dict[str, Any]:
        """One input command (`key`, `pointer`, `type`) over the persistent control
        connection: a process per keystroke made typing slow enough on the Wayland
        backend for a toast to expire between two words of a palette query.

        The app's answer, which says whether it took the input. A request it never
        answered -- a stalled main thread, a dead process -- raises, as any other
        request does, rather than reading as an input it refused: every sighting
        of "pointer drop ... was refused" (soak 8a6169d5fde8) was an app that had
        stopped answering, filed under the wrong oracle with the wrong cause."""
        return self.send_line(" ".join(quote_arg(w) for w in words), record=False)

    def _pointer(self, verb: str, x: float, y: float, *rest: Any, modifiers: tuple[str, ...] = ()) -> None:
        """The Wayland backend's mouse: the control channel's `pointer`, in the
        layout units every rect here is already in."""
        prefix = "".join(f"{chord_for_key_command(m)}+" for m in modifiers)
        answer = self._input("pointer", f"{prefix}{verb}", f"{x:.1f}", f"{y:.1f}", *(str(r) for r in rest))
        if not answer.get("ok", False):
            self.expect(False, f"pointer {verb} {x:.0f} {y:.0f} was refused: {answer}")

    def move(self, x: float, y: float) -> None:
        self._record({"move": [round(x), round(y)]})
        if self.wayland:
            self._pointer("move", x, y)
            return
        self._expect_pointer_input(motion=(x, y), motion_lands=self._hit_region(x, y) != "outside")
        self._x_move(x, y)

    def click(self, x: float, y: float, button: int = 1, clicks: int = 1,
              modifiers: tuple[str, ...] = ()) -> None:
        """A press at window (x, y); `modifiers` are xdotool key names held for it
        ("alt", "shift", "ctrl")."""
        if self.press_guard is not None and self.press_guard(x, y):
            return
        self._await_click_gap(x, y)
        self._record({"click": [round(x), round(y)], "button": button, "clicks": clicks,
                      "modifiers": list(modifiers)})
        if self.wayland:
            self._pointer("click", x, y, button, clicks, modifiers=modifiers)
            self._last_click = (time.monotonic(), x, y)
            return
        reaches_app = self._hit_region(x, y) == "normal"
        for key in modifiers:
            self._x_modifier(key, True)
        self._expect_pointer_input(buttons=2 * clicks if reaches_app else 0, at=(x, y))
        self._x_move(x, y)
        self._x_click(button, clicks)
        for key in reversed(modifiers):
            self._x_modifier(key, False)
        self._last_click = (time.monotonic(), x, y)

    def _await_click_gap(self, x: float, y: float) -> None:
        """SDL folds two presses within its double-click time and radius into a
        double click (a word selection in the editor), so a separate press right
        after another near it waits that window out first. Near the last PRESS:
        SDL measures from where the button went down, not where it came up, so a
        drag records its start -- recording its end let a double-click drag that
        began where the previous one had become its third click, a line selection
        (QA word-and-line-drags, on CI).

        And from when the app TOOK that press, not when it was sent: SDL stamps a
        press as it takes it, which on a loaded machine is long after. Measured from
        the send, a triple-click taken late and a drag pressed 613 ms after it was
        sent became its fourth click, a line selection (QA edit-selection-clipboard
        at load 20). So the presses still on their way are awaited first, and
        _await_typed_input restarts the window when it sees them taken."""
        if self._buttons_pending:
            self._await_typed_input()
        last_time, last_x, last_y = self._last_click
        if time.monotonic() - last_time < DOUBLE_CLICK_SECONDS and \
                abs(x - last_x) < DOUBLE_CLICK_RADIUS and abs(y - last_y) < DOUBLE_CLICK_RADIUS:
            time.sleep(DOUBLE_CLICK_SECONDS - (time.monotonic() - last_time))

    def drag(self, x0: float, y0: float, x1: float, y1: float, steps: int = 8,
             modifiers: tuple[str, ...] = (), before_release: Callable[[], object] | None = None,
             clicks: int = 1) -> None:
        """A press at (x0, y0), `steps` moves to (x1, y1), a release there.
        `before_release` runs with the button still held at (x1, y1) -- a drag held
        past an edge waiting for its autoscroll, say -- on either backend. With
        `clicks` > 1 the held press is the last of that many (a double-click drag
        extends by words, a triple-click drag by lines)."""
        if self.press_guard is not None and self.press_guard(x0, y0):
            return
        self._await_click_gap(x0, y0)
        self._record({"drag": [round(x0), round(y0), round(x1), round(y1)],
                      "modifiers": list(modifiers), "clicks": clicks})
        if self.wayland:
            if clicks > 1:
                self._pointer("click", x0, y0, 1, clicks - 1, modifiers=modifiers)
            self._pointer("down", x0, y0, 1, clicks, modifiers=modifiers)
            for i in range(1, steps + 1):
                self._pointer("move", x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, "left",
                              modifiers=modifiers)
                time.sleep(0.02)
            if before_release is not None:
                before_release()
            self._pointer("up", x1, y1, 1, modifiers=modifiers)
            self._last_click = (time.monotonic(), x0, y0)
            return
        # A press the window manager takes (a drag on the title bar, a resize edge)
        # reaches the app with none of the drag.
        reaches_app = self._hit_region(x0, y0) == "normal"
        end_lands = reaches_app and self._hit_region(x1, y1) != "outside"
        for key in modifiers:
            self._x_modifier(key, True)
        # Every press and release but the last, then the move onto (x1, y1), then
        # the release: `before_release` may query the app with the button held.
        self._expect_pointer_input(buttons=2 * clicks - 1 if reaches_app else 0, at=(x0, y0))
        self._x_move(x0, y0)
        if clicks > 1:
            self._x_click(1, clicks - 1)
            time.sleep(0.03)
        self._x_button(1, True)
        for i in range(1, steps + 1):
            x = x0 + (x1 - x0) * i / steps
            y = y0 + (y1 - y0) * i / steps
            if i == steps:
                self._expect_pointer_input(motion=(x, y), motion_lands=end_lands)
            self._x_move(x, y)
            time.sleep(0.02)
        if before_release is not None:
            before_release()
        self._expect_pointer_input(buttons=1 if reaches_app else 0)
        self._x_button(1, False)
        for key in reversed(modifiers):
            self._x_modifier(key, False)
        self._last_click = (time.monotonic(), x0, y0)

    def drop(self, x: float, y: float, path: Path | str) -> None:
        """A file dragged in from another application and released at (x, y). Through
        the control channel on every backend: X cannot synthesize a drop either."""
        row: dict[str, Any] = {"drop": [round(x), round(y)], "path": str(path)}
        if self.replay_root is not None:
            # Recorded as an absolute path only, a replay dropped a file of the
            # run's own fixture, long gone: every drop in a replay was a drop of
            # nothing (a remote run's mirror path came from a deleted work dir).
            with contextlib.suppress(ValueError):
                row["projectPath"] = Path(path).relative_to(self.replay_root).as_posix()
        self._record(row)
        self._pointer("drop", x, y, str(path))

    def wheel(self, x: float, y: float, notches: int, modifiers: tuple[str, ...] = ()) -> None:
        """`notches` > 0 scrolls down; `modifiers` (xdotool key names) are held for it."""
        self._record({"wheel": [round(x), round(y)], "notches": notches, "modifiers": list(modifiers)})
        if self.wayland:
            self._pointer("wheel", x, y, notches, modifiers=modifiers)
            return
        button = "5" if notches > 0 else "4"
        lands = self._hit_region(x, y) != "outside"
        self._expect_pointer_input(wheels=abs(notches) if lands else 0, at=(x, y))
        self._x_move(x, y)
        for key in modifiers:
            self._x_modifier(key, True)
        self._x_click(int(button), abs(notches))
        for key in reversed(modifiers):
            self._x_modifier(key, False)

    def _guard_activation(self, stepping: bool = False) -> None:
        if self.activation_guard is not None and self.activation_guard(stepping):
            self.key("Escape")

    def key(self, chord: str) -> None:
        """An xdotool chord: "ctrl+shift+f", "Escape", "alt+Up"; a two-step chord
        is its steps separated by a space: "ctrl+k ctrl+o"."""
        steps = chord.split()
        if any(step.rsplit("+", 1)[-1] in _ACTIVATING_KEYS for step in steps):
            self._guard_activation()
        elif any(step in _STEPPING_KEYS for step in steps):
            self._guard_activation(stepping=True)
        self._record({"key": chord})
        if self.wayland:
            for step in steps:
                answer = self._input("key", chord_for_key_command(step))
                if not answer.get("ok", False):
                    self.expect(False, f"key {step} (of {chord}) was refused: {answer}")
                    return
            return
        self._focus()
        self._expect_typed_input("", len(steps), chord)
        self._x_keys(*steps)

    def hold(self, modifier: str, chords: list[str]) -> None:
        """Presses `chords` while `modifier` (an xdotool key name: "ctrl") stays
        down, then lets it go: Ctrl+Tab, Tab, release. `key` releases the chord's
        modifiers with it, which is a tap. X11 only -- the control channel's `key`
        has no held modifier, so on Wayland this fails the flow rather than
        pretending to have held one."""
        self._record({"hold": modifier, "keys": chords})
        if self.wayland:
            self.expect(False, f"hold {modifier} has no Wayland backend")
            return
        self._focus()
        self._expect_typed_input("", len(chords), " ".join(chords))
        self._x_modifier(modifier, True)
        for chord in chords:
            if not ((xt := self._xt_or_none()) and xt.chord(chord)):
                self._xdotool("key", chord)
        self._x_modifier(modifier, False)

    def resize_layout(self, width: int, height: int) -> None:
        """Resizes the window to hold `width` x `height` layout units at the run's
        ui.scale (MQA_UI_SCALE; device_size)."""
        self.resize(*device_size(width, height))

    def resize(self, width: int, height: int) -> None:
        """Resizes the window to `width` x `height` X pixels (X11; the Wayland
        kiosk owns the size). The size the app settles on is read back from the
        `ui` query, not assumed: the driver resizes within the window's declared
        minimum, as a window manager would."""
        self._record({"resize": [int(width), int(height)]})
        self._resize_window(width, height)

    def _resize_window(self, width: int, height: int) -> None:
        """resize(), unrecorded: the startup sizing is not an action to replay."""
        if self.wayland or not self.window:
            return
        if (xt := self._xt_or_none()) and xt.resize(int(self.window), int(width), int(height)):
            return
        self._xdotool("windowsize", self.window, str(int(width)), str(int(height)))

    def foreign_clipboard(self, text: str) -> None:
        """Puts `text` on the X CLIPBOARD as another application would: a helper
        (tools/qa/x_clipboard.py) owns the selection and answers the app's paste
        until something else takes it. X only -- Wayland's clipboard has no
        stand-in here, so the row is recorded and nothing is sent."""
        self._record({"foreign-clipboard": text})
        self._own_clipboard(text, [])

    def foreign_clipboard_image(self, kind: str) -> None:
        """Puts an image and no text on the X CLIPBOARD, as a screenshot tool
        does: the bytes of x_clipboard.image_bytes(kind), offered as image/png.
        A terminal's paste stores such an image and types its path. X only, as
        foreign_clipboard."""
        self._record({"foreign-clipboard-image": kind})
        self._own_clipboard("", ["--image", kind])

    def _own_clipboard(self, text: str, arguments: list[str]) -> None:
        if self.wayland:
            return
        self._end_clipboard_owner()
        owner = subprocess.Popen([sys.executable, str(Path(__file__).resolve().parent / "x_clipboard.py"), *arguments],
                                 env=self.tool_env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 text=True, preexec_fn=self._dies_with_driver())
        assert owner.stdin is not None and owner.stdout is not None
        owner.stdin.write(text)
        owner.stdin.close()
        ready = owner.stdout.readline().strip()
        if ready != "ready":
            owner.kill()
            owner.wait()
            self.expect(False, f"the foreign clipboard owner did not start: {ready!r}")
            return
        self._clipboard_owner = owner

    def _end_clipboard_owner(self) -> None:
        owner = getattr(self, "_clipboard_owner", None)
        if owner is not None:
            if owner.poll() is None:
                owner.kill()
            owner.wait()
            self._clipboard_owner = None

    def focus_out(self) -> None:
        """Takes the X input focus away from the window (another application
        took it); the next key gives it back (_focus)."""
        self._record({"focus-out": True})
        if (xt := self._xt_or_none()) is not None:
            xt.focus_root()

    def current_size(self) -> tuple[int, int]:
        """The window's size as the last `ui` answer has it (the monkey resizes)."""
        window = (self.last_ui or {}).get("window") or {}
        if window.get("width") and window.get("height"):
            return int(window["width"]), int(window["height"])
        return self.size

    def _expect_typed_input(self, chars: str, presses: int, keys: str = "") -> None:
        """Notes input about to go through X -- `chars` of text and `presses`
        non-modifier key presses -- for the next _await_typed_input to wait on.
        Called just before each xdotool call, not once per `type`, so a control
        command issued part-way through typing waits only for what was sent."""
        if self.wayland or self.log_path is None:
            return
        if not self._input_pending():
            with contextlib.suppress(OSError):
                self._typed_log_offset = self.log_path.stat().st_size
            self._keys_sent = []
        self._typed_pending += "".join(c for c in chars if ord(c) <= 0xFF and c.isprintable())
        self._keys_pending += presses
        if keys:
            self._keys_sent.append(keys)

    def _hit_region(self, x: float, y: float) -> str:
        """Where a press at window (x, y) lands, as the app's own hit-test says
        (the `hit-test` query): "normal", "draggable" or "resize" (the window
        manager takes those presses; SDL never delivers them), or "outside" the
        window as it is sized now -- which the monkey changes (maximize/restore).
        Asked after the input sent before it is taken, since that input can move
        the window's edges. Expecting a press the app never gets cost a 3 s wait
        (9 s on the ASan lane) each time."""
        try:
            reply = self._socket_request({"query": "hit-test", "args": {"x": x, "y": y}},
                                         10.0, "hit-test")
        except QaFailure:
            return "normal"
        return str((reply.get("result") or {}).get("region", "normal")) if reply.get("ok") else "normal"

    def _expect_pointer_input(self, buttons: int = 0, wheels: int = 0,
                              motion: tuple[float, float] | None = None,
                              at: tuple[float, float] | None = None,
                              motion_lands: bool = True) -> None:
        """Notes pointer input about to go through X, as _expect_typed_input does
        keys: `buttons` press and release events, `wheels` notches, and a `motion`
        that must be seen to land. A click answered before the app handled it is
        the same race as a key: Discard All clicked, then the prompt read at once,
        found none on the slow ASan build (QA journey-rename-refactor). A move
        waits only when the pointer is not already there (X sends no motion
        then) and `motion_lands` (the point is in the window: _hit_region). `at`
        is where the input leaves the pointer, for the next move to compare with."""
        if self.wayland or self.log_path is None:
            return
        self._expect_typed_input("", 0)
        self._buttons_pending += buttons
        self._wheels_pending += wheels
        if motion is not None:
            target = (round(motion[0]), round(motion[1]))
            if self._pointer_at is None:
                self._pointer_at = self._x_pointer_location()
            if target != self._pointer_at and motion_lands:
                self._motion_pending = target
            at = motion
        if at is not None:
            self._pointer_at = (round(at[0]), round(at[1]))

    def _x_pointer_location(self) -> tuple[int, int] | None:
        """Where X has the pointer now, in window coordinates."""
        if (xt := self._xt_or_none()) and (at := xt.pointer()) is not None:
            values = {"X": str(at[0]), "Y": str(at[1])}
        else:
            result = subprocess.run(["timeout", "5", "xdotool", "getmouselocation", "--shell"],
                                    env=self.tool_env, capture_output=True, text=True)
            values = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        try:
            sx, sy = getattr(self, "window_scale", (1.0, 1.0))
            return (round((int(values["X"]) - self.origin[0]) / sx),
                    round((int(values["Y"]) - self.origin[1]) / sy))
        except (KeyError, ValueError):
            return None

    def _await_typed_input(self, timeout: float = 3.0) -> None:
        """Waits until the app has taken every key press and character sent through
        X since the last wait: as many non-modifier `microide input: key=` lines as
        presses, and the `text=` lines' characters in order (MICROIDE_TRACE_INPUT),
        after the log offset the input started at. Keystrokes travel through the X
        server and a control command through the socket, and nothing orders the two:
        `type(" more")` then `send("open", other)` once put the last `e` into the
        OTHER file, and `key("Return")`, `type("x")`, `send("goto", ...)` put the x
        on the line the goto named (QA doc-comment-enter) -- races no person at a
        keyboard can produce. Every control command waits here, queries included.
        Bounded: a timeout is recorded and printed, since what follows may race."""
        if self.wayland or self.log_path is None or not self._input_pending():
            return
        deadline = time.monotonic() + timeout * SLOW_FACTOR
        text_pattern = re.compile(rb'microide input: text="(.*?)" focus=')
        key_pattern = re.compile(rb'microide input: key=(.*?) mod=0x[0-9a-f]+ repeat=0 ')
        modifier = re.compile(rb'(Left|Right) (Ctrl|Shift|Alt|GUI)')
        button_pattern = re.compile(rb'microide input: button=')
        wheel_pattern = re.compile(rb'microide input: wheel=')
        motion_pattern = re.compile(rb'microide input: motion x=(-?\d+) y=(-?\d+)')
        taken_presses = 0
        taken_buttons = 0
        taken_wheels = 0
        moved = self._motion_pending is None
        taken = ""
        while time.monotonic() < deadline:
            with contextlib.suppress(OSError):
                with open(self.log_path, "rb") as log:
                    log.seek(self._typed_log_offset)
                    tail = log.read()
                taken = b"".join(text_pattern.findall(tail)).decode("utf-8", "replace")
                taken_presses = sum(1 for name in key_pattern.findall(tail) if not modifier.fullmatch(name))
                taken_buttons = len(button_pattern.findall(tail))
                taken_wheels = len(wheel_pattern.findall(tail))
                if not moved:
                    mx, my = self._motion_pending
                    moved = any(abs(int(x) - mx) <= 1 and abs(int(y) - my) <= 1
                                for x, y in motion_pattern.findall(tail))
                want = iter(taken)
                if (taken_presses >= self._keys_pending
                        and taken_buttons >= self._buttons_pending
                        and taken_wheels >= self._wheels_pending and moved
                        and all(char in want for char in self._typed_pending)):  # an in-order subsequence
                    break
            time.sleep(0.01)
        else:
            note = (f"typed input not seen taken after {timeout * SLOW_FACTOR:.0f}s: "
                    f"{taken_presses} of {self._keys_pending} key presses ({' '.join(self._keys_sent)}), "
                    f"text {taken!r} of {self._typed_pending!r}, "
                    f"{taken_buttons} of {self._buttons_pending} button events (pointer at {self._pointer_at}), "
                    f"{taken_wheels} of {self._wheels_pending} wheel notches"
                    + ("" if moved else f", no motion to {self._motion_pending}"))
            self._record({"note": note})
            print(f"        note: {note}", flush=True)
        if self._buttons_pending:
            # The last press is stamped by the app as late as now: the double-click
            # window runs from here (_await_click_gap).
            self._last_click = (time.monotonic(), self._last_click[1], self._last_click[2])
        self._typed_pending = ""
        self._keys_pending = 0
        self._buttons_pending = 0
        self._wheels_pending = 0
        self._motion_pending = None

    def _input_pending(self) -> bool:
        return bool(self._typed_pending or self._keys_pending or self._buttons_pending
                    or self._wheels_pending or self._motion_pending is not None)

    def type(self, text: str) -> None:
        """Types `text`. A newline is a Return press and a tab a Tab press: xdotool
        types '\n' as the Linefeed keysym, which no keyboard sends and SDL drops,
        so a typed command line never ran."""
        if "\n" in text or " " in text:
            self._guard_activation()
        self._record({"type": text})
        if self.wayland:
            # Key by key, as typed: `key <char>` is a key press with its text, so
            # what reacts to keys (auto-closing pairs, a terminal's keyboard
            # protocol) sees typing, not a paste. Past ASCII, the paste path.
            for char in text:
                if char == "\n":
                    self._input("key", "enter")
                elif char == "\t":
                    self._input("key", "tab")
                elif char == " ":
                    self._input("key", "space")
                elif char.isascii() and char.isprintable():
                    self._input("key", chord_for_key_command(char))
                else:
                    self._record({"type-via-control": char})
                    self._input("type", char)
            return
        self._focus()
        # Beyond ASCII, xdotool remaps a scratch keycode per character and restores
        # it right after the press, and SDL races that: a slow app looked the key up
        # under the restored map and typed nothing (unicode-editing lost the `é` on
        # the ASan lane, behind 80 ms sleeps that were a guess). Latin-1 is mapped
        # here instead, and stays mapped (_map_latin1). Past Latin-1 there is no way in: Xvfb has no
        # input method, and without one X delivers no text for such a keysym (a real
        # desktop's IME does) -- those characters are inserted through the control
        # channel's `type` (the paste path), and the action log says so.
        chunk = ""

        def flush() -> None:
            nonlocal chunk
            if chunk:
                self._expect_typed_input(chunk, len(chunk))
                self._x_type_ascii(chunk)
                chunk = ""
        for char in text:
            if char in "\n\t":
                flush()
                self._expect_typed_input("", 1)
                self._x_keys("Return" if char == "\n" else "Tab")
            elif ord(char) < 0x80:
                chunk += char
            elif ord(char) <= 0xFF:
                flush()
                self._map_latin1(char)
                self._expect_typed_input(char, 1)
                self._x_keys("0x%x" % ord(char))
            else:
                flush()
                self._record({"type-via-control": char})
                self._input("type", char)
        flush()

    def _map_latin1(self, char: str) -> None:
        """Binds Latin-1 `char`'s keysym to one of the display's free keycodes with
        xmodmap and leaves it bound, so xdotool presses it as an ordinary key and
        never remaps anything around the press. The free keycodes are a small
        cache: one is reused for a new character only once the app has taken every
        key sent before (_await_typed_input), so no pending key can be read under
        the new mapping. The X server delivers the MappingNotify ahead of any later
        key event, so SDL reads the next key under the mapping set here."""
        if char in self._latin1_keycodes:
            self._latin1_keycodes[char] = self._latin1_keycodes.pop(char)  # most recent last
            return
        if self._free_keycodes is None:
            listing = subprocess.run(["xmodmap", "-pke"], env=self.tool_env, capture_output=True, text=True)
            self._free_keycodes = [int(line.split()[1]) for line in listing.stdout.splitlines()
                                   if line.startswith("keycode") and len(line.split()) <= 3]
        if self._free_keycodes:
            keycode = self._free_keycodes.pop()
        else:
            self._await_typed_input()
            oldest = next(iter(self._latin1_keycodes))
            keycode = self._latin1_keycodes.pop(oldest)
        keysym = "0x%x" % ord(char)
        subprocess.run(["xmodmap", "-e", f"keycode {keycode} = {keysym} {keysym}"], env=self.tool_env,
                       capture_output=True)
        if xt := self._xt_or_none():
            xt.refresh_keymap()
        self._latin1_keycodes[char] = keycode

    def center(self, element: dict[str, Any]) -> tuple[float, float]:
        x, y, w, h = element["rect"]
        return x + w / 2, y + h / 2

    def wait_git_settled(self, timeout: float = 5.0) -> bool:
        """Waits until the git view has applied its refreshes with nothing more
        coming (`ui` -> git.settled): its rows move when a refresh lands. After a
        change made outside the app (outside_change_pending) it first asks for one
        -- the watcher may not have seen the change yet -- since that is what makes
        the rows read afterwards final. Only then: `git-refresh` also re-reads the
        files and reloads clean buffers, which would stand in for the watcher in a
        flow that checks it."""
        if self.outside_change_pending:
            self.outside_change_pending = False
            self.send("git-refresh")
        return self.wait_until(lambda: bool((self.ui().get("git") or {}).get("settled")), timeout,
                               "the git view to settle")

    def click_named(self, name: str, **kwargs: Any) -> dict[str, Any]:
        if name.startswith(GIT_VIEW_ELEMENTS):
            self.wait_git_settled()
        element = self.element(name)
        self.click(*self.center(element), **kwargs)
        return element

    def hover_named(self, name: str) -> dict[str, Any]:
        element = self.element(name)
        self.move(*self.center(element))
        return element

    # ---- observation -----------------------------------------------------

    def settle(self, seconds: float = 0.25) -> None:
        time.sleep(seconds)

    def settle_quiet(self, most: float = 0.25, quiet: float = 0.03, least: float = 0.005) -> float:
        """Waits until the app's log has been still for `quiet` seconds -- the
        frame trace logs every frame and the input trace every event, so a still
        log is an app that has painted what the last input asked for -- or `most`
        seconds have passed. Returns the time waited. The monkey's fixed 0.12 s
        settle was 80% of a step; most actions are painted in a few ms."""
        self.settled_since_record = True
        started = time.monotonic()
        deadline = started + most * SLOW_FACTOR
        # The app says when it is idle (the `idle` query, answered at its next idle
        # point): no 30 ms of silence to wait out, which was a third of a monkey step
        # (profiled 2026-11-13). A binary without the query -- an old finding's
        # replay -- falls back to watching the log.
        if self._idle_query is not False and self.alive():
            idle = self._wait_idle(deadline)
            if idle is not None:
                return time.monotonic() - started
        time.sleep(least)
        last_size = -1
        still_since = time.monotonic()
        while True:
            now = time.monotonic()
            try:
                size = self.log_path.stat().st_size if self.log_path is not None else 0
            except OSError:
                size = last_size
            if size != last_size:
                last_size, still_since = size, now
            elif now - still_since >= quiet:
                break
            if now >= deadline:
                break
            time.sleep(0.004)
        return time.monotonic() - started

    def _wait_idle(self, deadline: float) -> bool | None:
        """Waits for the app's next idle point (`idle` with wait) until `deadline`:
        True when it came, False when the deadline did first (the app is busier
        than the settle allows -- not a stall; its late answer is skipped by id),
        None when the app does not know the query. Never raises."""
        try:
            self._await_typed_input()
            if self._socket is None:
                listing = _last_json(self._control(["control-list"]).stdout) or {}
                if not listing.get("socket"):
                    return None
                self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self._socket.connect(listing["socket"])
            request_id = self._next_id
            self._next_id += 1
            self._socket.sendall(json.dumps({"id": request_id, "query": "idle", "args": {"wait": True}}).encode()
                                 + b"\n")
            while True:
                newline = self._socket_buffer.find(b"\n")
                if newline < 0:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        return False
                    self._socket.settimeout(left)
                    try:
                        chunk = self._socket.recv(65536)
                    except socket.timeout:
                        return False
                    if not chunk:
                        return None
                    self._socket_buffer += chunk
                    continue
                raw, self._socket_buffer = self._socket_buffer[:newline], self._socket_buffer[newline + 1:]
                with contextlib.suppress(json.JSONDecodeError, UnicodeDecodeError):
                    reply = json.loads(raw.decode("utf-8", "replace"))
                    if isinstance(reply, dict) and reply.get("id") == request_id:
                        if not reply.get("ok"):
                            self._idle_query = False  # an app without the query
                            return None
                        return True
        except (OSError, QaFailure):
            return None

    def grab_raw(self) -> "bytes | xtest.Shot | None":
        """The screen as xwd bytes (converted only if a finding needs it: the
        evidence ring keeps one per action, and converting each was most of a
        check's cost)."""
        if self.wayland:
            return None
        if (xt := self._xt_or_none()) is not None:
            with contextlib.suppress(Exception):
                if (shot := xt.screenshot()) is not None:
                    return shot
        try:
            return subprocess.run(["xwd", "-display", self.display, "-root", "-silent"],
                                  capture_output=True, env=self.tool_env, timeout=10).stdout
        except Exception:  # noqa: BLE001 - evidence must never abort the suite
            return None

    def image_of(self, raw: "bytes | xtest.Shot | Image.Image | None"):
        if raw is None:
            return Image.new("RGB", self.size, (0, 0, 0))
        if isinstance(raw, Image.Image):
            return raw
        if isinstance(raw, xtest.Shot):
            return raw.image()
        import io
        try:
            return _XWD.convert(io.BytesIO(raw))
        except Exception as error:  # noqa: BLE001 - evidence must never abort the suite
            print(f"grab failed: {error}", flush=True)
            return Image.new("RGB", self.size, (0, 0, 0))

    def grab(self):
        if self.wayland:
            # No screen reader on a headless weston without its debug protocols:
            # evidence is the frame trace and the redraw verifier's own dumps.
            return Image.new("RGB", self.size, (0, 0, 0))
        if (xt := self._xt_or_none()) is not None:
            with contextlib.suppress(Exception):
                if (shot := xt.screenshot()) is not None:
                    return shot.image()
        import io
        try:
            xwd = subprocess.run(["xwd", "-display", self.display, "-root", "-silent"],
                                 capture_output=True, env=self.tool_env, timeout=10)
            return _XWD.convert(io.BytesIO(xwd.stdout))
        except Exception as error:  # noqa: BLE001 - evidence must never abort the suite
            print(f"grab failed: {error}", flush=True)
            return Image.new("RGB", self.size, (0, 0, 0))

    def screenshot(self, label: str) -> str | None:
        if self.shots is None:
            return None
        self.shots.mkdir(parents=True, exist_ok=True)
        path = self.shots / f"{len(self.actions):04d}-{_slug(label)}.png"
        self.grab().save(path)
        return str(path)

    def new_log_lines(self) -> list[str]:
        if self.log_path is None or not self.log_path.exists():
            return []
        with open(self.log_path, "rb") as log:
            log.seek(self.log_offset)
            data = log.read()
            self.log_offset += len(data)
        return data.decode("utf-8", "replace").splitlines()

    # ---- oracles ---------------------------------------------------------

    def check(self, label: str = "") -> list[Finding]:
        """Collects what the always-on oracles saw since the last check, and
        keeps the window after this action in the evidence ring."""
        found: list[Finding] = []
        self.recent_log = self.new_log_lines()
        for line in finding_lines(self.recent_log, self.injected_write_failures):
            found.append(Finding("log", line.strip()))
        if self.alive() and self.dying():
            # On its way out (its core going to the crash reporter): the exit
            # status is worth the wait, and "alive" for these seconds is not.
            self.wait_gone(60.0)
        if not self.alive():
            code = self.proc.returncode if self.proc else None
            tail = self.log_tail(256 * 1024)
            lost = rr_failure(tail) if self.recording else None
            if lost is not None:
                # rr stopped, and took the app with it: the recorder's limit, not
                # the app's crash. Its own kind, which no ledger books.
                found.append(Finding("recorder", f"rr stopped recording after {label or 'last action'}: {lost}"))
            else:
                # The crashing thread's stack, as the app printed it on its way
                # down (platform/CrashDiagnostics.h), resolved: what the finding
                # is keyed on and what its bundle opens with.
                blocks = CRASH_BLOCK.findall(tail)
                if blocks and not self.crash_stack:
                    self.crash_stack = symbolize_backtrace(blocks[-1], self.binary)
                name = signal_name(code)
                found.append(Finding("crash", f"microide exited ({code}{': ' + name if name else ''}) "
                                              f"after {label or 'last action'}"))
        else:
            for kind, message in self._ui_oracles():
                found.append(Finding(kind, f"after {label or 'last action'}: {message}"))
        if found:
            shot = self._write_finding(label, found)
            for finding in found:
                finding.screenshot = shot
            self.findings.extend(found)
        if self.alive():
            with contextlib.suppress(Exception):
                self.recent_shots.append((len(self.actions), self.grab_raw()))
        return found

    def _ui_oracles(self) -> list[tuple[str, str]]:
        """Invariants of the `ui` answer, checked after every action, so every flow
        and the monkey hold them:

        - focus: keyboard focus on a surface that is not there (the overlay with no
          overlay, modal or menu up; a hidden sidebar or panel) -- every key then
          goes nowhere, which is what Escape from the palette did to the tree;
        - bounds: an element outside the window (a card centred on a narrow
          editor area ran off it);
        - surface: a sidebar, debug-pane or panel control outside its surface
          (the 41-px sidebar drew Refresh past its edge);
        - overlap: two controls that partly cover each other, with no overlay or
          modal up (the sidebar's ... button over its view tabs, monkey seed 4) --
          one nested in another, a tab's close button, is not one.

        Not one: "the active tab is in view". A tab strip scrolled by the wheel
        leaves it out of view on purpose, as VS Code's does; revealing it on
        activation is `tab-overflow`'s to check."""
        try:
            ui = self.ui()
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            self.last_ui = None
            return []
        self.last_ui = ui
        self.last_ui_actions = len(self.actions)
        out: list[tuple[str, str]] = []
        focus = ui.get("focus")
        menu = ui.get("menu") or {}
        if focus == "overlay" and not ((ui.get("overlay") or {}).get("visible") or ui.get("modal")
                                       or menu.get("open") or menu.get("contextMenu")):
            out.append(("focus", "keyboard focus is on an overlay, and no overlay, modal or menu is shown"))
        if focus == "sidebar" and not (ui.get("sidebar") or {}).get("visible", True):
            out.append(("focus", "keyboard focus is on the sidebar, which is hidden"))
        if focus == "panel" and not (ui.get("panel") or {}).get("visible", True):
            out.append(("focus", "keyboard focus is on the panel, which is hidden"))
        window = ui.get("window") or {}
        width, height = window.get("width", 0), window.get("height", 0)
        # Below the layout floor (util::kMinimumLayoutWidth x Height, in layout
        # units) the chrome is known not to fit -- a large ui.scale on a screen
        # too small to grow the window to the floor (TD-2026-11-08-435 item 11) --
        # so the geometry oracles would only repeat that; focus and state still run.
        below_floor = bool(width and height) and (width < LAYOUT_FLOOR[0] or height < LAYOUT_FLOOR[1])
        if width and height and not below_floor:
            for element in ui.get("elements", []):
                x, y, w, h = element["rect"]
                if x < -1 or y < -1 or x + w > width + 1 or y + h > height + 1:
                    out.append(("bounds", f"{element['name']} {element['rect']} leaves the {width}x{height} window"))
                    break
        if not below_floor:
            out += _surface_and_overlap_findings(ui)
        out += self._state_oracles(ui)
        return out

    def _state_oracles(self, ui: dict[str, Any]) -> list[tuple[str, str]]:
        """What the chrome SAYS, checked against the buffer it describes. The
        redraw verifier compares a partial frame with a full render of the same
        state, so a label derived from stale state passes it: the find widget kept
        the previous tab's match count across a tab switch, every frame correct.

        - status: `Ln N, Col M` names the focused editor's primary caret (Col is
          the visible column: a tab counts as its width);
        - find: the widget's count of a plain query (letters and digits, no regex,
          no whole word) is the number of its occurrences in the buffer;
        - tab: the focused group's active tab shows the dirty mark exactly when
          its buffer is unsaved, and is named after its buffer's file;
        - indent / language: the status bar's `Spaces: N` / `Tabs: N` and language
          segments name the buffer's own;
        - window title: names the focused group's active tab, dirty-marked when it
          is, and on X11 is what the platform window is called.

        Only an editor tab, and only what can be computed here without guessing:
        a line with a character past ASCII before the caret is skipped (wide
        characters), as is a buffer too large for the query to carry its text.
        The labels are derived at frame prep, so a disagreement is read again
        after the next frames before it is reported: a lag of one frame is not
        stale state."""
        def findings(ui: dict[str, Any]) -> list[tuple[str, str]]:
            return (self._state_findings(ui) + self._tab_strip_findings(ui) + self._terminal_tab_findings(ui)
                    + self._window_title_findings(ui))
        self.state_evidence = []
        found = findings(ui)
        for attempt in range(3):
            if not found:
                return []
            self.state_evidence.append(f"read {attempt}: {self._frames_logged()} frame(s) logged; "
                                       f"{[message for _, message in found]}")
            time.sleep(0.2 * SLOW_FACTOR)
            try:
                ui = self.ui()
                self.last_ui = ui
                found = findings(ui)
            except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
                return []
        return found

    def _terminal_tab_findings(self, ui: dict[str, Any]) -> list[tuple[str, str]]:
        """The panel's terminal tabs name their terminals: each terminal's active
        pane label (the shell, or the title its program set) is a panel tab, and the
        active terminal's tab is the highlighted one while a terminal is shown."""
        if not (ui.get("panel") or {}).get("visible"):
            return []
        shown = [e for e in ui.get("elements", []) if e.get("kind") == "panel-tab"]
        if not shown:
            return []
        try:
            tabs = self.query("terminals")
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return []
        tabs = tabs if isinstance(tabs, list) else tabs.get("tabs", [])

        def label_of(tab: dict) -> str:
            # As the strip shows it: a host terminal's label carries "host · ".
            pane = next((p for p in tab.get("panes", []) if p.get("active")), (tab.get("panes") or [{}])[0])
            return pane.get("labelPrefix", "") + pane.get("label", "")

        def names(element: dict, label: str) -> bool:
            shown_label = element.get("label", "")
            # A host terminal whose link is down says so after its label
            # (TabStripService's " · detached"): a disconnected remote project's
            # terminals were each reported as named by no tab.
            if shown_label.endswith(" · detached"):
                shown_label = shown_label[:-len(" · detached")]
            return shown_label == label or (shown_label.endswith("…") and label.startswith(shown_label[:-1]))
        out: list[tuple[str, str]] = []
        # A strip that overflows shows some of its tabs and an "<N"/"N>" chip for
        # the rest: a terminal scrolled out of it is named by no shown tab, by
        # design (the monkey's fifteen terminals). Only an unscrolled strip owes a
        # tab to every terminal; the highlight check below holds either way.
        scrolled = any(e.get("name", "").startswith("button:panel-tabs/more-") for e in ui.get("elements", []))
        for tab in [] if scrolled else tabs:
            label = label_of(tab)
            if label and not any(names(e, label) for e in shown):
                out.append(("state", f"terminal {tab.get('tab')} is {label!r}, and no panel tab names it "
                                     f"({[e.get('label') for e in shown]})"))
        lit = [e for e in shown if e.get("active")]
        active = next((t for t in tabs if t.get("active")), None)
        if active is not None and len(lit) == 1 and any(names(lit[0], label_of(t)) for t in tabs) \
                and not names(lit[0], label_of(active)):
            out.append(("state", f"the panel highlights {lit[0].get('label')!r}; the active terminal is "
                                 f"{label_of(active)!r}"))
        return out

    def _window_title_findings(self, ui: dict[str, Any]) -> list[tuple[str, str]]:
        """The window title (`● file — project — microide`) starts with the focused
        group's active tab, dirty-marked exactly when that tab is; on X11 the
        platform window carries the same string (what the taskbar and Alt+Tab show)."""
        title = ui.get("windowTitle")
        if title is None:
            return []
        try:
            groups = self.editor_state().get("groups", [])
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return []
        out: list[tuple[str, str]] = []
        group = next((g for g in groups if g.get("focused")), None)
        active = next((t for t in (group or {}).get("tabs", []) if t.get("active")), None)
        if active is not None:
            want = ("\u25cf " if active.get("dirty") else "") + active.get("title", "") + " \u2014 "
            if not title.startswith(want):
                out.append(("state", f"the window is titled {title!r}; the active tab is {want[:-3]!r}"))
        if not self.wayland and self.window:
            result = subprocess.run(["xdotool", "getwindowname", self.window], env=self.tool_env,
                                    capture_output=True, text=True)
            name = result.stdout.rstrip("\n")
            if result.returncode == 0 and name != title:
                out.append(("state", f"the platform window is named {name!r}; the title bar says {title!r}"))
        return out

    def _tab_strip_findings(self, ui: dict[str, Any]) -> list[tuple[str, str]]:
        """Every group's strip, not only the focused one: each visible tab is one of
        that group's tabs as the `editor` query has it -- its title, its dirty mark
        -- and the group's highlighted tab is its active one. A background group's
        strip is repainted by nothing the focused group does, so a stale label
        there has nothing to correct it."""
        try:
            self.last_editor = self.editor_state()
            groups = self.last_editor.get("groups", [])
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return []
        out: list[tuple[str, str]] = []
        for group in groups:
            prefix = f"tab:{group.get('index')}/"
            shown = [e for e in ui.get("elements", []) if e.get("kind") == "tab" and e["name"].startswith(prefix)]
            if not shown:
                continue
            tabs = group.get("tabs", [])

            def matches(label: str, tab: dict) -> bool:
                # The query names a diff or merge tab "compare: x" / "merge: x"; its
                # strip label is the file's name.
                # A comparison of two inputs is "compare: left  ↔  right" there, and
                # its strip label is the right side's file name.
                title = tab.get("title", "\0")
                for kind_prefix in ("compare: ", "merge: "):
                    title = title.removeprefix(kind_prefix)
                shown = label.lstrip("*")
                if "↔" in title:
                    # ... or, with no file behind it (the clipboard against an
                    # untitled buffer), the comparison's own name. The file's name
                    # may carry its folder (" — qa-weird") when another tab in the
                    # group has the same name, and the right side's label a note
                    # ("long-line.txt (unsaved)"): soak cf41bb666445.
                    right = title.rsplit("↔", 1)[-1].strip()
                    name = shown.split(" \u2014 ", 1)[0]
                    named = right.startswith(name) or shown.startswith(right) or shown == title
                else:
                    named = shown.startswith(title)
                return label.startswith("*") == bool(tab.get("dirty")) and named
            for element in shown:
                label = element.get("label", "")
                if not any(matches(label, tab) for tab in tabs):
                    out.append(("state", f"group {group.get('index')} shows a tab {label!r} it does not hold "
                                         f"({[('*' if t.get('dirty') else '') + t.get('title', '') for t in tabs]})"))
            active = next((t for t in tabs if t.get("active")), None)
            lit = next((e for e in shown if e.get("active")), None)
            if active is not None and lit is not None and not matches(lit.get("label", ""), active):
                out.append(("state", f"group {group.get('index')} highlights {lit.get('label')!r}; its active tab is "
                                     f"{('*' if active.get('dirty') else '') + active.get('title', '')!r}"))
        return out

    def _state_findings(self, ui: dict[str, Any]) -> list[tuple[str, str]]:
        try:
            buffer = self.query("buffer")
            self.last_buffer = buffer
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return []
        if not buffer.get("available") or buffer.get("kind") != "editor":
            return []
        out: list[tuple[str, str]] = []
        elements = {e["name"]: e for e in ui.get("elements", [])}
        text = buffer.get("text")
        cursors = buffer.get("cursors") or []
        status = elements.get("status:line-column")
        match = re.match(r"Ln (\d+), Col (\d+)", (status or {}).get("label", ""))
        if match and cursors and text is not None:
            caret = cursors[0]
            lines = text.split("\n")
            if 1 <= caret["line"] <= len(lines):
                prefix_bytes = lines[caret["line"] - 1].encode()[:caret["column"] - 1]
                if prefix_bytes.isascii():
                    tab = max(1, buffer.get("tabSize", 4))
                    visual = 0
                    for ch in prefix_bytes.decode():
                        visual += tab - visual % tab if ch == "\t" else 1
                    want = (caret["line"], visual + 1)
                    got = (int(match.group(1)), int(match.group(2)))
                    if got != want:
                        out.append(("state", f"the status bar says Ln {got[0]}, Col {got[1]}; the caret is at "
                                             f"Ln {want[0]}, Col {want[1]}"))
                        self.state_evidence.append(
                            f"status {status.get('label')!r}; caret line {caret['line']} byte column "
                            f"{caret['column']} of {len(cursors)} caret(s); tabSize {buffer.get('tabSize')}; "
                            f"revision {buffer.get('revision')}; line text "
                            f"{lines[caret['line'] - 1][:512]!r}; {len(lines)} line(s); carets {cursors[:8]}; "
                            f"the status bar's line text "
                            f"{lines[got[0] - 1][:512] if 1 <= got[0] <= len(lines) else None!r}")
        overlay = ui.get("overlay") or {}
        query = overlay.get("query") or ""
        if (overlay.get("visible") and overlay.get("mode") in ("buffer-search", "buffer-replace")
                and text is not None and len(query) >= 2 and query.isascii() and query.isalnum()
                and not (elements.get("button:find/regex") or {}).get("active")
                and not (elements.get("button:find/whole-word") or {}).get("active")
                and isinstance(overlay.get("count"), int) and overlay["count"] < 1000):
            haystack: str | None = text
            if scope := overlay.get("scope"):
                # Find in Selection counts inside the scope (1-based line, byte column
                # of the buffer's bytes). A reply decoded lossily (invalid UTF-8 read
                # as U+FFFD, three bytes for each bad one) no longer has the buffer's
                # byte geometry, so the scope cannot be cut from it: a binary.bin
                # selection holding the query once read as holding it none (soak
                # ccc6cfdc08e4). Cut before case-folding, which can change lengths.
                if "�" in text:
                    haystack = None
                else:
                    lines = text.encode().split(b"\n")
                    def offset(at: list[int]) -> int:
                        line, column = at[0] - 1, at[1] - 1
                        return sum(len(lines[i]) + 1 for i in range(min(line, len(lines)))) + column
                    haystack = text.encode()[offset(scope["start"]):offset(scope["end"])].decode("utf-8", "replace")
            needle = query
            if haystack is not None and not (elements.get("button:find/match-case") or {}).get("active"):
                haystack, needle = haystack.lower(), query.lower()
            want = haystack.count(needle) if haystack is not None else overlay["count"]
            if overlay["count"] != want:
                out.append(("state", f"the find widget counts {overlay['count']} of {query!r}; the buffer holds {want}"))
        focused = next((pane.get("group") for pane in ui.get("editors", []) if pane.get("focused")), None)
        if focused is not None:
            active = next((e for e in ui.get("elements", []) if e.get("kind") == "tab" and e.get("active")
                           and e["name"].startswith(f"tab:{focused}/")), None)
            if active is not None and active.get("label", "").startswith("*") != bool(buffer.get("dirty")):
                out.append(("state", f"the active tab {active['label']!r} and its buffer disagree about unsaved "
                                     f"changes (dirty: {buffer.get('dirty')})"))
            # A file's tab is named after it (an untitled buffer has no path). A
            # duplicate basename may carry a disambiguating suffix, so the label
            # starts with the name rather than equalling it.
            path = buffer.get("path") or ""
            if active is not None and path:
                name = path.rsplit("/", 1)[-1]
                if not active.get("label", "").lstrip("*").startswith(name):
                    out.append(("state", f"the active tab is labelled {active['label']!r}; its buffer is {name!r}"))
        indent = (elements.get("status:indent") or {}).get("label")
        if indent and "tabSize" in buffer:
            want = f"{'Spaces' if buffer.get('softTabs') else 'Tabs'}: {buffer['tabSize']}"
            if indent != want:
                out.append(("state", f"the status bar says {indent!r}; the buffer indents with {want!r}"))
        language = (elements.get("status:language") or {}).get("label")
        # No detected language is shown as "Plain Text" (the id is "unknown").
        if language and buffer.get("language") not in (None, "", "unknown") and language != buffer["language"]:
            out.append(("state", f"the status bar names the language {language!r}; the buffer's is "
                                 f"{buffer['language']!r}"))
        return out

    def report(self, label: str, found: list[tuple[str, str]]) -> list[Finding]:
        """Records findings an oracle outside check() made (the monkey's data,
        undo, leak and latency oracles), with the same evidence directory."""
        if not found:
            return []
        findings = [Finding(kind, f"after {label}: {message}") for kind, message in found]
        shot = self._write_finding(label, findings)
        for finding in findings:
            finding.screenshot = shot
        self.findings.extend(findings)
        return findings

    def _write_finding(self, label: str, found: list[Finding]) -> str | None:
        """A finding is a directory: the actions up to it, the frame lines since
        the previous action, the last screenshots and the window now."""
        if self.shots is None:
            return None
        folder = self.shots / f"finding-{len(self.findings) + 1:02d}-{_slug(label)}"
        folder.mkdir(parents=True, exist_ok=True)
        self.save_actions(folder / "replay.json", **self.replay_header)
        (folder / "findings.txt").write_text("\n".join(f"{f.kind}: {f.detail}" for f in found) + "\n")
        (folder / "trace.txt").write_text("\n".join(self.recent_log) + "\n")
        # The log's tail too: a crash's sanitizer report and stack, the frames
        # before the trace window (the soak runner's FINDING.md reads both).
        tail = self.log_tail(64 * 1024)
        (folder / "log-tail.txt").write_text(tail)
        # The actions and the frames they caused, on one clock (the frame
        # lines' mono= and the rows' t).
        if timeline := interleave_timeline(self.actions, tail.splitlines()):
            (folder / "timeline.txt").write_text(timeline)
        (folder / "machine.txt").write_text(machine_pressure())
        self._write_state_snapshot(folder)
        if self.recording and self._trace_dir is not None:
            # Which of the run's traces holds this finding, and how far into it.
            (folder / "recording.txt").write_text(
                f"trace: rr/{self._trace_dir.name}\n"
                f"mode: {'chaos' if self._rr_mode == 'chaos' else 'plain'}\n"
                f"launch: {self._launches} of this run (a kill -9 or a restart starts the next)\n"
                f"frames logged by this launch: {self._frames_logged()}\n")
        # After a kill -9 the log above is the relaunched process's; what the killed
        # one did last (its flushes, its saves) is in the launch before it.
        previous = self._previous_log_path()
        if previous is not None:
            with open(previous, "rb") as log:
                log.seek(0, os.SEEK_END)
                log.seek(max(0, log.tell() - 64 * 1024))
                (folder / "log-tail-before-relaunch.txt").write_text(log.read().decode("utf-8", "replace"))
        for name in ("stall-threads.txt", "stall-backtrace.txt"):
            if self.shots is not None and (self.shots / name).exists():
                shutil.copy2(self.shots / name, folder / name)
        if self.crash_stack and any(f.kind == "crash" for f in found):
            (folder / "crash-stack.txt").write_text(self.crash_stack)
        self._write_damage_requests(folder, found)
        # A persisted record the app read but could not decode: its framing and
        # checksum held, so the file is exactly what the encoder wrote and the
        # decoder refused -- the bytes are the evidence, and the app's next save
        # overwrites them (soak 4bbda24149d1).
        for finding in found:
            match = re.search(r"persistence: (\S+?)(?: \(backup\))? read but did not decode", finding.detail)
            if match is None:
                continue
            for source in (Path(match.group(1)), Path(match.group(1) + ".bak")):
                if source.is_file():
                    with contextlib.suppress(OSError):
                        shutil.copy2(source, folder / f"undecodable-{source.name}")
        if self.state_evidence and any(f.kind == "state" for f in found):
            (folder / "state-evidence.txt").write_text(
                "\n".join(self.state_evidence + [f"reported: {self._frames_logged()} frame(s) logged"]) + "\n")
        for index, raw in self.recent_shots:
            self.image_of(raw).save(folder / f"after-action-{index:04d}.png")
        # The verifier's own images of the stale frames this finding names.
        stale_dir = self.work / "stale"
        for finding in found:
            marker = finding.detail.split("frame=", 1)
            if len(marker) == 2:
                frame = marker[1].split(" ", 1)[0]
                for kind in ("retained", "reference"):
                    source = stale_dir / f"frame-{frame}-{kind}.bmp"
                    if source.exists():
                        with contextlib.suppress(Exception):
                            Image.open(source).save(folder / f"frame-{frame}-{kind}.png")
        path = folder / "now.png"
        # The folder is the evidence: a dead app has no window to grab, and its
        # finding still names where the log tail and the last screenshots are.
        if self.alive():
            self.grab().save(path)
        return str(path)

    # What the app says of itself when a finding is made, kept beside it: the
    # screenshot shows the window, these say what is behind it -- which tabs,
    # which documents and how dirty, what was staged, what async work the
    # notifications name. A finding no replay reproduces had none of it.
    _SNAPSHOT_QUERIES = (("status", None), ("tabs", None), ("editor", None), ("documents", {"all": True}),
                         ("projects", None), ("persistence", None), ("notifications", {"recent": True}),
                         ("terminals", None), ("debug-state", None), ("invariants", None),
                         ("crash-points", None))

    def _write_damage_requests(self, folder: Path, found: list[Finding]) -> None:
        """For a stale frame (QA-STALE frame=N): the repaint requests made for that
        frame, each with the code that asked, symbolized -- what painted instead
        of the damage that was owed (damage_requests_before)."""
        frames = sorted({int(m.group(1)) for f in found if (m := re.search(r"QA-STALE frame=(\d+)", f.detail))})
        if not frames:
            return
        log = self.log_tail(4 * 1024 * 1024)
        out: list[str] = []
        for frame in frames[:3]:
            requests = damage_requests_before(log, frame)
            out.append(f"== frame {frame}: {len(requests)} repaint request(s)" +
                       ("" if requests else " (none logged: the damage came from an event's own result, "
                                            "or the build predates MICROIDE_TRACE_DAMAGE)"))
            for block in requests:
                head, *stack = block.splitlines()
                out.append(symbolize_backtrace("\n".join([head, *(re.sub(r"^\s*#\d+\s+", "", line)
                                                                 for line in stack)]), Path(self.binary))
                           if self.binary else block)
        (folder / "damage-requests.txt").write_text("\n".join(out) + "\n")

    def _write_state_snapshot(self, folder: Path) -> None:
        """state.json: the app's answers to its state queries now. Best effort and
        bounded: a dead app answers nothing, a stalled one stops the snapshot at
        its first unanswered query."""
        if not self.alive() or self.dying() or self._in_snapshot:
            return
        self._in_snapshot = True
        snapshot: dict[str, Any] = {}
        keep = (self.slowest_answer, self.slowest_sched, self.slowest_stall, self.slowest_waits, self.slowest_helpers,
                self.slowest_profile)
        try:
            for verb, args in self._SNAPSHOT_QUERIES:
                payload: dict[str, Any] = {"query": verb}
                if args is not None:
                    payload["args"] = args
                try:
                    reply = self._socket_request(payload, 3.0, f"query {verb!r}")
                except Exception as failure:  # noqa: BLE001 - evidence never costs the finding it is for
                    snapshot[verb] = {"error": f"{type(failure).__name__}: {failure}"}
                    break
                snapshot[verb] = reply.get("result") if reply.get("ok") else {"error": reply.get("error")}
        finally:
            self._in_snapshot = False
            # The snapshot's own answers are not the run's latency.
            (self.slowest_answer, self.slowest_sched, self.slowest_stall, self.slowest_waits, self.slowest_helpers,
             self.slowest_profile) = keep
        with contextlib.suppress(OSError, TypeError, ValueError):
            (folder / "state.json").write_text(json.dumps(snapshot, indent=1)[:2 * 1024 * 1024])

    def _previous_log_path(self) -> Path | None:
        """The log of the launch before the current one (a crash_and_relaunch or a
        restart), if there was one."""
        if self._launches < 2:
            return None
        path = self.work / (f"microide-{self._launches - 2}.log" if self._launches > 2 else "microide.log")
        return path if path.exists() else None

    def log_tail(self, max_bytes: int = 96 * 1024) -> str:
        """The end of the app's log, without consuming it (the oracles read the log
        incrementally from their own offset)."""
        if self.log_path is None or not self.log_path.exists():
            return ""
        with open(self.log_path, "rb") as log:
            log.seek(0, os.SEEK_END)
            size = log.tell()
            log.seek(max(0, size - max_bytes))
            return log.read().decode("utf-8", "replace")

    def expect(self, condition: bool, message: str) -> bool:
        """Records a finding when `condition` is false; returns the condition, so a
        flow can gate its next steps on it (`if not app.expect(...): return`).
        Beside the screenshot go the last actions and the app log's tail (frames,
        and every key and text event with the focus that took it): an expectation
        that fails only on a CI runner had nothing else to read."""
        if not condition:
            finding = Finding("expect", message)
            # A finding directory like the oracles' (replay.json, log tail, the
            # last frames): with only a screenshot, the soak's bundle of an
            # expect finding had no replay to reproduce or minimize it from.
            shot = self._write_finding("expect-" + message[:40], [finding])
            if shot is not None:
                with contextlib.suppress(Exception):
                    recent = json.dumps(self.actions[-40:], indent=1)
                    dap = self.work / "dap.log"
                    dap_tail = dap.read_bytes()[-24 * 1024:].decode("utf-8", "replace") if dap.exists() else ""
                    (Path(shot).parent / "expect.txt").write_text(
                        f"{message}\n\n--- the last actions\n{recent}\n\n--- the app log's tail\n{self.log_tail()}"
                        + (f"\n\n--- the debug adapter's traffic (tail)\n{dap_tail}" if dap_tail else ""))
            finding.screenshot = shot
            self.findings.append(finding)
        return bool(condition)

    # ---- replay ----------------------------------------------------------

    def _record(self, action: dict[str, Any]) -> None:
        action["t"] = round(time.monotonic(), 3)
        if self.actions and not self.settled_since_record and not self.in_burst:
            self.actions[-1]["n"] = 1
        self.settled_since_record = False
        if self.in_burst:
            action["b"] = 1
        if self.in_oracle:
            action["o"] = 1
        self.actions.append(action)

    def record_external(self, action: dict[str, Any]) -> None:
        """Records something done to the run from outside the window (the
        monkey's file churn) so a replay does it at the same point."""
        self._record(action)

    def save_actions(self, path: Path, **header: Any) -> None:
        """A replay file: the run's header (fixture, profile, seed, size) and the
        actions in order. `tools/qa/run.py replay` re-runs it on a fresh fixture."""
        path.write_text(json.dumps({"size": list(self.size), **header, "actions": self.actions},
                                   indent=1))


# Which surface a control belongs to, by its name (the `surface` oracle).
_SURFACE_OWNERS = (
    ("sidebar-view:", "sidebar"), ("button:sidebar/", "sidebar"), ("button:search/", "sidebar"),
    ("debug-tab:", "rightPane"), ("debug-row:", "rightPane"),
    ("panel-tab:", "bottomPanel"), ("button:panel/", "bottomPanel"), ("button:close-panel-tab/", "bottomPanel"),
)
_OVERLAP_KINDS = {"button", "sidebar-view", "debug-tab", "panel-tab"}
_FLOATING_PREFIX = "button:notification/"
# The keys that choose an open menu's highlighted row (App.activation_guard).
_ACTIVATING_KEYS = {"Return", "KP_Enter", "Enter", "space"}
# The bare keys that step the Settings overlay's selected setting (Left/Right:
# settings_step_selected): they turned `control.enabled` off and cut the driver's
# own channel -- "no control socket to connect to", soak e02c374fd3ee.
_STEPPING_KEYS = {"Left", "Right", "KP_Left", "KP_Right"}
# Floating cards over the editor's CONTENT, and the content controls they float
# over: the debug toolbar is a card over the text ("the editor stays usable
# underneath"), and a code lens is part of that text -- it scrolls under the card
# like a line does (soak 5dd22e100ed0). VS Code's floating toolbar covers it the
# same way. The toolbar over chrome (a banner, the find widget, a panel button)
# is still a collision: those do not move out from under it.
_FLOATS_OVER_CONTENT = ("button:debug-toolbar/",)
# A diff's collapsed-lines bar ("Show all", "Show 20 more") is a row of the
# compare's content like a lens is of the editor's (soak 4df2b07d4b82).
_EDITOR_CONTENT_PREFIXES = ("button:lens/", "button:compare/context-")


def _floats_over(a: str, b: str) -> bool:
    """Whether one of the two is a floating card over editor content the other is."""
    return any(x.startswith(_FLOATS_OVER_CONTENT) and y.startswith(_EDITOR_CONTENT_PREFIXES)
               for x, y in ((a, b), (b, a)))


def _surface_and_overlap_findings(ui: dict[str, Any]) -> list[tuple[str, str]]:
    """The `surface` and `overlap` oracles of App._ui_oracles (one finding each at
    most, so a broken layout does not bury the rest)."""
    out: list[tuple[str, str]] = []
    surfaces = ui.get("surfaces") or {}
    elements = ui.get("elements", [])

    def inside(a: list[int], b: list[int], slack: int = 1) -> bool:
        return (a[0] >= b[0] - slack and a[1] >= b[1] - slack
                and a[0] + a[2] <= b[0] + b[2] + slack and a[1] + a[3] <= b[1] + b[3] + slack)

    for element in elements:
        rect = element["rect"]
        if rect[2] <= 0 or rect[3] <= 0:
            continue
        owner = next((surface for prefix, surface in _SURFACE_OWNERS if element["name"].startswith(prefix)), None)
        box = surfaces.get(owner) if owner else None
        if box and not inside(rect, box):
            out.append(("surface", f"{element['name']} {rect} leaves the {owner} {box}"))
            break
    menu = ui.get("menu") or {}
    if (ui.get("overlay") or {}).get("visible") or ui.get("modal") or menu.get("open") or menu.get("contextMenu"):
        return out
    seen: list[dict[str, Any]] = []
    for element in elements:
        if element.get("kind") not in _OVERLAP_KINDS or element["rect"][2] <= 0 or element["rect"][3] <= 0:
            continue
        a = element["rect"]
        for other in seen:
            # Notification rows float over the workbench (VS Code's toasts do too):
            # one covering a panel or editor button is the stack's layer, not a
            # layout collision. Two rows' own buttons overlapping still is.
            if element["name"].startswith(_FLOATING_PREFIX) != other["name"].startswith(_FLOATING_PREFIX):
                continue
            if _floats_over(element["name"], other["name"]):
                continue
            b = other["rect"]
            ix = min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0])
            iy = min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1])
            if ix > 1 and iy > 1 and not inside(a, b, 0) and not inside(b, a, 0):
                out.append(("overlap", f"{element['name']} {a} overlaps {other['name']} {b}"))
                return out
        seen.append(element)
    return out


# The longest recorded gap a paced replay reproduces: longer ones were a relaunch,
# a sanitizer build's stall or a check, not a timing the app depended on.
MAX_PACED_GAP = 5.0


# A fixture path as records made before the header named its root spell it:
# `<out>/fixtures/fx-<random>/taskflow` (run.py fixture()).
_LEGACY_ROOT = re.compile(r"(/[^\s'\"]*?/fixtures/fx-[^/\s'\"]+/(?:taskflow|project))(?=[/\s'\"]|$)")
# The rows whose text can name a file of the run's project.
_PATH_ROW_KEYS = ("line", "send")


def recorded_root(record: dict[str, Any]) -> str | None:
    """The project root a replay record's actions were recorded against: its
    header's `root`, or, for a record from before the header had one, the
    fixture path its command lines name. None when no line names one."""
    root = record.get("root")
    if root:
        return str(root)
    for action in record.get("actions", []):
        for key in _PATH_ROW_KEYS:
            value = action.get(key)
            text = " ".join(value) if isinstance(value, list) else value if isinstance(value, str) else ""
            match = _LEGACY_ROOT.search(text)
            if match:
                return match.group(1)
    return None


def rebase_action(action: dict[str, Any], old_root: str | None, new_root: str | None) -> dict[str, Any]:
    """`action` with the recorded project root in its command line replaced by
    the replay's own.

    A run's fixture is a fresh directory and is deleted with the run; a line
    that named a file by its absolute path (`compare-files`, `breakpoint-set`,
    `debug-run`) named, in every replay, a file that no longer existed. One
    replay in six was of such a record (204 of the soak's 1187 bundles on
    2026-11-16: every diff and most tooling findings), each measured "0 of N
    replays" and filed as a finding no replay reproduces. A corpus prefix has
    the same lines, and started the run that picked it from nothing.

    >>> rebase_action({"line": "compare-files /r/fx-1/taskflow/a.py /r/fx-1/taskflow/b.py"},
    ...               "/r/fx-1/taskflow", "/new/taskflow")
    {'line': 'compare-files /new/taskflow/a.py /new/taskflow/b.py'}
    >>> rebase_action({"line": "open /r/fx-1/taskflow-origin.git/x"}, "/r/fx-1/taskflow", "/new")["line"]
    'open /r/fx-1/taskflow-origin.git/x'
    """
    if not old_root or not new_root or old_root == new_root:
        return action
    pattern = re.compile(re.escape(old_root) + r"(?=[/\s'\"]|$)")
    out = action
    for key in _PATH_ROW_KEYS:
        value = action.get(key)
        if isinstance(value, str) and old_root in value:
            out = {**out, key: pattern.sub(lambda _: new_root, value)}
        elif isinstance(value, list) and any(isinstance(word, str) and old_root in word for word in value):
            out = {**out, key: [pattern.sub(lambda _: new_root, word) if isinstance(word, str) else word
                                for word in value]}
    return out


def replay(app: App, actions: list[dict[str, Any]], pause: float = 0.12,
           churn: Callable[[dict[str, Any]], None] | None = None,
           external: Callable[[dict[str, Any]], None] | None = None,
           check: bool = True,
           oracle: Callable[[dict[str, Any]], None] | None = None,
           paced: bool = False,
           root: str | None = None,
           follow: Callable[[int, dict[str, Any]], None] | None = None,
           stop: Callable[[], bool] | None = None) -> None:
    """Re-runs a recorded action list (what the monkey saves on a finding).

    `root` is the project root the actions were recorded against
    (recorded_root): a line naming a file under it is sent naming the same file
    under this app's `replay_root` (rebase_action).

    `follow(index, action)` is called after the check of every action that
    carries the state the run was in after it (an "s" field; the monkey records
    one per step): where a replay stops following the run is the first thing to
    know about one that does not reproduce (monkey_model.Divergence).

    `churn` re-applies a row the monkey's ExternalChurn recorded: a file rewrite
    that landed between two commands, with no settle or check of its own.
    `external` re-applies the monkey's outside-world rows (git, renames,
    permissions) and its kill -9s. Rows recorded inside a burst ("b") are sent
    back to back, as they were.

    `oracle` re-runs an oracle where the run ran it (an {"oracle": name} row);
    the commands that oracle sent then ("o" rows) are its to send again, so they
    are skipped. Without it they replay as plain actions.

    `paced`: each action waits until at least its recorded gap after the one
    before (up to MAX_PACED_GAP) -- the run's own timing, under the load it ran
    in, which a race needs and the default settle-and-go does not keep.

    `stop`, asked before each action, ends the replay there (ReplayStopped): a
    replay at the slow factor of a starved run takes many minutes, and a pause
    waiting for one waited all of them."""
    previous_recorded: float | None = None
    previous_sent = time.monotonic()
    new_root = str(app.replay_root) if getattr(app, "replay_root", None) is not None else None

    def observe(index: int, action: dict[str, Any]) -> None:
        """The path check for a row the replay applies without a check of its
        own. The run stamps its state on the LAST row a step recorded, whatever
        that row is: a kill -9, an outside-world change, an oracle's row, a
        character typed through the control channel. Skipped with its row, a
        stamp that changed the state left every "same as before" stamp after
        it compared against the state before -- a replay read as having left
        the run's path at the first step after each kill -9 (the relaunch
        moves the focus), and a finding whose replays "diverge" is not hunted."""
        if check and follow is not None and "s" in action:
            app.settle_quiet(most=pause)
            app.check(replay_label(index))
            follow(index, action)

    for index, action in enumerate(actions):
        if stop is not None and stop():
            raise ReplayStopped(f"stopped before action {index} of {len(actions)}")
        if "check" in action or "note" in action or "type-via-control" in action:
            observe(index, action)
            continue
        action = rebase_action(action, root, new_root)
        if "oracle" in action:
            if oracle is not None:
                oracle(action)
            observe(index, action)
            continue
        if action.get("o") and oracle is not None:
            observe(index, action)
            continue
        if paced and isinstance(action.get("t"), (int, float)):
            if previous_recorded is not None:
                gap = min(MAX_PACED_GAP, max(0.0, action["t"] - previous_recorded))
                wait = gap - (time.monotonic() - previous_sent)
                if wait > 0:
                    time.sleep(wait)
            previous_recorded = action["t"]
            previous_sent = time.monotonic()
        if "churn" in action:
            if churn is not None:
                churn(action)
            observe(index, action)
            continue
        if "external" in action or "kill9" in action:
            if external is not None:
                external(action)
            observe(index, action)
            continue
        if "resize" in action:
            app.resize(*action["resize"])
        elif "focus-out" in action:
            app.focus_out()
        elif "foreign-clipboard" in action:
            app.foreign_clipboard(action["foreign-clipboard"])
        elif "foreign-clipboard-image" in action:
            app.foreign_clipboard_image(action["foreign-clipboard-image"])
        elif "drop" in action:
            path = action["path"]
            if "projectPath" in action and app.replay_root is not None:
                path = str(app.replay_root / action["projectPath"])
            app.drop(*action["drop"], path)
        elif "hold" in action:
            app.hold(action["hold"], action.get("keys", []))
        elif "send" in action:
            app.send(*action["send"])
        elif "line" in action:
            app.send_line(action["line"])
        elif "move" in action:
            app.move(*action["move"])
        elif "click" in action:
            app.click(*action["click"], button=action.get("button", 1),
                      clicks=action.get("clicks", 1),
                      modifiers=tuple(action.get("modifiers", ())))
        elif "drag" in action:
            app.drag(*action["drag"], modifiers=tuple(action.get("modifiers", ())), clicks=action.get("clicks", 1))
        elif "wheel" in action:
            app.wheel(*action["wheel"], action["notches"], tuple(action.get("modifiers", ())))
        elif "key" in action:
            app.key(action["key"])
        elif "type" in action:
            app.type(action["type"])
        following = actions[index + 1] if index + 1 < len(actions) else {}
        if action.get("b") and following.get("b"):
            continue  # the rest of the burst follows with no settle
        if action.get("n") and following:
            continue  # the run sent the next row without a settle in between
        app.settle_quiet(most=pause)
        if check:
            app.check(replay_label(index))
            if follow is not None and "s" in action:
                follow(index, action)


def replay_label(index: int) -> str:
    """What a replayed action's check is labelled: its place in the record.

    It was the action itself as JSON, cut at 200 characters, and a finding's
    signature is its message less the label (findings.normalize). The cut left
    a label that did not close for any action over 200 characters -- a long
    text typed, and since the run stamps its state on its rows most drags and
    chords too -- and a brace in the text typed broke it at any length. The
    finding the replay then showed had another signature than the run's: it
    was booked under "the replays showed instead", and the finding it was a
    faithful reproduction of as "no replay reproduces it".

    >>> replay_label(37)
    'step 37'
    """
    return f"step {index}"


def sched_account(seconds: float, ran: float, queued: float) -> str:
    """Where a thread's `seconds` went, in words: on a CPU, runnable and waiting
    for one, or neither (blocked).

    >>> sched_account(4.0, 0.5, 3.0)
    'main thread over 4.00s: 0.50s on a CPU, 3.00s waiting for one (a starved machine), 0.50s blocked'
    """
    blocked = max(0.0, seconds - ran - queued)
    starved = " (a starved machine)" if is_starved(seconds, queued) else ""
    return (f"main thread over {seconds:.2f}s: {ran:.2f}s on a CPU, {queued:.2f}s waiting for one{starved}, "
            f"{blocked:.2f}s blocked")


def helpers_starved(helpers: tuple[float, float] | None) -> bool:
    """The main thread's wait was on threads the machine starved: over the late
    window the app's other threads spent, together, at least half of it runnable
    and waiting for a CPU -- a renderer's rasterizer threads (lavapipe) under the
    soak's load, which a real GPU does not have.

    >>> helpers_starved((2.4, 4.0)), helpers_starved((0.3, 4.0)), helpers_starved(None)
    (True, False, False)
    """
    return helpers is not None and helpers[1] > 0 and helpers[0] >= 0.5 * helpers[1]


def is_starved(seconds: float, queued: float) -> bool:
    """The time was the machine's, not the app's: the thread was runnable and
    waiting for a CPU for at least half of it."""
    return seconds > 0 and queued >= 0.5 * seconds


def make_fixture(dest: Path) -> Path:
    """The deterministic C++ demo repo the media pipeline uses."""
    subprocess.run(["bash", str(CAPTURE / "make-fixture.sh"), str(dest)], check=True,
                   capture_output=True)
    return dest


def quote_arg(word: str) -> str:
    """One argument of a control command line, as the app's tokenizer reads it
    (ParseCommandLine: shell-like, a backslash takes the next character literally,
    anything else -- newlines, non-ASCII -- passes through quotes as is). Not
    json.dumps: the tokenizer has no \\uXXXX, so an escaped name opened a file
    called "u540du524d" and typed text arrived mangled."""
    if word and not any(c.isspace() or c in "\"'\\" for c in word):
        return word
    return '"' + word.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _wait(predicate, timeout: float, message: str) -> None:
    deadline = time.monotonic() + timeout * SLOW_FACTOR
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise QaFailure(message)


def _last_json(text: str) -> dict[str, Any] | None:
    for line in reversed(text.strip().splitlines()):
        with contextlib.suppress(json.JSONDecodeError):
            value = json.loads(line)
            if isinstance(value, dict):
                return value
    return None


def _slug(text: str) -> str:
    return "".join(c if c.isalnum() else "-" for c in text)[:60].strip("-") or "shot"


def seeded(seed: int) -> random.Random:
    return random.Random(seed)
