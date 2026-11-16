# Development Workflow — the full guide

The short form agents read is `CLAUDE.md` § Development Workflow. This is the full
text it had, moved here verbatim on 2026-11-14: why the narrow inner loop is honest,
what each `tools/run-checks.sh` lane covers and why it exists, the sanitizer and
fuzz recipes.

Configure and build with the repo's CMake flow:

```bash
cmake -S . -B build
cmake --build build -j8
```

For the inner test loop, build only the two test binaries and run only their
shards. The narrow build skips the production `microide` executable and the bench
binaries (roughly halves the build), and the narrow test selection is what keeps it
HONEST:

```bash
cmake --build build --target microide_tests microide_kernel_tests -j8
ctest --test-dir build -R _tests_shard --output-on-failure -j$(nproc)
```

There are TWO test binaries. `microide_kernel_tests` holds the tests of the
SDL-free kernel (util, platform, project, compare, persistence, terminal) and
links `microide_kernel` alone — the link unit `microide-server` is built from — so
a test there that reaches for SDL or the shell fails to compile or link; move such
a test to `microide_tests` rather than adding the dependency. Focused filters work
the same on both (`./build/microide/microide_kernel_tests TerminalSession`).
`-R microide_tests_shard` on its own silently skips every kernel test.

`ctest` with no `-R` does **not** invoke just `microide_tests`. It also runs
`microide_perf_tests` (the `--smoke` perf gate), `microide_kernel_link_probe` and
the perf-fixture generators — none of which the narrow build rebuilds. Running
the full `ctest` after a narrow build therefore validates your change against
STALE copies of those binaries and reports green: `validation-traps.md` § "A lane
that narrows the build target but not the test selection" is the same trap, and
it was found in `run-checks.sh` before it was found here. A whole session's worth
of "31/31 passed" can mean the perf gate never ran.

So: the two lines above for the fast loop, and `tools/run-checks.sh tests` —
which builds every target `ctest` invokes — before concluding anything.

The build auto-uses **ccache** (compiler cache) and **ld.lld** (fast linker)
when installed; both are no-ops if absent. For the best inner-loop speed install
`ccache` once (`sudo apt install ccache`, then `ccache --max-size=10G`); the
shared `microide_core` object library means core compiles once across the
`microide`/`microide_tests`/`microide_perf` targets. `tools/run-checks.sh`
already exports the `CCACHE_SLOPPINESS` needed for ccache to cache PCH builds.

Run the full automated test suite with:

```bash
ctest --test-dir build --output-on-failure -j$(nproc)
```

The suite is registered as `MICROIDE_TEST_SHARDS` ctest tests (default 24), each
a `microide_tests --shard-index=I --shard-count=N` process running a
deterministic round-robin slice, so `ctest -jN` uses every core instead of
running the whole suite in one serial process. This is the dominant win for the
sanitizers, which used to run 18–30 min single-threaded. `run-checks.sh` passes
`-j` automatically (full width for plain runs, capped at 6 for the
memory-heavy sanitizers; override with `MICROIDE_CTEST_JOBS` /
`MICROIDE_CTEST_SAN_JOBS`). Set `-DMICROIDE_TEST_SHARDS=1` to get the old single
`microide_tests` test back. Run one shard directly with
`./build/microide/microide_tests --shard-index=0 --shard-count=24`.

Prefer the logging wrapper, which tees all build+test output to a deterministic
file under `/tmp` so results can be read back without rerunning:

```bash
tools/run-checks.sh tests   # -> /tmp/microide-tests.log
tools/run-checks.sh asan    # -> /tmp/microide-asan.log
tools/run-checks.sh ubsan   # -> /tmp/microide-ubsan.log
tools/run-checks.sh tsan    # -> /tmp/microide-tsan.log  (no sudo needed; see below)
tools/run-checks.sh perf-tests  # -> /tmp/microide-perf-tests.log (allocation counting armed)
tools/run-checks.sh perf-canary # -> /tmp/microide-perf-canary.log (proves the perf gate can fail)
tools/run-checks.sh clang-build # -> /tmp/microide-clang-build.log (2nd compiler, warnings-as-errors)
tools/run-checks.sh hardened    # -> /tmp/microide-hardened.log (_GLIBCXX_ASSERTIONS; catches library-precondition UB the sanitizers cannot)
tools/run-checks.sh tidy        # -> /tmp/microide-tidy.log (clang-tidy use-after-move + unchecked-optional-access; reviewed findings ratchet in tools/tidy-baseline.txt)
tools/run-checks.sh qa          # -> /tmp/microide-qa.log (GUI QA on Xvfb: scenario suite + 300-step monkey, redraw verifier on; see tools/qa/README.md)
tools/run-checks.sh qa-asan     # -> /tmp/microide-qa-asan.log (the GUI QA suite on the ASan build: a slow binary under a memory checker)
tools/run-checks.sh qa-remote   # -> /tmp/microide-qa-remote.log (remote-project scenarios against a loopback-only sshd run as you, then over a 60 ms each-way link; tools/qa/local-sshd.sh)
tools/run-checks.sh qa-wayland  # -> /tmp/microide-qa-wayland.log (the GUI QA suite on headless weston at scale 2; input over the control channel's pointer/key)
tools/run-checks.sh qa-scale    # -> /tmp/microide-qa-scale.log (scale-sensitive scenarios + a layout monkey at ui.scale 1.25 and 1.75)
tools/run-checks.sh wake-race   # -> /tmp/microide-wake-race.log (SDL's lost wake under contention, with and without LostWakeRelay)
tools/check-invariants-bite.sh  # every recorded negative control (tools/negative-controls/) must make its check fail
tools/run-checks.sh all     # all four in sequence
```

`clang-build` is the only lane that compiles the whole tree with clang and the only
one that sets `MICROIDE_WARNINGS_AS_ERRORS=ON`. Every other lane is GCC (the
sanitizer presets set no compiler), and `coverage` builds only `microide_tests` —
so `src/app/main.cpp`, `microide_perf` and the three bench binaries are covered by
nothing else. Run it after adding a source to a curated target list.

`perf-canary` is a vacuity probe for the perf gate: it runs `perf_gate_canary`
clean (must pass) and inflated 4x (must **fail**), so a gate that has stopped
gating is loud instead of green. See `dev-docs/project/validation-traps.md`
§ Perf-Gate Vacuity.

The default `tests` target leaves `MICROIDE_PERF_HARNESS_BUILD` OFF, which is what
arms the counting `operator new`/`delete`. Every `#if MICROIDE_PERF_HARNESS_BUILD`
assertion — all of `PluginPresentationAllocationTests` and
`EditorRenderViewModelAllocationTests`, plus parts of `TextViewportTests` — therefore
compiles to nothing there. Run `perf-tests` after touching a render, view-model, or
presentation hot path, or those "must not allocate" contracts go unchecked.

After a run, READ `/tmp/microide-<target>.log` instead of rebuilding and rerunning.

The QA monkey also runs continuously on the dev PC: `tools/soak/soak.py run` (a
`systemd --user` unit, `tools/soak/microide-soak.service`) follows `main` in
`build/wt-soak`, rotates jobs over builds, profiles, sizes and scales, and keeps a
findings ledger with a `FINDING.md` per finding. `tools/soak/soak.py status` /
`report` read it, `soak.py health` says whether every cell is taking steps, and
`tools/soak/AGENT.md` is the brief for fixing what it finds
(`dev-docs/project/safety-plan.md`). A third of its default-build jobs record the
app under `rr`; any monkey run or replay does with `--rr` (`MQA_RR=1`), and
`rr replay <out>/seed-N/rr/launch-0` then re-runs that exact execution.

Run focused tests with one or more substring filters:

```bash
./build/microide/microide_tests TextRenderer
./build/microide/microide_tests "WorkspaceShell/EditorDirty"
```

When a change affects performance-sensitive code, run relevant `dev-docs/performance/perf-harness.md` scenarios first, then use startup/runtime tracing docs for deeper diagnosis.

Sanitizer and fuzz workflows expected for risky changes:

```bash
cmake --preset microide-asan && cmake --build build/microide-asan -j8 && LSAN_OPTIONS=suppressions=$PWD/tests/lsan.supp ctest --test-dir build/microide-asan --output-on-failure
cmake --preset microide-ubsan && cmake --build build/microide-ubsan -j8 && ctest --test-dir build/microide-ubsan --output-on-failure
cmake --preset microide-tsan && cmake --build build/microide-tsan -j8
# LSan reads tests/lsan.supp (system-library leaks only); run-checks.sh fails a lane on ANY
# sanitizer report in its log, including one from a spawned microide-server.
# No sudo: setarch -R clears ASLR for this process only. `sudo sysctl
# vm.mmap_rnd_bits=28` is the machine-wide fallback if personality() is blocked.
setarch -R env TSAN_OPTIONS=suppressions=tests/tsan.supp \
  ctest --test-dir build/microide-tsan --output-on-failure
cmake -S . -B build/microide-fuzz -DMICROIDE_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/microide-fuzz -j8
./build/microide-fuzz/microide/PersistedRecordReaderFuzz -max_total_time=60 tests/fuzz/corpora/PersistedRecordReaderFuzz
```
