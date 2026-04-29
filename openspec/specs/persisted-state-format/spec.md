# persisted-state-format Specification

## Purpose
TBD - created by archiving change comprehensive-tech-debt-cleanup. Update Purpose after archive.
## Requirements
### Requirement: Single Structured Format For Workspace State

Project workspace state, user configuration, workspace session restore data, and chat conversation data SHALL be persisted using one shared structured format with a typed record stream and an explicit schema-version field. There SHALL be exactly one reader implementation and one writer implementation in the source tree.

#### Scenario: Single reader/writer
- **WHEN** the source tree is built
- **THEN** all four persisted artifacts SHALL be produced and consumed by `PersistedRecordReader` and `PersistedRecordWriter`, and no other parser SHALL exist for these artifacts

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

### Requirement: One-Shot Migration From Legacy Text Format

The application SHALL migrate existing text-command-based workspace state, user configuration, session restore, and conversation files to the structured format on first launch and SHALL NOT retain a runtime fallback to the legacy parser.

#### Scenario: First launch with legacy files
- **WHEN** the application starts and detects legacy `project.state`, `user.config`, `session.workspace`, or `chat.conversations` files
- **THEN** it SHALL import each file once, write the equivalent structured file via the atomic writer, verify the round-trip by re-reading the new file, and only then rename the legacy file to `<name>.legacy`

#### Scenario: Legacy reader is deleted
- **WHEN** the migration step lands
- **THEN** the source tree SHALL NOT contain any code path that parses the legacy command-style format outside the one-shot importer

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

