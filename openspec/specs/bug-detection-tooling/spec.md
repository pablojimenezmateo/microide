# bug-detection-tooling Specification

## Purpose
Define the regression-detection tooling contract for defects that ordinary unit tests often miss:
sanitizer builds, parser fuzzers, allocation assertions, and triage rules for findings that surface
outside the immediate merge path. This spec keeps those tools mandatory, bounded, and connected to
the documented debt queue instead of relying on ad hoc local runs.
## Requirements
### Requirement: Sanitizer Build Variants And CI Coverage

The repository SHALL ship CMake presets for ASAN, UBSAN, and TSAN sanitizer builds, and CI SHALL run `microide_tests` under each preset on every merge candidate.

#### Scenario: Sanitizer presets exist
- **WHEN** a developer configures the project
- **THEN** the presets `microide-asan`, `microide-ubsan`, and `microide-tsan` SHALL be available, and each SHALL flip the appropriate `-fsanitize=` and link flags without requiring manual flag composition

#### Scenario: CI runs all three sanitizers
- **WHEN** a CI build executes
- **THEN** three matrix entries SHALL run `microide_tests` under ASAN, UBSAN, and TSAN respectively, and SHALL fail the merge if any sanitizer reports an error

#### Scenario: TSAN catches the LspClient race
- **WHEN** the TSAN CI variant runs the workspace test suite
- **THEN** the previously-known `WorkspaceLspClient` race SHALL be reported, the change SHALL fix it, and TSAN SHALL flip from soft-fail to hard-fail in the same change once the run is clean

### Requirement: Fuzzing Harnesses For Parsers

The repository SHALL ship libFuzzer entry points for the typed persistence reader, the search regex compiler, and the git-blame parser, and CI SHALL run each fuzzer for a bounded time on every merge candidate. (The legacy persistence importer was removed in the 2026-04-29 cleanup; its fuzz target was retired along with the importer.)

#### Scenario: Fuzz targets exist
- **WHEN** the build is configured with `MICROIDE_FUZZ=ON`
- **THEN** the binaries `PersistedRecordReaderFuzz`, `SearchRegexFuzz`, and `GitBlameParserFuzz` SHALL build and SHALL accept libFuzzer input on standard input

#### Scenario: Fuzz CI runs on every merge candidate
- **WHEN** a CI build executes
- **THEN** each fuzz target SHALL run for at least 60 seconds against the committed corpus, SHALL fail the merge on a crash or sanitizer error, and SHALL NOT block on a timeout

#### Scenario: Crash corpora are committed
- **WHEN** a fuzzer finds a reproducible crash
- **THEN** the minimized input SHALL be added to `tests/fuzz/corpora/<target>/`, the underlying bug SHALL be fixed in the same change, and the corpus entry SHALL remain as a regression seed

#### Scenario: Extended fuzz runs are manually triggerable
- **WHEN** maintainers need deeper fuzz coverage beyond merge-candidate budgets
- **THEN** extended fuzz runs SHALL execute through an explicit manual workflow trigger and SHALL NOT rely on periodic `schedule` execution

### Requirement: Allocation-Counter Test Support

The repository SHALL provide an instrumented allocator that test fixtures can use to assert that specific code paths produce no heap allocations, and the allocator SHALL be enabled only in the perf-harness and test builds.

#### Scenario: Allocation counter is available in tests
- **WHEN** a test built with `MICROIDE_PERF_HARNESS_BUILD=ON` calls `Allocations::Snapshot()` before and after a code path
- **THEN** the snapshot delta SHALL reflect every heap allocation produced by that code path

#### Scenario: Production builds are unaffected
- **WHEN** the production binary is built without `MICROIDE_PERF_HARNESS_BUILD`
- **THEN** the allocator SHALL NOT be instrumented and SHALL NOT introduce overhead

### Requirement: Bug Triage Queue

Findings from sanitizers, fuzzers, and the long-soak SHALL be tracked in `dev-docs/project/known-tech-debt.md` as a triage queue when they are not blocking the merge that surfaced them.

#### Scenario: Non-blocking finding
- **WHEN** a fuzzer or sanitizer surfaces an issue that is not reachable from real input or is not on the change's critical path
- **THEN** the issue SHALL be added to `dev-docs/project/known-tech-debt.md` with reproduction steps and a severity assessment, and SHALL NOT block the merge that surfaced it

#### Scenario: Blocking finding
- **WHEN** a finding is reachable from real input, indicates memory unsafety, or affects a hot path under measurement
- **THEN** the merge SHALL block until the finding is fixed, and the corpus entry or sanitizer reproduction SHALL be committed alongside the fix

### Requirement: GUI QA Lane

`tools/run-checks.sh` SHALL provide a `qa` lane that builds the application, runs the local GUI QA scenario suite and a fixed-seed monkey run headlessly with the redraw verifier on, writes its log to `/tmp/microide-qa.log`, and fails on any finding. When Xvfb, xdotool or xwd is missing the lane SHALL fail with a message naming the missing tool rather than pass. The remote suite SHALL run only when `MQA_REMOTE_HOST` is set.

#### Scenario: The lane fails on a finding
- **WHEN** `tools/run-checks.sh qa` runs and any scenario or the monkey records a finding
- **THEN** the lane exits non-zero and the log names the finding, the action before it, and the screenshot path

#### Scenario: A missing tool is not a pass
- **WHEN** the lane runs on a machine without Xvfb
- **THEN** it exits non-zero naming Xvfb

### Requirement: One headless driver

Headless drivers of the shipped binary (random control commands, pointer and keyboard input, repro scenarios) SHALL live in `tools/qa` on one launcher, one isolation recipe and one oracle set. The former `tools/fuzz-control-commands.py` and `tools/repro/overlay-backdrop-repro.sh` SHALL be removed once their function is reachable from `tools/qa/run.py`, and documents that named them SHALL name their replacement.

#### Scenario: The command fuzzer keeps its seeds
- **WHEN** `tools/qa/run.py monkey --profile commands --seed N` runs against the ASAN binary
- **THEN** it drives the same command generators with on-disk churn as the former script did for seed N, and a sanitizer report fails the run

