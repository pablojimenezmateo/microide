#!/usr/bin/env python3
"""Difference two releases' committed perf baselines, refusing incomparable pairs.

    tools/perf-release-diff.py v2.8.1                 # that tag vs the working tree
    tools/perf-release-diff.py v2.8.1 v2.9.0          # two tags
    tools/perf-release-diff.py v2.9.0 --json

Why this exists (TD-2026-08-07-167). Cutting a release needs one number: how much
faster is this than the last one. The obvious way to get it — diff
`p50_allocations` across two tags' `tests/perf/baselines/` — is wrong whenever a
scenario changed what it *does*, and there is no way to see that in the numbers.
For v2.9.0 it reported three regressions and all three were artifacts: two
terminal scenarios that used to scroll an empty buffer and now feed 4,000 lines
through the emulator, and a git scenario whose harness regime changed underneath
it. The changelog worked around it by naming the exclusions in prose, which is a
fact that survives exactly one release.

A baseline now carries `measurement_revision` (see `Scenario::measurement_revision`).
This refuses to difference two baselines whose revisions differ and reports them
as NOT COMPARABLE instead of as a phantom regression. Absent means 1, which is
correct for every baseline written before the field existed.

Duration metrics get the same treatment for a different reason: they are only
comparable when both sides recorded `p50_cpu_calibration_ns`, the harness's
fixed-work clock probe. Baselines older than that field cannot be normalised for
the machine's clock state at all, so wall/cpu are reported as uncomparable rather
than differenced (this is why the v2.9.0 note had to lead on allocations).

Exit status: 0 always — this is a reporting tool, not a gate.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

BASELINE_DIR = "tests/perf/baselines"
# Same ceiling the harness applies (Baseline.h kMaxClockNormalizationFactor): a
# probe reading 3x out is a different machine class or a preempted probe, and
# scaling by it would make the comparison meaningless in a way that reads as a
# result.
MAX_CLOCK_FACTOR = 3.0


def read_ref(ref: str | None) -> dict[str, dict]:
    """Every baseline at `ref`, keyed by scenario name. None means the working tree."""
    out: dict[str, dict] = {}
    if ref is None:
        for path in sorted(Path(BASELINE_DIR).glob("*.json")):
            out[path.stem] = json.loads(path.read_text(encoding="utf-8"))
        return out
    listing = subprocess.run(
        ["git", "ls-tree", "--name-only", f"{ref}:{BASELINE_DIR}"],
        check=True, stdout=subprocess.PIPE, text=True,
    ).stdout.split()
    for name in listing:
        if not name.endswith(".json"):
            continue
        blob = subprocess.run(
            ["git", "show", f"{ref}:{BASELINE_DIR}/{name}"],
            check=True, stdout=subprocess.PIPE, text=True,
        ).stdout
        out[name[: -len(".json")]] = json.loads(blob)
    return out


def revision(record: dict) -> int:
    value = record.get("measurement_revision", 1)
    return int(value) if isinstance(value, (int, float)) and value >= 1 else 1


def metric(record: dict, name: str) -> float | None:
    value = record.get("metrics", {}).get(name)
    return float(value) if isinstance(value, (int, float)) else None


def pct(old: float, new: float) -> float | None:
    return None if old == 0.0 else (new - old) / old * 100.0


def compare(old: dict, new: dict) -> dict:
    """One scenario's verdict: a delta per metric, or a reason it has none."""
    result: dict = {"comparable": True, "reason": "", "deltas": {}}
    if revision(old) != revision(new):
        result["comparable"] = False
        result["reason"] = (
            f"measurement_revision {revision(old)} -> {revision(new)}: "
            "the scenario changed what it measures"
        )
        return result

    for name in ("p50_allocations", "p95_allocations", "max_allocations", "p50_net_heap_bytes"):
        a, b = metric(old, name), metric(new, name)
        if a is None or b is None:
            continue
        delta = pct(a, b)
        if delta is not None:
            result["deltas"][name] = {"old": a, "new": b, "percent": delta}

    # Durations only when both sides can be put in the same machine state.
    calib_old, calib_new = metric(old, "p50_cpu_calibration_ns"), metric(new, "p50_cpu_calibration_ns")
    if not calib_old or not calib_new:
        result["duration_reason"] = (
            "no p50_cpu_calibration_ns on "
            + ("both sides" if not calib_old and not calib_new else "one side")
            + ": duration cannot be normalised for the machine clock (TD-2026-08-05-137)"
        )
        return result
    factor = calib_new / calib_old
    if factor > MAX_CLOCK_FACTOR or factor < 1.0 / MAX_CLOCK_FACTOR:
        result["duration_reason"] = (
            f"clock probe differs by {factor:.2f}x, past the {MAX_CLOCK_FACTOR}x sanity "
            "clamp: these are not the same reference lane"
        )
        return result
    for name in ("p50_wall_ms", "p50_cpu_ms"):
        a, b = metric(old, name), metric(new, name)
        if a is None or b is None:
            continue
        normalised = b / factor
        delta = pct(a, normalised)
        if delta is not None:
            result["deltas"][name] = {"old": a, "new": normalised, "percent": delta,
                                      "clock_factor": factor}
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("old_ref", help="git ref (tag/commit) holding the older baselines")
    parser.add_argument("new_ref", nargs="?", default=None,
                        help="git ref holding the newer baselines (default: working tree)")
    parser.add_argument("--metric", default="p50_allocations",
                        help="metric to rank the table by (default: p50_allocations)")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args()

    old_all = read_ref(args.old_ref)
    new_all = read_ref(args.new_ref)
    new_label = args.new_ref or "worktree"

    shared = sorted(set(old_all) & set(new_all))
    added = sorted(set(new_all) - set(old_all))
    removed = sorted(set(old_all) - set(new_all))

    results = {name: compare(old_all[name], new_all[name]) for name in shared}
    incomparable = {n: r for n, r in results.items() if not r["comparable"]}
    comparable = {n: r for n, r in results.items() if r["comparable"]}

    if args.json:
        json.dump({"old": args.old_ref, "new": new_label, "scenarios": results,
                   "added": added, "removed": removed}, sys.stdout, indent=2)
        print()
        return 0

    rows = [(n, r["deltas"][args.metric]) for n, r in comparable.items()
            if args.metric in r["deltas"]]
    rows.sort(key=lambda row: row[1]["percent"])

    print(f"{args.old_ref} -> {new_label}, ranked by {args.metric}\n")
    if rows:
        width = max(len(n) for n, _ in rows)
        for name, d in rows:
            sign = "+" if d["percent"] >= 0 else ""
            print(f"  {name:<{width}}  {d['old']:>12,.0f} -> {d['new']:>12,.0f}"
                  f"  {sign}{d['percent']:.1f}%")
        total_old = sum(d["old"] for _, d in rows)
        total_new = sum(d["new"] for _, d in rows)
        overall = pct(total_old, total_new)
        print(f"\n  {len(rows)} comparable scenarios, summed {args.metric}: "
              f"{total_old:,.0f} -> {total_new:,.0f} "
              f"({'+' if overall and overall >= 0 else ''}{overall:.1f}%)")
    else:
        print(f"  no scenario has {args.metric} on both sides")

    # The whole point of the tool: say what was excluded, out loud, every time.
    if incomparable:
        print(f"\n  {len(incomparable)} scenario(s) NOT COMPARABLE — excluded from the totals:")
        for name, r in sorted(incomparable.items()):
            print(f"    {name}: {r['reason']}")
    duration_blocked = {n: r["duration_reason"] for n, r in comparable.items()
                        if "duration_reason" in r}
    if duration_blocked:
        reasons = sorted(set(duration_blocked.values()))
        print(f"\n  {len(duration_blocked)} scenario(s) have no comparable DURATION metric:")
        for reason in reasons:
            count = sum(1 for r in duration_blocked.values() if r == reason)
            print(f"    {count}x {reason}")
    if added:
        print(f"\n  {len(added)} scenario(s) added since {args.old_ref}: {', '.join(added[:8])}"
              + (", ..." if len(added) > 8 else ""))
    if removed:
        print(f"\n  {len(removed)} scenario(s) removed since {args.old_ref}: "
              f"{', '.join(removed[:8])}" + (", ..." if len(removed) > 8 else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
