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

The binary must be built with identical-code folding OFF, or the symbol names in
this report are not trustworthy: at -O2 (whole-program under LTO) GCC's -fipa-icf
folds functionally identical bodies and keeps ONE of their symbols, so addr2line
resolves every folded body to whichever name survived. That is how an earlier
sweep came to rank `TextViewport::operator=() [clone .part.0]` as the #1 allocator
of eight phases whose parent frames never copy-assign a viewport
(TD-2026-08-13-197). Counts, sizes and phases were always measured; only the
NAMES were unreliable.

  cmake --preset microide-perf-trace
  cmake --build build/microide-perf-trace --target microide_perf -j8
  tools/trace-perf-phases.py --binary build/microide-perf-trace/microide/microide_perf

This tool refuses to run against a binary without it rather than print a table it
cannot stand behind; pass --allow-folded-symbols to override, and read every
symbol name in the output as a hypothesis to confirm in the source.
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
DEFAULT_BINARY = REPO / "build" / "microide-perf-trace" / "microide" / "microide_perf"
TRACE_SYMBOLS_MARKER = "+no-icf"


def build_config(binary: pathlib.Path) -> str | None:
    """The build configuration baked into `binary`, or None when it cannot say.

    A binary too old to know the flag is exactly the case this guard exists for,
    so an unparseable answer is treated as "not trustworthy", not as "fine".
    """
    try:
        proc = subprocess.run([str(binary), "--build-config"], capture_output=True,
                              text=True, timeout=60, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    config = proc.stdout.strip()
    return config or None

# The tracer's own bucket table drops each new site whole once full, so a run that
# overflows prints a correctly-sorted and WRONG table (TD-2026-08-10-178). The
# report carries the warning verbatim rather than paraphrasing it.
OVERFLOW_MARKER = "WARNING"

SITE_RE = re.compile(r"^\[alloctrace\] #(\d+): (\d+) allocations, (\d+) bytes")

# One `backtrace_symbols_fd` line. glibc emits three shapes, and the difference
# between them is the whole reason this regex exists rather than a bare offset
# grab (TD-2026-08-30-270):
#
#   /path/to/microide_perf(+0x3dfb2a)[0x5b88...]          module-relative offset
#   /lib/.../libstdc++.so.6(_ZNSt10..._+0x13d)[0x761c...] SYMBOL-relative offset
#   /lib/.../libstdc++.so.6(+0x1ae8d5)[0x761c...]         a DIFFERENT module
#
# The tool used to take `\+0x([0-9a-f]+)\)` from every line and hand all of them
# to `addr2line -e <the traced binary>`. Both of the last two shapes then resolved
# against the wrong base: `libstdc++.so.6(+0x1ae8d5)` landed inside whatever
# microide function happened to live at 0x1ae8d5 of the binary. That is how
# `microide::editor::ApplyChoiceForTab` — a static helper in the snippet engine —
# came to be reported as the #1 allocator of six unrelated phases, and why the
# report's own header blamed identical-code folding for names ICF never touched.
FRAME_RE = re.compile(r"^(?P<module>[^(]+)\((?P<symbol>[^)+]*)\+0x(?P<offset>[0-9a-f]+)\)")


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


# addr2line on an LTO binary costs seconds per invocation, and the same offsets
# recur across phases (every stack ends in the same harness frames). Without this
# the sweep spent most of its wall time re-resolving addresses it had already seen.
_RESOLVE_CACHE: dict[str, list[str]] = {}


def demangle(names: list[str]) -> list[str]:
    if not names:
        return []
    proc = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True,
                          text=True, check=False)
    out = proc.stdout.splitlines()
    return out if len(out) == len(names) else names


def resolve_offsets(binary: pathlib.Path, offsets: list[str]) -> dict[str, list[str]]:
    """addr2line every module-relative offset INTO `binary`, memoized across phases."""
    unknown = [o for o in offsets if o not in _RESOLVE_CACHE]
    if unknown:
        proc = subprocess.run(
            ["addr2line", "-e", str(binary), "-f", "-C", "-p", *unknown],
            capture_output=True, text=True, check=False,
        )
        resolved = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        # One line per offset without -i; with an inline chain addr2line emits the
        # chain on continuation lines, which -p prefixes with "(inlined by)".
        current: list[str] = []
        index = 0
        for line in resolved:
            if line.startswith("(inlined by)") and current:
                current.append(line)
                continue
            if current and index < len(unknown):
                _RESOLVE_CACHE[unknown[index]] = current
                index += 1
            current = [line]
        if current and index < len(unknown):
            _RESOLVE_CACHE[unknown[index]] = current
        for offset in unknown:
            _RESOLVE_CACHE.setdefault(offset, [])
    return {offset: _RESOLVE_CACHE.get(offset, []) for offset in offsets}


def shorten(line: str) -> str:
    line = re.sub(r"\(.*?\)", "()", line)
    line = re.sub(r"<[^<>]{40,}>", "<...>", line)
    return line[:200]


def frames_for_site(binary: pathlib.Path, raw_frames: list[str]) -> list[str]:
    """Resolve one site's raw `backtrace_symbols_fd` lines, in stack order.

    Each frame is resolved through the mechanism its OWN shape allows, which is
    the correction described at FRAME_RE. Frames from the traced binary go to
    addr2line; a frame that already carries a mangled symbol is demangled as-is;
    a frame in another module with neither is reported as `module+0xoffset`
    rather than resolved against a binary it does not belong to.
    """
    binary_name = binary.name
    kinds: list[tuple[str, str]] = []  # (kind, payload) in stack order
    offsets: list[str] = []
    mangled: list[str] = []
    for frame in raw_frames:
        hit = FRAME_RE.match(frame.strip())
        if not hit:
            continue
        module = pathlib.PurePath(hit.group("module")).name
        symbol = hit.group("symbol")
        if symbol:
            kinds.append(("symbol", f"{module}\x00{symbol}"))
            mangled.append(symbol)
        elif module == binary_name:
            offset = "0x" + hit.group("offset")
            kinds.append(("offset", offset))
            offsets.append(offset)
        else:
            kinds.append(("foreign", f"{module}+0x{hit.group('offset')}"))

    resolved = resolve_offsets(binary, offsets)
    demangled = dict(zip(mangled, demangle(mangled)))

    lines: list[str] = []
    for kind, payload in kinds:
        if kind == "offset":
            lines.extend(resolved.get(payload, []) or [f"{binary_name}+{payload}"])
        elif kind == "symbol":
            module, symbol = payload.split("\x00", 1)
            lines.append(f"{module}: {demangled.get(symbol, symbol)}")
        else:
            lines.append(payload)

    # Keep the frames a reader can act on. An inlined libstdc++ stack resolves
    # into pages of variant/vtable mangling that bury the one `microide::` frame
    # naming the call site, so project frames come first -- but the non-project
    # ones are the fallback rather than being dropped, because a site whose whole
    # stack is inside libstdc++ (a `std::filesystem::path` operator, say) still
    # has to be nameable.
    project = [shorten(l) for l in lines
               if "microide::" in l and "/usr/include" not in l and not l.startswith("??")]
    if project:
        return project[:4]
    return [shorten(l) for l in lines if not l.startswith("??") and "_start" not in l][:4]


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
        sites.append({
            "rank": int(match.group(1)),
            "allocations": int(match.group(2)),
            "bytes": int(match.group(3)),
            "frames": frames_for_site(binary, lines[index + 1: index + 9]),
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
    parser.add_argument("--allow-folded-symbols", action="store_true",
                        help="run against a binary built WITH identical-code folding; "
                             "every symbol name in the report is then a hypothesis")
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"perf binary not found: {args.binary}\n"
              f"build it with: cmake --preset microide-perf-trace && "
              f"cmake --build build/microide-perf-trace --target microide_perf", file=sys.stderr)
        return 1

    # Refuse rather than print a table this tool cannot stand behind. The failure
    # it guards is silent and convincing: the counts stay correct while the NAMES
    # attribute them to whichever folded twin GCC kept (TD-2026-08-13-197).
    config = build_config(args.binary)
    if config is None or TRACE_SYMBOLS_MARKER not in config:
        described = config if config is not None else "unknown (binary too old to say)"
        message = (f"refusing to trace: {args.binary} was built as {described}, without "
                   f"identical-code folding disabled.\n"
                   f"-fipa-icf folds functionally identical bodies onto ONE symbol, so every "
                   f"resolved name in the report may belong to a different function than the "
                   f"one that allocated.\n"
                   f"build the trace lane instead:\n"
                   f"  cmake --preset microide-perf-trace\n"
                   f"  cmake --build build/microide-perf-trace --target microide_perf -j8\n"
                   f"or pass --allow-folded-symbols and treat every name as a hypothesis.")
        if not args.allow_folded_symbols:
            print(message, file=sys.stderr)
            return 1
        print(f"WARNING: {message}\n", file=sys.stderr)

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

    def flush() -> None:
        # Written after EVERY phase, not once at the end. A full sweep is over an
        # hour of wall time, and the first version of this tool produced nothing at
        # all when it was interrupted -- which made a partial answer impossible to
        # keep, on exactly the runs where a partial answer is what you have.
        if args.out:
            args.out.write_text("\n".join(report) + "\n")

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
        flush()

    text = "\n".join(report) + "\n"
    if args.out:
        args.out.write_text(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
