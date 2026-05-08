## ADDED Requirements

### Requirement: Persistent Shell Controls Use State-Oriented Labels
Host-owned persistent controls in menus, status surfaces, and settings entry points SHALL use state-oriented labels that communicate current mode or condition, not only an action verb.

#### Scenario: Menu item represents persisted layout mode
- **WHEN** a menu item controls a persisted shell state such as compact mode
- **THEN** the visible label SHALL be state-oriented (for example `Compact mode`) and the menu SHALL present selection via checkmark semantics rather than a `Toggle ...` verb label

#### Scenario: Action item remains action-oriented
- **WHEN** a menu item triggers an immediate one-shot operation without persistent state
- **THEN** the item SHALL keep an action verb label and SHALL NOT be converted to a state label

### Requirement: Clickable Status Segments Expose Intent Before Interaction
Any status-bar segment that routes to another panel or mode SHALL expose enough visible text to communicate both current state and click destination without requiring tooltip hover.

#### Scenario: Source-control status segment
- **WHEN** the status bar renders source-control information
- **THEN** the segment SHALL include branch identity and clean/dirty state text and SHALL indicate navigation intent through label composition and tooltip copy

#### Scenario: Tooltip supplements but does not replace discoverability
- **WHEN** a clickable status segment is hovered
- **THEN** the tooltip MAY provide additional detail but SHALL NOT be the only place where the primary interaction purpose is explained

### Requirement: UX Copy Taxonomy Matches Message Intent
Shell copy labels such as `Tip`, `Note`, and `Warning` SHALL be reserved for messages that match their semantic intent; descriptive behavior text SHALL be presented as neutral description rather than mislabeled advice.

#### Scenario: Behavior explanation in settings overlay
- **WHEN** a settings row includes explanatory text that describes default behavior
- **THEN** the text SHALL render as neutral description or `Note`, and SHALL NOT be labeled `Tip` unless it provides optional optimization guidance

#### Scenario: True tip is labeled as tip
- **WHEN** explanatory text recommends an optional workflow improvement that is not required for normal operation
- **THEN** the UI SHALL label that text as `Tip`

### Requirement: Comparable Sidebar Headers Use Cohesive Row Structure
Host-owned sidebars that share the same interaction model (primary selector/title plus global actions) SHALL use the same row composition pattern so controls are predictable across panels.

#### Scenario: Project and Source Control header alignment
- **WHEN** both Project and Source Control sidebars are rendered in the same layout mode
- **THEN** each panel SHALL place the primary selector/title on the first row and place action buttons on a dedicated second row, with consistent spacing and alignment rules

#### Scenario: Actions remain discoverable after row split
- **WHEN** Project panel actions are moved from a single row to the second actions row
- **THEN** all existing actions SHALL remain visible and reachable without additional clicks, and button ordering SHALL remain stable
