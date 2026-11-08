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


def signature(kind: str, detail: str, log_text: str = "") -> str:
    """A stable id for a finding: kind, normalized message, and for a crash or
    a sanitizer report the top frames of ours."""
    key = f"{kind}|{normalize(detail)}"
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
TIMING_KINDS = frozenset({"latency", "liveness", "idle"})
# Kinds after which the monkey cannot go on with the same process.
FATAL_KINDS = frozenset({"crash"})
