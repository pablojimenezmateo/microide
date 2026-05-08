## Why

MicroIDE's UI includes high-value actions that are currently hidden behind non-obvious click targets and inconsistent wording, which makes core workflows harder to discover than they should be. A full UX pass is needed now so behavior, labels, and settings copy match user expectations across status bar, menus, and shell surfaces.

## What Changes

- Redesign status bar affordances so left and right regions expose clear purpose, state, and intent before interaction.
- Surface source-control branch health directly in the status area (for example clean vs dirty) instead of relying on implicit navigation behavior.
- Replace ambiguous or action-heavy menu labels with state-oriented wording (for example `Compact mode` with checkmark semantics instead of `Toggle compact mode`).
- Audit and normalize settings and help copy so descriptive text explains behavior without misleading labels (for example replacing non-tip text labeled as `Tip`).
- Align sidebar header layouts for cohesion (for example Project and Source Control using the same selector row + actions row structure).
- Apply the same discoverability, naming, and consistency standards to all shell paths (menus, status items, popups, and mode controls) with explicit acceptance criteria.

## Capabilities

### New Capabilities
- `ui-command-labeling-and-discoverability`: Defines naming and affordance requirements for menu/status actions so command intent and current state are visible without guesswork.

### Modified Capabilities
- `workspace-status-bar`: Clarify interaction zones, expose branch/working-tree state, and require explicit labels for navigation actions.
- `responsive-shell-layout`: Align compact-mode entry points and wording with predictable state-based UX behavior.
- `settings-overlay-surface`: Require accurate, non-misleading copy labels and behavior descriptions throughout settings/help surfaces.

## Impact

- Affected code: workspace status bar rendering/view models, layout-mode actions, command/menu registries, sidebar header layout/view models, settings overlay view model/content, and UX-related tests.
- Affected systems: shell interaction model, copy/content conventions, and discoverability defaults for non-editor workflows.
- Dependencies: no new external libraries expected; primary impact is host UI logic, labels, and regression test coverage.
