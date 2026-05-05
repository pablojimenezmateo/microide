## ADDED Requirements

### Requirement: Editor Mutations Publish A Canonical Applied Edit

`editor::TextViewport` SHALL publish a canonical applied-edit description for every mutation that can be represented as one contiguous range replacement. The applied edit SHALL include the pre-edit range and the normalized replacement text, and workspace consumers SHALL treat that metadata as the source of truth for redraw invalidation and incremental LSP synchronization.

#### Scenario: Single-range insertion publishes an applied edit
- **WHEN** the user inserts text, presses Backspace, presses Delete, pastes text, or accepts a completion that maps to one contiguous range replacement
- **THEN** the active `TextViewport` SHALL expose the affected pre-edit range plus the normalized inserted text as its last applied edit

#### Scenario: Structural fallback remains explicit
- **WHEN** a viewport mutation cannot be expressed as one contiguous range replacement
- **THEN** `TextViewport` SHALL expose no applied edit, and workspace consumers SHALL fall back to their conservative full-redraw or full-sync path

### Requirement: Undo And Redo Reuse The Applied-Edit Contract

Undo and redo SHALL publish the reverse or forward applied edit for the history entry they replay instead of clearing edit metadata. The emitted delta SHALL be suitable for incremental LSP sync and retained-scene redraw decisions without recomputing a whole-buffer diff on the workspace side.

#### Scenario: Undo publishes the reverse edit
- **WHEN** the user triggers undo after a contiguous editor mutation
- **THEN** the viewport SHALL expose the reverse replacement range and text for that history entry as the last applied edit

#### Scenario: Redo republishes the forward edit
- **WHEN** the user triggers redo for a previously undone contiguous editor mutation
- **THEN** the viewport SHALL expose the forward replacement range and text for that history entry as the last applied edit
