"""Finding signatures: what makes two findings the same defect.

A monkey run that keeps going after a finding sees the same stale-paint line on
every frame until something repaints it, and the soak runner sees the same
defect on many seeds. Both deduplicate by signature: the oracle's kind plus its
message with the run-specific parts (numbers, rects, paths, frame ids, quoted
text) masked, and for a crash the first frames of the stack that are ours.

Shared by tools/qa/monkey.py (per run) and tools/soak (the ledger).
"""

from __future__ import annotations

import hashlib
import re

# Run-specific parts of a message, in the order they are masked.
_MASKS = (
    (re.compile(r"/tmp/[^\s'\"]+|/home/[^\s'\"]+|build/[^\s'\"]+"), "<path>"),
    (re.compile(r"'[^']{0,200}'|\"[^\"]{0,200}\""), "<q>"),
    (re.compile(r"\[[-\d., ]+\]"), "<rect>"),
    (re.compile(r"0x[0-9a-fA-F]+"), "<hex>"),
    # Digests and ids (a buffer's and a file's SHA-256 prefix differ every run).
    (re.compile(r"\b(?=[0-9a-f]*[0-9])(?=[0-9a-f]*[a-f])[0-9a-f]{8,}\b"), "<hash>"),
    (re.compile(r"\d+(\.\d+)?"), "<n>"),
    (re.compile(r"\s+"), " "),
)
# A stack frame of ours in a sanitizer or glibc report: "#3 0x... in Func(...) /path/src/x.cpp:12".
_FRAME = re.compile(r"#\d+ 0x[0-9a-f]+ in (\S+?)(\(|\s).*?(src/[\w/.+-]+):\d+")
# The "step N:" / "after monkey step N:" prefixes the monkey puts on a message.
_STEP_PREFIX = re.compile(r"^(after )?(monkey )?step \d+:?\s*|^after [^:]{0,120}: ")


def normalize(message: str) -> str:
    text = _STEP_PREFIX.sub("", message.strip())
    for pattern, replacement in _MASKS:
        text = pattern.sub(replacement, text)
    return text.strip()[:240]


def stack_key(log_text: str, frames: int = 3) -> str:
    """The first `frames` frames under src/ of the first report in `log_text`,
    as "Func@src/file.cpp" joined -- the part of a crash that names the defect."""
    found = []
    for match in _FRAME.finditer(log_text):
        found.append(f"{match.group(1)}@{match.group(3)}")
        if len(found) >= frames:
            break
    return " > ".join(found)


_STALE_BBOX = re.compile(r"QA-STALE .*?bbox=(-?\d+),(-?\d+),(\d+),(\d+)")
_STALE_SCALE = re.compile(r"QA-STALE .*?\bscale=([0-9.]+)")


def stale_scale(detail: str) -> float:
    """Device pixels per logical unit of a QA-STALE line. Its bbox and first pixel
    are device pixels, its clips (and the `ui` answer's surfaces) logical units;
    a line from before the field existed is taken as 1."""
    match = _STALE_SCALE.search(detail)
    try:
        scale = float(match.group(1)) if match else 1.0
    except ValueError:
        scale = 1.0
    return scale if scale > 0 else 1.0


def stale_surface(detail: str, surfaces: dict | None) -> str:
    """For a redraw-verifier finding, the surface its stale box sits on (by the
    box's centre, from the `ui` answer's surface rects): two stale-paint defects
    differ by where they are, which the normalized message no longer says."""
    match = _STALE_BBOX.search(detail)
    if not match or not surfaces:
        return ""
    scale = stale_scale(detail)
    x, y, w, h = (int(v) for v in match.groups())
    cx, cy = (x + w / 2) / scale, (y + h / 2) / scale
    for name, rect in surfaces.items():
        if isinstance(rect, list) and len(rect) == 4 and rect[0] <= cx < rect[0] + rect[2] \
                and rect[1] <= cy < rect[1] + rect[3]:
            return name
    return "elsewhere"


_STALE_FIRST = re.compile(r"QA-STALE .*?first=(-?\d+),(-?\d+)")
_STALE_CLIPS = re.compile(r"QA-STALE .*?clips=(\S+)")


def stale_clip_relation(detail: str) -> str:
    """Where a redraw-verifier finding's first stale pixel sits against the
    frame's clip rects: `in-clip` (the partial frame repainted it, wrongly),
    `clip-edge` (the pixel just past a clip's edge: paint that bleeds a pixel
    beyond its damage, or damage rounded a pixel short) or `outside` (nothing
    repainted it: damage that was never requested). Three different defects,
    which a signature keyed on the reason and the clip count merged."""
    first = _STALE_FIRST.search(detail)
    clips = _STALE_CLIPS.search(detail)
    if not first or not clips:
        return ""
    # In logical units, like the clips: a device pixel's unit is floor(p / scale).
    scale = stale_scale(detail)
    px, py = int(int(first.group(1)) // scale), int(int(first.group(2)) // scale)
    rects = []
    for part in clips.group(1).split(";"):
        try:
            x, y, w, h = (int(v) for v in part.split(","))
        except ValueError:
            continue
        rects.append((x, y, w, h))
    if any(x <= px < x + w and y <= py < y + h for x, y, w, h in rects):
        return "in-clip"
    for x, y, w, h in rects:
        on_column = px in (x - 1, x + w) and y - 1 <= py <= y + h
        on_row = py in (y - 1, y + h) and x - 1 <= px <= x + w
        if on_column or on_row:
            return "clip-edge"
    return "outside"


def stale_context(detail: str, surfaces: dict | None) -> str:
    """The signature context of a redraw-verifier finding: the surface its box
    sits on, how its first pixel relates to the frame's clips, and whether the
    box is a one-pixel line (a caret, a border, a rounding seam) or an area.
    Empty for any other finding."""
    match = _STALE_BBOX.search(detail)
    if not match:
        return ""
    w, h = int(match.group(3)), int(match.group(4))
    shape = "line" if min(w, h) <= 1 else "area"
    return f"{stale_surface(detail, surfaces)}|{stale_clip_relation(detail)}|{shape}"


def signature(kind: str, detail: str, log_text: str = "", context: str = "") -> str:
    """A stable id for a finding: kind, normalized message (plus `context`, such
    as the surface a stale box sits on), and for a crash or a sanitizer report
    the top frames of ours."""
    key = f"{kind}|{normalize(detail)}|{context}"
    if kind in ("crash", "log") and log_text:
        stack = stack_key(log_text)
        if stack:
            key = f"{kind}|{stack}"
    return hashlib.sha1(key.encode()).hexdigest()[:12]


def describe(kind: str, detail: str) -> str:
    """The one-line human label stored beside a signature."""
    return f"{kind}: {normalize(detail)}"[:200]


# Kinds whose findings say nothing about the app if the machine was overloaded:
# the soak runner re-checks them before calling them deterministic.
TIMING_KINDS = frozenset({"latency", "liveness", "idle"})  # re-checked before trusted
# Kinds after which the monkey cannot go on with the same process.
FATAL_KINDS = frozenset({"crash"})
