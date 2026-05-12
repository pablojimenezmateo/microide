# bug-detection-tooling Specification

## Purpose
TBD - created by archiving change comprehensive-tech-debt-and-perf-harness. Update Purpose after archive.
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

#### Scenario: Long-running fuzz runs nightly
- **WHEN** the nightly CI job executes
- **THEN** each fuzz target SHALL run for an extended documented duration against the same corpus, and SHALL surface findings into a triage queue tracked in `docs/known-tech-debt.md`

### Requirement: Allocation-Counter Test Support

The repository SHALL provide an instrumented allocator that test fixtures can use to assert that specific code paths produce no heap allocations, and the allocator SHALL be enabled only in the perf-harness and test builds.

#### Scenario: Allocation counter is available in tests
- **WHEN** a test built with `MICROIDE_PERF_HARNESS_BUILD=ON` calls `Allocations::Snapshot()` before and after a code path
- **THEN** the snapshot delta SHALL reflect every heap allocation produced by that code path

#### Scenario: Production builds are unaffected
- **WHEN** the production binary is built without `MICROIDE_PERF_HARNESS_BUILD`
- **THEN** the allocator SHALL NOT be instrumented and SHALL NOT introduce overhead

### Requirement: Long-Soak Idle Run

A nightly CI job SHALL run a headless 8-hour idle scenario and SHALL fail if average CPU exceeds a documented threshold or RSS grows beyond a documented bound.

#### Scenario: Long-soak runs nightly
- **WHEN** the nightly CI job executes
- **THEN** the harness SHALL run the `long_soak_8h` scenario, SHALL parse RSS at the start, midpoint, and end of the run, SHALL parse CPU usage continuously, and SHALL fail the run on threshold breach

#### Scenario: Idle wake-ups are bounded
- **WHEN** the long-soak scenario reports SDL wake-ups
- **THEN** the count SHALL fit within a documented per-hour budget, and the run SHALL fail otherwise; this catches idle-CPU and accidental-poll regressions that shorter scenarios miss

### Requirement: Bug Triage Queue

Findings from sanitizers, fuzzers, and the long-soak SHALL be tracked in `docs/known-tech-debt.md` as a triage queue when they are not blocking the merge that surfaced them.

#### Scenario: Non-blocking finding
- **WHEN** a fuzzer or sanitizer surfaces an issue that is not reachable from real input or is not on the change's critical path
- **THEN** the issue SHALL be added to `docs/known-tech-debt.md` with reproduction steps and a severity assessment, and SHALL NOT block the merge that surfaced it

#### Scenario: Blocking finding
- **WHEN** a finding is reachable from real input, indicates memory unsafety, or affects a hot path under measurement
- **THEN** the merge SHALL block until the finding is fixed, and the corpus entry or sanitizer reproduction SHALL be committed alongside the fix

