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
import importlib.util
import json
import re
import os
import random
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterator

from PIL import Image

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
)


def is_finding_line(line: str) -> bool:
    """A log line an oracle reports. The input trace echoes what was typed, so a
    typed "Assertion" is not one."""
    return "microide input: " not in line and any(marker in line for marker in FINDING_MARKERS)

# Sanitizer runtime defaults for a QA run (a sanitizer build reads them; others
# ignore them). halt_on_error so the first report is the one attributable to the
# last action, not the cascade after a corrupted heap; leaks off unless asked
# (MQA_DETECT_LEAKS=1), since a killed GUI process "leaks" everything it held.
SANITIZER_ENV = {
    "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=" + os.environ.get("MQA_DETECT_LEAKS", "0"),
    "LSAN_OPTIONS": f"suppressions={REPO / 'tests' / 'lsan.supp'}",
    "UBSAN_OPTIONS": "print_stacktrace=1",
    "TSAN_OPTIONS": f"suppressions={REPO / 'tests' / 'tsan.supp'}",
}


# SDL's defaults (SDL_HINT_MOUSE_DOUBLE_CLICK_TIME / _RADIUS), with margin.
DOUBLE_CLICK_SECONDS = 0.55

# The window system a run drives: "x11" (Xvfb + xdotool, the default) or
# "wayland" (a headless weston; input goes through the control channel's `pointer`
# and `key` commands, since a Wayland compositor takes input from no other client).
BACKEND = os.environ.get("MQA_BACKEND", "x11")
# How much slower than a release build the binary under test is; every control
# timeout is multiplied by it (a coverage build ran `control-list` past the 3 s a
# fast build answers in 50 ms).
SLOW_FACTOR = float(os.environ.get("MQA_SLOW_FACTOR", "1"))
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


class QaFailure(AssertionError):
    pass


@dataclass
class Finding:
    kind: str
    detail: str
    screenshot: str | None = None


@dataclass
class App:
    binary: Path
    work: Path
    display: str
    size: tuple[int, int]
    env: dict[str, str]
    xvfb: subprocess.Popen | None = None
    proc: subprocess.Popen | None = None
    log_path: Path | None = None
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
    _extra_args: list[str] = field(default_factory=list)

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
               plugins: tuple[str, ...] = ()) -> Iterator["App"]:
        binary = Path(binary or os.environ.get("MICROIDE_BIN") or REPO / "build/microide/microide")
        if not binary.exists():
            raise FileNotFoundError(f"no microide binary at {binary} (set MICROIDE_BIN)")
        # Short path: the control socket lives under XDG_RUNTIME_DIR and AF_UNIX
        # paths stop at 108 bytes.
        work = Path(tempfile.mkdtemp(prefix="mqa-", dir="/tmp"))
        env = dict(os.environ)
        env.update({
            "SDL_VIDEODRIVER": "wayland" if BACKEND == "wayland" else "x11",
            # One SDL_Log line per frame and per clip under an open menu: the frame
            # sequence a finding is read against.
            "MICROIDE_TRACE_REDRAW_VERBOSE": "1",
            "MICROIDE_TRACE_MENU_HOVER": "1",
            # One line per key and text event with the focus that took it: where a
            # keystroke that never reached the buffer went (TD-2026-10-20-368).
            "MICROIDE_TRACE_INPUT": "1",
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
        env.update(extra_env or {})
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
        app = cls(binary=binary, work=work, display=display or "", size=size, env=env, shots=shots,
                  project=project)
        try:
            try:
                app._start(project, extra_args or [])
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
            self.env["DISPLAY"] = self.display
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

    def restart(self, project: Path | None = None, quit_timeout: float = 15.0) -> None:
        """Quits the app the way a person does (the `quit` command, so session state
        is written as on a real exit) and starts it again on the same display, XDG
        tree and project -- the session-restore path. An unsaved buffer makes quit
        ask first; a scenario that wants that prompt drives it before calling this."""
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
        for line in self.new_log_lines():
            if is_finding_line(line):
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
        self._spawn(project if project is not None else self.project, self._extra_args)

    def crash_and_relaunch(self, project: Path | None = None) -> None:
        """Kills the app with SIGKILL -- no quit, no session save, the crash path --
        and starts it again on the same display, XDG tree and project."""
        self._bank_action_counts()
        if self.alive():
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait(timeout=5)
        for line in self.new_log_lines():
            if is_finding_line(line):
                self.findings.append(Finding("log", line.strip()))
        if self._socket is not None:
            with contextlib.suppress(OSError):
                self._socket.close()
            self._socket = None
        self.log_offset = 0
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
        self._keys_sent: list[str] = []  # what the presses were, for a timeout's note
        self._typed_log_offset = 0
        args = [str(self.binary)]
        if project is not None:
            args.append(str(project))
        args += ["--set", "control.enabled", "true", *extra_args]
        with open(self.log_path, "wb") as log:
            self.proc = subprocess.Popen(args, env=self.env, stdout=log, stderr=subprocess.STDOUT,
                                         start_new_session=True)
        _wait(lambda: self._control_ready() or not self.alive(), 20.0,
              "the control channel never came up")
        if not self.alive():
            raise QaFailure(f"microide exited at startup; log: {self.log_path}")
        self.window = self._find_window()
        self.origin = self._window_origin()
        self.settle(0.6)

    def _start_xvfb(self, w: int, h: int) -> None:
        """Xvfb picks a free display and tells us (-displayfd); scanning lock
        files raced when two drivers started at once on one machine."""
        xkb = plain_fkey_xkb_dir()
        xkb_args = ["-xkbdir", str(xkb)] if xkb is not None else []
        if self.display:
            self.xvfb = subprocess.Popen(
                ["Xvfb", self.display, *xkb_args, "-screen", "0", f"{w}x{h}x24", "-nolisten", "tcp"],
                env=self.tool_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            read_fd, write_fd = os.pipe()
            self.xvfb = subprocess.Popen(
                ["Xvfb", "-displayfd", str(write_fd), *xkb_args, "-screen", "0", f"{w}x{h}x24",
                 "-nolisten", "tcp"],
                env=self.tool_env, pass_fds=(write_fd,), stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL)
            os.close(write_fd)
            with os.fdopen(read_fd) as reader:
                number = reader.readline().strip()
            if not number.isdigit():
                raise QaFailure("Xvfb did not report a display number")
            self.display = f":{number}"
        _wait(lambda: subprocess.run(["xdpyinfo", "-display", self.display], env=self.tool_env,
                                     capture_output=True).returncode == 0,
              5.0, "Xvfb never accepted connections")

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
        if self._socket is not None:
            with contextlib.suppress(OSError):
                self._socket.close()
            self._socket = None
        for proc in (self.proc, self.xvfb):
            if proc is not None and proc.poll() is None:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(proc.pid, signal.SIGTERM) if proc is self.proc else proc.terminate()
                with contextlib.suppress(subprocess.TimeoutExpired):
                    proc.wait(timeout=5)
                if proc.poll() is None:
                    proc.kill()
        if os.environ.get("MQA_KEEP_WORK") != "1":
            shutil.rmtree(self.work, ignore_errors=True)

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

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
        self._await_typed_input()
        if self._socket is None:
            listing = _last_json(self._control(["control-list"]).stdout) or {}
            path = listing.get("socket")
            if not path:
                raise QaFailure("no control socket to connect to")
            self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._socket.connect(path)
        request_id = self._next_id
        self._next_id += 1
        self._socket.settimeout(timeout)
        try:
            self._socket.sendall(json.dumps({"id": request_id, "command": line}).encode() + b"\n")
            while True:
                newline = self._socket_buffer.find(b"\n")
                if newline < 0:
                    chunk = self._socket.recv(65536)
                    if not chunk:
                        raise QaFailure("the app closed the control connection")
                    self._socket_buffer += chunk
                    continue
                raw, self._socket_buffer = (self._socket_buffer[:newline],
                                            self._socket_buffer[newline + 1:])
                with contextlib.suppress(json.JSONDecodeError):
                    reply = json.loads(raw)
                    if isinstance(reply, dict) and reply.get("id") == request_id:
                        return reply
        except (OSError, socket.timeout) as error:
            raise QaFailure(f"command {line!r} got no reply: {error}") from error

    def query(self, verb: str, args: dict[str, Any] | None = None) -> dict[str, Any]:
        command = ["control-send", "--query", verb]
        if args is not None:
            command += ["--args", json.dumps(args)]
        result = self._control(command)
        reply = _last_json(result.stdout)
        if not reply or not reply.get("ok"):
            raise QaFailure(f"query {verb!r} failed: {result.stdout or result.stderr}")
        if verb == "ui" and "\ufffd" in result.stdout:
            self._check_chrome_utf8(reply.get("result", {}))
        return reply.get("result", {})

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
        """Waits for the element called `name` to be on screen."""
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

    def _find_window(self) -> str | None:
        if self.wayland:
            return None
        # By the app's pid (its _NET_WM_PID), else its WM_CLASS -- never by title:
        # the title names the active file ("● main.cpp — taskflow — microide"), and
        # xdotool, in the C locale, cannot even match a UTF-8 one.
        searches = [["--class", "microide"]]
        if self.proc is not None:
            searches.insert(0, ["--pid", str(self.proc.pid)])
        for _ in range(25):
            for search in searches:
                result = subprocess.run(["xdotool", "search", *search], env=self.tool_env,
                                        capture_output=True, text=True)
                ids = result.stdout.split()
                if ids:
                    return ids[-1]
            time.sleep(0.2)
        return None

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
        if self.window:
            self._xdotool("windowfocus", "--sync", self.window)

    def _input(self, *words: str) -> dict[str, Any]:
        """One input command (`key`, `pointer`, `type`) over the persistent control
        connection: a process per keystroke made typing slow enough on the Wayland
        backend for a toast to expire between two words of a palette query."""
        def quote(word: str) -> str:
            if word and not any(c.isspace() or c in "\"'\\" for c in word):
                return word
            return '"' + word.replace("\\", "\\\\").replace('"', '\\"') + '"'
        try:
            return self.send_line(" ".join(quote(w) for w in words), record=False)
        except QaFailure as failure:
            return {"ok": False, "error": str(failure)}

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
        self._expect_pointer_input(motion=(x, y))
        self._xdotool("mousemove", *self._root(x, y))

    def click(self, x: float, y: float, button: int = 1, clicks: int = 1,
              modifiers: tuple[str, ...] = ()) -> None:
        """A press at window (x, y); `modifiers` are xdotool key names held for it
        ("alt", "shift", "ctrl")."""
        self._await_click_gap(x, y)
        self._record({"click": [round(x), round(y)], "button": button, "clicks": clicks,
                      "modifiers": list(modifiers)})
        if self.wayland:
            self._pointer("click", x, y, button, clicks, modifiers=modifiers)
            self._last_click = (time.monotonic(), x, y)
            return
        for key in modifiers:
            self._xdotool("keydown", key)
        self._expect_pointer_input(buttons=2 * clicks, at=(x, y))
        self._xdotool("mousemove", *self._root(x, y), "click", "--repeat", str(clicks),
                      "--delay", "60", str(button))
        for key in reversed(modifiers):
            self._xdotool("keyup", key)
        self._last_click = (time.monotonic(), x, y)

    def _await_click_gap(self, x: float, y: float) -> None:
        """SDL folds two presses within its double-click time and radius into a
        double click (a word selection in the editor), so a separate press right
        after another near it waits that window out first. Near the last PRESS:
        SDL measures from where the button went down, not where it came up, so a
        drag records its start -- recording its end let a double-click drag that
        began where the previous one had become its third click, a line selection
        (QA word-and-line-drags, on CI)."""
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
        for key in modifiers:
            self._xdotool("keydown", key)
        # Every press and release but the last, then the move onto (x1, y1), then
        # the release: `before_release` may query the app with the button held.
        self._expect_pointer_input(buttons=2 * clicks - 1, at=(x0, y0))
        if clicks > 1:
            self._xdotool("mousemove", *self._root(x0, y0), "click", "--repeat", str(clicks - 1),
                          "--delay", "60", "1")
        self._xdotool("mousemove", *self._root(x0, y0), "mousedown", "1")
        for i in range(1, steps + 1):
            x = x0 + (x1 - x0) * i / steps
            y = y0 + (y1 - y0) * i / steps
            if i == steps:
                self._expect_pointer_input(motion=(x, y))
            self._xdotool("mousemove", *self._root(x, y))
            time.sleep(0.02)
        if before_release is not None:
            before_release()
        self._expect_pointer_input(buttons=1)
        self._xdotool("mouseup", "1")
        for key in reversed(modifiers):
            self._xdotool("keyup", key)
        self._last_click = (time.monotonic(), x0, y0)

    def drop(self, x: float, y: float, path: Path | str) -> None:
        """A file dragged in from another application and released at (x, y). Through
        the control channel on every backend: X cannot synthesize a drop either."""
        self._record({"drop": [round(x), round(y)], "path": str(path)})
        self._pointer("drop", x, y, str(path))

    def wheel(self, x: float, y: float, notches: int, modifiers: tuple[str, ...] = ()) -> None:
        """`notches` > 0 scrolls down; `modifiers` (xdotool key names) are held for it."""
        self._record({"wheel": [round(x), round(y)], "notches": notches, "modifiers": list(modifiers)})
        if self.wayland:
            self._pointer("wheel", x, y, notches, modifiers=modifiers)
            return
        button = "5" if notches > 0 else "4"
        self._expect_pointer_input(wheels=abs(notches), at=(x, y))
        self._xdotool("mousemove", *self._root(x, y))
        for key in modifiers:
            self._xdotool("keydown", key)
        self._xdotool("click", "--repeat", str(abs(notches)), "--delay", "20", button)
        for key in reversed(modifiers):
            self._xdotool("keyup", key)

    def key(self, chord: str) -> None:
        """An xdotool chord: "ctrl+shift+f", "Escape", "alt+Up"; a two-step chord
        is its steps separated by a space: "ctrl+k ctrl+o"."""
        self._record({"key": chord})
        steps = chord.split()
        if self.wayland:
            for step in steps:
                answer = self._input("key", chord_for_key_command(step))
                if not answer.get("ok", False):
                    self.expect(False, f"key {step} (of {chord}) was refused: {answer}")
                    return
            return
        self._focus()
        self._expect_typed_input("", len(steps), chord)
        self._xdotool("key", "--clearmodifiers", *steps)

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
        self._xdotool("keydown", "--clearmodifiers", modifier)
        for chord in chords:
            self._xdotool("key", chord)
        self._xdotool("keyup", modifier)

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

    def _expect_pointer_input(self, buttons: int = 0, wheels: int = 0,
                              motion: tuple[float, float] | None = None,
                              at: tuple[float, float] | None = None) -> None:
        """Notes pointer input about to go through X, as _expect_typed_input does
        keys: `buttons` press and release events, `wheels` notches, and a `motion`
        that must be seen to land. A click answered before the app handled it is
        the same race as a key: Discard All clicked, then the prompt read at once,
        found none on the slow ASan build (QA journey-rename-refactor). A move
        waits only when the pointer is not already there (X sends no motion
        then) and the point is inside the window. `at` is where the input leaves
        the pointer, for the next move to compare with."""
        if self.wayland or self.log_path is None:
            return
        self._expect_typed_input("", 0)
        self._buttons_pending += buttons
        self._wheels_pending += wheels
        if motion is not None:
            target = (round(motion[0]), round(motion[1]))
            w, h = self.size
            if self._pointer_at is None:
                self._pointer_at = self._x_pointer_location()
            if target != self._pointer_at and 0 <= target[0] < w and 0 <= target[1] < h:
                self._motion_pending = target
            at = motion
        if at is not None:
            self._pointer_at = (round(at[0]), round(at[1]))

    def _x_pointer_location(self) -> tuple[int, int] | None:
        """Where X has the pointer now, in window coordinates."""
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
                    f"{taken_buttons} of {self._buttons_pending} button events, "
                    f"{taken_wheels} of {self._wheels_pending} wheel notches"
                    + ("" if moved else f", no motion to {self._motion_pending}"))
            self._record({"note": note})
            print(f"        note: {note}", flush=True)
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
        args = ["type", "--clearmodifiers", "--delay", "25"]
        # Beyond ASCII, xdotool remaps a scratch keycode per character and SDL races
        # the keymap change, so a run of them drops characters; Latin-1 goes as its
        # own keysym press instead. Past Latin-1 there is no way in: Xvfb has no
        # input method, and without one X delivers no text for such a keysym (a real
        # desktop's IME does) -- those characters are inserted through the control
        # channel's `type` (the paste path), and the action log says so.
        key_args = ["key", "--clearmodifiers"]
        chunk = ""

        def flush() -> None:
            nonlocal chunk
            if chunk:
                self._expect_typed_input(chunk, len(chunk))
                self._xdotool(*args, "--", chunk)
                chunk = ""
        for char in text:
            if char in "\n\t":
                flush()
                self._expect_typed_input("", 1)
                self._xdotool(*key_args, "Return" if char == "\n" else "Tab")
            elif ord(char) < 0x80:
                chunk += char
            elif ord(char) <= 0xFF:
                flush()
                time.sleep(0.08)  # let SDL see the ASCII run's keymap settle first
                self._expect_typed_input(char, 1)
                self._xdotool(*key_args, "U%04X" % ord(char))
                time.sleep(0.08)
            else:
                flush()
                self._record({"type-via-control": char})
                self._control(["control-send", "type", char])
        flush()

    def center(self, element: dict[str, Any]) -> tuple[float, float]:
        x, y, w, h = element["rect"]
        return x + w / 2, y + h / 2

    def click_named(self, name: str, **kwargs: Any) -> dict[str, Any]:
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

    def grab(self):
        if self.wayland:
            # No screen reader on a headless weston without its debug protocols:
            # evidence is the frame trace and the redraw verifier's own dumps.
            return Image.new("RGB", self.size, (0, 0, 0))
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
        for line in self.recent_log:
            if is_finding_line(line):
                found.append(Finding("log", line.strip()))
        if not self.alive():
            code = self.proc.returncode if self.proc else None
            found.append(Finding("crash", f"microide exited ({code}) after {label or 'last action'}"))
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
                self.recent_shots.append((len(self.actions), self.grab()))
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
            return []
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
        if width and height:
            for element in ui.get("elements", []):
                x, y, w, h = element["rect"]
                if x < -1 or y < -1 or x + w > width + 1 or y + h > height + 1:
                    out.append(("bounds", f"{element['name']} {element['rect']} leaves the {width}x{height} window"))
                    break
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
        found = findings(ui)
        for _ in range(3):
            if not found:
                return []
            time.sleep(0.2 * SLOW_FACTOR)
            try:
                ui = self.ui()
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
            return shown_label == label or (shown_label.endswith("…") and label.startswith(shown_label[:-1]))
        out: list[tuple[str, str]] = []
        for tab in tabs:
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
            groups = self.editor_state().get("groups", [])
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
                    # untitled buffer), the comparison's own name.
                    right = title.rsplit("↔", 1)[-1].strip()
                    named = right.startswith(shown) or shown.startswith(right) or shown == title
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
        overlay = ui.get("overlay") or {}
        query = overlay.get("query") or ""
        if (overlay.get("visible") and overlay.get("mode") in ("buffer-search", "buffer-replace")
                and text is not None and len(query) >= 2 and query.isascii() and query.isalnum()
                and not (elements.get("button:find/regex") or {}).get("active")
                and not (elements.get("button:find/whole-word") or {}).get("active")
                and isinstance(overlay.get("count"), int) and overlay["count"] < 1000):
            haystack, needle = (text, query) if (elements.get("button:find/match-case") or {}).get("active") \
                else (text.lower(), query.lower())
            if scope := overlay.get("scope"):
                # Find in Selection counts inside the scope (1-based line, byte column).
                lines = haystack.encode().split(b"\n")
                def offset(at: list[int]) -> int:
                    line, column = at[0] - 1, at[1] - 1
                    return sum(len(lines[i]) + 1 for i in range(min(line, len(lines)))) + column
                haystack = haystack.encode()[offset(scope["start"]):offset(scope["end"])].decode("utf-8", "replace")
            want = haystack.count(needle)
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
        for index, image in self.recent_shots:
            image.save(folder / f"after-action-{index:04d}.png")
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
        if not self.alive():
            return None
        path = folder / "now.png"
        self.grab().save(path)
        return str(path)

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
            shot = self.screenshot("expect-" + message[:40])
            if shot is not None:
                with contextlib.suppress(Exception):
                    recent = json.dumps(self.actions[-40:], indent=1)
                    dap = self.work / "dap.log"
                    dap_tail = dap.read_bytes()[-24 * 1024:].decode("utf-8", "replace") if dap.exists() else ""
                    Path(shot).with_suffix(".txt").write_text(
                        f"{message}\n\n--- the last actions\n{recent}\n\n--- the app log's tail\n{self.log_tail()}"
                        + (f"\n\n--- the debug adapter's traffic (tail)\n{dap_tail}" if dap_tail else ""))
            self.findings.append(Finding("expect", message, shot))
        return bool(condition)

    # ---- replay ----------------------------------------------------------

    def _record(self, action: dict[str, Any]) -> None:
        action["t"] = round(time.monotonic(), 3)
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
            b = other["rect"]
            ix = min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0])
            iy = min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1])
            if ix > 1 and iy > 1 and not inside(a, b, 0) and not inside(b, a, 0):
                out.append(("overlap", f"{element['name']} {a} overlaps {other['name']} {b}"))
                return out
        seen.append(element)
    return out


def replay(app: App, actions: list[dict[str, Any]], pause: float = 0.12,
           churn: Callable[[dict[str, Any]], None] | None = None) -> None:
    """Re-runs a recorded action list (what the monkey saves on a finding).

    `churn` re-applies a row the monkey's ExternalChurn recorded: a file rewrite
    that landed between two commands, with no settle or check of its own."""
    for action in actions:
        if "check" in action:
            continue
        if "churn" in action:
            if churn is not None:
                churn(action)
            continue
        if "send" in action:
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
        app.settle(pause)
        app.check(json.dumps(action))


def make_fixture(dest: Path) -> Path:
    """The deterministic C++ demo repo the media pipeline uses."""
    subprocess.run(["bash", str(CAPTURE / "make-fixture.sh"), str(dest)], check=True,
                   capture_output=True)
    return dest


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
