#!/usr/bin/env python3
"""The soak runner: the QA monkey on this PC, continuously (safety-plan W1.1).

    tools/soak/soak.py run [--jobs N] [--once] [--max-hours H]   the runner (a systemd --user service)
    tools/soak/soak.py status                                    uptime, rate, open findings, current job
    tools/soak/soak.py report                                    writes build/soak/report.md
    tools/soak/soak.py ledger [list|show SIG|claim SIG|fixed SIG COMMIT|reopen SIG|note SIG TEXT]
    tools/soak/soak.py build                                     update the worktree and build once
    tools/soak/soak.py pause [REASON] [--wait S] / resume         take turns with the fixing agent
    tools/soak/soak.py hunt [SIG] [--runs N] [--rr chaos|1|off]   replay a finding under rr until it shows
    tools/soak/soak.py coverage [--functions SRC...]              what of src/ the soak executes
    tools/soak/soak.py health                                    is every cell running, is the ledger honest

A private soak of a branch, beside the live one -- how a change to the runner or
the driver is tried before it merges (never from a shell inside a microide
terminal: `systemd-run --user --wait --pipe`, see tools/soak/AGENT.md):

    MICROIDE_SOAK_REF=my-branch MICROIDE_SOAK_BUILDS=default \
    MICROIDE_SOAK_DIR=build/soak-try MICROIDE_SOAK_WORKTREE=build/wt-soak-try \
        tools/soak/soak.py run --jobs 4 --max-hours 1

It follows `main` in its own worktree (`build/wt-soak`, never the live tree) and
rebuilds when `main` moves: the default build (assertions on), an optimized
release, ASan, UBSan and TSan. A broken build is a finding against that commit;
the runner keeps soaking the last good binaries. Jobs rotate by weight over the
matrix (profile x build x window size x ui.scale), each with its own seed,
output directory and hard timeout, under `nice -n 19` and `ionice -c3`. How
many run at once follows the machine's load (allowed_jobs): up to about 90% of
the cores when nobody else uses them -- an idle night runs flat out -- and down
to none while the machine is busy; paused by `build/soak/PAUSE` and while a
`run-checks.sh` lane runs.

Every finding goes into the ledger (ledger.py), deduplicated by signature. A
new one gets a bundle under `build/soak/findings/<sig>/` with FINDING.md; it is
then replayed for a reproduction rate and, when deterministic, minimized
(minimize.py). A fixed signature is re-checked on every later build and
reopened if it comes back. See tools/soak/AGENT.md for the fixing agent's side.

Most severe findings never reproduce from a replay of their actions (a race, a
kill -9 at the wrong instant). Three things are for those: a share of the jobs
runs the app under `rr record` and a finding keeps the trace, which replays
the same execution every time (RECORD_FRACTION); a fix no replay can check is
verified by the soak it then survives, not by a replay that never showed it
(exposure.py); and an open finding the soak stopped showing is listed as
quiet rather than as work to do.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import json
import math
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools" / "qa"))
import codecov  # noqa: E402
import exposure as ex  # noqa: E402
import ledger as ld  # noqa: E402
import findings as fnd  # noqa: E402
import monkey_model as mm  # noqa: E402
import recording  # noqa: E402

SOAK = Path(os.environ.get("MICROIDE_SOAK_DIR", REPO / "build" / "soak"))
WORKTREE = Path(os.environ.get("MICROIDE_SOAK_WORKTREE", REPO / "build" / "wt-soak"))
# The ref the runner builds and soaks: `main`, or a branch for a private soak of
# it (with a soak directory and a worktree of its own, MICROIDE_SOAK_DIR and
# MICROIDE_SOAK_WORKTREE). A change to the runner or the driver could only be
# tried by merging it: the runner built `main` and nothing else, so the soak's
# own tooling was the one part of the tree that went live untested -- and a
# deep review of it was being tried by hand, from a shell, when that shell's
# environment took the developer's server down with it (2026-11-16).
FOLLOW_REF = os.environ.get("MICROIDE_SOAK_REF", "main")
# The QA driver a build's jobs run, copied out of the worktree at build time
# (snapshot_driver): jobs and checks run from here, so the worktree can move to
# the next commit under them and a build needs no drain.
DRIVERS = SOAK / "driver"
# What the driver reads from the repository (microide_qa.REPO is two levels up
# from tools/qa): the driver itself, the fixture script, the soak's own tools
# (minimize.py), the plugins a profile loads and the sanitizers' suppression
# files its SANITIZER_ENV names. Without the last, every TSan job died at
# startup ("failed to read suppressions file") from the day jobs began to run
# from a snapshot: 23 runs of 0 steps and no finding, for four days
# (test_snapshot.py holds the list to what the driver names).
DRIVER_PATHS = ("tools/qa", "tools/capture-media", "tools/soak", "plugins",
                "tests/lsan.supp", "tests/tsan.supp")
PAUSE = SOAK / "PAUSE"
# A check's minimize: its wall-clock budget, counted from its start.
MINIMIZE_BUDGET_S = 3600
STATE = SOAK / "state.json"
RUNS = SOAK / "runs"
FINDINGS = SOAK / "findings"
CORPUS = SOAK / "corpus"
COVERAGE = SOAK / "coverage.json"
RUN_LOG = SOAK / "runs.jsonl"
DISK_BUDGET_GB = float(os.environ.get("MICROIDE_SOAK_DISK_GB", "15"))


# ---- builds ----------------------------------------------------------------------

@dataclass
class Build:
    name: str
    directory: str           # relative to the worktree
    configure: list[str]     # arguments after `cmake`
    slow: float              # MQA_SLOW_FACTOR
    env: dict[str, str] = field(default_factory=dict)
    prefix: list[str] = field(default_factory=list)  # e.g. setarch -R for TSan
    # A clang source-coverage build: its jobs leave raw profiles, merged into
    # the soak's profile when each is reaped (codecov.py).
    coverage: bool = False

    def binary(self) -> Path:
        return WORKTREE / self.directory / "microide" / "microide"


BUILDS = {
    # With debug info (-g changes no code): a recorded job's trace (RECORD_BUILDS)
    # is debugged with this binary, and without it gdb has names and no lines,
    # locals or types to watch.
    # -gz: the sections compressed, 280 MB where plain -g is 600 (the binary is
    # copied per commit, and every kept trace holds a link to its own).
    "default": Build("default", "build", ["-S", ".", "-B", "build", "-DCMAKE_CXX_FLAGS=-g -gz",
                                          "-DCMAKE_C_FLAGS=-g -gz"], 1.0),
    "release": Build("release", "build-release", ["-S", ".", "-B", "build-release",
                                                  "-DCMAKE_BUILD_TYPE=Release"], 1.0),
    "asan": Build("asan", "build/microide-asan", ["--preset", "microide-asan"], 3.0),
    "ubsan": Build("ubsan", "build/microide-ubsan", ["--preset", "microide-ubsan"], 2.0),
    "tsan": Build("tsan", "build/microide-tsan", ["--preset", "microide-tsan"], 5.0,
                  prefix=["setarch", "-R"]),
    # What the soak executes of src/ at all (codecov.py): the surface inventory
    # says which commands and keys were reached, this says what they ran. -O1,
    # so a job is about as fast as the default build's.
    "coverage": Build("coverage", "build-coverage",
                      ["-S", ".", "-B", "build-coverage", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
                       "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF",
                       "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping -O1",
                       "-DCMAKE_C_FLAGS=-fprofile-instr-generate -fcoverage-mapping -O1",
                       "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate"], 1.5, coverage=True),
}
# A private soak may build fewer: MICROIDE_SOAK_BUILDS=default,asan. Six builds
# of a branch are an hour of the machine before the first job; the default
# build alone is what a change to the runner or the driver needs to be tried.
if os.environ.get("MICROIDE_SOAK_BUILDS"):
    _wanted = [name.strip() for name in os.environ["MICROIDE_SOAK_BUILDS"].split(",") if name.strip()]
    _unknown = [name for name in _wanted if name not in BUILDS]
    if _unknown:
        raise SystemExit(f"MICROIDE_SOAK_BUILDS names no build: {', '.join(_unknown)} (known: {', '.join(BUILDS)})")
    BUILDS = {name: BUILDS[name] for name in _wanted}
# The soak's merged profile, and where a job's raw ones go: `%4m` merges the
# writers of one binary under a lock into at most four files -- the app, every
# relaunch of it, and each `control-list` the driver runs are that binary.
CODE_PROFILE = SOAK / "coverage" / "soak.profdata"
RAW_PROFILE_PATTERN = "cov-%4m.profraw"
CODE_REPORT_SECONDS = 3600.0


def git(*args: str, cwd: Path = REPO) -> str:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, env=env).stdout.strip()


def followed_head() -> str:
    """The commit FOLLOW_REF is at: what the runner builds next."""
    return git("rev-parse", "--verify", "--quiet", f"{FOLLOW_REF}^{{commit}}")


def ensure_worktree() -> None:
    if not (WORKTREE / ".git").exists():
        WORKTREE.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "worktree", "add", "--detach", str(WORKTREE), FOLLOW_REF], cwd=REPO,
                       capture_output=True)


class BackgroundBuild:
    """build_all on a copy of the state, on its own thread, so the scheduling loop
    keeps reaping, timing out and starting jobs through a build -- minutes with a
    warm ccache, an hour for six cold configurations, during which the loop used
    to stand still. `finish` hands back the keys the build changed; the loop
    folds them into its own state, the only copy it saves."""

    def __init__(self, state: dict[str, Any], log) -> None:
        self.before = copy.deepcopy(state)
        self.after = copy.deepcopy(state)
        self.error: str | None = None
        self.thread = threading.Thread(target=self._run, args=(log,), name="soak-build", daemon=True)
        self.thread.start()

    def _run(self, log) -> None:
        try:
            build_all(self.after, log)
        except Exception as error:  # noqa: BLE001 - a broken build never stops the runner
            self.error = str(error)

    def done(self) -> bool:
        return not self.thread.is_alive()

    def finish(self, state: dict[str, Any]) -> str | None:
        self.thread.join()
        for key, value in self.after.items():
            if self.before.get(key) != value:
                state[key] = value
        return self.error


def build_all(state: dict[str, Any], log) -> None:
    """Moves the worktree to main's head and builds every configuration. A build
    that fails is recorded (and a ledger finding); its last good binary stays."""
    ensure_worktree()
    head = followed_head()
    if git("log", "-1", "--format=%ae", head) == QA_GIT_EMAIL:
        raise RuntimeError(f"{FOLLOW_REF} is at {head[:10]}, a QA monkey commit: not building it")
    if is_built(state, head):
        # Built before this runner had snapshots (or lost it): take it now, from
        # the worktree that is still at `head`.
        if not (Path(state.get("driver") or "") / "tools" / "qa" / "run.py").exists():
            state["driver"] = str(snapshot_driver(head))
        return
    log(f"building {head[:10]}")
    subprocess.run(["git", "checkout", "-q", "--detach", head], cwd=WORKTREE, capture_output=True)
    good = state.setdefault("good", {})
    cores = max(2, (os.cpu_count() or 4) // 2)
    tried = state.setdefault("tried", {})
    for build in BUILDS.values():
        if not buildable(build):
            continue  # no clang or llvm tools here: `health` says so
        tried[build.name] = head
        env = {**os.environ, "CCACHE_SLOPPINESS": "pch_defines,time_macros"}
        started = time.monotonic()
        configure = subprocess.run(["nice", "-n", "19", "cmake", *build.configure], cwd=WORKTREE,
                                   capture_output=True, text=True, env=env)
        # The server too: a remote job installs the one built with its app on the
        # run's loopback host (tools/qa/remote_host.py).
        result = configure if configure.returncode else subprocess.run(
            ["nice", "-n", "19", "ionice", "-c3", "cmake", "--build", build.directory, "--target", "microide",
             "microide-server", f"-j{cores}"], cwd=WORKTREE, capture_output=True, text=True, env=env)
        if result.returncode == 0 and build.binary().exists():
            # A copy per commit: jobs keep running the binary they started with.
            target = SOAK / "bin" / build.name / head[:12]
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(build.binary(), target / "microide")
            server = build.binary().parent / "microide-server"
            if server.exists():
                shutil.copy2(server, target / "microide-server")
            good[build.name] = {"sha": head, "binary": str(target / "microide")}
            log(f"  {build.name}: ok ({time.monotonic() - started:.0f}s)")
            close_build_finding(build.name, head, log)
        else:
            tail = (result.stdout + result.stderr)[-4000:]
            log(f"  {build.name}: FAILED")
            signature = build_signature(build.name)
            ledger().append(signature, "new" if signature not in ledger().entries() else "seen",
                            kind="build", label=f"build: the {build.name} build fails", commit=head,
                            cell=build.name, detail=tail)
    state["built_sha"] = head
    state["driver"] = str(snapshot_driver(head))
    prune_binaries(good)
    prune_drivers(state)
    refresh_surface(state, log, inventory=True)


SURFACE_SUMMARY = SOAK / "surface-summary.json"
# Between builds the inventory stands and the coverage grows: recounted this often.
SURFACE_RECOUNT_SECONDS = 6 * 3600.0


def refresh_surface(state: dict[str, Any], log, inventory: bool) -> None:
    """Writes the surface's coverage (tools/soak/surface.py): what the app can do
    against what the soak has reached, as `surface-gaps.md` and
    `surface-summary.json`, which `health` and `report` read.

    With `inventory`, from a launch of the new build (a feature shows up as not
    reached the day it lands); without, from the saved inventory against the
    coverage as it has grown. It was a tool somebody had to remember to run: the
    soak's directory had never held a gap list, and "89% reached" was a number
    from a measure that could not see a pressed key."""
    driver = driver_root(state)
    good = state.get("good", {}).get("default")
    command = [sys.executable, str(driver / "tools" / "soak" / "surface.py"), "--coverage", str(COVERAGE),
               "--out", str(SOAK), "--extra", str(driver / "tools" / "soak" / "surface-extra.toml")]
    if inventory:
        if not good:
            return
        command += ["--binary", good["binary"]]
    elif (SOAK / "surface.json").exists():
        command += ["--inventory", str(SOAK / "surface.json")]
    else:
        return
    try:
        result = subprocess.run(["nice", "-n", "19", *command], cwd=driver, capture_output=True, text=True,
                                timeout=600)
    except (OSError, subprocess.TimeoutExpired) as error:
        log(f"surface: {error}")
        return
    tail = (result.stdout + result.stderr).strip().splitlines()
    log(f"surface: {tail[-1][:200] if tail else 'no output'}")
    state["surface_at"] = time.time()


def surface_line() -> str | None:
    """`health`'s line on the surface, or None before the first count."""
    try:
        summary = json.loads(SURFACE_SUMMARY.read_text())
    except (OSError, ValueError):
        return None
    worst = sorted(((len(c["unreached"]), name) for name, c in summary.get("categories", {}).items()
                    if c["unreached"]), reverse=True)
    return (f"surface: {summary.get('reached')} of {summary.get('total')} inventoried items reached, "
            f"{summary.get('excluded')} excluded with a reason, {summary.get('unreached')} NOT REACHED"
            + (" (" + ", ".join(f"{name} {count}" for count, name in worst[:6]) + ")" if worst else "")
            + (f"; {len(summary['staleExclusions'])} stale exclusion(s)" if summary.get("staleExclusions") else "")
            + f" -- {SOAK / 'surface-gaps.md'}")


def build_signature(name: str) -> str:
    return fnd.signature("build", f"{name} build failed")


def close_build_finding(name: str, head: str, log) -> None:
    """A build that fails is a finding; the same build succeeding is its end.
    Nothing closed one: its rate is one sighting in thousands of the job
    type's runs, so exposure never would, and "the default build fails" led
    the severe list for days after the build was green again."""
    sig = build_signature(name)
    entry = ledger().entries().get(sig)
    if entry is not None and entry.status in ("new", "reopened", "claimed"):
        ledger().append(sig, "closed", by="build", note=f"the {name} build succeeded at {head[:12]}")
        log(f"  closed {sig}: the {name} build is good again")


def snapshot_driver(head: str) -> Path:
    """The worktree's QA driver at `head`, copied where the next checkout cannot
    change it under a running job (about 4 MB)."""
    target = DRIVERS / head[:12]
    if not target.exists():
        staging = DRIVERS / f".{head[:12]}.partial"
        shutil.rmtree(staging, ignore_errors=True)
        for relative in DRIVER_PATHS:
            copy_driver_path(WORKTREE / relative, staging / relative)
        staging.rename(target)
    else:
        # A snapshot an older runner took of this commit, with a shorter list:
        # the worktree is still at `head` (both callers), so fill it in.
        for relative in DRIVER_PATHS:
            if not (target / relative).exists():
                copy_driver_path(WORKTREE / relative, target / relative)
    return target


def copy_driver_path(source: Path, destination: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, destination, symlinks=True, ignore=shutil.ignore_patterns("__pycache__"))
    elif source.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def driver_root(state: dict[str, Any]) -> Path:
    """Where jobs and checks run the driver from: the newest build's snapshot (the
    worktree before the first build that took one)."""
    path = Path(state.get("driver") or WORKTREE)
    return path if (path / "tools" / "qa" / "run.py").exists() else WORKTREE


def mark_in_use(path: Path) -> None:
    """Dates `path` -- a binary's directory or a driver snapshot -- to now: a job
    starts on it. The prunes below keep what a job started on lately (prune_drivers
    for six hours; prune_binaries for minutes, then while a process names it). They
    went by the directory's own date, which is when it was MADE, so a binary that
    served past six hours went the moment the next build replaced it, under the
    jobs still running it: they died at their next relaunch with "No such file"
    and a driver whose sources had gone too (soak 15d0bae54bb7, 6a8bab5043ad,
    89a4437078a2, ab3bacbd4195, eb4768093ac2)."""
    with contextlib.suppress(OSError):
        os.utime(path)


def prune_drivers(state: dict[str, Any]) -> None:
    """Snapshots no job has started on for longer than the longest job (checks:
    three hours), and never the current one: a job started on an older snapshot
    finishes on it (mark_in_use)."""
    keep = Path(state.get("driver") or "")
    for snapshot in DRIVERS.glob("*"):
        if snapshot != keep and time.time() - snapshot.stat().st_mtime > 6 * 3600:
            shutil.rmtree(snapshot, ignore_errors=True)


def buildable(build: Build) -> bool:
    """Whether this machine can build it: the coverage build needs clang and the
    llvm tools (`health` says when they are missing)."""
    return not build.coverage or codecov.unusable_reason() is None


def is_built(state: dict[str, Any], head: str) -> bool:
    """Whether every configuration was built -- or tried: a failed build is a
    finding, not a reason to build again every interval -- at `head`, and the
    binaries are in hand. A configuration added to BUILDS since the commit was
    built has not been tried: a runner restarted onto a new build type (the
    coverage build, the day it landed) builds it now, not at the next push."""
    tried = state.get("tried", {})
    return (state.get("built_sha") == head
            and all(BUILDS[name].binary().exists() for name in state.get("good", {}) if name in BUILDS)
            and all(tried.get(name) == head for name, build in BUILDS.items() if buildable(build)))


def refresh_due(state: dict[str, Any]) -> bool:
    """Whether a build and a verification are due (they run beside the jobs).

    A build used to drain the runner first, and draining unconditionally every
    fifteen minutes left the soak at about three jobs of a budget of nine. Since
    jobs run their own build's driver snapshot there is no drain at all; this
    still skips the work when there is none: main has not moved, no binary is
    missing, and no fixed finding waits for its replay.
    """
    return not is_built(state, followed_head()) or any(True for _ in fixes_to_verify(state))


# A job's binary is marked (mark_in_use) just before its driver spawns: until
# then no process names it.
BINARY_GRACE_SECONDS = 15 * 60


def live_process_names() -> bytes:
    """Every live process's command line and executable, NUL-separated: a job's,
    a check's or a hunt's driver names its build with `--binary`, and the app it
    runs is that build's executable."""
    parts = []
    for proc in Path("/proc").glob("[0-9]*"):
        with contextlib.suppress(OSError):
            parts.append((proc / "cmdline").read_bytes())
        with contextlib.suppress(OSError):
            parts.append(os.readlink(proc / "exe").encode())
    return b"\0".join(parts)


def prune_binaries(good: dict[str, Any]) -> None:
    """A superseded build's binaries once no live process names them -- a check
    runs three hours, so no idle time is safe -- and no job has started on them
    for BINARY_GRACE_SECONDS. Never the current ones."""
    keep = {Path(entry["binary"]).parent for entry in good.values()}
    names: bytes | None = None
    for build_dir in (SOAK / "bin").glob("*/*"):
        if build_dir in keep or time.time() - build_dir.stat().st_mtime <= BINARY_GRACE_SECONDS:
            continue
        if names is None:
            names = live_process_names()
        if bytes(build_dir) + b"/" not in names:
            shutil.rmtree(build_dir, ignore_errors=True)


# ---- jobs ------------------------------------------------------------------------

@dataclass
class JobType:
    name: str
    weight: float
    build: str
    profile: str = "default"
    steps: int = 1500
    timeout: float = 1800
    project: str = ""       # empty: taskflow; "repo": the worktree itself
    matrix: bool = True     # randomize window size and ui.scale
    env: tuple[tuple[str, str], ...] = ()  # the QA driver's environment for this job (a backend)


# A job type whose profile is drawn afresh for each job (job_profile).
ANY_PROFILE = "*"


JOB_TYPES = [
    JobType("default", 26, "default"),
# The QA driver's Wayland backend (a headless weston per run, input through the
# control channel): the X11 cells never meet the Wayland window, its scale or its
# clipboard. Offered only where weston is (wayland_available).
JobType("wayland", 3, "default", env=(("MQA_BACKEND", "wayland"), ("MQA_WAYLAND_SCALE", "2")), matrix=False),
# An openbox on the job's Xvfb (MQA_WM): the app's own maximize and fullscreen
# reach a window manager, which no other cell has. Offered only where openbox is.
JobType("window-manager", 2, "default", "layout", env=(("MQA_WM", "openbox"),)),
    JobType("editing", 12, "default", "editing"),
    JobType("layout", 10, "default", "layout"),
    JobType("world", 8, "default", "world"),
    JobType("diff", 6, "default", "diff"),
    JobType("tooling", 8, "default", "tooling"),
    JobType("terminal", 4, "default", "terminal"),
    JobType("settings", 4, "default", "settings"),
    # A remote project over a loopback host of the run's own, with link cuts and
    # server kills: no other job reaches src/project/remote or the server.
    JobType("remote", 6, "default", "remote", steps=800),
    # Crash safety, a dozen kills a run and half of them at a named durable-write
    # boundary: what the other jobs do three times in 1500 steps.
    JobType("crash", 8, "default", "crash", steps=600),
    # The Lua plugin host: the bundled data-only plugins, their commands and
    # surfaces. No other job runs a plugin's command.
    JobType("plugins", 4, "default", "plugins"),
    JobType("commands", 4, "default", "commands", steps=600, project="repo", matrix=False),
    JobType("editor", 3, "default", "editor", steps=600, project="repo", matrix=False),
    JobType("release", 6, "release"),
    # The coverage build under every profile in turn (ANY_PROFILE): the measure is
    # of the whole soak, not of one profile's walk.
    JobType("coverage", 6, "coverage", ANY_PROFILE, steps=800, timeout=3600),
    JobType("asan", 10, "asan", steps=600, timeout=3600),
    JobType("asan-editing", 4, "asan", "editing", steps=500, timeout=3600),
    JobType("ubsan", 5, "ubsan", steps=800, timeout=3600),
    # The one build that names a data race outright, and the open severe findings
    # are races: it had the lowest weight of all (2) and, dead at startup for
    # four days, a yield of nothing that the steering then starved further.
    JobType("tsan", 8, "tsan", steps=300, timeout=3600, matrix=False),
    # The scaling oracle (tools/qa/scaling.py): each step times one operation at N
    # and 2N. Eight steps are every probe once; the window does not matter.
    JobType("scaling", 2, "default", "scaling", steps=8, timeout=1800, matrix=False),
]

# ---- recorded jobs -----------------------------------------------------------------

# The builds whose jobs may run under `rr record` (tools/qa/recording.py): the
# sanitizer builds are slow enough already and say what they find themselves.
RECORD_BUILDS = frozenset({"default"})
# Profiles that are not recorded: none now. rr stops a recording, app and all,
# at a system call it does not implement, and upstream rr does not implement
# tee(2), which this machine's coreutils (cat, head, seq, yes) call: the
# terminal and world monkeys lost their recorder several times in 500 steps.
# The rr installed here carries tools/soak/rr-tee.patch (TD-2026-11-16-451). On
# a machine whose rr does not, list "terminal" and "world" here; a run that
# loses its recorder anyway is counted (`soak.py health`), never booked.
UNRECORDED_PROFILES: frozenset[str] = frozenset()
# The share of those jobs that is recorded, and of the recorded ones in rr's
# chaos mode (randomized scheduling: races show more often, at more cost). Not
# all of them: rr runs the app's threads one at a time, so a recorded run has
# its own timing, and the soak wants both.
RECORD_FRACTION = 0.3
RECORD_CHAOS_SHARE = 0.5
# MQA_SLOW_FACTOR for a recorded job (the build's own, when larger).
RECORD_SLOW = {"1": 3.0, "chaos": 5.0}
RECORD_CELL_TAG = {"": "", "1": "+rr", "chaos": "+rr-chaos"}
# The largest window a recorded job gets, in pixels. rr cannot record a GPU
# driver, so a recorded app falls back to SDL's software renderer, on the one
# core rr gives it, with the redraw verifier painting every frame twice: at
# 2560x1440 single frames took a minute and the driver's queries timed out.
RECORD_MAX_PIXELS = 1440 * 900


def choose_recording(job_type: "JobType", rng: random.Random) -> str:
    """"" (not recorded), "1" or "chaos" for a job about to start. Nothing is
    recorded where rr cannot run (`soak.py health` says why)."""
    if job_type.build not in RECORD_BUILDS or job_type.profile in UNRECORDED_PROFILES or job_type.env \
            or recording.rr_unusable_reason() is not None:
        return ""
    if rng.random() >= RECORD_FRACTION:
        return ""
    return "chaos" if rng.random() < RECORD_CHAOS_SHARE else "1"

SIZES = [(1440, 900), (1440, 900), (1280, 720), (1920, 1080), (1024, 700), (800, 600), (2560, 1440), (640, 480)]
SCALES = ["1", "1", "1", "1.25", "1.5", "1.75", "2"]
# A cell whose LOGICAL window (pixels / ui.scale) is smaller than this is "tiny":
# 640x480 at 2, 1024x700 at 2. Every panel there sits at its minimum size, so most
# of what a run finds is a control squeezed past its container -- real, but a
# window almost nobody uses, and it crowded the ledger out (two days: most of the
# open stale-paint findings came from those cells). They still run, rarely.
TINY_LOGICAL = (800, 500)
TINY_CELL_WEIGHT = 0.05


def is_tiny(size: tuple[int, int], scale: str) -> bool:
    factor = float(scale)
    return size[0] / factor < TINY_LOGICAL[0] or size[1] / factor < TINY_LOGICAL[1]


def cell_is_tiny(cell: str) -> bool:
    """For a ledger cell "<job>/<build>/<W>x<H>@<scale>"."""
    try:
        dims, scale = cell.rsplit("/", 1)[-1].split("@")
        width, height = (int(v) for v in dims.split("x"))
        return is_tiny((width, height), scale)
    except ValueError:
        return False


def choose_cell(rng: random.Random, max_pixels: int | None = None) -> tuple[tuple[int, int], str]:
    """A window size and ui.scale, drawn from SIZES x SCALES with tiny cells
    weighted down to TINY_CELL_WEIGHT; with `max_pixels`, from the sizes of at
    most that many pixels."""
    cells = [(size, scale) for size in SIZES for scale in SCALES
             if max_pixels is None or size[0] * size[1] <= max_pixels]
    weights = [TINY_CELL_WEIGHT if is_tiny(size, scale) else 1.0 for size, scale in cells]
    return rng.choices(cells, weights=weights)[0]


# ---- what to fix first ---------------------------------------------------------

# Impact tiers, most severe first. A stale frame or a squeezed layout is a defect,
# but a crash, a lost or corrupted buffer, a broken undo or a leak costs a user
# their work or their session; the sanitizers' log lines are memory and thread
# bugs whatever they look like on screen.
TIER_NAMES = ("severe", "behaviour", "layout", "paint")
# `invariant`: a structural rule of the workspace broken (the app's `invariants`
# query) -- the state a lost buffer or a crash is later the symptom of.
_SEVERE_KINDS = frozenset({"crash", "data", "undo", "leak", "build", "containment", "job", "invariant"})
# What a new finding of each tier counts for when the rotation follows yield
# (choose_job). Counted one each, the yield was the stale-paint signatures' --
# 983 of the ledger's 1336, one defect fanning out into dozens -- and the jobs
# that find those took the weight from the ones that find lost data.
TIER_YIELD = (1.0, 0.5, 0.1, 0.05)
_BEHAVIOUR_KINDS = frozenset({"state", "focus", "expect", "idle", "latency", "liveness"})
_LAYOUT_KINDS = frozenset({"overlap", "bounds"})


def impact_tier(entry: ld.Entry) -> int:
    return tier_of(entry.kind, entry.label)


def tier_of(kind: str, label: str) -> int:
    if kind == "log":
        return 3 if "QA-STALE" in label else 0
    if kind in _SEVERE_KINDS:
        return 0
    if kind in _BEHAVIOUR_KINDS:
        return 1
    if kind in _LAYOUT_KINDS:
        return 2
    return 1


# ---- families ------------------------------------------------------------------------

_FAMILY_FILE = re.compile(r"\b(buffer of|buffer|text of|composition changed) (\S.*?) "
                          r"(did not come back|came back|differs|\(<n> ->|before it was committed)")
_FAMILY_STALE = re.compile(r"^log: (\[[^\]]*\] )?QA-STALE .*?(reason=\S+)")


def family(kind: str, label: str) -> str:
    """A label with what splits one defect into many signatures taken out: the
    file a data or undo finding names (the kill -9 restore failure is ten
    signatures, one per fixture file), a leak's budget note, a stale frame's
    clip count, and for a stale frame that names the element it left stale,
    everything but its surface and that element: how the frame caught it (the
    reason) and where its first pixel sat against the clips vary from walk to
    walk for one defect (the acceptance run's planted stale status segment was
    seven signatures). For reading the ledger, not for deduplication."""
    if kind in ("data", "undo", "state"):
        return _FAMILY_FILE.sub(lambda m: f"{m.group(1)} <file> {m.group(3)}", label)
    if kind == "leak":
        return label.split(" over the run", 1)[0]
    stale = _FAMILY_STALE.match(label)
    if stale:
        context = (stale.group(1) or "").strip()[1:-1].split("|")
        if len(context) == 4 and context[3] not in ("", "-"):
            return f"log: [{context[0]}|{context[3]}] QA-STALE"
        return f"log: {stale.group(1) or ''}QA-STALE {stale.group(2)}"
    return label


def tiny_only(entry: ld.Entry) -> bool:
    """Seen only in tiny cells (see TINY_LOGICAL)."""
    return bool(entry.cells) and all(cell_is_tiny(cell) for cell in entry.cells)


def priority_key(entry: ld.Entry) -> tuple:
    """Sort key, best first: a finding the soak still shows before a quiet one
    (is_quiet); then the impact tier; within a tier, a finding a real-size
    window shows before one only a tiny window does, then deterministic before
    unchecked before flaky, then the most sightings."""
    det = 0 if entry.deterministic else (2 if entry.deterministic is False else 1)
    return (is_quiet(entry), impact_tier(entry), tiny_only(entry), det, -entry.count, entry.sig)


CHECK_LOW_TIER_EVERY = 3


def next_check(pending: list[str], known: dict[str, ld.Entry], started: int) -> str:
    """The pending finding to check next. A family nobody has measured goes
    before another signature of one already measured (a check is an hour and a
    half of three displays; the acceptance run spent them on twins). Among those,
    the most severe first, except that every CHECK_LOW_TIER_EVERY-th check goes
    to a layout or paint finding: by severity alone the acceptance run's planted
    stale segment waited all 24 h behind severe findings and was never minimized."""
    entries = {sig: known.get(sig, ld.Entry(sig=sig)) for sig in pending}
    measured = {family(e.kind, e.label) for sig, e in known.items()
                if e.deterministic is not None and sig not in entries}
    fresh = [sig for sig, e in entries.items() if family(e.kind, e.label) not in measured]
    pool = fresh or list(pending)
    if started % CHECK_LOW_TIER_EVERY == CHECK_LOW_TIER_EVERY - 1:
        low = [sig for sig in pool if impact_tier(entries[sig]) >= 2]
        pool = low or pool
    return min(pool, key=lambda sig: priority_key(entries[sig]))


# ---- exposure: what the soak's silence says ------------------------------------------

_EXPOSURE: tuple[float, ex.Exposure] | None = None


def current_exposure() -> ex.Exposure:
    """The soak's runs (runs.jsonl), re-read when the file changes."""
    global _EXPOSURE
    try:
        stamp = RUN_LOG.stat().st_mtime
    except OSError:
        stamp = 0.0
    if _EXPOSURE is None or _EXPOSURE[0] != stamp:
        _EXPOSURE = (stamp, ex.Exposure(run_rows()))
    return _EXPOSURE[1]


def quiet_for(entry: ld.Entry) -> ex.Quiet | None:
    """The soak since the finding's last sighting (exposure.py)."""
    return current_exposure().quiet(entry.sightings) if entry.sightings else None


def is_quiet(entry: ld.Entry) -> bool:
    """An open finding the soak has stopped showing: enough runs of the jobs
    that showed it, since, that its old rate was due QUIET_EXPECTED sightings.
    Probably fixed by something else, or by a fix nobody booked; it stays in the
    ledger, and one sighting makes it active again."""
    if entry.status not in ("new", "reopened"):
        return False
    quiet = quiet_for(entry)
    return quiet is not None and quiet.expected >= ex.QUIET_EXPECTED


def fix_exposure(entry: ld.Entry) -> ex.Quiet | None:
    """The soak on builds that contain the entry's fix."""
    if not entry.fix_commit or not entry.sightings:
        return None
    return current_exposure().quiet(entry.sightings, since=0.0,
                                    on_build=lambda sha: bool(sha) and has_commit(entry.fix_commit, sha))


def priority_tag(entry: ld.Entry) -> str:
    return TIER_NAMES[impact_tier(entry)] + ("/tiny" if tiny_only(entry) else "")


@dataclass
class Job:
    type: JobType
    seed: int
    out: Path
    command: list[str]
    cell: str
    started: float
    proc: subprocess.Popen
    sha: str
    binary: str
    record: str = ""    # "" | "1" | "chaos": run under rr (choose_recording)
    slow: float = 1.0   # the MQA_SLOW_FACTOR it ran with


_ANCESTRY: dict[tuple[str, str], bool] = {}


def has_commit(fix: str, commit: str) -> bool:
    """The build at `commit` has `fix` (git merge-base --is-ancestor). Answers
    for known objects never change, so they are memoized: the ledger asks for
    every sighting of every fixed signature on each fold."""
    key = (fix, commit)
    if key not in _ANCESTRY:
        code = subprocess.run(["git", "merge-base", "--is-ancestor", fix, commit],
                              cwd=REPO, capture_output=True).returncode
        if code not in (0, 1):
            return False  # an object this checkout lacks: ask again later
        _ANCESTRY[key] = code == 0
    return _ANCESTRY[key]


def ledger() -> ld.Ledger:
    return ld.Ledger(SOAK / "ledger.jsonl", contains=has_commit)


WESTON = Path(os.environ.get("MQA_WESTON_ROOT", str(Path.home() / ".cache" / "microide-qa" / "weston" / "root"))) \
    / "usr" / "bin" / "weston"


def job_runnable(job: JobType) -> bool:
    """Whether this machine can run the job type at all: a Wayland job needs a
    weston, on PATH or unpacked by tools/qa/weston.sh (which the soak does not
    run: it fetches packages)."""
    if dict(job.env).get("MQA_BACKEND") == "wayland":
        return WESTON.exists() or shutil.which("weston") is not None
    if dict(job.env).get("MQA_WM"):
        return shutil.which(dict(job.env)["MQA_WM"]) is not None
    return True


def choose_job(rng: random.Random, state: dict[str, Any]) -> JobType | None:
    good = state.get("good", {})
    available = [job for job in JOB_TYPES if job.build in good and job.build in BUILDS and job_runnable(job)]
    if not available:
        return None
    # Yield steering: a job type's weight grows with the distinct findings it
    # produced per CPU hour (W1.6), so effort follows what finds bugs.
    stats = state.get("stats", {})
    return rng.choices(available, weights=[job_weight(job, stats.get(job.name, {})) for job in available])[0]


def job_weight(job: JobType, stats: dict[str, Any]) -> float:
    """A job type's weight in the rotation: its own, raised up to fivefold by
    the new findings it brought per hour, each counted by its tier (TIER_YIELD)
    over the hours since that count began.

    >>> job_weight(JobType("a", 10, "default"), {})
    10.0
    >>> job_weight(JobType("a", 10, "default"), {"weighted_hours": 10.0, "new_weighted": 5.0})
    30.0
    >>> job_weight(JobType("a", 10, "default"), {"cpu_hours": 99.0, "new_findings": 500})  # the old count: unused
    10.0
    """
    hours = max(0.5, stats.get("weighted_hours", 0.0))
    yield_rate = stats.get("new_weighted", 0.0) / hours
    plateau = PLATEAU_SHARE if stats.get("quiet_runs", 0) >= PLATEAU_RUNS else 1.0
    return job.weight * (1 + min(4.0, 4 * yield_rate)) * plateau


PLATEAU_RUNS = 20
PLATEAU_SHARE = 0.25


def note_novelty(stats: dict[str, Any], summaries: list[dict[str, Any]], new_findings: int) -> None:
    """Counts a job type's runs in a row that brought nothing new -- no state, no
    transition, no finding (W1.6's plateau); a run that took no step counts for
    nothing. From PLATEAU_RUNS on, job_weight gives the type PLATEAU_SHARE of its
    weight, and the rotation's effort goes to the types still finding new ground."""
    if sum(s.get("steps", 0) for s in summaries) == 0:
        return
    novel = new_findings > 0 or any(s.get("new_states", 0) or s.get("new_transitions", 0) for s in summaries)
    stats["quiet_runs"] = 0 if novel else stats.get("quiet_runs", 0) + 1


def job_profile(job_type: JobType, rng: random.Random) -> str:
    """The monkey profile a job of `job_type` runs: its own, or for ANY_PROFILE
    one of the window-driving profiles the default-build jobs run, by their
    weights -- a remote project needs the host's server built the same way,
    which the coverage build is.

    >>> job_profile(JobType("a", 1, "default", "editing"), random.Random(1))
    'editing'
    >>> job_profile(JobType("c", 1, "coverage", ANY_PROFILE), random.Random(1)) in {j.profile for j in JOB_TYPES}
    True
    """
    if job_type.profile != ANY_PROFILE:
        return job_type.profile
    pool = [job for job in JOB_TYPES if job.build == "default" and job.profile not in ("commands", "editor")
            and job.profile != ANY_PROFILE]
    return rng.choices(pool, weights=[job.weight for job in pool])[0].profile


def start_job(job_type: JobType, state: dict[str, Any], rng: random.Random) -> Job:
    seed = rng.randrange(1, 2**31)
    build = BUILDS[job_type.build]
    profile = job_profile(job_type, rng)
    entry = state["good"][job_type.build]
    out = RUNS / f"{time.strftime('%Y%m%d-%H%M%S')}-{job_type.name}-{seed}"
    out.mkdir(parents=True, exist_ok=True)
    record = choose_recording(job_type, rng)
    size, scale = (choose_cell(rng, RECORD_MAX_PIXELS if record else None) if job_type.matrix
                   else ((1440, 900), "1"))
    driver = driver_root(state)
    qa = driver / "tools" / "qa" / "run.py"
    slow = max(build.slow, RECORD_SLOW.get(record, 1.0))
    command = ["nice", "-n", "19", "ionice", "-c3", *build.prefix, sys.executable, str(qa),
               "--binary", entry["binary"], "--out", str(out), "monkey", "--profile", profile,
               "--seed", str(seed), "--steps", str(job_type.steps), "--time-budget", str(job_type.timeout * 0.8)]
    if profile in ("commands", "editor"):
        command += ["--rounds", str(job_type.steps), "--project", str(WORKTREE)]
    else:
        command += ["--size", f"{size[0]}x{size[1]}", "--coverage", str(COVERAGE), "--corpus", str(CORPUS)]
        if scale != "1":
            command += ["--set", f"ui.scale={scale}"]
        if not is_tiny(size, scale):
            # A real-size cell stays one: without this a run's own resizes and
            # `ui-scale 999999` took most cells tiny within minutes.
            command += ["--min-logical", f"{TINY_LOGICAL[0]}x{TINY_LOGICAL[1]}"]
    env = {**os.environ, "MQA_SLOW_FACTOR": str(slow), **build.env, **dict(job_type.env),
           # driver.log as it stands when the job dies: block-buffered, a job the
           # hard timeout killed left it empty -- even its first line -- and
           # its finding said nothing (soak dd0052b5e422, fe426d7e7bf9, a4d46bd5fdb4).
           "PYTHONUNBUFFERED": "1",
           # The driver's Python stack on the SIGABRT the timeout sends it before
           # the kill (reap_timed_out): where it was stuck.
           "PYTHONFAULTHANDLER": "1"}
    env.pop("MQA_RR", None)
    if record:
        env["MQA_RR"] = record
    if build.coverage:
        (out / "profraw").mkdir(parents=True, exist_ok=True)
        env["LLVM_PROFILE_FILE"] = str(out / "profraw" / RAW_PROFILE_PATTERN)
    # Used from now on: the prunes keep what a job started on in the last six
    # hours (mark_in_use).
    mark_in_use(Path(entry["binary"]).parent)
    mark_in_use(driver)
    cell = f"{job_type.name}/{job_type.build}{RECORD_CELL_TAG[record]}/{size[0]}x{size[1]}@{scale}"
    with open(out / "driver.log", "wb") as log:
        proc = subprocess.Popen(command, cwd=driver, env=env, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
    return Job(job_type, seed, out, command, cell, time.time(), proc, entry["sha"], entry["binary"],
               record=record, slow=slow)


CHECK_JOB = JobType("check", 0, "default", timeout=3 * 3600, matrix=False)
# A fixed finding's replay on the build that has its fix (verify_one): three runs.
VERIFY_JOB = JobType("verify", 0, "default", timeout=3600 + 600, matrix=False)


def start_check(sig: str, state: dict[str, Any]) -> Job:
    out = RUNS / f"check-{sig}"
    out.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(HERE / "soak.py"), "check", sig]
    with open(out / "driver.log", "wb") as log:
        proc = subprocess.Popen(command, cwd=REPO, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    return Job(CHECK_JOB, 0, out, command, f"check/{sig}", time.time(), proc, state.get("built_sha", ""), "")


# The least time between two looks for a finding to hunt (each reads the ledger).
HUNT_INTERVAL = 120.0

# A check or a verification replays its finding three runs at a time
# (minimize --runs 3 --jobs 3): three apps, three displays. Counted as one job,
# five checks were fifteen apps in a budget of five, and the machine ran at a
# load of 45 on 16 cores (2026-11-14) -- every job slowed, and the latency
# oracles filed the slowness as findings.
REPLAY_JOB_SLOTS = 3


def slots(jobs: list["Job"]) -> int:
    """The budget the running jobs take: a check or verification counts as the
    runs it replays at once.

    >>> class J:
    ...     def __init__(self, t): self.type = t
    >>> slots([J(CHECK_JOB), J(VERIFY_JOB), J(JOB_TYPES[0])])
    7
    """
    return sum(REPLAY_JOB_SLOTS if job.type is CHECK_JOB or job.type is VERIFY_JOB else 1 for job in jobs)


# The most of the budget checks and verifications may hold between them. They
# came first without a bound ("a new finding's rate is worth more than another
# fresh run"), and a check is three slots for up to hours when it minimizes: two
# of them held all six slots of a private soak and no job started for the rest
# of it; five hold fifteen of the live soak's sixteen. With hundreds of findings
# queued -- every change to how findings are keyed queues a wave -- the soak
# stopped soaking to measure what it had already found.
REPLAY_SHARE = 0.5


def replay_room(jobs: list["Job"], cap: int) -> bool:
    """Whether one more check or verification may start: it fits the budget, and
    the replays stay within REPLAY_SHARE of it (one always may).

    >>> class J:
    ...     def __init__(self, t): self.type = t
    >>> replay_room([], 16), replay_room([J(CHECK_JOB)], 16), replay_room([J(CHECK_JOB), J(VERIFY_JOB)], 16)
    (True, True, False)
    >>> replay_room([J(JOB_TYPES[0])] * 14, 16), replay_room([], 6), replay_room([J(CHECK_JOB)], 6), replay_room([], 2)
    (False, True, False, False)
    """
    held = sum(REPLAY_JOB_SLOTS for job in jobs if job.type is CHECK_JOB or job.type is VERIFY_JOB)
    return slots(jobs) + REPLAY_JOB_SLOTS <= cap and \
        held + REPLAY_JOB_SLOTS <= max(REPLAY_JOB_SLOTS, int(cap * REPLAY_SHARE))


# ---- hunts: a recording of a finding no replay of its actions shows ----------------

# A hunt replays one finding's actions at the run's recorded pace under rr's
# chaos mode, again and again, until it shows -- and then holds a trace of it,
# which reproduces every time. It is what AGENT.md told the fixing agent to do
# by hand for every severe finding without a recording; the runner does it with
# the machine's spare hours instead.
HUNT_JOB = JobType("hunt", 0, "default", timeout=2 * 3600, matrix=False)
# Replays a hunt makes before it gives the slot back, and hunts a finding gets
# before the runner stops trying (a note in the ledger counts them).
HUNT_RUNS = 6
HUNT_LIMIT = 4
HUNT_NOTE = "hunt: "


def hunts_made(entry: ld.Entry) -> int:
    return sum(1 for note in entry.notes if note.startswith(HUNT_NOTE))


def hunt_candidates(entries) -> list[ld.Entry]:
    """The open severe and behaviour findings worth a hunt, best first: the soak
    still shows them, no replay has, their replays did not leave the run's path
    (a diverged replay cannot show the finding however often it runs: that is
    tools/qa's to fix first), they have no recording, and HUNT_LIMIT hunts have
    not already failed."""
    out = []
    for entry in entries:
        if entry.status not in ("new", "reopened") or impact_tier(entry) > 1 or is_quiet(entry):
            continue
        if entry.rate != 0 or entry.replay_verdict in ("diverged", "untested"):
            continue
        if entry.kind in fnd.TIMING_KINDS or entry.kind in ("build", "job", "containment"):
            continue  # a timing finding under rr is the recorder's timing
        bundle = FINDINGS / entry.sig
        if has_recording(bundle) or hunts_made(entry) >= HUNT_LIMIT:
            continue
        if not ((bundle / "replay.json").exists() or any(bundle.glob("sightings/*/replay.json"))):
            continue
        out.append(entry)
    return sorted(out, key=lambda e: (hunts_made(e), *priority_key(e)))


def hunt_replay(bundle: Path) -> Path | None:
    """The record a hunt replays: the newest kept sighting's (the current
    build's behaviour, and for a reopened finding the only replay with
    evidence), else the bundle's own."""
    sightings = sorted(bundle.glob("sightings/*/replay.json"), reverse=True)
    if sightings:
        return sightings[0]
    return bundle / "replay.json" if (bundle / "replay.json").exists() else None


def start_hunt(sig: str, state: dict[str, Any]) -> Job:
    out = RUNS / f"hunt-{sig}"
    out.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(HERE / "soak.py"), "hunt", sig]
    with open(out / "driver.log", "wb") as log:
        proc = subprocess.Popen(command, cwd=REPO, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    return Job(HUNT_JOB, 0, out, command, f"hunt/{sig}", time.time(), proc, state.get("built_sha", ""), "")


def hunt_finding(sig: str, state: dict[str, Any], log, runs: int = HUNT_RUNS, mode: str = "chaos") -> bool:
    """Replays `sig` up to `runs` times, paced and recorded, on the current
    default build; keeps the first replay that shows it as a sighting of the
    bundle, trace and all. True when one did."""
    bundle = FINDINGS / sig
    replay = hunt_replay(bundle)
    good = state.get("good", {}).get("default")
    if replay is None or not good:
        log(f"{sig}: nothing to hunt with (no replay, or no default build)")
        return False
    driver = driver_root(state)
    slow = RECORD_SLOW.get(mode, 1.0)
    scratch = RUNS / f"hunt-{sig}"
    command: list[str] = []
    for attempt in range(1, runs + 1):
        out = scratch / f"attempt-{attempt}"
        shutil.rmtree(out, ignore_errors=True)
        command = ["nice", "-n", "19", "ionice", "-c3", sys.executable, str(driver / "tools" / "qa" / "run.py"),
                   "--binary", good["binary"], "--out", str(out), "replay", str(replay), "--paced",
                   *(["--rr", mode] if mode in recording.RR_MODES else [])]
        env = {**os.environ, "MQA_SLOW_FACTOR": str(slow)}
        env.pop("MQA_RR", None)
        with contextlib.suppress(subprocess.TimeoutExpired):
            subprocess.run(command, cwd=driver, env=env, capture_output=True, text=True, timeout=3000)
        caught = None
        with contextlib.suppress(OSError, ValueError):
            report = json.loads((out / "replay" / "findings.json").read_text())
            caught = next((f for f in report.get("findings", []) if f.get("signature") == sig), None)
        if caught is None:
            shutil.rmtree(out, ignore_errors=True)
            continue
        target = bundle / "sightings" / f"{time.strftime('%Y%m%d-%H%M%S')}-{good['sha'][:12]}-hunt"
        evidence = Path(caught.get("evidence") or "")
        if evidence.is_dir() and out.resolve() in evidence.resolve().parents:
            shutil.copytree(evidence, target, dirs_exist_ok=True)
            # A hunt's whole point is the trace. The runner hunts only severe and
            # behaviour findings, so a stale paint is hunted because someone asked
            # (`soak.py hunt SIG`); recording_wanted's tier rule dropped its trace and
            # the one replay that showed it kept nothing to rewind (e93ce81f04ac).
            keep_run_evidence(evidence, target, caught, recording=True)
        else:
            target.mkdir(parents=True, exist_ok=True)
        (target / "sighting.json").write_text(json.dumps(
            {"cell": f"hunt/default{RECORD_CELL_TAG.get(mode, '')}", "sha": good["sha"], "binary": good["binary"],
             "build": "default", "record": mode, "slow": slow, "command": command,
             "detail": caught.get("detail", ""), "step": caught.get("step"), "state": caught.get("state"),
             "status_before": "hunted"}, indent=1))
        ledger().append(sig, "note", text=f"{HUNT_NOTE}caught on replay {attempt} of {runs} "
                                          f"({'rr ' + mode if mode in recording.RR_MODES else 'unrecorded'}, "
                                          f"paced) at {good['sha'][:12]}: {target.name}")
        if (bundle / "meta.json").exists():
            write_finding_md(bundle)
        log(f"hunt {sig}: CAUGHT on replay {attempt} of {runs}; kept {target}")
        shutil.rmtree(out, ignore_errors=True)
        return True
    ledger().append(sig, "note", text=f"{HUNT_NOTE}{runs} paced replays under "
                                      f"{'rr ' + mode if mode in recording.RR_MODES else 'no recorder'} at "
                                      f"{good['sha'][:12]} did not show it")
    log(f"hunt {sig}: {runs} replays did not show it")
    return False


def start_verify(sig: str, state: dict[str, Any]) -> Job:
    out = RUNS / f"verify-{sig}"
    out.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(HERE / "soak.py"), "verify", sig]
    with open(out / "driver.log", "wb") as log:
        proc = subprocess.Popen(command, cwd=REPO, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    return Job(VERIFY_JOB, 0, out, command, f"verify/{sig}", time.time(), proc, state.get("built_sha", ""), "")


def stop_timed_out(job: Job) -> None:
    """Ends a job past its hard timeout: SIGABRT to the driver alone first, so
    faulthandler (PYTHONFAULTHANDLER, start_job) writes where it was stuck into
    driver.log, then the whole group. The kill alone left nothing to read."""
    with contextlib.suppress(OSError):
        os.kill(job.proc.pid, signal.SIGABRT)
    with contextlib.suppress(subprocess.TimeoutExpired):
        job.proc.wait(timeout=5)
    with contextlib.suppress(OSError):
        os.killpg(job.proc.pid, signal.SIGKILL)
    job.proc.wait()


def exit_report(job: Job, timed_out: bool) -> str:
    """How a job ended, for its dead-job finding: the status or signal, how long
    it ran against its budget, and how loaded the machine was then."""
    code = job.proc.returncode
    if code is None:
        how = "still running"
    elif code < 0:
        try:
            name = signal.Signals(-code).name
        except ValueError:
            name = "unknown"
        how = f"killed by signal {-code} ({name})"
    else:
        how = f"exit status {code}"
    load = " ".join(f"{value:.1f}" for value in os.getloadavg())
    available = memory_available()[0] / 2**30
    return (f"ended: {how}{' after the hard timeout (SIGABRT, then the group killed)' if timed_out else ''}\n"
            f"ran: {time.time() - job.started:.0f}s of a {job.type.timeout:.0f}s budget\n"
            f"load (1/5/15 min): {load} on {os.cpu_count()} CPUs; {available:.1f} GB available\n"
            f"command: {' '.join(job.command)}\n")


def record_dead_job(job: Job, timed_out: bool, log) -> str:
    """A job that ran nothing found nothing, and "no findings" is what a healthy
    cell reports too: the TSan cell died at startup for four days (23 runs, 0
    steps) and nothing said so. It is a finding of its own -- the cell's
    coverage is gone until someone looks. Returns its signature."""
    what = ("hit its hard timeout before it wrote a result" if timed_out
            else "ended without a step taken or a result written")
    recorded = " under rr" if job.record else ""
    label = f"job: a {job.type.name} job on the {job.type.build} build{recorded} {what}"
    sig = fnd.signature("job", label)
    ended = exit_report(job, timed_out)
    with contextlib.suppress(OSError):
        (job.out / "exit.txt").write_text(ended)
    tail = f"--- exit.txt\n{ended}"
    logs = [job.out / "exit.txt", job.out / "driver.log", *sorted(job.out.glob("seed-*/startup/*.log"))]
    for path in logs:
        with contextlib.suppress(OSError):
            tail += f"--- {path.name}\n{path.read_text(errors='replace')[-3000:]}\n"
    book = ledger()
    known = sig in book.entries()
    bundle = FINDINGS / sig
    if not known:
        (bundle / "evidence").mkdir(parents=True, exist_ok=True)
        for path in logs:
            with contextlib.suppress(OSError):
                shutil.copy2(path, bundle / "evidence" / path.name)
        finding = {"signature": sig, "kind": "job", "label": label, "detail": tail[-3000:], "step": 0,
                   "actions": 0, "state": ""}
        (bundle / "meta.json").write_text(json.dumps(job_meta(job, finding, {}), indent=1))
    book.append(sig, "seen" if known else "new", kind="job", label=label, commit=job.sha, cell=job.cell,
                detail=tail[-3000:], **({} if known else {"bundle": str(bundle)}))
    if not known:
        write_finding_md(bundle)
    log(f"  {'DEAD' if not known else 'dead'} {sig} {label}")
    return sig


def merge_code_profile(job: Job, state: dict[str, Any], log) -> None:
    """Adds a coverage job's raw profiles to the soak's (CODE_PROFILE), before
    the job's directory goes."""
    raws = sorted((job.out / "profraw").glob("*.profraw"))
    failure = codecov.merge(CODE_PROFILE, raws)
    if failure is None:
        state["code_runs"] = state.get("code_runs", 0) + 1
    else:
        # A coverage job that leaves no profile measured nothing: said, since
        # the report would otherwise go stale without a word.
        state["code_failures"] = state.get("code_failures", 0) + 1
        log(f"  coverage: no profile from {job.cell} seed {job.seed}: {failure}")


def previous_report_sha(sha: str) -> str:
    """The commit of the newest coverage report at another commit than `sha`:
    the base of the report's changed-code section (codecov.changed_since)."""
    try:
        lines = (SOAK / "code-coverage.jsonl").read_text().splitlines()
    except OSError:
        return ""
    for line in reversed(lines):
        with contextlib.suppress(ValueError, AttributeError):
            earlier = str(json.loads(line).get("sha") or "")
            if earlier and earlier != sha:
                return earlier
    return ""


def refresh_code_report(state: dict[str, Any], log) -> None:
    """Writes code-coverage.md / .json from the merged profile against the newest
    coverage build (codecov.py), when runs have been merged since the last one."""
    good = state.get("good", {}).get("coverage")
    runs = state.get("code_runs", 0)
    if not good or not CODE_PROFILE.is_file() or runs == state.get("code_reported_runs"):
        return
    binary = Path(good["binary"])
    server = binary.parent / "microide-server"
    since = previous_report_sha(good["sha"])
    command = ["nice", "-n", "19", sys.executable, str(HERE / "codecov.py"), "report", "--binary", str(binary),
               *(["--object", str(server)] if server.exists() else []), "--profdata", str(CODE_PROFILE),
               "--out", str(SOAK), "--sha", good["sha"], "--runs", str(runs), "--root", str(WORKTREE / "src"),
               *(["--since", since] if since else [])]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=2400)
    except (OSError, subprocess.TimeoutExpired) as error:
        log(f"code coverage: {error}")
        return
    tail = (result.stdout + result.stderr).strip().splitlines()
    log(f"code coverage: {tail[-1][:200] if tail else 'no output'}")
    if result.returncode == 0:
        state["code_reported_runs"] = runs
    state["code_report_at"] = time.time()


def code_line(state: dict[str, Any]) -> str:
    """`health`'s line on the source coverage."""
    reason = codecov.unusable_reason()
    if reason is not None:
        return f"code coverage: OFF -- {reason}"
    try:
        record = json.loads((SOAK / "code-coverage.json").read_text())
    except (OSError, ValueError):
        return ("code coverage: NOT MEASURED yet (the coverage build's jobs, then codecov.py; "
                f"{state.get('code_runs', 0)} run(s) merged so far)")
    totals = record.get("totals", {})
    dark = sum(1 for counts in record.get("files", {}).values()
               if counts["functions"][1] and not counts["functions"][0])
    failures = state.get("code_failures", 0)
    return (f"code coverage: {codecov.percent(totals.get('lines', [0, 0])).strip()} of lines, "
            f"{codecov.percent(totals.get('functions', [0, 0])).strip()} of functions, "
            f"{codecov.percent(totals.get('branches', [0, 0])).strip()} of branches executed "
            f"({record.get('runs')} runs, at {str(record.get('sha'))[:12]}); {dark} files never entered"
            + (f"; {failures} job(s) left no profile" if failures else "")
            + f" -- {SOAK / 'code-coverage.md'}")


def oracle_counts(summaries: list[dict[str, Any]]) -> dict[str, Any]:
    """What a run's oracles that can run vacuously did (the monkey's summary):
    invariant checks the app answered, kills at named crash points by how they
    ended, slow answers put down to a starved machine. For `soak.py health`:
    an oracle that never ran found nothing, and that reads like "no defect".

    >>> oracle_counts([{"invariant_checks": 300, "starved": 2, "kill_points": {"hit:durable.renaming": 1, "missed": 1}},
    ...                {"invariant_checks": 10, "kill_points": {"hit:durable.renaming": 2}}])
    {'invariants': 310, 'starved': 2, 'kills_hit': 3, 'kills_missed': 1}
    >>> oracle_counts([{"steps": 4}])
    {}
    """
    out = {"invariants": sum(int(s.get("invariant_checks") or 0) for s in summaries),
           "starved": sum(int(s.get("starved") or 0) for s in summaries),
           "kills_hit": sum(n for s in summaries for key, n in (s.get("kill_points") or {}).items()
                            if key.startswith("hit:")),
           "kills_missed": sum(int((s.get("kill_points") or {}).get("missed") or 0) for s in summaries)}
    return {key: value for key, value in out.items() if value}


def job_meta(job: Job, finding: dict[str, Any], header: dict[str, Any]) -> dict[str, Any]:
    """A bundle's meta.json: the finding and everything about the run it came from."""
    return {"finding": finding, "cell": job.cell, "sha": job.sha, "binary": job.binary,
            "build": job.type.build, "job": job.type.name, "seed": job.seed, "slow": job.slow,
            "record": job.record, "command": job.command, "header": header}


def reap_job(job: Job, state: dict[str, Any], log, timed_out: bool = False) -> None:
    if job.type is CHECK_JOB or job.type is VERIFY_JOB or job.type is HUNT_JOB:
        tail = (job.out / "driver.log").read_text(errors="replace")[-400:] if (job.out / "driver.log").exists() else ""
        log(f"  {job.cell} done: {tail.strip().splitlines()[-1] if tail.strip() else ''}")
        shutil.rmtree(job.out, ignore_errors=True)
        return
    elapsed = time.time() - job.started
    if BUILDS[job.type.build].coverage:
        with contextlib.suppress(Exception):  # a profile that cannot be merged never stops the runner
            merge_code_profile(job, state, log)
    stats = state.setdefault("stats", {}).setdefault(job.type.name, {})
    stats["runs"] = stats.get("runs", 0) + 1
    stats["cpu_hours"] = stats.get("cpu_hours", 0.0) + elapsed / 3600
    stats["weighted_hours"] = stats.get("weighted_hours", 0.0) + elapsed / 3600
    new = 0
    summaries = []
    for path in job.out.glob("seed-*/findings.json"):
        with contextlib.suppress(OSError, ValueError):
            summaries.append(json.loads(path.read_text()))
    if sum(s.get("steps", 0) for s in summaries) == 0:
        record_dead_job(job, timed_out, log)
    book = ledger()
    known = book.entries()
    recorder_lost = ""
    for summary in summaries:
        for finding in summary.get("findings", []):
            if finding["kind"] == "recorder":
                # rr's limit, not a defect of the app: counted, never booked.
                recorder_lost = finding.get("detail", "")[-200:]
                continue
            sig = finding["signature"]
            fields = dict(kind=finding["kind"], label=finding["label"], commit=job.sha, cell=job.cell,
                          count=finding.get("count", 1))
            if sig not in known:
                bundle = make_bundle(job, summary, finding)
                book.append(sig, "new", bundle=str(bundle), **fields)
                state.setdefault("pending_checks", []).append(sig)
                known[sig] = ld.Entry(sig=sig)
                new += 1
                stats["new_weighted"] = stats.get("new_weighted", 0.0) + \
                    TIER_YIELD[tier_of(finding["kind"], finding["label"])]
                log(f"  NEW {sig} {finding['label']}")
            else:
                keep_sighting(job, finding, known[sig])
                book.append(sig, "seen", **fields)
    stats["new_findings"] = stats.get("new_findings", 0) + new
    was_flat = stats.get("quiet_runs", 0) >= PLATEAU_RUNS
    note_novelty(stats, summaries, new)
    if (stats.get("quiet_runs", 0) >= PLATEAU_RUNS) != was_flat:
        log(f"  {job.type.name}: " + (f"plateau, {PLATEAU_RUNS} runs with nothing new; its weight is cut"
                                       if not was_flat else "found new ground; its weight is back"))
    with open(RUN_LOG, "a") as out:
        out.write(json.dumps({"t": time.time(), "job": job.type.name, "cell": job.cell, "seed": job.seed,
                              "sha": job.sha, "seconds": round(elapsed), "exit": job.proc.returncode,
                              "steps": sum(s.get("steps", 0) for s in summaries),
                              "new_states": sum(s.get("new_states", 0) for s in summaries),
                              "findings": sum(len(s.get("findings", [])) for s in summaries),
                              "new": new, **({"recorder_lost": recorder_lost} if recorder_lost else {}),
                              **oracle_counts(summaries)}) + "\n")
    # The run's directory is evidence only while a finding points into it.
    shutil.rmtree(job.out, ignore_errors=True)


# ---- bundles ---------------------------------------------------------------------

def make_bundle(job: Job, summary: dict[str, Any], finding: dict[str, Any]) -> Path:
    """build/soak/findings/<sig>/: the evidence, the replay up to the finding and
    FINDING.md. Everything an agent needs, nothing it must re-derive."""
    bundle = FINDINGS / finding["signature"]
    bundle.mkdir(parents=True, exist_ok=True)
    # Only a finding's own evidence directory, inside this job's output. An empty
    # path is Path("") == "." -- the runner's working directory, the whole
    # checkout with its build trees -- and copying it once filled the disk with a
    # 254 GB findings tree (2026-11-08).
    evidence = finding_evidence(job, finding)
    if evidence is not None:
        shutil.copytree(evidence, bundle / "evidence", dirs_exist_ok=True)
        keep_run_evidence(evidence, bundle / "evidence", finding)
    replay_source = bundle / "evidence" / "replay.json"
    if replay_source.exists():
        record = json.loads(replay_source.read_text())
        record["binary_sha"] = job.sha
        (bundle / "replay.json").write_text(json.dumps(record, indent=1))
    (bundle / "meta.json").write_text(json.dumps(job_meta(job, finding, summary.get("header", {})), indent=1))
    write_finding_md(bundle)
    return bundle


# What the driver keeps per RUN, beside its findings' own directories: every
# launch's whole log (run.py cmd_monkey, App.save_run_logs) and, for a recorded
# job, the rr traces (App.recording_dir).
RUN_LOGS = "run-logs"
RECORDING = "rr"
# A run's traces above this are not kept: a few hundred MB is the usual size.
RECORDING_MAX_BYTES = 3 * 2**30


def recording_wanted(finding: dict[str, Any]) -> bool:
    """A trace is kept for what a user loses work or time to. Stale paint and
    squeezed layouts replay from their actions, and there are hundreds."""
    return tier_of(finding.get("kind", ""), finding.get("label", "")) <= 1


def link_tree(source: Path, destination: Path) -> None:
    """Copies a tree as hard links (the run's directory is deleted right after;
    several findings of one run then share one copy of its logs and traces)."""
    shutil.copytree(source, destination, copy_function=os.link, dirs_exist_ok=True)


def keep_run_evidence(evidence: Path, target: Path, finding: dict[str, Any], *,
                      recording: bool | None = None) -> None:
    """Adds the run-level evidence of the run `evidence` (a finding's own
    directory) came from to `target`: the whole logs always, the rr traces for
    a finding worth one -- `recording` overrides that choice (a hunt keeps the
    trace it was asked for)."""
    run = evidence.parent
    wanted = recording_wanted(finding) if recording is None else recording
    with contextlib.suppress(OSError):
        if (run / RUN_LOGS).is_dir():
            link_tree(run / RUN_LOGS, target / RUN_LOGS)
        if (run / RECORDING).is_dir() and wanted \
                and disk_usage(run / RECORDING) <= RECORDING_MAX_BYTES:
            link_tree(run / RECORDING, target / RECORDING)


def has_recording(bundle: Path) -> bool:
    return (bundle / "evidence" / RECORDING).is_dir() or any(bundle.glob(f"sightings/*/{RECORDING}"))


def finding_evidence(job: Job, finding: dict[str, Any]) -> Path | None:
    """The finding's own evidence directory inside this job's output, or None.
    Only that: an empty path is Path("") == "." -- the runner's working
    directory, the whole checkout with its build trees -- and copying it once
    filled the disk with a 254 GB findings tree (2026-11-08)."""
    evidence_text = finding.get("evidence") or ""
    if not evidence_text:
        return None
    evidence = Path(evidence_text)
    if not evidence.is_absolute():
        # Relative to the job's working directory: the driver snapshot it ran from.
        root = next((Path(arg).parents[2] for arg in job.command if arg.endswith("tools/qa/run.py")), WORKTREE)
        evidence = (root / evidence).resolve()
    if evidence.is_dir() and evidence.name.startswith("finding-") and \
            job.out.resolve() in evidence.resolve().parents:
        return evidence
    return None


MAX_SIGHTINGS = 3
# Evidence an oracle writes beside some findings and not others, newer than most
# bundles: the data oracle's bytes (with the buffer-io history), the kill -9
# oracle's documents and on-disk state, the idle oracle's wake sampling. A
# sighting carrying one the bundle has nowhere is kept even before any fix --
# without it, a flaky finding that never replays never gets that evidence.
DIAGNOSTIC_EVIDENCE = ("mismatch-*.txt", "kill-state.tar.gz", "kill-documents.json", "idle-wakes.txt",
                       "undecodable-*", "children.txt", "undo-before.txt")


def new_build_for(bundle: Path, sha: str) -> bool:
    """Neither the bundle nor any kept sighting comes from the build at `sha`.
    An open finding whose evidence all predates a fix to its class otherwise
    keeps nothing of what the current code does (ed121110a40f: seen on 5f577c2a
    after 3793bc00b, and that run was deleted)."""
    with contextlib.suppress(OSError, ValueError):
        if json.loads((bundle / "meta.json").read_text()).get("sha") == sha:
            return False
    return not any(bundle.glob(f"sightings/*-{sha[:12]}-*"))


def keep_sighting(job: Job, finding: dict[str, Any], entry: ld.Entry) -> None:
    """A later sighting of a known signature is evidence too, and the run
    directory holding it is deleted when the job is reaped. Keep it under the
    bundle's `sightings/` when it says something the first one cannot: it came
    after a fix (the sighting that reopens a finding is the only evidence of
    what the fix missed), it carries diagnostic evidence (DIAGNOSTIC_EVIDENCE)
    the bundle has none of (that evidence postdates most such findings, which
    never replay), or it is the first from a build the bundle has nothing from.
    The newest MAX_SIGHTINGS stay; FINDING.md lists them."""
    bundle = FINDINGS / entry.sig
    evidence = finding_evidence(job, finding)
    if evidence is None or not bundle.is_dir():
        return
    after_fix = entry.status == "reopened" or entry.fixed_in(job.sha, has_commit)
    diagnostics_missing = any(
        any(evidence.glob(pattern)) and not any(bundle.glob(f"evidence/{pattern}"))
        and not any(bundle.glob(f"sightings/*/{pattern}"))
        for pattern in DIAGNOSTIC_EVIDENCE)
    # The first recorded sighting of a finding with no recording: for one no
    # replay reproduces, the trace is the reproduction.
    recorded = (evidence.parent / RECORDING).is_dir() and recording_wanted(finding)
    first_recording = recorded and not has_recording(bundle)
    if not (after_fix or diagnostics_missing or first_recording or new_build_for(bundle, job.sha)):
        return
    sightings = bundle / "sightings"
    name = f"{time.strftime('%Y%m%d-%H%M%S')}-{job.sha[:12]}-{job.cell.replace('/', '_')}"
    target = sightings / name
    shutil.copytree(evidence, target, dirs_exist_ok=True)
    keep_run_evidence(evidence, target, finding)
    (target / "sighting.json").write_text(json.dumps(
        {"cell": job.cell, "sha": job.sha, "binary": job.binary, "build": job.type.build, "seed": job.seed,
         "record": job.record, "slow": job.slow, "command": job.command,
         "detail": finding.get("detail", ""), "step": finding.get("step"), "state": finding.get("state"),
         "status_before": entry.status}, indent=1))
    for old in sightings_to_drop(sorted(p for p in sightings.iterdir() if p.is_dir()),
                                 (bundle / "evidence" / RECORDING).is_dir()):
        shutil.rmtree(old, ignore_errors=True)
    write_finding_md(bundle)


def sightings_to_drop(sightings: list[Path], bundle_recorded: bool) -> list[Path]:
    """Which of a bundle's kept sightings (oldest first) go: all but the newest
    MAX_SIGHTINGS -- and never the newest recorded one while nothing that stays
    has a recording: unrecorded sightings arrive ten to one, and would push the
    only trace of a finding out within a day."""
    drop = sightings[:-MAX_SIGHTINGS]
    kept_recorded = bundle_recorded or any((p / RECORDING).is_dir() for p in sightings[-MAX_SIGHTINGS:])
    if not kept_recorded:
        newest = next((p for p in reversed(drop) if (p / RECORDING).is_dir()), None)
        drop = [p for p in drop if p != newest]
    return drop


def suspected_files(text: str) -> list[str]:
    import re
    files = []
    for match in re.finditer(r"(src/[\w/.+-]+\.(?:cpp|h|inc)):(\d+)", text):
        entry = f"{match.group(1)}:{match.group(2)}"
        if entry not in files:
            files.append(entry)
    return files[:12]


def similar_entries(entry_kind: str, label: str, sig: str) -> list[ld.Entry]:
    words = set(fnd.normalize(label).split())
    out = []
    for other in ledger().entries().values():
        if other.sig == sig or other.kind != entry_kind:
            continue
        overlap = len(words & set(fnd.normalize(other.label).split())) / max(1, len(words))
        if overlap > 0.5:
            out.append(other)
    return out[:5]


# What FINDING.md says of the files a bundle's evidence may hold, in order.
EVIDENCE_NOTES = (
    ("trace.txt", "the app's frame and input trace since the previous action"),
    ("log-tail.txt", "the app log's tail"),
    ("log-tail-before-relaunch.txt", "the killed process's log tail (its last flushes and saves)"),
    (RUN_LOGS, "the WHOLE log of every launch of the run, gzipped (`zcat`): what set the symptom up "
               "hundreds of actions earlier, which the tail does not reach"),
    ("machine.txt", "the machine's load and pressure when it was found (a timing finding from a starved "
                    "machine is the machine's)"),
    ("crash-stack.txt", "the crashing thread's stack, as the app printed it on its way down, resolved"),
    ("latency.txt", "where a slow answer's time went (on a CPU, waiting for one, blocked) and the main "
                    "thread's stack from the app's stall watchdog"),
    ("invariants.json", "the app's own answer on its structural rules (`invariants`), with the tabs, "
                        "documents and editor state beside it"),
    ("state.json", "the app's answers to its state queries when the finding was made"),
    ("recording.txt", "which rr trace the finding was reported in, and how far into it"),
    ("children.txt", "the app's child processes when the leak oracle fired"),
    ("fds.txt", "what the app's descriptors point at, by kind"),
    ("undo-before.txt", "the text before the sampled undos (`undo-after.txt`: after the redos)"),
    ("kill-documents.json", "the documents the app reported before the kill -9"),
    ("kill-state.tar.gz", "the killed process's state tree: what the relaunch restored from"),
    ("driver.log", "the job's own output"),
    ("microide.log", "the app's log of a launch that failed"),
)


def reproduction_text(entry: ld.Entry) -> str:
    """How a replay of the finding's actions fares, with the number of replays:
    "0%" of three says little about a finding that shows one run in ten."""
    if entry.rate is None:
        return "not measured yet"
    runs = entry.replays_ran if entry.replays_ran is not None else (entry.rate_runs or 3)
    text = (f"{'deterministic' if entry.deterministic else 'flaky'}, {round(entry.rate * runs)} of {runs} "
            f"replays of its actions showed it")
    # Not when none ran: the verdict below says that, with the same reason.
    if entry.replays_ran and entry.rate_runs and entry.replays_ran < entry.rate_runs:
        text += (f" ({entry.rate_runs - entry.replays_ran} more could not run"
                 + (f": {entry.replay_errors[0]}" if entry.replay_errors else "") + ")")
    if entry.paced:
        text += " at the recorded pace (the replays keep the run's timing; `run.py replay` reads it from the record)"
    if entry.checked_on and entry.commits and not any(c.startswith(entry.checked_on[:12]) or
                                                      entry.checked_on.startswith(c[:12]) for c in entry.commits):
        text += (f" -- replayed on build `{entry.checked_on[:12]}`, later than any that showed it (the binary of "
                 "the build that found it was gone): a fix since then also reads as zero")
    verdict = entry.replay_verdict
    if verdict == "untested":
        text += (" -- NOT TESTED: the driver finished none of them, so this says nothing about the finding"
                 + (f" ({entry.replay_errors[0]})" if entry.replay_errors else ""))
    elif verdict == "diverged":
        first = entry.divergence or {}
        text += (f" -- and {entry.replays_diverged} of them LEFT THE RUN'S PATH, first after action "
                 f"{first.get('action')} ({first.get('after')}): the run was in `{first.get('run')}`, the replay "
                 f"in `{first.get('replay')}`. The record does not rebuild the run -- a gap in the harness "
                 f"(tools/qa), not a property of the defect: look there first")
    elif verdict == "followed":
        text += (f" -- and {entry.replays_followed} of them followed the run's states to the end: the actions "
                 "are not enough (timing, a race). Use a recording (below, or the next recorded sighting), or "
                 "`soak.py hunt` to replay it under rr until it shows")
    elif not entry.rate:
        text += (" -- a replay will not reproduce it: use a recording (below, or the next recorded sighting), "
                 "or `soak.py hunt` to replay it under rr until it shows")
    if entry.replay_others and not entry.rate:
        text += ". The replays showed instead: " + "; ".join(f"`{other}`" for other in entry.replay_others[:4])
    return text


def monkey_rerun(meta: dict[str, Any]) -> str:
    """The monkey command of the run that found it, for a binary built at HEAD
    (the job's own command names the soak's binary, output and corpus)."""
    command = meta.get("command") or []
    try:
        start = command.index("monkey")
    except ValueError:
        return ""
    words, skip = [], False
    for word in command[start:]:
        if skip:
            skip = False
            continue
        if word in ("--coverage", "--corpus", "--time-budget"):
            skip = True  # the soak's own steering and budget
            continue
        words.append(word)
    return "tools/qa/run.py --binary build/microide/microide " + " ".join(words)


def recording_section(bundle: Path) -> list[str]:
    """FINDING.md's section on the rr traces the bundle holds, newest first."""
    holders = [bundle / "evidence"] + (sorted((p for p in (bundle / "sightings").iterdir() if p.is_dir()),
                                              reverse=True) if (bundle / "sightings").is_dir() else [])
    holders = [h for h in holders if (h / RECORDING).is_dir()]
    if not holders:
        return []
    lines = ["## Recording (rr)", "",
             "This finding was seen in a run recorded under `rr`. A trace replays that run's exact execution "
             "-- same instructions, same thread interleaving, same kill -- as often as needed and backwards. "
             "It IS the reproduction; no replay of the actions is needed.", ""]
    for holder in holders:
        where = holder.relative_to(bundle)
        seen_in = ""
        with contextlib.suppress(OSError):
            seen_in = next((line.split(": ", 1)[1] for line in (holder / "recording.txt").read_text().splitlines()
                            if line.startswith("trace: ")), "")
        traces = sorted(p.name for p in (holder / RECORDING).iterdir() if p.is_dir())
        lines.append(f"- `{where}/{RECORDING}/`: {', '.join(traces)}"
                     + (f"; the finding was reported in `{seen_in}`" if seen_in else ""))
    first = holders[0] / RECORDING
    with contextlib.suppress(OSError, StopIteration):
        reported = next(line.split(": ", 1)[1] for line in (holders[0] / "recording.txt").read_text().splitlines()
                        if line.startswith("trace: "))
        if (holders[0] / reported).is_dir():
            first = holders[0] / reported
    lines += ["",
              "One trace per launch of the app: after a kill -9, `launch-N` is the killed process up to its last "
              "instruction and `launch-N+1` the one that restored from what it left.", "",
              "```bash",
              f"T={first if first.name.startswith('launch-') else first / 'launch-0'}",
              "rr replay -a $T                      # run it through: the app's log comes out again",
              "rr replay -M -a $T 2>&1 | grep -a 'buffer-io'   # -M tags each line [rr <pid> <event>]",
              "rr replay -g <event> $T              # gdb AT that event; then: reverse-continue, reverse-finish,",
              "                                     #   watch -l <expr> + reverse-continue (who wrote this last?)",
              "# without a terminal, the debugger takes its commands from stdin:",
              "printf 'set pagination off\\nthread apply all bt 12\\nquit\\n' | rr replay -g <event> $T",
              "```", "",
              "The trace needs this machine's system libraries as they were; `rr pack <trace>` makes one "
              "self-contained (about 450 MB) if it must outlive an upgrade.", ""]
    return lines


def write_finding_md(bundle: Path) -> None:
    meta = json.loads((bundle / "meta.json").read_text())
    finding = meta["finding"]
    entry = ledger().entries().get(finding["signature"])
    evidence = bundle / "evidence"
    log_text = ""
    for name in ("crash-stack.txt", "log-tail.txt", "trace.txt"):
        if (evidence / name).exists():
            log_text += (evidence / name).read_text(errors="replace")
    stack = fnd.stack_key(log_text, frames=8)
    run_py = "tools/qa/run.py"
    slow = meta.get("slow", 1)
    env = f"MQA_SLOW_FACTOR={slow} " if slow != 1 else ""
    quiet = quiet_for(entry) if entry else None
    lines = [
        f"# {finding['label']}",
        "",
        f"- **Signature:** `{finding['signature']}` (oracle `{finding['kind']}`)",
        f"- **Status:** {entry.status if entry else 'new'}; seen {entry.count if entry else 1} time(s) in "
        f"{len(entry.cells) if entry else 1} cell(s)",
        f"- **Reproduction:** " + (reproduction_text(entry) if entry else "not measured yet"),
        *([f"- **Quiet:** {quiet.describe()} -- probably gone; a sighting brings it back"]
          if quiet is not None and entry.status in ("new", "reopened") and quiet.expected >= ex.QUIET_EXPECTED
          else []),
        f"- **Commit:** `{meta['sha'][:12]}`; build `{meta['build']}`; cell `{meta['cell']}`",
        f"- **Step:** {finding.get('step')} of the run, {finding.get('actions')} recorded actions; state "
        f"`{finding.get('state')}`",
        "",
        "## Symptom",
        "",
        "```",
        finding.get("detail", "")[:3000],
        "```",
        "",
        "## Reproduce",
        "",
    ]
    if (bundle / "minimal.json").exists():
        minimal = json.loads((bundle / "minimal.json").read_text())
        lines += [f"Minimal ({len(minimal.get('actions', []))} actions, from {minimal.get('minimized_from')}):", "",
                  "```bash", f"{env}{run_py} --binary build/microide/microide replay {bundle / 'minimal.json'}",
                  "```", ""]
    if (bundle / "replay.json").exists():
        lines += ["Full replay:", "", "```bash",
                  f"{env}{run_py} --binary build/microide/microide replay {bundle / 'replay.json'}", "```", ""]
    rerun = monkey_rerun(meta)
    if rerun:
        lines += ["A fresh monkey run of the same profile, window and seed number. NOT the same walk: the "
                  "run steered by the soak's coverage and may have started from a corpus prefix, so this "
                  "explores differently -- it is a way to look for the finding again in its cell (add `--rr` "
                  "to record it), not a reproduction. The replay above is the run's own actions:", "",
                  "```bash", f"{env}{rerun}", "```", ""]
    lines += [f"The binary that found it: `{meta['binary']}`.", ""]
    lines += recording_section(bundle)
    if stack:
        lines += ["## Stack (ours)", "", "```", stack.replace(" > ", "\n"), "```", ""]
    if (evidence / "stall-threads.txt").exists():
        lines += ["## Threads at the stall", "", "```", (evidence / "stall-threads.txt").read_text()[:4000], "```", ""]
    # The data oracle's byte evidence: where a clean buffer and its file part, with
    # both sides saved beside it as mismatch-<name>.buffer / .disk.
    for summary in sorted(evidence.glob("mismatch-*.txt")) if evidence.exists() else []:
        lines += [f"## Byte mismatch ({summary.name})", "", "```",
                  summary.read_text(errors="replace")[:4000], "```", ""]
    sightings = sorted((p for p in (bundle / "sightings").iterdir() if p.is_dir()), reverse=True) \
        if (bundle / "sightings").is_dir() else []
    if sightings:
        lines += ["## Later sightings", "",
                  "Kept because they came after a fix, or carry diagnostic evidence the first one lacks; "
                  "newest first. Each directory holds that run's evidence and its `replay.json`.", ""]
        for path in sightings:
            try:
                info = json.loads((path / "sighting.json").read_text())
            except (OSError, ValueError):
                info = {}
            lines += [f"- `sightings/{path.name}`: commit `{str(info.get('sha', ''))[:12]}`, cell "
                      f"`{info.get('cell', '')}`, status before `{info.get('status_before', '')}`",
                      "", "  ```", "  " + str(info.get("detail", ""))[:600], "  ```", ""]
            for summary in sorted(path.glob("mismatch-*.txt")):
                lines += ["  ```", *("  " + l for l in summary.read_text(errors="replace")[:2000].splitlines()),
                          "  ```", ""]
    files = suspected_files(log_text)
    if files:
        lines += ["## Suspected files", ""] + [f"- `{f}`" for f in files] + [""]
    listed = [f"- `evidence/{p.name}`" for p in sorted(evidence.glob("*.png"))] if evidence.exists() else []
    for name, what in EVIDENCE_NOTES:
        if (evidence / name).exists():
            listed.append(f"- `evidence/{name}`: {what}")
    if listed:
        lines += ["## Evidence", "", *listed, ""]
    similar = similar_entries(finding["kind"], finding["label"], finding["signature"])
    if similar:
        lines += ["## Similar findings in the ledger", ""]
        for other in similar:
            lines.append(f"- `{other.sig}` {other.label} -- {other.status}"
                         + (f", fixed in `{other.fix_commit}`" if other.fix_commit else ""))
        lines.append("")
    how = ("reproduce from the minimal replay" if entry is None or entry.rate is None or entry.rate
           else "start from a recording if there is one (no replay reproduces it; `tools/soak/AGENT.md` "
                "§ A finding no replay reproduces)")
    lines += ["## For the fixing agent", "", "Follow `tools/soak/AGENT.md`: claim it "
              f"(`tools/soak/soak.py ledger claim {finding['signature']}`), {how}, "
              "write the failing-first test, fix, and mark it fixed with the commit.", ""]
    (bundle / "FINDING.md").write_text("\n".join(lines))


# ---- checks: determinism, minimization, verification ------------------------------

# How many replays measure a new finding's rate. Three for paint and layout
# (hundreds of signatures, most of them deterministic); twelve for what costs a
# user work or time: three replays miss a finding that shows one run in five
# half the time, and "0%" then read as "cannot be reproduced" -- the rate every
# one of the 43 open severe findings had on 2026-11-16.
CHECK_RUNS = (12, 12, 3, 3)


# What a check's report (minimize.summarize_runs) adds to the ledger's `checked`
# event beside the rate.
CHECK_REPORT_FIELDS = ("ran", "followed", "diverged", "divergence", "errors", "others")


def check_runs(finding: dict[str, Any]) -> int:
    return CHECK_RUNS[tier_of(finding.get("kind", ""), finding.get("label", ""))]


def verify_runs(rate: float) -> int:
    """Replays that show a defect still there, at its measured replay rate, 19
    times in 20: three for a deterministic one, more for a flaky one.

    >>> [verify_runs(r) for r in (1.0, 2 / 3, 1 / 3, 1 / 12)]
    [3, 3, 8, 24]
    """
    if rate >= 1.0:
        return 3
    return max(3, min(24, math.ceil(math.log(0.05) / math.log(1.0 - rate))))


def check_binary(meta: dict[str, Any], state: dict[str, Any]) -> tuple[str | None, str]:
    """The binary a finding's replays run, and the commit it was built at: the
    build that found it while the runner still has it, else the newest good
    build of the same configuration; (None, "") when there is neither.

    A build's binary goes once the next one replaced it and no process runs it
    (prune_binaries), and a
    check named the finding's own and nothing else. Every check of a finding
    older than that -- all of the open ones, the day replays learned to say
    whether they followed the run and the old rates were queued to be measured
    again -- copied a file that was not there, printed nothing, and was booked
    as "NOT TESTED: the driver finished none of them", which nothing requeues."""
    found = str(meta.get("binary") or "")
    if found and Path(found).exists():
        return found, str(meta.get("sha") or "")
    good = state.get("good", {}).get(str(meta.get("build") or "default"))
    if good and Path(good.get("binary", "")).exists():
        return good["binary"], str(good.get("sha") or "")
    return None, ""


def check_finding(sig: str, state: dict[str, Any], log) -> None:
    """Replays a new finding three times for its rate; minimizes a deterministic
    one. Runs as a job of the runner (it holds three displays at once)."""
    bundle = FINDINGS / sig
    replay = bundle / "replay.json"
    meta_path = bundle / "meta.json"
    if not replay.exists() or not meta_path.exists():
        return
    meta = json.loads(meta_path.read_text())
    binary, checked_on = check_binary(meta, state)
    if binary is None:
        # Nothing to replay it with: say so and book no measurement -- a rate of
        # zero from replays that never ran is not one.
        log(f"  {sig}: no {meta.get('build', 'default')} binary to replay it with; left unmeasured")
        return
    env = {**os.environ, "MQA_SLOW_FACTOR": str(meta.get("slow", 1))}
    minimize = [sys.executable, str(driver_root(state) / "tools" / "soak" / "minimize.py"), str(replay),
                "--signature", sig, "--binary", binary, "--out", str(bundle)]
    runs = check_runs(meta["finding"])
    result = subprocess.run(["nice", "-n", "19", *minimize, "--check-only", "--runs", str(runs), "--jobs", "3"],
                            capture_output=True, text=True, env=env, timeout=5400)
    report: dict[str, Any] = {}
    for line in result.stdout.splitlines():
        if line.startswith("{"):
            with contextlib.suppress(ValueError):
                report = json.loads(line)
    rate, paced = float(report.get("rate", 0.0)), bool(report.get("paced", False))
    actions = len(json.loads(replay.read_text()).get("actions", []))
    deterministic = rate >= 2 / 3
    if not report:
        # The check itself died (a timeout, a broken driver): no replay ran.
        report = {"ran": 0, "errors": [f"the check printed no result: {(result.stderr or result.stdout)[-200:]}"]}
    ledger().append(sig, "checked", rate=rate, runs=runs, deterministic=deterministic, actions=actions,
                    paced=paced, on=checked_on, **{key: report[key] for key in CHECK_REPORT_FIELDS if key in report})
    log(f"  checked {sig}: {round(rate * report.get('ran', runs))} of {report.get('ran', runs)} replays"
        + (" at the recorded pace" if paced else "")
        + (f"; {report['diverged']} left the run's path" if report.get("diverged") else "")
        + (f"; {runs - report['ran']} could not run" if report.get("ran", runs) < runs else ""))
    if paced:
        # Every later replay of it -- the minimizer's, the agent's, the fix's
        # verification -- keeps the recorded pace.
        replay.write_text(json.dumps({**json.loads(replay.read_text()), "paced": True}))
    if deterministic:
        if shutil.which("notify-send"):
            subprocess.run(["notify-send", "microide soak: new deterministic finding",
                            json.loads(meta_path.read_text())["finding"]["label"]], capture_output=True)
        # Bounded, and preempted by a pause: a minimize ran for hours (its budget
        # started after its first replays) and `pause` waited for it, so lanes and
        # timing slots waited too. Stopped, it keeps the smallest replay it found.
        result = subprocess.run(["nice", "-n", "19", *minimize, "--jobs", "4",
                                 "--time-budget", str(MINIMIZE_BUDGET_S), "--stop-file", str(PAUSE)],
                                capture_output=True, text=True, env=env, timeout=MINIMIZE_BUDGET_S + 900)
        for line in result.stdout.splitlines():
            if not line.startswith("{"):
                continue
            with contextlib.suppress(ValueError):
                outcome = json.loads(line)
                stopped = outcome.get("stopped")
                if outcome.get("reproduced"):
                    ledger().append(sig, "minimized", actions=outcome["actions"],
                                    **({"stopped": stopped} if stopped else {}))
                    log(f"  minimized {sig}: {outcome['actions']} actions"
                        + (f" (minimize: {stopped})" if stopped else ""))
                elif stopped:
                    log(f"  minimize of {sig}: {stopped} before its first replay")
    write_finding_md(bundle)


# The most findings requeue_unmeasured puts back in the check queue at a time: a
# check is replays, and new findings' checks come first.
REQUEUE_BATCH = 40


def requeue_unmeasured(state: dict[str, Any], log) -> int:
    """Puts the open findings whose "no replay reproduces it" predates the
    replays' own fidelity back in the check queue, the most severe first.

    Their rate of 0 was measured by replays that named files of a deleted run
    directory, dropped a corpus prefix's outside-world steps and did not say
    whether they followed the run (Entry.replays_ran is None for those): it is
    not a measurement. Checked again, each gets a rate a replay can be held
    to and a verdict on its path. Returns how many it queued.

    So is a check none of whose replays ran (`untested`: a pruned binary, a
    remote finding with no server to install): measured again once per build,
    since the next build may be the one that can.

    Only what this runner has seen. A signature is a function of the driver's
    rules, and those change (a crash keyed on its stack, a slow answer on its
    verb): a finding under an older rule is never shown again under its old
    signature, by the soak or by a replay, so replaying it measures nothing --
    and there were hundreds, each a dozen replays. They go quiet and close."""
    pending = state.setdefault("pending_checks", [])
    running = set(state.get("current", []))
    entries = ledger().entries()
    built = str(state.get("built_sha") or "")
    since = float(state.get("started") or 0.0)

    def unmeasured(entry: ld.Entry) -> bool:
        if entry.replays_ran is None:
            return True  # a check from before replays said whether they ran
        return entry.replays_ran == 0 and bool(built) and entry.checked_on != built

    stale = [e for e in entries.values()
             if e.status in ("new", "reopened") and e.rate == 0 and unmeasured(e) and e.last_seen >= since
             and e.sig not in pending and f"check/{e.sig}" not in running
             and (FINDINGS / e.sig / "replay.json").exists()]
    room = max(0, REQUEUE_BATCH - len(pending))
    chosen = sorted(stale, key=priority_key)[:room]
    for entry in chosen:
        pending.append(entry.sig)
    if chosen:
        log(f"requeued {len(chosen)} finding(s) whose replay rate predates faithful replays "
            f"({len(stale) - len(chosen)} more wait)")
    return len(chosen)


def fixes_to_verify(state: dict[str, Any]):
    """The fixed ledger entries a replay can check -- one has reproduced them
    (Entry.replay_reproduces) -- whose fix commit the default build contains,
    each with that build's record. The rest wait for exposure
    (verify_by_exposure)."""
    good = state.get("good", {}).get("default")
    if not good:
        return
    for entry in ledger().entries().values():
        if entry.status != "fixed" or not entry.fix_commit or not entry.replay_reproduces:
            continue
        if has_commit(entry.fix_commit, good["sha"]):
            yield entry, good


def verify_by_exposure(log) -> None:
    """Books `verified` for the fixed findings no replay ever reproduced, once
    the soak has run long enough on builds with the fix: VERIFIED_EXPECTED
    sightings due at the finding's old rate and none seen (exposure.py).

    Their verification used to be the same three replays as everyone's. A
    replay that never showed the defect shows nothing after any commit at all,
    so every such fix was "verified" within the hour: 141 of the ledger's 173
    verified findings on 2026-11-16, 35 of which came back. A sighting from a
    build with the fix still reopens one at once (Ledger.entries)."""
    for entry in ledger().entries().values():
        if entry.status != "fixed" or not entry.fix_commit or entry.replay_reproduces:
            continue
        quiet = fix_exposure(entry)
        if quiet is None or quiet.expected < ex.VERIFIED_EXPECTED:
            continue
        ledger().append(entry.sig, "verified", by="exposure", runs=quiet.runs, expected=round(quiet.expected, 1),
                        note=f"by exposure: {quiet.describe()}, on builds with {entry.fix_commit[:12]}")
        freed = prune_verified_bundle(FINDINGS / entry.sig)
        log(f"verified {entry.sig} by exposure ({quiet.describe()}); pruned {freed / 1e6:.1f} MB")


def close_quiet(log) -> None:
    """Books `closed` for the open findings the soak has stopped showing for
    good: CLOSED_EXPECTED sightings due at the old rate, over runs of the job
    types that showed them, and none seen (exposure.py) -- fixed by a commit
    nobody booked against them, or a fault of the harness of the day. Only the
    job types that showed a finding count, so a cell that stopped running (the
    TSan cell, dead for four days) closes nothing. A claimed one is someone's
    work and stays. Any later sighting reopens it (Ledger.entries), and its
    bundle keeps the replay; the evidence goes, as a verified one's does."""
    for entry in ledger().entries().values():
        if entry.status not in ("new", "reopened"):
            continue
        quiet = quiet_for(entry)
        if quiet is None or quiet.expected < ex.CLOSED_EXPECTED:
            continue
        ledger().append(entry.sig, "closed", by="exposure", runs=quiet.runs, expected=round(quiet.expected, 1),
                        note=f"quiet: {quiet.describe()}")
        freed = prune_verified_bundle(FINDINGS / entry.sig)
        log(f"closed {entry.sig} ({quiet.describe()}); pruned {freed / 1e6:.1f} MB")


def verify_fixed(state: dict[str, Any], log) -> None:
    """Queues every fixed signature whose fix commit is in the current default
    build for a verify job (start_verify, `soak.py verify SIG`), which replays it:
    gone -> verified, back -> reopened.

    Queued, not replayed here: this runs inside the scheduling loop, and a
    replay is three runs of up to an hour each. Inline, a build that brought in
    five fixes kept the runner from starting any job for as long as their
    replays took, one after another (2026-11-13: zero jobs for the better part
    of an hour after a restart)."""
    queued = state.setdefault("pending_verifies", [])
    running = set(state.get("current", []))
    for entry, _good in fixes_to_verify(state):
        if entry.sig not in queued and f"verify/{entry.sig}" not in running:
            queued.append(entry.sig)
            log(f"  queued {entry.sig} to verify")


# Open findings a replay has reproduced are replayed again on newer builds: a
# fix elsewhere often cures one nobody booked it against (the ui.scale 1.1 clip
# fix closed 11 of 37 such findings when they were replayed by hand), and
# otherwise it stays open until its rate's worth of runs has passed without it
# -- weeks for a rare one. At most RECHECK_BATCH a build, each at most once
# every RECHECK_INTERVAL, the most severe first; a claimed one is someone's.
RECHECK_BATCH = 8
RECHECK_INTERVAL = 24 * 3600.0


def open_to_recheck(state: dict[str, Any], now: float | None = None) -> list[ld.Entry]:
    good = state.get("good", {}).get("default")
    if not good:
        return []
    now = time.time() if now is None else now
    queued = set(state.get("pending_verifies", []))
    running = set(state.get("current", []))
    out = []
    for entry in ledger().entries().values():
        if entry.status not in ("new", "reopened") or not entry.replay_reproduces:
            continue
        if entry.rechecked_on == good["sha"] or now - entry.rechecked_at < RECHECK_INTERVAL:
            continue
        if entry.sig in queued or f"verify/{entry.sig}" in running:
            continue
        bundle = FINDINGS / entry.sig
        if not ((bundle / "minimal.json").exists() or (bundle / "replay.json").exists()):
            continue
        out.append(entry)
    return sorted(out, key=priority_key)


def recheck_open(state: dict[str, Any], log) -> None:
    """Queues up to RECHECK_BATCH reproducing open findings to be replayed on the
    current default build, through the verify queue (`soak.py verify SIG`)."""
    queued = state.setdefault("pending_verifies", [])
    for entry in open_to_recheck(state)[:RECHECK_BATCH]:
        queued.append(entry.sig)
        log(f"  queued {entry.sig} to re-check on the new build")


def recheck_one(entry: ld.Entry, good: dict[str, Any], state: dict[str, Any], log) -> None:
    """Replays an open finding on `good`: gone -> closed (a later sighting
    reopens it, and the bundle keeps its replays), still there -> rechecked."""
    bundle = FINDINGS / entry.sig
    replay = bundle / "minimal.json" if (bundle / "minimal.json").exists() else bundle / "replay.json"
    runs = verify_runs(entry.rate or 1.0)
    result = subprocess.run(["nice", "-n", "19", sys.executable, str(driver_root(state) / "tools/soak/minimize.py"),
                             str(replay), "--signature", entry.sig, "--binary", good["binary"],
                             "--check-only", "--runs", str(runs), "--jobs", "3",
                             "--out", str(bundle / "verify")],
                            capture_output=True, text=True, timeout=3600)
    report = next((json.loads(l) for l in result.stdout.splitlines() if l.startswith("{")), None)
    if report is None or not report.get("ran", runs):
        ledger().append(entry.sig, "rechecked", on=good["sha"], rate=None, note="no replay ran")
        log(f"re-check of {entry.sig}: no replay ran; left open")
        return
    rate = float(report.get("rate", 1.0))
    if rate == 0:
        ledger().append(entry.sig, "closed", by="recheck", commit=good["sha"],
                        note=f"no longer reproduces on {good['sha'][:10]}: 0 of {report.get('ran', runs)} replays "
                             f"(it used to, at {entry.rate:.0%})")
        log(f"closed {entry.sig}: no longer reproduces on {good['sha'][:10]}; "
            f"pruned {prune_verified_bundle(bundle) / 1e6:.1f} MB")
    else:
        ledger().append(entry.sig, "rechecked", on=good["sha"], rate=rate)
        log(f"re-checked {entry.sig}: still there on {good['sha'][:10]} ({rate:.0%})")


def verify_one(sig: str, state: dict[str, Any], log) -> None:
    """Replays one fixed signature on the current default build (its minimal
    replay, else the full one) and books the verdict."""
    match = next(((entry, good) for entry, good in fixes_to_verify(state) if entry.sig == sig), None)
    if match is None:
        entry = ledger().entries().get(sig)
        good = state.get("good", {}).get("default")
        if entry is not None and good and entry.status in ("new", "reopened") and entry.replay_reproduces:
            recheck_one(entry, good, state, log)
            return
        log(f"{sig}: not a fixed signature the current build contains; nothing to verify")
        return
    entry, good = match
    bundle = FINDINGS / entry.sig
    replay = bundle / "minimal.json" if (bundle / "minimal.json").exists() else bundle / "replay.json"
    if not replay.exists():
        log(f"{entry.sig}: no replay to re-check; left to exposure")
        return
    # As often as its rate needs (verify_runs); --check-only tries the default
    # pace and then the recorded one, so a paced finding is replayed paced.
    result = subprocess.run(["nice", "-n", "19", sys.executable, str(driver_root(state) / "tools/soak/minimize.py"),
                             str(replay), "--signature", entry.sig, "--binary", good["binary"],
                             "--check-only", "--runs", str(verify_runs(entry.rate or 1.0)), "--jobs", "3",
                             "--out", str(bundle / "verify")],
                            capture_output=True, text=True, timeout=3600)
    rate = next((json.loads(l)["rate"] for l in result.stdout.splitlines() if l.startswith("{")), 1.0)
    if rate == 0:
        ledger().append(entry.sig, "verified", by="replay", commit=good["sha"])
        log(f"verified {entry.sig}; pruned {prune_verified_bundle(bundle) / 1e6:.1f} MB of its evidence")
    else:
        ledger().append(entry.sig, "reopened", commit=good["sha"], rate=rate)
        log(f"REOPENED {entry.sig} (rate {rate:.0%} on {good['sha'][:10]})")


# ---- housekeeping ----------------------------------------------------------------

MIN_FREE_BYTES = int(float(os.environ.get("MICROIDE_SOAK_MIN_FREE_GB", "30")) * 1e9)


# ---- containment ------------------------------------------------------------------

# Who the monkey's git is (tools/qa/monkey.py git_env) and the branches it makes.
QA_GIT_EMAIL = "qa@example.com"
QA_BRANCH_PREFIX = "qa-branch-"
CONTAINMENT_INTERVAL = 30.0


def containment_breach() -> str | None:
    """What a run did to the CHECKOUT, if anything.

    Every scratch project lives under build/, inside the checkout, and once a
    run's git escaped into it: a commit on main, a branch switch, a stash, and a
    qa-branch pushed to GitHub, all while the runner kept going. The fences
    (GIT_CEILING_DIRECTORIES, a bare stand-in origin, the app's own ceiling) are
    what prevents it; this is the alarm for whatever gets past them. It reads
    every ref's tip: a monkey commit, stash or branch is at a tip the moment it
    is made, and the check runs every CONTAINMENT_INTERVAL seconds."""
    rows = git("for-each-ref", "--format=%(refname)\t%(authoremail)\t%(committeremail)")
    hits = []
    for row in rows.splitlines():
        refname, _, emails = row.partition("\t")
        if (refname.rsplit("/", 1)[-1].startswith(QA_BRANCH_PREFIX)
                or f"<{QA_GIT_EMAIL}>" in emails.split("\t")):
            hits.append(refname)
    head = git("log", "-1", "--format=%ae", "HEAD")
    if head == QA_GIT_EMAIL:
        hits.append("HEAD")
    return ", ".join(hits) if hits else None


def contain_breach(breach: str, jobs: list["Job"], log) -> None:
    """Stop everything and say so: kill the running jobs (one of them did it),
    pause the runner until a person has looked, and record a finding."""
    cells = [f"{job.cell} seed {job.seed}" for job in jobs]
    for job in jobs:
        if job.proc.poll() is None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(job.proc.pid, signal.SIGKILL)
    message = (f"CONTAINMENT BREACH: QA git activity reached the checkout ({breach}). "
               f"Running when found: {'; '.join(cells) or 'none'}. Runner paused: undo the "
               f"damage (main, qa-branch-* refs, the stash, origin), then delete {PAUSE}.")
    log(message)
    PAUSE.write_text(message + "\n")
    signature = fnd.signature("containment", "QA git activity reached the checkout")
    ledger().append(signature, "new" if signature not in ledger().entries() else "seen",
                    kind="containment", label="containment: QA git activity reached the checkout",
                    commit=followed_head(), cell="runner", detail=message)


# The first line of a PAUSE the fixing agent wrote for its phase (`pause`): the
# soak and the agent take turns on the machine, each with every core. Any other
# PAUSE -- a containment breach -- waits for a person, and `resume` leaves it.
PHASE_PAUSE_MARK = "phase: "


def pause_for_phase(reason: str) -> bool:
    """Stops new jobs and builds for the agent's phase. False, and nothing
    written, when the runner is already paused for something else."""
    if PAUSE.exists() and not PAUSE.read_text(errors="replace").startswith(PHASE_PAUSE_MARK):
        return False
    PAUSE.write_text(f"{PHASE_PAUSE_MARK}{reason}\n")
    return True


def resume_from_phase() -> bool:
    """Lifts a phase pause. False when the pause is not one (it stays)."""
    if not PAUSE.exists():
        return True
    if not PAUSE.read_text(errors="replace").startswith(PHASE_PAUSE_MARK):
        return False
    PAUSE.unlink()
    return True


def build_paused() -> bool:
    """A paused runner builds nothing either: a push during the agent's phase
    would otherwise start five builds beside its lanes."""
    return PAUSE.exists()


def wait_for_idle(timeout: float, poll: float = 10.0) -> bool:
    """Until the runner's running jobs and checks have ended (they finish under
    a pause; nothing new starts). False at `timeout` with some still running."""
    deadline = time.monotonic() + timeout
    while load_state().get("current"):
        if time.monotonic() >= deadline:
            return False
        time.sleep(poll)
    return True


def busy_reason(our_cores: float, others: float | None = None) -> str | None:
    """Why no job may start now, or None. `others` is the cores something other
    than the soak uses (JobLoad.others); without it, the load average less ours."""
    if PAUSE.exists():
        return "paused (build/soak/PAUSE)"
    free = shutil.disk_usage(SOAK).free
    if free < MIN_FREE_BYTES:
        return f"only {free / 1e9:.0f} GB free on the disk (MICROIDE_SOAK_MIN_FREE_GB)"
    # A run-checks lane is not a reason to stop: every soak process runs at nice
    # 19, idle I/O and a cgroup CPU weight of 10 (microide-soak.service), so a
    # lane takes the cores it wants first, and the load below -- a lane's load
    # included -- is what keeps new jobs from starting on a busy machine. The
    # runner used to stand idle for every lane anyone ran, which with several
    # sessions on the machine was most of the day (2026-11-13).
    cores = os.cpu_count() or 4
    load = os.getloadavg()[0]
    other = others if others is not None else max(0.0, load - our_cores)
    if other > cores * BUSY_FRACTION:
        return f"the machine is busy ({other:.1f} cores not the soak's; load {load:.1f})"
    return None


IDLE_NOTE_SECONDS = 15 * 60


def idle_note(idle: dict[str, Any], reason: str | None, now: float) -> str | None:
    """The log line for a reason to start nothing that has held IDLE_NOTE_SECONDS
    (once), and for its end, with how long it held. Its numbers aside, a reason is
    one wait. The main soak stood idle 40 hours under the disk floor (2026-11-27)
    and its log said nothing: the reason lived only in `status`."""
    kind = re.sub(r"\d+(\.\d+)?", "<n>", reason) if reason is not None else None
    if kind == idle.get("kind"):
        if kind is not None and not idle.get("noted") and now - idle["since"] >= IDLE_NOTE_SECONDS:
            idle["noted"] = True
            return f"starting nothing for {(now - idle['since']) / 60:.0f} min: {reason}"
        return None
    noted, since = idle.get("noted"), idle.get("since", now)
    idle.clear()
    if kind is not None:
        idle.update(kind=kind, since=now, noted=False)
    return f"starting jobs again after {(now - since) / 3600:.1f} h" if noted else None


# A job's cost before one is measured (JobLoad): the app, Xvfb and the driver.
# Measured, a job costs about twice this (an ASan app alone takes a core and a
# half), and the estimate -- the whole accounting until 2026-11-12 -- booked the
# soak's own load as someone else's: at five running jobs the load read 16 and
# the budget fell to nothing on a machine running only the soak.
CORES_PER_JOB = 1.5
# Someone else's load above this share of the cores: start nothing.
BUSY_FRACTION = 0.6
# The share of the cores the soak may fill when nothing else runs. The machine is
# the soak's (2026-11-13: "full use of the PC"), and every soak process runs at
# the lowest priority, so anything else started on it still gets the cores first;
# the ramp (RAMP_SECONDS) covers the load average's minute of lag.
FILL_FRACTION = 1.0
# The least time between two job starts: an idle night reaches the ceiling in a
# few minutes, and a job's cost is in the load average before the next starts.
RAMP_SECONDS = 20.0


class JobLoad:
    """The cores the soak's own jobs use, measured rather than assumed: the CPU
    time of every process in their sessions (each job is a session of its own:
    start_new_session), children already reaped included (cutime/cstime), over
    the time since the last sample."""

    WINDOW = 15.0

    def __init__(self) -> None:
        self._time = 0.0
        self._ticks: dict[int, int] = {}
        self._machine: tuple[int, int] | None = None
        self.cores: float | None = None
        # The cores the whole machine kept busy over the same window (/proc/stat),
        # the soak's own included: what is not the soak is `busy - cores`.
        self.busy: float | None = None
        # The resident memory of the same sessions, in bytes, as of the last scan.
        self.rss: int | None = None

    @staticmethod
    def session_ticks(sessions: set[int], rss: list[int] | None = None) -> dict[int, int]:
        """CPU ticks per session; with `rss`, also appends the sessions' total
        resident bytes (stat's rss field, in pages)."""
        totals: dict[int, int] = {}
        resident = 0
        page = os.sysconf("SC_PAGE_SIZE")
        with contextlib.suppress(OSError):
            for entry in os.scandir("/proc"):
                if not entry.name.isdigit():
                    continue
                try:
                    with open(f"/proc/{entry.name}/stat") as handle:
                        stat = handle.read()
                except OSError:
                    continue
                fields = stat[stat.rfind(")") + 2:].split()
                if len(fields) < 22 or int(fields[3]) not in sessions:
                    continue
                totals[int(fields[3])] = totals.get(int(fields[3]), 0) + sum(int(f) for f in fields[11:15])
                resident += int(fields[21]) * page
        if rss is not None:
            rss.append(resident)
        return totals

    def sample(self, sessions: set[int]) -> float | None:
        now = time.monotonic()
        if self._time and now - self._time < self.WINDOW:
            return self.cores
        resident: list[int] = []
        ticks = self.session_ticks(sessions, resident)
        self.rss = resident[0] if resident else None
        machine = self.machine_ticks()
        if self._time:
            # A session seen both times: its growth. One that ended took its time
            # with it (no negative deltas); one that started counts from its first sample.
            used = sum(max(0, ticks[s] - self._ticks[s]) for s in ticks if s in self._ticks)
            self.cores = used / os.sysconf("SC_CLK_TCK") / (now - self._time)
            if machine is not None and self._machine is not None:
                busy = machine[0] - self._machine[0]
                total = machine[1] - self._machine[1]
                if total > 0:
                    self.busy = (os.cpu_count() or 1) * busy / total
        self._time, self._ticks, self._machine = now, ticks, machine
        return self.cores

    @staticmethod
    def machine_ticks() -> tuple[int, int] | None:
        """(busy, total) CPU ticks of the whole machine since boot, from /proc/stat's
        `cpu` line: busy is everything but idle and iowait."""
        try:
            with open("/proc/stat") as handle:
                fields = [int(f) for f in handle.readline().split()[1:9]]
        except (OSError, ValueError):
            return None
        return sum(fields) - fields[3] - fields[4], sum(fields)

    def others(self, fallback_load: float) -> float:
        """The cores something other than the soak uses now: measured over the
        same window as the soak's own when both are known. The load average,
        which this replaces once measured, trails by a minute: the CPU of jobs
        that had just ended was still in it and no longer in the soak's own
        measurement, so it was booked as someone else's, the budget fell to
        zero and a machine running two jobs idled (2026-11-13)."""
        if self.busy is not None and self.cores is not None:
            return max(0.0, self.busy - self.cores)
        return max(0.0, fallback_load - (self.cores or 0.0))


def max_jobs(cores: int) -> int:
    """The most jobs at once: one per core. Only a bound -- the measured cost of
    the running jobs (allowed_jobs) and the memory they hold (memory_jobs) decide
    how many fit. It used to be the cores over the 1.5-core estimate, which held a
    machine whose cheap jobs measure under a core to 9 of 16 (2026-11-13).

    >>> max_jobs(16)
    16
    """
    return max(1, int(cores * FILL_FRACTION))


def allowed_jobs(load: float, our_cores: float | None, our_jobs: int, cores: int, ceiling: int) -> int:
    """How many jobs may run at once now: the cores others leave free, in jobs.

    `our_cores` is what the soak's jobs measurably use (None before a sample:
    CORES_PER_JOB each); a job is counted at what the running ones cost on average.

    Running jobs are never killed when this falls; they finish (each has a hard
    timeout) and are not replaced. The load average lags by about a minute, so
    the runner starts at most one job per RAMP_SECONDS and the ramp shows in
    the load before it goes further.

    >>> allowed_jobs(0.0, None, 0, 16, 9)     # idle: the ceiling
    9
    >>> allowed_jobs(9.0, None, 6, 16, 9)     # all of it ours, estimated: still the ceiling
    9
    >>> allowed_jobs(16.0, 15.0, 5, 16, 9)    # five jobs measured at 3 cores: five fit
    5
    >>> allowed_jobs(16.0, None, 5, 16, 9)    # the same, estimated: 8.5 cores booked as not ours
    5
    >>> allowed_jobs(7.0, None, 0, 16, 9)     # someone compiling on 7 cores
    6
    >>> allowed_jobs(14.0, 0.0, 0, 16, 9)     # busy: nothing new
    0
    """
    ours = our_cores if our_cores is not None else CORES_PER_JOB * our_jobs
    other = max(0.0, load - ours)
    if other > cores * BUSY_FRACTION:
        return 0
    per_job = max(1.0, ours / our_jobs) if our_jobs and our_cores is not None else CORES_PER_JOB
    free = cores * FILL_FRACTION - other
    return max(1, min(ceiling, int(free / per_job)))


# Memory: what one job holds before any is measured (the app, Xvfb, the driver;
# an ASan app, clangd or a check's parallel replays hold several times this),
# and what the machine keeps free whatever the soak would like to run.
BYTES_PER_JOB = 3 * 2**30
MEMORY_RESERVE_FRACTION = 0.15


def memory_available() -> tuple[int, int]:
    """(MemAvailable, MemTotal) in bytes, from /proc/meminfo."""
    values: dict[str, int] = {}
    with contextlib.suppress(OSError), open("/proc/meminfo") as meminfo:
        for line in meminfo:
            name, _, rest = line.partition(":")
            if name in ("MemAvailable", "MemTotal"):
                values[name] = int(rest.split()[0]) * 1024
    return values.get("MemAvailable", 0), values.get("MemTotal", 0)


def memory_jobs(available: int, total: int, our_rss: int | None, our_jobs: int) -> int:
    """How many jobs memory allows at once: those running, plus as many more as
    fit in what is available above the reserve, each at what a running job holds
    on average.

    The budget followed the cores alone. On 2026-11-13 systemd-oomd killed the
    whole runner at a 53.6 GB peak on a 61 GB machine: two tooling jobs with
    clangd, an ASan job and checks replaying four apps at once, each counted as
    one job of a core and a half. Running jobs are never killed for this; they
    finish and are not replaced.

    >>> G = 2**30
    >>> memory_jobs(40 * G, 61 * G, None, 0)        # idle: (40 - 9.15) / 3
    10
    >>> memory_jobs(14 * G, 61 * G, 30 * G, 6)      # 5 GB each, 4.85 GB spare: none more
    6
    >>> memory_jobs(8 * G, 61 * G, 40 * G, 7)       # under the reserve: not even those
    7
    >>> memory_jobs(30 * G, 61 * G, 6 * G, 3)       # 2 GB each, but no less than the estimate
    9
    """
    reserve = int(total * MEMORY_RESERVE_FRACTION)
    per_job = max(BYTES_PER_JOB, our_rss // our_jobs) if our_rss and our_jobs else BYTES_PER_JOB
    spare = available - reserve
    return our_jobs + max(0, spare // per_job)


def disk_usage(path: Path) -> int:
    """Bytes under `path`, each file once however many names it has: a run's
    logs and traces are hard-linked into every bundle that keeps them
    (link_tree), and a trace links the build's binary."""
    total = 0
    seen: set[tuple[int, int]] = set()
    for root, _, files in os.walk(path):
        for name in files:
            try:
                stat = os.lstat(os.path.join(root, name))
            except OSError:
                continue
            if stat.st_nlink > 1:
                if (stat.st_dev, stat.st_ino) in seen:
                    continue
                seen.add((stat.st_dev, stat.st_ino))
            total += stat.st_size
    return total


RECORDING_BUDGET_GB = float(os.environ.get("MICROIDE_SOAK_RECORDING_GB", "15"))


def enforce_recording_budget(log) -> None:
    """Keeps the bundles' rr traces within RECORDING_BUDGET_GB: the oldest go
    first, except that a finding's only recording outlives every finding's
    second one."""
    budget = RECORDING_BUDGET_GB * 1e9
    holders = sorted((p for pattern in (f"*/evidence/{RECORDING}", f"*/sightings/*/{RECORDING}")
                      for p in FINDINGS.glob(pattern) if p.is_dir()), key=lambda p: p.stat().st_mtime)
    if not holders or disk_usage_of(holders) <= budget:
        return
    def bundle_of(holder: Path) -> Path:
        return next(parent for parent in holder.parents if parent.parent == FINDINGS)
    per_bundle: dict[Path, int] = {}
    for holder in holders:
        per_bundle[bundle_of(holder)] = per_bundle.get(bundle_of(holder), 0) + 1
    removed = 0
    for only_spares in (True, False):
        for holder in list(holders):
            if disk_usage_of(holders) <= budget * 0.9:
                break
            bundle = bundle_of(holder)
            if only_spares and per_bundle[bundle] <= 1:
                continue
            shutil.rmtree(holder, ignore_errors=True)
            holders.remove(holder)
            per_bundle[bundle] -= 1
            removed += 1
    if removed:
        log(f"recording budget: dropped the {removed} oldest rr trace set(s)")


def disk_usage_of(paths: list[Path]) -> int:
    """disk_usage over several trees, a file they share counted once."""
    total = 0
    seen: set[tuple[int, int]] = set()
    for path in paths:
        for root, _, files in os.walk(path):
            for name in files:
                with contextlib.suppress(OSError):
                    stat = os.lstat(os.path.join(root, name))
                    if (stat.st_dev, stat.st_ino) not in seen:
                        seen.add((stat.st_dev, stat.st_ino))
                        total += stat.st_size
    return total


# What a verified finding's bundle stops needing: the fix is in and its replay
# no longer reproduces, so the frames, logs and repeat sightings are moot.
# FINDING.md, meta.json and the replays stay -- a few KB that let a later
# sighting reopen it against the original record.
VERIFIED_PRUNED = ("evidence", "sightings", "verify")


# Where a severe finding's evidence goes when its bundle is pruned, and how
# much of it is kept (oldest first out). Deleting it outright lost the one rr
# trace that could say what produced a crash's bad state: the fix was right, the
# finding verified, and the question its cause left open had nothing left to
# walk back (e041d436ae59, 2026-11-21).
ARCHIVE = SOAK / "archive"
ARCHIVE_MAX_BYTES = int(float(os.environ.get("MICROIDE_SOAK_ARCHIVE_GB", "5")) * 1e9)


def bundle_tier(bundle: Path) -> int:
    """The impact tier of the finding a bundle holds (meta.json), paint when
    unreadable: an unreadable bundle's evidence is not worth keeping."""
    try:
        finding = json.loads((bundle / "meta.json").read_text()).get("finding", {})
    except (OSError, ValueError):
        return len(TIER_NAMES) - 1
    return tier_of(str(finding.get("kind", "")), str(finding.get("label", "")))


def trim_archive(limit: int = ARCHIVE_MAX_BYTES) -> int:
    """Removes the oldest archived bundles until the archive fits `limit`;
    returns the bytes freed."""
    if not ARCHIVE.is_dir():
        return 0
    held = sorted((p for p in ARCHIVE.iterdir() if p.is_dir()), key=lambda p: p.stat().st_mtime)
    sizes = {p: disk_usage(p) for p in held}
    total = sum(sizes.values())
    freed = 0
    for path in held:
        if total <= limit:
            break
        shutil.rmtree(path, ignore_errors=True)
        total -= sizes[path]
        freed += sizes[path]
    return freed


def prune_verified_bundle(bundle: Path) -> int:
    """Clears a verified or closed bundle's evidence; returns the bytes freed
    from the bundle. A severe finding's (a crash, a sanitizer report, data, undo,
    a leak) is MOVED to ARCHIVE/<sig>, its recording included, so a question the
    fix left open -- what produced the state it now survives -- can still be
    walked back; the archive keeps the newest ARCHIVE_MAX_BYTES of them."""
    freed = 0
    keep = bundle_tier(bundle) == 0
    for name in VERIFIED_PRUNED:
        path = bundle / name
        if not path.is_dir():
            continue
        size = disk_usage(path)
        freed += size
        if keep:
            target = ARCHIVE / bundle.name / name
            shutil.rmtree(target, ignore_errors=True)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(path), str(target))
            os.utime(target.parent)  # its age in the archive is the move's
        else:
            shutil.rmtree(path, ignore_errors=True)
    if keep and freed:
        trim_archive()
    return freed


# A run directory nothing has written to for this long belongs to no running
# job: the longest one (a check: three hours) ends well before it.
ORPHAN_RUN_SECONDS = 4 * 3600.0


def sweep_orphan_runs(running: set[Path], log, now: float | None = None) -> int:
    """Removes the run directories under RUNS that no job owns; returns how many.

    A run's directory goes when its job is reaped, and three things left one
    behind for good -- 219 of them, a week into the soak, in a directory `health`
    never looked at:
    - the runner stopped (a restart, a build of its own unit) with jobs running:
      they die with it and nothing reaps them;
    - a later run wrote into a directory already deleted. A replayed corpus
      prefix named the files of the run that saved it by their absolute paths,
      and the app saved the diff's editable side there, parents and all (110 of
      the 219 held nothing but a `qa-diff/v2.right.js`, written hours after
      their run);
    - a check, hunt or verification whose runner died under it.
    `running` holds the directories of the jobs this runner has now; the age
    keeps a hunt or a check somebody runs by hand (`soak.py hunt SIG`)."""
    now = time.time() if now is None else now
    removed = 0
    try:
        entries = sorted(RUNS.iterdir())
    except OSError:
        return 0
    for entry in entries:
        if entry in running or not entry.is_dir() or entry.is_symlink():
            continue
        try:
            newest = max([entry.stat().st_mtime] + [child.lstat().st_mtime for child in entry.iterdir()])
        except OSError:
            continue  # going as we look
        if now - newest < ORPHAN_RUN_SECONDS:
            continue
        shutil.rmtree(entry, ignore_errors=True)
        removed += 1
    if removed:
        log(f"gc: {removed} run director{'y' if removed == 1 else 'ies'} no job owns")
    return removed


# A minimize or check killed mid-run leaves its scratch (minimize.py removes it
# in a finally a SIGKILL skips); none runs longer than an hour and ten.
STALE_SCRATCH_SECONDS = 6 * 3600
# A finding with a fix booked, verified or closed is replayed from its actions;
# its rr traces are not read again.
RECORDING_DONE_STATUSES = ("fixed", "verified", "closed")


def drop_stale_scratch() -> int:
    freed = 0
    for scratch in FINDINGS.glob("*/verify/minimize-*"):
        with contextlib.suppress(OSError):
            if time.time() - scratch.stat().st_mtime > STALE_SCRATCH_SECONDS:
                freed += disk_usage(scratch)
                shutil.rmtree(scratch, ignore_errors=True)
    return freed


def bundle_recordings(bundle: Path) -> list[Path]:
    """A bundle's rr traces, newest first (the first sighting's under
    evidence/, later ones under sightings/<date>-.../)."""
    found = [p for p in (bundle / "evidence" / RECORDING, *bundle.glob(f"sightings/*/{RECORDING}")) if p.is_dir()]
    return sorted(found, key=lambda p: p.stat().st_mtime, reverse=True)


def drop_recordings(entries: dict[str, Any], *, open_beyond_newest: bool) -> int:
    """Drops the traces of findings whose fix is booked, verified or closed, and
    with `open_beyond_newest` every open finding's traces but its newest."""
    freed = 0
    for sig, entry in entries.items():
        traces = bundle_recordings(FINDINGS / sig)
        if entry.status not in RECORDING_DONE_STATUSES:
            if not open_beyond_newest:
                continue
            traces = traces[1:]
        for trace in traces:
            freed += disk_usage(trace)
            shutil.rmtree(trace, ignore_errors=True)
    return freed


def collect_garbage(log) -> None:
    """Drops what nothing reads any more: verified bundles' evidence, the traces
    of findings with a fix booked or closed, a killed minimize's scratch, and the
    corpus prefixes too long for a run to start from."""
    freed = 0
    entries = ledger().entries()
    for sig, entry in entries.items():
        if entry.status == "verified":
            freed += prune_verified_bundle(FINDINGS / sig)
    freed += drop_recordings(entries, open_beyond_newest=False)
    freed += drop_stale_scratch()
    freed += trim_archive()
    removed, corpus_freed = mm.Corpus(CORPUS).prune()
    if freed or removed:
        log(f"gc: {freed / 1e6:.0f} MB of evidence, traces and scratch, {removed} unusable corpus prefixes "
            f"({corpus_freed / 1e6:.0f} MB)")


def enforce_disk_budget(log) -> None:
    budget = DISK_BUDGET_GB * 1e9
    if disk_usage(SOAK) <= budget:
        return
    # Older traces of open findings first (the newest of each stays), then the
    # oldest verified bundles, then the corpus's longest prefixes.
    entries = ledger().entries()
    if drop_recordings(entries, open_beyond_newest=True) and disk_usage(SOAK) <= budget * 0.9:
        log("disk budget: dropped open findings' older traces")
        return
    verified = sorted((FINDINGS / sig for sig, e in entries.items() if e.status == "verified"),
                      key=lambda p: p.stat().st_mtime if p.exists() else 0)
    for path in verified:
        prune_verified_bundle(path)
        if disk_usage(SOAK) <= budget * 0.9:
            return
    corpus = sorted(CORPUS.glob("*.json"), key=lambda p: p.stat().st_size, reverse=True)
    for path in corpus[: len(corpus) // 4]:
        path.unlink(missing_ok=True)
    log("disk budget: pruned verified evidence and a quarter of the corpus")
    if disk_usage(SOAK) > budget and (dropped := drop_newest_recordings(entries, budget * 0.9)):
        log(f"disk budget: dropped {dropped} open findings' last traces, the least recently seen first "
            "(each still replays; `soak.py hunt` records it again)")


def drop_newest_recordings(entries: dict[str, Any], target: float) -> int:
    """Drops open findings' remaining traces, the oldest first, until the soak is
    within `target` bytes; returns how many it dropped."""
    traces = sorted((trace for sig in entries for trace in bundle_recordings(FINDINGS / sig)),
                    key=lambda p: p.stat().st_mtime)
    dropped = 0
    for trace in traces:
        if disk_usage(SOAK) <= target:
            break
        shutil.rmtree(trace, ignore_errors=True)
        dropped += 1
    return dropped


def runner_code() -> str:
    """A digest of the runner's own sources as they are on disk now (tools/soak's
    modules, and tools/qa's findings and model it imports)."""
    import hashlib
    digest = hashlib.sha1()
    for path in sorted([*HERE.glob("*.py"), REPO / "tools" / "qa" / "findings.py",
                        REPO / "tools" / "qa" / "monkey_model.py", REPO / "tools" / "qa" / "recording.py"]):
        if path.name.startswith("test_"):
            continue
        with contextlib.suppress(OSError):
            digest.update(path.name.encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()[:16]


def stale_runner(state: dict[str, Any]) -> bool:
    """The runner is running sources older than the checkout's. Jobs run each
    build's own driver snapshot, so a change to the driver is live with the next
    build -- but the runner is one long-lived process, and a change to it (how
    findings are booked, checked, verified, swept) does nothing until the
    service is restarted. Nothing said so: `health` does now."""
    started_with = state.get("runner_code")
    return bool(started_with) and started_with != runner_code()


def load_state() -> dict[str, Any]:
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {}


def save_state(state: dict[str, Any]) -> None:
    STATE.parent.mkdir(parents=True, exist_ok=True)
    temp = STATE.with_suffix(".tmp")
    temp.write_text(json.dumps(state, indent=1))
    os.replace(temp, STATE)


def cmd_run(args: argparse.Namespace) -> int:
    SOAK.mkdir(parents=True, exist_ok=True)
    rng = random.Random()
    state = load_state()
    state["started"] = time.time()
    state["runner_code"] = runner_code()
    log_file = open(SOAK / "soak.log", "a")

    def log(message: str) -> None:
        line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {message}"
        print(line, flush=True)
        log_file.write(line + "\n")
        log_file.flush()

    cores = os.cpu_count() or 4
    ceiling = args.jobs or max_jobs(cores)

    job_load = JobLoad()

    def our_cores() -> float | None:
        return job_load.sample({job.proc.pid for job in jobs})

    def current_cap(our_jobs: int) -> int:
        if args.fixed:
            return ceiling
        ours = our_cores()
        # allowed_jobs takes the machine's demand and subtracts ours: hand it ours
        # plus what others measurably use, not the trailing load average.
        demand = (ours or 0.0) + job_load.others(os.getloadavg()[0])
        by_cores = allowed_jobs(demand, ours, our_jobs, cores, ceiling)
        available, total = memory_available()
        return min(by_cores, memory_jobs(available, total, job_load.rss, our_jobs))
    jobs: list[Job] = []
    stopping = False

    def stop(*unused: Any) -> None:
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    last_build = 0.0
    building: BackgroundBuild | None = None
    last_housekeeping = 0.0
    deadline = time.time() + args.max_hours * 3600 if args.max_hours else None
    mode = "fixed" if args.fixed else "following the load"
    log(f"soak runner up: up to {ceiling} job(s) at once, {mode} -> {SOAK}")
    unusable = recording.rr_unusable_reason()
    log(f"recording: {'OFF -- ' + unusable if unusable else f'{RECORD_FRACTION:.0%} of the default-build jobs run under rr'}")
    last_cap = -1
    last_start = 0.0
    idle: dict[str, Any] = {}
    last_containment = 0.0
    last_hunt = 0.0
    while not stopping:
        now = time.time()
        if deadline and now > deadline:
            break
        if now - last_containment > CONTAINMENT_INTERVAL and not PAUSE.exists():
            last_containment = now
            breach = containment_breach()
            if breach is not None:
                contain_breach(breach, jobs, log)
                for job in jobs:
                    job.proc.wait()
                jobs.clear()
                state["current"] = []
                save_state(state)
                continue
        # A build runs at most once per interval counted from the END of the last
        # one (stamped at its start, a 20-minute build on a moving main left the
        # interval spent and the runner rebuilt forever, starting no job). It runs
        # beside the jobs: they run the binaries and the driver snapshot of their
        # own build (snapshot_driver), so nothing they read moves under them. It
        # used to wait for every running job to end first -- a drain that left the
        # soak at zero jobs 4.5 hours a day, waiting on hour-long ASan jobs for a
        # build that took seconds.
        if building is not None and building.done():
            error = building.finish(state)
            building = None
            if error is not None:
                log(f"build step failed: {error}")
            verify_fixed(state, log)
            recheck_open(state, log)
            save_state(state)
            last_build = time.time()
        if building is None and now - last_build > args.build_interval and not build_paused():
            if not state.get("good") or driver_root(state) == WORKTREE or refresh_due(state):
                building = BackgroundBuild(state, log)
            else:
                last_build = time.time()
        if building is not None and (not state.get("good") or driver_root(state) == WORKTREE):
            time.sleep(1.0)
            continue
        for job in list(jobs):
            if job.proc.poll() is not None or now - job.started > job.type.timeout:
                timed_out = job.proc.poll() is None
                if timed_out:
                    stop_timed_out(job)
                    log(f"job {job.type.name} seed {job.seed} hit its timeout")
                jobs.remove(job)
                reap_job(job, state, log, timed_out)
                state["current"] = [j.cell for j in jobs]
                save_state(state)
        pending = state.get("pending_checks", [])
        cap = current_cap(slots(jobs))
        if cap != last_cap:
            log(f"job budget {cap} (load {os.getloadavg()[0]:.1f}, {memory_available()[0] / 2**30:.0f} GB available, "
                f"{len(jobs)} running, {slots(jobs)} slots)")
            last_cap = cap
        if pending and replay_room(jobs, cap) and busy_reason(our_cores() or CORES_PER_JOB * slots(jobs), job_load.others(os.getloadavg()[0])) is None:
            # Checks first, up to their share (replay_room): a new finding's rate
            # and minimal replay are worth more than another fresh run. A check
            # holds three displays, so it counts as three jobs.
            sig = next_check(pending, ledger().entries(), int(state.get("checks_started", 0)))
            state["checks_started"] = int(state.get("checks_started", 0)) + 1
            pending.remove(sig)
            jobs.append(start_check(sig, state))
            log(f"check {sig}")
            save_state(state)
        verifies = state.get("pending_verifies", [])
        if verifies and replay_room(jobs, cap) and busy_reason(our_cores() or CORES_PER_JOB * slots(jobs), job_load.others(os.getloadavg()[0])) is None:
            # A fix's verification next, as a job of its own like a check (three
            # displays): it used to run inline here and hold every start behind it.
            sig = verifies.pop(0)
            jobs.append(start_verify(sig, state))
            log(f"verify {sig}")
            state["current"] = [j.cell for j in jobs]
            save_state(state)
        if not any(job.type is HUNT_JOB for job in jobs) and slots(jobs) < cap and not PAUSE.exists() \
                and now - last_hunt >= HUNT_INTERVAL and recording.rr_unusable_reason() is None:
            # One hunt at a time, for the finding most worth a recording.
            last_hunt = now
            candidates: list[ld.Entry] = []
            with contextlib.suppress(Exception):  # the ledger's bookkeeping never stops the runner
                candidates = hunt_candidates(ledger().entries().values())
            if candidates:
                jobs.append(start_hunt(candidates[0].sig, state))
                log(f"hunt {candidates[0].sig} ({len(candidates)} finding(s) without a recording)")
                state["current"] = [j.cell for j in jobs]
                save_state(state)
        reason = busy_reason(our_cores() or CORES_PER_JOB * slots(jobs), job_load.others(os.getloadavg()[0]))
        if (note := idle_note(idle, reason, now)) is not None:
            log(note)
        if reason is None:
            # One per RAMP_SECONDS: the load average needs time to show what the
            # last one costs before the budget is trusted for the next.
            if slots(jobs) < cap and (args.fixed or now - last_start >= RAMP_SECONDS):
                job_type = choose_job(rng, state)
                if job_type is not None:
                    job = start_job(job_type, state, rng)
                    jobs.append(job)
                    last_start = now
                    log(f"start {job.cell} seed {job.seed}")
        elif not jobs:
            state["waiting"] = reason
        state["current"] = [j.cell for j in jobs]
        save_state(state)
        if args.once and not jobs and not state.get("pending_checks") and not state.get("pending_verifies"):
            break
        if now - last_housekeeping > 600:
            collect_garbage(log)
            sweep_orphan_runs({job.out for job in jobs}, log)
            enforce_recording_budget(log)
            enforce_disk_budget(log)
            with contextlib.suppress(Exception):  # the ledger's bookkeeping never stops the runner
                verify_by_exposure(log)
                close_quiet(log)
            with contextlib.suppress(Exception):
                requeue_unmeasured(state, log)
            if now - state.get("code_report_at", 0) > CODE_REPORT_SECONDS and not PAUSE.exists():
                with contextlib.suppress(Exception):
                    refresh_code_report(state, log)
            if now - state.get("surface_at", 0) > SURFACE_RECOUNT_SECONDS and not PAUSE.exists():
                with contextlib.suppress(Exception):
                    # A runner that came up on a commit it had already built has
                    # no inventory yet: take it now, not at the next push.
                    refresh_surface(state, log, inventory=not (SOAK / "surface.json").exists())
            last_housekeeping = now
        time.sleep(3)
    for job in jobs:
        if job.proc.poll() is None:
            os.killpg(job.proc.pid, signal.SIGTERM)
    log("soak runner down")
    return 0


# ---- reporting ---------------------------------------------------------------------

def run_rows(hours: float | None = None) -> list[dict[str, Any]]:
    rows = []
    if RUN_LOG.exists():
        cutoff = time.time() - hours * 3600 if hours else 0
        for line in RUN_LOG.read_text().splitlines():
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("t", 0) >= cutoff:
                rows.append(row)
    return rows


def exposure_progress(entry: ld.Entry) -> str:
    """How far a fixed finding is from verified by exposure."""
    quiet = fix_exposure(entry)
    if quiet is None:
        return "no rate to measure against"
    return f"{quiet.expected:.1f} of {ex.VERIFIED_EXPECTED} due ({quiet.runs} runs)"


def open_families(entries, paint: bool = False) -> list[tuple[str, list[ld.Entry]]]:
    """The open, still-showing severe and behaviour findings, or with `paint`
    the stale-paint ones, grouped by family (family), most sightings first."""
    groups: dict[str, list[ld.Entry]] = {}
    for entry in entries:
        wanted = impact_tier(entry) == 3 if paint else impact_tier(entry) <= 1
        if entry.status in ("new", "reopened") and wanted and not is_quiet(entry):
            groups.setdefault(family(entry.kind, entry.label), []).append(entry)
    return sorted(groups.items(), key=lambda item: -sum(e.count for e in item[1]))


def cmd_health(args: argparse.Namespace) -> int:
    """Whether the soak is doing what it says: every cell taking steps, the
    recorder on, and what each of the ledger's verdicts rests on."""
    state = load_state()
    hours = args.hours
    rows = run_rows(hours)
    print(f"cells, last {hours:.0f} h (a job type with no steps finds nothing and reports nothing):")
    by_job: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_job.setdefault(row["job"], []).append(row)
    problems = 0
    for job in JOB_TYPES:
        if job.build not in BUILDS:
            continue  # a private soak that does not make this build (MICROIDE_SOAK_BUILDS)
        items = by_job.get(job.name, [])
        dead = sum(1 for r in items if r.get("steps", 0) <= 0)
        steps = sum(r.get("steps", 0) for r in items) / max(1, len(items) - dead) if items else 0
        recorded = sum(1 for r in items if "+rr" in r.get("cell", ""))
        verdict = "ok"
        if job.build not in state.get("good", {}):
            verdict = f"NO BUILD ({job.build})"
        elif not items:
            verdict = "did not run"
        elif dead * 2 > len(items):
            verdict = "DEAD: most runs took no step"
        elif dead:
            verdict = f"{dead} dead run(s)"
        elif steps < job.steps * 0.5:
            verdict = f"short runs (asked {job.steps} steps)"
        problems += verdict not in ("ok", "did not run")
        quiet = state.get("stats", {}).get(job.name, {}).get("quiet_runs", 0)
        if verdict == "ok" and quiet >= PLATEAU_RUNS:
            verdict = f"plateau: {quiet} runs with nothing new (weight cut)"
        print(f"  {job.name:14} {len(items):5} runs  {steps:6.0f} steps/run  {recorded:4} recorded  {verdict}")
    if stale_runner(state):
        print("runner: STALE -- tools/soak has changed since the runner started and it still runs the old code; "
              "restart it between jobs: `soak.py pause --wait 3600 && systemctl --user restart microide-soak && "
              "soak.py resume`")
        problems += 1
    orphans = sum(1 for entry in RUNS.iterdir() if entry.is_dir()) - len(state.get("current", [])) \
        if RUNS.is_dir() else 0
    if orphans > 2 * max_jobs(os.cpu_count() or 4):
        print(f"runs: {orphans} run directories no running job owns under {RUNS} (housekeeping sweeps them "
              f"after {ORPHAN_RUN_SECONDS / 3600:.0f} h; this many means it is not running)")
        problems += 1
    unusable = recording.rr_unusable_reason()
    lost = [r["recorder_lost"] for r in rows if r.get("recorder_lost")]
    print(f"recording: {'OFF -- ' + unusable if unusable else 'rr usable'}; "
          f"{sum(1 for r in rows if '+rr' in r.get('cell', ''))} of {len(rows)} runs recorded"
          + (f"; {len(lost)} lost their recorder, e.g. {max(set(lost), key=lost.count)[-140:]}" if lost else ""))
    problems += unusable is not None
    print(code_line(state))
    if (line := surface_line()) is not None:
        print(line)
    else:
        print("surface: NOT COUNTED yet (tools/soak/surface.py runs with each build)")
    counted = {key: sum(int(r.get(key) or 0) for r in rows)
               for key in ("invariants", "starved", "kills_hit", "kills_missed")}
    print(f"oracles: {counted['invariants']} invariant checks answered; "
          f"{counted['kills_hit']} kills at a named crash point ({counted['kills_missed']} more passed none); "
          f"{counted['starved']} slow answers put down to a starved machine and not filed")
    # Since the driver that counts them: older rows have none, which is not a fault.
    counting = [r for r in rows if "invariants" in r or "kills_hit" in r]
    if rows and counting and not counted["invariants"]:
        print("  NO INVARIANT CHECK was answered: the app's `invariants` query is not reaching the runs")
        problems += 1
    entries = ledger().entries()
    open_entries = [e for e in entries.values() if e.status in ("new", "reopened")]
    print("open findings (the soak still shows / quiet / with a recording):")
    for tier, name in enumerate(TIER_NAMES):
        items = [e for e in open_entries if impact_tier(e) == tier]
        quiet = sum(is_quiet(e) for e in items)
        zero = sum(1 for e in items if not is_quiet(e) and e.rate == 0)
        print(f"  {name:10} {len(items) - quiet:5} / {quiet:5} / {sum(has_recording(FINDINGS / e.sig) for e in items):3}"
              f"   ({zero} of the active ones no replay reproduces)")
    # Why not, which is two different jobs: a replay that left the run's path is
    # the harness's to fix (it makes every such finding look like a race); one
    # that followed it needs a recording.
    unshown = [e for e in open_entries if not is_quiet(e) and e.rate == 0]
    verdicts: dict[str, int] = {}
    for entry in unshown:
        verdicts[entry.replay_verdict] = verdicts.get(entry.replay_verdict, 0) + 1
    if unshown:
        print(f"no replay reproduces {len(unshown)} of them: "
              f"{verdicts.get('diverged', 0)} whose replays LEFT the run's path (a harness gap: tools/qa), "
              f"{verdicts.get('followed', 0)} whose replays followed it (timing: needs a recording or a hunt), "
              f"{verdicts.get('untested', 0)} the driver could not replay at all, "
              f"{verdicts.get('unknown', 0)} checked before replays compared their path")
    verified = [e for e in entries.values() if e.status == "verified"]
    by_replay = [e for e in verified if e.verified_by != "exposure" and e.replay_reproduces]
    by_exposure = [e for e in verified if e.verified_by == "exposure"]
    hollow = [e for e in verified if e.verified_by != "exposure" and not e.replay_reproduces]
    confirmed = 0
    for entry in hollow:
        quiet = fix_exposure(entry)
        confirmed += quiet is not None and quiet.expected >= ex.VERIFIED_EXPECTED
    print(f"verified: {len(verified)} -- {len(by_replay)} by a replay that used to reproduce, "
          f"{len(by_exposure)} by exposure, {len(hollow)} on a replay that never reproduced "
          f"({confirmed} of those the soak has since borne out, {len(hollow) - confirmed} not yet)")
    waiting = [e for e in entries.values() if e.status == "fixed" and not e.replay_reproduces]
    print(f"fixed, waiting for exposure: {len(waiting)}")
    return 1 if problems else 0


def cmd_status(args: argparse.Namespace) -> int:
    state = load_state()
    entries = ledger().entries()
    started = state.get("started")
    rows = run_rows(24)
    print(f"uptime: {(time.time() - started) / 3600:.1f} h" if started else "uptime: not running")
    print(f"build: {state.get('built_sha', '?')[:12]}  good: "
          + ", ".join(f"{k}@{v['sha'][:8]}" for k, v in state.get("good", {}).items()))
    hours = max(1e-6, (time.time() - min((r["t"] for r in rows), default=time.time())) / 3600)
    print(f"last 24 h: {len(rows)} jobs ({len(rows) / hours:.1f}/h), {sum(r.get('steps', 0) for r in rows)} steps, "
          f"{sum(r.get('new', 0) for r in rows)} new findings")
    by_status: dict[str, list[ld.Entry]] = {}
    for entry in entries.values():
        by_status.setdefault(entry.status, []).append(entry)
    for status in ld.STATES:
        items = by_status.get(status, [])
        quiet = sum(is_quiet(entry) for entry in items)
        print(f"{status:9} {len(items)}" + (f"  ({len(items) - quiet} the soak still shows, {quiet} quiet)" if quiet else ""))
        for entry in sorted(items, key=priority_key)[:8] if status in ("new", "reopened", "claimed") else []:
            if is_quiet(entry):
                break  # the quiet ones sort last: `report` lists them
            det = "det" if entry.deterministic else ("flaky" if entry.deterministic is False else "?")
            mark = " [rr]" if has_recording(FINDINGS / entry.sig) else ""
            print(f"    {entry.sig} x{entry.count:<4} {det:5} {priority_tag(entry):15} {entry.label[:90]}{mark}")
    waiting = [e for e in by_status.get("fixed", []) if not e.replay_reproduces]
    if waiting:
        print(f"fixed, waiting for exposure (no replay reproduces them): {len(waiting)}")
        for entry in sorted(waiting, key=priority_key)[:8]:
            print(f"    {entry.sig} {exposure_progress(entry):28} {entry.label[:80]}")
    for heading, families, shown in (
            ("open families the soak still shows (severe and behaviour; one defect is often many signatures):",
             open_families(entries.values()), 10),
            ("open stale-paint families:", open_families(entries.values(), paint=True), 5)):
        if families:
            print(heading)
        for name, members in families[:shown]:
            recorded = sum(has_recording(FINDINGS / e.sig) for e in members)
            print(f"    x{sum(e.count for e in members):<5} {len(members):3} sig(s) {name[:92]}"
                  + (f" [rr x{recorded}]" if recorded else ""))
    print(f"current: {', '.join(state.get('current', [])) or state.get('waiting', 'idle')}")
    if PAUSE.exists():
        print("PAUSED (remove build/soak/PAUSE)")
    return 0


def cmd_report(args: argparse.Namespace) -> int:
    entries = ledger().entries()
    rows = run_rows(24 * 7)
    lines = ["# Soak report", "", f"Generated {time.strftime('%Y-%m-%d %H:%M')}.", ""]

    def section(title: str, items: list[ld.Entry]) -> None:
        lines.extend([f"## {title} ({len(items)})", ""])
        for entry in sorted(items, key=priority_key):
            lines.append(f"- `{entry.sig}` **{priority_tag(entry)}** x{entry.count} {entry.label} -- "
                         f"[FINDING.md]({FINDINGS / entry.sig / 'FINDING.md'})"
                         + (f", minimal {entry.minimal_actions} actions" if entry.minimal_actions else ""))
        lines.append("")
    every_open = [e for e in entries.values() if e.status in ("new", "reopened")]
    open_entries = [e for e in every_open if not is_quiet(e)]
    lines.extend(["## Families the soak still shows (severe and behaviour)", "",
                  "One defect is often many signatures (a file name, a budget note); fix the family.", ""])
    for name, members in open_families(entries.values()):
        recorded = [e.sig for e in members if has_recording(FINDINGS / e.sig)]
        lines.append(f"- x{sum(e.count for e in members)} in {len(members)} signature(s): {name}"
                     + (f" -- recorded (rr): {', '.join(f'`{sig}`' for sig in recorded)}" if recorded else "")
                     + f" -- e.g. `{max(members, key=lambda e: e.last_seen).sig}`")
    lines.append("")
    section("New, deterministic", [e for e in open_entries if e.deterministic])
    section("New, flaky", [e for e in open_entries if e.deterministic is False])
    section("New, not yet checked", [e for e in open_entries if e.deterministic is None])
    section("Claimed", [e for e in entries.values() if e.status == "claimed"])
    section("Fixed, awaiting verification", [e for e in entries.values() if e.status == "fixed"])
    section("Quiet: open, but the soak stopped showing them (probably gone; a sighting brings one back)",
            [e for e in every_open if is_quiet(e)])
    lines.extend(["## Yield (last 7 days)", "", "| job | runs | hours | steps | new findings | per hour |",
                  "|---|---|---|---|---|---|"])
    by_job: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_job.setdefault(row["job"], []).append(row)
    for job, items in sorted(by_job.items()):
        hours = sum(r.get("seconds", 0) for r in items) / 3600
        new = sum(r.get("new", 0) for r in items)
        lines.append(f"| {job} | {len(items)} | {hours:.1f} | {sum(r.get('steps', 0) for r in items)} | {new} | "
                     f"{new / max(hours, 1e-6):.2f} |")
    coverage = {}
    if COVERAGE.exists():
        coverage = json.loads(COVERAGE.read_text())
    lines.extend(["", "## Coverage", "", f"- states: {len(coverage.get('states', {}))}",
                  f"- transitions: {len(coverage.get('transitions', {}))}",
                  f"- state x action pairs: {len(coverage.get('pairs', {}))}",
                  f"- commands run: {len(coverage.get('commands', {}))}",
                  f"- elements pressed: {len(coverage.get('elements', {}))}",
                  f"- setting values set: {len(coverage.get('settings', {}))}",
                  f"- corpus prefixes: {len(list(CORPUS.glob('*.json')))}", ""])
    lines.extend(command_gaps(coverage.get("commands", {})))
    parts = {name: disk_usage(SOAK / name) / 1e9 for name in ("bin", "corpus", "findings", "runs")}
    lines.extend(["## Disk", "", f"- {sum(parts.values()):.1f} GB of the {DISK_BUDGET_GB:.0f} GB budget: "
                  + ", ".join(f"{name} {size:.1f}" for name, size in parts.items()),
                  "- verified findings keep FINDING.md and their replays; `soak.py gc` drops the rest", ""])
    surface = SOAK / "surface-gaps.md"
    if surface.exists():
        lines.extend(["## Surface gaps", "", f"See [{surface.name}]({surface}).", ""])
    (SOAK / "report.md").write_text("\n".join(lines))
    print(SOAK / "report.md")
    return 0


# Commands the monkey does not run on purpose: control-channel primitives a
# driver uses to steer (`key`, `pointer`) and the one that ends the run.
UNSOUGHT_COMMANDS = frozenset({"key", "pointer", "quit"})
RARE_COMMAND_RUNS = 20


def registered_commands(source: Path | None = None) -> list[str]:
    """The app's command names, from its command registry's source (the soak
    keeps no running app to ask)."""
    path = source or WORKTREE / "src" / "workspace" / "registries" / "WorkspaceCommandRegistry.cpp"
    if not path.exists():
        path = REPO / "src" / "workspace" / "registries" / "WorkspaceCommandRegistry.cpp"
    text = path.read_text() if path.exists() else ""
    return sorted(set(re.findall(r'ActionSpec\{ActionId::\w+,\s*"([^"]+)"', text)))


def command_gaps(ran: dict[str, int], registered: list[str] | None = None) -> list[str]:
    """The report's list of commands the soak has never run, or barely: a whole
    area with no soak behind it (the debugger, remote projects) shows up here
    before anyone has to go looking for it.

    >>> command_gaps({"open": 900, "goto": 3}, ["open", "goto", "debug-run", "quit"])[:5]
    ['## Command gaps', '', '- never run (1): `debug-run`', '- run fewer than 20 times (1): `goto` 3', '']
    """
    names = [n for n in (registered if registered is not None else registered_commands())
             if n not in UNSOUGHT_COMMANDS]
    never = [n for n in names if not ran.get(n)]
    rare = sorted(((n, ran[n]) for n in names if 0 < ran.get(n, 0) < RARE_COMMAND_RUNS), key=lambda x: x[1])
    return ["## Command gaps", "",
            f"- never run ({len(never)}): " + (", ".join(f"`{n}`" for n in never) or "none"),
            f"- run fewer than {RARE_COMMAND_RUNS} times ({len(rare)}): "
            + (", ".join(f"`{n}` {c}" for n, c in rare) or "none"), ""]


def family_members(entries: dict[str, ld.Entry], sig: str) -> list[str]:
    """`sig` and every other still-open signature of its family (family)."""
    chosen = entries[sig]
    name = family(chosen.kind, chosen.label)
    return [sig] + sorted(other for other, e in entries.items() if other != sig
                          and e.status in ("new", "reopened", "claimed") and family(e.kind, e.label) == name)


def cmd_ledger(args: argparse.Namespace) -> int:
    book = ledger()
    entries = book.entries()
    if args.action == "list":
        for entry in sorted(entries.values(), key=lambda e: (e.status, -e.count)):
            print(f"{entry.sig} {entry.status:9} x{entry.count:<4} {entry.label[:100]}")
        return 0
    if not args.sig or args.sig not in entries:
        print(f"no such signature {args.sig!r}", file=sys.stderr)
        return 2
    if args.action == "show":
        print(json.dumps(entries[args.sig].to_json(), indent=1))
        md = FINDINGS / args.sig / "FINDING.md"
        if md.exists():
            print(md.read_text())
        return 0
    targets = family_members(entries, args.sig) if args.family else [args.sig]
    if len(targets) > 1:
        print(f"{args.action}: {len(targets)} signatures of {family(entries[args.sig].kind, entries[args.sig].label)}")
    if args.action == "claim":
        for sig in targets:
            book.append(sig, "claimed", by=args.extra or os.environ.get("USER", "agent"))
    elif args.action == "fixed":
        if not args.extra:
            print("fixed needs the fix commit", file=sys.stderr)
            return 2
        commit = git("rev-parse", args.extra)
        for sig in targets:
            book.append(sig, "fixed", commit=commit)
    elif args.action == "reopen":
        book.append(args.sig, "reopened", note=args.extra or "")
    elif args.action == "close":
        # By hand: a harness fault, or a finding the soak can no longer reach.
        if not args.extra:
            print("close needs the reason", file=sys.stderr)
            return 2
        for sig in targets:
            book.append(sig, "closed", by=os.environ.get("USER", "agent"), note=args.extra)
    elif args.action == "note":
        book.append(args.sig, "note", text=args.extra or "")
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    check_finding(args.sig, load_state(), print)
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    verify_one(args.sig, load_state(), print)
    return 0


def cmd_coverage(args: argparse.Namespace) -> int:
    """The source coverage report, refreshed; or the uncalled functions of some sources;
    or, with --reset, the merged profile started over (the runs it held are dropped:
    a deleted function lingers in it otherwise, and an old commit's counts stay)."""
    state = load_state()
    if args.reset:
        CODE_PROFILE.unlink(missing_ok=True)
        state["code_reported_runs"] = None
        save_state(state)
        print(f"removed {CODE_PROFILE}; the next coverage job starts the profile afresh")
        return 0
    good = state.get("good", {}).get("coverage")
    if not good or not CODE_PROFILE.is_file():
        print(code_line(state))
        return 1
    binary = Path(good["binary"])
    server = binary.parent / "microide-server"
    objects = [server] if server.exists() else []
    if args.functions:
        sources = [str((WORKTREE / source).resolve()) for source in args.functions]
        for source, function in codecov.uncalled_functions(binary, objects, CODE_PROFILE, sources):
            print(f"{source}\t{function}")
        return 0
    state["code_reported_runs"] = None
    refresh_code_report(state, print)
    save_state(state)
    print(SOAK / "code-coverage.md")
    return 0


def cmd_hunt(args: argparse.Namespace) -> int:
    if args.sig is None:
        for entry in hunt_candidates(ledger().entries().values()):
            print(f"{entry.sig} {priority_tag(entry):10} x{entry.count:<4} hunts {hunts_made(entry)}  {entry.label[:100]}")
        return 0
    return 0 if hunt_finding(args.sig, load_state(), print, runs=args.runs, mode=args.rr) else 1


def cmd_build(args: argparse.Namespace) -> int:
    state = load_state()
    build_all(state, print)
    save_state(state)
    return 0


def cmd_pause(args: argparse.Namespace) -> int:
    if not pause_for_phase(args.reason):
        print(f"already paused, not for a phase -- left as it is: {PAUSE.read_text(errors='replace').strip()}")
        return 1
    print(f"paused for: {args.reason}")
    if args.wait > 0:
        if not wait_for_idle(args.wait):
            print(f"still running after {args.wait:.0f} s: {', '.join(load_state().get('current', []))}")
            return 2
        print("idle: no job running")
    return 0


def cmd_resume(_args) -> int:
    if not resume_from_phase():
        print(f"paused for something other than a phase -- not lifted: {PAUSE.read_text(errors='replace').strip()}")
        return 1
    print("resumed")
    return 0


def cmd_gc(_args) -> int:
    collect_garbage(print)
    return 0

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    run = sub.add_parser("run")
    run.add_argument("--jobs", type=int, default=0,
                     help="the most jobs at once (default: one per core; the measured cost per job and the memory then decide how many fit)")
    run.add_argument("--fixed", action="store_true",
                     help="always run --jobs jobs instead of following the machine's load")
    run.add_argument("--once", action="store_true", help="exit when the started jobs and checks are done")
    run.add_argument("--max-hours", type=float, default=0)
    run.add_argument("--build-interval", type=float, default=900, help="seconds between main checks")
    sub.add_parser("status")
    health = sub.add_parser("health", help="is every cell taking steps, is rr on, what the verdicts rest on")
    health.add_argument("--hours", type=float, default=24)
    sub.add_parser("report")
    sub.add_parser("build")
    sub.add_parser("gc", help="delete verified findings' evidence and unusable corpus prefixes")
    pause = sub.add_parser("pause", help="stop new jobs and builds for the fixing agent's phase")
    pause.add_argument("reason", nargs="?", default="fixing")
    pause.add_argument("--wait", type=float, default=0, help="seconds to wait for the running jobs to end")
    sub.add_parser("resume", help="lift a phase pause (never a containment one)")
    check = sub.add_parser("check", help="measure a finding's reproduction rate and minimize it")
    check.add_argument("sig")
    verify = sub.add_parser("verify", help="replay a fixed finding on the current build and book the verdict")
    verify.add_argument("sig")
    hunt = sub.add_parser("hunt", help="replay a finding paced under rr until it shows, and keep the trace; "
                                       "with no signature, list the findings worth one")
    hunt.add_argument("sig", nargs="?")
    hunt.add_argument("--runs", type=int, default=HUNT_RUNS)
    hunt.add_argument("--rr", default="chaos", choices=(*recording.RR_MODES, "off"),
                      help="rr mode (default chaos: randomized scheduling), or off")
    coverage = sub.add_parser("coverage", help="the soak's source coverage: refresh code-coverage.md, or list "
                                               "the functions of some sources no job ever called")
    coverage.add_argument("--functions", nargs="+", metavar="SOURCE", help="paths under the repository (src/...)")
    coverage.add_argument("--reset", action="store_true", help="drop the merged profile and start it over")
    led = sub.add_parser("ledger")
    led.add_argument("action", nargs="?", default="list", choices=("list", "show", "claim", "fixed", "reopen", "close", "note"))
    led.add_argument("sig", nargs="?")
    led.add_argument("extra", nargs="?")
    led.add_argument("--family", action="store_true",
                     help="claim, fixed, close: every open signature of SIG's family, not SIG alone")
    args = parser.parse_args()
    return {"run": cmd_run, "status": cmd_status, "health": cmd_health, "report": cmd_report, "ledger": cmd_ledger,
            "build": cmd_build, "check": cmd_check, "verify": cmd_verify, "gc": cmd_gc, "hunt": cmd_hunt, "coverage": cmd_coverage,
            "pause": cmd_pause, "resume": cmd_resume}[args.mode](args)



if __name__ == "__main__":
    sys.exit(main())
