## MODIFIED Requirements

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

## REMOVED Requirements

### Requirement: Long-Soak Idle Run
**Reason**: Periodic/nightly workflows are being removed; this requirement hard-codes nightly schedule behavior that is no longer allowed by CI trigger policy.
**Migration**: Run long-soak validation through manual dispatch or bounded merge-candidate checks documented by the CI reliability capability, without any periodic `schedule` trigger.
