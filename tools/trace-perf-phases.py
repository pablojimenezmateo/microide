#!/usr/bin/env python3
"""Run the allocation tracer over every gated perf phase and rank what it finds.

TD-2026-08-06-159 was filed as "63 of the suite's 70 interactive phases have never
been read through the allocation tracer", and every pass over it found something.
The reason it stayed open for months is not that the passes were hard -- it is
that each one was hand-driven: pick a phase, remember the two environment
variables, run it, pipe the offsets through addr2line, read the table. Nobody does
that 122 times.

This does. It reads the phase list out of the committed baselines (so a new
scenario is swept the moment it has a baseline), runs the tracer once per phase,
resolves the top sites, and writes a ranked markdown report.

What to do with the output: the interesting rows are not the biggest phases, they
are the ones whose top site is not the operation the phase is named for. Three of
those were found by hand on 2026-08-12 alone -- a session-record encoder on the
project-switch path, a scenario building 2,048 paths per iteration inside its own
measurement, and the harness's own RSS probe allocating inside every window.

Usage:
  tools/trace-perf-phases.py                      # every phase that allocates
  tools/trace-perf-phases.py --top 25             # the 25 biggest, for a quick pass
  tools/trace-perf-phases.py --scenario merge     # substring filter on scenario name
  tools/trace-perf-phases.py --out report.md

The binary must be the perf lane's:
  cmake --preset microide-perf && cmake --build build/microide-perf-make --target microide_perf
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
BASELINES = REPO / "tests" / "perf" / "baselines"
DEFAULT_BINARY = REPO / "build" / "microide-perf-make" / "microide" / "microide_perf"

# The tracer's own bucket table drops each new site whole once full, so a run that
# overflows prints a correctly-sorted and WRONG table (TD-2026-08-10-178). The
# report carries the warning verbatim rather than paraphrasing it.
OVERFLOW_MARKER = "WARNING"

SITE_RE = re.compile(r"^\[alloctrace\] #(\d+): (\d+) allocations, (\d+) bytes")
OFFSET_RE = re.compile(r"\+0x([0-9a-f]+)\)")


def phases_from_baselines(scenario_filter: str | None) -> list[tuple[str, str, float]]:
    out: list[tuple[str, str, float]] = []
    for path in sorted(BASELINES.glob("*.json")):
        scenario = path.stem
        if scenario_filter and scenario_filter not in scenario:
            continue
        try:
            record = json.loads(path.read_text())
        except json.JSONDecodeError:
            continue
        for phase in record.get("phases", []):
            allocations = phase.get("p50_allocations") or 0
            if allocations > 0:
                out.append((scenario, phase["name"], float(allocations)))
    out.sort(key=lambda row: -row[2])
    return out


def resolve(binary: pathlib.Path, offsets: list[str]) -> list[str]:
    if not offsets:
        return []
    proc = subprocess.run(
        ["addr2line", "-e", str(binary), "-f", "-C", "-p", "-i", *offsets],
        capture_output=True, text=True, check=False,
    )
    lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    # Keep the frames a reader can act on. An inlined libstdc++ stack resolves into
    # pages of variant/vtable mangling that bury the one `microide::` frame naming
    # the call site, so project frames come first and the rest are a fallback for a
    # stack that has none.
    def shorten(line: str) -> str:
        line = re.sub(r"\(.*?\)", "()", line)
        line = re.sub(r"<[^<>]{40,}>", "<...>", line)
        return line[:200]

    project = [shorten(l) for l in lines
               if "microide::" in l and "/usr/include" not in l and not l.startswith("??")]
    if project:
        return project
    return [shorten(l) for l in lines if not l.startswith("??") and "_start" not in l]


def trace_phase(binary: pathlib.Path, scenario: str, phase: str, iterations: int,
                min_bytes: int, timeout: int) -> dict:
    env = dict(os.environ)
    env["MICROIDE_PERF_ALLOC_TRACE"] = f"{min_bytes}:200000000"
    # The filter is a substring of the phase name; use the part after the dot so a
    # scenario whose phases share a prefix still selects exactly one.
    env["MICROIDE_PERF_ALLOC_TRACE_PHASE"] = phase.split(".")[-1]
    try:
        proc = subprocess.run(
            [str(binary), f"--scenarios={scenario}", f"--iterations={iterations}", "--no-isolate"],
            capture_output=True, text=True, env=env, cwd=str(REPO), timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired:
        return {"error": f"timed out after {timeout}s"}

    text = proc.stderr + proc.stdout
    if OVERFLOW_MARKER in text and "alloctrace" in text:
        warnings = [l for l in text.splitlines() if "alloctrace" in l and OVERFLOW_MARKER in l]
    else:
        warnings = []

    sites: list[dict] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = SITE_RE.match(line)
        if not match:
            continue
        offsets = []
        for frame in lines[index + 1: index + 9]:
            hit = OFFSET_RE.search(frame)
            if hit:
                offsets.append("0x" + hit.group(1))
        sites.append({
            "rank": int(match.group(1)),
            "allocations": int(match.group(2)),
            "bytes": int(match.group(3)),
            "frames": resolve(binary, offsets[:4]),
        })
        if len(sites) >= 3:
            break

    total = next((l for l in lines if l.startswith("[alloctrace]") and "distinct sites" in l), "")
    return {"total": total, "sites": sites, "warnings": warnings}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=pathlib.Path, default=DEFAULT_BINARY)
    parser.add_argument("--top", type=int, default=0, help="only the N biggest phases")
    parser.add_argument("--scenario", default=None, help="substring filter on scenario name")
    parser.add_argument("--iterations", type=int, default=2)
    parser.add_argument("--min-bytes", type=int, default=1,
                        help="lower bound of the traced size band")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--out", type=pathlib.Path, default=None)
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"perf binary not found: {args.binary}\n"
              f"build it with: cmake --preset microide-perf && "
              f"cmake --build build/microide-perf-make --target microide_perf", file=sys.stderr)
        return 1

    phases = phases_from_baselines(args.scenario)
    if args.top:
        phases = phases[: args.top]
    if not phases:
        print("no gated phases matched", file=sys.stderr)
        return 1

    report: list[str] = [
        "# Perf phase allocation trace",
        "",
        f"Generated by `tools/trace-perf-phases.py` over {len(phases)} phase(s), "
        f"`--iterations={args.iterations}`.",
        "",
        "Read the TOP SITE column, not the allocation count: a phase whose biggest",
        "allocator is not the operation its name describes is the finding. Counts are",
        "for the whole run (warmup + measured iterations), not per iteration.",
        "",
    ]

    for index, (scenario, phase, baseline_allocations) in enumerate(phases, start=1):
        print(f"[{index}/{len(phases)}] {scenario} :: {phase}", file=sys.stderr)
        result = trace_phase(args.binary, scenario, phase, args.iterations,
                             args.min_bytes, args.timeout)
        report.append(f"## `{phase}`  ({scenario})")
        report.append("")
        report.append(f"baseline p50_allocations: {baseline_allocations:.0f}")
        report.append("")
        if "error" in result:
            report.append(f"**{result['error']}**")
            report.append("")
            continue
        for warning in result.get("warnings", []):
            report.append(f"> {warning}")
            report.append("")
        if result.get("total"):
            report.append(f"`{result['total']}`")
            report.append("")
        if not result["sites"]:
            report.append("_no allocation in the traced band — the phase allocates nothing, "
                          "or the work is on another thread (the tracer is armed per thread)._")
            report.append("")
            continue
        for site in result["sites"]:
            head = site["frames"][0] if site["frames"] else "(unresolved)"
            report.append(f"- **#{site['rank']}** {site['allocations']} allocations, "
                          f"{site['bytes']} bytes — `{head}`")
            for frame in site["frames"][1:3]:
                report.append(f"  - {frame}")
        report.append("")

    text = "\n".join(report) + "\n"
    if args.out:
        args.out.write_text(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
