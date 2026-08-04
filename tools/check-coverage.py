#!/usr/bin/env python3
"""Aggregate an llvm-cov report by source area and enforce per-area floors.

Why per-area and not one number: the tree measured 74.4% overall on the day
WorkspaceShellRenderMerge.cpp had 568 lines and 0 of 11 functions ever executed.
A global target is comfortably met by workspace/registries at 96% while the
surface the product is built around is dark. Floors per area make a specific area
rotting visible; a global number hides it by construction.

Usage:
  llvm-cov report <binary> -instr-profile=<profdata> \
      -ignore-filename-regex='(tests/|third_party/|/usr/)' > report.txt
  tools/check-coverage.py report.txt [--floors tools/coverage-floors.json]
                                     [--update-floors]

Exits non-zero when any area (or the total) is under its floor.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys


def area_for(path: str) -> str:
    """Group a source path into the same buckets the repo's own docs use.

    workspace/ is split one level deeper because it is ~58% of the tree; a single
    "workspace" bucket would be almost as blind as a global number.
    """
    parts = path.split("/")
    if parts[0] == "workspace" and len(parts) > 2:
        return "/".join(parts[:2])
    if parts[0] == "workspace":
        return "workspace"
    return parts[0]


def parse_report(text: str) -> dict[str, tuple[int, int]]:
    """Return {area: (total_lines, missed_lines)}.

    llvm-cov's table is whitespace-aligned with a fixed column order:
      filename regions missed_regions cover functions missed_fns executed
      lines missed_lines cover branches missed_branches cover
    """
    agg: dict[str, list[int]] = collections.defaultdict(lambda: [0, 0])
    seen_any = False
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 10:
            continue
        name = parts[0]
        if not name.endswith((".cpp", ".h", ".inc", ".hpp")):
            continue
        try:
            total, missed = int(parts[7]), int(parts[8])
        except ValueError:
            continue
        seen_any = True
        bucket = agg[area_for(name)]
        bucket[0] += total
        bucket[1] += missed

    if not seen_any:
        # A report whose rows never matched means the format moved under us. Fail
        # loudly: silently reporting "no areas under floor" would be a check that
        # cannot fail, which is worse than no check.
        raise SystemExit(
            "check-coverage: parsed no per-file rows out of the llvm-cov report.\n"
            "The report format likely changed; refusing to report a vacuous pass."
        )
    return {area: (total, missed) for area, (total, missed) in agg.items()}


def percent(total: int, missed: int) -> float:
    return 100.0 if total == 0 else 100.0 * (total - missed) / total


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument(
        "--floors",
        type=pathlib.Path,
        default=pathlib.Path(__file__).parent / "coverage-floors.json",
    )
    parser.add_argument(
        "--update-floors",
        action="store_true",
        help="rewrite the floors file from this run (review the diff before committing)",
    )
    args = parser.parse_args()

    measured = parse_report(args.report.read_text())
    config = json.loads(args.floors.read_text())
    floors: dict[str, float] = config["areas"]

    grand_total = sum(t for t, _ in measured.values())
    grand_missed = sum(m for _, m in measured.values())
    total_pct = percent(grand_total, grand_missed)

    if args.update_floors:
        # Record a couple of points of slack so ordinary churn does not trip a
        # floor; a real regression is much larger than that.
        config["areas"] = {
            area: round(max(0.0, percent(*measured[area]) - 2.0), 1)
            for area in sorted(measured)
        }
        config["total"] = round(max(0.0, total_pct - 2.0), 1)
        args.floors.write_text(json.dumps(config, indent=2) + "\n")
        print(f"check-coverage: rewrote {args.floors} from this run")
        return 0

    failures: list[str] = []
    print(f"{'area':<34}{'lines':>9}{'uncovered':>11}{'cov%':>8}{'floor':>8}")
    for area in sorted(measured):
        total, missed = measured[area]
        pct = percent(total, missed)
        floor = floors.get(area)
        floor_text = "-" if floor is None else f"{floor:.1f}"
        marker = ""
        if floor is not None and pct + 1e-9 < floor:
            marker = "  <-- BELOW FLOOR"
            failures.append(f"{area}: {pct:.2f}% < {floor:.1f}%")
        print(f"{area:<34}{total:>9}{missed:>11}{pct:>7.1f}%{floor_text:>8}{marker}")

    total_floor = float(config.get("total", 0.0))
    print(f"{'TOTAL':<34}{grand_total:>9}{grand_missed:>11}{total_pct:>7.1f}%{total_floor:>8.1f}")
    if total_pct + 1e-9 < total_floor:
        failures.append(f"TOTAL: {total_pct:.2f}% < {total_floor:.1f}%")

    # An area that vanished from the report is not a pass. It usually means files
    # moved and the floor now names nothing — the same stale-path failure mode the
    # architecture lint guards against with ReadRuleTarget.
    for area in sorted(set(floors) - set(measured)):
        failures.append(f"{area}: has a floor but produced no rows (renamed or removed?)")

    if failures:
        print("\ncheck-coverage: FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(
            "\nRaise coverage, or lower the floor in "
            f"{args.floors.name} with a reason in the commit message.",
            file=sys.stderr,
        )
        return 1

    print("\ncheck-coverage: all areas at or above their floor")
    return 0


if __name__ == "__main__":
    sys.exit(main())
