## ADDED Requirements

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
