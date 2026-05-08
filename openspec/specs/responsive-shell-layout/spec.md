# responsive-shell-layout Specification

## Purpose
TBD - created by archiving change responsive-layout-and-options-polish. Update Purpose after archive.
## Requirements
### Requirement: Discrete Layout Modes Replace Fluid Reflow

The shell SHALL select between exactly two layout modes — `Regular` and `Compact` — once per layout pass, based on a width breakpoint plus a user override, and SHALL NOT animate or interpolate chrome positions between modes. Discrete modes are required to keep frame-time predictable on the speed/CPU budget defined by `product-vision`.

#### Scenario: Layout mode is computed once per layout pass
- **WHEN** `ComputeLayout` runs for a frame
- **THEN** it SHALL resolve `LayoutMode` exactly once from the user-config setting `ui.layout_mode` (`auto`/`regular`/`compact`) and the breakpoint `ui.layout_compact_breakpoint_px`, store the result on `WorkspaceLayout`, and downstream chrome computations SHALL read that single value rather than recomputing the mode

#### Scenario: Auto-mode crosses the breakpoint
- **WHEN** `ui.layout_mode = auto` and the window width crosses the `ui.layout_compact_breakpoint_px` threshold across two consecutive layout passes
- **THEN** the resolved `LayoutMode` SHALL flip exactly once at the threshold and SHALL NOT oscillate within a 24px hysteresis band centered on the threshold

#### Scenario: User override defeats auto
- **WHEN** `ui.layout_mode` is `regular` or `compact`
- **THEN** the resolved `LayoutMode` SHALL equal the override regardless of window width

### Requirement: Menu Bar Has An Overflow Chevron Instead Of Silent Truncation

When one or more menu-bar items do not fit, the menu bar SHALL render an overflow chevron that opens a popup containing every overflowed item in original order. The shell SHALL NEVER drop menu items without a visible affordance.

#### Scenario: All items fit
- **WHEN** the window is wide enough to fit every visible menu-bar item plus the window-control buttons
- **THEN** the chevron SHALL NOT be drawn and the chevron region SHALL NOT consume hit area

#### Scenario: Some items do not fit
- **WHEN** any menu-bar item would extend past the available `max_x`
- **THEN** the shell SHALL truncate the visible item list at the last fitting item, render an overflow chevron immediately to the right of that item, and route clicks on the chevron to a popup whose entries are the omitted items in their declared order

#### Scenario: Compact mode collapses to a single hamburger
- **WHEN** `LayoutMode` is `Compact`
- **THEN** the menu bar SHALL render a single hamburger button that opens a popup containing every top-level menu, and SHALL NOT render individual menu-bar item rectangles

### Requirement: Pointer Targets Meet WCAG 2.2 Minimums Without Visual Inflation

Every interactive shell affordance SHALL expose a pointer hit area of at least 24×24 CSS pixels, achieved by transparent hit-pad inflation around the visible glyph or thumb so the visual chrome remains compact. The visible visuals SHALL remain at the existing pixel sizes.

#### Scenario: Resize handles
- **WHEN** the user moves the pointer over a sidebar or bottom-panel divider
- **THEN** the hit rectangle SHALL be at least 12px in the divider-perpendicular axis and at least 24px in the parallel axis at the cursor, while the painted divider SHALL stay at `kWorkspaceDividerThickness` plus a 6px visible drag affordance

#### Scenario: Scrollbar thumb
- **WHEN** the user moves the pointer over a vertical or horizontal scrollbar thumb
- **THEN** the hit rectangle SHALL be at least 18px in the cross-axis direction (centered on the visual 10px thumb) while the painted thumb SHALL remain at `kWorkspaceScrollbarThickness`

#### Scenario: Tab close button
- **WHEN** the user moves the pointer near a tab's close glyph
- **THEN** the hit rectangle SHALL be at least 20×20px (centered on the visual 14px glyph) and SHALL NOT extend outside the tab's own rectangle

#### Scenario: Diagnostic hover hit strip
- **WHEN** the user hovers over a diagnostic-decorated line in the active editor
- **THEN** the hover trigger hit area SHALL span the full visual line height, not the 2px-tall anchor strip currently used to position the popup, while the popup anchor itself SHALL remain unchanged

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

