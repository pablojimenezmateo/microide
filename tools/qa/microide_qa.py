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
import os
import random
import shutil
import signal
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator

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
)


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
    # Evidence for a finding: the window after each of the last few actions, and
    # the app's frame-trace lines since the previous check.
    recent_shots: collections.deque = field(default_factory=lambda: collections.deque(maxlen=3))
    recent_log: list[str] = field(default_factory=list)

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
               keep_home: bool = False) -> Iterator["App"]:
        binary = Path(binary or os.environ.get("MICROIDE_BIN") or REPO / "build/microide/microide")
        if not binary.exists():
            raise FileNotFoundError(f"no microide binary at {binary} (set MICROIDE_BIN)")
        # Short path: the control socket lives under XDG_RUNTIME_DIR and AF_UNIX
        # paths stop at 108 bytes.
        work = Path(tempfile.mkdtemp(prefix="mqa-", dir="/tmp"))
        env = dict(os.environ)
        env.update({
            "SDL_VIDEODRIVER": "x11",
            # One SDL_Log line per frame and per clip under an open menu: the frame
            # sequence a finding is read against.
            "MICROIDE_TRACE_REDRAW_VERBOSE": "1",
            "MICROIDE_TRACE_MENU_HOVER": "1",
            "XDG_CONFIG_HOME": str(work / "config"),
            "XDG_STATE_HOME": str(work / "state"),
            "XDG_DATA_HOME": str(work / "data"),
            "XDG_RUNTIME_DIR": str(work / "run"),
            "MICROIDE_QA_VERIFY_REDRAW": "1",
        })
        if not keep_home:
            # A remote run needs the real HOME for ssh keys and config; anything
            # else gets a throwaway one so the embedded shell is predictable.
            env["HOME"] = str(work)
        env.update(extra_env or {})
        for sub in ("config", "state", "data", "run"):
            (work / sub).mkdir(parents=True, exist_ok=True)
        os.chmod(work / "run", 0o700)
        app = cls(binary=binary, work=work, display=display or "", size=size, env=env, shots=shots)
        try:
            app._start(project, extra_args or [])
            yield app
        finally:
            app.stop()

    def _start(self, project: Path | None, extra_args: list[str]) -> None:
        w, h = self.size
        self._start_xvfb(w, h)
        self.env["DISPLAY"] = self.display
        self.log_path = self.work / "microide.log"
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
        self.settle(0.6)

    def _start_xvfb(self, w: int, h: int) -> None:
        """Xvfb picks a free display and tells us (-displayfd); scanning lock
        files raced when two drivers started at once on one machine."""
        if self.display:
            self.xvfb = subprocess.Popen(
                ["Xvfb", self.display, "-screen", "0", f"{w}x{h}x24", "-nolisten", "tcp"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            read_fd, write_fd = os.pipe()
            self.xvfb = subprocess.Popen(
                ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", f"{w}x{h}x24",
                 "-nolisten", "tcp"],
                pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.close(write_fd)
            with os.fdopen(read_fd) as reader:
                number = reader.readline().strip()
            if not number.isdigit():
                raise QaFailure("Xvfb did not report a display number")
            self.display = f":{number}"
        _wait(lambda: subprocess.run(["xdpyinfo", "-display", self.display], env=self.env,
                                     capture_output=True).returncode == 0,
              5.0, "Xvfb never accepted connections")

    def stop(self) -> None:
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
        return subprocess.run([str(self.binary), *args], env=self.env, capture_output=True,
                              text=True, timeout=timeout)

    def _control_ready(self) -> bool:
        result = self._control(["control-list"], timeout=3.0)
        return result.returncode == 0 and bool(result.stdout.strip())

    def send(self, *words: str) -> dict[str, Any]:
        """Runs a control command; returns the reply. Records it for replay."""
        self._record({"send": list(words)})
        result = self._control(["control-send", *words])
        return _last_json(result.stdout) or {"ok": False, "raw": result.stdout + result.stderr}

    def query(self, verb: str) -> dict[str, Any]:
        result = self._control(["control-send", "--query", verb])
        reply = _last_json(result.stdout)
        if not reply or not reply.get("ok"):
            raise QaFailure(f"query {verb!r} failed: {result.stdout or result.stderr}")
        return reply.get("result", {})

    def ui(self) -> dict[str, Any]:
        """Named on-screen elements and focus/overlay/menu state (the `ui` query)."""
        return self.query("ui")

    def element(self, name: str) -> dict[str, Any]:
        """The element called `name` in the ui query (e.g. "menu:Edit", "tab:main.cpp")."""
        for element in self.ui().get("elements", []):
            if element.get("name") == name:
                return element
        raise QaFailure(f"no element named {name!r} on screen")

    # ---- input -----------------------------------------------------------

    def _xdotool(self, *args: str) -> None:
        subprocess.run(["timeout", "5", "xdotool", *args], env=self.env, capture_output=True)

    def _find_window(self) -> str | None:
        for _ in range(25):
            result = subprocess.run(["xdotool", "search", "--name", "microide"], env=self.env,
                                    capture_output=True, text=True)
            ids = result.stdout.split()
            if ids:
                return ids[-1]
            time.sleep(0.2)
        return None

    def _focus(self) -> None:
        if self.window:
            self._xdotool("windowfocus", self.window)

    def move(self, x: float, y: float) -> None:
        self._record({"move": [round(x), round(y)]})
        self._xdotool("mousemove", str(round(x)), str(round(y)))

    def click(self, x: float, y: float, button: int = 1, clicks: int = 1) -> None:
        self._record({"click": [round(x), round(y)], "button": button, "clicks": clicks})
        self._xdotool("mousemove", str(round(x)), str(round(y)), "click", "--repeat", str(clicks),
                      "--delay", "60", str(button))

    def drag(self, x0: float, y0: float, x1: float, y1: float, steps: int = 8) -> None:
        self._record({"drag": [round(x0), round(y0), round(x1), round(y1)]})
        self._xdotool("mousemove", str(round(x0)), str(round(y0)), "mousedown", "1")
        for i in range(1, steps + 1):
            x = x0 + (x1 - x0) * i / steps
            y = y0 + (y1 - y0) * i / steps
            self._xdotool("mousemove", str(round(x)), str(round(y)))
            time.sleep(0.02)
        self._xdotool("mouseup", "1")

    def wheel(self, x: float, y: float, notches: int) -> None:
        self._record({"wheel": [round(x), round(y)], "notches": notches})
        button = "5" if notches > 0 else "4"
        self._xdotool("mousemove", str(round(x)), str(round(y)), "click", "--repeat",
                      str(abs(notches)), "--delay", "20", button)

    def key(self, chord: str) -> None:
        """An xdotool chord: "ctrl+shift+f", "Escape", "alt+Up"."""
        self._record({"key": chord})
        self._focus()
        args = ["key", "--clearmodifiers"]
        if self.window:
            args += ["--window", self.window]
        self._xdotool(*args, chord)

    def type(self, text: str) -> None:
        self._record({"type": text})
        self._focus()
        args = ["type", "--clearmodifiers", "--delay", "25"]
        if self.window:
            args += ["--window", self.window]
        self._xdotool(*args, "--", text)

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
        xwd = subprocess.run(["xwd", "-display", self.display, "-root", "-silent"],
                             capture_output=True, env=self.env)
        import io
        return _XWD.convert(io.BytesIO(xwd.stdout))

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
            if any(marker in line for marker in FINDING_MARKERS):
                found.append(Finding("log", line.strip()))
        if not self.alive():
            code = self.proc.returncode if self.proc else None
            found.append(Finding("crash", f"microide exited ({code}) after {label or 'last action'}"))
        if found:
            shot = self._write_finding(label, found)
            for finding in found:
                finding.screenshot = shot
            self.findings.extend(found)
        if self.alive():
            with contextlib.suppress(Exception):
                self.recent_shots.append((len(self.actions), self.grab()))
        return found

    def _write_finding(self, label: str, found: list[Finding]) -> str | None:
        """A finding is a directory: the actions up to it, the frame lines since
        the previous action, the last screenshots and the window now."""
        if self.shots is None:
            return None
        folder = self.shots / f"finding-{len(self.findings) + 1:02d}-{_slug(label)}"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "actions.json").write_text(json.dumps(self.actions, indent=1))
        (folder / "findings.txt").write_text("\n".join(f"{f.kind}: {f.detail}" for f in found) + "\n")
        (folder / "trace.txt").write_text("\n".join(self.recent_log) + "\n")
        for index, image in self.recent_shots:
            image.save(folder / f"after-action-{index:04d}.png")
        if not self.alive():
            return None
        path = folder / "now.png"
        self.grab().save(path)
        return str(path)

    def expect(self, condition: bool, message: str) -> None:
        if not condition:
            shot = self.screenshot("expect-" + message[:40])
            self.findings.append(Finding("expect", message, shot))

    # ---- replay ----------------------------------------------------------

    def _record(self, action: dict[str, Any]) -> None:
        action["t"] = round(time.monotonic(), 3)
        self.actions.append(action)

    def save_actions(self, path: Path) -> None:
        path.write_text(json.dumps(self.actions, indent=1))


def replay(app: App, actions: list[dict[str, Any]], pause: float = 0.12) -> None:
    """Re-runs a recorded action list (what the monkey saves on a finding)."""
    for action in actions:
        if "send" in action:
            app.send(*action["send"])
        elif "move" in action:
            app.move(*action["move"])
        elif "click" in action:
            app.click(*action["click"], button=action.get("button", 1),
                      clicks=action.get("clicks", 1))
        elif "drag" in action:
            app.drag(*action["drag"])
        elif "wheel" in action:
            app.wheel(*action["wheel"], action["notches"])
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
    deadline = time.monotonic() + timeout
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
