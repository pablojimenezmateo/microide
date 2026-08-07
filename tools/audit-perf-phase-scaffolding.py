#!/usr/bin/env python3
"""Measure how much of each perf phase gate is the scenario's own scaffolding.

TD-2026-08-07-163. `plugin_status_item_update` measured 172,005 allocations and
95 % of them were the *measured loop* composing its own input string, against a
product function that allocates none. A doubling of the thing that gate is named
after could not have moved it out of tolerance: green, and blind.

This walks every phase in `tests/perf/baselines/*.json`, runs it under the
allocation-site tracer, resolves each site's stack with addr2line, and buckets
the site by whether the innermost frame that lands in this repository is under
`src/` (product) or under `tests/` (scaffolding). A site whose stack LTO
flattened into the harness's own `Measure` frame is reported as unattributed
rather than charged to either bucket.

    tools/audit-perf-phase-scaffolding.py                       # whole suite
    tools/audit-perf-phase-scaffolding.py --phase toggle_line   # substring filter
    tools/audit-perf-phase-scaffolding.py --json out.json --markdown out.md

Machine state does not affect the result — allocation counts are deterministic —
so unlike a timing pass this needs no idle runner.

Reading the output: a high scaffolding share is not automatically a defect. The
pure-unit scenarios (`user_config_record_decode`, `dap_protocol_encode_decode`,
`lsp_*_parse`, …) legitimately measure construction, because construction IS the
thing under test. The finding is a phase where the scenario builds a
*convenience* value — a formatted id, a path string, a fixture line — that the
product would have had in hand already.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = REPO / "build" / "microide-perf-make" / "microide" / "microide_perf"

SITE_HEADER = re.compile(r"^\[alloctrace\] #(\d+): (\d+) allocations, (\d+) bytes$")
TOTAL_HEADER = re.compile(r"^\[alloctrace\] (\d+) allocations in \[")
FRAME = re.compile(r"\(\+0x([0-9a-f]+)\)\[0x[0-9a-f]+\]$")

# The tracer's own operator new frame and the harness plumbing that arms it are
# neither product nor scaffolding-under-test; they are the instrument.
INSTRUMENT_FILES = ("tests/perf/AllocationCounter.cpp",)

# The measurement boundary. EVERY allocation the phase makes is nested inside
# `ScenarioContext::Measure`, so resolving to it is not an attribution — it means
# the whole lambda body was flattened into it and addr2line kept no inline record
# of what was really running (LTO does this to `.isra`/`.constprop` clones). A
# header-defined product template instantiated in the scenario TU is the case
# that matters: `assist_merge::RankedUnion` resolved here and read as "100 %
# scaffolding" when it is 100 % product. Bucket it as unattributed and say so,
# rather than charging it to whichever frame happens to be next on the stack.
BOUNDARY_SYMBOLS = (
    "microide::tests::perf::ScenarioContext::Measure",
    "microide::tests::perf::PerfHarness::RunScenario",
)


class Addr2Line:
    """Batched, cached addr2line. Offsets are static, so one cache spans the run."""

    def __init__(self, binary: Path) -> None:
        self.binary = binary
        self.cache: dict[str, list[tuple[str, str]]] = {}

    def resolve(self, offsets: list[str]) -> None:
        missing = [o for o in offsets if o not in self.cache]
        if not missing:
            return
        # addr2line -i emits a variable number of lines per address, so ask one
        # address at a time would be correct but slow; instead use a sentinel
        # address between batches. Simpler and still fast: chunk and re-run with
        # -i per address only where the batch is ambiguous. In practice the
        # ambiguity is unavoidable, so resolve one address per invocation but
        # keep them all in one process via a single call per chunk of one.
        for offset in missing:
            out = subprocess.run(
                ["addr2line", "-e", str(self.binary), "-f", "-C", "-p", "-i", "0x" + offset],
                capture_output=True,
                text=True,
                check=False,
            ).stdout
            self.cache[offset] = _parse_addr2line(out)


def _parse_addr2line(text: str) -> list[tuple[str, str]]:
    """(function, file) pairs, innermost inline frame first."""
    entries: list[tuple[str, str]] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("(inlined by) "):
            line = line[len("(inlined by) ") :]
        # `-p` formats as "<func> at <file>:<line>"; the function name itself can
        # contain " at " inside template arguments, so split from the right.
        head, sep, tail = line.rpartition(" at ")
        if not sep:
            entries.append((line, ""))
            continue
        path = tail.rsplit(":", 1)[0]
        entries.append((head, path))
    return entries


def classify(stack: list[str], resolver: Addr2Line) -> tuple[str, str]:
    """(bucket, attribution) for one captured stack.

    bucket is 'product' | 'scaffolding' | 'unattributed'; attribution is the
    file:function that decided it, for the report.
    """
    resolver.resolve(stack)
    for offset in stack:
        for func, path in resolver.cache.get(offset, []):
            if any(path.endswith(name) for name in INSTRUMENT_FILES):
                continue
            if func.startswith(BOUNDARY_SYMBOLS):
                return "unattributed", _short(func)
            rel = _repo_relative(path) if path else None
            if rel is not None and rel.startswith("src/"):
                return "product", f"{rel} {_short(func)}"
            if rel is not None and rel.startswith("tests/"):
                return "scaffolding", f"{rel} {_short(func)}"
            if rel is not None:
                continue
            # LTO drops the file/line for some inlined bodies but keeps the
            # symbol, and the symbol is the better signal anyway: a `microide::`
            # name is this repository's code no matter which file addr2line
            # thinks it came from. Without this fallback the walk skips straight
            # past the product frame and lands on the harness that called it,
            # which reads as 100 % scaffolding for a phase that is 100 % product.
            if func.startswith("microide::tests::"):
                return "scaffolding", _short(func)
            if func.startswith("microide::"):
                return "product", _short(func)
    return "unattributed", ""


def _repo_relative(path: str) -> str | None:
    # Only absolute paths: addr2line writes "??" for an unknown file, and
    # resolving that against the cwd (which IS the repo) would fabricate a hit.
    if not path.startswith("/"):
        return None
    try:
        return str(Path(path).resolve().relative_to(REPO))
    except (ValueError, OSError):
        return None


def _short(func: str) -> str:
    """Drop template arguments and parameter lists — these names are enormous."""
    depth = 0
    out: list[str] = []
    for ch in func:
        if ch in "<(":
            depth += 1
            continue
        if ch in ">)":
            depth = max(0, depth - 1)
            continue
        if depth == 0:
            out.append(ch)
    return "".join(out).strip() or func[:60]


def parse_trace(stderr: str) -> tuple[int, list[tuple[int, int, list[str]]]]:
    """(total allocations, [(count, bytes, stack offsets)])."""
    total = 0
    sites: list[tuple[int, int, list[str]]] = []
    current: list[str] | None = None
    count = size = 0
    for line in stderr.splitlines():
        match = TOTAL_HEADER.match(line)
        if match:
            total = int(match.group(1))
            continue
        match = SITE_HEADER.match(line)
        if match:
            if current is not None:
                sites.append((count, size, current))
            count, size = int(match.group(2)), int(match.group(3))
            current = []
            continue
        if current is None:
            continue
        frame = FRAME.search(line)
        if frame:
            current.append(frame.group(1))
        elif line.startswith("[alloctrace]"):
            sites.append((count, size, current))
            current = None
    if current is not None:
        sites.append((count, size, current))
    return total, sites


def phases_from_baselines(baselines: Path) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    for path in sorted(baselines.glob("*.json")):
        data = json.loads(path.read_text())
        scenario = data.get("scenario") or path.stem
        for phase in data.get("phases", []):
            pairs.append((scenario, phase["name"]))
    return pairs


def audit_phase(binary: Path, scenario: str, phase: str, iterations: int,
                resolver: Addr2Line, timeout: int) -> dict:
    env = dict(os.environ)
    env["MICROIDE_PERF_ALLOC_TRACE"] = "1:100000000"
    env["MICROIDE_PERF_ALLOC_TRACE_PHASE"] = phase
    env["MICROIDE_PERF_ALLOC_TRACE_SITES"] = "1024"
    try:
        proc = subprocess.run(
            [str(binary), f"--scenarios={scenario}", f"--iterations={iterations}", "--no-isolate"],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {"scenario": scenario, "phase": phase, "error": "timeout"}

    stderr = proc.stderr
    if "no measured phase name contained" in stderr:
        return {"scenario": scenario, "phase": phase, "error": "filter never matched"}
    total, sites = parse_trace(stderr)
    if total == 0 and not sites:
        return {"scenario": scenario, "phase": phase, "total": 0, "classified": 0,
                "scaffolding": 0, "product": 0, "unattributed": 0, "share": 0.0, "top": []}

    buckets = {"product": 0, "scaffolding": 0, "unattributed": 0}
    attributed: dict[tuple[str, str], int] = {}
    for count, _size, stack in sites:
        bucket, attribution = classify(stack, resolver)
        buckets[bucket] += count
        key = (bucket, attribution)
        attributed[key] = attributed.get(key, 0) + count

    # The share is over what could be attributed: an unattributed site is not
    # evidence either way, and folding it into the denominator would quietly
    # dilute a real finding.
    classified = buckets["product"] + buckets["scaffolding"]
    share = buckets["scaffolding"] / classified if classified else 0.0
    top = [
        {"bucket": bucket, "site": attribution, "allocations": count}
        for (bucket, attribution), count in
        sorted(attributed.items(), key=lambda kv: -kv[1])[:6]
    ]
    return {
        "scenario": scenario,
        "phase": phase,
        "total": total,
        "classified": classified,
        "scaffolding": buckets["scaffolding"],
        "product": buckets["product"],
        "unattributed": buckets["unattributed"],
        "share": share,
        "top": top,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--baselines", type=Path, default=REPO / "tests" / "perf" / "baselines")
    parser.add_argument("--phase", default="", help="substring filter over phase names")
    parser.add_argument("--scenario", default="", help="substring filter over scenario names")
    parser.add_argument("--iterations", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--markdown", type=Path)
    parser.add_argument("--threshold", type=float, default=0.20)
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"missing perf binary: {args.binary}", file=sys.stderr)
        return 2

    pairs = phases_from_baselines(args.baselines)
    if args.phase:
        pairs = [p for p in pairs if args.phase in p[1]]
    if args.scenario:
        pairs = [p for p in pairs if args.scenario in p[0]]
    if not pairs:
        print("no phases matched", file=sys.stderr)
        return 2

    resolver = Addr2Line(args.binary)
    results = []
    for index, (scenario, phase) in enumerate(pairs, start=1):
        print(f"[{index}/{len(pairs)}] {phase} [{scenario}]", file=sys.stderr, flush=True)
        result = audit_phase(args.binary, scenario, phase, args.iterations, resolver,
                             args.timeout)
        results.append(result)
        if "error" in result:
            print(f"    ! {result['error']}", file=sys.stderr, flush=True)
        else:
            print(f"    {result['scaffolding']}/{result['classified']} scaffolding"
                  f" = {result['share'] * 100:.1f}%", file=sys.stderr, flush=True)

    results.sort(key=lambda r: -(r.get("share") or 0.0))

    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + "\n")
    lines = ["| phase | scenario | attributed allocations | scaffolding | share | unattributed |",
             "| --- | --- | ---: | ---: | ---: | ---: |"]
    for result in results:
        if "error" in result:
            lines.append(f"| `{result['phase']}` | `{result['scenario']}` | — | — |"
                         f" {result['error']} | — |")
            continue
        flag = " **" if result["share"] >= args.threshold else " "
        lines.append(
            f"| `{result['phase']}` | `{result['scenario']}` | {result['classified']} |"
            f" {result['scaffolding']} |{flag}{result['share'] * 100:.0f} %{flag.strip()} |"
            f" {result['unattributed']} |")
    table = "\n".join(lines)
    if args.markdown:
        args.markdown.write_text(table + "\n")
    print(table)

    over = [r for r in results if r.get("share", 0.0) >= args.threshold]
    print(f"\n{len(over)} of {len(results)} phase(s) at or above "
          f"{args.threshold * 100:.0f} % scaffolding", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
