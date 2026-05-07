## ADDED Requirements

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
