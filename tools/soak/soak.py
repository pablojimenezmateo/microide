#!/usr/bin/env python3
"""The soak runner: the QA monkey on this PC, continuously (safety-plan W1.1).

    tools/soak/soak.py run [--jobs N] [--once] [--max-hours H]   the runner (a systemd --user service)
    tools/soak/soak.py status                                    uptime, rate, open findings, current job
    tools/soak/soak.py report                                    writes build/soak/report.md
    tools/soak/soak.py ledger [list|show SIG|claim SIG|fixed SIG COMMIT|reopen SIG|note SIG TEXT]
    tools/soak/soak.py build                                     update the worktree and build once

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
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools" / "qa"))
import ledger as ld  # noqa: E402
import findings as fnd  # noqa: E402
import monkey_model as mm  # noqa: E402

SOAK = Path(os.environ.get("MICROIDE_SOAK_DIR", REPO / "build" / "soak"))
WORKTREE = Path(os.environ.get("MICROIDE_SOAK_WORKTREE", REPO / "build" / "wt-soak"))
PAUSE = SOAK / "PAUSE"
STATE = SOAK / "state.json"
RUNS = SOAK / "runs"
FINDINGS = SOAK / "findings"
CORPUS = SOAK / "corpus"
COVERAGE = SOAK / "coverage.json"
RUN_LOG = SOAK / "runs.jsonl"
DISK_BUDGET_GB = float(os.environ.get("MICROIDE_SOAK_DISK_GB", "25"))


# ---- builds ----------------------------------------------------------------------

@dataclass
class Build:
    name: str
    directory: str           # relative to the worktree
    configure: list[str]     # arguments after `cmake`
    slow: float              # MQA_SLOW_FACTOR
    env: dict[str, str] = field(default_factory=dict)
    prefix: list[str] = field(default_factory=list)  # e.g. setarch -R for TSan

    def binary(self) -> Path:
        return WORKTREE / self.directory / "microide" / "microide"


BUILDS = {
    "default": Build("default", "build", ["-S", ".", "-B", "build"], 1.0),
    "release": Build("release", "build-release", ["-S", ".", "-B", "build-release",
                                                  "-DCMAKE_BUILD_TYPE=Release"], 1.0),
    "asan": Build("asan", "build/microide-asan", ["--preset", "microide-asan"], 3.0),
    "ubsan": Build("ubsan", "build/microide-ubsan", ["--preset", "microide-ubsan"], 2.0),
    "tsan": Build("tsan", "build/microide-tsan", ["--preset", "microide-tsan"], 5.0,
                  prefix=["setarch", "-R"]),
}


def git(*args: str, cwd: Path = REPO) -> str:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, env=env).stdout.strip()


def ensure_worktree() -> None:
    if not (WORKTREE / ".git").exists():
        WORKTREE.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "worktree", "add", "--detach", str(WORKTREE), "main"], cwd=REPO,
                       capture_output=True)


def build_all(state: dict[str, Any], log) -> None:
    """Moves the worktree to main's head and builds every configuration. A build
    that fails is recorded (and a ledger finding); its last good binary stays."""
    ensure_worktree()
    head = git("rev-parse", "main")
    if git("log", "-1", "--format=%ae", head) == QA_GIT_EMAIL:
        raise RuntimeError(f"main is at {head[:10]}, a QA monkey commit: not building it")
    if is_built(state, head):
        return
    log(f"building {head[:10]}")
    subprocess.run(["git", "checkout", "-q", "--detach", head], cwd=WORKTREE, capture_output=True)
    good = state.setdefault("good", {})
    cores = max(2, (os.cpu_count() or 4) // 2)
    for build in BUILDS.values():
        env = {**os.environ, "CCACHE_SLOPPINESS": "pch_defines,time_macros"}
        started = time.monotonic()
        configure = subprocess.run(["nice", "-n", "19", "cmake", *build.configure], cwd=WORKTREE,
                                   capture_output=True, text=True, env=env)
        result = configure if configure.returncode else subprocess.run(
            ["nice", "-n", "19", "ionice", "-c3", "cmake", "--build", build.directory, "--target", "microide",
             f"-j{cores}"], cwd=WORKTREE, capture_output=True, text=True, env=env)
        if result.returncode == 0 and build.binary().exists():
            # A copy per commit: jobs keep running the binary they started with.
            target = SOAK / "bin" / build.name / head[:12]
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(build.binary(), target / "microide")
            good[build.name] = {"sha": head, "binary": str(target / "microide")}
            log(f"  {build.name}: ok ({time.monotonic() - started:.0f}s)")
        else:
            tail = (result.stdout + result.stderr)[-4000:]
            log(f"  {build.name}: FAILED")
            signature = fnd.signature("build", f"{build.name} build failed")
            ledger().append(signature, "new" if signature not in ledger().entries() else "seen",
                            kind="build", label=f"build: the {build.name} build fails", commit=head,
                            cell=build.name, detail=tail)
    state["built_sha"] = head
    prune_binaries(good)


def is_built(state: dict[str, Any], head: str) -> bool:
    """Whether every configuration's binary for `head` is already in hand."""
    return state.get("built_sha") == head and all(BUILDS[name].binary().exists() for name in state.get("good", {}))


def refresh_due(state: dict[str, Any]) -> bool:
    """Whether the runner has to drain its jobs for a build and a verification.

    Draining is what the build interval used to buy unconditionally, every
    fifteen minutes: no new job started until the running ones ended, and the
    longest of them is a check minimizing a finding for up to three hours. The
    build it waited for was nearly always a no-op (main had not moved), so the
    soak spent most of its time on one job -- three on average of a budget of
    nine. A drain is now taken only when there is work behind it: main moved,
    a binary went missing, or a fixed finding waits for its replay.
    """
    return not is_built(state, git("rev-parse", "main")) or any(True for _ in fixes_to_verify(state))


def prune_binaries(good: dict[str, Any]) -> None:
    keep = {Path(entry["binary"]).parent for entry in good.values()}
    for build_dir in (SOAK / "bin").glob("*/*"):
        if build_dir not in keep and time.time() - build_dir.stat().st_mtime > 6 * 3600:
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


JOB_TYPES = [
    JobType("default", 26, "default"),
    JobType("editing", 12, "default", "editing"),
    JobType("layout", 10, "default", "layout"),
    JobType("world", 8, "default", "world"),
    JobType("diff", 6, "default", "diff"),
    JobType("tooling", 8, "default", "tooling"),
    JobType("terminal", 4, "default", "terminal"),
    JobType("settings", 4, "default", "settings"),
    JobType("commands", 4, "default", "commands", steps=600, project="repo", matrix=False),
    JobType("editor", 3, "default", "editor", steps=600, project="repo", matrix=False),
    JobType("release", 6, "release"),
    JobType("asan", 10, "asan", steps=600, timeout=3600),
    JobType("asan-editing", 4, "asan", "editing", steps=500, timeout=3600),
    JobType("ubsan", 5, "ubsan", steps=800, timeout=3600),
    JobType("tsan", 2, "tsan", steps=300, timeout=3600, matrix=False),
]
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


def choose_cell(rng: random.Random) -> tuple[tuple[int, int], str]:
    """A window size and ui.scale, drawn from SIZES x SCALES with tiny cells
    weighted down to TINY_CELL_WEIGHT."""
    cells = [(size, scale) for size in SIZES for scale in SCALES]
    weights = [TINY_CELL_WEIGHT if is_tiny(size, scale) else 1.0 for size, scale in cells]
    return rng.choices(cells, weights=weights)[0]


# ---- what to fix first ---------------------------------------------------------

# Impact tiers, most severe first. A stale frame or a squeezed layout is a defect,
# but a crash, a lost or corrupted buffer, a broken undo or a leak costs a user
# their work or their session; the sanitizers' log lines are memory and thread
# bugs whatever they look like on screen.
TIER_NAMES = ("severe", "behaviour", "layout", "paint")
_SEVERE_KINDS = frozenset({"crash", "data", "undo", "leak", "build", "containment"})
_BEHAVIOUR_KINDS = frozenset({"state", "focus", "expect", "idle", "latency", "liveness"})
_LAYOUT_KINDS = frozenset({"overlap", "bounds"})


def impact_tier(entry: ld.Entry) -> int:
    if entry.kind == "log":
        return 3 if "QA-STALE" in entry.label else 0
    if entry.kind in _SEVERE_KINDS:
        return 0
    if entry.kind in _BEHAVIOUR_KINDS:
        return 1
    if entry.kind in _LAYOUT_KINDS:
        return 2
    return 1


def tiny_only(entry: ld.Entry) -> bool:
    """Seen only in tiny cells (see TINY_LOGICAL)."""
    return bool(entry.cells) and all(cell_is_tiny(cell) for cell in entry.cells)


def priority_key(entry: ld.Entry) -> tuple:
    """Sort key, best first: impact tier; within a tier, a finding a real-size
    window shows before one only a tiny window does, then deterministic before
    unchecked before flaky, then the most sightings."""
    det = 0 if entry.deterministic else (2 if entry.deterministic is False else 1)
    return (impact_tier(entry), tiny_only(entry), det, -entry.count, entry.sig)


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


def choose_job(rng: random.Random, state: dict[str, Any]) -> JobType | None:
    good = state.get("good", {})
    available = [job for job in JOB_TYPES if job.build in good]
    if not available:
        return None
    # Yield steering: a job type's weight grows with the distinct findings it
    # produced per CPU hour (W1.6), so effort follows what finds bugs.
    stats = state.get("stats", {})
    weights = []
    for job in available:
        entry = stats.get(job.name, {})
        hours = max(0.5, entry.get("cpu_hours", 0.0))
        yield_rate = entry.get("new_findings", 0) / hours
        weights.append(job.weight * (1 + min(4.0, 4 * yield_rate)))
    return rng.choices(available, weights=weights)[0]


def start_job(job_type: JobType, state: dict[str, Any], rng: random.Random) -> Job:
    seed = rng.randrange(1, 2**31)
    build = BUILDS[job_type.build]
    entry = state["good"][job_type.build]
    out = RUNS / f"{time.strftime('%Y%m%d-%H%M%S')}-{job_type.name}-{seed}"
    out.mkdir(parents=True, exist_ok=True)
    size, scale = choose_cell(rng) if job_type.matrix else ((1440, 900), "1")
    qa = WORKTREE / "tools" / "qa" / "run.py"
    command = ["nice", "-n", "19", "ionice", "-c3", *build.prefix, sys.executable, str(qa),
               "--binary", entry["binary"], "--out", str(out), "monkey", "--profile", job_type.profile,
               "--seed", str(seed), "--steps", str(job_type.steps), "--time-budget", str(job_type.timeout * 0.8)]
    if job_type.profile in ("commands", "editor"):
        command += ["--rounds", str(job_type.steps), "--project", str(WORKTREE)]
    else:
        command += ["--size", f"{size[0]}x{size[1]}", "--coverage", str(COVERAGE), "--corpus", str(CORPUS)]
        if scale != "1":
            command += ["--set", f"ui.scale={scale}"]
        if not is_tiny(size, scale):
            # A real-size cell stays one: without this a run's own resizes and
            # `ui-scale 999999` took most cells tiny within minutes.
            command += ["--min-logical", f"{TINY_LOGICAL[0]}x{TINY_LOGICAL[1]}"]
    env = {**os.environ, "MQA_SLOW_FACTOR": str(build.slow), **build.env}
    cell = f"{job_type.name}/{job_type.build}/{size[0]}x{size[1]}@{scale}"
    with open(out / "driver.log", "wb") as log:
        proc = subprocess.Popen(command, cwd=WORKTREE, env=env, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
    return Job(job_type, seed, out, command, cell, time.time(), proc, entry["sha"], entry["binary"])


CHECK_JOB = JobType("check", 0, "default", timeout=3 * 3600, matrix=False)


def start_check(sig: str, state: dict[str, Any]) -> Job:
    out = RUNS / f"check-{sig}"
    out.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(HERE / "soak.py"), "check", sig]
    with open(out / "driver.log", "wb") as log:
        proc = subprocess.Popen(command, cwd=REPO, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    return Job(CHECK_JOB, 0, out, command, f"check/{sig}", time.time(), proc, state.get("built_sha", ""), "")


def reap_job(job: Job, state: dict[str, Any], log) -> None:
    if job.type is CHECK_JOB:
        tail = (job.out / "driver.log").read_text(errors="replace")[-400:] if (job.out / "driver.log").exists() else ""
        log(f"  {job.cell} done: {tail.strip().splitlines()[-1] if tail.strip() else ''}")
        shutil.rmtree(job.out, ignore_errors=True)
        return
    elapsed = time.time() - job.started
    stats = state.setdefault("stats", {}).setdefault(job.type.name, {})
    stats["runs"] = stats.get("runs", 0) + 1
    stats["cpu_hours"] = stats.get("cpu_hours", 0.0) + elapsed / 3600
    new = 0
    summaries = [json.loads(p.read_text()) for p in job.out.glob("seed-*/findings.json")]
    book = ledger()
    known = book.entries()
    for summary in summaries:
        for finding in summary.get("findings", []):
            sig = finding["signature"]
            fields = dict(kind=finding["kind"], label=finding["label"], commit=job.sha, cell=job.cell,
                          count=finding.get("count", 1))
            if sig not in known:
                bundle = make_bundle(job, summary, finding)
                book.append(sig, "new", bundle=str(bundle), **fields)
                state.setdefault("pending_checks", []).append(sig)
                known[sig] = ld.Entry(sig=sig)
                new += 1
                log(f"  NEW {sig} {finding['label']}")
            else:
                keep_sighting(job, finding, known[sig])
                book.append(sig, "seen", **fields)
    stats["new_findings"] = stats.get("new_findings", 0) + new
    with open(RUN_LOG, "a") as out:
        out.write(json.dumps({"t": time.time(), "job": job.type.name, "cell": job.cell, "seed": job.seed,
                              "sha": job.sha, "seconds": round(elapsed), "exit": job.proc.returncode,
                              "steps": sum(s.get("steps", 0) for s in summaries),
                              "new_states": sum(s.get("new_states", 0) for s in summaries),
                              "findings": sum(len(s.get("findings", [])) for s in summaries),
                              "new": new}) + "\n")
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
    replay_source = bundle / "evidence" / "replay.json"
    if replay_source.exists():
        record = json.loads(replay_source.read_text())
        record["binary_sha"] = job.sha
        (bundle / "replay.json").write_text(json.dumps(record, indent=1))
    meta = {"finding": finding, "cell": job.cell, "sha": job.sha, "binary": job.binary,
            "build": job.type.build, "job": job.type.name, "seed": job.seed,
            "slow": BUILDS[job.type.build].slow, "header": summary.get("header", {})}
    (bundle / "meta.json").write_text(json.dumps(meta, indent=1))
    write_finding_md(bundle)
    return bundle


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
        evidence = (WORKTREE / evidence).resolve()
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
                       "undecodable-*")


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
    if not (after_fix or diagnostics_missing or new_build_for(bundle, job.sha)):
        return
    sightings = bundle / "sightings"
    name = f"{time.strftime('%Y%m%d-%H%M%S')}-{job.sha[:12]}-{job.cell.replace('/', '_')}"
    target = sightings / name
    shutil.copytree(evidence, target, dirs_exist_ok=True)
    (target / "sighting.json").write_text(json.dumps(
        {"cell": job.cell, "sha": job.sha, "binary": job.binary, "build": job.type.build, "seed": job.seed,
         "detail": finding.get("detail", ""), "step": finding.get("step"), "state": finding.get("state"),
         "status_before": entry.status}, indent=1))
    for old in sorted(p for p in sightings.iterdir() if p.is_dir())[:-MAX_SIGHTINGS]:
        shutil.rmtree(old, ignore_errors=True)
    write_finding_md(bundle)


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


def write_finding_md(bundle: Path) -> None:
    meta = json.loads((bundle / "meta.json").read_text())
    finding = meta["finding"]
    entry = ledger().entries().get(finding["signature"])
    evidence = bundle / "evidence"
    log_text = ""
    for name in ("log-tail.txt", "trace.txt"):
        if (evidence / name).exists():
            log_text += (evidence / name).read_text(errors="replace")
    stack = fnd.stack_key(log_text, frames=8)
    run_py = "tools/qa/run.py"
    slow = meta.get("slow", 1)
    env = f"MQA_SLOW_FACTOR={slow} " if slow != 1 else ""
    lines = [
        f"# {finding['label']}",
        "",
        f"- **Signature:** `{finding['signature']}` (oracle `{finding['kind']}`)",
        f"- **Status:** {entry.status if entry else 'new'}; seen {entry.count if entry else 1} time(s) in "
        f"{len(entry.cells) if entry else 1} cell(s)",
        f"- **Reproduction:** " + (f"{'deterministic' if entry.deterministic else 'flaky'}, rate "
                                   f"{entry.rate:.0%}" + (" at the recorded pace (the replays keep the run's "
                                                          "timing; `run.py replay` reads it from the record)"
                                                          if entry.paced else "")
                                   if entry and entry.rate is not None else "not measured yet"),
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
    lines += ["Full replay:", "", "```bash",
              f"{env}{run_py} --binary build/microide/microide replay {bundle / 'replay.json'}", "```", "",
              f"The binary that found it: `{meta['binary']}`.", ""]
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
    pictures = sorted(p.name for p in evidence.glob("*.png")) if evidence.exists() else []
    if pictures:
        lines += ["## Evidence", ""] + [f"- `evidence/{p}`" for p in pictures] + [
            "- `evidence/trace.txt`: the app's frame and input trace since the previous action",
            "- `evidence/log-tail.txt`: the app log's tail"] + (
            ["- `evidence/log-tail-before-relaunch.txt`: the killed process's log tail (its last flushes and saves)"]
            if (evidence / "log-tail-before-relaunch.txt").exists() else []) + [""]
    similar = similar_entries(finding["kind"], finding["label"], finding["signature"])
    if similar:
        lines += ["## Similar findings in the ledger", ""]
        for other in similar:
            lines.append(f"- `{other.sig}` {other.label} -- {other.status}"
                         + (f", fixed in `{other.fix_commit}`" if other.fix_commit else ""))
        lines.append("")
    lines += ["## For the fixing agent", "", "Follow `tools/soak/AGENT.md`: claim it "
              f"(`tools/soak/soak.py ledger claim {finding['signature']}`), reproduce from the minimal replay, "
              "write the failing-first test, fix, and mark it fixed with the commit.", ""]
    (bundle / "FINDING.md").write_text("\n".join(lines))


# ---- checks: determinism, minimization, verification ------------------------------

def check_finding(sig: str, state: dict[str, Any], log) -> None:
    """Replays a new finding three times for its rate; minimizes a deterministic
    one. Runs as a job of the runner (it holds three displays at once)."""
    bundle = FINDINGS / sig
    replay = bundle / "replay.json"
    meta_path = bundle / "meta.json"
    if not replay.exists() or not meta_path.exists():
        return
    meta = json.loads(meta_path.read_text())
    binary = meta["binary"]
    env = {**os.environ, "MQA_SLOW_FACTOR": str(meta.get("slow", 1))}
    minimize = [sys.executable, str(WORKTREE / "tools" / "soak" / "minimize.py"), str(replay),
                "--signature", sig, "--binary", binary, "--out", str(bundle)]
    result = subprocess.run(["nice", "-n", "19", *minimize, "--check-only", "--runs", "3", "--jobs", "3"],
                            capture_output=True, text=True, env=env, timeout=3600)
    rate, paced = 0.0, False
    for line in result.stdout.splitlines():
        if line.startswith("{"):
            rate = json.loads(line).get("rate", 0.0)
            paced = bool(json.loads(line).get("paced", False))
    actions = len(json.loads(replay.read_text()).get("actions", []))
    deterministic = rate >= 2 / 3
    ledger().append(sig, "checked", rate=rate, deterministic=deterministic, actions=actions, paced=paced)
    log(f"  checked {sig}: rate {rate:.0%}" + (" at the recorded pace" if paced else ""))
    if paced:
        # Every later replay of it -- the minimizer's, the agent's, the fix's
        # verification -- keeps the recorded pace.
        replay.write_text(json.dumps({**json.loads(replay.read_text()), "paced": True}))
    if deterministic:
        if shutil.which("notify-send"):
            subprocess.run(["notify-send", "microide soak: new deterministic finding",
                            json.loads(meta_path.read_text())["finding"]["label"]], capture_output=True)
        result = subprocess.run(["nice", "-n", "19", *minimize, "--jobs", "4"], capture_output=True,
                                text=True, env=env, timeout=7200)
        for line in result.stdout.splitlines():
            if line.startswith("{") and json.loads(line).get("reproduced"):
                ledger().append(sig, "minimized", actions=json.loads(line)["actions"])
                log(f"  minimized {sig}: {json.loads(line)['actions']} actions")
    write_finding_md(bundle)


def fixes_to_verify(state: dict[str, Any]):
    """The fixed ledger entries whose fix commit the default build contains, each
    with that build's record."""
    good = state.get("good", {}).get("default")
    if not good:
        return
    for entry in ledger().entries().values():
        if entry.status != "fixed" or not entry.fix_commit:
            continue
        if has_commit(entry.fix_commit, good["sha"]):
            yield entry, good


def verify_fixed(state: dict[str, Any], log) -> None:
    """A fixed signature whose fix commit is in the current default build is
    replayed (minimal replay, else the full one): gone -> verified, back ->
    reopened."""
    for entry, good in list(fixes_to_verify(state)):
        bundle = FINDINGS / entry.sig
        replay = bundle / "minimal.json" if (bundle / "minimal.json").exists() else bundle / "replay.json"
        if not replay.exists():
            ledger().append(entry.sig, "verified", note="no replay to re-check; trusted the fix")
            prune_verified_bundle(bundle)
            continue
        result = subprocess.run(["nice", "-n", "19", sys.executable, str(WORKTREE / "tools/soak/minimize.py"),
                                 str(replay), "--signature", entry.sig, "--binary", good["binary"],
                                 "--check-only", "--runs", "3", "--jobs", "3", "--out", str(bundle / "verify")],
                                capture_output=True, text=True, timeout=3600)
        rate = next((json.loads(l)["rate"] for l in result.stdout.splitlines() if l.startswith("{")), 1.0)
        if rate == 0:
            ledger().append(entry.sig, "verified", commit=good["sha"])
            log(f"  verified {entry.sig}; pruned {prune_verified_bundle(bundle) / 1e6:.1f} MB of its evidence")
        else:
            ledger().append(entry.sig, "reopened", commit=good["sha"], rate=rate)
            log(f"  REOPENED {entry.sig} (rate {rate:.0%} on {good['sha'][:10]})")


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
                    commit=git("rev-parse", "main"), cell="runner", detail=message)


def busy_reason(our_jobs: int) -> str | None:
    if PAUSE.exists():
        return "paused (build/soak/PAUSE)"
    free = shutil.disk_usage(SOAK).free
    if free < MIN_FREE_BYTES:
        return f"only {free / 1e9:.0f} GB free on the disk (MICROIDE_SOAK_MIN_FREE_GB)"
    if subprocess.run(["pgrep", "-f", "tools/run-checks.sh"], capture_output=True).returncode == 0:
        return "a run-checks lane is running"
    cores = os.cpu_count() or 4
    load = os.getloadavg()[0]
    if external_load(load, our_jobs) > cores * BUSY_FRACTION:
        return f"the machine is busy (load {load:.1f})"
    return None


# Our own jobs each keep about one and a half cores busy (the app, Xvfb, the
# driver), which is what both the load accounting and the job budget count.
CORES_PER_JOB = 1.5
# Someone else's load above this share of the cores: start nothing.
BUSY_FRACTION = 0.6
# The share of the cores the soak may fill when nothing else runs: the rest is
# headroom for the desktop and for the load average's minute of lag.
FILL_FRACTION = 0.9
# The least time between two job starts: an idle night reaches the ceiling in a
# few minutes, and a job's cost is in the load average before the next starts.
RAMP_SECONDS = 20.0


def external_load(load: float, our_jobs: int) -> float:
    """The 1-minute load that is not ours."""
    return max(0.0, load - CORES_PER_JOB * our_jobs)


def max_jobs(cores: int) -> int:
    return max(1, int(cores * FILL_FRACTION / CORES_PER_JOB))


def allowed_jobs(load: float, our_jobs: int, cores: int, ceiling: int) -> int:
    """How many jobs may run at once now: the cores others leave free, in jobs.

    Running jobs are never killed when this falls; they finish (each has a hard
    timeout) and are not replaced. The load average lags by about a minute, so
    the runner starts at most one job per RAMP_SECONDS and the ramp shows in
    the load before it goes further.

    >>> allowed_jobs(0.0, 0, 16, 9)    # idle: the ceiling
    9
    >>> allowed_jobs(9.0, 6, 16, 9)    # all of it ours: still the ceiling
    9
    >>> allowed_jobs(7.0, 0, 16, 9)    # someone compiling on 7 cores
    4
    >>> allowed_jobs(12.0, 2, 16, 9)   # 9 cores not ours: what they leave
    3
    >>> allowed_jobs(14.0, 0, 16, 9)   # busy: nothing new
    0
    """
    other = external_load(load, our_jobs)
    if other > cores * BUSY_FRACTION:
        return 0
    free = cores * FILL_FRACTION - other
    return max(1, min(ceiling, int(free / CORES_PER_JOB)))


def disk_usage(path: Path) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(root, name)).st_size
            except OSError:
                pass
    return total


# What a verified finding's bundle stops needing: the fix is in and its replay
# no longer reproduces, so the frames, logs and repeat sightings are moot.
# FINDING.md, meta.json and the replays stay -- a few KB that let a later
# sighting reopen it against the original record.
VERIFIED_PRUNED = ("evidence", "sightings", "verify")


def prune_verified_bundle(bundle: Path) -> int:
    """Deletes a verified bundle's evidence; returns the bytes freed."""
    freed = 0
    for name in VERIFIED_PRUNED:
        path = bundle / name
        if path.is_dir():
            freed += disk_usage(path)
            shutil.rmtree(path, ignore_errors=True)
    return freed


def collect_garbage(log) -> None:
    """Drops what nothing reads any more: verified bundles' evidence and the
    corpus prefixes too long for a run to start from."""
    freed = 0
    for sig, entry in ledger().entries().items():
        if entry.status == "verified":
            freed += prune_verified_bundle(FINDINGS / sig)
    removed, corpus_freed = mm.Corpus(CORPUS).prune()
    if freed or removed:
        log(f"gc: {freed / 1e6:.0f} MB of verified evidence, {removed} unusable corpus prefixes "
            f"({corpus_freed / 1e6:.0f} MB)")


def enforce_disk_budget(log) -> None:
    budget = DISK_BUDGET_GB * 1e9
    if disk_usage(SOAK) <= budget:
        return
    # Oldest verified bundles first, then the corpus's longest prefixes.
    entries = ledger().entries()
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
    log_file = open(SOAK / "soak.log", "a")

    def log(message: str) -> None:
        line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {message}"
        print(line, flush=True)
        log_file.write(line + "\n")
        log_file.flush()

    cores = os.cpu_count() or 4
    ceiling = args.jobs or max_jobs(cores)

    def current_cap(our_jobs: int) -> int:
        if args.fixed:
            return ceiling
        return allowed_jobs(os.getloadavg()[0], our_jobs, cores, ceiling)
    jobs: list[Job] = []
    stopping = False

    def stop(*unused: Any) -> None:
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    last_build = 0.0
    last_housekeeping = 0.0
    deadline = time.time() + args.max_hours * 3600 if args.max_hours else None
    mode = "fixed" if args.fixed else "following the load"
    log(f"soak runner up: up to {ceiling} job(s) at once, {mode} -> {SOAK}")
    last_cap = -1
    last_start = 0.0
    last_containment = 0.0
    # Set once per build interval when refresh_due finds work, cleared by the
    # build that does it: until then no job or check starts, so the running
    # ones drain.
    draining = False
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
        # A build runs between jobs, at most once per interval counted from the
        # END of the last one (stamped at its start, a 20-minute build on a moving
        # main left the interval spent and the runner rebuilt forever, starting no
        # job). Jobs keep running on the last good binaries meanwhile.
        if now - last_build > args.build_interval and not draining:
            if not state.get("good") or refresh_due(state):
                draining = True
            else:
                last_build = now
        if draining and not jobs:
            try:
                build_all(state, log)
            except Exception as error:  # noqa: BLE001 - a broken build never stops the runner
                log(f"build step failed: {error}")
            verify_fixed(state, log)
            last_build = time.time()
            draining = False
            continue
        for job in list(jobs):
            if job.proc.poll() is not None or now - job.started > job.type.timeout:
                if job.proc.poll() is None:
                    os.killpg(job.proc.pid, signal.SIGKILL)
                    job.proc.wait()
                    log(f"job {job.type.name} seed {job.seed} hit its timeout")
                jobs.remove(job)
                reap_job(job, state, log)
                state["current"] = [j.cell for j in jobs]
                save_state(state)
        pending = state.get("pending_checks", [])
        cap = current_cap(len(jobs))
        if cap != last_cap:
            log(f"job budget {cap} (load {os.getloadavg()[0]:.1f}, {len(jobs)} running)")
            last_cap = cap
        if pending and not draining and len(jobs) < cap and busy_reason(len(jobs)) is None:
            # Checks first: a new finding's rate and minimal replay are worth more
            # than another fresh run. A check holds three displays, so it counts
            # as a job of its own.
            # The most severe first: a paint finding from a tiny window waits
            # behind everything a user could lose work to.
            known = ledger().entries()
            sig = min(pending, key=lambda p: priority_key(known.get(p, ld.Entry(sig=p))))
            pending.remove(sig)
            jobs.append(start_check(sig, state))
            log(f"check {sig}")
            save_state(state)
        reason = busy_reason(len(jobs))
        if reason is None and draining:
            # A build is due: let the running jobs drain instead of starting more.
            reason = "draining for a build"
        if reason is None:
            # One per RAMP_SECONDS: the load average needs time to show what the
            # last one costs before the budget is trusted for the next.
            if len(jobs) < cap and (args.fixed or now - last_start >= RAMP_SECONDS):
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
        if args.once and not jobs and not state.get("pending_checks"):
            break
        if now - last_housekeeping > 600:
            collect_garbage(log)
            enforce_disk_budget(log)
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
        print(f"{status:9} {len(items)}")
        for entry in sorted(items, key=priority_key)[:8] if status in ("new", "reopened", "claimed") else []:
            det = "det" if entry.deterministic else ("flaky" if entry.deterministic is False else "?")
            print(f"    {entry.sig} x{entry.count:<4} {det:5} {priority_tag(entry):15} {entry.label[:90]}")
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
    open_entries = [e for e in entries.values() if e.status in ("new", "reopened")]
    section("New, deterministic", [e for e in open_entries if e.deterministic])
    section("New, flaky", [e for e in open_entries if e.deterministic is False])
    section("New, not yet checked", [e for e in open_entries if e.deterministic is None])
    section("Claimed", [e for e in entries.values() if e.status == "claimed"])
    section("Fixed, awaiting verification", [e for e in entries.values() if e.status == "fixed"])
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
    elif args.action == "claim":
        book.append(args.sig, "claimed", by=args.extra or os.environ.get("USER", "agent"))
    elif args.action == "fixed":
        if not args.extra:
            print("fixed needs the fix commit", file=sys.stderr)
            return 2
        book.append(args.sig, "fixed", commit=git("rev-parse", args.extra))
    elif args.action == "reopen":
        book.append(args.sig, "reopened", note=args.extra or "")
    elif args.action == "note":
        book.append(args.sig, "note", text=args.extra or "")
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    check_finding(args.sig, load_state(), print)
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    state = load_state()
    build_all(state, print)
    save_state(state)
    return 0


def cmd_gc(_args) -> int:
    collect_garbage(print)
    return 0

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    run = sub.add_parser("run")
    run.add_argument("--jobs", type=int, default=0,
                     help="the most jobs at once (default: 90%% of the cores, at 1.5 cores a job)")
    run.add_argument("--fixed", action="store_true",
                     help="always run --jobs jobs instead of following the machine's load")
    run.add_argument("--once", action="store_true", help="exit when the started jobs and checks are done")
    run.add_argument("--max-hours", type=float, default=0)
    run.add_argument("--build-interval", type=float, default=900, help="seconds between main checks")
    sub.add_parser("status")
    sub.add_parser("report")
    sub.add_parser("build")
    sub.add_parser("gc", help="delete verified findings' evidence and unusable corpus prefixes")
    check = sub.add_parser("check", help="measure a finding's reproduction rate and minimize it")
    check.add_argument("sig")
    led = sub.add_parser("ledger")
    led.add_argument("action", nargs="?", default="list", choices=("list", "show", "claim", "fixed", "reopen", "note"))
    led.add_argument("sig", nargs="?")
    led.add_argument("extra", nargs="?")
    args = parser.parse_args()
    return {"run": cmd_run, "status": cmd_status, "report": cmd_report, "ledger": cmd_ledger,
            "build": cmd_build, "check": cmd_check, "gc": cmd_gc}[args.mode](args)



if __name__ == "__main__":
    sys.exit(main())
