#!/usr/bin/env bash
#
# perf-gate.sh — run the full perf gate on the reference runner, keep the report,
# and say what drifted since last time.
#
# TD-2026-08-06-141: nothing reruns the gate, so drift is only ever found by
# accident. Baselines went 40-80% loose and five allocation gates drifted up
# before anyone looked, and the failure mode is silent by construction: gates trip
# on increases, so a baseline that has gone loose is green forever. CI cannot
# re-measure — the baselines are absolute timings from the pinned perf-runner-v1
# host, which is the maintainer's workstation — but the gate does not need CI. It
# needs *something* to run it on the reference machine and report.
#
# Usage:
#   tools/perf-gate.sh                 # build, run the gate, record, report drift
#   tools/perf-gate.sh --scenarios=a,b # a subset (does not become the drift record)
#   tools/perf-gate.sh --install-timer # install a weekly systemd user timer
#   tools/perf-gate.sh --status        # what the last recorded run said
#   tools/perf-gate.sh --drift         # re-report drift without running anything
#
# Environment:
#   MICROIDE_PERF_DRIFT_DIR  where dated reports accumulate
#                            (default ${XDG_STATE_HOME:-~/.local/state}/microide/perf-drift)
#   MICROIDE_PERF_ITERATIONS iterations per scenario (default 10 — the count the
#                            baselines were recorded at; see the note below)
#   MICROIDE_PERF_RUNNER     reference runner id (default perf-runner-v1)
#   MICROIDE_BUILD_JOBS      build parallelism (default nproc)
#
# Iterations: 10 is not a tuning knob. A baseline records a p50/p95 captured at
# some iteration count, and re-measuring at a different one compares percentiles
# of differently-sized samples — which reads as drift that is not there. Change it
# only alongside a rebaseline at the same count.
#
# Exit status is the gate's: non-zero when a scenario failed its envelope, or when
# the drift report flags deterministic (allocation) movement. That matters — a
# scheduled run whose failures nobody sees is the same defect one layer up, so
# this exits loudly, writes a summary a human can read, and (if notify-send is
# present) raises a desktop notification.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DRIFT_DIR="${MICROIDE_PERF_DRIFT_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/microide/perf-drift}"
ITERATIONS="${MICROIDE_PERF_ITERATIONS:-10}"
RUNNER="${MICROIDE_PERF_RUNNER:-perf-runner-v1}"
JOBS="${MICROIDE_BUILD_JOBS:-$(nproc 2>/dev/null || echo 8)}"
BUILD_DIR="build/microide-perf-make"
BIN="$BUILD_DIR/microide/microide_perf"

# ccache caches PCH-using translation units only when told to tolerate the
# precompiled-header sloppiness; same export run-checks.sh makes.
export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros}"

mkdir -p "$DRIFT_DIR"

notify() {
  local urgency="$1" title="$2" body="$3"
  command -v notify-send >/dev/null 2>&1 &&
    notify-send --urgency="$urgency" "$title" "$body" >/dev/null 2>&1
  return 0
}

# ---------------------------------------------------------------------------
# --status / --drift: read the record without running anything
# ---------------------------------------------------------------------------

cmd_status() {
  local summary="$DRIFT_DIR/latest-summary.txt"
  if [[ ! -f "$summary" ]]; then
    echo "perf-gate: no recorded run yet in $DRIFT_DIR"
    echo "           run: tools/perf-gate.sh"
    return 1
  fi
  cat "$summary"
  local count
  count=$(find "$DRIFT_DIR" -maxdepth 1 -name '*.json' -not -name 'latest*' | wc -l)
  echo
  echo "perf-gate: $count report(s) recorded in $DRIFT_DIR"
}

cmd_drift() {
  python3 tools/perf-drift.py --dir "$DRIFT_DIR"
}

# ---------------------------------------------------------------------------
# --install-timer: the scheduler
# ---------------------------------------------------------------------------
#
# A systemd *user* timer, not a system one: the gate has to run as the
# maintainer, in the maintainer's session, on the maintainer's machine. Weekly
# rather than nightly because a full run is ~20 minutes of exclusive machine time
# and drift accumulates over weeks, not hours. Persistent=true so a machine that
# was off on the scheduled day runs on next boot instead of skipping the week —
# a scheduler that silently misses is the failure this exists to prevent.

cmd_install_timer() {
  local unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
  mkdir -p "$unit_dir"

  cat >"$unit_dir/microide-perf-gate.service" <<EOF
[Unit]
Description=microide perf gate (drift record for TD-2026-08-06-141)
Documentation=file://$REPO_ROOT/dev-docs/performance/perf-harness.md

[Service]
Type=oneshot
WorkingDirectory=$REPO_ROOT
ExecStart=$REPO_ROOT/tools/perf-gate.sh
# The gate measures absolute timings. Anything else competing for the pinned
# cores invalidates the run, so give it the machine rather than sharing it.
Nice=-5
IOSchedulingClass=best-effort
IOSchedulingPriority=0
EOF

  cat >"$unit_dir/microide-perf-gate.timer" <<EOF
[Unit]
Description=Weekly microide perf gate run

[Timer]
OnCalendar=Sun 04:00
# Run on next boot if the machine was off when it was due. A missed week is
# indistinguishable from a clean week in the record, which is the whole problem.
Persistent=true
RandomizedDelaySec=0

[Install]
WantedBy=timers.target
EOF

  systemctl --user daemon-reload
  systemctl --user enable --now microide-perf-gate.timer
  echo "perf-gate: installed and enabled microide-perf-gate.timer"
  systemctl --user list-timers microide-perf-gate.timer --no-pager
  echo
  echo "perf-gate: results land in $DRIFT_DIR"
  echo "           read them with: tools/perf-gate.sh --status"
  echo "           (add that line to a shell profile if it should nag)"
}

# ---------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------

cmd_run() {
  local extra_args=("$@")
  local subset=0
  for arg in "${extra_args[@]:-}"; do
    [[ "$arg" == --scenarios=* ]] && subset=1
  done

  local commit dirty stamp
  commit=$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
  dirty=""
  git diff --quiet HEAD 2>/dev/null || dirty="-dirty"
  stamp=$(date -u +%Y%m%dT%H%M%SZ)

  local run_dir="$DRIFT_DIR"
  # A subset run is a diagnostic, not a drift record: comparing a 3-scenario
  # report against a 93-scenario one reads every absent scenario as a change.
  # Keep it out of the series.
  if (( subset )); then
    run_dir="$DRIFT_DIR/subset"
    mkdir -p "$run_dir"
  fi

  local report="$run_dir/${stamp}-${commit}${dirty}.json"
  local log="$run_dir/${stamp}-${commit}${dirty}.log"

  echo "perf-gate: building $BIN"
  if ! cmake --preset microide-perf >"$log" 2>&1; then
    echo "perf-gate: cmake configure FAILED; see $log" >&2
    notify critical "microide perf gate" "cmake configure failed"
    return 2
  fi
  if ! cmake --build "$BUILD_DIR" --target microide_perf -j"$JOBS" >>"$log" 2>&1; then
    echo "perf-gate: build FAILED; see $log" >&2
    notify critical "microide perf gate" "build failed — see $log"
    return 2
  fi

  # Previous report BEFORE this one is written, so the comparison is against the
  # last run rather than against itself.
  local previous
  previous=$(python3 - "$run_dir" <<'PY'
import json, sys
from pathlib import Path
best = None
for path in Path(sys.argv[1]).glob("*.json"):
    if path.name.startswith("."):
        continue
    try:
        stamp = json.loads(path.read_text()).get("metadata", {}).get("timestamp_utc", "")
    except Exception:
        continue
    key = (str(stamp), path.name)
    if best is None or key > best[0]:
        best = (key, path)
print(best[1] if best else "")
PY
)

  echo "perf-gate: running the gate at ${ITERATIONS} iterations (runner=$RUNNER)"
  "$BIN" --iterations="$ITERATIONS" --reference-runner="$RUNNER" \
    --report-json="$report" "${extra_args[@]:-}" 2>&1 | tee -a "$log"
  local gate_rc=${PIPESTATUS[0]}

  if [[ ! -f "$report" ]]; then
    echo "perf-gate: the run wrote no report; see $log" >&2
    notify critical "microide perf gate" "run produced no report — see $log"
    return 2
  fi

  # --------------------------------------------------------------------
  # The report. This is the half TD-2026-08-06-141 says is easy to get
  # wrong: a scheduled run nobody reads is the same defect one layer up.
  # --------------------------------------------------------------------
  local summary="$run_dir/latest-summary.txt"
  {
    echo "microide perf gate"
    echo "  when:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "  commit:  ${commit}${dirty}"
    echo "  runner:  $RUNNER at ${ITERATIONS} iterations"
    echo "  verdict: $([[ $gate_rc -eq 0 ]] && echo PASS || echo FAIL)"
    echo "  report:  $report"
    echo "  log:     $log"
    echo
    # The gate's own near-miss summary, which is where a drift that has not yet
    # tripped anything shows up in a SINGLE run.
    grep -E '^\[perf\] (HEADROOM|headroom)' "$log" || true
    echo
    if [[ -n "$previous" ]]; then
      echo "--- drift vs $(basename "$previous") ---"
      NO_COLOR=1 python3 tools/perf-drift.py "$report" "$previous" 2>&1
    else
      echo "--- first recorded run: comparing against the committed baselines ---"
      NO_COLOR=1 python3 tools/perf-drift.py "$report" 2>&1
    fi
  } >"$summary"

  ln -sfn "$(basename "$report")" "$run_dir/latest.json"

  cat "$summary"

  local drift_rc=0
  if [[ -n "$previous" ]]; then
    python3 tools/perf-drift.py "$report" "$previous" --fail-on-drift >/dev/null 2>&1
    drift_rc=$?
  fi

  if (( gate_rc != 0 )); then
    notify critical "microide perf gate FAILED" "$(basename "$report") — tools/perf-gate.sh --status"
  elif (( drift_rc != 0 )); then
    notify normal "microide perf drift" "gate passed but something drifted — tools/perf-gate.sh --status"
  fi

  (( gate_rc != 0 )) && return 1
  (( drift_rc != 0 )) && return 1
  return 0
}

case "${1:-}" in
  --install-timer) cmd_install_timer ;;
  --status) cmd_status ;;
  --drift) cmd_drift ;;
  --help|-h) sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's|^# \?||' ;;
  *) cmd_run "$@" ;;
esac
