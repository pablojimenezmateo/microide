## MODIFIED Requirements

### Requirement: Layout Mode Is Persisted And Observable
The user's last `ui.layout_mode` value SHALL be persisted through `PersistenceService`, observable to the `LayoutModeService`, and exposed through state-oriented controls in the View menu, Settings overlay, and any status-surface mode affordances.

#### Scenario: Mode survives restart
- **WHEN** the user changes `ui.layout_mode` to `compact` and restarts the application
- **THEN** the next launch SHALL apply `compact` immediately on first paint, before any user interaction

#### Scenario: Service exposes a current-mode snapshot
- **WHEN** any subsystem queries `LayoutModeService::CurrentMode()`
- **THEN** it SHALL receive the same `LayoutMode` value used by the most recent layout pass, and SHALL NOT recompute it independently

#### Scenario: View menu reflects compact mode as state
- **WHEN** the View menu is opened
- **THEN** the compact-mode control SHALL render as a state-oriented label with checkmark semantics that reflect whether compact mode is active, rather than only a verb-style toggle label
