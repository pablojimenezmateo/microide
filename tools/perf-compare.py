#!/usr/bin/env python3
"""Run the full perf harness against the current working tree (including
uncommitted changes) and against a comparison commit, then print a coloured
ASCII comparison table for every scenario and every metric.

Usage:
  tools/perf-compare.py [COMMIT_SHA]

COMMIT_SHA defaults to the local `main` branch's HEAD (falling back to
`origin/main` if no local `main` exists).

Environment overrides:
  ITERATIONS    perf iterations per scenario (default: 10)
  SCENARIOS     comma-separated scenario filter (default: all registered)
  NOISE_SIGMA   k for the k-sigma noise band (default: 2.0). A delta whose
                absolute size is within k * max(stdev_cur, stdev_tgt) of the
                per-iteration samples is treated as noise (dimmed, excluded
                from the regressions/improvements summary).
  REGRESS_PCT   secondary percentage floor for the summary lists only — a
                statistically-real delta whose magnitude is below this pct is
                kept dim and not summarised. Set to 0 to disable. Default: 5.
  KEEP          if "1", keep the temporary worktree and JSON reports
  NO_COLOR      if set, disable ANSI colour output
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Optional


# ---------------------------------------------------------------------------
# Colour support
# ---------------------------------------------------------------------------

_ANSI_RE = re.compile(r"\x1b\[[\d;]*m")


def _supports_color() -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    return sys.stderr.isatty()


_USE_COLOR = _supports_color()


def _c(code: str, text: str) -> str:
    return f"\x1b[{code}m{text}\x1b[0m" if _USE_COLOR else text


def bold(t: str) -> str:    return _c("1", t)
def dim(t: str) -> str:     return _c("2", t)
def red(t: str) -> str:     return _c("31", t)
def green(t: str) -> str:   return _c("32", t)
def yellow(t: str) -> str:  return _c("33", t)
def cyan(t: str) -> str:    return _c("36", t)


def visible_len(s: str) -> int:
    return len(_ANSI_RE.sub("", s))


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

_PREFIX = cyan("[perf-compare]")


def log(label: Optional[str], message: str) -> None:
    if label:
        print(f"{_PREFIX} ({label}) {message}", file=sys.stderr, flush=True)
    else:
        print(f"{_PREFIX} {message}", file=sys.stderr, flush=True)


def die(message: str, code: int = 1) -> None:
    print(red(f"fatal: {message}"), file=sys.stderr, flush=True)
    sys.exit(code)


def print_tail(log_path: Path, n: int = 120) -> None:
    if not log_path.exists():
        return
    print(dim(f"----- tail of {log_path} -----"), file=sys.stderr)
    lines = log_path.read_text(errors="replace").splitlines()
    for line in lines[-n:]:
        print(line, file=sys.stderr)


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------

def git(*args: str, cwd: Optional[Path] = None, check: bool = True) -> str:
    result = subprocess.run(
        ["git", *args], cwd=cwd, check=check,
        capture_output=True, text=True,
    )
    return result.stdout.strip()


def repo_root() -> Path:
    return Path(git("rev-parse", "--show-toplevel"))


def resolve_commit(ref: str) -> Optional[str]:
    try:
        return git("rev-parse", "--verify", f"{ref}^{{commit}}")
    except subprocess.CalledProcessError:
        return None


def resolve_default_target() -> str:
    for ref in ("refs/heads/main", "refs/remotes/origin/main"):
        sha = resolve_commit(ref)
        if sha:
            return sha
    die("cannot find local 'main' or 'origin/main' to use as default target")
    return ""  # unreachable, satisfies type checker


def short_sha(sha: str) -> str:
    return git("rev-parse", "--short", sha)


def is_dirty(root: Path) -> bool:
    diff = subprocess.run(
        ["git", "diff", "--quiet", "--ignore-submodules"], cwd=str(root),
    ).returncode
    if diff != 0:
        return True
    staged = subprocess.run(
        ["git", "diff", "--cached", "--quiet", "--ignore-submodules"], cwd=str(root),
    ).returncode
    if staged != 0:
        return True
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard"],
        cwd=str(root), capture_output=True, text=True,
    ).stdout.strip()
    return bool(untracked)


# ---------------------------------------------------------------------------
# Subprocess helpers
# ---------------------------------------------------------------------------

LineFilter = Callable[[str], Optional[str]]


def run_streaming(argv: list[str], cwd: Path, log_file,
                  line_filter: Optional[LineFilter] = None,
                  env: Optional[dict[str, str]] = None) -> int:
    """Run argv, stream stdout+stderr lines into log_file, optionally render
    a filtered line to stderr live. Returns process return code."""
    proc = subprocess.Popen(
        argv, cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
    )
    assert proc.stdout is not None
    try:
        for line in proc.stdout:
            log_file.write(line)
            log_file.flush()
            if line_filter is None:
                continue
            rendered = line_filter(line.rstrip("\n"))
            if rendered is not None:
                print(rendered, file=sys.stderr, flush=True)
    finally:
        proc.stdout.close()
    return proc.wait()


# ---------------------------------------------------------------------------
# Runner environment
# ---------------------------------------------------------------------------

def detect_runner() -> list[str]:
    """Prefix for a microide_perf invocation.

    Deliberately empty. microide_perf pins its own lane (video=dummy,
    renderer=software) before SDL_Init, which is the lane the committed
    baselines were recorded in. This used to wrap the run in `xvfb-run -a env
    SDL_VIDEODRIVER=x11`, which charged real window-system present cost: 2-12x
    on every frame-pumping scenario, 1.0x on the pure-unit ones. Both sides of
    an A/B paid it so the comparison stayed valid, but it made this tool several
    times slower and buried small deltas under present jitter.
    """
    return ["env", "SDL_AUDIODRIVER=dummy"]


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

BUILD_DIR = Path("build/microide-perf-make")


def prepare_current_worktree(root: Path, out_dir: Path) -> Path:
    """Create a worktree at HEAD with the working tree's dirty diff applied.

    Running the current side from a worktree (rather than the user's main
    tree) ensures cwd-dependent startup work — `Application::Initialize`
    treats cwd as the project root, walking .git and .gitignore from there —
    is symmetric with the target side. Without this, a dirty main tree
    allocates far more during startup-only scenarios than a fresh worktree,
    producing apparent regressions that are pure measurement artifacts.
    """
    head_sha = git("rev-parse", "HEAD")
    wt = out_dir / "wt-current"
    log("current", dim(f"preparing worktree at {wt}"))
    subprocess.run(
        ["git", "worktree", "add", "--detach", str(wt), head_sha],
        check=True, capture_output=True,
    )

    if not is_dirty(root):
        return wt

    log("current", dim("applying dirty diff to worktree"))
    patch_path = out_dir / "current.dirty.patch"
    with open(patch_path, "wb") as patch_file:
        rc = subprocess.run(
            ["git", "diff", "HEAD", "--binary"],
            cwd=str(root), stdout=patch_file,
        ).returncode
    if rc != 0:
        die("failed to capture dirty diff for current side")
    if patch_path.stat().st_size > 0:
        applied = subprocess.run(
            ["git", "apply", "--whitespace=nowarn", str(patch_path)],
            cwd=str(wt), capture_output=True,
        )
        if applied.returncode != 0:
            sys.stderr.write(applied.stderr.decode(errors="replace"))
            die("failed to apply dirty diff to current worktree")

    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=str(root), check=True, capture_output=True,
    ).stdout.split(b"\x00")
    for raw in untracked:
        if not raw:
            continue
        rel = raw.decode()
        src = root / rel
        if not src.is_file():
            continue
        dst = wt / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    return wt


# A cached object whose LTO IR section will not decompress, which gcc reports as
# an internal compiler error at LTO link time (TD-2026-08-06-146):
#
#   lto1: internal compiler error: original not compressed with zstd
#   lto-wrapper: fatal error: /usr/bin/c++ returned 1 exit status
#
# It is a toolchain fault, not a source fault, and nothing in the message says so
# — the commit looks broken. That matters far more than an occasional retry
# because a `git bisect run` script reports a build failure as `skip` (exit 125),
# and a bisect that skips a quarter of its candidates converges on a RANGE, or on
# the wrong commit when the real one is in the skipped set. Both microide-perf and
# this script build with LTO on, so every walk over history is exposed.
_CCACHE_LTO_ICE_MARKERS = (
    "original not compressed with zstd",
    "lto1: internal compiler error",
)


def _log_has_ccache_lto_ice(log_path: Path) -> bool:
    try:
        text = log_path.read_text(errors="replace")
    except OSError:
        return False
    return any(marker in text for marker in _CCACHE_LTO_ICE_MARKERS)


def build_side(where: Path, label: str, log_path: Path) -> Path:
    log(label, f"building microide_perf (log: {log_path})")
    last_pct = [-10]

    def filter_build(line: str) -> Optional[str]:
        m = re.match(r"^\[\s*(\d+)%\] (Built target|Linking)", line)
        if not m:
            return None
        pct = int(m.group(1))
        if "Built target" in line or pct == 100 or pct - last_pct[0] >= 10:
            last_pct[0] = pct
            return f"{_PREFIX} ({label}) {dim(line)}"
        return None

    # ccache caches PCH-using translation units only when told to tolerate the
    # pch_defines/time_macros sloppiness. Without it ~99.8% of this tree's
    # compiles are "could not use precompiled header" and bypass the cache
    # entirely, which is most of a history walk's wall time.
    build_env = dict(os.environ)
    build_env.setdefault("CCACHE_SLOPPINESS", "pch_defines,time_macros")

    def build(env: dict[str, str]) -> int:
        return run_streaming(
            ["cmake", "--build", str(BUILD_DIR),
             "-j", str(os.cpu_count() or 1),
             "--target", "microide_perf"],
            cwd=where, log_file=log_file, line_filter=filter_build, env=env,
        )

    with open(log_path, "w") as log_file:
        rc = run_streaming(
            ["cmake", "--preset", "microide-perf"],
            cwd=where, log_file=log_file, env=build_env,
        )
        if rc != 0:
            print_tail(log_path)
            die(f"({label}) cmake configure failed (rc={rc})")
        rc = build(build_env)
        if rc != 0 and _log_has_ccache_lto_ice(log_path):
            # Retry ONCE with the cache bypassed, and say so loudly. Silence here
            # would be the whole defect: the point is that this side's numbers
            # came from a build that had to work around the toolchain, not from a
            # commit that failed to build.
            log(label, yellow(
                "ccache/LTO internal compiler error detected — this is a "
                "toolchain fault, not a broken commit (TD-2026-08-06-146). "
                "Rebuilding once with CCACHE_DISABLE=1."))
            retry_env = dict(build_env)
            retry_env["CCACHE_DISABLE"] = "1"
            rc = build(retry_env)
            if rc == 0:
                log(label, yellow(
                    "cache-bypassed rebuild succeeded; the commit is fine. "
                    "Consider `ccache --clear` if this keeps happening."))
        if rc != 0:
            print_tail(log_path)
            die(f"({label}) build failed (rc={rc})")
    binary = where / BUILD_DIR / "microide" / "microide_perf"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        die(f"({label}) microide_perf binary missing at {binary}")
    return binary


# ---------------------------------------------------------------------------
# Fixtures + scenarios
# ---------------------------------------------------------------------------

FIXTURE_ROOT_REL = "tests/perf/fixtures"
# Entries under the fixture root that are not fixtures and must never be mirrored.
FIXTURE_MIRROR_SKIP = {".gitignore"}

# Scenarios where additional iterations add no signal (long deterministic
# sleeps whose value is a single binary wake-budget assertion). Capped here so
# `perf-compare.py` doesn't multiply minutes of idle time by ITERATIONS. Any new
# deterministic-sleep scenario must be registered here or it will run the full
# ITERATIONS count and add minutes of idle wall-time per comparison.
SLOW_SCENARIO_ITERS: dict[str, int] = {
    "idle_soak_30s": 1,
    "long_soak_8h": 1,
}


def mirror_fixtures(src_root: Path, dst_root: Path, label: str) -> None:
    """Mirror perf fixtures from src_root into dst_root via symlink.

    The fixture set is discovered dynamically from the current side's fixture
    tree rather than a hand-maintained list, so it can never drift out of sync
    with the scenarios that consume it. Generated (gitignored) trees are absent
    on a fresh target checkout and would otherwise make `EnsureFixtureOrSkip`
    scenarios silently return ~0 ms / ~0 alloc metrics that look like absurd
    regressions vs the current side. Committed fixtures already arrive with the
    target checkout, so the `not dst.exists()` guard makes mirroring them a
    no-op and never masks a legitimate per-commit fixture difference."""
    src_fixtures = src_root / FIXTURE_ROOT_REL
    (dst_root / FIXTURE_ROOT_REL).mkdir(parents=True, exist_ok=True)
    if not src_fixtures.is_dir():
        return
    for entry in sorted(src_fixtures.iterdir()):
        if entry.name in FIXTURE_MIRROR_SKIP:
            continue
        dst = dst_root / FIXTURE_ROOT_REL / entry.name
        if entry.exists() and not dst.exists():
            os.symlink(entry.resolve(), dst)
            log(label, dim(f"mirrored fixture: {FIXTURE_ROOT_REL}/{entry.name}"))


def discover_scenarios(root: Path) -> list[str]:
    baselines = root / "tests" / "perf" / "baselines"
    if not baselines.is_dir():
        return []
    return sorted(p.stem for p in baselines.glob("*.json"))


# ---------------------------------------------------------------------------
# Interleaved run (scenario-by-scenario across sides)
# ---------------------------------------------------------------------------

class SideRun:
    def __init__(self, where: Path, label: str, out_dir: Path) -> None:
        self.where = where
        self.label = label
        self.merged_dir = out_dir / f"{label}.scenarios"
        self.merged_dir.mkdir(parents=True, exist_ok=True)
        self.binary = where / BUILD_DIR / "microide" / "microide_perf"
        self.failed: list[str] = []


def _mark_failed(side: SideRun, scenario: str) -> None:
    if scenario not in side.failed:
        side.failed.append(scenario)


def run_scenario(side: SideRun, scenario: str, idx: int, total: int,
                 iterations: int, runner: list[str], chunk_tag: str) -> None:
    if iterations <= 0:
        return
    single_json = side.merged_dir / f"{scenario}.{chunk_tag}.json"
    log_path = side.merged_dir / f"{scenario}.{chunk_tag}.log"
    effective_iters = min(iterations, SLOW_SCENARIO_ITERS.get(scenario, iterations))
    tag = f"[{idx}/{total}] {bold(scenario)} {dim('(' + chunk_tag + ')')}"
    starting = dim("starting")
    if effective_iters != iterations:
        starting += dim(f" ({effective_iters} iter)")
    log(side.label, f"{tag} {starting}")
    argv = [
        *runner, str(side.binary),
        f"--iterations={effective_iters}",
        f"--scenarios={scenario}",
        f"--report-json={single_json}",
    ]

    def progress(line: str, _tag: str = tag, _label: str = side.label) -> Optional[str]:
        if line.startswith("[perf] scenario=") and "iteration=" in line:
            i = line.find("iteration=") + len("iteration=")
            return f"{_PREFIX} ({_label}) {_tag} {dim('iter ' + line[i:])}"
        if line.startswith("scenario failed to run:"):
            return f"{_PREFIX} ({_label}) {_tag} {red(line)}"
        return None

    with open(log_path, "w") as log_file:
        rc = run_streaming(argv, cwd=side.where, log_file=log_file, line_filter=progress)

    if single_json.is_file() and single_json.stat().st_size > 0:
        log(side.label, f"{tag} {green('ok')}")
    else:
        log(side.label, f"{tag} {red(f'FAILED (rc={rc}; log: {log_path})')}")
        _mark_failed(side, scenario)


def _percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    values = sorted(values)
    idx = p * (len(values) - 1)
    lo = int(idx)
    hi = math.ceil(idx)
    if lo == hi:
        return values[lo]
    weight = idx - lo
    return values[lo] * (1.0 - weight) + values[hi] * weight


def _recompute_metrics(iterations: list[dict]) -> dict[str, float]:
    def series(key: str) -> list[float]:
        return [float(it[key]) for it in iterations if key in it]

    out: dict[str, float] = {}
    # cpu_ms and rss_growth_bytes are absent from reports produced before those
    # metrics existed; emitting a 0.0 for them would read as a huge improvement in
    # the comparison table rather than "not measured".
    for key in ("wall_ms", "allocations", "cpu_ms", "rss_growth_bytes"):
        values = series(key)
        if not values:
            continue
        out[f"p50_{key}"] = _percentile(values, 0.50)
        out[f"p95_{key}"] = _percentile(values, 0.95)
        out[f"max_{key}"] = max(values)
    # The trimmed mean is the ONE resident statistic the gate enforces, and the
    # merge dropped it: p50 was shown to be both unstable and outright blind for
    # this metric (0 on a scenario retaining a megabyte an iteration,
    # TD-2026-08-05-136), so an A/B that reported only the percentiles could not
    # see a resident regression that the gate would then fail on.
    #
    # Same statistic as the harness (mean with the single largest sample dropped),
    # with one difference worth knowing: a merged side concatenates iterations from
    # several chunk processes, so it carries several cold passes and only one is
    # dropped. That biases both sides identically, which is what a comparison
    # needs; it is not a substitute for the harness's own number.
    rss = series("rss_growth_bytes")
    if rss:
        trimmed = sorted(rss)[:-1] if len(rss) >= 3 else rss
        out["mean_rss_growth_bytes"] = sum(trimmed) / len(trimmed)
    return out


def merge_side_reports(side: SideRun, json_out: Path) -> None:
    merged: dict = {"scenarios": []}
    scenarios_by_name: dict[str, dict] = {}
    for json_path in sorted(side.merged_dir.glob("*.json")):
        try:
            data = json.loads(json_path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        if "metadata" in data and "metadata" not in merged:
            merged["metadata"] = data["metadata"]
        for scenario in data.get("scenarios", []):
            name = scenario.get("scenario")
            if not name:
                continue
            existing = scenarios_by_name.get(name)
            if existing is None:
                copied = dict(scenario)
                copied["iterations"] = list(scenario.get("iterations", []))
                scenarios_by_name[name] = copied
                continue
            existing["iterations"].extend(scenario.get("iterations", []))

    for scenario in scenarios_by_name.values():
        iterations = scenario.get("iterations", [])
        for index, iteration in enumerate(iterations, start=1):
            iteration["index"] = index
        scenario["metrics"] = _recompute_metrics(iterations)
        merged["scenarios"].append(scenario)
    json_out.write_text(json.dumps(merged))


# ---------------------------------------------------------------------------
# Comparison table
# ---------------------------------------------------------------------------

METRICS: list[tuple[str, str]] = [
    ("p50_wall_ms", "ms"),
    ("p95_wall_ms", "ms"),
    ("max_wall_ms", "ms"),
    ("p50_allocations", ""),
    ("p95_allocations", ""),
    ("max_allocations", ""),
    # CPU time sums every thread, so it is the metric that sees work moved onto the
    # background executor -- which wall time cannot. Resident growth is per-iteration
    # delta, not absolute RSS: the harness runs everything in one process, so
    # absolute RSS is dominated by whatever ran before.
    ("p50_cpu_ms", "ms"),
    ("p95_cpu_ms", "ms"),
    ("max_cpu_ms", "ms"),
    # The gated one first: p50/p95/max stay for diagnosis, but the trimmed mean is
    # the statistic that decides a resident gate, so it is the one to read.
    ("mean_rss_growth_bytes", "bytes"),
    ("p50_rss_growth_bytes", "bytes"),
    ("p95_rss_growth_bytes", "bytes"),
    ("max_rss_growth_bytes", "bytes"),
]


def fmt_value(value, unit: str) -> str:
    if value is None:
        return dim("-")
    if unit == "ms":
        return f"{value:,.3f}"
    return f"{value:,.0f}"


def delta_pct(cur, tgt) -> Optional[float]:
    if cur is None or tgt is None or tgt == 0:
        return None
    return (cur - tgt) / tgt * 100.0


def _samples_for_metric(scenario_report: Optional[dict], metric: str) -> list[float]:
    """Per-iteration raw values that underlie the given aggregate metric.

    All wall_ms aggregates (p50/p95/max) draw from the same per-iteration
    `wall_ms` sample stream; same for allocations. We use that stream as the
    spread proxy when judging whether a delta is statistically real.
    """
    if not scenario_report:
        return []
    iters = scenario_report.get("iterations", [])
    if metric.endswith("_wall_ms"):
        key = "wall_ms"
    elif metric.endswith("_allocations"):
        key = "allocations"
    else:
        return []
    return [float(it[key]) for it in iters if key in it]


def _noise_band(cur_samples: list[float], tgt_samples: list[float],
                k: float) -> float:
    """k-sigma absolute noise band from per-iteration spread.

    Uses the larger of the two per-side sample stdevs (conservative). Returns
    0.0 when neither side has enough samples to estimate stdev — in that
    deterministic case any non-zero delta is taken as a real change.
    """
    sd_cur = statistics.stdev(cur_samples) if len(cur_samples) >= 2 else 0.0
    sd_tgt = statistics.stdev(tgt_samples) if len(tgt_samples) >= 2 else 0.0
    return k * max(sd_cur, sd_tgt)


def classify_delta(cv, tv, noise_band: float) -> str:
    """Returns 'noise', 'regression', 'improvement', or 'flat'."""
    if cv is None or tv is None:
        return 'flat'
    abs_delta = abs(cv - tv)
    if abs_delta <= noise_band:
        return 'noise'
    if cv > tv:
        return 'regression'
    if cv < tv:
        return 'improvement'
    return 'flat'


def color_delta(pct: Optional[float], classification: str) -> str:
    if pct is None:
        return dim("-")
    sign = "+" if pct >= 0 else ""
    text = f"{sign}{pct:.2f}%"
    if classification == 'regression':
        return red(text)
    if classification == 'improvement':
        return green(text)
    return dim(text)


def render_comparison(current_report: dict, target_report: dict,
                      current_label: str, target_label: str,
                      current_failed: list[str], target_failed: list[str],
                      summary_pct_floor: float, noise_sigma: float) -> None:
    cur_scen = {s["scenario"]: s for s in current_report.get("scenarios", [])}
    tgt_scen = {s["scenario"]: s for s in target_report.get("scenarios", [])}
    scenarios = sorted(set(cur_scen) | set(tgt_scen))

    only_current = [s for s in scenarios if s in cur_scen and s not in tgt_scen]
    only_target = [s for s in scenarios if s in tgt_scen and s not in cur_scen]

    header = [
        bold("scenario"),
        bold("metric"),
        bold(f"current ({current_label})"),
        bold(f"target ({target_label})"),
        bold("delta"),
    ]
    rows: list[list[str]] = [header]
    row_groups: list[Optional[str]] = [None]
    regressions: list[tuple[str, str, float]] = []
    improvements: list[tuple[str, str, float]] = []

    for scenario in scenarios:
        cur_s = cur_scen.get(scenario)
        tgt_s = tgt_scen.get(scenario)
        for metric, unit in METRICS:
            cv = (cur_s or {}).get("metrics", {}).get(metric)
            tv = (tgt_s or {}).get("metrics", {}).get(metric)
            pct = delta_pct(cv, tv)
            band = _noise_band(
                _samples_for_metric(cur_s, metric),
                _samples_for_metric(tgt_s, metric),
                noise_sigma,
            )
            cls = classify_delta(cv, tv, band)
            rows.append([
                scenario,
                metric,
                fmt_value(cv, unit),
                fmt_value(tv, unit),
                color_delta(pct, cls),
            ])
            row_groups.append(scenario)
            if pct is None:
                continue
            if cls == 'regression' and abs(pct) >= summary_pct_floor:
                regressions.append((scenario, metric, pct))
            elif cls == 'improvement' and abs(pct) >= summary_pct_floor:
                improvements.append((scenario, metric, pct))

    widths = [max(visible_len(str(r[i])) for r in rows) for i in range(len(header))]

    def hr(char: str = "-") -> str:
        return "+" + "+".join(char * (w + 2) for w in widths) + "+"

    def fmt_row(row: list[str]) -> str:
        cells = []
        for i, value in enumerate(row):
            s = str(value)
            pad = widths[i] - visible_len(s)
            if i < 2:
                cells.append(" " + s + " " * pad + " ")
            else:
                cells.append(" " + " " * pad + s + " ")
        return "|" + "|".join(cells) + "|"

    print(hr("="))
    print(fmt_row(rows[0]))
    print(hr("="))
    last_group: Optional[str] = None
    for row, group in zip(rows[1:], row_groups[1:]):
        if last_group is not None and group != last_group:
            print(hr())
        print(fmt_row(row))
        last_group = group
    print(hr("="))

    if only_current:
        print()
        print(bold("Scenarios only present in current run:"))
        for name in only_current:
            print(f"  {yellow('+')} {name}")
    if only_target:
        print()
        print(bold("Scenarios only present in target run:"))
        for name in only_target:
            print(f"  {yellow('+')} {name}")
    if current_failed:
        print()
        print(bold(f"Scenarios that THREW on current ({current_label}):"))
        for name in current_failed:
            print(f"  {red('x')} {name}")
    if target_failed:
        print()
        print(bold(f"Scenarios that THREW on target ({target_label}):"))
        for name in target_failed:
            print(f"  {red('x')} {name}")

    print()
    print(dim(f"deltas dimmed when within {noise_sigma:g}σ of per-iteration "
              f"sample spread (max(stdev_cur, stdev_tgt))"))
    if summary_pct_floor > 0:
        print(dim(f"summary lists below also require |delta| ≥ "
                  f"{summary_pct_floor:g}%"))
    print()
    if regressions:
        print(bold(red(f"Regressions (outside {noise_sigma:g}σ noise band):")))
        for scenario, metric, pct in sorted(regressions, key=lambda x: -x[2]):
            print(f"  {red(f'+{pct:>8.2f}%')}  {scenario}  {dim(metric)}")
    else:
        print(green(f"No metric regressed beyond the {noise_sigma:g}σ noise band."))

    if improvements:
        print()
        print(bold(green(f"Improvements (outside {noise_sigma:g}σ noise band):")))
        for scenario, metric, pct in sorted(improvements, key=lambda x: x[2]):
            print(f"  {green(f'{pct:>9.2f}%')}  {scenario}  {dim(metric)}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare perf scenario metrics between current working "
                    "tree and a target commit.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("commit", nargs="?",
                        help="Target commit SHA (default: main HEAD)")
    args = parser.parse_args()

    iterations = int(os.environ.get("ITERATIONS", "10"))
    scenarios_filter = os.environ.get("SCENARIOS", "").strip()
    summary_pct_floor = float(os.environ.get("REGRESS_PCT", "5"))
    noise_sigma = float(os.environ.get("NOISE_SIGMA", "2"))
    keep = os.environ.get("KEEP", "0") == "1"

    root = repo_root()
    os.chdir(root)

    if args.commit:
        target_sha = resolve_commit(args.commit)
        if not target_sha:
            die(f"{args.commit!r} does not resolve to a commit")
    else:
        target_sha = resolve_default_target()
    target_short = short_sha(target_sha)

    current_short = short_sha(git("rev-parse", "HEAD"))
    current_label = current_short + ("+dirty" if is_dirty(root) else "")

    out_dir = Path(tempfile.mkdtemp(prefix="microide-perf-compare-"))
    target_worktree_dir: Optional[Path] = None
    current_worktree_dir: Optional[Path] = None

    try:
        log(None, f"current = {bold(current_label)}")
        log(None, f"target  = {bold(target_short)} ({target_sha})")
        runner = detect_runner()

        current_worktree_dir = prepare_current_worktree(root, out_dir)
        mirror_fixtures(root, current_worktree_dir, "current")
        build_side(current_worktree_dir, "current", out_dir / "current.build.log")

        target_worktree_dir = out_dir / "wt-target"
        log("target", dim(f"preparing worktree at {target_worktree_dir}"))
        subprocess.run(
            ["git", "worktree", "add", "--detach", str(target_worktree_dir), target_sha],
            check=True, capture_output=True,
        )
        mirror_fixtures(root, target_worktree_dir, "target")
        build_side(target_worktree_dir, "target", out_dir / "target.build.log")

        if scenarios_filter:
            scenarios = [s for s in scenarios_filter.split(",") if s]
            log(None, f"scenario set: user filter "
                      f"({bold(str(len(scenarios)))} scenarios)")
        else:
            cur_set = discover_scenarios(current_worktree_dir)
            tgt_set = discover_scenarios(target_worktree_dir)
            seen: set[str] = set()
            scenarios = []
            for s in cur_set + tgt_set:
                if s not in seen:
                    scenarios.append(s)
                    seen.add(s)
            log(None, f"scenario set: {bold(str(len(scenarios)))} scenarios "
                      f"(union of baselines on both sides)")

        if not scenarios:
            die("no scenarios to compare")

        current_json = out_dir / "current.json"
        target_json = out_dir / "target.json"
        current_side = SideRun(current_worktree_dir, "current", out_dir)
        target_side = SideRun(target_worktree_dir, "target", out_dir)
        total = len(scenarios)
        for idx, scenario in enumerate(scenarios, start=1):
            # Measure each scenario in both side orders and merge the per-iteration
            # streams. This avoids the "single-scenario current always runs first"
            # bias that per-scenario alternation could not address.
            first_half = iterations // 2
            second_half = iterations - first_half
            if idx % 2 == 1:
                chunks = [
                    (current_side, first_half, "a"),
                    (target_side, first_half, "a"),
                    (target_side, second_half, "b"),
                    (current_side, second_half, "b"),
                ]
            else:
                chunks = [
                    (target_side, first_half, "a"),
                    (current_side, first_half, "a"),
                    (current_side, second_half, "b"),
                    (target_side, second_half, "b"),
                ]
            for side, chunk_iters, chunk_tag in chunks:
                run_scenario(side, scenario, idx, total, chunk_iters, runner, chunk_tag)

        merge_side_reports(current_side, current_json)
        merge_side_reports(target_side, target_json)
        current_failed = current_side.failed
        target_failed = target_side.failed

        current_report = json.loads(current_json.read_text())
        target_report = json.loads(target_json.read_text())

        render_comparison(
            current_report, target_report,
            current_label, target_short,
            current_failed, target_failed,
            summary_pct_floor, noise_sigma,
        )
        return 0
    finally:
        for wt in (current_worktree_dir, target_worktree_dir):
            if wt and wt.exists():
                subprocess.run(
                    ["git", "worktree", "remove", "--force", str(wt)],
                    check=False, capture_output=True,
                )
        if keep:
            log(None, dim(f"keeping artifacts under {out_dir}"))
        else:
            shutil.rmtree(out_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        die("interrupted", 130)
