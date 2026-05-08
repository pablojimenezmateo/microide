## MODIFIED Requirements

### Requirement: Host-Owned Modal Settings Overlay

MicroIDE SHALL expose a host-owned modal settings overlay reachable from the menu bar (Preferences → Settings…), the status bar, and a keyboard accelerator. The overlay SHALL render as a single modal surface in the editor area using the existing prompt-overlay pattern; it SHALL NOT spawn an OS dialog and SHALL NOT replace editor workflows.

#### Scenario: Overlay opens and consumes input focus
- **WHEN** the user invokes the Settings overlay action
- **THEN** the overlay SHALL render at the centered prompt-surface rectangle, capture keyboard focus, dim the editor area, and route Escape to dismissal

#### Scenario: Overlay is host-owned
- **WHEN** the source tree is searched for settings-overlay rendering
- **THEN** the overlay rendering SHALL live in `src/workspace/WorkspaceShellRenderSettings*.cpp`, owned by `SettingsOverlayService`, and SHALL NOT be replaceable by a plugin
