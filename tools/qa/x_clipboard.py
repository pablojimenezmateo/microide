"""Another application's clipboard, for the QA driver (App.foreign_clipboard).

Run as `python3 x_clipboard.py`, with DISPLAY set and the text on stdin: it takes
the X CLIPBOARD selection, prints "ready" once it owns it, and answers every
paste request (TARGETS, UTF8_STRING, TEXT, text/plain, and STRING as Latin-1)
until something else takes the selection -- the app copying, or the next
foreign clipboard -- and then exits. Its display connection closing (Xvfb gone)
ends it too: Xlib's I/O error handler exits the process.

The app's own copy and paste only ever round-tripped its own text. What another
application leaves on the clipboard -- CRLF, a NUL-free but enormous line, tabs,
text only in STRING -- never reached it (tools/soak/surface-extra.toml,
"clipboard set by other applications"). There is no xclip or xsel on the soak
machine, and no tkinter: plain Xlib through ctypes, as tools/qa/xtest.py does.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import sys

Window = Atom = Time = ctypes.c_ulong

SELECTION_CLEAR = 29
SELECTION_REQUEST = 30
SELECTION_NOTIFY = 31
PROP_MODE_REPLACE = 0
XA_ATOM = 4
XA_STRING = 31


class XSelectionRequestEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("owner", Window), ("requestor", Window),
                ("selection", Atom), ("target", Atom), ("property", Atom), ("time", Time)]


class XSelectionEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("requestor", Window), ("selection", Atom),
                ("target", Atom), ("property", Atom), ("time", Time)]


class XEvent(ctypes.Union):
    _fields_ = [("type", ctypes.c_int), ("xselectionrequest", XSelectionRequestEvent),
                ("xselection", XSelectionEvent), ("pad", ctypes.c_long * 24)]


def _x11() -> ctypes.CDLL:
    x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XInternAtom.restype = Atom
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XDefaultRootWindow.restype = Window
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XCreateSimpleWindow.restype = Window
    x11.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, Window, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                        ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_ulong]
    x11.XSetSelectionOwner.argtypes = [ctypes.c_void_p, Atom, Window, Time]
    x11.XGetSelectionOwner.restype = Window
    x11.XGetSelectionOwner.argtypes = [ctypes.c_void_p, Atom]
    x11.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.POINTER(XEvent)]
    x11.XChangeProperty.argtypes = [ctypes.c_void_p, Window, Atom, Atom, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_void_p, ctypes.c_int]
    x11.XSendEvent.argtypes = [ctypes.c_void_p, Window, ctypes.c_int, ctypes.c_long, ctypes.POINTER(XEvent)]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    return x11


def serve(text: str) -> int:
    x11 = _x11()
    dpy = x11.XOpenDisplay(None)
    if not dpy:
        print("no display", flush=True)
        return 1
    atom = lambda name: x11.XInternAtom(dpy, name.encode(), 0)  # noqa: E731
    clipboard, targets_atom, utf8 = atom("CLIPBOARD"), atom("TARGETS"), atom("UTF8_STRING")
    text_atoms = {utf8, atom("TEXT"), atom("text/plain;charset=utf-8"), atom("text/plain")}
    window = x11.XCreateSimpleWindow(dpy, x11.XDefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0)
    x11.XSetSelectionOwner(dpy, clipboard, window, 0)  # CurrentTime
    if x11.XGetSelectionOwner(dpy, clipboard) != window:
        print("not the owner", flush=True)
        return 1
    print("ready", flush=True)
    utf8_bytes = text.encode("utf-8")
    latin1_bytes = text.encode("latin-1", errors="replace")
    event = XEvent()
    while True:
        x11.XNextEvent(dpy, ctypes.byref(event))
        if event.type == SELECTION_CLEAR:
            return 0  # someone else owns the clipboard now
        if event.type != SELECTION_REQUEST:
            continue
        request = event.xselectionrequest
        prop = request.property or request.target  # an obsolete client names no property
        if request.target == targets_atom:
            offered = (Atom * 5)(targets_atom, utf8, atom("TEXT"), atom("text/plain;charset=utf-8"), XA_STRING)
            x11.XChangeProperty(dpy, request.requestor, prop, XA_ATOM, 32, PROP_MODE_REPLACE, offered, 5)
        elif request.target in text_atoms or request.target == XA_STRING:
            data = latin1_bytes if request.target == XA_STRING else utf8_bytes
            kind = XA_STRING if request.target == XA_STRING else utf8
            buffer = ctypes.create_string_buffer(data, len(data))
            x11.XChangeProperty(dpy, request.requestor, prop, kind, 8, PROP_MODE_REPLACE, buffer, len(data))
        else:
            prop = 0  # refused: a target this owner does not offer
        reply = XEvent()
        reply.xselection.type = SELECTION_NOTIFY
        reply.xselection.requestor = request.requestor
        reply.xselection.selection = request.selection
        reply.xselection.target = request.target
        reply.xselection.property = prop
        reply.xselection.time = request.time
        x11.XSendEvent(dpy, request.requestor, 0, 0, ctypes.byref(reply))
        x11.XFlush(dpy)


if __name__ == "__main__":
    sys.exit(serve(sys.stdin.read()))
