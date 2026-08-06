#!/usr/bin/env python3
"""Read drift out of microide_perf `--report-json` files.

The perf gate trips on *increases* past a tolerance. That makes one whole class
of failure silent by construction:

  * a scenario that drifts UP but stays inside its envelope passes forever, and
    then gets rebaselined at the drifted value and is green forever
    (TD-2026-08-06-139: five allocation gates, one at +9.4% against +10%);
  * a scenario that got FASTER and was never rebaselined leaves an envelope
    that no longer gates anything (TD-2026-08-05-135: eleven gates 40-80% loose).

Neither is a gate failure. Both are found only by comparing measurements taken at
different times — which is what a dated series of report JSONs is for, and what
this reads.

Usage:
  tools/perf-drift.py REPORT                 # REPORT vs the committed baselines
  tools/perf-drift.py NEWER OLDER            # two dated reports
  tools/perf-drift.py --dir DIR              # newest two reports in DIR

Options:
  --alloc-threshold PCT   flag allocation drift at/above this percent (default 1.0)
  --loose-threshold PCT   flag a baseline this much looser than the measurement
                          (default 20.0)
  --fail-on-drift         exit non-zero when anything is flagged (default: exit
                          non-zero only on a real gate failure recorded in the
                          report)
  --json                  emit machine-readable output instead of a table
  --baselines DIR         committed baseline directory (default tests/perf/baselines)

Exit status:
  0  nothing to report
  1  a gate failed, or --fail-on-drift and something was flagged
  2  bad usage / unreadable input
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent

# Metrics whose value is a deterministic property of the code, not of the
# machine. An allocation count is identical run to run on the same binary, so any
# movement here is a code change and is worth a percent. Everything else carries
# scheduler, governor and allocator state (see TD-2026-08-05-137 for the 1.44x
# per-thread clock swing this box is capable of), so a few percent there is
# nothing.
DETERMINISTIC_PREFIXES = ("p50_allocations", "p95_allocations", "max_allocations")


def is_deterministic(metric: str) -> bool:
    return metric.startswith(DETERMINISTIC_PREFIXES)


# ---------------------------------------------------------------------------
# Colour
# ---------------------------------------------------------------------------

_USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")


def _c(code: str, text: str) -> str:
    return f"\x1b[{code}m{text}\x1b[0m" if _USE_COLOR else text


def red(t: str) -> str:
    return _c("31", t)


def yellow(t: str) -> str:
    return _c("33", t)


def green(t: str) -> str:
    return _c("32", t)


def bold(t: str) -> str:
    return _c("1", t)


def dim(t: str) -> str:
    return _c("2", t)


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------


def load_report(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        sys.exit(f"perf-drift: cannot read {path}: {exc}")
    if "scenarios" not in data:
        sys.exit(f"perf-drift: {path} is not a microide_perf --report-json file")
    return data


def report_metrics(report: dict[str, Any]) -> dict[str, dict[str, float]]:
    """{scenario: {metric: value}} for every scenario in a report."""
    out: dict[str, dict[str, float]] = {}
    for scenario in report["scenarios"]:
        out[scenario["scenario"]] = dict(scenario.get("metrics", {}))
    return out


def report_gate(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """{scenario: baseline-block} for scenarios the run actually gated.

    Present only in reports written after TD-2026-08-06-141; older reports carry
    the measurements but not what they were measured against, so envelope
    consumption cannot be recovered from them.
    """
    out: dict[str, dict[str, Any]] = {}
    for scenario in report["scenarios"]:
        baseline = scenario.get("baseline")
        if isinstance(baseline, dict) and baseline.get("gated"):
            out[scenario["scenario"]] = baseline
    return out


def load_committed_baselines(directory: Path) -> dict[str, dict[str, float]]:
    out: dict[str, dict[str, float]] = {}
    if not directory.is_dir():
        sys.exit(f"perf-drift: no baseline directory at {directory}")
    for path in sorted(directory.glob("*.json")):
        try:
            data = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            sys.exit(f"perf-drift: cannot read baseline {path}: {exc}")
        name = data.get("scenario") or path.stem
        out[name] = dict(data.get("metrics", {}))
        out[name]["__tolerances__"] = data.get("tolerances", {})  # type: ignore[assignment]
    return out


def newest_reports(directory: Path, count: int) -> list[Path]:
    """The `count` newest reports in `directory`, newest first.

    Ordered by the report's own `metadata.timestamp_utc` when it has one, and by
    filename otherwise — never by mtime, which a copy or an rsync rewrites.
    """
    candidates = []
    for path in directory.glob("*.json"):
        if path.name.startswith("."):
            continue
        try:
            data = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        stamp = str(data.get("metadata", {}).get("timestamp_utc") or "")
        candidates.append((stamp, path.name, path))
    candidates.sort(reverse=True)
    return [path for _, _, path in candidates[:count]]


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------


def pct(new: float, old: float) -> float | None:
    if old == 0:
        return None
    return (new / old - 1.0) * 100.0


def tolerance_for(metric: str, tolerances: dict[str, Any]) -> float | None:
    """The envelope a committed baseline records for one metric name."""
    kind = "alloc_" if "allocations" in metric else ""
    if "cpu_ms" in metric:
        kind = "cpu_"
    if "rss" in metric:
        return tolerances.get("rss_mean_percent") if metric.startswith("mean_") else None
    for percentile in ("p50", "p95", "max"):
        if metric.startswith(percentile + "_"):
            return tolerances.get(f"{kind}{percentile}_percent")
    return None


def analyse(
    new_metrics: dict[str, dict[str, float]],
    old_metrics: dict[str, dict[str, float]],
    alloc_threshold: float,
    loose_threshold: float,
) -> dict[str, list[dict[str, Any]]]:
    findings: dict[str, list[dict[str, Any]]] = {
        "alloc_drift_up": [],
        "alloc_drift_down": [],
        "loose_gate": [],
    }
    for scenario in sorted(new_metrics):
        if scenario not in old_metrics:
            continue
        new = new_metrics[scenario]
        old = old_metrics[scenario]
        tolerances = old.get("__tolerances__") or {}
        for metric in sorted(new):
            if metric.startswith("__") or metric not in old:
                continue
            delta = pct(new[metric], old[metric])
            if delta is None:
                continue
            row = {
                "scenario": scenario,
                "metric": metric,
                "old": old[metric],
                "new": new[metric],
                "delta_percent": delta,
            }
            if is_deterministic(metric):
                if delta >= alloc_threshold:
                    findings["alloc_drift_up"].append(row)
                elif delta <= -alloc_threshold:
                    findings["alloc_drift_down"].append(row)
            tolerance = tolerance_for(metric, tolerances) if tolerances else None
            if tolerance is not None and delta <= -loose_threshold:
                row = dict(row)
                row["tolerance_percent"] = tolerance
                # The envelope now spans from the measurement up to
                # old * (1 + tolerance/100): a regression has to clear all of
                # that before the gate says anything.
                row["slack_percent"] = -delta + tolerance
                findings["loose_gate"].append(row)
    for rows in findings.values():
        rows.sort(key=lambda r: abs(r["delta_percent"]), reverse=True)
    return findings


def envelope_pressure(gate: dict[str, dict[str, Any]], notice: float) -> list[dict[str, Any]]:
    """Gated metrics that PASSED while consuming `notice`% or more of their envelope."""
    rows = []
    for scenario, baseline in gate.items():
        for metric in baseline.get("metrics", []):
            if not metric.get("passed", True):
                continue
            used = float(metric.get("envelope_used_percent", 0.0))
            if used >= notice:
                rows.append({"scenario": scenario, **metric, "used": used})
    rows.sort(key=lambda r: r["used"], reverse=True)
    return rows


def gate_failures(gate: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    rows = []
    for scenario, baseline in gate.items():
        for metric in baseline.get("metrics", []):
            if not metric.get("passed", True):
                rows.append({"scenario": scenario, **metric})
    return rows


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


def fmt(value: float) -> str:
    if abs(value) >= 1000 or value == int(value):
        return f"{value:,.0f}"
    return f"{value:.4g}"


def print_rows(rows: list[dict[str, Any]], colour) -> None:
    width = max((len(r["scenario"]) for r in rows), default=0)
    for row in rows:
        delta = row["delta_percent"]
        sign = "+" if delta >= 0 else ""
        line = (
            f"  {row['scenario']:<{width}}  {row['metric']:<24}"
            f"  {fmt(row['old']):>12} -> {fmt(row['new']):>12}"
            f"  {colour(f'{sign}{delta:.2f}%')}"
        )
        if "slack_percent" in row:
            line += dim(
                f"   (envelope now allows +{row['slack_percent']:.0f}%"
                f" before it trips)"
            )
        print(line)


def main() -> int:
    parser = argparse.ArgumentParser(add_help=True, description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reports", nargs="*", type=Path)
    parser.add_argument("--dir", type=Path, default=None)
    parser.add_argument("--baselines", type=Path, default=REPO_ROOT / "tests/perf/baselines")
    parser.add_argument("--alloc-threshold", type=float, default=1.0)
    parser.add_argument("--loose-threshold", type=float, default=20.0)
    parser.add_argument("--envelope-notice", type=float, default=75.0)
    parser.add_argument("--fail-on-drift", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    if args.dir is not None:
        found = newest_reports(args.dir, 2)
        if not found:
            print(f"perf-drift: no reports in {args.dir} yet — nothing to compare.")
            return 0
        args.reports = found
    if not args.reports:
        parser.error("give a report path, or --dir")

    newer = load_report(args.reports[0])
    new_metrics = report_metrics(newer)
    gate = report_gate(newer)

    if len(args.reports) >= 2:
        older = load_report(args.reports[1])
        old_metrics = report_metrics(older)
        baseline_label = str(args.reports[1])
        old_stamp = older.get("metadata", {}).get("timestamp_utc", "(undated)")
    else:
        old_metrics = load_committed_baselines(args.baselines)
        baseline_label = f"committed baselines in {args.baselines}"
        old_stamp = "(as committed)"

    findings = analyse(new_metrics, old_metrics, args.alloc_threshold, args.loose_threshold)
    pressure = envelope_pressure(gate, args.envelope_notice)
    failures = gate_failures(gate)

    if args.json:
        json.dump(
            {
                "newer": str(args.reports[0]),
                "older": baseline_label,
                "findings": findings,
                "envelope_pressure": pressure,
                "gate_failures": failures,
            },
            sys.stdout,
            indent=2,
        )
        print()
    else:
        new_stamp = newer.get("metadata", {}).get("timestamp_utc", "(undated)")
        print(bold("microide perf drift"))
        print(f"  newer: {args.reports[0]}  {new_stamp}")
        print(f"  older: {baseline_label}  {old_stamp}")
        print()

        if failures:
            print(red(bold(f"GATE FAILURES ({len(failures)})")))
            for row in failures:
                print(
                    f"  {row['scenario']}  {row['metric']}: "
                    f"baseline={fmt(row['expected'])} measured={fmt(row['actual'])} "
                    f"({row['delta_percent']:+.2f}% of +{row['tolerance_percent']:.0f}% allowed)"
                )
            print()

        if findings["alloc_drift_up"]:
            print(red(bold(f"ALLOCATION DRIFT UP ({len(findings['alloc_drift_up'])})")))
            print(dim("  Allocation counts are deterministic. Every row here is a code change,"))
            print(dim("  whether or not it tripped a gate. This is the TD-2026-08-06-139 class."))
            print_rows(findings["alloc_drift_up"], red)
            print()

        if findings["alloc_drift_down"]:
            print(green(bold(f"allocation drift down ({len(findings['alloc_drift_down'])})")))
            print(dim("  Real improvements. Rebaseline them or the gate keeps the old slack."))
            print_rows(findings["alloc_drift_down"], green)
            print()

        if findings["loose_gate"]:
            print(yellow(bold(f"LOOSE GATES ({len(findings['loose_gate'])})")))
            print(dim("  The measurement sits far below the baseline, so the envelope spans"))
            print(dim("  ground the code already gave up. TD-2026-08-05-135 class."))
            print_rows(findings["loose_gate"], yellow)
            print()

        if pressure:
            print(yellow(bold(f"ENVELOPE PRESSURE ({len(pressure)})")))
            print(dim("  Gates that PASSED while consuming most of their tolerance."))
            for row in pressure:
                print(
                    f"  {row['scenario']}  {row['metric']}: "
                    f"{row['delta_percent']:+.2f}% of +{row['tolerance_percent']:.0f}% allowed"
                    f"  = {row['used']:.0f}% of envelope"
                )
            print()
        elif gate:
            print(dim("  no gated metric consumed "
                      f">={args.envelope_notice:.0f}% of its envelope"))
            print()

        flagged = sum(len(v) for v in findings.values()) + len(pressure)
        if not failures and flagged == 0:
            print(green("no drift to report"))

    if failures:
        return 1
    if args.fail_on_drift and (
        findings["alloc_drift_up"] or findings["loose_gate"] or pressure
    ):
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
