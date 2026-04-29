## ADDED Requirements

### Requirement: Reader And Importer Survive Adversarial Input

`PersistedRecordReader` and the one-shot legacy importer SHALL survive truncated input, swapped or unknown record tags, length fields that exceed the available buffer, and CRC-mismatched bodies, without aborting, without out-of-bounds reads, and without unbounded allocation. A libFuzzer harness SHALL exercise both paths in CI.

#### Scenario: Truncated input is rejected cleanly
- **WHEN** the reader is given fewer bytes than the header or record framing requires
- **THEN** it SHALL return a structured decode error, SHALL NOT abort, SHALL NOT read past the buffer end, and SHALL release any partially-allocated records

#### Scenario: Length field exceeds remaining buffer
- **WHEN** a record's length field claims more bytes than remain in the buffer
- **THEN** the reader SHALL return a structured decode error and SHALL NOT attempt the read

#### Scenario: CRC mismatch falls back or fails closed
- **WHEN** the body CRC does not match the header value
- **THEN** the reader SHALL fall back to the most recent valid `<file>.bak` if present, or SHALL fail closed with a structured error if no valid backup exists, and in neither case SHALL it expose partially-decoded data to the rest of the application

#### Scenario: Fuzz CI runs on every merge candidate
- **WHEN** CI builds with `MICROIDE_FUZZ=ON`
- **THEN** the `PersistedRecordReaderFuzz` and `LegacyImporterFuzz` targets SHALL run for at least 60 seconds against the committed corpus, and SHALL fail the merge on any crash or sanitizer error

### Requirement: One-Shot Legacy Importer Has A Documented End-Of-Life

The one-shot legacy importer added in `comprehensive-tech-debt-cleanup` SHALL be scheduled for removal in the release-after-next, and the change that removes it SHALL also delete remaining `<file>.legacy` files written by the original migration.

#### Scenario: End-of-life is scheduled in this change
- **WHEN** this change lands
- **THEN** the change record SHALL include a `legacy-persistence-cleanup` follow-up entry naming the target release and the files to be removed (`WorkspacePersistenceLegacyFormat.{h,cpp}`, the importer call sites, and any `<file>.legacy` files still present in the user data directory at upgrade time)

#### Scenario: End-of-life cleanup is gated on harness baseline
- **WHEN** the follow-up change runs
- **THEN** the harness `cold_startup_*` scenarios SHALL be green on the same release, and the legacy importer SHALL be deleted in one commit; partial deletions are not allowed
