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

import ast

import collections
import contextlib
import hashlib
import json
import os
import random
import re
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
# How long a trigger is given to reach its boundary: the flush's quiet and then
# some (how much more no test can tell).
POINT_KILL_WAIT_S = CRASH_SAFETY_QUIET_S + 8.0  # pragma: no mutate


def _forget_files_of(disk_before: dict[str, str | None], paths: set[str]) -> None:
    """Drops from `disk_before` every path that reaches the same file as one of
    `paths` (by the file it resolves to): a save through a link changes the file
    behind it, which a clean tab can show under its own name -- the crash point's
    edit, autosaved through link-to-crlf.txt, read as crlf.txt "changed on disk,
    and nothing saved it" (soak be74f3e72707)."""
    files = {os.path.realpath(path) for path in paths}
    for path in [path for path in disk_before if os.path.realpath(path) in files]:
        del disk_before[path]


_WRITE_LOGGED = re.compile(r"^microide buffer-io: write path=(.+?) doc=\S+ rev=\S+ dirty=\d bytes=\d+ "
                           r"sha=([0-9a-f]+)", re.MULTILINE)


_WRITING_LOGGED = re.compile(r"^microide buffer-io: writing path=(.+?) doc=\S+ rev=\S+ dirty=\d bytes=\d+ "
                             r"sha=([0-9a-f]+)", re.MULTILINE)


def _writings_logged(log: str) -> dict[str, set[str]]:
    """The bytes the app logged it was ABOUT to write, per file (sha256 prefixes):
    what a save killed part-way may have left in place of the old content."""
    found: dict[str, set[str]] = {}
    for match in _WRITING_LOGGED.finditer(log):
        found.setdefault(match.group(1), set()).add(match.group(2))
    return found


def _writes_logged(log: str) -> dict[str, str]:
    """The files the app logged saving in `log`, each with the sha256 prefix of
    what it wrote (the last write of a path wins)."""
    return {match.group(1): match.group(2) for match in _WRITE_LOGGED.finditer(log)}


def _trace_field(line: str, key: str) -> str:
    """The value after the first `key` of a trace line, up to the next space."""
    return (line.partition(key)[2].split() or [""])[0]


def _rule_site(rule: str) -> str:
    """The crash site a crash-points rule (`[action:]site[@file][#nth]`) names."""
    after_action = rule.partition(":")[2] if ":" in rule else rule
    return after_action.partition("@")[0].partition("#")[0]


def _past(deadline: float) -> bool:
    """The monotonic clock has reached `deadline` (a wait's end; whether the
    instant itself counts no test can tell)."""
    return time.monotonic() >= deadline  # pragma: no mutate


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

def _logged_history(app: Any, prefix: str, path: str, keep: int = 200) -> list[str]:
    """The app log's `prefix` lines about `path` (a ` path=<path> ` field), across
    the run's launches, newest last: what a trace flag recorded for one file."""
    marker = f" path={path} "
    work = getattr(app, "work", None)
    logs = sorted(Path(work).glob("microide*.log"), key=lambda p: p.stat().st_mtime) if work else []
    found: list[str] = []
    for log in logs:
        try:
            text = log.read_text(errors="replace")
        except OSError:
            continue
        found += [f"{log.name}: {line}" for line in text.splitlines() if prefix in line and marker in line]
    return found[-keep:]


def terminal_origins(log_text: str, alive: set[int], binary: Path | None = None) -> str:
    """Which code started each terminal shell still alive, from the app log's
    MICROIDE_TRACE_TERMINAL_LIFE lines: the alive pids grouped by the stack of
    their pty backend's start, each saying whether that backend was ever
    stopped. A leak finding counted the shells; this names the path that made
    the backends nobody stopped (soak 2d33d0bc377e)."""
    starts: dict[int, list[str]] = {}  # pid -> stack lines
    stopped: set[int] = set()
    current: list[str] | None = None
    for line in log_text.splitlines():
        if current is not None and line.startswith("  #"):
            current.append(line)
            continue
        current = None
        match = re.match(r"microide terminal-life: (\S+) pid=(-?\d+)", line)
        if match is None:
            continue
        event, pid = match.group(1), int(match.group(2))
        if event == "start":
            current = []
            starts[pid] = current
            stopped.discard(pid)
        elif event == "stop":
            stopped.add(pid)
    groups: dict[tuple[str, ...], list[str]] = collections.defaultdict(list)
    unknown = sorted(pid for pid in alive if pid not in starts)
    for pid in sorted(alive & starts.keys()):
        fate = "stopped, still alive" if pid in stopped else "never stopped"
        groups[tuple(starts[pid])].append(f"{pid} ({fate})")
    out = []
    for stack, pids in sorted(groups.items(), key=lambda item: -len(item[1])):
        out.append(f"{len(pids)} alive shell(s) started here: {' '.join(pids[:20])}"
                   + (" ..." if len(pids) > 20 else ""))
        if binary is not None:
            block = "\n".join(["microide terminal-life: start", *(re.sub(r"^\s*#\d+\s+", "", line) for line in stack)])
            out.append(q.symbolize_backtrace(block, binary).rstrip())
        else:
            out += list(stack)
    if unknown:
        out.append(f"{len(unknown)} alive child(ren) with no traced start: {' '.join(map(str, unknown[:20]))}")
    return "\n".join(out) + "\n" if out else ""


def _result_beside_its_files_buffer(doc: dict[str, Any], docs: list[dict[str, Any]]) -> bool:
    """Whether `doc` is a merge's unsaved result for a file whose own buffer (an
    editor's, a diff's side) also holds unsaved text. The session keeps the
    file's buffer on the file and brings the result back in an Untitled tab
    (soak 02922e8eed90): one path, two texts, and one buffer per path."""
    return doc.get("surface") == "merge" and any(
        other.get("surface") != "merge" and other.get("dirty") and _doc_key(other) == _doc_key(doc)
        for other in docs)


class DataOracle:
    """The data-loss oracle. Stateless between checks except for the snapshot a
    kill -9 compares against."""

    def __init__(self, app: q.App):
        self.app = app
        # The sha256 of the file a point kill's save trigger was about to write,
        # before it wrote: one of the two contents the file may hold afterwards.
        self.saved_file_before: str | None = None
        # Files for the evidence directory of the finding the last check made:
        # {file name: bytes}. The monkey writes them next to FINDING.md, so a
        # mismatch that does not replay still says how the two sides differ.
        self.evidence: dict[str, bytes] = {}
        self.untitled_evidence: dict[str, bytes] = {}
        # How each kill at a named crash point ended, for the run's summary:
        # `hit:<site>` (the app killed itself there) or `missed` (the trigger
        # passed no such boundary). An oracle that only ever missed checked
        # nothing new.
        self.point_outcomes: collections.Counter[str] = collections.Counter()
        self.known_points: set[tuple[str, str]] = set()
        self._opened = 0

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

    def check_untitled_names(self, docs: list[dict[str, Any]] | None = None) -> list[tuple[str, str]]:
        """Two different unsaved buffers of one project under one Untitled-N name.
        Each untitled buffer has a number of its own (a split shows ONE buffer in
        two panes, same text): a duplicate is the editor's own bookkeeping going
        wrong, and the kill -9 restore then brings both back with one text (soak
        93ba7c8703e3). Caught where it starts, not at the restore that loses one."""
        docs = self.documents() if docs is None else docs
        texts: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
        for doc in docs:
            if not doc.get("path") and doc.get("title"):
                texts[_doc_key(doc)].add(str(doc.get("sha256")))
        found = [("data", f"{len(shas)} different unsaved buffers are all titled {title}")
                 for (_, title), shas in sorted(texts.items()) if len(shas) > 1]
        if found:
            # Which tabs collide: their group, place and the number each holds,
            # beside every tab of the project (the documents query skips a tab
            # whose content has not landed, which still holds its number).
            untitled = [doc for doc in docs if not doc.get("path")]
            try:
                tabs = self.app.query("tabs")
            except Exception:  # noqa: BLE001 - evidence is best effort
                tabs = None
            self.untitled_evidence = {
                "untitled-documents.json": json.dumps(untitled, indent=1).encode(),
                "tabs.json": json.dumps(tabs, indent=1).encode(),
            }
        return found

    def check_clean(self, attempts: int = 4, gap: float = 0.3) -> list[tuple[str, str]]:
        """A clean document whose file differs, and keeps differing while the
        file stays the same for `attempts` reads -- an external rewrite the
        watcher has not reloaded yet is not a finding, a stable disagreement is."""
        docs = self.documents()
        self.untitled_evidence = {}
        names = self.check_untitled_names(docs)
        mismatched = self.clean_mismatches(docs)
        for _ in range(attempts - 1):
            if not mismatched:
                self.evidence = self.untitled_evidence
                return names
            time.sleep(gap * q.SLOW_FACTOR)
            again = self.clean_mismatches(self.documents())
            # Only what still differs from the SAME disk bytes.
            mismatched = {path: pair for path, pair in again.items()
                          if path in mismatched and mismatched[path][1] == pair[1]}
        self.evidence = self.untitled_evidence
        self.untitled_evidence = {}
        for path, pair in mismatched.items():
            self.evidence.update(self._mismatch_evidence(path, pair))
        return names + [("data", f"the clean buffer of {Path(path).name} differs from its file on disk "
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
        return _logged_history(self.app, "microide buffer-io:", path, keep)

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

    def crash_points(self) -> list[tuple[str, str]]:
        """(site, file) for every durable-write boundary the app has passed in
        this run (the `crash-points` query; each launch starts its count afresh,
        so they are kept across relaunches); empty from a binary without it."""
        try:
            answer = self.app.query("crash-points")
        except Exception:  # noqa: BLE001 - an older binary, or a dead or busy app
            answer = {}
        if answer.get("installed"):
            for site in answer.get("sites", []):
                for name in (site.get("files") or {}):
                    if site.get("site") and name:
                        self.known_points.add((site["site"], name))
        return sorted(self.known_points)

    def choose_point(self, rng: random.Random, tried: Any = None) -> str | None:
        """A crash point to kill at, as a rule with its trigger
        (`kill:<site>@<file>#1|edit`), the boundaries least tried first; None
        when the app has passed none (or cannot say).

        `tried(key)` is how often the pair was killed at before (the soak's
        coverage), so over many runs every boundary of every record is hit."""
        pairs = self.crash_points()
        if not pairs:
            return None
        weights = [1.0 / (1 + (tried(f"{site}@{name}") if tried is not None else 0)) for site, name in pairs]
        site, name = rng.choices(pairs, weights=weights)[0]
        return f"kill:{site}@{name}#1|{self._trigger_for(name)}"

    def _trigger_for(self, name: str) -> str:
        """What makes the app pass a boundary of the file the inventory calls
        `name` (`session`, `session.bak`, `recents (staging)`, `main.cpp`):

        edit     an edit arms the crash-safety flush, which rewrites the project's
                 session record (and syncs its directory)
        save     the file the focused editor is saved to
        open     the recents record: a file not opened before
        project  the workspace's session record: a switch of project"""
        record = name.removesuffix(" (staging)").removesuffix(".bak")
        if record == "recents":
            return "open"
        if record == "workspace-session":
            return "project"
        try:
            active = self.app.query("buffer")
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return "edit"
        if active.get("kind") == "editor" and active.get("path") and not active.get("readOnly") \
                and Path(active["path"]).name == record:
            return "save"
        return "edit"

    def _kill_at(self, point: str) -> tuple[bool, list[dict[str, Any]], str | None]:
        """Arms `point`'s rule, does its trigger and waits for the app to kill
        itself there. Returns (it died, the documents after the trigger and
        before the death, the path the trigger saved)."""
        app = self.app
        rule, _, trigger = point.partition("|")
        try:
            app.query("crash-points", {"arm": rule})
        except q.QaFailure:
            return False, [], None  # a binary without the query (an older build's replay)
        saved: str | None = None
        self.saved_file_before = None
        after_trigger: list[dict[str, Any]] = []
        try:
            # An edit the flush has not staged: the restore owes the buffer as it
            # was staged or as it is now, never neither.
            app.send_line("insert-line-below", record=False)
            after_trigger = self.documents(all_projects=True)
            if trigger == "save":
                active = app.query("buffer")
                if active.get("kind") == "editor" and active.get("path") and active.get("dirty"):
                    saved = str(active["path"])
                    self.saved_file_before = _sha256_file(Path(saved))
                    app.send_line("save", record=False)
            elif trigger == "open" and app.project is not None:
                # A file the recents list has not held: created, then opened.
                self._opened += 1
                fresh = Path(app.project) / f"qa-point-{self._opened}.txt"
                with contextlib.suppress(OSError):
                    fresh.write_text(f"{self._opened}\n")
                app.send_line(f"open {q.quote_arg(str(fresh))}", record=False)
            elif trigger == "project":
                app.send_line("project-next", record=False)
            limit = time.monotonic() + POINT_KILL_WAIT_S * q.SLOW_FACTOR
            while app.alive() and not app.dying() and not _past(limit):
                # Nothing owed to disk and nothing in flight: the trigger passed
                # no such boundary (no editor took the edit, the flush wrote
                # another record), and no later moment will.
                if app.query("persistence").get("staged"):
                    break
                time.sleep(0.05)
        except q.QaFailure:
            pass  # the app died under the request: what was asked for
        if app.dying():
            app.wait_gone(60.0)
        died = not app.alive()
        site = _rule_site(rule)
        self.point_outcomes[f"hit:{site}" if died else "missed"] += 1
        if not died:
            with contextlib.suppress(q.QaFailure):
                app.query("crash-points", {"disarm": True})
        return died, after_trigger, saved

    def kill9_and_check(self, rng: random.Random, waited: bool | None = None, point: str | None = None,
                        choose: Any = None) -> list[tuple[str, str]]:
        """SIGKILL, relaunch on the same profile, and compare. Waits for the
        crash-safety flush first (most kills), so every dirty buffer is owed back;
        a kill with no wait checks integrity only (an unstaged edit may be lost
        by design: CrashSafetyFlushSchedule).

        With `point` (choose_point) the kill is the app's own, at a named
        durable-write boundary (util/CrashPointControl.h) instead of wherever a
        signal from outside happens to land: after the flush has staged
        everything, the oracle arms the rule, makes one more edit (or saves) so
        the app passes the boundary, and the app dies there -- between the
        temporary's write and its rename, between the backup's rename and the
        record's. What it restores must be each buffer as it was staged or as
        the edit left it, never neither; a file a killed save was writing holds
        what it held or what the buffer had. The same rule kills at the same
        boundary in a replay, which a signal's instant never did.

        `choose()` picks the point instead, once the flush has run -- a launch's
        inventory of boundaries is empty until it has written something."""
        app = self.app
        if point is not None or choose is not None:
            waited = True
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
        if point is None and choose is not None:
            point = choose()
        before = self.documents(all_projects=True)
        disk_before = {doc["path"]: _sha256_file(Path(doc["path"])) for doc in before
                       if doc.get("path") and not doc.get("dirty")}
        app.record_external({"kill9": True, "waited": waited, **({"point": point} if point else {})})
        # The saves the killed process made after the snapshot, read from its log
        # before the relaunch starts another: path -> the sha256 prefix it wrote.
        log_mark = app._log_size() if hasattr(app, "_log_size") else None
        written: dict[str, str] = {}
        writing: dict[str, set[str]] = {}
        # The killed process's state tree (its session records, primary and .bak):
        # what the relaunch restored from. Lost buffers whose staging the log shows
        # left no other trace of which record was read or why it lacked them.
        work = getattr(app, "work", None)
        self.last_kill_state = Path(work) / "kill-state" if work else None

        def keep_state() -> None:
            if log_mark is not None:
                killed_log = app.log_since(log_mark, 16 * 1024 * 1024)
                written.update(_writes_logged(killed_log))
                for path, shas in _writings_logged(killed_log).items():
                    writing.setdefault(path, set()).update(shas)
            if self.last_kill_state is None:
                return
            shutil.rmtree(self.last_kill_state, ignore_errors=True)
            state = Path(work) / "state"
            if state.is_dir():
                shutil.copytree(state, self.last_kill_state, symlinks=True)

        # At a named boundary: the content each buffer may also come back with
        # (as the trigger's edit left it), the file its save was writing, and the
        # files of the buffers the trigger changed -- which a save or an autosave
        # may by then have written, as asked.
        newer: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
        saved_path: str | None = None  # pragma: no mutate
        touched: set[str] = set()
        killed_at = ""
        if point is not None:
            died, after_trigger, saved_path = self._kill_at(point)
            for doc in after_trigger:
                newer[_doc_key(doc)].add(str(doc.get("sha256")))
            was = {_doc_key(doc): doc.get("sha256") for doc in before}
            touched = {str(doc["path"]) for doc in after_trigger
                       if doc.get("path") and was.get(_doc_key(doc)) != doc.get("sha256")}
            if saved_path is not None:
                touched.add(saved_path)
            killed_at = f" (killed at {point.partition('|')[0].split(':', 1)[-1]})" if died else ""
            _forget_files_of(disk_before, touched)
        # With no project on the command line: the app restores its saved
        # workspace, as after a real crash. Naming the run's project opened that
        # project alone, and a second project's buffers, kept in its session
        # record, were reported lost (soak e946ec9333d7).
        app.crash_and_relaunch(between=keep_state, restore_workspace=True)
        dirty = [doc for doc in before if doc.get("dirty")]
        # Read what the relaunch restored BEFORE the lap that loads the other
        # projects: with `editor.autosave = on_focus_change` each project-next of
        # that lap is the user leaving an editor, and it saves the dirty buffers --
        # the oracle's own switch saved a restored buffer and then reported it as
        # coming back different (soak 0afa15c55c0f, once the relaunch itself no
        # longer saved them).
        # Only the ACTIVE project's buffers can be back before the lap: the others
        # load on their first activation. Waiting for all of them here ran to the
        # full deadline whenever a second project held a dirty buffer -- ten
        # seconds of every such kill (profiled 2026-11-13: a kill cost ~5 s on
        # average, a quarter of a 300-step run). The lap below waits for the rest.
        restored = self._wait_restored(self._of_active_project(dirty))
        # The projects the lap left with `editor.autosave = on_focus_change` (the
        # setting is per project: what the active one says after the lap is not
        # what the one it left had).
        saving = self._load_restored_projects(dirty)
        after = self._wait_restored(dirty) if saving is not None else restored  # pragma: no mutate
        by_first = {_doc_key(d): d for d in restored}
        after = [by_first.get(_doc_key(d), d) for d in after] + \
                [d for d in restored if _doc_key(d) not in {_doc_key(a) for a in after}]
        self.last_kill = {"before": before, "after": after}
        # The files a dirty buffer was saved to before the kill, by the file they
        # resolve to: back clean, and its file holds what it came back with. An
        # autosave (`after_delay`, or one the trigger's edit set off) may write every
        # dirty buffer between the snapshot and the kill -- the buffer was reported
        # "marked saved; its file does not hold it" without the file being read,
        # and a link to a file it wrote "changed on disk, and nothing saved it"
        # (soak 1bee2822b458, fd10a1441e1d).
        # The app's own record first: every file the killed process logged writing
        # after the snapshot, and that holds what it wrote, was saved -- whatever its
        # buffer's text was, and whether or not its content changed.
        logged_saves = {path: sha for path, sha in written.items()
                        if str(_sha256_file(Path(path))).startswith(sha)}
        if saved_path is not None and saved_path in writing:
            # A save killed part-way leaves the file as it was or as the save was
            # writing it (the bytes it logged before the write), never a third
            # thing: the durable write is a temporary and a rename.
            now = _sha256_file(Path(saved_path))
            if now != self.saved_file_before and not any(str(now).startswith(sha) for sha in writing[saved_path]):
                out.append(("data", f"a save of {Path(saved_path).name} killed part-way{killed_at} left the file "
                                    f"neither as it was nor as the save was writing it"))
        saved_before_kill: set[str] = {os.path.realpath(path) for path in logged_saves}
        if waited:
            by_key = {_doc_key(d): d for d in after}
            for doc in dirty:
                key = doc.get("path") or doc.get("title")
                if _result_beside_its_files_buffer(doc, dirty):
                    # Not the file's text until it is kept: it comes back in an
                    # Untitled tab, and the file's own buffer on the file.
                    shas = {doc.get("sha256"), *newer.get(_doc_key(doc), ())}
                    if not any(not d.get("path") and d.get("dirty") and d.get("sha256") in shas and
                               d.get("project") == doc.get("project") for d in after):
                        out.append(("data", f"the unsaved merge result for {Path(str(key)).name} did not come back "
                                            f"after a kill -9 (its file's own buffer holds other unsaved text)"
                                            f"{killed_at}"))
                    continue
                back = by_key.get(_doc_key(doc))
                if back is None and doc.get("path") and doc.get("sha256") and \
                        _sha256_file(Path(doc["path"])) == doc["sha256"]:
                    # Saved before the kill -- an autosave the kill landed in or after
                    # -- so the relaunch reopened the tab clean from its file and
                    # deferred, and `documents` lists no pending tab (soak 93826423aa06).
                    continue
                if doc.get("path") and (sha := logged_saves.get(str(doc["path"]))) and \
                        (back is None or (not back.get("dirty") and str(back.get("sha256")).startswith(sha))):
                    # The same, through the save's transforms: the autosave added a
                    # final newline or trimmed trailing whitespace, so neither the file
                    # nor the buffer that came back clean is the snapshot's text -- they
                    # are what the app logged writing (soak 4e1229a05d76's family, in
                    # 1bee2822b458's run; rtl.txt 4103 -> 4104 bytes in fd10a1441e1d's).
                    continue
                if back is None:
                    out.append(("data", f"the unsaved buffer {Path(str(key)).name} did not come back after a kill -9 "
                                        f"(the crash-safety flush had {CRASH_SAFETY_QUIET_S:.1f}s of quiet)"
                                        f"{killed_at}"))
                elif not back.get("dirty") and saving and doc.get("project") in saving and \
                        _doc_key(doc) not in by_first:
                    continue  # back, then saved by the lap's own project switch, as configured
                elif doc.get("path") in touched and not back.get("dirty"):
                    # A buffer the trigger changed, back clean: a save -- the
                    # trigger's, or an autosave the edit set off -- wrote it
                    # before the kill, and then the file holds it. Its file is
                    # out of the disk check already (`touched`).
                    if _sha256_file(Path(str(doc["path"]))) != back.get("sha256"):
                        out.append(("data", f"the unsaved buffer {Path(str(key)).name} came back after a kill -9 "
                                            f"marked saved; its file does not hold it{killed_at}"))
                elif doc.get("path") and doc.get("path") == saved_path:
                    # Still unsaved, in the file a killed save was writing: a save
                    # may change the buffer on its way (trailing whitespace, a
                    # final newline, a formatter), so its text is not held to the
                    # staged or the edited one.
                    continue
                elif back.get("sha256") not in {doc.get("sha256"), *newer.get(_doc_key(doc), ())}:
                    out.append(("data", f"the unsaved buffer {Path(str(key)).name} came back after a kill -9 with "
                                        f"different content ({doc.get('bytes')} -> {back.get('bytes')} bytes)"
                                        f"{killed_at}"))
                elif not back.get("dirty"):
                    if doc.get("path") and _sha256_file(Path(str(doc["path"]))) == back.get("sha256"):
                        saved_before_kill.add(os.path.realpath(str(doc["path"])))
                        continue
                    out.append(("data", f"the unsaved buffer {Path(str(key)).name} came back after a kill -9 "
                                        f"marked saved; its file does not hold it{killed_at}"))
        # The files a restored dirty buffer was saved to (the lap's autosave), by
        # the file they resolve to: a clean tab can reach the same file by another
        # path -- a project rooted at qa-weird/loop, a link to qa-weird -- and the
        # save through the buffer's path changed the clean tab's file with it
        # (soak ce9c3a0386c3).
        # The same rule as the buffer check above: back clean, in a project the
        # lap left with autosave on, and not already back before the lap.
        back_by_key = {_doc_key(d): d for d in after}
        saved_files = {os.path.realpath(doc["path"]) for doc in dirty
                       if doc.get("path") and saving and doc.get("project") in saving
                       and _doc_key(doc) not in by_first
                       and (back := back_by_key.get(_doc_key(doc))) is not None
                       and not back.get("dirty")} | saved_before_kill
        for path, sha in disk_before.items():
            if os.path.realpath(path) in saved_files:
                continue
            now = _sha256_file(Path(path))
            if sha is not None and now is not None and now != sha and not self._churned(path):
                out.append(("data", f"{Path(path).name} changed on disk across a kill -9 and relaunch, "
                                    f"and nothing saved it"))
        return out

    def _wait_restored(self, dirty: list[dict[str, Any]]) -> list[dict[str, Any]]:
        """Every project's documents, once the dirty ones are back (a restore lands
        over a few frames) or after a bounded wait."""
        deadline = time.monotonic() + 10 * q.SLOW_FACTOR
        after = self.documents(all_projects=True)
        while time.monotonic() < deadline:
            keys = {_doc_key(d) for d in after}
            if all(_doc_key(d) in keys for d in dirty):
                break
            time.sleep(0.2)
            after = self.documents(all_projects=True)
        return after

    def _of_active_project(self, docs: list[dict[str, Any]]) -> list[dict[str, Any]]:
        """The documents of the project now in front (`status`'s projectRoot); all
        of them when that cannot be asked, as before."""
        try:
            root = str(self.app.query("status").get("projectRoot") or "")
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return docs
        if not root:
            return docs
        return [doc for doc in docs if str(doc.get("project") or root) == root]

    def _autosave_mode(self) -> str | None:
        try:
            rows = self.app.query("settings")
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return None
        for row in rows if isinstance(rows, list) else []:
            if row.get("id") == "editor.autosave":
                return str(row.get("value"))
        return None

    def _load_restored_projects(self, dirty: list[dict[str, Any]]) -> set[str] | None:
        """A relaunch restores every project of the workspace, but only the active
        one loads its tabs; the others load on their first activation, and until
        then `documents` lists nothing of them -- their unsaved buffers read as
        lost (soak 55673209c35c). When a project owing one has not loaded, one lap
        of project-next activates each project once and ends where it began.

        None when there was no lap; else the roots it LEFT with
        `editor.autosave = on_focus_change`, whose dirty buffers that switch saved."""
        owed = {doc.get("project") for doc in dirty if doc.get("project")}
        if not owed:
            return None
        try:
            projects = self.app.query("projects")
        except Exception:  # noqa: BLE001 - a dead or busy app is another oracle's
            return None
        if not isinstance(projects, list):
            return None
        # `loaded` is absent from a binary that predates it: nothing to wait for.
        if not any(p.get("loaded") is False and p.get("root") in owed for p in projects):
            return None
        saving: set[str] = set()
        try:
            for _ in projects:
                if self._autosave_mode() == "on_focus_change":
                    now = next((p.get("root") for p in self.app.query("projects") if p.get("active")), None)
                    if now:
                        saving.add(now)
                self.app.send_line("project-next")
        except Exception:  # noqa: BLE001 - as above
            pass
        return saving

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
        relative = _project_relative(self.app, path)
        recent = self.app.actions[-20:]
        return any(row.get("churn") == relative or "external" in row for row in recent)


def _project_relative(app: q.App, path: str) -> str:
    project = str(app.project or "")
    return path[len(project) + 1:] if project and path.startswith(project + "/") else path


# How long after the churn writes a file an open clean buffer may still reload
# it: inotify reports at once, the FileTreeWatcher poll fallback within its
# 750 ms interval, and the reload lands on the next frame.
CHURN_RELOAD_GRACE_S = 1.5


# The buffers whose history the undo rule holds for: an editor's, a diff's
# editable side and a merge's result (the `buffer` query's kinds). It was asked of
# editor buffers alone, so the two histories that interleave edits with model
# rebuilds -- a diff re-derived per keystroke, a merge choice patched into typed
# text -- were the ones it never saw.
UNDO_KINDS = frozenset({"editor", "compare", "merge"})

# How long the undo rule waits for the language server's outstanding answers
# before it reads the text (scaled by the slow factor). A format or a rename
# answered after that read lands as an edit of its own, which the undos then
# take back and the redos put back: the text comes out as format(before), from a
# history that was exact (soak 76e3925879dc).
LSP_ANSWER_WAIT_S = 3.0


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
        if not self._lsp_answers_landed():
            return []  # an edit may still be on its way: not this rule's case
        try:
            before = app.query("buffer")
        except q.QaFailure:
            return []
        if before.get("kind") not in UNDO_KINDS or before.get("readOnly") or "text" not in before:
            return []
        # Only the undos that undid something are owed a redo: at the bottom of the
        # history an undo does nothing, and the k redos then replayed edits the run
        # had undone earlier -- a changed text from a correct editor (soak
        # 1aa1784d1685, after twenty Ctrl+Z). An undo that applied moves the
        # buffer's content revision.
        asked = self.pick() if asked is None else asked
        started = time.monotonic()
        k = 0
        revision = before.get("revision")
        for _ in range(asked):
            try:
                app.send_line("undo")
                now = app.query("buffer")
            except q.QaFailure:
                return []
            if now.get("path") != before.get("path") or now.get("kind") != before.get("kind"):
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
        if after.get("path") != before.get("path") or after.get("kind") != before.get("kind"):
            return []  # the commands moved focus somewhere else: not this rule's case
        if after.get("text") != before.get("text"):
            if self._churned_during_check(before.get("path"), started):
                # The monkey's ExternalChurn rewrote this file under the clean
                # buffer while the commands ran, and the buffer reloaded it: the
                # text moved for a reason that is not undo or redo (soak
                # d6710c671734, "churn 13\n" prepended between two undos).
                return []
            self.evidence = {"undo-before.txt": str(before["text"]).encode(),
                             "undo-after.txt": str(after.get("text") or "").encode()}
            return [("undo", f"{k} undo(s) then {k} redo(s) changed the text of "
                             f"{Path(before.get('path') or 'untitled').name} "
                             f"({len(before['text'])} -> {len(after.get('text') or '')} chars)")]
        return []

    def _lsp_answers_landed(self) -> bool:
        """Whether the app has no language-server request outstanding (the
        `status` query's `lspRequestsInFlight`), waiting up to LSP_ANSWER_WAIT_S.
        A request counts until its answer has been applied or dropped, so a
        format-document's edits are in the buffer once this is true. A binary
        without the field -- an old finding's replay -- reads as none."""
        deadline = time.monotonic() + LSP_ANSWER_WAIT_S * q.SLOW_FACTOR
        while True:
            try:
                in_flight = int(self.app.query("status").get("lspRequestsInFlight") or 0)
            except q.QaFailure:
                return False
            if in_flight == 0:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.02)

    def _churned_during_check(self, path: str | None, started: float) -> bool:
        """A churn row for `path` from the reload grace before the check began
        on: ExternalChurn records each row before its write, so a rewrite the
        check could have seen is already in the list."""
        if not path:
            return False
        relative = _project_relative(self.app, path)
        since = started - CHURN_RELOAD_GRACE_S
        return any(row.get("churn") == relative and row.get("t", since) >= since
                   for row in reversed(self.app.actions[-64:]))


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
        # Files for a leak finding's bundle: what the app's children are and what
        # its descriptors point at. The counts alone said "grew by 108" and
        # nothing about what (soak d1e39df0281e, 6da9bc4a526a).
        self.evidence: dict[str, bytes] = {}
        # The app's thread names at the baseline, so a thread finding says which
        # kind of thread there are more of (`threads.txt`).
        self.baseline_threads: collections.Counter[str] = collections.Counter()
        # (monkey step, length of the action list, rss_kb) after every step, for the
        # finding's rss-timeline.txt: the samples every 50 steps said THAT 3.5 GB
        # were held after 150 steps, and nothing said which action took them
        # (soak e53948e728c8). One /proc read per step.
        self.rss_steps: list[tuple[int, int, int]] = []
        self.rss_pid: int | None = None

    def note_step(self, step: int) -> None:
        pid = self.app.pid if self.app.proc is not None and self.app.proc.poll() is None else None
        if pid is None:
            return
        if pid != self.rss_pid:
            self.rss_pid = pid
            self.rss_steps.clear()  # a relaunch: the new process starts afresh
        with contextlib.suppress(OSError, ValueError, IndexError):
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    self.rss_steps.append((step, len(self.app.actions), int(line.split()[1])))
                    break

    def rss_timeline(self, jumps: int = 5, around: int = 3) -> str:
        """The run's rss after each step, thinned to 40 rows, then the steps with
        the largest growth, each with the actions recorded since the step before:
        which action took the memory."""
        if len(self.rss_steps) < 2:
            return ""
        stride = max(1, len(self.rss_steps) // 40)
        out = ["step  actions  rss_kb"]
        out += [f"{step:5d}  {actions:7d}  {rss}" for step, actions, rss in self.rss_steps[::stride]]
        deltas = sorted(((self.rss_steps[i][2] - self.rss_steps[i - 1][2], i) for i in range(1, len(self.rss_steps))),
                        reverse=True)[:jumps]
        actions = getattr(self.app, "actions", [])
        for delta, i in deltas:
            if delta <= 0:
                break
            (step0, from_action, _), (step1, to_action, _) = self.rss_steps[i - 1], self.rss_steps[i]
            out.append(f"\n+{delta} kB between step {step0} and step {step1} (actions {from_action}..{to_action}):")
            for row in actions[max(0, from_action - around):to_action]:
                text = json.dumps(row, ensure_ascii=False)
                out.append("  " + (text if len(text) <= 300 else text[:300] + f"... ({len(text)} chars)"))
        return "\n".join(out) + "\n"

    @staticmethod
    def _thread_names(pid: int) -> collections.Counter[str]:
        names: collections.Counter[str] = collections.Counter()
        with contextlib.suppress(OSError):
            for task in Path(f"/proc/{pid}/task").iterdir():
                with contextlib.suppress(OSError):
                    names[(task / "comm").read_text().strip()] += 1
        return names

    @staticmethod
    def memory_report(rollup: str, smaps: str) -> str:
        """`memory.txt`: the totals of smaps_rollup that say where the resident
        memory is, then the twelve mappings with the most of it, grouped by what
        they map ([heap], an anonymous region, a file)."""
        totals = ("Rss", "Pss", "Anonymous", "Swap", "Private_Dirty")
        lines = [line for line in rollup.splitlines() if line.partition(":")[0] in totals]
        by_name: collections.Counter[str] = collections.Counter()
        name = "?"
        for line in smaps.splitlines():
            head = line.split()
            if head and "-" in head[0] and ":" not in head[0] and len(head) >= 5:
                name = head[5] if len(head) >= 6 else "[anonymous]"
            elif line.startswith("Rss:"):
                by_name[name] += int(line.split()[1])
        lines.append("largest resident mappings (kB):")
        lines += [f"  {kb:10d}  {mapped}" for mapped, kb in by_name.most_common(12)]
        return "\n".join(lines) + "\n"

    @staticmethod
    def _memory(pid: int) -> str:
        texts = []
        for name in ("smaps_rollup", "smaps"):
            try:
                texts.append(Path(f"/proc/{pid}/{name}").read_text())
            except OSError:
                texts.append("")  # pragma: no mutate
        return ResourceOracle.memory_report(*texts)

    @staticmethod
    def children_report(children: list[tuple[int, str, bytes]], uptime: float, ticks: int) -> str:
        """`children.txt`: each child (pid, its /proc stat, its raw cmdline) with its
        state, age and command line, under a count of them by program."""
        lines = ["pid state age_s  command"]
        kinds: collections.Counter[str] = collections.Counter()
        for child, stat, cmdline in children:
            fields = stat[stat.rfind(")") + 2:].split()
            if len(fields) <= 19 or not fields[19].isdigit():
                continue
            age = uptime - int(fields[19]) / ticks
            argv = cmdline.replace(b"\0", b" ").decode(errors="replace").strip()
            lines.append(f"{child} {fields[0]} {age:8.1f}  {argv[:200]}")
            kinds[Path(argv.partition(" ")[0]).name or "?"] += 1
        lines.insert(0, f"{len(children)} direct children by program: {dict(kinds.most_common())}")
        return "\n".join(lines) + "\n"

    @staticmethod
    def fd_kind(target: str) -> str:
        """What a descriptor points at, as fds.txt counts it: a pseudo-file's kind
        (`pipe`, `socket`, `anon_inode`), the terminal, a device, or a file by its
        extension."""
        if ":" in target and not target.startswith("/"):
            return target.partition(":")[0]
        if target.startswith("/dev/pts"):
            return "/dev/pts"
        if target.startswith("/dev"):
            return target
        return "file:" + Path(target).suffix

    @staticmethod
    def fds_report(targets: list[str]) -> str:
        kinds = collections.Counter(ResourceOracle.fd_kind(target) for target in targets)
        return "\n".join(f"{n:5d}  {kind}" for kind, n in kinds.most_common()) + "\n"

    @staticmethod
    def threads_report(now: collections.Counter[str], baseline: collections.Counter[str]) -> str:
        lines = ["threads by name: now (at the baseline, change)"]
        for thread, count in now.most_common():
            was = baseline.get(thread, 0)
            lines.append(f"  {count:4d}  ({was:4d}, {count - was:+d})  {thread}")
        for thread, was in baseline.items():
            if thread not in now:
                lines.append(f"     0  ({was:4d}, {-was:+d})  {thread}")
        return "\n".join(lines) + "\n"

    @staticmethod
    def _uptime(path: Path = Path("/proc/uptime")) -> float:
        """Seconds since boot, the clock /proc/<pid>/stat's start times count on;
        0.0 when it cannot be read."""
        try:
            return float(path.read_text().split()[0])
        except (OSError, ValueError, IndexError):
            return 0.0

    @staticmethod
    def _child_pids(pid: int) -> list[int]:
        try:
            return [int(c) for task in Path(f"/proc/{pid}/task").iterdir()
                    for c in (task / "children").read_text().split()]
        except (OSError, ValueError):
            return []

    @staticmethod
    def _read_child(pid: int) -> tuple[int, str, bytes] | None:
        try:
            return pid, Path(f"/proc/{pid}/stat").read_text(), Path(f"/proc/{pid}/cmdline").read_bytes()
        except OSError:
            return None

    @staticmethod
    def _fd_targets(pid: int) -> list[str]:
        targets = []
        try:
            descriptors = list(Path(f"/proc/{pid}/fd").iterdir())
        except OSError:
            return []
        for fd in descriptors:
            try:
                targets.append(os.readlink(fd))
            except OSError:
                continue
        return targets

    def _what_it_holds(self, pid: int) -> dict[str, bytes]:
        pids = self._child_pids(pid)
        children = [child for child in map(self._read_child, pids) if child is not None]
        out = {"children.txt": self.children_report(children, self._uptime(), os.sysconf("SC_CLK_TCK")).encode(),
               "fds.txt": self.fds_report(self._fd_targets(pid)).encode(),
               "threads.txt": self.threads_report(self._thread_names(pid), self.baseline_threads).encode(),
               "memory.txt": self._memory(pid).encode()}
        work = getattr(self.app, "work", None)
        logs = sorted(Path(work).glob("microide*.log"), key=lambda p: p.stat().st_mtime) if work else []
        log_text = ""
        for log in logs:
            with contextlib.suppress(OSError):
                log_text += log.read_text(errors="replace")
        binary = getattr(self.app, "binary", None)
        origins = terminal_origins(log_text, set(pids), Path(binary) if binary else None)
        if origins:
            out["children-origin.txt"] = origins.encode()
        if timeline := self.rss_timeline():
            out["rss-timeline.txt"] = timeline.encode()
        return out

    def sample(self) -> dict[str, int] | None:
        proc = self.app.proc
        if proc is None or proc.poll() is not None:
            return None
        pid = self.app.pid  # the app's own, not a recorder's (App.pid)
        if pid is None:
            return None
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
            self.baseline_threads = self._thread_names(pid)
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
                if not self.evidence and self.pid is not None:
                    self.evidence = self._what_it_holds(self.pid)
                out.append(("leak", f"the app's {name} grew by {grown} over the run "
                                    f"({self.baseline[name]} -> {sample[name]}; budget {budget}"
                                    + (f", plus {held} for {opened} more open terminal pane(s)" if held else "")
                                    + ")"))
        return out


def latency_subject(what: str) -> str:
    """What a slow answer was to, as its finding names it: a query by its verb, a
    command line by its command -- never by the line.

    `what` is the request as the driver labels it: `query 'ui'`, `hit-test`, or
    a command line's repr. The finding quoted that whole, and its signature
    masks a quoted text only up to 200 characters: `type` with a long argument
    was a signature per text typed (five of them open at once, for the one
    defect that a long line types slowly), while every other slow command, of
    whatever kind, shared the single `<q> took <n>s`.

    >>> latency_subject("query 'ui'"), latency_subject("hit-test")
    ('the ui query', 'hit-test')
    >>> latency_subject(repr('type "word word word"')), latency_subject(repr("format-document"))
    ('the type command', 'the format-document command')
    >>> latency_subject(repr("it's")), latency_subject("''")
    ('the it command', 'an empty command')
    """
    query = re.fullmatch(r"query '([\w-]+)'", what)
    if query:
        return f"the {query.group(1)} query"
    if what[:1] in "'\"" and what[-1:] == what[:1] and len(what) >= 2:
        try:
            line = ast.literal_eval(what)
        except (ValueError, SyntaxError):
            line = what[1:-1]
        verb = re.match(r"[\w-]+", str(line).lstrip())
        return f"the {verb.group(0)} command" if verb else "an empty command"
    return what


class Liveness:
    """Every control answer within `budget` seconds (stretched by the slow
    factor). One finding per run per query verb or command (latency_subject).

    Whose seconds they were is read off the app's main thread (App.note_latency:
    /proc's schedstat around the request). A thread that was runnable and not
    run was starved by the machine -- the soak fills every core on purpose --
    and that is not the app's latency: it is counted (`starved`) and not filed.
    The 764 sightings of "query took Ns to answer" could not be told from that.
    What is filed says where the time went and carries the stack the app's
    stall watchdog printed (`evidence`)."""

    def __init__(self, app: q.App, budget: float = 2.5):
        self.app = app
        self.budget = budget * q.SLOW_FACTOR
        self.reported: set[str] = set()
        self.starved = 0
        self.evidence: dict[str, bytes] = {}

    def check(self) -> list[tuple[str, str]]:
        what, seconds = self.app.slowest_answer
        sched, stall = self.app.slowest_sched, self.app.slowest_stall
        waits = getattr(self.app, "slowest_waits", "")  # pragma: no mutate
        helpers = getattr(self.app, "slowest_helpers", None)
        profile = getattr(self.app, "slowest_profile", "")  # pragma: no mutate
        self.app.slowest_helpers = None
        self.app.slowest_profile = ""
        self.app.slowest_answer = ("", 0.0)
        self.app.slowest_sched, self.app.slowest_stall = None, ""
        self.app.slowest_waits = ""
        self.evidence = {}
        subject = latency_subject(what)
        if seconds <= self.budget or subject in self.reported:
            return []
        where = ""
        account = ""  # pragma: no mutate
        if sched is not None:
            ran, queued = sched
            if q.is_starved(seconds, queued):
                self.starved += 1
                return []
            account = q.sched_account(seconds, ran, queued)
            blocked = max(0.0, seconds - ran - queued)
            if blocked > ran and q.helpers_starved(helpers):
                # Blocked on the app's own threads, which the machine starved: the
                # soak's CPU renderer under full load (soak 049ca74b0cee, after the
                # lost-wake fix: every remaining sighting in libvulkan_lvp).
                self.starved += 1
                return []
            where = "; its main thread was mostly " + ("computing" if ran >= blocked else
                                                       "blocked (I/O, a lock, a child)")
        self.reported.add(subject)
        # The request itself, whole, is evidence: which line, how long a text.
        self.evidence = {"latency.txt": "\n".join(filter(None, [
            f"{what} took {seconds:.2f}s (budget {self.budget:.1f}s)", account,
            "", "the app's stall watchdog during it (MICROIDE_TRACE_STALL_MS):" if stall else
            "the app's stall watchdog printed nothing during it (no single loop turn ran that long, "
            "or the binary predates the watchdog)", stall,
            "", "where the main thread sat while the answer was late (wchan, syscall nr and first arg; "
                "sampled every 0.2s):" if waits else "", waits])).encode() + b"\n"}
        if profile:
            # perf of the main thread while the answer was late: what it computed.
            self.evidence["latency-profile.txt"] = profile.encode()
        return [("latency", f"{subject} took {seconds:.2f}s to answer (budget {self.budget:.1f}s){where}")]


_FRAME_LINE = re.compile(r"microide redraw frame: frame=(\d+) reason=(\S+) requested=(\S+) rendered=(\S+) "
                         r".*? elapsed=([0-9.]+) ms")


class FrameLatency:
    """A frame that took longer than `budget` to prepare and paint, read off the
    app's redraw trace (`microide redraw frame: ... elapsed=N ms`): the time
    between the input and the picture, which an answer's latency (Liveness)
    does not see when the slow part is the paint. One finding per run per frame
    reason; the redraw trace before it is the evidence."""

    def __init__(self, app: q.App, budget: float = 0.5):
        self.app = app
        self.budget_ms = budget * 1000.0 * q.SLOW_FACTOR
        self.offset: int | None = None
        self.reported: set[str] = set()
        self.evidence: dict[str, bytes] = {}

    def check(self) -> list[tuple[str, str]]:
        self.evidence = {}
        size = self.app._log_size()
        if self.offset is None or size < self.offset:  # pragma: no mutate
            self.offset = size
            return []
        text = self.app.log_since(self.offset, 4 * 1024 * 1024)
        self.offset = size
        lines = text.splitlines()
        for index, line in enumerate(lines):
            match = _FRAME_LINE.search(line)
            if match is None or float(match.group(5)) <= self.budget_ms:
                continue
            reason = match.group(2)
            if reason in self.reported:
                continue
            self.reported.add(reason)
            self.evidence = {"frame-latency.txt": "\n".join(lines[max(0, index - 40):index + 1]).encode() + b"\n"}
            return [("latency", f"a {reason} frame took {float(match.group(5)):.0f} ms to prepare and paint "
                                f"({match.group(3)} requested, {match.group(4)} rendered; budget "
                                f"{self.budget_ms:.0f} ms)")]
        return []


def caret_bounds_reports(log: str, binary: Path | None = None) -> str:
    """Each `microide caret-bounds:` report in `log` with its backtrace lines;
    with `binary`, the frames resolved to function and file:line as a crash
    stack's are (the raw `microide(+0x...)` offsets cost a hand addr2line run to
    name ReplaceAllRanges in soak 872c8984d39b).

    >>> caret_bounds_reports("x\\nmicroide caret-bounds: here: f has its caret at 2:3\\n  #1 a\\n  #2 b\\ny\\n")
    'microide caret-bounds: here: f has its caret at 2:3\\n  #1 a\\n  #2 b\\n'
    """
    reports: list[list[str]] = []
    taking = False
    for line in log.splitlines():
        if "microide caret-bounds:" in line:
            taking = True
            reports.append([line])
        elif taking and line.startswith("  #"):
            reports[-1].append(line)
        else:
            taking = False
    if binary is None:
        return "".join(line + "\n" for report in reports for line in report)
    # The app numbers its frames ("  #1 module(+0x...) [0x...]"); the resolver
    # reads bare backtrace_symbols lines.
    return "".join(q.symbolize_backtrace("\n".join([report[0], *(re.sub(r"^\s*#\d+\s+", "", line)
                                                                   for line in report[1:])]), binary)
                   for report in reports)


class InvariantOracle:
    """The workspace's structural rules, asked of the app itself (the
    `invariants` query, ControlInvariantsQuery.h): groups against the split
    tree, tabs against their payloads, one Untitled number per buffer, one
    buffer per file, every caret inside its buffer, and (deep) the focused
    editor's layout tables against a fresh build.

    Asked every few steps, so a broken rule is found within a few actions of
    the one that broke it -- the symptom-reading oracles find it when it shows,
    which for the duplicated Untitled-N was a kill -9 hundreds of steps later."""

    def __init__(self, app: q.App):
        self.app = app
        self.reported: set[str] = set()
        self.evidence: dict[str, bytes] = {}
        self.unavailable = False
        self.checks = 0

    def check(self, deep: bool = False) -> list[tuple[str, str]]:
        self.evidence = {}
        if self.unavailable:
            return []
        try:
            answer = self.app.query("invariants", {"deep": deep})
        except q.AppGone:
            return []
        except q.QaFailure as failure:
            if "unknown query" in str(failure):
                self.unavailable = True  # a binary from before the query (an old finding's replay)
            return []
        self.checks += 1
        found = []
        for violation in answer.get("violations") or []:
            rule, detail = str(violation.get("rule")), str(violation.get("detail"))
            if rule in self.reported:
                continue
            self.reported.add(rule)
            found.append(("invariant", f"{rule}: {detail}"))
        if found:
            self.evidence = {"invariants.json": json.dumps(answer, indent=1).encode()}
            for name, verb, args in (("tabs.json", "tabs", None), ("documents.json", "documents", {"all": True}),
                                     ("editor.json", "editor", None)):
                with contextlib.suppress(Exception):
                    self.evidence[name] = json.dumps(self.app.query(verb, args), indent=1).encode()
            # The app's own report of the first caret it left outside a document,
            # with the stack that left it (MICROIDE_TRACE_CARET_BOUNDS) -- likely
            # steps before this check, and out of the log tail's reach.
            with contextlib.suppress(Exception):
                if reports := caret_bounds_reports(self.app.log_since(0, 8 * 1024 * 1024),
                                                   getattr(self.app, "binary", None)):
                    self.evidence["caret-bounds.txt"] = reports.encode()
        return found


def _utf8_boundaries(raw: bytes) -> set[int]:
    """The offsets in `raw` that are not inside a character: every well-formed
    UTF-8 sequence is one character, and every byte that is not part of one --
    a stray continuation byte in binary data, a cut-off lead -- is one on its own.
    Reading every 0x80-0xBF byte as "inside a character" called a span beside a
    lone 0x97 a split (soak 1b3aae58e0b7, 883 sightings on binary lines)."""
    out = {0}
    offset = 0
    for character in raw.decode("utf-8", "surrogateescape"):  # pragma: no mutate
        size = len(character.encode("utf-8", "surrogateescape"))  # pragma: no mutate
        offset += size
        out.add(offset)
    return out


class StructuralDiffOracle:
    """What the compare view's structural refinement claims, held against what it
    shows (the `compare` query: rows, hunks and each side's novel byte spans).

    - spans: inside their line, sorted, non-overlapping, on UTF-8 boundaries, and
      only where the row has that side and is not unchanged;
    - layout only: a hunk whose two sides are the same tokens (words and single
      punctuation characters) marks no token -- re-indenting, re-wrapping or re-spacing code costs
      nothing (checked where no string or comment could make whitespace matter);
    - a real change shows: a hunk whose sides differ beyond whitespace marks a
      token, or adds or deletes a row with text -- never "layout only".

    Rows with cut text (`textTruncated`) are skipped, and a cut row list is
    checked only up to the cut, hunk by complete hunk."""

    _QUOTES_OR_COMMENTS = ('"', "'", "//", "/*", "#")
    # The diff's own token rule (src/compare/DiffSyntax.cpp): a word, a run of
    # ASCII operator characters as ONE token (`->`, `+=`, `.-`), or any other
    # single non-space character -- a bracket, a quote, `,` or `;`, a non-ASCII
    # symbol. Splitting every operator character, as this did, called deleting
    # the space in `-S . -B` layout only, though it fused `.` and `-` into the
    # operator `.-`: a real change of token, rightly marked (soak 96055d2c8eec,
    # 2026-11-21). Comments and strings, whose lexing differs by language, are
    # skipped below anyway.
    _TOKEN = re.compile(r"\w+|[!#%&*+\-./:<=>?@\\^|~]+|[^\w\s]")

    @classmethod
    def _tokens(cls, text: str) -> list[str]:
        return cls._TOKEN.findall(text)

    def __init__(self, app: q.App):
        self.app = app
        self.evidence: dict[str, bytes] = {}

    def check(self) -> list[tuple[str, str]]:
        self.evidence = {}
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
            if (hunk := row.get("hunk")) is not None and hunk >= 0:
                hunks[hunk].append(row)
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
                boundaries = _utf8_boundaries(raw)
                for start, end in spans:
                    if not (last <= start < end <= len(raw)):
                        found.append(("diff", f"the {side} side of {where} has a novel span [{start}, {end}) "
                                              f"outside its {len(raw)}-byte line or out of order"))
                        break
                    if any(at not in boundaries for at in (start, end)):
                        found.append(("diff", f"the {side} side of {where} has a novel span [{start}, {end}) "
                                              f"that splits a character"))
                        # The row as the query gave it, and the bytes the check
                        # read: a split is the app's or this check's, and only
                        # the row says which.
                        self.evidence.setdefault(f"split-span-row-{index + 1}.json", json.dumps(
                            {"row": row, "side": side, "bytes": raw.hex()}, indent=1).encode())
                        break
                    last = end
        complete = sorted(hunks)[:-1] if diff.get("rowsTruncated") else sorted(hunks)
        for hunk in complete:
            members = hunks[hunk]
            if any(r.get("textTruncated") for r in members):
                continue
            left = "\n".join(r.get("leftText", "") for r in members if r.get("leftLine", 0) > 0)
            right = "\n".join(r.get("rightText", "") for r in members if r.get("rightLine", 0) > 0)
            marked = any(r.get("leftSpans") or r.get("rightSpans") for r in members)
            # Layout only means the same TOKENS, not the same characters once the
            # whitespace is gone: a line break inside an identifier splits it into
            # two, and marking both is right (soak 96055d2c8eec,
            # "cmake_minimum_required" -> "cmake" / "_minimum_required").
            if self._tokens(left) == self._tokens(right):
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
            pid = self.app.pid
            status = Path(f"/proc/{pid}/task/{pid}/status").read_text()
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
        base = Path(f"/proc/{self.app.pid}/task/{self.app.pid}")
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
        # Settled first: the app's own idle answer (App.settle_quiet waits on the
        # `idle` query), not a fixed second -- half of this oracle's two seconds,
        # every time it ran (profiled 2026-11-13). The window below is the measure.
        self.app.settle_quiet(most=1.0)
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
        while not _past(deadline):
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
                _trace_field(line, "reason=") for line in frames if "reason=" in line)
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
        bounds = collections.Counter(_trace_field(line, "bounds=") for line in frames if "bounds=" in line)
        if not bounds:
            return []
        try:
            elements = (self.app.ui() or {}).get("elements", [])
        except Exception:  # noqa: BLE001 - evidence only
            elements = []
        out = []
        # Which event asked for each frame (the frame trace's `event=`: a wake
        # channel's name or SDL's event type), most common first: the requester
        # that keeps an idle app repainting.
        events = collections.Counter(_trace_field(line, " event=") for line in frames if " event=" in line)
        if events:
            out.append("frames by the event that asked for them: " +
                       ", ".join(f"{count} x {name}" for name, count in events.most_common(5)))
        out.append("repainted bounds (x,y,w,h), most common first:")
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

    def _side_evidence(self, path: str) -> dict[str, bytes]:
        """{file name: bytes} of `path` on each side, and a line of sizes and
        mtimes, for the finding's evidence directory."""
        name = path.replace("/", "__")
        out: dict[str, bytes] = {}
        stamps = []
        for side, root in (("host", self.host_root), ("mirror", self.mirror)):
            target = Path(root) / path
            try:
                out[f"diff-{name}.{side}"] = target.read_bytes()[:1 << 20]
                info = target.stat()
                stamps.append(f"{side}: {info.st_size} bytes, mtime_ns {info.st_mtime_ns}")
            except OSError as error:
                stamps.append(f"{side}: {error.strerror}")
        out[f"diff-{name}.txt"] = ("\n".join(stamps) + "\n").encode()
        # What the sync engine decided about the path (MICROIDE_TRACE_MIRROR_SYNC):
        # the sides say THAT they differ, this which push, delete or resolution
        # left them so (soak 37ec61829b08 said only "only in the mirror").
        history = _logged_history(self.app, "microide mirror-sync:", path)
        if history:
            out[f"diff-{name}.sync.txt"] = ("\n".join(history) + "\n").encode()
        return out

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
        # Stretched like every other wait of the driver's: recorded, the app
        # runs on one core at a third of its speed (MQA_SLOW_FACTOR 3, chaos 5),
        # and a reconnect and five pulls that take 30 s then take 90. The plain
        # 90 s made "did not settle" a finding of the recorded and the
        # sanitizer cells for a project that was settling.
        settle_seconds = self.SETTLE_SECONDS * q.SLOW_FACTOR
        deadline = time.monotonic() + settle_seconds
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
        while not self.settled(remote) and not _past(deadline):
            time.sleep(0.5)
            remote = self._remote()
        if remote.get("elsewhere"):
            self.outcomes["elsewhere"] += 1
            return [("remote", f"the remote project is not in front {settle_seconds:.0f} s after "
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
            return [("remote", f"the remote project did not settle within {settle_seconds:.0f} s "
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
        deadline = time.monotonic() + self.AGREE_SECONDS * q.SLOW_FACTOR
        while True:
            host, mirror = remote_host.content_set(self.host_root, self.mirror)
            diff = self.differing(host, mirror)
            if not diff or _past(deadline):
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
        # Both sides' bytes for the first few, with their mtimes: a hash says they
        # differ, not which side moved last -- the side the sync failed to carry.
        for path in diff[:3]:
            self.evidence.update(self._side_evidence(path))
        first = diff[0]
        where = "only on the host" if first not in mirror else "only in the mirror" if first not in host \
            else "with different content"
        return [("remote", f"the settled mirror and the host differ in {len(diff)} file(s); "
                           f"{Path(first).name} is {where}")]
