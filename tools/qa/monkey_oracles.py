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

import collections
import hashlib
import json
import os
import random
import shutil
import time
from pathlib import Path
from typing import Any

import microide_qa as q

STAGING_MARKER = ".microide-staging."  # util::kTemporaryStagingMarker
# How long the crash-safety flush may take to stage unsaved buffers once edits
# are quiet: CrashSafetyFlushSchedule::kQuietMs (2.5 s) plus a margin for the write.
CRASH_SAFETY_QUIET_S = 3.5
# The longest a quiet app may take to report its unsaved buffers staged (the
# `persistence` query) before the kill oracle calls the flush missing. Generous:
# under a loaded soak the wake, the encode and the fsync all queue behind others.
CRASH_SAFETY_STAGED_LIMIT_S = 20.0


def _sha256_file(path: Path) -> str | None:
    try:
        if not path.is_file():
            return None
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None




def _doc_key(doc: dict[str, Any]) -> tuple[str, str]:
    """A document across a relaunch: its path, or an untitled buffer's title --
    within its project, since each project numbers its own Untitled-N."""
    return (str(doc.get("project") or ""), str(doc.get("path") or doc.get("title")))

class DataOracle:
    """The data-loss oracle. Stateless between checks except for the snapshot a
    kill -9 compares against."""

    def __init__(self, app: q.App):
        self.app = app
        # Files for the evidence directory of the finding the last check made:
        # {file name: bytes}. The monkey writes them next to FINDING.md, so a
        # mismatch that does not replay still says how the two sides differ.
        self.evidence: dict[str, bytes] = {}

    def documents(self, all_projects: bool = False) -> list[dict[str, Any]]:
        """The open documents: the active project's, or with `all_projects` every
        open project's, each then naming its `project` and whether it is `active`."""
        try:
            answer = self.app.query("documents", {"all": True} if all_projects else None)
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
        history = self._buffer_io_history(path)
        if history:
            out[f"mismatch-{name}.io.txt"] = ("\n".join(history) + "\n").encode()
        return out

    def _buffer_io_history(self, path: str, keep: int = 200) -> list[str]:
        """Every read, write, adopted baseline, revert and undo-to-clean of `path`
        the app logged (MICROIDE_TRACE_BUFFER_IO), across its launches, newest
        last. The bytes say THAT the buffer and the file disagree; this says which
        moment put them apart, and it is gone from the log tail by the time a
        long run's oracle fires."""
        marker = f" path={path} "
        work = getattr(self.app, "work", None)
        logs = sorted(Path(work).glob("microide*.log"), key=lambda p: p.stat().st_mtime) if work else []
        found: list[str] = []
        for log in logs:
            try:
                text = log.read_text(errors="replace")
            except OSError:
                continue
            found += [f"{log.name}: {line}" for line in text.splitlines()
                      if "microide buffer-io:" in line and marker in line]
        return found[-keep:]

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

    def kill9_and_check(self, rng: random.Random, waited: bool | None = None) -> list[tuple[str, str]]:
        """SIGKILL, relaunch on the same profile, and compare. Waits for the
        crash-safety flush first (most kills), so every dirty buffer is owed back;
        a kill with no wait checks integrity only (an unstaged edit may be lost
        by design: CrashSafetyFlushSchedule)."""
        app = self.app
        if waited is None:
            waited = rng.random() < 0.7
        out: list[tuple[str, str]] = []
        if waited:
            time.sleep(CRASH_SAFETY_QUIET_S * q.SLOW_FACTOR)
            # Then ask, rather than trust the sleep: on a loaded machine the write
            # can land after a fixed wait, and a buffer "lost" to that was a slow
            # flush, not a missing one -- the two had one finding between them.
            staged = self.wait_staged(CRASH_SAFETY_STAGED_LIMIT_S * q.SLOW_FACTOR)
            if staged is False:
                out.append(("data", f"the crash-safety flush did not stage the unsaved buffers within "
                                    f"{CRASH_SAFETY_STAGED_LIMIT_S:.0f}s of quiet (the `persistence` query never "
                                    f"answered staged)"))
        # Every project's: the dirty buffers of a second project the monkey had
        # opened come back in that project, which need not be the active one.
        before = self.documents(all_projects=True)
        disk_before = {doc["path"]: _sha256_file(Path(doc["path"])) for doc in before
                       if doc.get("path") and not doc.get("dirty")}
        app.record_external({"kill9": True, "waited": waited})
        # The killed process's state tree (its session records, primary and .bak):
        # what the relaunch restored from. Lost buffers whose staging the log shows
        # left no other trace of which record was read or why it lacked them.
        work = getattr(app, "work", None)
        self.last_kill_state = Path(work) / "kill-state" if work else None

        def keep_state() -> None:
            if self.last_kill_state is None:
                return
            shutil.rmtree(self.last_kill_state, ignore_errors=True)
            state = Path(work) / "state"
            if state.is_dir():
                shutil.copytree(state, self.last_kill_state, symlinks=True,
                                ignore_dangling_symlinks=True, dirs_exist_ok=True)

        # With no project on the command line: the app restores its saved
        # workspace, as after a real crash. Naming the run's project opened that
        # project alone, and a second project's buffers, kept in its session
        # record, were reported lost (soak e946ec9333d7).
        app.crash_and_relaunch(between=keep_state, restore_workspace=True)
        dirty = [doc for doc in before if doc.get("dirty")]
        self._load_restored_projects(dirty)
        # Restore lands over a few frames: wait until every dirty path is back.
        deadline = time.monotonic() + 10 * q.SLOW_FACTOR
        after = self.documents(all_projects=True)
        while time.monotonic() < deadline:
            keys = {_doc_key(d) for d in after}
            if all(_doc_key(d) in keys for d in dirty):
                break
            time.sleep(0.2)
            after = self.documents(all_projects=True)
        self.last_kill = {"before": before, "after": after}
        if waited:
            by_key = {_doc_key(d): d for d in after}
            for doc in dirty:
                key = doc.get("path") or doc.get("title")
                back = by_key.get(_doc_key(doc))
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

    def _load_restored_projects(self, dirty: list[dict[str, Any]]) -> None:
        """A relaunch restores every project of the workspace, but only the active
        one loads its tabs; the others load on their first activation, and until
        then `documents` lists nothing of them -- their unsaved buffers read as
        lost (soak 55673209c35c). When a project owing one has not loaded, one lap
        of project-next activates each project once and ends where it began."""
        owed = {doc.get("project") for doc in dirty if doc.get("project")}
        if not owed:
            return
        try:
            projects = self.app.query("projects")
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return
        if not isinstance(projects, list):
            return
        # `loaded` is absent from a binary that predates it: nothing to wait for.
        if not any(p.get("loaded") is False and p.get("root") in owed for p in projects):
            return
        try:
            for _ in projects:
                self.app.send_line("project-next")
        except Exception:  # noqa: BLE001 - as above
            return

    def wait_staged(self, limit_s: float) -> bool | None:
        """Polls the `persistence` query until it answers staged: True when it did,
        False when it never did within `limit_s`, None from a binary that predates
        the query (a replay against an older build), which keeps the plain sleep."""
        deadline = time.monotonic() + limit_s
        while True:
            try:
                if self.app.query("persistence").get("staged"):
                    return True
            except q.QaFailure as failure:
                if "unknown query" in str(failure):
                    return None
                raise
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.1)

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
        # The text before and after the last check's undos and redos, for the
        # finding's evidence: {file name: bytes}.
        self.evidence: dict[str, bytes] = {}

    def pick(self) -> int:
        """How many undos the next check asks for (recorded, so a replay asks the same)."""
        return self.rng.randint(1, 4)

    def check(self, asked: int | None = None) -> list[tuple[str, str]]:
        app = self.app
        self.evidence = {}
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
        # Only the undos that undid something are owed a redo: at the bottom of the
        # history an undo does nothing, and the k redos then replayed edits the run
        # had undone earlier -- a changed text from a correct editor (soak
        # 1aa1784d1685, after twenty Ctrl+Z). An undo that applied moves the
        # buffer's content revision.
        asked = self.pick() if asked is None else asked
        k = 0
        revision = before.get("revision")
        for _ in range(asked):
            try:
                app.send_line("undo")
                now = app.query("buffer")
            except q.QaFailure:
                return []
            if now.get("path") != before.get("path") or now.get("kind") != "editor":
                return []
            if revision is None or now.get("revision") != revision:
                k += 1
                revision = now.get("revision")
        for _ in range(k):
            try:
                app.send_line("redo")
            except q.QaFailure:
                return []
        try:
            after = app.query("buffer")
        except q.QaFailure:
            return []
        if after.get("path") != before.get("path") or after.get("kind") != "editor":
            return []  # the commands moved focus somewhere else: not this rule's case
        if after.get("text") != before.get("text"):
            self.evidence = {"undo-before.txt": str(before.get("text", "")).encode(),
                             "undo-after.txt": str(after.get("text") or "").encode()}
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
    # What one open terminal pane holds, measured (2026-11-12): its pty (3 fds),
    # its reader thread and its shell. Twenty opened and closed again return to
    # the baseline exactly, so terminals the run still has open are not a leak --
    # the terminal profile keeps 200 of them and every run "leaked" its budgets
    # (soak 6da9bc4a526a, d1e39df0281e, bdea62052550). Growth is measured past
    # what the open panes account for, with a little slack per pane.
    PER_PANE = {"fds": 4, "threads": 2, "children": 1, "rss_kb": 0}

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
        sample = {"fds": fds, "threads": threads, "rss_kb": rss, "children": children,
                  "panes": self._terminal_panes()}
        self.samples.append(sample)
        if self.baseline is None:
            self.baseline = sample
        return sample

    def _terminal_panes(self) -> int:
        try:
            tabs = self.app.query("terminals")
        except Exception:  # noqa: BLE001 - a busy app: count none, the budgets stay strict
            return 0
        tabs = tabs if isinstance(tabs, list) else tabs.get("tabs", [])
        return sum(max(1, len(tab.get("panes", []))) for tab in tabs)

    def check(self) -> list[tuple[str, str]]:
        sample = self.sample()
        if sample is None or self.baseline is None:
            return []
        out = []
        opened = max(0, sample.get("panes", 0) - self.baseline.get("panes", 0))
        budgets = {"fds": self.FD_BUDGET, "threads": self.THREAD_BUDGET, "children": self.CHILD_BUDGET,
                   "rss_kb": int(self.RSS_BUDGET_KB * max(1.0, q.SLOW_FACTOR / 2))}
        for name, budget in budgets.items():
            grown = sample[name] - self.baseline[name]
            held = self.PER_PANE[name] * opened
            if grown - held > budget and name not in self.reported:
                self.reported.add(name)
                out.append(("leak", f"the app's {name} grew by {grown} over the run "
                                    f"({self.baseline[name]} -> {sample[name]}; budget {budget}"
                                    + (f", plus {held} for {opened} more open terminal pane(s)" if held else "")
                                    + ")"))
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


class StructuralDiffOracle:
    """What the compare view's structural refinement claims, held against what it
    shows (the `compare` query: rows, hunks and each side's novel byte spans).

    - spans: inside their line, sorted, non-overlapping, on UTF-8 boundaries, and
      only where the row has that side and is not unchanged;
    - layout only: a hunk whose two sides are the same text once whitespace is
      gone marks no token -- re-indenting, re-wrapping or re-spacing code costs
      nothing (checked where no string or comment could make whitespace matter);
    - a real change shows: a hunk whose sides differ beyond whitespace marks a
      token, or adds or deletes a row with text -- never "layout only".

    Rows with cut text (`textTruncated`) are skipped, and a cut row list is
    checked only up to the cut, hunk by complete hunk."""

    _QUOTES_OR_COMMENTS = ('"', "'", "//", "/*", "#")

    def __init__(self, app: q.App):
        self.app = app
        self.evidence: dict[str, bytes] = {}

    def check(self) -> list[tuple[str, str]]:
        try:
            diff = self.app.query("compare")
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return []
        if not diff.get("available"):
            return []
        rows = diff.get("rows") or []
        found: list[tuple[str, str]] = []
        hunks: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
        for index, row in enumerate(rows):
            if row.get("hunk", -1) >= 0:
                hunks[row["hunk"]].append(row)
            if row.get("textTruncated"):
                continue
            for side, present in (("left", row.get("leftLine", 0) > 0), ("right", row.get("rightLine", 0) > 0)):
                spans = row.get(f"{side}Spans") or []
                if not spans:
                    continue
                where = f"row {index + 1} ({row.get('kind')}, {side} line {row.get(side + 'Line')})"
                if not present or row.get("kind") == "unchanged":
                    found.append(("diff", f"the {side} side of {where} marks novel tokens on a side "
                                          f"that {'is absent' if not present else 'did not change'}"))
                    continue
                # The spans are offsets into the line's real bytes: the JSON text of
                # a line that is not valid UTF-8 has U+FFFD in place of each bad
                # run, so such a side comes with its exact bytes as hex.
                raw = bytes.fromhex(row[f"{side}Hex"]) if row.get(f"{side}Hex") \
                    else str(row.get(f"{side}Text", "")).encode()
                last = 0
                for start, end in spans:
                    if not (last <= start < end <= len(raw)):
                        found.append(("diff", f"the {side} side of {where} has a novel span [{start}, {end}) "
                                              f"outside its {len(raw)}-byte line or out of order"))
                        break
                    if any(0 < at < len(raw) and (raw[at] & 0xC0) == 0x80 for at in (start, end)):
                        found.append(("diff", f"the {side} side of {where} has a novel span [{start}, {end}) "
                                              f"that splits a character"))
                        break
                    last = end
        complete = sorted(hunks)[:-1] if diff.get("rowsTruncated") else sorted(hunks)
        for hunk in complete:
            members = hunks[hunk]
            if any(r.get("textTruncated") for r in members):
                continue
            left = "".join(r.get("leftText", "") for r in members if r.get("leftLine", 0) > 0)
            right = "".join(r.get("rightText", "") for r in members if r.get("rightLine", 0) > 0)
            marked = any(r.get("leftSpans") or r.get("rightSpans") for r in members)
            squeezed_left, squeezed_right = "".join(left.split()), "".join(right.split())
            if squeezed_left == squeezed_right:
                if marked and not any(m in left + right for m in self._QUOTES_OR_COMMENTS):
                    found.append(("diff", f"hunk {hunk + 1} changes only whitespace and line breaks, "
                                          f"and marks tokens novel"))
                    self.evidence[f"structural-hunk-{hunk + 1}.json"] = json.dumps(members, indent=1).encode()
            elif not marked and not any(r.get("kind") in ("added", "deleted") and
                                        (r.get("leftText", "") + r.get("rightText", "")).strip()
                                        for r in members):
                found.append(("diff", f"hunk {hunk + 1} changes more than whitespace, and shows it as "
                                      f"layout only (no novel token, no added or deleted line)"))
                self.evidence[f"structural-hunk-{hunk + 1}.json"] = json.dumps(members, indent=1).encode()
        return found


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
        self.evidence: str | None = None  # what the main thread did, when it fired

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

    def _sample_main_thread(self) -> str:
        """Where the main thread is right now: its kernel wait channel and syscall."""
        proc = self.app.proc
        if proc is None:
            return ""
        base = Path(f"/proc/{proc.pid}/task/{proc.pid}")
        parts = []
        for name in ("wchan", "syscall"):
            try:
                parts.append((base / name).read_text().strip().split(" ")[0])
            except OSError:
                parts.append("?")
        return " ".join(parts)

    def check(self) -> list[tuple[str, str]]:
        if self.reported:
            return []
        time.sleep(1.0 * q.SLOW_FACTOR)
        log_path = getattr(self.app, "log_path", None)
        log_start = log_path.stat().st_size if log_path is not None and log_path.exists() else None
        before = self._main_thread_wakes()
        window = 1.0 * q.SLOW_FACTOR
        # What the wakes were: the main thread's wait channel and syscall sampled
        # through the window. The count alone said "too many" and nothing about
        # whom to blame -- a terminal streaming output, a timer, a lock (soak
        # a64655101a5a, every build since the oracle landed).
        samples: list[str] = []
        deadline = time.monotonic() + window
        while time.monotonic() < deadline:
            samples.append(self._sample_main_thread())
            time.sleep(window / 40)
        after = self._main_thread_wakes()
        if before is None or after is None:
            return []
        rate = (after - before) / window
        if rate > self.BUDGET_PER_SECOND * max(1.0, q.SLOW_FACTOR):
            logged: list[str] = []
            if log_start is not None:
                try:
                    with open(log_path, "rb") as log:
                        log.seek(log_start)
                        logged = log.read().decode(errors="replace").splitlines()
                except OSError:
                    pass
            frames = [line for line in logged if "redraw frame" in line]
            reasons = collections.Counter(
                line.split("reason=", 1)[1].split(" ", 1)[0] for line in frames if "reason=" in line)
            self.evidence = "\n".join([
                f"voluntary wakes/s: {rate:.0f} (budget {self.BUDGET_PER_SECOND:.0f})",
                f"frames in the window: {len(frames)} {dict(reasons)}",
                *self._repainted_regions(frames),
                f"log lines in the window: {len(logged)}",
                "main thread wchan + syscall, sampled:",
                *(f"  {count:3d}  {sample}" for sample, count in collections.Counter(samples).most_common()),
                "last log lines:",
                *(f"  {line}" for line in logged[-40:]),
            ]) + "\n"
            self.reported = True
            return [("idle", f"the main thread woke {rate:.0f} times a second while the app was idle "
                             f"(budget {self.BUDGET_PER_SECOND:.0f})")]
        return []

    def _repainted_regions(self, frames: list[str]) -> list[str]:
        """What the idle frames repainted: each frame's dirty bounds (the app logs
        them), most common first, with the UI elements under each -- the frame log
        said "dirty=1" and nothing about which surface kept asking."""
        bounds = collections.Counter(
            line.split("bounds=", 1)[1].split()[0] for line in frames if "bounds=" in line)
        if not bounds:
            return []
        try:
            elements = (self.app.ui() or {}).get("elements", [])
        except Exception:  # noqa: BLE001 - evidence only
            elements = []
        out = ["repainted bounds (x,y,w,h), most common first:"]
        for text, count in bounds.most_common(3):
            try:
                x, y, w, h = (int(v) for v in text.split(","))
            except ValueError:
                continue
            under = [e["name"] for e in elements
                     if len(e.get("rect") or ()) == 4 and e["rect"][0] < x + w and x < e["rect"][0] + e["rect"][2]
                     and e["rect"][1] < y + h and y < e["rect"][1] + e["rect"][3]]
            out.append(f"  {count:3d}  {text}  under: {', '.join(under[:12]) or '-'}")
        return out



class RemoteConvergence:
    """A remote project's mirror and the host's directory hold the same files once
    the connection has nothing left to do: the `remote` profile's oracle, after
    link cuts, server kills and changes on both sides.

    It waits for the project to settle (ready, nothing pending or unsent) and
    compares the two trees only then, and only when the editor has no conflict or
    held delete open -- those are divergence the user has been asked about. A
    project that does not settle at all is a finding of its own."""

    SETTLE_SECONDS = 90.0
    AGREE_SECONDS = 15.0

    def __init__(self, app: q.App, mirror: Path, host_root: Path, open_line: str):
        self.app = app
        self.mirror = mirror
        self.host_root = host_root
        # What brings the project back in front when the run left it (another
        # project switched to, this one closed): `remote-open host:root`.
        self.open_line = open_line
        self.evidence: dict[str, bytes] = {}
        # How each check ended, for the run's summary: an oracle that only ever
        # skipped (another project in front, a conflict open) checked nothing.
        self.outcomes: collections.Counter[str] = collections.Counter()

    def _remote(self) -> dict[str, Any]:
        """The status query's `remote`, which describes the ACTIVE project: with
        another project in front (the run switched, or closed this one) it is
        not this one's, and the answer is {"elsewhere": True}."""
        try:
            status = self.app.query("status")
        except q.QaFailure:
            return {}
        if Path(status.get("projectRoot") or "") != self.mirror:
            return {"elsewhere": True}
        return status.get("remote") or {}

    @staticmethod
    def settled(remote: dict[str, Any]) -> bool:
        return (remote.get("state") == "ready" and not remote.get("filesPending")
                and not remote.get("unsentPushes"))

    @staticmethod
    def differing(host: dict[str, str], mirror: dict[str, str]) -> list[str]:
        """Paths whose content differs, or that one side has and the other not.

        >>> RemoteConvergence.differing({"a": "1", "b": "2"}, {"a": "1", "b": "3", "c": "4"})
        ['b', 'c']
        """
        return sorted(k for k in set(host) | set(mirror) if host.get(k) != mirror.get(k))

    def check(self) -> list[tuple[str, str]]:
        import remote_host  # the monkey's other profiles never load it
        self.evidence = {}
        if not self.app.alive():
            return []
        deadline = time.monotonic() + self.SETTLE_SECONDS
        remote = self._remote()
        if remote.get("elsewhere"):
            # Not a verdict to skip: reopening it is a path of its own (the
            # project switched back to, or reopened after a close), and the
            # comparison is owed either way. Not recorded: a replay of the
            # oracle sends it again.
            self.outcomes["reopened"] += 1
            try:
                self.app.send_line(self.open_line, record=False)
            except q.QaFailure:
                pass
            remote = self._remote()
        if remote.get("state") == "disconnected":
            # The run disconnected on purpose (remote-disconnect, a menu row): the
            # comparison is owed once it is back, so the oracle asks for that. A
            # replay of the oracle sends it again; it is not recorded twice.
            try:
                self.app.send_line("remote-reconnect", record=False)
            except q.QaFailure:
                pass
        while not self.settled(remote) and time.monotonic() < deadline:
            time.sleep(0.5)
            remote = self._remote()
        if remote.get("elsewhere"):
            self.outcomes["elsewhere"] += 1
            return [("remote", f"the remote project is not in front {self.SETTLE_SECONDS:.0f} s after "
                               f"`{self.open_line.split()[0]}` asked for it")]
        if not self.settled(remote):
            # What the project says about itself: a refused push or a paused sync
            # posts a notification, and a screenshot cuts its text off.
            try:
                notices = self.app.query("notifications", {"recent": True})
            except q.QaFailure:
                notices = []
            self.outcomes["unsettled"] += 1
            self.evidence = {"remote-status.json": json.dumps(
                {"remote": remote, "notifications": notices}, indent=1).encode()}
            return [("remote", f"the remote project did not settle within {self.SETTLE_SECONDS:.0f} s "
                               f"(state {remote.get('state')}, {remote.get('filesPending')} pending, "
                               f"{remote.get('unsentPushes')} unsent)")]
        if remote.get("conflicts") or remote.get("heldDeletes"):
            self.outcomes["conflicted"] += 1
            return []
        if remote.get("blocked"):
            # Saves the host refused for the path itself (soak 464a40fee2bd: one onto
            # a dangling link, one over a file made unreadable there): not unsent --
            # `settled` holds -- and each has a row with Retry and Replace on Host.
            # Those files differ on purpose until the user chooses.
            self.outcomes["blocked"] += 1
            return []
        deadline = time.monotonic() + self.AGREE_SECONDS
        while True:
            host, mirror = remote_host.content_set(self.host_root, self.mirror)
            diff = self.differing(host, mirror)
            if not diff or time.monotonic() >= deadline:
                break
            time.sleep(0.5)
        if not diff:
            self.outcomes[f"agreed:{len(host)}"] += 1
            return []
        remote = self._remote()
        if remote.get("conflicts") or remote.get("heldDeletes") or remote.get("blocked") or \
                not self.settled(remote):
            self.outcomes["moved"] += 1
            return []  # something new arrived while it compared: not a verdict
        self.outcomes["differed"] += 1
        self.evidence = {"trees.txt": "\n".join(
            f"{path}: host {host.get(path, 'absent')[:12]} mirror {mirror.get(path, 'absent')[:12]}"
            for path in diff).encode()}
        first = diff[0]
        where = "only on the host" if first not in mirror else "only in the mirror" if first not in host \
            else "with different content"
        return [("remote", f"the settled mirror and the host differ in {len(diff)} file(s); "
                           f"{Path(first).name} is {where}")]
