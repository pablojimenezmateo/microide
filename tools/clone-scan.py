#!/usr/bin/env python3
"""Cross-TU clone scan: find byte-identical function bodies duplicated across files.

The AppendHintSegment / ProjectLabelForRoot failure shape: two files grow a
byte-identical private copy of a helper, then one drifts. This script extracts
function bodies by brace matching, normalizes away whitespace and // comments,
and reports any body of >= MIN_LINES normalized lines that appears in more than
one file. Every prior hit of this sweep was a real drift risk (see
dev-docs/project/validation-traps.md § Mechanical Sweeps).

Usage:  python3 tools/clone-scan.py [root=src] [--min-lines=N]

Prints clone groups (largest first) and exits 1 if any UNACCEPTED cross-file
clone is found, 0 otherwise — so it can gate a CI lane if promoted. The
deliberately-kept clones live in ACCEPTED_CLONES below with the reason;
without that list the tool exited 1 on the standing tree, which makes the exit
code useless as a gate (a new clone is indistinguishable from the accepted
state) — the validation-traps failure shape, inverted: a check that can never
go green is as blind as one that can never go red. A stale ACCEPTED_CLONES
entry (its signature no longer occurs as a clone) also fails the run, so the
list cannot rot into silently accepting future clones (the RequireRuleTarget
rule, applied here).
"""

import collections
import hashlib
import pathlib
import re
import sys

min_lines = 8
root = pathlib.Path("src")
for arg in sys.argv[1:]:
    if arg.startswith("--min-lines="):
        min_lines = int(arg.split("=", 1)[1])
    else:
        root = pathlib.Path(arg)

bodies = collections.defaultdict(list)
definition_re = re.compile(
    r"^(if|for|while|switch|else|do|catch|namespace|struct|class|enum|union)\b"
)

for path in sorted(root.rglob("*.cpp")):
    lines = path.read_text(errors="replace").splitlines()
    total = len(lines)
    i = 0
    while i < total:
        stripped = lines[i].strip()
        # A definition heuristically: a line ending in '{' with a '(' that is not
        # a control-flow statement and not an assignment (lambdas/initializers).
        if (
            stripped.endswith("{")
            and "(" in stripped
            and not definition_re.match(stripped)
            and "=" not in stripped.split("(")[0]
        ):
            depth = 0
            j = i
            body = []
            while j < total:
                depth += lines[j].count("{") - lines[j].count("}")
                body.append(lines[j])
                if depth <= 0 and j > i:
                    break
                j += 1
            if len(body) >= min_lines and depth <= 0:
                norm = []
                for raw in body[1:]:  # drop the signature line: names may differ
                    text = re.sub(r"//.*", "", raw).strip()
                    if text:
                        norm.append(text)
                if len(norm) >= min_lines - 2:
                    digest = hashlib.sha1("\n".join(norm).encode()).hexdigest()
                    bodies[digest].append(
                        (str(path), i + 1, len(norm), body[0].strip()[:100])
                    )
            i = j + 1
        else:
            i += 1

# Clone groups that are ACCEPTED, keyed by the signature line every occurrence
# in the group must carry. Each entry is a decision with a reason, not a mute
# button: the `#ifdef _WIN32` subprocess trio is never compiled on the supported
# host, and deduplicating untestable code risks breaking it invisibly (the
# platform WON'T-DO policy, TD-2026-09-03-282).
ACCEPTED_CLONES = {
    "std::wstring ToWide(std::string_view text) {",
    "std::wstring QuoteWindowsArgument(std::string_view arg) {",
    "std::wstring BuildCommandLine(const std::vector<std::string>& argv) {",
}

found = False
accepted_seen = set()
for digest, occurrences in sorted(bodies.items(), key=lambda kv: -kv[1][0][2]):
    files = {occ[0] for occ in occurrences}
    if len(files) <= 1:
        continue
    signatures = {occ[3] for occ in occurrences}
    if signatures <= ACCEPTED_CLONES:
        accepted_seen |= signatures
        print(f"--- accepted ({occurrences[0][2]} normalized lines, {len(occurrences)} copies — see ACCEPTED_CLONES):")
        for file, line, _, signature in occurrences:
            print(f"    {file}:{line}  {signature}")
        continue
    found = True
    print(f"--- {occurrences[0][2]} normalized lines, {len(occurrences)} copies:")
    for file, line, _, signature in occurrences:
        print(f"    {file}:{line}  {signature}")

stale = ACCEPTED_CLONES - accepted_seen
if stale and root == pathlib.Path("src"):
    found = True
    print("stale ACCEPTED_CLONES entries (no longer occur as clones — remove them):")
    for signature in sorted(stale):
        print(f"    {signature}")

sys.exit(1 if found else 0)
