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
#   tools/run-checks.sh perf-tests # tests with allocation counting -> /tmp/microide-perf-tests.log
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

# Default build parallelism to the full core count (was a hardcoded 8, which
# left cores idle on a 12-thread box). Override with MICROIDE_BUILD_JOBS.
JOBS="${MICROIDE_BUILD_JOBS:-$(nproc 2>/dev/null || echo 8)}"

# ctest parallelism. The suite is registered as MICROIDE_TEST_SHARDS ctest
# tests (see CMakeLists.txt), so `ctest -jN` runs N shard processes at once —
# the change that turns an 18-30 min serial sanitizer run into a few minutes.
# Each sanitizer process has a large RSS, so cap concurrency below the build
# job count to avoid swap thrash; plain (non-sanitized) runs use the full width.
CTEST_JOBS="${MICROIDE_CTEST_JOBS:-$JOBS}"
CTEST_SAN_JOBS="${MICROIDE_CTEST_SAN_JOBS:-$(( JOBS > 6 ? 6 : JOBS ))}"

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
    ctest --test-dir build --output-on-failure -j'"$CTEST_JOBS"'
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

  # TSAN aborts at startup ("unexpected memory mapping") when the kernel's ASLR
  # entropy is higher than its shadow-mapping assumptions. The documented fix is
  # `sudo sysctl vm.mmap_rnd_bits=28`, which needs root and changes the setting
  # machine-wide for every process.
  #
  # `setarch -R` gets the same result with neither: it clears ASLR for one child
  # via personality(ADDR_NO_RANDOMIZE), so only the ctest process tree is
  # affected and nothing needs elevating. Some sandboxes block that personality
  # bit, so probe it and fall back to the sysctl advice rather than failing.
  local test_prefix=""
  if [[ "$san" == "tsan" ]]; then
    if command -v setarch >/dev/null 2>&1 && setarch -R true >/dev/null 2>&1; then
      test_prefix="setarch -R "
      echo "run-checks: TSAN running under 'setarch -R' (per-process ASLR off; no sudo needed)." >&2
    else
      echo "run-checks: 'setarch -R' unavailable here, so TSAN needs the machine-wide" >&2
      echo "            'sudo sysctl vm.mmap_rnd_bits=28'. If TSAN aborts at startup, run that." >&2
    fi
  fi

  run_logged "$log" bash -c '
    set -e
    cmake --preset '"$preset"'
    cmake --build '"$build_dir"' -j'"$JOBS"'
    '"$test_prefix"'ctest --test-dir '"$build_dir"' --output-on-failure -j'"$CTEST_SAN_JOBS"'
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
    ctest --test-dir '"$build_dir"' --output-on-failure -j'"$CTEST_JOBS"'
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
    PieceTreeEquivalenceFuzz JsonRpcMessageFramingFuzz
    GitPorcelainV2ParserFuzz
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

# Allocation-discipline gate. A large body of assertions -- all of
# PluginPresentationAllocationTests and EditorRenderViewModelAllocationTests, plus
# parts of TextViewportTests -- is wrapped in `#if MICROIDE_PERF_HARNESS_BUILD`,
# because that define is what arms the counting operator new/delete in
# tests/perf/AllocationCounter.cpp. The default `tests` target leaves it OFF, so
# those blocks compile to nothing and every "this path must not allocate"
# assertion is silently skipped. Nothing else in the documented flow turned it on,
# which meant the allocation contracts were never actually checked anywhere.
#
# The microide-perf preset sets it tree-wide, and that tree registers the same
# 24 test shards. Run them here. `-R microide_tests_shard` deliberately excludes
# the microide_perf_tests scenario run -- that is the separate perf-baseline gate
# with its own fixtures, not part of this check. NOTE: RelWithDebInfo + LTO, so
# the first build is slow (several minutes); the ccache-warm rebuild is not.
check_perf_tests() {
  local build_dir="build/microide-perf-make"
  local log="${LOG_DIR}/microide-perf-tests.log"
  run_logged "$log" bash -c '
    set -e
    cmake --preset microide-perf
    # Build microide_perf too: NOTHING in the default flow compiles it, so a perf
    # scenario can stop building (a removed #if, a source dropped from its curated
    # list) and every other check stays green. Building it here is the cheap guard.
    cmake --build '"$build_dir"' --target microide_tests microide_perf -j'"$JOBS"'
    ctest --test-dir '"$build_dir"' -R microide_tests_shard --output-on-failure -j'"$CTEST_JOBS"'
  '
  local rc=$?
  echo "run-checks: perf-tests finished (exit $rc); log at $log"
  return $rc
}

# Line-coverage gate with per-area floors.
#
# This is the one measurement discipline the repo did not have. Wall time,
# allocation counts, three sanitizers, twelve fuzz targets and a vacuity-probed
# architecture lint were all gated; nothing measured whether a line ever ran. The
# cost showed up as WorkspaceShellRenderMerge.cpp — 568 lines, 0 of 11 functions
# executed by 2611 tests, on the surface the product is built around. Sanitizers
# cannot find a defect in code that never executes.
#
# clang source-based coverage (-fcoverage-mapping), not gcov: it gives exact
# region counts and llvm-cov ships in the same apt package set CI already installs.
# CMAKE_CXX_SCAN_FOR_MODULES=OFF because Ninja otherwise wants clang-scan-deps,
# which is not part of that set.
check_coverage() {
  local build_dir="build/microide-coverage"
  local log="${LOG_DIR}/microide-coverage.log"
  local update="${1:-}"
  run_logged "$log" bash -c '
    set -e
    cmake -S . -B '"$build_dir"' -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
      -DMICROIDE_TEST_SHARDS=1 \
      -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping -O0 -g0" \
      -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate"
    cmake --build '"$build_dir"' --target microide_tests -j'"$JOBS"'

    # One process, not the 24 shards: a merged multi-shard profile is fine for the
    # numbers but slower to produce, and coverage is not a latency-sensitive lane.
    cd '"$build_dir"'
    LLVM_PROFILE_FILE=coverage.profraw ./microide/microide_tests
    llvm-profdata merge -sparse coverage.profraw -o coverage.profdata
    llvm-cov report ./microide/microide_tests \
      -instr-profile=coverage.profdata \
      -ignore-filename-regex="(tests/|third_party/|/usr/)" > coverage-report.txt
    cd - >/dev/null

    tools/check-coverage.py '"$build_dir"'/coverage-report.txt '"$update"'
  '
  local rc=$?
  echo "run-checks: coverage finished (exit $rc); log at $log"
  return $rc
}

usage() {
  echo "usage: tools/run-checks.sh {tests|asan|ubsan|tsan|release|fuzz|perf-tests|coverage|all}" >&2
  echo "       tools/run-checks.sh coverage      # line coverage + per-area floors" >&2
  echo "       tools/run-checks.sh coverage --update-floors  # re-record the floors" >&2
  echo "       tools/run-checks.sh perf-tests    # tests with allocation counting armed" >&2
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
    perf-tests) check_perf_tests ;;
    coverage) check_coverage "${2:-}" ;;
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
