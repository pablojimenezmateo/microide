#!/usr/bin/env bash
# Mutation testing of the soak's oracles (TD-2026-11-08-435 item 8): mutmut
# mutates tools/qa/monkey_oracles.py and runs test_monkey_oracles.py against
# each mutant; a mutant the tests do not fail is an oracle change nothing
# would notice. Runs on a scratch copy of tools/ (mutmut writes a mutants/
# tree beside the module), prints the per-function summary, and leaves the
# copy for `mutmut show <name>`.
#
# Usage: tools/qa/mutation-test.sh [scratch-dir] [children]
# Needs mutmut with pytest and pillow in its environment:
#   pipx install mutmut && pipx inject mutmut pytest pillow
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
scratch="${1:-$repo/build/mutation}"
children="${2:-8}"
command -v mutmut >/dev/null || { echo "mutation-test: no mutmut (pipx install mutmut)" >&2; exit 2; }

rm -rf "$scratch"
mkdir -p "$scratch"
cp -r "$repo/tools" "$scratch/"
qa="$scratch/tools/qa"
mkdir -p "$qa/tests"
mv "$qa/test_monkey_oracles.py" "$qa/tests/"
# The test imports its siblings from its own directory; under tests/ the mutated
# module comes from mutants/ first and the rest from the copy's tools/qa.
sed -i "s|^sys.path.insert(0, str(Path(__file__).resolve().parent))|sys.path.insert(0, str(Path(__file__).resolve().parent.parent))\nsys.path.append('$qa')|" \
  "$qa/tests/test_monkey_oracles.py"
printf '[mutmut]\npaths_to_mutate=monkey_oracles.py\ntests_dir=tests/\n' > "$qa/setup.cfg"

cd "$qa"
mutmut run --max-children "$children" >mutmut.log 2>&1 || true
mutmut results --all true >results.txt 2>/dev/null || mutmut results >results.txt
python3 - results.txt <<'EOF'
import collections, re, sys
counts = collections.defaultdict(collections.Counter)
for line in open(sys.argv[1]):
    m = re.match(r"\s*monkey_oracles\.(?:x[ǁ_]?)?(.+?)__mutmut_\d+:\s*(.+?)\s*$", line)
    if m:
        counts[m.group(1).replace("ǁ", ".")][m.group(2)] += 1
total = collections.Counter()
for c in counts.values():
    total.update(c)
print(f"mutants: {sum(total.values())}  killed {total['killed']}  survived {total['survived']}  "
      f"untested {total['no tests']}  timeout {total['timeout']}")
rows = sorted(counts.items(), key=lambda kv: -(kv[1]["survived"] + kv[1]["no tests"]))
for name, c in rows:
    if c["survived"] or c["no tests"]:
        print(f"  {name:50s} survived {c['survived']:4d}  untested {c['no tests']:4d}  killed {c['killed']:4d}")
EOF
