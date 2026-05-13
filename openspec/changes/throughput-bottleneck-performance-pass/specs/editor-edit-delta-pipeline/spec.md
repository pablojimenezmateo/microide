## ADDED Requirements

### Requirement: Known-Range Edits Avoid Full-Buffer Work

Editor edit paths SHALL apply and record known-range changes with cost proportional to the changed range and the unavoidable line-count shift, not proportional to the total document size. Single-line and same-line-count history entries SHALL update lines in place instead of erasing and reinserting through the full vector tail.

#### Scenario: Single-line insertion applies in place
- **WHEN** a user inserts a character or auto-close pair near the top of a 50000-line file
- **THEN** `TextViewport` SHALL replace the affected line in place, preserve the canonical applied edit, and SHALL NOT shift every following line in the storage vector

#### Scenario: Undo of same-count history applies in place
- **WHEN** undo replays a history entry whose before and after line counts are equal
- **THEN** `TextViewport` SHALL update the affected line range in place, preserve undo/redo semantics, and invalidate derived caches from the affected start line only

#### Scenario: Line-count-changing edit remains correct
- **WHEN** an edit inserts or removes lines
- **THEN** `TextViewport` MAY shift the tail storage as required, but SHALL still avoid unrelated full-buffer snapshots and SHALL publish the canonical applied edit when the mutation is contiguous

### Requirement: Multi-Caret Edits Build Aggregate Deltas From Touched Ranges

Multi-caret insert, delete, backspace, and surround operations SHALL build aggregate history and applied-edit metadata from normalized touched ranges rather than copying the entire document and diffing before and after buffers.

#### Scenario: Multi-caret surround avoids document snapshot
- **WHEN** `editor_surround_multi_caret` surrounds selections in a 50000-line fixture
- **THEN** the common non-overlapping range path SHALL create one undoable aggregate entry without copying every document line

#### Scenario: Multi-caret fallback is explicit
- **WHEN** multi-caret ranges overlap or cannot be normalized into a safe aggregate entry
- **THEN** the editor SHALL use a documented conservative fallback and tests SHALL cover that fallback separately from the common fast path

### Requirement: Undo Groups Do Not Snapshot Whole Buffers By Default

Undo groups SHALL merge child history entries or track touched ranges instead of snapshotting `document_->lines` at group start when the grouped operations report known ranges.

#### Scenario: Completion or snippet group uses child deltas
- **WHEN** a completion, snippet, or other grouped edit performs known-range child edits
- **THEN** ending the undo group SHALL merge those child deltas into a single undo entry without storing a full copy of the pre-edit document

#### Scenario: Unknown group mutation remains conservative
- **WHEN** a grouped mutation cannot provide child deltas or touched ranges
- **THEN** the editor MAY use a conservative snapshot fallback, and the fallback SHALL be traceable in tests or perf diagnostics

### Requirement: Large-Buffer Edit Perf Is Gated

Large-buffer edit optimizations SHALL be verified by focused performance scenarios and traces that expose per-operation latency and allocation count.

#### Scenario: Auto-close and smart-indent loops are measured
- **WHEN** this change is implemented
- **THEN** before and after runs SHALL include `editor_auto_close_typing` and `editor_smart_indent_typing`, and the change record SHALL report total wall time plus trace evidence for text input and undo scopes

#### Scenario: Multi-caret allocation regression is blocked
- **WHEN** multi-caret edit code changes
- **THEN** `editor_surround_multi_caret`, `editor_mouse_selection_drag`, and at least one adjacent typing scenario SHALL pass the isolated harness baseline or include a justified `perf-baseline:` update
