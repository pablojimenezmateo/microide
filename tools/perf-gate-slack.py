#!/usr/bin/env python3
"""How much slack does every deterministic perf gate have against what it gates?

An allocation gate is ONE-SIDED: it fails on an increase and says nothing when the
measurement collapses. So a phase that stops doing its work reports a smaller
number forever and stays green -- and a baseline recorded before an optimization
stays green at whatever multiple of the real number it was recorded at, gating
nothing. Neither shows up in a run's verdict lines, which is why this exists.

The suite has now been bitten by the collapse case four times
(TD-2026-08-10-170, TD-2026-08-10-179, TD-2026-08-15-253, TD-2026-08-17-258); the
last was a phase reading 3 allocations against a baseline of 10,980 while
reporting PASS. Run this after any pass that removes allocations, and read the
extremes rather than the list:

  - a gate loose by a small factor is a rebaseline (`--update-baseline=deterministic`)
  - a gate loose by orders of magnitude is a scenario that stopped measuring, and
    rebaselining it enshrines that

Usage:
  ./build/microide-perf-make/microide/microide_perf --report-json=/tmp/perf.json
  tools/perf-gate-slack.py /tmp/perf.json
"""
import json, pathlib, sys

report = json.loads(pathlib.Path(sys.argv[1]).read_text())
base_dir = pathlib.Path("tests/perf/baselines")

rows = []
for scenario in report.get("scenarios", []):
    name = scenario["scenario"]
    bp = base_dir / f"{name}.json"
    if not bp.exists():
        continue
    baseline = json.loads(bp.read_text())
    b_alloc = baseline["metrics"].get("p50_allocations", 0)
    m_alloc = scenario["metrics"].get("p50_allocations", 0)
    if b_alloc and m_alloc:
        rows.append((b_alloc / m_alloc, name, "(scenario total)", b_alloc, m_alloc))
    b_phases = {p["name"]: p for p in baseline.get("phases", [])}
    for phase in scenario.get("phases", []):
        b = b_phases.get(phase["name"])
        if not b:
            continue
        ba, ma = b.get("p50_allocations", 0), phase.get("p50_allocations", 0)
        if ba and ma:
            rows.append((ba / ma, name, phase["name"], ba, ma))

rows.sort(reverse=True)
print(f"{'slack':>8}  {'baseline':>9}  {'measured':>9}  gate")
loose = 0
for ratio, scenario, phase, ba, ma in rows:
    if ratio >= 1.5:
        loose += 1
    if ratio >= 1.5 or ratio < 0.95:
        print(f"{ratio:>7.1f}x  {ba:>9.0f}  {ma:>9.0f}  {scenario}::{phase}")
print(f"\n{loose} of {len(rows)} allocation gates are >= 1.5x looser than the code they gate")
tight = [r for r in rows if r[0] < 1.1]
print(f"{len(tight)} are within 10% of their measurement (nothing to reclaim there)")
