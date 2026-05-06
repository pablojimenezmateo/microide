## MODIFIED Requirements

### Requirement: One-Shot Legacy Importer Has A Documented End-Of-Life

The one-shot legacy importer added in `comprehensive-tech-debt-cleanup` SHALL be deleted in this change. The deletion SHALL remove `WorkspacePersistenceLegacyFormat.{h,cpp}`, the importer call sites in `PersistenceService.cpp`, the unit-test fixtures that exercise the legacy parser, and any `<file>.legacy` files written by the original migration during first launch after the upgrade.

#### Scenario: Legacy module is deleted in one commit
- **WHEN** this change lands
- **THEN** the source tree SHALL NOT contain `src/workspace/WorkspacePersistenceLegacyFormat.h` or `src/workspace/WorkspacePersistenceLegacyFormat.cpp`, and `PersistenceService.cpp` SHALL NOT call `Parse*Text` or `Serialize*` legacy helpers, and the deletion SHALL be a single commit per the existing single-commit-deletion rule

#### Scenario: Legacy artifact files are removed at first launch
- **WHEN** the application starts for the first time on a build that contains this change
- **THEN** it SHALL detect any `project.state.legacy`, `user.config.legacy`, `session.workspace.legacy`, or `chat.conversations.legacy` files in the user data directory and SHALL delete them after confirming the structured equivalents exist

#### Scenario: Cold-startup harness is green at deletion time
- **WHEN** the deletion commit lands
- **THEN** the harness `cold_startup_small_project` and `cold_startup_large_project` scenarios SHALL pass without regression, and the change record SHALL cite that run

## ADDED Requirements

### Requirement: Architectural Lint Rejects Legacy Persistence Symbols

After the legacy importer is deleted, the architectural-lint test SHALL hard-fail on any reintroduction of legacy-persistence symbols anywhere in `src/`, `tests/`, or `tools/`. The set of forbidden identifiers SHALL include at minimum `WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`, `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`, `ParseProjectSessionText`, and `ParseWorkspaceSessionText`.

#### Scenario: Reintroduced legacy symbol is rejected
- **WHEN** a file under `src/`, `tests/`, or `tools/` contains the substring `WorkspacePersistenceLegacyFormat` (in include path, identifier, or comment used as code)
- **THEN** the architectural-lint test in `microide_tests` SHALL hard-fail and SHALL identify the offending file in the failure message

#### Scenario: Lint covers all forbidden identifiers
- **WHEN** the architectural-lint test runs
- **THEN** it SHALL apply the same hard-fail check to each identifier in the forbidden list and SHALL emit one violation per matching code-mask hit
