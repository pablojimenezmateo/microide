## MODIFIED Requirements

### Requirement: Compact Mode Reduces Chrome Without Hiding Functionality

`Compact` mode SHALL reduce on-screen chrome density without removing any user-reachable command. Every command available in `Regular` SHALL remain reachable in `Compact` either through the same gesture, an icon-only equivalent, or a command-palette/keyboard shortcut.

#### Scenario: Project tabs lose badges, keep title
- **WHEN** `LayoutMode` is `Compact`
- **THEN** project tabs SHALL omit the `kProjectTabBadgeWidth` badge and the close glyph SHALL render only on hover, while the project title SHALL remain visible until the title would clip below 64px

#### Scenario: Bottom panel terminal new-tab control collapses
- **WHEN** `LayoutMode` is `Compact`
- **THEN** the terminal new-tab affordance SHALL render as a compact `+` glyph at `kBottomPanelHeaderButtonSize` minus 4px, MUST keep its hit pad ≥20×20px, and SHALL never overlap the active terminal-tab strip

#### Scenario: Sidebar controls remain reachable in compact mode
- **WHEN** `LayoutMode` is `Compact` and a host-owned sidebar surface is visible
- **THEN** sidebar controls SHALL remain reachable without requiring text labels, and compact rendering SHALL preserve minimum interaction targets and tooltips
