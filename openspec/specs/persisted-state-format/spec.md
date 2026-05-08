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

### Requirement: User-Config Records New Polish Setting Keys

The user-config persisted artifact SHALL accept and round-trip the following new typed records, with `PersistedRecordReader` and `PersistedRecordWriter` as the only implementations: `editor.font_family`, `editor.font_size`, `editor.line_endings`, `editor.trim_trailing_whitespace`, `editor.insert_final_newline`, `editor.format_on_save`, `editor.autosave`, `editor.hover_delay_ms`, `ui.layout_mode`, `ui.layout_compact_breakpoint_px`, `ui.scrollbar_size`, `ui.resize_handle_size`, `ui.show_status_bar`, `terminal.shell`, `terminal.font_size`, `diagnostics.min_severity`.

#### Scenario: New keys round-trip
- **WHEN** any new key listed above is written to the user-config artifact and then read back
- **THEN** the value SHALL parse back to the same `SettingValue` variant, and `PersistedRecordReader` SHALL emit no warning

#### Scenario: Missing key falls back to default
- **WHEN** a user-config artifact written by a prior version of MicroIDE is read on first launch after this change
- **THEN** every new key SHALL be treated as absent, the corresponding `SettingSpec` default SHALL be used, and `PersistedRecordReader` SHALL NOT abort or warn for the missing keys

### Requirement: AI Provider Configuration Has A Persisted Section

The user-config artifact SHALL include a typed `ai_provider_config` section keyed by `provider_id`. Each entry SHALL store the chosen `model_id` and a boolean `is_default`. The project-state artifact SHALL store an optional `ai_provider_override` that takes precedence while that project is active. Secrets SHALL NOT live in either artifact and SHALL continue to flow through the existing secret store via `WorkspaceAuthProvider`.

#### Scenario: Provider/model selection survives restart
- **WHEN** the user picks a model for a provider in the picker overlay and restarts the application
- **THEN** the next launch SHALL surface the same model selection in the picker without prompting

#### Scenario: Project override defeats user default
- **WHEN** a project has an `ai_provider_override` and the user's default provider differs
- **THEN** chat and inline completion in that project SHALL use the override, and switching to a project without an override SHALL revert to the user default

#### Scenario: Secrets are not persisted in plain text
- **WHEN** the user-config or project-state artifacts are inspected on disk after configuring a provider that requires an API key
- **THEN** neither artifact SHALL contain the secret value, and the secret SHALL only be reachable through the host secret store

### Requirement: Reader Survives Unknown Setting Keys

The reader SHALL apply the existing forward-compatibility rule (skip unknown record tags) to every new setting key. Older builds reading user-config files written by newer builds SHALL skip the new keys without warning to the user.

#### Scenario: Older build reads newer config
- **WHEN** a user-config artifact written by a build that supports the new setting keys is opened by an older build
- **THEN** the older build SHALL skip the unknown keys per the existing `Forward And Backward Compatibility Rules` requirement and SHALL load the rest of the artifact normally

