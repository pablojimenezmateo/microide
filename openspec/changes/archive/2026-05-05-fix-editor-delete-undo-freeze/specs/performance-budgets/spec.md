## MODIFIED Requirements

### Requirement: Typing And Scrolling Frame Budget

Editor typing, editor scrolling, compare scrolling, merge scrolling, terminal scrolling, soft-wrap relayout, and multiple-caret edits SHALL fit within a single-digit millisecond frame budget on the reference host. Changes that modify text layout, wrapped-line mapping, selection fan-out, syntax state, retained-scene redraw policy, viewport scroll math, or editor mutation plumbing SHALL include `MICROIDE_TRACE_REDRAW` before-and-after output.

Single-range editor mutations on large files — including insert, delete, backspace, paste, completion acceptance, undo, and redo — SHALL avoid whole-buffer snapshotting and full-text synchronization on the main input path when the active LSP client supports incremental sync.

#### Scenario: Typing into a large open file
- **WHEN** the editor has a file loaded that triggers the large-file code path and the user holds a printable key
- **THEN** each insertion SHALL render within the frame budget, SHALL NOT produce a full-surface repaint per keystroke, and SHALL NOT regress measurable typing latency compared to the prior release

#### Scenario: Typing with soft wrap and multiple carets
- **WHEN** soft wrap is enabled and the user edits through multiple carets in a long file
- **THEN** wrapped-line recompute, caret placement, and repaint SHALL remain within the frame budget without degrading unrelated editor responsiveness

#### Scenario: Scrolling a merge tab
- **WHEN** a three-way merge tab is scrolled with the mouse wheel or `PageDown`
- **THEN** each repaint SHALL fit within the frame budget and SHALL reuse the shared row-decoration cache rather than rebuilding decorations per frame

#### Scenario: Delete and undo in a large LSP-backed file
- **WHEN** the user presses Delete, Backspace, or `Ctrl+Z` in a large open file whose active language server supports incremental sync
- **THEN** the input handler SHALL stay off the full-buffer snapshot and full-text LSP path, the redraw invalidation SHALL be derived from the applied edit rather than a whole-buffer diff, and the edit SHALL remain interactive without multi-hundred-millisecond stalls
