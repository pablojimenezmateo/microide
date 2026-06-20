#!/usr/bin/env bash
#
# run-checks.sh — build and run microide tests / sanitizers, persisting all
# output to a deterministic file under /tmp so it can be read back later
# WITHOUT rebuilding and rerunning the whole suite.
#
# Usage:
#   tools/run-checks.sh tests   # plain Debug build + ctest     -> /tmp/microide-tests.log
#   tools/run-checks.sh asan    # AddressSanitizer build + ctest -> /tmp/microide-asan.log
#   tools/run-checks.sh ubsan   # UndefinedBehavior build + ctest-> /tmp/microide-ubsan.log
#   tools/run-checks.sh tsan    # ThreadSanitizer build + ctest  -> /tmp/microide-tsan.log
#   tools/run-checks.sh all     # tests, asan, ubsan, tsan in sequence
#
# The full console output (build + test) is tee'd to the log file; the script's
# exit status is the real status of the underlying command (via PIPESTATUS), so
# CI and callers still see pass/fail while the file captures the details.
#
# IMPORTANT for agents: after a run, READ /tmp/microide-<target>.log instead of
# rerunning. Each log starts with a header naming the build it came from.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

JOBS="${MICROIDE_BUILD_JOBS:-8}"

# Run a command, tee combined stdout+stderr to $1, and return the command's
# real exit status (not tee's).
run_logged() {
  local log="$1"; shift
  {
    echo "=== run-checks.sh: $* ==="
    echo "=== date:   $(date -u '+%Y-%m-%dT%H:%M:%SZ') ==="
    echo "=== commit: $(git rev-parse --short HEAD 2>/dev/null || echo unknown) ==="
    echo "=== branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown) ==="
    echo
    "$@"
  } 2>&1 | tee "$log"
  return "${PIPESTATUS[0]}"
}

check_tests() {
  local log="/tmp/microide-tests.log"
  run_logged "$log" bash -c '
    set -e
    cmake -S . -B build
    cmake --build build -j'"$JOBS"'
    ctest --test-dir build --output-on-failure
  '
  local rc=$?
  echo "run-checks: tests finished (exit $rc); log at $log"
  return $rc
}

# $1 = sanitizer name (asan|ubsan|tsan)
check_sanitizer() {
  local san="$1"
  local preset="microide-${san}"
  local build_dir="build/${preset}"
  local log="/tmp/microide-${san}.log"

  # Route the sanitizer runtime's own diagnostics into /tmp as well. The tee'd
  # log is the primary artifact; log_path is belt-and-suspenders and appends a
  # ".<pid>" suffix per the sanitizer runtime.
  export ASAN_OPTIONS="halt_on_error=1:log_path=/tmp/microide-asan-rt"
  export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=/tmp/microide-ubsan-rt"
  export TSAN_OPTIONS="halt_on_error=1:suppressions=${REPO_ROOT}/tests/tsan.supp:log_path=/tmp/microide-tsan-rt"

  if [[ "$san" == "tsan" ]]; then
    echo "run-checks: TSAN requires 'sudo sysctl vm.mmap_rnd_bits=28' before running." >&2
    echo "            If TSAN aborts at startup, run that and retry." >&2
  fi

  run_logged "$log" bash -c '
    set -e
    cmake --preset '"$preset"'
    cmake --build '"$build_dir"' -j'"$JOBS"'
    ctest --test-dir '"$build_dir"' --output-on-failure
  '
  local rc=$?

  # The sanitizer runtime writes its report (the stack-use-after-scope / leak /
  # data-race dump) to the log_path file, NOT to stderr, so the tee'd log above
  # never sees it. Fold every per-pid report into the main log so a single file
  # has the full failure, then clean the scratch files up.
  shopt -s nullglob
  local rt_files=(/tmp/microide-"${san}"-rt.*)
  if (( ${#rt_files[@]} )); then
    {
      echo
      echo "=== ${san} sanitizer runtime reports (${#rt_files[@]} file(s)) ==="
      cat "${rt_files[@]}"
    } >> "$log"
    rm -f "${rt_files[@]}"
  fi
  shopt -u nullglob

  echo "run-checks: ${san} finished (exit $rc); log at $log"
  return $rc
}

usage() {
  echo "usage: tools/run-checks.sh {tests|asan|ubsan|tsan|all}" >&2
  exit 2
}

main() {
  [[ $# -eq 1 ]] || usage
  case "$1" in
    tests) check_tests ;;
    asan)  check_sanitizer asan ;;
    ubsan) check_sanitizer ubsan ;;
    tsan)  check_sanitizer tsan ;;
    all)
      local overall=0
      check_tests            || overall=1
      check_sanitizer asan   || overall=1
      check_sanitizer ubsan  || overall=1
      check_sanitizer tsan   || overall=1
      echo "run-checks: all finished (overall exit $overall)"
      return $overall
      ;;
    *) usage ;;
  esac
}

main "$@"
