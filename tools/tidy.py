#!/usr/bin/env python3
"""clang-tidy over src/, for the checks no compiler warning replaces.

    tools/tidy.py [--build DIR] [--jobs N] [--checks LIST] [FILE ...]

The checks (TD-2026-10-16-351):

  bugprone-use-after-move        `F(Use(path), [p = std::move(path)] {...})`: sibling
                                 arguments are evaluated in an unspecified order and
                                 GCC moved first -- the outline asked clangd about
                                 "file://" (227a2ddf). No warning flag sees it.
  bugprone-unchecked-optional-access
                                 `*opt` / `opt->x` on a path where it may be empty.

A finding of a check listed in BASELINED is compared against tools/tidy-baseline.txt:
the reviewed findings that stay (unchecked-optional-access cannot see an optional
checked through another call, or an invariant), keyed by check, file and the
flagged line's text -- not its number, so an edit above does not churn the file.
A finding not in it fails the run, and so does an entry nothing reports any more
(remove it: the baseline only shrinks). `--update-baseline` rewrites it, for a
reviewed change.

Needs a compile database built with clang (tools/run-checks.sh tidy configures
build/microide-tidy for it: clang, no precompiled headers, no module scanning),
and clang-tidy: $CLANG_TIDY, then clang-tidy[-N] on PATH, then the distribution's
clang-tidy package unpacked under ~/.cache/microide-qa/clang-tidy (no root; the
same as tools/qa/local-sshd.sh does for sshd). Exits 1 on any finding of the
checks, listing each once.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CHECKS = "-*,bugprone-use-after-move,bugprone-unchecked-optional-access"
# Checks whose existing findings were reviewed and recorded rather than silenced
# one NOLINT at a time; use-after-move is not among them: it has none.
BASELINED = {"bugprone-unchecked-optional-access"}
BASELINE = REPO / "tools" / "tidy-baseline.txt"
CACHE = Path.home() / ".cache" / "microide-qa" / "clang-tidy"


def find_clang_tidy() -> str:
    if os.environ.get("CLANG_TIDY"):
        return os.environ["CLANG_TIDY"]
    for name in ["clang-tidy"] + [f"clang-tidy-{v}" for v in range(30, 13, -1)]:
        if shutil.which(name):
            return name
    unpacked = sorted(glob.glob(str(CACHE / "root/usr/lib/llvm-*/bin/clang-tidy")))
    if unpacked:
        return unpacked[-1]
    # The package that matches the installed clang, unpacked without root.
    version = re.search(r"version (\d+)", subprocess.run(["clang", "--version"], capture_output=True,
                                                          text=True).stdout or "")
    if version is None or shutil.which("apt") is None:
        sys.exit("tidy: no clang-tidy (set CLANG_TIDY, or install clang-tidy)")
    deb = CACHE / "deb"
    deb.mkdir(parents=True, exist_ok=True)
    subprocess.run(["apt", "download", f"clang-tidy-{version.group(1)}"], cwd=deb, check=True,
                   capture_output=True)
    for package in deb.glob("*.deb"):
        subprocess.run(["dpkg", "-x", str(package), str(CACHE / "root")], check=True)
    unpacked = sorted(glob.glob(str(CACHE / "root/usr/lib/llvm-*/bin/clang-tidy")))
    if not unpacked:
        sys.exit("tidy: the clang-tidy package had no clang-tidy binary")
    return unpacked[-1]


def self_test(tidy: str, checks: str) -> int:
    """The positive control (dev-docs/project/validation-traps.md): a lint that can
    no longer fire passes as green as one with nothing to report. A planted
    use-after-move and an unchecked optional must both be reported."""
    import tempfile
    planted = (
        "#include <optional>\n#include <string>\n#include <utility>\n"
        "void take(std::string);\n"
        "int moved(std::string path) { take(std::move(path)); return static_cast<int>(path.size()); }\n"
        "int unchecked(std::optional<int> value) { return *value; }\n")
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "planted.cpp"
        source.write_text(planted)
        result = subprocess.run([tidy, f"--checks={checks}", "--quiet", str(source), "--", "-std=c++20"],
                                capture_output=True, text=True)
    missing = [check for check in ("bugprone-use-after-move", "bugprone-unchecked-optional-access")
               if f"[{check}]" not in result.stdout]
    if missing:
        print(f"tidy: self-test FAILED, nothing reported for {missing}:\n{result.stdout}{result.stderr}")
        return 1
    print("tidy: self-test ok (both checks fire on the planted cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=str(REPO / "build" / "microide-tidy"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--checks", default=CHECKS)
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--self-test", action="store_true",
                        help="prove the checks fire on a planted case, then exit")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()

    if args.self_test:
        return self_test(find_clang_tidy(), args.checks)
    database = Path(args.build) / "compile_commands.json"
    if not database.exists():
        sys.exit(f"tidy: no {database}; configure it first (tools/run-checks.sh tidy does)")
    if args.files:
        files = [str(Path(f).resolve()) for f in args.files]
    else:
        src = str(REPO / "src") + os.sep
        files = sorted({entry["file"] for entry in json.loads(database.read_text())
                        if entry["file"].startswith(src) and entry["file"].endswith(".cpp")})
    if not files:
        sys.exit("tidy: no translation units under src/ in the database -- a vacuous run")
    tidy = find_clang_tidy()
    wanted = re.compile(r"^(.*?:\d+:\d+): warning: (.*) \[(" +
                        "|".join(c for c in args.checks.split(",") if not c.startswith("-")) + r")\]$")

    def run(path: str) -> tuple[str, list[str], str]:
        result = subprocess.run([tidy, "-p", args.build, f"--checks={args.checks}", "--quiet", path],
                                capture_output=True, text=True)
        found = [line for line in result.stdout.splitlines() if wanted.match(line)]
        # A TU clang cannot parse checks nothing: say so rather than pass it.
        failed = "" if result.returncode == 0 or found else (result.stderr.strip().splitlines() or ["?"])[-1]
        return path, found, failed

    findings: set[str] = set()
    broken: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for path, found, failed in pool.map(run, files):
            findings.update(found)
            if failed:
                broken.append(f"{os.path.relpath(path, REPO)}: {failed}")

    def key(line: str) -> tuple[str, str] | None:
        """(baseline key, shown line) for a baselined check's finding, else None."""
        match = wanted.match(line)
        if match is None or match.group(3) not in BASELINED:
            return None
        location = match.group(1)
        path, number = location.rsplit(":", 2)[0], int(location.rsplit(":", 2)[1])
        try:
            text = Path(path).read_text(errors="replace").splitlines()[number - 1]
        except (OSError, IndexError):
            text = ""
        relative = os.path.relpath(path, REPO)
        return f"{match.group(3)}|{relative}|{' '.join(text.split())}", line

    keyed: dict[str, str] = {}
    gating: list[str] = []
    for line in sorted(findings):
        entry = key(line)
        if entry is None:
            gating.append(line)
        else:
            keyed[entry[0]] = entry[1]
    if args.update_baseline:
        BASELINE.write_text("# Reviewed clang-tidy findings that stay (tools/tidy.py). check|file|line text\n" +
                            "".join(f"{k}\n" for k in sorted(keyed)))
        print(f"tidy: wrote {len(keyed)} baseline entries to {os.path.relpath(BASELINE, REPO)}")
    accepted = set()
    if BASELINE.exists():
        accepted = {l for l in BASELINE.read_text().splitlines() if l and not l.startswith("#")}
    new = [keyed[k] for k in sorted(keyed) if k not in accepted]
    # Only judged on a whole-tree run: a run over a few files cannot know what the
    # rest still reports.
    stale = sorted(accepted - set(keyed)) if not args.files else []
    show = lambda line: os.path.relpath(line, REPO) if line.startswith(str(REPO)) else line
    for line in gating + new:
        print(show(line))
    for entry in stale:
        print(f"tidy: baseline entry no longer reported (remove it): {entry}")
    for line in broken:
        print(f"tidy: could not check {line}", file=sys.stderr)
    print(f"tidy: {len(files)} translation units, {len(gating) + len(new)} new finding(s), "
          f"{len(keyed) - len(new)} baselined, {len(stale)} stale, {len(broken)} unparsed")
    return 1 if gating or new or stale or broken else 0


if __name__ == "__main__":
    sys.exit(main())
