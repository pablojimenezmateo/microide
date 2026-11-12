"""The soak runner's findings ledger (safety-plan W1.5).

An append-only event log, `build/soak/ledger.jsonl`, one JSON object per line:

    {"t": <unix>, "sig": "<signature>", "event": "<event>", ...}

Events: `new` (first sighting; carries kind, label, bundle), `seen` (a later
sighting: the count rises, no new entry), `checked` (determinism measured: rate,
deterministic), `minimized` (the minimal replay's length), `claimed` (by an
agent), `fixed` (with the fix commit), `verified` (the fix's commit is in the
soak build and the minimal replay no longer reproduces), `reopened` (a fixed or
verified signature came back: a sighting from a build that has the fix, or the
verification replay), `note`.

The state of a signature is the fold of its events; nothing is rewritten, so two
writers (the runner and an agent marking a fix) never lose each other's lines.
Writes take an exclusive flock on the file.
"""

from __future__ import annotations

import fcntl
import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterator

# contains(fix_commit, commit): the build at `commit` has the fix.
Contains = Callable[[str, str], bool]

STATES = ("new", "claimed", "fixed", "verified", "reopened")


@dataclass
class Entry:
    sig: str
    kind: str = ""
    label: str = ""
    status: str = "new"
    count: int = 0
    first_seen: float = 0.0
    last_seen: float = 0.0
    bundle: str = ""
    deterministic: bool | None = None
    rate: float | None = None
    paced: bool = False  # reproduces only at the run's recorded pace
    minimal_actions: int | None = None
    full_actions: int | None = None
    fix_commit: str = ""
    claimed_by: str = ""
    cells: set[str] = field(default_factory=set)
    commits: set[str] = field(default_factory=set)
    notes: list[str] = field(default_factory=list)

    def fixed_in(self, commit: str, contains: Contains | None) -> bool:
        """A sighting at `commit` comes after this entry's fix: it is fixed or
        verified and that build has the fix commit. Without an ancestry check
        every sighting counts."""
        if self.status not in ("fixed", "verified"):
            return False
        if contains is None or not self.fix_commit or not commit:
            return True
        return contains(self.fix_commit, commit)

    def to_json(self) -> dict[str, Any]:
        out = dict(self.__dict__)
        out["cells"] = sorted(self.cells)
        out["commits"] = sorted(self.commits)
        return out


class Ledger:
    """`contains` tells whether a build has a fix commit; with it, a sighting from
    a build older than the fix (the runner keeps the previous build's jobs going
    while it builds the next) does not reopen the finding."""

    def __init__(self, path: Path, contains: Contains | None = None):
        self.path = path
        self.contains = contains
        path.parent.mkdir(parents=True, exist_ok=True)

    def append(self, sig: str, event: str, **fields: Any) -> None:
        record = {"t": round(time.time(), 3), "sig": sig, "event": event, **fields}
        line = json.dumps(record, sort_keys=True) + "\n"
        with open(self.path, "a") as out:
            fcntl.flock(out, fcntl.LOCK_EX)
            out.write(line)
            out.flush()
            fcntl.flock(out, fcntl.LOCK_UN)

    def events(self) -> Iterator[dict[str, Any]]:
        if not self.path.exists():
            return
        with open(self.path) as source:
            for line in source:
                try:
                    yield json.loads(line)
                except ValueError:
                    continue

    def entries(self) -> dict[str, Entry]:
        out: dict[str, Entry] = {}
        for event in self.events():
            sig = event.get("sig", "")
            entry = out.setdefault(sig, Entry(sig=sig))
            kind = event.get("event")
            when = float(event.get("t", 0))
            if kind in ("new", "seen"):
                entry.count += int(event.get("count", 1))
                entry.first_seen = entry.first_seen or when
                entry.last_seen = when
                entry.kind = event.get("kind", entry.kind)
                entry.label = event.get("label", entry.label)
                entry.bundle = event.get("bundle", entry.bundle) or entry.bundle
                if event.get("cell"):
                    entry.cells.add(event["cell"])
                if event.get("commit"):
                    entry.commits.add(event["commit"])
                if kind == "seen" and entry.fixed_in(event.get("commit", ""), self.contains):
                    entry.status = "reopened"
            elif kind == "checked":
                entry.deterministic = bool(event.get("deterministic"))
                entry.rate = event.get("rate")
                entry.paced = bool(event.get("paced", False))
                entry.full_actions = event.get("actions", entry.full_actions)
            elif kind == "minimized":
                entry.minimal_actions = event.get("actions")
            elif kind == "claimed":
                entry.status = "claimed"
                entry.claimed_by = event.get("by", "")
            elif kind == "fixed":
                entry.status = "fixed"
                entry.fix_commit = event.get("commit", "")
            elif kind == "verified":
                entry.status = "verified"
            elif kind == "reopened":
                entry.status = "reopened"
            elif kind == "note":
                entry.notes.append(str(event.get("text", "")))
        return out

    def open_entries(self) -> list[Entry]:
        return [e for e in self.entries().values() if e.status in ("new", "claimed", "reopened")]
