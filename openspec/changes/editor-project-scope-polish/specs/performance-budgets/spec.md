## ADDED Requirements

### Requirement: Ignored-Tree Visibility And Expansion Stay Interactive

Project-tree initial load SHALL surface ignored nodes without recursively walking ignored descendants, and expanding an ignored directory SHALL enumerate only the requested level within a documented interactive latency budget. Changes that modify ignored-node discovery or expansion SHALL include harness or trace evidence showing that initial tree open does not scan the full ignored subtree.

#### Scenario: Initial tree open with a large ignored directory
- **WHEN** a project contains a large ignored directory such as `node_modules/`
- **THEN** the initial project tree SHALL surface the ignored directory node without recursively enumerating its full descendant set

#### Scenario: Expanding an ignored directory enumerates one level
- **WHEN** the user expands an ignored directory in the project tree
- **THEN** the UI SHALL enumerate only that directory's immediate children within an interactive latency budget instead of scanning the entire ignored subtree

## MODIFIED Requirements

### Requirement: Typing And Scrolling Frame Budget

Editor typing, editor scrolling, compare scrolling, merge scrolling, terminal scrolling, soft-wrap relayout, and multiple-caret edits SHALL fit within a single-digit millisecond frame budget on the reference host. Changes that modify text layout, wrapped-line mapping, selection fan-out, syntax state, retained-scene redraw policy, or viewport scroll math SHALL include `MICROIDE_TRACE_REDRAW` before-and-after output.

#### Scenario: Typing into a large open file
- **WHEN** the editor has a file loaded that triggers the large-file code path and the user holds a printable key
- **THEN** each insertion SHALL render within the frame budget, SHALL NOT produce a full-surface repaint per keystroke, and SHALL NOT regress measurable typing latency compared to the prior release

#### Scenario: Typing with soft wrap and multiple carets
- **WHEN** soft wrap is enabled and the user edits through multiple carets in a long file
- **THEN** wrapped-line recompute, caret placement, and repaint SHALL remain within the frame budget without degrading unrelated editor responsiveness

#### Scenario: Scrolling a merge tab
- **WHEN** a three-way merge tab is scrolled with the mouse wheel or `PageDown`
- **THEN** each repaint SHALL fit within the frame budget and SHALL reuse the shared row-decoration cache rather than rebuilding decorations per frame
