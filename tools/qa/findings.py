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
    # A word of its own: "640x480" is a size, whose "0x480" read as hex split
    # one bounds defect into "<n>x<n> window" and "<n><hex> window".
    (re.compile(r"\b0x[0-9a-fA-F]+"), "<hex>"),
    # Digests and ids (a buffer's and a file's SHA-256 prefix differ every run).
    (re.compile(r"\b(?=[0-9a-f]*[0-9])(?=[0-9a-f]*[a-f])[0-9a-f]{8,}\b"), "<hash>"),
    (re.compile(r"\d+(\.\d+)?"), "<n>"),
    (re.compile(r"\s+"), " "),
)
# A stack frame of ours in a sanitizer or glibc report: "#3 0x... in Func(...) /path/src/x.cpp:12".
_FRAME = re.compile(r"#\d+ 0x[0-9a-f]+ in (\S+?)(\(|\s).*?(src/[\w/.+-]+):\d+")
# The "step N:" / "after monkey step N:" prefixes the monkey puts on a message.
_STEP_PREFIX = re.compile(r"^(after )?(monkey )?step \d+:?\s*|^after [^:]{0,120}: ")


# A `ui` element named in a layout finding ("button:close-tab/3/notes.md").
_ELEMENT_NAME = re.compile(r"\b([a-z][a-z-]*):([^\s\[\]]+)")
_LAYOUT_KINDS = frozenset({"overlap", "bounds"})


def normalize(message: str, kind: str = "") -> str:
    text = _STEP_PREFIX.sub("", message.strip())
    if kind in _LAYOUT_KINDS:
        # The elements by their stable part (element_key): a tab's close button
        # overlapping the panel tabs is one defect whichever file the tab holds,
        # and keyed by the file it was a one-off per file.
        text = _ELEMENT_NAME.sub(lambda m: element_key(m.group(0)), text)
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


# Element kinds whose locator's first two parts name a fixed control
# ("button:search/replace-all", "scrollbar:editor/vertical"); for the rest (tabs,
# rows, menu items) the parts after the first name the document or the label.
_FIXED_CONTROL_KINDS = frozenset({"button", "field", "scrollbar", "divider", "setting"})
_STABLE_PART = re.compile(r"^[A-Za-z][A-Za-z-]*$")


def element_key(name: str) -> str:
    """An element name with its run-specific parts dropped: the kind, and the
    leading locator parts that are words (no index, file name, label with spaces
    or `#2` suffix) -- two for a fixed control, one for anything else."""
    kind, _, locator = name.partition(":")
    keep = 2 if kind in _FIXED_CONTROL_KINDS else 1
    parts = []
    for part in locator.split("#", 1)[0].split("/")[:keep]:
        if not _STABLE_PART.match(part):
            break
        parts.append(part)
    return f"{kind}:{'/'.join(parts)}" if parts else kind


def stale_element(detail: str, elements: list | None) -> str:
    """For a redraw-verifier finding, the smallest `ui` element under its stale
    box's centre, by element_key; `-` when none is. A surface holds many
    controls, and two stale controls on one surface are two defects: the soak's
    250931ec505a held a search-view button left lit and a split pane's scrollbar
    drawn over the sidebar, both "sidebar, outside the clips, an area"."""
    match = _STALE_BBOX.search(detail)
    if not match or elements is None:
        return ""
    scale = stale_scale(detail)
    x, y, w, h = (int(v) for v in match.groups())
    cx, cy = (x + w / 2) / scale, (y + h / 2) / scale
    best: tuple[float, str] | None = None
    for element in elements:
        rect = element.get("rect") if isinstance(element, dict) else None
        name = element.get("name", "") if isinstance(element, dict) else ""
        if not name or not isinstance(rect, list) or len(rect) != 4 or rect[2] <= 0 or rect[3] <= 0:
            continue
        if rect[0] <= cx < rect[0] + rect[2] and rect[1] <= cy < rect[1] + rect[3]:
            area = rect[2] * rect[3]
            if best is None or area < best[0]:
                best = (area, name)
    return element_key(best[1]) if best else "-"


def stale_context(detail: str, surfaces: dict | None, elements: list | None = None) -> str:
    """The signature context of a redraw-verifier finding: the surface its box
    sits on, the element under it (when the `ui` answer listed them), how its
    first pixel relates to the frame's clips, and whether the box is a one-pixel
    line (a caret, a border, a rounding seam) or an area. Empty for any other
    finding."""
    match = _STALE_BBOX.search(detail)
    if not match:
        return ""
    w, h = int(match.group(3)), int(match.group(4))
    shape = "line" if min(w, h) <= 1 else "area"
    context = f"{stale_surface(detail, surfaces)}|{stale_clip_relation(detail)}|{shape}"
    element = stale_element(detail, elements)
    return f"{context}|{element}" if element else context


def signature(kind: str, detail: str, log_text: str = "", context: str = "") -> str:
    """A stable id for a finding: kind, normalized message (plus `context`, such
    as the surface a stale box sits on), and for a crash or a sanitizer report
    the top frames of ours."""
    key = f"{kind}|{normalize(detail, kind)}|{context}"
    if kind in ("crash", "log") and log_text:
        stack = stack_key(log_text)
        if stack:
            key = f"{kind}|{stack}"
    return hashlib.sha1(key.encode()).hexdigest()[:12]


def describe(kind: str, detail: str, context: str = "") -> str:
    """The one-line human label stored beside a signature. A stale-paint
    finding's context leads it: every such message reads the same once its
    numbers are masked, so the context is what tells two apart in a listing."""
    if context:
        return f"{kind}: [{context}] {normalize(detail, kind)}"[:200]
    return f"{kind}: {normalize(detail, kind)}"[:200]


# Kinds whose findings say nothing about the app if the machine was overloaded:
# the soak runner re-checks them before calling them deterministic.
TIMING_KINDS = frozenset({"latency", "liveness", "idle"})  # re-checked before trusted
# Kinds after which the monkey cannot go on with the same process.
FATAL_KINDS = frozenset({"crash"})
