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
output directory and hard timeout, under `nice -n 19` and `ionice -c3`, capped
at half the cores, paused by `build/soak/PAUSE`, backing off while the machine
is busy or a `run-checks.sh` lane runs.

Every finding goes into the ledger (ledger.py), deduplicated by signature. A
new one gets a bundle under `build/soak/findings/<sig>/` with FINDING.md; it is
then replayed for a reproduction rate and, when deterministic, minimized
(minimize.py). A fixed signature is re-checked on every later build and
reopened if it comes back. See tools/soak/AGENT.md for the fixing agent's side.
"""

from __future__ import annotations

import argparse
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
    if state.get("built_sha") == head and all(BUILDS[name].binary().exists() for name in state.get("good", {})):
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


def ledger() -> ld.Ledger:
    return ld.Ledger(SOAK / "ledger.jsonl")


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
    size = rng.choice(SIZES) if job_type.matrix else (1440, 900)
    scale = rng.choice(SCALES) if job_type.matrix else "1"
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
    evidence = Path(finding.get("evidence") or "")
    if evidence.is_dir():
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
                                   f"{entry.rate:.0%}" if entry and entry.rate is not None else "not measured yet"),
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
    files = suspected_files(log_text)
    if files:
        lines += ["## Suspected files", ""] + [f"- `{f}`" for f in files] + [""]
    pictures = sorted(p.name for p in evidence.glob("*.png")) if evidence.exists() else []
    if pictures:
        lines += ["## Evidence", ""] + [f"- `evidence/{p}`" for p in pictures] + [
            "- `evidence/trace.txt`: the app's frame and input trace since the previous action",
            "- `evidence/log-tail.txt`: the app log's tail", ""]
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
    rate = 0.0
    for line in result.stdout.splitlines():
        if line.startswith("{"):
            rate = json.loads(line).get("rate", 0.0)
    actions = len(json.loads(replay.read_text()).get("actions", []))
    deterministic = rate >= 2 / 3
    ledger().append(sig, "checked", rate=rate, deterministic=deterministic, actions=actions)
    log(f"  checked {sig}: rate {rate:.0%}")
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


def verify_fixed(state: dict[str, Any], log) -> None:
    """A fixed signature whose fix commit is in the current default build is
    replayed (minimal replay, else the full one): gone -> verified, back ->
    reopened."""
    good = state.get("good", {}).get("default")
    if not good:
        return
    for entry in ledger().entries().values():
        if entry.status != "fixed" or not entry.fix_commit:
            continue
        ancestor = subprocess.run(["git", "merge-base", "--is-ancestor", entry.fix_commit, good["sha"]],
                                  cwd=REPO, capture_output=True).returncode == 0
        if not ancestor:
            continue
        bundle = FINDINGS / entry.sig
        replay = bundle / "minimal.json" if (bundle / "minimal.json").exists() else bundle / "replay.json"
        if not replay.exists():
            ledger().append(entry.sig, "verified", note="no replay to re-check; trusted the fix")
            continue
        result = subprocess.run(["nice", "-n", "19", sys.executable, str(WORKTREE / "tools/soak/minimize.py"),
                                 str(replay), "--signature", entry.sig, "--binary", good["binary"],
                                 "--check-only", "--runs", "3", "--jobs", "3", "--out", str(bundle / "verify")],
                                capture_output=True, text=True, timeout=3600)
        rate = next((json.loads(l)["rate"] for l in result.stdout.splitlines() if l.startswith("{")), 1.0)
        if rate == 0:
            ledger().append(entry.sig, "verified", commit=good["sha"])
            log(f"  verified {entry.sig}")
        else:
            ledger().append(entry.sig, "reopened", commit=good["sha"], rate=rate)
            log(f"  REOPENED {entry.sig} (rate {rate:.0%} on {good['sha'][:10]})")


# ---- housekeeping ----------------------------------------------------------------

def busy_reason(our_jobs: int) -> str | None:
    if PAUSE.exists():
        return "paused (build/soak/PAUSE)"
    if subprocess.run(["pgrep", "-f", "tools/run-checks.sh"], capture_output=True).returncode == 0:
        return "a run-checks lane is running"
    cores = os.cpu_count() or 4
    load = os.getloadavg()[0]
    # Our own jobs each keep about one and a half cores busy.
    if load - 1.5 * our_jobs > cores * 0.6:
        return f"the machine is busy (load {load:.1f})"
    return None


def disk_usage(path: Path) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(root, name)).st_size
            except OSError:
                pass
    return total


def enforce_disk_budget(log) -> None:
    budget = DISK_BUDGET_GB * 1e9
    if disk_usage(SOAK) <= budget:
        return
    # Oldest verified bundles first, then the corpus's longest prefixes.
    entries = ledger().entries()
    verified = sorted((FINDINGS / sig for sig, e in entries.items() if e.status == "verified"),
                      key=lambda p: p.stat().st_mtime if p.exists() else 0)
    for path in verified:
        shutil.rmtree(path / "evidence", ignore_errors=True)
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

    cap = args.jobs or max(1, (os.cpu_count() or 4) // 4)
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
    log(f"soak runner up: {cap} job(s) at once -> {SOAK}")
    while not stopping:
        now = time.time()
        if deadline and now > deadline:
            break
        if now - last_build > args.build_interval and not jobs:
            try:
                build_all(state, log)
            except Exception as error:  # noqa: BLE001 - a broken build never stops the runner
                log(f"build step failed: {error}")
            last_build = now
            verify_fixed(state, log)
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
        if pending and len(jobs) < cap and busy_reason(len(jobs)) is None:
            # Checks first: a new finding's rate and minimal replay are worth more
            # than another fresh run. A check holds three displays, so it counts
            # as a job of its own.
            sig = pending.pop(0)
            jobs.append(start_check(sig, state))
            log(f"check {sig}")
            save_state(state)
        reason = busy_reason(len(jobs))
        if reason is None:
            while len(jobs) < cap:
                job_type = choose_job(rng, state)
                if job_type is None:
                    break
                job = start_job(job_type, state, rng)
                jobs.append(job)
                log(f"start {job.cell} seed {job.seed}")
        elif not jobs:
            state["waiting"] = reason
        state["current"] = [j.cell for j in jobs]
        save_state(state)
        if args.once and not jobs and not state.get("pending_checks"):
            break
        if now - last_housekeeping > 600:
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
        for entry in sorted(items, key=lambda e: -e.count)[:8] if status in ("new", "reopened", "claimed") else []:
            det = "det" if entry.deterministic else ("flaky" if entry.deterministic is False else "?")
            print(f"    {entry.sig} x{entry.count:<4} {det:5} {entry.label[:90]}")
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
        for entry in sorted(items, key=lambda e: (-e.count, e.sig)):
            lines.append(f"- `{entry.sig}` x{entry.count} {entry.label} -- "
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    run = sub.add_parser("run")
    run.add_argument("--jobs", type=int, default=0, help="jobs at once (default: a quarter of the cores)")
    run.add_argument("--once", action="store_true", help="exit when the started jobs and checks are done")
    run.add_argument("--max-hours", type=float, default=0)
    run.add_argument("--build-interval", type=float, default=900, help="seconds between main checks")
    sub.add_parser("status")
    sub.add_parser("report")
    sub.add_parser("build")
    check = sub.add_parser("check", help="measure a finding's reproduction rate and minimize it")
    check.add_argument("sig")
    led = sub.add_parser("ledger")
    led.add_argument("action", nargs="?", default="list", choices=("list", "show", "claim", "fixed", "reopen", "note"))
    led.add_argument("sig", nargs="?")
    led.add_argument("extra", nargs="?")
    args = parser.parse_args()
    return {"run": cmd_run, "status": cmd_status, "report": cmd_report, "ledger": cmd_ledger,
            "build": cmd_build, "check": cmd_check}[args.mode](args)


if __name__ == "__main__":
    sys.exit(main())
