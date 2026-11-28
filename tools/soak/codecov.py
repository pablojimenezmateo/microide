#!/usr/bin/env python3
"""The soak's source coverage: which of the app's code the jobs ever execute
(safety-plan W1.6 and W1.7's "code" depth).

    tools/soak/codecov.py report --binary B [--object O ...] --profdata P --out DIR [--sha SHA] [--runs N]
    tools/soak/codecov.py functions --binary B --profdata P SOURCE...

The surface inventory (surface.py) says which commands, keys and settings a run
reached; it cannot say what they ran. A command the monkey sends ten thousand
times with arguments that all fail the parser has "covered" the command and
none of what it does. This is the other measure: a clang source-coverage build
in the soak's rotation (soak.py BUILDS["coverage"]), every job's raw profiles
merged into one (`merge`), and a report of the files and areas no job executes.

`report` writes `<out>/code-coverage.json` (per file and per area: lines,
functions, branches -- covered of count), `<out>/code-coverage.md` (totals, the
areas, the files nothing ever entered, the largest uncovered files) and appends
a line to `<out>/code-coverage.jsonl`, so a drop is visible over time.
`functions` lists the functions of the given sources that were never called.
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

# Not the app's own code: what it vendors and the system's headers. What is the
# app's is decided by the source root (summarize), not here: a pattern for
# "build directories" also matched the soak's own worktree, which lives under
# build/, and the first report was 0 lines of 0.
IGNORE = r"(/third_party/|^/usr/)"
KINDS = ("lines", "functions", "branches")


def tool(name: str) -> str | None:
    """`llvm-profdata` / `llvm-cov`: the unversioned name where the toolchain
    installs it, else the newest versioned one (Ubuntu ships llvm-cov-21)."""
    found = shutil.which(name)
    if found:
        return found
    versioned = sorted(glob.glob(f"/usr/bin/{name}-[0-9]*"),
                       key=lambda path: int(path.rsplit("-", 1)[-1]) if path.rsplit("-", 1)[-1].isdigit() else 0)
    return versioned[-1] if versioned else None


def unusable_reason() -> str | None:
    """Why the coverage cell cannot run here, or None."""
    for name in ("clang++", "llvm-profdata", "llvm-cov"):
        if tool(name) is None:
            return f"{name} is not installed"
    return None


def merge(profdata: Path, raws: list[Path], timeout: float = 600.0) -> str | None:
    """Merges raw profiles into `profdata` (kept and added to when it exists).
    Returns None, or why it failed -- and then leaves `profdata` as it was.

    Profiles of an older commit stay in: a function that did not change keeps
    its counts across builds, and one that did is dropped by llvm-cov on its
    hash, so the total is what the soak has executed of the code as it is now."""
    profdata_tool = tool("llvm-profdata")
    raws = [raw for raw in raws if raw.is_file() and raw.stat().st_size > 0]
    if profdata_tool is None:
        return "llvm-profdata is not installed"
    if not raws:
        return "no raw profile to merge"
    profdata.parent.mkdir(parents=True, exist_ok=True)
    staging = profdata.with_suffix(".merging")
    inputs = [str(profdata)] if profdata.is_file() else []
    try:
        result = subprocess.run([profdata_tool, "merge", "-sparse", "-failure-mode=warn", "-o", str(staging),
                                 *inputs, *(str(raw) for raw in raws)],
                                capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return str(error)
    if result.returncode != 0 or not staging.is_file():
        staging.unlink(missing_ok=True)
        return (result.stderr or result.stdout).strip()[-300:] or f"llvm-profdata exited {result.returncode}"
    os.replace(staging, profdata)
    return None


def area_for(path: str) -> str:
    """A source path (relative to src/) in the buckets the repo's docs use;
    workspace/ one level deeper, since it is over half of the tree.

    >>> area_for("workspace/render/WorkspaceShellRenderMerge.cpp"), area_for("workspace/FileUri.cpp")
    ('workspace/render', 'workspace')
    >>> area_for("editor/TextViewport.cpp"), area_for("main.cpp")
    ('editor', '(top)')
    """
    parts = path.split("/")
    if len(parts) == 1:
        return "(top)"
    if parts[0] == "workspace":
        return "/".join(parts[:2]) if len(parts) > 2 else "workspace"
    return parts[0]


def summarize(export: dict[str, Any], source_root: str) -> dict[str, Any]:
    """llvm-cov's `export -summary-only` as {totals, files, areas}: each a
    {lines, functions, branches} of [covered, count], files keyed by their path
    under `source_root` (the `src/` of the tree the binary was built from, as an
    absolute path with its trailing slash). A file anywhere else -- a system
    header, a dependency built in the build tree -- is not the app's.

    >>> one = {"lines": {"count": 10, "covered": 4}, "functions": {"count": 2, "covered": 1},
    ...        "branches": {"count": 6, "covered": 0}}
    >>> none = {"lines": {"count": 5, "covered": 0}, "functions": {"count": 1, "covered": 0},
    ...         "branches": {"count": 0, "covered": 0}}
    >>> out = summarize({"data": [{"files": [{"filename": "/w/src/editor/A.cpp", "summary": one},
    ...                                      {"filename": "/w/src/editor/B.h", "summary": none},
    ...                                      {"filename": "/w/build/_deps/sdl/src/x.c", "summary": one},
    ...                                      {"filename": "/usr/include/c++/15/vector", "summary": one}]}]}, "/w/src/")
    >>> out["totals"], out["areas"]["editor"]["functions"], sorted(out["files"])
    ({'lines': [4, 15], 'functions': [1, 3], 'branches': [0, 6]}, [1, 3], ['editor/A.cpp', 'editor/B.h'])
    """
    files: dict[str, dict[str, list[int]]] = {}
    for unit in export.get("data", []):
        for entry in unit.get("files", []):
            name = str(entry.get("filename", ""))
            if not name.startswith(source_root):
                continue
            summary = entry.get("summary") or {}
            files[name[len(source_root):]] = {
                kind: [int((summary.get(kind) or {}).get("covered", 0)), int((summary.get(kind) or {}).get("count", 0))]
                for kind in KINDS}
    areas: dict[str, dict[str, list[int]]] = collections.defaultdict(lambda: {kind: [0, 0] for kind in KINDS})
    totals = {kind: [0, 0] for kind in KINDS}
    for name, counts in files.items():
        for kind in KINDS:
            for bucket in (areas[area_for(name)], totals):
                bucket[kind][0] += counts[kind][0]
                bucket[kind][1] += counts[kind][1]
    return {"totals": totals, "files": files, "areas": dict(areas)}


def percent(pair: list[int]) -> str:
    return f"{100 * pair[0] / pair[1]:5.1f}%" if pair[1] else "    -"


def report_markdown(summary: dict[str, Any], sha: str, runs: int) -> str:
    totals, files, areas = summary["totals"], summary["files"], summary["areas"]
    lines = ["# Source coverage of the soak", "",
             f"What the soak's jobs executed of `src/` at `{sha[:12]}`, from {runs} run(s) of the coverage build "
             f"(clang source-based; profiles merged across runs and commits). Generated "
             f"{time.strftime('%Y-%m-%d %H:%M')}.", "",
             f"- lines: **{percent(totals['lines']).strip()}** ({totals['lines'][0]} of {totals['lines'][1]})",
             f"- functions: **{percent(totals['functions']).strip()}** "
             f"({totals['functions'][0]} of {totals['functions'][1]})",
             f"- branches: **{percent(totals['branches']).strip()}** "
             f"({totals['branches'][0]} of {totals['branches'][1]})", "",
             "A line no job executes is code the soak cannot find a defect in. Each row below is a place to "
             "aim a profile, an action or a fixture at -- or an exclusion to write down with what covers it "
             "instead (`tools/soak/surface-extra.toml`).", "",
             "## By area", "", "| area | lines | functions | branches | lines not executed |", "|---|---|---|---|---|"]
    for name, counts in sorted(areas.items(), key=lambda item: item[1]["lines"][0] - item[1]["lines"][1]):
        lines.append(f"| {name} | {percent(counts['lines'])} | {percent(counts['functions'])} | "
                     f"{percent(counts['branches'])} | {counts['lines'][1] - counts['lines'][0]} |")
    dark = sorted((name for name, counts in files.items()
                   if counts["functions"][1] and not counts["functions"][0]),
                  key=lambda name: -files[name]["lines"][1])
    lines += ["", f"## Files no job ever entered ({len(dark)})", "",
              "Not one function of these was called (headers of only inline code included).", ""]
    lines += [f"- `{name}` ({files[name]['lines'][1]} lines, {files[name]['functions'][1]} functions)"
              for name in dark[:150]]
    if len(dark) > 150:
        lines.append(f"- ... and {len(dark) - 150} more")
    partial = sorted(((counts["lines"][1] - counts["lines"][0], name) for name, counts in files.items()
                      if counts["functions"][0]), reverse=True)
    lines += ["", "## The most unexecuted lines in files that are entered", "",
              "| file | lines | functions | lines not executed |", "|---|---|---|---|"]
    lines += [f"| `{name}` | {percent(files[name]['lines'])} | {percent(files[name]['functions'])} | {missing} |"
              for missing, name in partial[:60]]
    lines += ["", "For the functions of a file that were never called:", "", "```bash",
              "tools/soak/soak.py coverage --functions src/<path>", "```", ""]
    return "\n".join(lines)


def export(binary: Path, objects: list[Path], profdata: Path, sources: list[str] | None = None,
           summary_only: bool = True, timeout: float = 1800.0, branches: bool = False) -> dict[str, Any]:
    cov = tool("llvm-cov")
    if cov is None:
        raise RuntimeError("llvm-cov is not installed")
    detail = ["-skip-expansions"] + ([] if branches else ["-skip-branches"])
    command = [cov, "export", "-format=text", f"-instr-profile={profdata}", f"-ignore-filename-regex={IGNORE}",
               *(["-summary-only"] if summary_only else detail), str(binary)]
    for other in objects:
        command += ["-object", str(other)]
    command += sources or []
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError((result.stderr or "llvm-cov export failed").strip()[-400:])
    return json.loads(result.stdout)


_HUNK = re.compile(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", re.MULTILINE)


def changed_lines(diff: str) -> dict[str, set[int]]:
    """The new-side lines a `git diff -U0` adds or changes, per file under src/
    (keyed by the path under it); a deleted file has none."""
    changed: dict[str, set[int]] = {}
    for chunk in diff.split("\ndiff --git "):
        target = re.search(r"^\+\+\+ b/src/(\S+)$", chunk, re.MULTILINE)
        if target is None:
            continue
        lines = {start + offset for match in _HUNK.finditer(chunk)
                 for start, count in [(int(match.group(1)), int(match.group(2) or 1))]
                 for offset in range(count)}
        if lines:
            changed[target.group(1)] = lines
    return changed


def changed_branches(export_data: dict[str, Any], changed: dict[str, set[int]],
                     source_root: str) -> dict[str, list[int]]:
    """For each changed file, [executed, count] of the branch conditions on its
    changed lines, each branch counted as llvm-cov does: once taken, once not."""
    out: dict[str, list[int]] = {}
    for unit in export_data.get("data", []):
        for entry in unit.get("files", []):
            name = str(entry.get("filename", ""))
            if not name.startswith(source_root) or name[len(source_root):] not in changed:
                continue
            rel = name[len(source_root):]
            counts = out.setdefault(rel, [0, 0])
            for branch in entry.get("branches", []):
                if branch[0] in changed[rel]:
                    counts[0] += (branch[4] > 0) + (branch[5] > 0)
                    counts[1] += 2
    return out


def changed_markdown(per_file: dict[str, list[int]], base: str, sha: str) -> str:
    """The report's section on the code the commits since the last report changed."""
    rows = sorted(((counts[0] / counts[1], name) for name, counts in per_file.items() if counts[1]))
    executed = sum(per_file[name][0] for _, name in rows)
    total = sum(per_file[name][1] for _, name in rows)
    lines = ["", f"## Changed since the last report ({base[:12]}..{sha[:12]})", "",
             f"Branch conditions on the changed lines the soak executed: **{executed} of {total}** "
             "(each branch once taken, once not). New code no job reaches is code the soak cannot "
             "find a defect in yet.", ""]
    if rows:
        lines += ["| file | branch conditions executed |", "|---|---|"]
        lines += [f"| `{name}` | {per_file[name][0]} of {per_file[name][1]} |" for _, name in rows[:60]]
    return "\n".join(lines) + "\n"


def changed_since(binary: Path, objects: list[Path], profdata: Path, source_root: Path, base: str,
                  sha: str) -> dict[str, list[int]]:
    repo = source_root.resolve().parent
    diff = subprocess.run(["git", "diff", "-U0", "--no-color", base, sha, "--", "src"], cwd=repo,
                          capture_output=True, text=True, timeout=300)
    if diff.returncode != 0:
        raise RuntimeError((diff.stderr or "git diff failed").strip()[-400:])
    changed = changed_lines(diff.stdout)
    if not changed:
        return {}
    root = str(source_root.resolve()) + "/"
    data = export(binary, objects, profdata, [root + name for name in sorted(changed)], summary_only=False,
                  branches=True)
    return changed_branches(data, changed, root)


def write_report(out: Path, binary: Path, objects: list[Path], profdata: Path, sha: str, runs: int,
                 source_root: Path, since: str = "") -> dict[str, Any]:
    summary = summarize(export(binary, objects, profdata), str(source_root.resolve()) + "/")
    if not summary["files"]:
        raise RuntimeError(f"no file of the report is under {source_root}: the binary was built elsewhere")
    out.mkdir(parents=True, exist_ok=True)
    record = {"t": time.time(), "sha": sha, "runs": runs, **summary}
    markdown = report_markdown(summary, sha, runs)
    line = {"t": round(record["t"]), "sha": sha, "runs": runs, **summary["totals"]}
    if since and since != sha:
        per_file = changed_since(binary, objects, profdata, source_root, since, sha)
        record["changed"] = {"base": since, "files": per_file}
        line["changed_branches"] = [sum(c[0] for c in per_file.values()), sum(c[1] for c in per_file.values())]
        line["since"] = since
        markdown += changed_markdown(per_file, since, sha)
    (out / "code-coverage.json").write_text(json.dumps(record))
    (out / "code-coverage.md").write_text(markdown)
    with open(out / "code-coverage.jsonl", "a") as history:
        history.write(json.dumps(line) + "\n")
    return record


def uncalled_functions(binary: Path, objects: list[Path], profdata: Path, sources: list[str]) -> list[tuple[str, str]]:
    """(source, function) for every function of `sources` no run called."""
    data = export(binary, objects, profdata, sources, summary_only=False)
    wanted = set(sources)
    out = []
    for unit in data.get("data", []):
        for function in unit.get("functions", []):
            names = function.get("filenames") or ["?"]
            # The export lists every function of the binary; the sources only
            # narrow its file summaries.
            if function.get("count", 0) == 0 and names[0] in wanted:
                out.append((names[0], demangle(str(function.get("name", "")))))
    return sorted(set(out))


def demangle(name: str) -> str:
    """A function's name as llvm-cov gives it (`file.cpp:_ZN...` for a static
    one), demangled when c++filt is there."""
    prefix, _, symbol = name.rpartition(":")
    filt = shutil.which("c++filt")
    if filt is None or not symbol.startswith("_Z"):
        return name
    try:
        text = subprocess.run([filt, symbol], capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return name
    return f"{prefix}:{text}" if prefix else text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    for name in ("report", "functions"):
        command = sub.add_parser(name)
        command.add_argument("--binary", type=Path, required=True)
        command.add_argument("--object", type=Path, action="append", default=[])
        command.add_argument("--profdata", type=Path, required=True)
        if name == "report":
            command.add_argument("--out", type=Path, required=True)
            command.add_argument("--sha", default="")
            command.add_argument("--runs", type=int, default=0)
            command.add_argument("--since", default="",
                                 help="also report the branches on the lines changed since this commit")
            command.add_argument("--root", type=Path, required=True,
                                 help="the src/ directory of the tree the binary was built from")
        else:
            command.add_argument("sources", nargs="+")
    args = parser.parse_args()
    if args.mode == "report":
        record = write_report(args.out, args.binary, args.object, args.profdata, args.sha, args.runs, args.root,
                              args.since)
        totals = record["totals"]
        print(f"lines {percent(totals['lines']).strip()}, functions {percent(totals['functions']).strip()}, "
              f"branches {percent(totals['branches']).strip()}; {args.out / 'code-coverage.md'}")
        return 0
    for source, function in uncalled_functions(args.binary, args.object, args.profdata,
                                               [str(Path(s).resolve()) for s in args.sources]):
        print(f"{source}\t{function}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
