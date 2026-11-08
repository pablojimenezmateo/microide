"""In-process X input through the XTEST extension (ctypes over libX11/libXtst).

The driver used to spawn `timeout 5 xdotool ...` for every pointer move, click and
key: ~30 ms for xdotool itself and ~100 ms more for `timeout`, so input was over
half of a monkey step (profiled 2026-11-08: 91 calls, 10.2 s of a 23 s run). The
same XTEST requests sent from this process cost a round trip each.

The events are the ones xdotool sends -- XTestFakeMotionEvent/ButtonEvent/KeyEvent,
which the server delivers as device input, so SDL reads them through XInput2 the
way it reads a keyboard (never XSendEvent, see App._focus). What xdotool did on
top is redone here: keysym -> keycode with the Shift level a character needs, and
focusing the window and waiting until it has the focus.

One `Display` per App, and XInitThreads first: the suite drives several apps from
a thread pool, each on its own display connection.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import time

_x11 = _xtst = None


class _SizeHints(ctypes.Structure):
    """Xlib's XSizeHints: what a window asks of the window manager."""
    _fields_ = [("flags", ctypes.c_long), ("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int),
                ("height", ctypes.c_int), ("min_width", ctypes.c_int), ("min_height", ctypes.c_int),
                ("max_width", ctypes.c_int), ("max_height", ctypes.c_int), ("width_inc", ctypes.c_int),
                ("height_inc", ctypes.c_int), ("min_aspect_x", ctypes.c_int), ("min_aspect_y", ctypes.c_int),
                ("max_aspect_x", ctypes.c_int), ("max_aspect_y", ctypes.c_int), ("base_width", ctypes.c_int),
                ("base_height", ctypes.c_int), ("win_gravity", ctypes.c_int)]


P_MIN_SIZE = 1 << 4
P_MAX_SIZE = 1 << 5


def _load() -> bool:
    global _x11, _xtst
    if _x11 is not None:
        return _xtst is not None
    try:
        _x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        _xtst = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")
    except OSError:
        _x11 = False  # type: ignore[assignment]
        _xtst = None
        return False
    _x11.XInitThreads()
    _x11.XOpenDisplay.restype = ctypes.c_void_p
    _x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    _x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    _x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _x11.XFlush.argtypes = [ctypes.c_void_p]
    _x11.XStringToKeysym.restype = ctypes.c_ulong
    _x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
    _x11.XKeysymToKeycode.restype = ctypes.c_ubyte
    _x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    _x11.XkbKeycodeToKeysym.restype = ctypes.c_ulong
    _x11.XkbKeycodeToKeysym.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_int, ctypes.c_int]
    _x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    _x11.XGetInputFocus.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong),
                                    ctypes.POINTER(ctypes.c_int)]
    _x11.XDefaultRootWindow.restype = ctypes.c_ulong
    _x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    _x11.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + \
        [ctypes.POINTER(ctypes.c_ulong)] * 2 + [ctypes.POINTER(ctypes.c_int)] * 4 + \
        [ctypes.POINTER(ctypes.c_uint)]
    _x11.XRefreshKeyboardMapping.argtypes = [ctypes.c_void_p]
    _x11.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
    _x11.XGetWMNormalHints.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(_SizeHints),
                                       ctypes.POINTER(ctypes.c_long)]
    _xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_int, ctypes.c_ulong]
    _xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    _xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    return True


# xdotool's modifier spellings, as keysyms.
_MODIFIERS = {"ctrl": "Control_L", "control": "Control_L", "shift": "Shift_L", "alt": "Alt_L",
              "super": "Super_L", "meta": "Meta_L"}
REVERT_TO_PARENT = 2
# Between two key presses. SDL's X11 backend reads a release followed by a press
# of the same keycode less than 2 ms (server time) later as auto-repeat
# (X11_KeyRepeat), so "xxxx" sent back to back arrived as one press and three
# repeats -- the text was right and every key count was not.
KEY_GAP = 0.004


class XTest:
    """XTEST input on one display. `available()` is False when libXtst is missing
    or the display cannot be opened; the App then falls back to xdotool."""

    def __init__(self, display: str):
        self._dpy = None
        # MQA_XTEST=0 drives the display through xdotool instead (a comparison
        # when an input-dependent flow behaves differently).
        if os.environ.get("MQA_XTEST", "1") != "0" and _load():
            self._dpy = _x11.XOpenDisplay(display.encode())
        self._keycodes: dict[str, tuple[int, bool]] = {}

    def available(self) -> bool:
        return bool(self._dpy)

    def close(self) -> None:
        if self._dpy:
            _x11.XCloseDisplay(self._dpy)
            self._dpy = None

    # ---- pointer -----------------------------------------------------------

    def move(self, x: int, y: int) -> None:
        _xtst.XTestFakeMotionEvent(self._dpy, -1, int(x), int(y), 0)
        _x11.XSync(self._dpy, 0)

    def button(self, button: int, down: bool) -> None:
        _xtst.XTestFakeButtonEvent(self._dpy, int(button), 1 if down else 0, 0)
        _x11.XSync(self._dpy, 0)

    def click(self, button: int, repeat: int = 1, delay: float = 0.03) -> None:
        for index in range(repeat):
            if index:
                time.sleep(delay)
            self.button(button, True)
            self.button(button, False)

    def pointer(self) -> tuple[int, int] | None:
        root = _x11.XDefaultRootWindow(self._dpy)
        r, c = ctypes.c_ulong(), ctypes.c_ulong()
        rx, ry, wx, wy = (ctypes.c_int() for _ in range(4))
        mask = ctypes.c_uint()
        if not _x11.XQueryPointer(self._dpy, root, ctypes.byref(r), ctypes.byref(c), ctypes.byref(rx),
                                  ctypes.byref(ry), ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask)):
            return None
        return rx.value, ry.value

    # ---- keyboard ----------------------------------------------------------

    def refresh_keymap(self) -> None:
        """After the keymap was changed from outside (xmodmap): forget what was
        looked up and have Xlib re-read the mapping."""
        self._keycodes.clear()

    def keycode(self, name: str) -> tuple[int, bool] | None:
        """The keycode for keysym `name` ("a", "A", "Return", "0xe9", "plus") and
        whether Shift reaches it; None when no key carries it."""
        cached = self._keycodes.get(name)
        if cached is not None:
            return cached
        if name.startswith("0x"):
            keysym = int(name, 16)
        elif len(name) == 1 and 0x20 <= ord(name) <= 0x7E:
            keysym = ord(name)  # an ASCII character's keysym is its code
        else:
            keysym = _x11.XStringToKeysym(name.encode())
        if not keysym:
            return None
        code = _x11.XKeysymToKeycode(self._dpy, keysym)
        if not code:
            return None
        shifted = _x11.XkbKeycodeToKeysym(self._dpy, code, 0, 0) != keysym and \
            _x11.XkbKeycodeToKeysym(self._dpy, code, 0, 1) == keysym
        self._keycodes[name] = (code, shifted)
        return code, shifted

    def key_event(self, code: int, down: bool) -> None:
        _xtst.XTestFakeKeyEvent(self._dpy, code, 1 if down else 0, 0)

    def modifier(self, name: str, down: bool) -> bool:
        found = self.keycode(_MODIFIERS.get(name.lower(), name))
        if found is None:
            return False
        self.key_event(found[0], down)
        _x11.XSync(self._dpy, 0)
        return True

    def chord(self, chord: str) -> bool:
        """One xdotool-spelled chord ("ctrl+shift+Prior", "G", "plus"): modifiers
        down, the key down and up, modifiers up. False when a part has no key."""
        parts = chord.split("+") if chord != "+" else ["plus"]
        if chord.endswith("++"):
            parts = parts[:-2] + ["plus"]
        *mods, key = parts
        mod_codes = []
        for mod in mods:
            found = self.keycode(_MODIFIERS.get(mod.lower(), mod))
            if found is None:
                return False
            mod_codes.append(found[0])
        found = self.keycode(key)
        if found is None:
            return False
        code, shifted = found
        shift = self.keycode("Shift_L")
        held = list(mod_codes)
        if shifted and shift is not None and shift[0] not in held:
            held.append(shift[0])
        for mod in held:
            self.key_event(mod, True)
        self.key_event(code, True)
        self.key_event(code, False)
        for mod in reversed(held):
            self.key_event(mod, False)
        _x11.XSync(self._dpy, 0)
        return True

    def type_ascii(self, text: str, delay: float = KEY_GAP) -> str:
        """Types printable ASCII; returns the characters no key carries (the
        caller types those another way). `delay` apart: see KEY_GAP."""
        missed = ""
        for char in text:
            name = "space" if char == " " else char
            if not self.chord(name):
                missed += char
            if delay:
                time.sleep(delay)
        return missed

    # ---- window ------------------------------------------------------------

    def resize(self, window: int, width: int, height: int) -> bool:
        """Resizes as a window manager would: within the minimum and maximum the
        window's WM_NORMAL_HINTS ask for. Xvfb runs no window manager, so a raw
        XResizeWindow granted any size -- far below the app's declared minimum."""
        hints = _SizeHints()
        supplied = ctypes.c_long()
        if _x11.XGetWMNormalHints(self._dpy, window, ctypes.byref(hints), ctypes.byref(supplied)):
            if hints.flags & P_MIN_SIZE:
                width, height = max(width, hints.min_width), max(height, hints.min_height)
            if hints.flags & P_MAX_SIZE and hints.max_width > 0 and hints.max_height > 0:
                width, height = min(width, hints.max_width), min(height, hints.max_height)
        _x11.XResizeWindow(self._dpy, window, max(1, width), max(1, height))
        _x11.XSync(self._dpy, 0)
        return True

    def focus_root(self) -> None:
        """Gives the input focus to the root window: the app sees a focus loss."""
        _x11.XSetInputFocus(self._dpy, 1, REVERT_TO_PARENT, 0)  # PointerRoot
        _x11.XSync(self._dpy, 0)

    # ---- focus -------------------------------------------------------------

    def focus(self, window: int, timeout: float = 2.0) -> bool:
        """Gives `window` the input focus and waits until the server says it has
        it (xdotool's `windowfocus --sync`)."""
        focused = ctypes.c_ulong()
        revert = ctypes.c_int()
        _x11.XGetInputFocus(self._dpy, ctypes.byref(focused), ctypes.byref(revert))
        if focused.value == window:
            return True
        deadline = time.monotonic() + timeout
        while True:
            _x11.XSetInputFocus(self._dpy, window, REVERT_TO_PARENT, 0)
            _x11.XSync(self._dpy, 0)
            _x11.XGetInputFocus(self._dpy, ctypes.byref(focused), ctypes.byref(revert))
            if focused.value == window or time.monotonic() > deadline:
                return focused.value == window
            time.sleep(0.01)
