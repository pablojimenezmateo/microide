# persisted-state-format Specification

## Purpose
Define the structured persistence contract for workspace state, project state, user configuration,
and session restore data. Persisted artifacts must use one typed record format, atomic writes,
explicit compatibility rules, non-throwing parsing, debug inspection, and bounded behavior under
corrupt or adversarial input.

## Requirements
### Requirement: Single Structured Format For Workspace State

Project workspace state, user configuration, and workspace session restore data SHALL be persisted using one shared structured format with a typed record stream and an explicit schema-version field. There SHALL be exactly one reader implementation and one writer implementation in the source tree.

#### Scenario: Single reader/writer
- **WHEN** the source tree is built
- **THEN** all persisted artifacts SHALL be produced and consumed by `PersistedRecordReader` and `PersistedRecordWriter`, and no other parser SHALL exist for these artifacts

#### Scenario: Schema version is explicit
- **WHEN** any persisted file is opened for reading
- **THEN** the reader SHALL read a 4-byte format-version field as the first decoded value after the magic header, and SHALL reject the file with a structured error if the major version is unknown

### Requirement: Atomic Write With Integrity Check

Persisted files SHALL be written atomically and SHALL include an integrity check that the reader verifies before exposing parsed data to the rest of the application.

#### Scenario: Atomic write
- **WHEN** any persisted artifact is saved
- **THEN** the writer SHALL write to a sibling temporary file, call `fsync` (or platform equivalent), rename it over the destination, and SHALL NOT leave the destination in a partially-written state on crash

#### Scenario: CRC verification on read
- **WHEN** a persisted file is read
- **THEN** the reader SHALL verify a CRC32C of the body against the value in the header, and on mismatch SHALL fall back to the most recent valid `<file>.bak` backup or report a structured error if no valid backup exists

### Requirement: Forward And Backward Compatibility Rules

The format SHALL support adding new typed records without breaking older readers, and SHALL support reading older files written by previous versions of the application.

#### Scenario: Unknown record tag is skipped
- **WHEN** a reader encounters a record tag it does not know
- **THEN** it SHALL skip exactly the bytes indicated by the record's length field and continue reading subsequent records, optionally logging a warning, but SHALL NOT abort

#### Scenario: Older minor version is accepted
- **WHEN** a reader opens a file written by an older minor version of the same major
- **THEN** it SHALL parse all known tags and SHALL fill missing fields with documented defaults

### Requirement: Legacy Text Format Is Fully Retired

The one-shot legacy-text-format importer has been removed (the source tree no
longer contains any legacy command-style parser or importer). The application
SHALL NOT parse, import, or retain any runtime fallback to the legacy
command-style format. This supersedes the earlier one-shot-migration requirement,
which has been satisfied and retired.

#### Scenario: No legacy parser or importer exists
- **WHEN** the architectural-lint test runs over `src/`, `tests/`, and `tools/`
- **THEN** it SHALL fail the build if any legacy persistence symbol
  (`WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`,
  `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`,
  `ParseProjectSessionText`, `ParseWorkspaceSessionText`) or a one-shot importer
  path reappears

### Requirement: Non-Throwing Typed Token Parser

All numeric and string-token parsing across the source tree SHALL use a single non-throwing parser layer based on `std::from_chars`. There SHALL be no `try`/`catch` blocks wrapping `std::stoi`, `std::stoll`, `std::stoull`, `std::stof`, or `std::stod` in workspace, persistence, or related code.

#### Scenario: Parse helpers exist and are used
- **WHEN** a caller needs to parse a numeric token
- **THEN** it SHALL call into `util/Parse.*` (`ParseInt`, `ParseInt64`, `ParseSize`, `ParseFloat`) and SHALL handle the returned `std::optional` directly

#### Scenario: Lint rejects throwing numeric parsing
- **WHEN** the architectural-lint test runs
- **THEN** it SHALL fail the build if any `try` block in `src/` contains a call to `std::sto*`

### Requirement: Debug Inspection Of Persisted State

A debug tooling path SHALL exist to dump the contents of any persisted file in human-readable form so developers can inspect state without external tooling.

#### Scenario: Dump subcommand
- **WHEN** a developer runs the `microide` binary (or test tool) with the documented dump subcommand against a persisted file
- **THEN** the tool SHALL print each record's tag, length, and decoded payload in a stable text form suitable for diff

### Requirement: Reader Survives Adversarial Input

`PersistedRecordReader` SHALL survive truncated input, swapped or unknown record tags, length fields that exceed the available buffer, and CRC-mismatched bodies, without aborting, without out-of-bounds reads, and without unbounded allocation. It SHALL also reject a non-regular path (directory, FIFO, device) before opening it. A libFuzzer harness SHALL exercise the reader in CI.

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
- **THEN** the `PersistedRecordReaderFuzz` target SHALL run for at least 60 seconds against the committed corpus, and SHALL fail the merge on any crash or sanitizer error

### Requirement: One-Shot Legacy Importer Removal Is Complete

The one-shot legacy importer added in `comprehensive-tech-debt-cleanup` has been
removed, along with its `LegacyImporterFuzz` target. No further end-of-life work
is pending; the architectural-lint ban on legacy persistence symbols keeps it
from returning.

#### Scenario: End-of-life cleanup is complete
- **WHEN** the source tree is inspected
- **THEN** `WorkspacePersistenceLegacyFormat.{h,cpp}`, every importer call site, and the `LegacyImporterFuzz` target SHALL be absent, and the architectural-lint ban on legacy persistence symbols SHALL keep them from returning

### Requirement: User-Config Records New Polish Setting Keys

The user-config persisted artifact SHALL accept and round-trip the following new typed records, with `PersistedRecordReader` and `PersistedRecordWriter` as the only implementations: `editor.font_family`, `editor.font_size`, `editor.line_endings`, `editor.save.trim_trailing_whitespace`, `editor.save.ensure_final_newline`, `editor.format_on_save`, `editor.autosave`, `editor.hover_delay_ms`, `ui.layout_mode`, `ui.layout_compact_breakpoint_px`, `ui.scrollbar_size`, `ui.resize_handle_size`, `ui.show_status_bar`, `terminal.shell`, `terminal.font_size`, `diagnostics.min_severity`.

#### Scenario: New keys round-trip
- **WHEN** any new key listed above is written to the user-config artifact and then read back
- **THEN** the value SHALL parse back to the same `SettingValue` variant, and `PersistedRecordReader` SHALL emit no warning

#### Scenario: Missing key falls back to default
- **WHEN** a user-config artifact written by a prior version of MicroIDE is read on first launch after this change
- **THEN** every new key SHALL be treated as absent, the corresponding `SettingSpec` default SHALL be used, and `PersistedRecordReader` SHALL NOT abort or warn for the missing keys

### Requirement: Legacy AI Records Are Tolerated But Not Rewritten

The persisted-state reader SHALL tolerate legacy AI-related records that may exist in historical workspace/session files, and the writer SHALL NOT emit new AI conversation/provider records.

#### Scenario: Workspace with legacy AI records is opened and saved
- **WHEN** a persisted workspace/session payload contains historical AI conversation or provider fields
- **THEN** load SHALL complete without failure, and the next successful save SHALL omit AI-only records

### Requirement: Reader Survives Unknown Setting Keys

The reader SHALL apply the existing forward-compatibility rule (skip unknown record tags) to every new setting key. Older builds reading user-config files written by newer builds SHALL skip the new keys without warning to the user.

#### Scenario: Older build reads newer config
- **WHEN** a user-config artifact written by a build that supports the new setting keys is opened by an older build
- **THEN** the older build SHALL skip the unknown keys per the existing `Forward And Backward Compatibility Rules` requirement and SHALL load the rest of the artifact normally

### Requirement: Project State Persists Branch Review Records
The project-scoped persisted artifact SHALL accept and round-trip typed branch review records for review target identity, reviewed file entries, reviewed hunk entries, optional note text, hunk content identity, and changed-since-reviewed metadata. These records SHALL be read and written only through `PersistedRecordReader`, `PersistedRecordWriter`, and `PersistenceService`.

#### Scenario: Review state round-trips
- **WHEN** a project saves reviewed file, reviewed hunk, and note records and then reloads the project
- **THEN** the same review state SHALL be restored without invoking a bespoke parser

#### Scenario: Older build skips review records
- **WHEN** an older reader encounters branch review records it does not understand
- **THEN** it SHALL skip them using the existing unknown-record compatibility rule and SHALL load the rest of the project state normally

#### Scenario: Corrupt review record is rejected safely
- **WHEN** a branch review record has an invalid length or malformed hunk identity
- **THEN** the persisted-state reader SHALL fail the record or artifact according to structured error rules and SHALL NOT expose partially decoded review state
