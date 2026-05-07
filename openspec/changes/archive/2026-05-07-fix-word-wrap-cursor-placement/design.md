## Context

`TextViewport` already has a complete soft-wrap data model:

- `WrappedRowLayout { line_index, visual_start, visual_end }` describing one visible row.
- `wrapped_row_layouts_` (vector) and `wrapped_line_row_offsets_` (logical→first-visual map).
- A cache keyed on `(document_->layout_revision, tab_size_, visible_columns_)` rebuilt by `EnsureWrappedRowLayouts()`.
- Wrap-aware vertical motion for the **primary** caret in `MoveCursorVertical()` via `CursorVisualRow()` and `ResolveSoftWrapCursorColumnForTargetRow()`.
- Scroll model that interprets `scroll_line_` as visual-row index when wrap is on, with horizontal scroll forced to zero.
- Settings plumbing: `editor.wrap` flows through `WorkspacePersistenceCoordinatorConfig::ApplyCanonicalProjectSetting` → `WorkspaceShellEditor::ApplyEditorPreferences*` → `TextViewport::SetSoftWrap()`.

Two concrete defects exist:

1. **Render path ignores wrap.** `EditorViewRenderer.cpp` iterates `scroll_line + row` over raw document lines and clips with `horizontal_scroll`. It never consults `VisibleWrappedRowLayout()`. The cache is built and kept fresh, but never displayed → users see "wrap doesn't work".
2. **Multi-caret vertical motion only moves the primary caret.** Secondary carets in `secondary_carets_` are not advanced by `MoveCursorVertical()`, and have no per-caret `preferred_column_`. Under wrap this is doubly wrong: secondary carets stay put while the primary jumps by visual rows.

Hit-testing under wrap is also logical-only today (clicks on continuation rows resolve to the start of the logical line); since wrap currently never paints, this defect is invisible but will surface immediately once render is fixed.

## Goals / Non-Goals

**Goals:**

- The wrapped-row layout is the single source of truth for paint, gutter, every caret's vertical motion, hit-testing, and scroll, when `soft_wrap()` is true.
- Multi-caret Up/Down works identically under wrap and no-wrap, with each caret carrying its own sticky preferred column.
- Hot path stays allocation-free; wrap layout recomputes only on `(layout_revision, tab_size, visible_columns)` change.
- Existing no-wrap render path is unchanged in behaviour and cost.

**Non-Goals:**

- Per-document or per-tab wrap toggle (stays project-scoped).
- Hard-wrap, ruler-anchored wrap, or word-boundary heuristics beyond what `EnsureWrappedRowLayouts` already does.
- Re-architecting the renderer; the change is an additive branch inside the existing render loop plus a small API surface on `TextViewport`.
- Changing `editor.wrap` setting key, persistence format, or default.

## Decisions

### D1. Render branches on `viewport.soft_wrap()` and consumes `VisibleWrappedRowLayout()`.

When wrap is on, the row loop in `EditorViewRenderer` produces, for each visible row index `r`:

- `layout = viewport.VisibleWrappedRowLayout(r)` → `{ line_index, visual_start, visual_end }`.
- The painted text for that row is the slice of `lines[line_index]` between `visual_start` and `visual_end`, expanded for tabs by the existing `TextLayout` helpers.
- Horizontal scroll is forced to zero (already enforced in `ClampScrollState`); the renderer SHALL NOT apply `horizontal_scroll` in this branch.

Alternative considered: have the renderer keep iterating logical lines and call `TextLayout::WrapLine` itself per frame. Rejected — duplicates the cache, blows the per-frame budget, and creates two sources of truth.

### D2. Gutter renders line numbers on the first visual row of each logical line only.

Detect "first visual row" by `visual_start == 0` (cheap, no extra map lookup). Continuation rows paint an empty gutter cell of the same width so the editor text column is stable. This matches VS Code, Sublime, Helix.

### D3. Hit-testing converts (visual_row, visual_col) → (logical_line, logical_col) via `wrapped_row_layouts_`.

Add `TextViewport::LogicalPositionForVisualHit(visual_row, visual_col)` returning `{ line_index, column = visual_start + clamp(visual_col, 0, visual_end - visual_start) }`. The renderer's existing click handler calls this when `soft_wrap()` is true and falls through to the current logical mapping otherwise. No new caches.

### D4. Per-caret preferred column for multi-caret vertical motion.

Replace the single `preferred_column_` on `TextViewport` with a per-caret value. Concretely:

- The primary caret keeps a `preferred_column_` field on `TextViewport` (existing).
- Each entry in `secondary_carets_` gains a `preferred_column` member (new).
- `MoveCursorVertical(direction, count)` iterates primary + every secondary; for each caret it calls a shared helper `AdvanceCaretVertical(caret, preferred, direction, count)` that does exactly what the primary path does today (visual-row math when `soft_wrap_`, logical-line math otherwise).
- After motion, deduplicate carets by `(line, column)` and re-sort. Selection-anchor handling (Shift+Up/Down) follows the same per-caret rule, mirroring existing single-caret semantics.
- Horizontal motion (Left/Right/Home/End) resets each caret's `preferred_column` to its current visual column, exactly as the single-caret path does today.

Alternative considered: keep one shared `preferred_column_` and apply it to all carets. Rejected — carets drift to a single column on the first vertical motion, which is the standard VS Code-vs-Sublime difference users actively dislike. Per-caret stickiness is the contract our spec already requires.

### D5. Cache invariants: no recompute on hot paths.

`EnsureWrappedRowLayouts()` is already keyed on `(document_->layout_revision, tab_size_, visible_columns_)`. We preserve that. The new render branch calls `VisibleWrappedRowLayout(r)` which is O(1) on cache hit. The cache is rebuilt only when:

- The document mutates (layout_revision bumps).
- Tab size changes (`SetTabSize`).
- Viewport width changes (`SetViewportSize` updates `visible_columns_`).

We do **not** invalidate the cache on cursor motion, scroll, or selection change. The render loop allocates nothing per row; `WrappedRowLayout` is a POD value type returned by reference from the cache.

### D6. Public API surface on `TextViewport` (additive, narrow).

- `int VisualRowCount() const;` — total wrapped rows (already implicit; expose if not already).
- `const WrappedRowLayout& VisibleWrappedRowLayout(int visual_row) const;` (already exists).
- `LogicalPosition LogicalPositionForVisualHit(int visual_row, int visual_col) const;` (new).
- No new mutators. No new dependencies.

### D7. Test strategy.

- `tests/TextViewportTests.cpp`: extend existing wrap tests with multi-caret vertical motion, per-caret preferred-column stickiness across continuation rows, hit-test on continuation row, cache reuse counter (instrument `EnsureWrappedRowLayouts` with a build-counter readable by tests).
- `tests/EditorViewRendererTests.cpp` (new or extend): golden-style assertion that, with wrap on, the renderer requests rows from `VisibleWrappedRowLayout` rather than from raw document lines, and that gutter numbers appear only on first-visual rows.
- Architectural-lint test: assert `EditorViewRenderer.cpp` does not allocate strings on the hot row loop (existing invariant from CLAUDE.md re: render TUs).

## Risks / Trade-offs

- **Risk**: Switching the gutter to first-visual-row only could shift column positions for downstream code that assumes gutter-width parity. → **Mitigation**: gutter width is a constant per frame (max line-number digits); continuation rows render an empty cell of identical pixel width, so downstream column math is unaffected.
- **Risk**: Per-caret `preferred_column` increases `secondary_carets_` size by one int per caret. → **Mitigation**: negligible; multi-caret count is bounded by user input and already tracked in a `std::vector`.
- **Risk**: Hit-testing tab-expanded characters on a continuation row could land between cells if `visual_col` isn't snapped. → **Mitigation**: reuse `TextLayout::TextColumnForVisualColumn` (already used by the no-wrap hit path) and clamp to `[visual_start, visual_end]`.
- **Risk**: A logical line of width 0 (empty) must still produce one wrapped row. → **Mitigation**: existing `EnsureWrappedRowLayouts()` already emits `{line, 0, 0}` for empty lines; the render branch must paint nothing but still advance one row, mirroring no-wrap behaviour.
- **Risk**: Performance regression from extra indirection per row. → **Mitigation**: `VisibleWrappedRowLayout` is `[[nodiscard]]` const-by-reference into a contiguous vector; benchmark wrap-on render against wrap-off baseline using `docs/perf-harness.md` scenarios on a 10k-line file.

## Migration Plan

No persisted-state or user-visible setting change. Ship behind no flag — this is a defect fix. Rollback is `git revert` of the change commit; no schema or persistence backwards-compat concerns.

## Open Questions

- Should secondary carets have their own selection anchors track wrap separately for Shift+Up/Down across continuation rows? Default answer: yes, identical to primary; flagged here so the implementer confirms during review.
- Does the existing `EditorViewRendererTests` harness exercise the row loop directly, or only through pixel-level golden tests? If only pixel-level, we add a row-emission seam (e.g., a test fake renderer that records `(visual_row → layout)` calls) rather than asserting on bitmaps.
