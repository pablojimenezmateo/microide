#!/usr/bin/env bash
# Check the checks (safety-plan W10): every recorded negative control must make its
# check FAIL, and the check must PASS without it.
#
#   tools/check-invariants-bite.sh [--sha <commit>] [--only <patch-substring>] [--jobs N]
#
# Each row of tools/negative-controls/manifest.tsv names a patch (a reverted fix or a
# planted defect), the targets to build, and the check that exists to catch it. The
# script works in its own worktree (build/wt-bite, never the live tree) at <commit>
# (default HEAD): it runs every check clean first, then applies one patch at a time,
# rebuilds, runs the check, expects a failure, and reverts. A control that no longer
# applies is a failure too -- the code it guards moved, so the control must be
# re-recorded. Output: one line per control, and a non-zero exit if any check did not
# bite (or did not pass clean).
set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sha="HEAD"
only=""
jobs="${MICROIDE_BUILD_JOBS:-8}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --sha) sha="$2"; shift 2 ;;
    --only) only="$2"; shift 2 ;;
    --jobs) jobs="$2"; shift 2 ;;
    *) echo "usage: $0 [--sha <commit>] [--only <patch-substring>] [--jobs N]" >&2; exit 2 ;;
  esac
done

wt="$repo/build/wt-bite"
manifest="$repo/tools/negative-controls/manifest.tsv"
log="$repo/build/bite-logs"
mkdir -p "$log"
if [[ ! -e "$wt/.git" ]]; then
  git -C "$repo" worktree add --detach "$wt" "$sha" >/dev/null || exit 1
fi
git -C "$wt" checkout -q --detach "$(git -C "$repo" rev-parse "$sha")" || exit 1
git -C "$wt" reset -q --hard
cmake -S "$wt" -B "$wt/build" >/dev/null || exit 1

status=0
run_row() {  # patch targets check -> 0 when it bit
  local patch="$1" targets="$2" check="$3" name="${1%.patch}"
  # shellcheck disable=SC2086
  if ! cmake --build "$wt/build" --target $targets -j"$jobs" >"$log/$name-clean-build.txt" 2>&1; then
    echo "  BROKEN  $name: the clean tree does not build ($log/$name-clean-build.txt)"; return 1
  fi
  if ! (cd "$wt" && eval "$check") >"$log/$name-clean.txt" 2>&1; then
    echo "  BROKEN  $name: the check fails WITHOUT the control ($log/$name-clean.txt)"; return 1
  fi
  if ! git -C "$wt" apply "$repo/tools/negative-controls/$patch" 2>"$log/$name-apply.txt"; then
    echo "  STALE   $name: the control no longer applies -- re-record it ($log/$name-apply.txt)"; return 1
  fi
  local bit=1
  # A control that does not compile proves nothing: it is stale, not biting.
  # shellcheck disable=SC2086
  if ! cmake --build "$wt/build" --target $targets -j"$jobs" >"$log/$name-build.txt" 2>&1; then
    git -C "$wt" checkout -q -- .
    echo "  STALE   $name: the control does not compile -- re-record it ($log/$name-build.txt)"
    return 1
  fi
  if (cd "$wt" && eval "$check") >"$log/$name.txt" 2>&1; then
    bit=0
  fi
  git -C "$wt" checkout -q -- .
  if [[ $bit -eq 1 ]]; then
    echo "  bites   $name"
    return 0
  fi
  echo "  BLUNT   $name: the check PASSED with the control applied ($log/$name.txt)"
  return 1
}

echo "check-invariants-bite: $(git -C "$wt" log --oneline -1)"
while IFS=$'\t' read -r patch targets check; do
  [[ -z "$patch" || "$patch" == \#* ]] && continue
  [[ -n "$only" && "$patch" != *"$only"* ]] && continue
  run_row "$patch" "$targets" "$check" || status=1
done < "$manifest"
# Leave the worktree's build matching HEAD again.
cmake --build "$wt/build" --target microide microide_tests microide_kernel_tests -j"$jobs" >/dev/null 2>&1 || true
echo "check-invariants-bite: $([[ $status -eq 0 ]] && echo 'every control bites' || echo 'FAILED')"
exit $status
