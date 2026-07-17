#!/usr/bin/env bash
#
# run-checks.sh — build and run microide tests / sanitizers, persisting all
# output to a deterministic file under /tmp (override with MICROIDE_LOG_DIR) so
# it can be read back later WITHOUT rebuilding and rerunning the whole suite.
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

# ccache caches PCH-using translation units only when told to tolerate the
# precompiled-header define/time-macro sloppiness. Harmless when ccache is not
# installed (CMake then skips the launcher entirely).
export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros}"

# Where build/test/sanitizer logs land. Defaults to /tmp (the documented
# location agents read back from); override with MICROIDE_LOG_DIR when /tmp is a
# small tmpfs or logs must survive a reboot.
LOG_DIR="${MICROIDE_LOG_DIR:-/tmp}"
mkdir -p "$LOG_DIR"

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
  local log="${LOG_DIR}/microide-tests.log"
  # Fast, build-free pre-check: every public doc must state the CMakeLists
  # version. Catches a manual version bump that forgot a doc surface before it
  # ships — see tools/check-doc-versions.sh. Cheap grep, so run it up front.
  bash tools/check-doc-versions.sh || {
    echo "run-checks: doc/version drift — fix before building (tests not run)" >&2
    return 1
  }
  # ctest in the default build only invokes the microide_tests binary (see
  # add_test in CMakeLists.txt), so scope the build to that target and its deps.
  # This skips the production microide executable and the bench binaries, which
  # the test run does not need — roughly halving inner-loop build work.
  run_logged "$log" bash -c '
    set -e
    cmake -S . -B build
    cmake --build build --target microide_tests -j'"$JOBS"'
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
  local log="${LOG_DIR}/microide-${san}.log"

  # Route the sanitizer runtime's own diagnostics into LOG_DIR as well. The tee'd
  # log is the primary artifact; log_path is belt-and-suspenders and appends a
  # ".<pid>" suffix per the sanitizer runtime.
  export ASAN_OPTIONS="halt_on_error=1:log_path=${LOG_DIR}/microide-asan-rt"
  export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=${LOG_DIR}/microide-ubsan-rt"
  export TSAN_OPTIONS="halt_on_error=1:suppressions=${REPO_ROOT}/tests/tsan.supp:log_path=${LOG_DIR}/microide-tsan-rt"

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
  local rt_files=("${LOG_DIR}"/microide-"${san}"-rt.*)
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

# Release test gate: run ctest against an ALREADY-CONFIGURED-AND-BUILT release tree
# (tools/release.sh builds Release+LTO in $MICROIDE_RELEASE_BUILD_DIR, default "build").
# Unlike check_tests this does NOT reconfigure the tree (that would clobber the Release
# config with a Debug one); it only builds the test target and runs ctest, preserving
# the wrapper's deterministic /tmp log so the release gate produces the same artifact
# agents/reviewers are told to read. (TD-2026-07-16-28.)
check_release() {
  local build_dir="${MICROIDE_RELEASE_BUILD_DIR:-build}"
  local log="${LOG_DIR}/microide-release.log"
  run_logged "$log" bash -c '
    set -e
    cmake --build '"$build_dir"' --target microide_tests -j'"$JOBS"'
    ctest --test-dir '"$build_dir"' --output-on-failure
  '
  local rc=$?
  echo "run-checks: release test gate finished (exit $rc); log at $log"
  return $rc
}

# libFuzzer gate for parser/decoder-adjacent changes. Configures a dedicated
# clang + MICROIDE_FUZZ=ON tree and runs each fuzz target for a bounded time
# against its committed corpus. `tools/run-checks.sh fuzz --list` configures and
# lists the targets without running them (a cheap smoke check). Any crash or
# sanitizer error fails the run. (TD-2026-07-17-049.)
check_fuzz() {
  local mode="${1:-run}"
  local build_dir="build/microide-fuzz"
  local log="${LOG_DIR}/microide-fuzz.log"
  # Per-target wall-clock budget (seconds). Keep small so the gate stays cheap;
  # override with MICROIDE_FUZZ_SECONDS for a deeper local campaign.
  local seconds="${MICROIDE_FUZZ_SECONDS:-30}"
  # Every libFuzzer executable in the tree. Keep in sync with add_executable(*Fuzz)
  # in CMakeLists.txt; check_fuzz --list prints exactly what was built.
  local targets=(
    PersistedRecordReaderFuzz DebugStateRecordFuzz GitBlameParserFuzz
    JsonValueParseFuzz PluginDisplayListParseFuzz SearchRegexFuzz
    SurfaceRasterDecodeFuzz TerminalCsiParserFuzz TerminalSessionOutputFuzz
    PieceTreeEquivalenceFuzz
  )

  run_logged "$log" bash -c '
    set -e
    cmake -S . -B '"$build_dir"' -DMICROIDE_FUZZ=ON \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    targets="'"${targets[*]}"'"
    cmake --build '"$build_dir"' -j'"$JOBS"' --target $targets
    mode="'"$mode"'"
    seconds="'"$seconds"'"
    if [[ "$mode" == "--list" ]]; then
      echo "=== fuzz targets built (list mode; no runs) ==="
      for t in $targets; do
        bin=$(find '"$build_dir"' -name "$t" -type f -perm -u+x | head -n1)
        echo "  $t -> ${bin:-<not found>}"
      done
      exit 0
    fi
    for t in $targets; do
      bin=$(find '"$build_dir"' -name "$t" -type f -perm -u+x | head -n1)
      corpus="tests/fuzz/corpora/$t"
      echo "=== fuzzing $t for ${seconds}s (corpus: $corpus) ==="
      mkdir -p "$corpus"
      "$bin" -max_total_time="$seconds" "$corpus"
    done
  '
  local rc=$?
  echo "run-checks: fuzz finished (exit $rc); log at $log"
  return $rc
}

usage() {
  echo "usage: tools/run-checks.sh {tests|asan|ubsan|tsan|release|fuzz|all}" >&2
  echo "       tools/run-checks.sh fuzz --list   # configure+build fuzz targets, list them, no runs" >&2
  exit 2
}

main() {
  [[ $# -ge 1 ]] || usage
  case "$1" in
    tests) check_tests ;;
    asan)  check_sanitizer asan ;;
    ubsan) check_sanitizer ubsan ;;
    tsan)  check_sanitizer tsan ;;
    release) check_release ;;
    fuzz)  check_fuzz "${2:-run}" ;;
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
