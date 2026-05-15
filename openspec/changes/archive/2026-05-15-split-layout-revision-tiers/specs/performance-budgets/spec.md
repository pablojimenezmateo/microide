## MODIFIED Requirements

### Requirement: Typing And Scrolling Frame Budget

Editor typing, editor scrolling, compare scrolling, merge scrolling, terminal scrolling, soft-wrap relayout, and multiple-caret edits SHALL fit within a single-digit millisecond frame budget on the reference host. Changes that modify text layout, wrapped-line mapping, selection fan-out, syntax state, retained-scene redraw policy, viewport scroll math, or editor mutation plumbing SHALL include `MICROIDE_TRACE_REDRAW` before-and-after output.

Single-range editor mutations on large files — including insert, delete, backspace, paste, completion acceptance, undo, and redo — SHALL avoid whole-buffer snapshotting and full-text synchronization on the main input path when the active LSP client supports incremental sync.

In addition, scrolling SHALL NOT invalidate caches that are content- or syntax-only. The four-tier document-revision model defined in the `tiered-document-revisions` capability SHALL be the mechanism that satisfies this requirement; a pure scroll over a large syntax-highlighted file SHALL keep `editor.content_revision_bumps`, `editor.syntax_revision_bumps`, and `editor.layout_shape_revision_bumps` at zero.

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

#### Scenario: Scrolling does not bump non-presentation tiers
- **WHEN** the `editor_scroll_only_no_content_bump` harness scenario runs (warm large syntax-highlighted file, then scroll N frames with no input)
- **THEN** `editor.content_revision_bumps`, `editor.syntax_revision_bumps`, and `editor.layout_shape_revision_bumps` SHALL all be zero over the measurement window

## ADDED Requirements

### Requirement: Editor Paint Budget After Tier Split

Once the `tiered-document-revisions` capability lands and the harness has recorded the new ceiling, the p50 wall budgets for `editor_sticky_scroll_scroll`, `editor_render_whitespace_paint`, and `editor_indent_guides_paint` SHALL be tightened in `tests/perf/baselines/<scenario>.json` to reflect that ceiling, and SHALL NOT be allowed to drift back above the pre-split numbers without a tagged `perf-baseline:` justification.

#### Scenario: Baselines tightened in the same change
- **WHEN** the tiered-document-revisions change lands
- **THEN** `tests/perf/baselines/editor_sticky_scroll_scroll.json`, `tests/perf/baselines/editor_render_whitespace_paint.json`, and `tests/perf/baselines/editor_indent_guides_paint.json` SHALL be updated in the same commit with measured post-split medians, accompanied by a `perf-baseline:` line in the change record

#### Scenario: Regression past pre-split numbers blocks merge
- **WHEN** a later change causes any of those three scenarios' p50 wall time to exceed its pre-tier-split baseline value
- **THEN** the harness gate SHALL fail and SHALL require either a fix or an explicit, justified `perf-baseline:` rollback
