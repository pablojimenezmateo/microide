#!/usr/bin/env bash
set -euo pipefail

BASE_REF="${1:-origin/main}"
HEAD_REF="${2:-HEAD}"
PR_BODY="${3:-}"
COMMIT_MESSAGES="${4:-}"

changed="$(git diff --name-only "${BASE_REF}...${HEAD_REF}" -- 'tests/perf/baselines/*.json' || true)"
if [[ -z "${changed}" ]]; then
  echo "No perf baseline JSON changes detected."
  exit 0
fi

if grep -q '^perf-baseline:' <<<"${PR_BODY}"; then
  echo "Found perf-baseline tag in PR body."
  exit 0
fi

if grep -q '^perf-baseline:' <<<"${COMMIT_MESSAGES}"; then
  echo "Found perf-baseline tag in commit messages."
  exit 0
fi

echo "Baseline files changed:"
echo "${changed}"
echo "Missing required 'perf-baseline:' line in PR description or commit message."
exit 1
