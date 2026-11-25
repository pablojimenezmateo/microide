#!/usr/bin/env python3
"""Do cleanup passes undo each other? Measure it from history.

Walks every commit whose subject starts with refactor/cleanup/chore, oldest
first, and tracks each normalized source line (>= 30 chars, includes and
namespace lines ignored) it adds or removes under src/. It reports:

  * re-adds: a line one cleanup removed and a LATER cleanup put back. This is
    the oscillation signal -- two passes disagreeing about the same code.
  * the commit pairs with the most re-adds, with an example line, so the
    disagreement can be read and settled (then encoded as a lint, see
    dev-docs/project/architecture-invariants.md).

Lines added and later removed are normal iteration (extract, then move) and
are reported only as a total.

Usage:  python3 tools/refactor-churn.py [--since=<rev>] [--top=N]
"""

import collections
import re
import subprocess
import sys

since = None
top = 15
for arg in sys.argv[1:]:
    if arg.startswith("--since="):
        since = arg.split("=", 1)[1]
    elif arg.startswith("--top="):
        top = int(arg.split("=", 1)[1])

rev_range = [f"{since}..HEAD"] if since else ["HEAD"]
log = subprocess.run(["git", "log", "--reverse", "--format=%h %s", *rev_range],
                     capture_output=True, text=True, check=True).stdout.splitlines()
commits = [line.split(" ", 1) for line in log]
cleanups = [(h, s) for h, s in commits if re.match(r"(refactor|cleanup|chore)", s)]
subject = dict(cleanups)
boilerplate = re.compile(r"^(#include|namespace\b|}\s*//|using\s)")


def interesting(line):
    text = " ".join(line.split())
    return text if len(text) >= 30 and not boilerplate.match(text) else None


last_added = {}
last_removed = {}
undone = 0
readded = 0
pairs = collections.Counter()
example = {}
for commit, _ in cleanups:
    diff = subprocess.run(["git", "show", "--format=", "--unified=0", "--no-color", commit, "--", "src"],
                          capture_output=True, text=True, check=True).stdout
    added, removed = set(), set()
    for line in diff.splitlines():
        if line.startswith(("+++", "---")):
            continue
        if line[:1] in "+-":
            text = interesting(line[1:])
            if text:
                (added if line[0] == "+" else removed).add(text)
    moved = added & removed
    added -= moved
    removed -= moved
    for text in removed:
        if text in last_added and last_added[text] != commit:
            undone += 1
    for text in added:
        earlier = last_removed.get(text)
        if earlier and earlier != commit:
            readded += 1
            pairs[(earlier, commit)] += 1
            example.setdefault((earlier, commit), text)
    for text in added:
        last_added[text] = commit
    for text in removed:
        last_removed[text] = commit

added_total = len(last_added)
print(f"cleanup commits: {len(cleanups)}")
print(f"lines added by a cleanup and removed by a later one: {undone}")
print(f"lines removed by a cleanup and RE-ADDED by a later one: {readded}"
      f" ({100.0 * readded / max(added_total, 1):.2f}% of distinct lines added)")
print(f"\nTop re-add pairs (removed by A, put back by B):")
for (first, second), count in pairs.most_common(top):
    print(f"{count:4d} {first} {subject[first][:72]}")
    print(f"     -> {second} {subject[second][:72]}")
    print(f"     e.g. {example[(first, second)][:100]}")
