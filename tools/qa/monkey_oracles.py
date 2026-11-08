"""The monkey's oracles beyond the App's always-on ones (safety-plan W1.3, W4).

Each takes the App and returns findings as (kind, message) pairs; the monkey
records them through App.report so they carry the same evidence directory.

* DataOracle -- the bytes on disk. A clean editor document's file holds exactly
  what a save would write (the `documents` query's SHA-256). Around a kill -9 and
  relaunch: every dirty document comes back dirty with the same content, every
  clean file is untouched, and the new process leaves no save temporary behind.
* UndoAlgebra -- sampled: k undos then k redos give back the same text.
* ResourceOracle -- descriptors, threads, child processes and RSS from /proc;
  growth past a budget over a run is a leak.
* Liveness -- every control answer within a budget (the TD-433 class).
"""

from __future__ import annotations

import hashlib
import os
import random
import time
from pathlib import Path
from typing import Any

import microide_qa as q

STAGING_MARKER = ".microide-staging."  # util::kTemporaryStagingMarker
# How long the crash-safety flush may take to stage unsaved buffers once edits
# are quiet: CrashSafetyFlushSchedule::kQuietMs (2.5 s) plus a margin for the write.
CRASH_SAFETY_QUIET_S = 3.5


def _sha256_file(path: Path) -> str | None:
    try:
        if not path.is_file():
            return None
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


class DataOracle:
    """The data-loss oracle. Stateless between checks except for the snapshot a
    kill -9 compares against."""

    def __init__(self, app: q.App):
        self.app = app
        # Files for the evidence directory of the finding the last check made:
        # {file name: bytes}. The monkey writes them next to FINDING.md, so a
        # mismatch that does not replay still says how the two sides differ.
        self.evidence: dict[str, bytes] = {}

    def documents(self) -> list[dict[str, Any]]:
        try:
            answer = self.app.query("documents")
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return []
        return answer if isinstance(answer, list) else []

    @staticmethod
    def _comparable(doc: dict[str, Any]) -> bool:
        # A file that loaded with mixed endings saves normalized: its clean buffer
        # does not claim to be byte-identical to the disk.
        return bool(doc.get("path")) and not doc.get("mixedLineEndings") and not doc.get("readOnly")

    def clean_mismatches(self, docs: list[dict[str, Any]]) -> dict[str, tuple[str, str]]:
        """{path: (buffer sha, disk sha)} for clean documents whose file differs."""
        out = {}
        for doc in docs:
            if doc.get("dirty") or not self._comparable(doc):
                continue
            disk = _sha256_file(Path(doc["path"]))
            if disk is not None and disk != doc.get("sha256"):
                out[doc["path"]] = (doc.get("sha256", ""), disk)
        return out

    def check_clean(self, attempts: int = 4, gap: float = 0.3) -> list[tuple[str, str]]:
        """A clean document whose file differs, and keeps differing while the
        file stays the same for `attempts` reads -- an external rewrite the
        watcher has not reloaded yet is not a finding, a stable disagreement is."""
        mismatched = self.clean_mismatches(self.documents())
        for _ in range(attempts - 1):
            if not mismatched:
                return []
            time.sleep(gap * q.SLOW_FACTOR)
            again = self.clean_mismatches(self.documents())
            # Only what still differs from the SAME disk bytes.
            mismatched = {path: pair for path, pair in again.items()
                          if path in mismatched and mismatched[path][1] == pair[1]}
        self.evidence = {}
        for path, pair in mismatched.items():
            self.evidence.update(self._mismatch_evidence(path, pair))
        return [("data", f"the clean buffer of {Path(path).name} differs from its file on disk "
                         f"(buffer {pair[0][:12]}, disk {pair[1][:12]}) and the file is not changing")
                for path, pair in mismatched.items()]

    def _mismatch_evidence(self, path: str, pair: tuple[str, str]) -> dict[str, bytes]:
        """Both sides' bytes and where they first part, for one mismatched path."""
        name = Path(path).name
        out: dict[str, bytes] = {}
        try:
            disk = Path(path).read_bytes()
        except OSError:
            disk = None
        buffer = None
        try:
            for doc in self.app.query("documents", {"content": path}) or []:
                if doc.get("path") == path and "contentHex" in doc:
                    buffer = bytes.fromhex(doc["contentHex"])
        except Exception:  # noqa: BLE001 - evidence is best effort
            pass
        if disk is not None:
            out[f"mismatch-{name}.disk"] = disk
        if buffer is not None:
            out[f"mismatch-{name}.buffer"] = buffer
        lines = [f"path: {path}", f"buffer sha256: {pair[0]}", f"disk sha256:   {pair[1]}"]
        if disk is not None and buffer is not None:
            first = next((i for i, (a, b) in enumerate(zip(buffer, disk)) if a != b),
                         min(len(buffer), len(disk)))
            lines += [f"buffer {len(buffer)} bytes, disk {len(disk)} bytes; first difference at byte {first}",
                      f"buffer[{first}:+48]: {buffer[first:first + 48]!r}",
                      f"disk[{first}:+48]:   {disk[first:first + 48]!r}"]
            try:
                st = Path(path).stat()
                lines.append(f"disk mtime_ns {st.st_mtime_ns} size {st.st_size} mode {oct(st.st_mode)}")
            except OSError:
                pass
        out[f"mismatch-{name}.txt"] = ("\n".join(lines) + "\n").encode()
        return out

    def staging_leftovers(self, pid: int | None) -> list[Path]:
        """Save temporaries of process `pid` under the project (its own saves
        are done once it is idle, so one left is a leak of the save path)."""
        project = self.app.project
        if project is None or pid is None:
            return []
        found = []
        for root, dirs, files in os.walk(project):
            dirs[:] = [d for d in dirs if d != ".git"]
            for name in files:
                if STAGING_MARKER in name and f"{STAGING_MARKER}{pid}." in name:
                    found.append(Path(root) / name)
        return found

    def kill9_and_check(self, rng: random.Random) -> list[tuple[str, str]]:
        """SIGKILL, relaunch on the same profile, and compare. Waits for the
        crash-safety flush first (most kills), so every dirty buffer is owed back;
        a kill with no wait checks integrity only (an unstaged edit may be lost
        by design: CrashSafetyFlushSchedule)."""
        app = self.app
        waited = rng.random() < 0.7
        if waited:
            time.sleep(CRASH_SAFETY_QUIET_S * q.SLOW_FACTOR)
        before = self.documents()
        disk_before = {doc["path"]: _sha256_file(Path(doc["path"])) for doc in before
                       if doc.get("path") and not doc.get("dirty")}
        app.record_external({"kill9": True, "waited": waited})
        app.crash_and_relaunch()
        out: list[tuple[str, str]] = []
        # Restore lands over a few frames: wait until every dirty path is back.
        dirty = [doc for doc in before if doc.get("dirty")]
        deadline = time.monotonic() + 10 * q.SLOW_FACTOR
        after = self.documents()
        while time.monotonic() < deadline:
            keys = {(d.get("path") or d.get("title")) for d in after}
            if all((d.get("path") or d.get("title")) in keys for d in dirty):
                break
            time.sleep(0.2)
            after = self.documents()
        if waited:
            by_key = {(d.get("path") or d.get("title")): d for d in after}
            for doc in dirty:
                key = doc.get("path") or doc.get("title")
                back = by_key.get(key)
                if back is None:
                    out.append(("data", f"the unsaved buffer {Path(str(key)).name} did not come back after a kill -9 "
                                        f"(the crash-safety flush had {CRASH_SAFETY_QUIET_S:.1f}s of quiet)"))
                elif back.get("sha256") != doc.get("sha256"):
                    out.append(("data", f"the unsaved buffer {Path(str(key)).name} came back after a kill -9 with "
                                        f"different content ({doc.get('bytes')} -> {back.get('bytes')} bytes)"))
                elif not back.get("dirty"):
                    out.append(("data", f"the unsaved buffer {Path(str(key)).name} came back after a kill -9 "
                                        f"marked saved; its file does not hold it"))
        for path, sha in disk_before.items():
            now = _sha256_file(Path(path))
            if sha is not None and now is not None and now != sha and not self._churned(path):
                out.append(("data", f"{Path(path).name} changed on disk across a kill -9 and relaunch, "
                                    f"and nothing saved it"))
        return out

    def _churned(self, path: str) -> bool:
        """The file churn (or an external git step) rewrote it during the kill."""
        project = str(self.app.project or "")
        relative = path[len(project) + 1:] if path.startswith(project) else path
        recent = self.app.actions[-20:]
        return any(row.get("churn") == relative or "external" in row for row in recent)


class UndoAlgebra:
    """k undos then k redos are the identity on the text (the editing invariants'
    rule, sampled mid-run on whatever state the monkey reached)."""

    def __init__(self, app: q.App, rng: random.Random):
        self.app = app
        self.rng = rng

    def check(self) -> list[tuple[str, str]]:
        app = self.app
        ui = app.last_ui or {}
        if ui.get("focus") != "editor" or (ui.get("overlay") or {}).get("visible") or ui.get("modal") \
                or (ui.get("menu") or {}).get("open"):
            return []
        try:
            before = app.query("buffer")
        except q.QaFailure:
            return []
        if before.get("kind") != "editor" or before.get("readOnly") or "text" not in before:
            return []
        k = self.rng.randint(1, 4)
        for verb in ["undo"] * k + ["redo"] * k:
            try:
                app.send_line(verb)
            except q.QaFailure:
                return []
        try:
            after = app.query("buffer")
        except q.QaFailure:
            return []
        if after.get("path") != before.get("path") or after.get("kind") != "editor":
            return []  # the commands moved focus somewhere else: not this rule's case
        if after.get("text") != before.get("text"):
            return [("undo", f"{k} undo(s) then {k} redo(s) changed the text of "
                             f"{Path(before.get('path') or 'untitled').name} "
                             f"({len(before['text'])} -> {len(after.get('text') or '')} chars)")]
        return []


class ResourceOracle:
    """Descriptors, threads, children and RSS of the app process, sampled."""

    # Growth over a run beyond the first sample after warm-up. Generous: a run
    # opens files, terminals and language servers on purpose; a leak per action
    # crosses these within a few hundred steps.
    FD_BUDGET = 256
    THREAD_BUDGET = 64
    CHILD_BUDGET = 48
    RSS_BUDGET_KB = 1_500_000

    def __init__(self, app: q.App):
        self.app = app
        self.baseline: dict[str, int] | None = None
        self.pid: int | None = None
        self.samples: list[dict[str, int]] = []
        self.reported: set[str] = set()

    def sample(self) -> dict[str, int] | None:
        proc = self.app.proc
        if proc is None or proc.poll() is not None:
            return None
        pid = proc.pid
        if pid != self.pid:  # a relaunch starts a new baseline
            self.pid, self.baseline = pid, None
        base = Path(f"/proc/{pid}")
        try:
            fds = len(os.listdir(base / "fd"))
            status = (base / "status").read_text()
            fields = dict(line.split(":", 1) for line in status.splitlines() if ":" in line)
            threads = int(fields.get("Threads", "0").strip())
            rss = int(fields.get("VmRSS", "0 kB").split()[0])
            children = 0
            for task in (base / "task").iterdir():
                children += len((task / "children").read_text().split())
        except (OSError, ValueError):
            return None
        sample = {"fds": fds, "threads": threads, "rss_kb": rss, "children": children}
        self.samples.append(sample)
        if self.baseline is None:
            self.baseline = sample
        return sample

    def check(self) -> list[tuple[str, str]]:
        sample = self.sample()
        if sample is None or self.baseline is None:
            return []
        out = []
        budgets = {"fds": self.FD_BUDGET, "threads": self.THREAD_BUDGET, "children": self.CHILD_BUDGET,
                   "rss_kb": int(self.RSS_BUDGET_KB * max(1.0, q.SLOW_FACTOR / 2))}
        for name, budget in budgets.items():
            grown = sample[name] - self.baseline[name]
            if grown > budget and name not in self.reported:
                self.reported.add(name)
                out.append(("leak", f"the app's {name} grew by {grown} over the run "
                                    f"({self.baseline[name]} -> {sample[name]}; budget {budget})"))
        return out


class Liveness:
    """Every control answer within `budget` seconds (stretched by the slow
    factor). One finding per run per query verb."""

    def __init__(self, app: q.App, budget: float = 2.5):
        self.app = app
        self.budget = budget * q.SLOW_FACTOR
        self.reported: set[str] = set()

    def check(self) -> list[tuple[str, str]]:
        what, seconds = self.app.slowest_answer
        self.app.slowest_answer = ("", 0.0)
        if seconds > self.budget and what not in self.reported:
            self.reported.add(what)
            return [("latency", f"{what} took {seconds:.2f}s to answer (budget {self.budget:.1f}s)")]
        return []


class IdleOracle:
    """After a quiet second, the app's main thread sleeps: voluntary wakes per
    second of the thread named `microide` (its main thread) within a budget.
    Measured idle under Xvfb with the caret blinking: 40-60/s (2026-11-08); a
    busy loop or a timer firing per millisecond is thousands. Involuntary
    switches are left out: they count the machine's load, not the app."""

    BUDGET_PER_SECOND = 250.0

    def __init__(self, app: q.App):
        self.app = app
        self.reported = False

    def _main_thread_wakes(self) -> int | None:
        proc = self.app.proc
        if proc is None or proc.poll() is not None:
            return None
        try:
            status = Path(f"/proc/{proc.pid}/task/{proc.pid}/status").read_text()
        except OSError:
            return None
        for line in status.splitlines():
            if line.startswith("voluntary_ctxt_switches:"):
                return int(line.split()[1])
        return None

    def check(self) -> list[tuple[str, str]]:
        if self.reported:
            return []
        time.sleep(1.0 * q.SLOW_FACTOR)
        before = self._main_thread_wakes()
        window = 1.0 * q.SLOW_FACTOR
        time.sleep(window)
        after = self._main_thread_wakes()
        if before is None or after is None:
            return []
        rate = (after - before) / window
        if rate > self.BUDGET_PER_SECOND * max(1.0, q.SLOW_FACTOR):
            self.reported = True
            return [("idle", f"the main thread woke {rate:.0f} times a second while the app was idle "
                             f"(budget {self.BUDGET_PER_SECOND:.0f})")]
        return []

