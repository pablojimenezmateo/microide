"""What the monkey knows about where it has been: state signatures, coverage and
the corpus of prefixes that reach rare states (safety-plan W1.2, W1.6, W1.7).

* A **state signature** is a short string read from answers the oracles already
  asked for (the `ui`, `buffer` and `editor` queries a check makes): focus,
  overlay, sidebar, panel, menu, modal, split count, active tab kind, soft wrap,
  selection kind, popups. Coarse on purpose: a few hundred states, so "unseen"
  means something.
* **Coverage** counts states, transitions (state, action, state'), state x action
  pairs and commands run. The monkey steers by it: an action already tried in this
  state is drawn less often, and a source whose actions keep reaching new states is
  drawn more. A coverage file is the soak runner's memory across runs; it is merged
  under a lock, since several jobs finish at once.
* The **corpus** keeps, per state signature first reached by a run, the action
  prefix that reached it. A later run may replay one (rarest states first) and
  explore from there, so a deep state is widened rather than found once by luck.
"""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import json
import os
import random
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any


def state_signature(ui: dict[str, Any] | None, buffer: dict[str, Any] | None,
                    editor: dict[str, Any] | None) -> str:
    """The coarse state the app is in, from answers already in hand."""
    if not ui:
        return "dead"
    overlay = ui.get("overlay") or {}
    sidebar = ui.get("sidebar") or {}
    panel = ui.get("panel") or {}
    menu = ui.get("menu") or {}
    modal = ui.get("modal")
    popups = ui.get("popups") or {}
    surfaces = ui.get("surfaces") or {}
    parts = [
        f"f={ui.get('focus')}",
        f"o={overlay.get('mode') if overlay.get('visible') else '-'}",
        f"s={sidebar.get('view') if sidebar.get('visible') else '-'}",
        f"p={(panel.get('content') or '?') + ('+max' if panel.get('maximized') else '') if panel.get('visible') else '-'}",
        f"m={'ctx' if menu.get('contextMenu') else ('sub' if menu.get('submenu') else 'open') if menu.get('open') or menu.get('contextMenu') else '-'}",
        f"d={(modal.get('kind') or modal.get('title') or 'modal') if isinstance(modal, dict) else ('modal' if modal else '-')}",
        f"g={len(ui.get('editors') or [])}",
        f"r={'1' if surfaces.get('rightPane') else '0'}",
        f"h={'hover' if popups.get('hover') else ''}{'sig' if popups.get('signatureHelp') else ''}",
    ]
    tab_kind = "-"
    for group in (editor or {}).get("groups", []):
        if group.get("focused"):
            tab_kind = next((t.get("kind", "?") for t in group.get("tabs", []) if t.get("active")), "none")
    parts.append(f"t={tab_kind}")
    if buffer and buffer.get("available"):
        cursors = buffer.get("cursors") or []
        if len(cursors) > 1:
            selection = "multi"
        elif cursors and not cursors[0].get("empty", True):
            selection = "lines" if cursors[0].get("line") != cursors[0].get("anchorLine") else "chars"
        else:
            selection = "caret"
        parts.append(f"b={buffer.get('kind')}:{'w' if buffer.get('softWrap') else ''}"
                     f"{'d' if buffer.get('dirty') else ''}{'ro' if buffer.get('readOnly') else ''}:{selection}")
    else:
        parts.append("b=-")
    return " ".join(parts)


def action_key(action: dict[str, Any], element: dict[str, Any] | None = None) -> str:
    """A coarse name for an action, for state x action coverage: what was done
    and to which kind of thing, not where."""
    if element is not None:
        name = element.get("name", "")
        kind = element.get("kind", "?")
        head, _, rest = name.partition("/")
        if kind in ("menu", "menu-item", "button", "sidebar-view", "panel-tab", "status", "debug-tab",
                    "setting", "settings-category"):
            target = name
        else:
            target = head  # rows, tabs, fields: one key per surface
        return f"{action.get('how', 'click')}:{target}"
    for key in ("line", "key", "type", "burst", "setting", "drag", "wheel", "resize", "external", "kill9",
                "drop", "focus-out", "corpus"):
        if key in action:
            value = action[key]
            if key == "line":
                return f"cmd:{str(value).split(' ', 1)[0]}"
            if key == "type":
                return f"type:{action.get('class', 'text')}"
            if key == "setting":
                return f"set:{value}"
            if key == "external":
                return f"ext:{value}"
            return f"{key}:{value if key in ('key', 'burst') else ''}"
    return "other"


class Coverage:
    """States, transitions, state x action pairs and commands, with a persistent
    form the soak runner merges across runs."""

    def __init__(self) -> None:
        self.states: Counter[str] = Counter()
        self.transitions: Counter[str] = Counter()
        self.pairs: Counter[str] = Counter()
        self.commands: Counter[str] = Counter()
        self.elements: Counter[str] = Counter()
        self.settings: Counter[str] = Counter()
        self.baseline_states: set[str] = set()  # what the loaded file had seen

    # ---- recording ---------------------------------------------------------

    def pair_count(self, state: str, key: str) -> int:
        return self.pairs.get(f"{state}\t{key}", 0)

    def record(self, before: str, key: str, after: str) -> tuple[bool, bool]:
        """Records one step; returns (new state, new transition) -- new to this
        run and everything loaded before it."""
        new_state = after not in self.states and after not in self.baseline_states
        transition = f"{before}\t{key}\t{after}"
        new_transition = transition not in self.transitions
        self.states[after] += 1
        self.transitions[transition] += 1
        self.pairs[f"{before}\t{key}"] += 1
        return new_state, new_transition

    # ---- persistence -------------------------------------------------------

    def to_json(self) -> dict[str, Any]:
        return {"states": dict(self.states), "transitions": dict(self.transitions),
                "pairs": dict(self.pairs), "commands": dict(self.commands),
                "elements": dict(self.elements), "settings": dict(self.settings)}

    def absorb(self, data: dict[str, Any]) -> None:
        for name in ("states", "transitions", "pairs", "commands", "elements", "settings"):
            getattr(self, name).update(data.get(name) or {})

    @classmethod
    def load(cls, path: Path | None) -> "Coverage":
        coverage = cls()
        if path is not None and path.exists():
            with contextlib.suppress(OSError, ValueError):
                coverage.absorb(json.loads(path.read_text()))
        coverage.baseline_states = set(coverage.states)
        return coverage

    def merge_into(self, path: Path, loaded: "Coverage | None" = None) -> None:
        """Adds what THIS run saw (everything recorded minus what was loaded)
        to the file, under an exclusive lock; atomic replace."""
        path.parent.mkdir(parents=True, exist_ok=True)
        lock_path = path.with_suffix(path.suffix + ".lock")
        with open(lock_path, "a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            current = Coverage()
            if path.exists():
                with contextlib.suppress(OSError, ValueError):
                    current.absorb(json.loads(path.read_text()))
            delta = self.to_json()
            if loaded is not None:
                base = loaded.to_json()
                delta = {name: {k: v - base[name].get(k, 0) for k, v in counts.items()
                                if v - base[name].get(k, 0) > 0}
                         for name, counts in delta.items()}
            current.absorb(delta)
            fd, temp = tempfile.mkstemp(dir=path.parent, prefix=".coverage-")
            with os.fdopen(fd, "w") as out:
                json.dump(current.to_json(), out)
            os.replace(temp, path)


# The longest prefix a run starts from. `pick` skips longer ones, so `save`
# refuses them: before it did, 92% of the soak's corpus (4.2 GB, 29.7k files)
# was prefixes no run could use, each read and parsed by every run's pick.
MAX_PREFIX_ACTIONS = 400


class Corpus:
    """Action prefixes that reached a state first, one file per state signature
    (`<sha1>.json`: the replay header, the state, and the actions)."""

    def __init__(self, directory: Path | None):
        self.directory = directory
        if directory is not None:
            directory.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def _name(state: str) -> str:
        return hashlib.sha1(state.encode()).hexdigest()[:16] + ".json"

    def save(self, state: str, header: dict[str, Any], actions: list[dict[str, Any]]) -> bool:
        """Keeps the shortest prefix per state. True when written."""
        kept = [a for a in actions if "check" not in a]
        if self.directory is None or not kept or len(kept) > MAX_PREFIX_ACTIONS:
            return False
        path = self.directory / self._name(state)
        if path.exists():
            with contextlib.suppress(OSError, ValueError):
                if len(json.loads(path.read_text()).get("actions", [])) <= len(kept):
                    return False
        record = {**header, "state": state, "actions": kept}
        fd, temp = tempfile.mkstemp(dir=self.directory, prefix=".corpus-")
        with os.fdopen(fd, "w") as out:
            json.dump(record, out)
        os.replace(temp, path)
        return True

    def prune(self) -> tuple[int, int]:
        """Deletes the prefixes no run can start from: longer than
        MAX_PREFIX_ACTIONS, or unreadable. Returns (files removed, bytes freed)."""
        removed = freed = 0
        if self.directory is None:
            return removed, freed
        for path in self.directory.glob("*.json"):
            try:
                usable = len(json.loads(path.read_text()).get("actions", [])) <= MAX_PREFIX_ACTIONS
            except (OSError, ValueError, AttributeError):
                usable = False
            if not usable:
                with contextlib.suppress(OSError):
                    size = path.stat().st_size
                    path.unlink()
                    removed, freed = removed + 1, freed + size
        return removed, freed

    def pick(self, rng: random.Random, coverage: Coverage, header: dict[str, Any],
             max_actions: int = MAX_PREFIX_ACTIONS) -> dict[str, Any] | None:
        """A prefix for this run's fixture and window size, the rarest states
        most likely (weight 1 / (1 + times the state was seen))."""
        if self.directory is None:
            return None
        entries: list[tuple[float, dict[str, Any]]] = []
        for path in sorted(self.directory.glob("*.json")):
            with contextlib.suppress(OSError, ValueError):
                record = json.loads(path.read_text())
                if record.get("fixture") != header.get("fixture") or \
                        list(record.get("size", [])) != list(header.get("size", [])) or \
                        len(record.get("actions", [])) > max_actions:
                    continue
                weight = 1.0 / (1 + coverage.states.get(record.get("state", ""), 0))
                entries.append((weight, record))
        if not entries:
            return None
        total = sum(weight for weight, _ in entries)
        pick = rng.uniform(0, total)
        for weight, record in entries:
            pick -= weight
            if pick <= 0:
                return record
        return entries[-1][1]
