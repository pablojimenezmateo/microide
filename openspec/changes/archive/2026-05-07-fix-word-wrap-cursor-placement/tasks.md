## 1. Baseline & instrumentation

- [x] 1.1 Read current state of `src/editor/TextViewport.{h,cpp}` and `src/editor/EditorViewRenderer.cpp`; confirm `WrappedRowLayout`, `EnsureWrappedRowLayouts()`, and `VisibleWrappedRowLayout()` shapes match design.md (D1, D5).
- [x] 1.2 Add a debug-only build counter on `TextViewport` (incremented inside `EnsureWrappedRowLayouts()` only when the cache is rebuilt) plus a const accessor; this is the seam the cache-reuse test in 5.5 reads.
- [x] 1.3 Confirm `editor.wrap` setting plumbing: trace `WorkspaceSettingsRegistry` → `ApplyCanonicalProjectSetting` → `WorkspaceShellEditor::ApplyEditorPreferencesToAllTabs` → `TextViewport::SetSoftWrap`. No code change expected; document any gap as a sub-task.

## 2. TextViewport API surface

- [x] 2.1 Add `int VisualRowCount() const;` returning `wrapped_row_layouts_.size()` when `soft_wrap_` is true, else `document_->lines.size()`.
- [x] 2.2 Add `LogicalPosition LogicalPositionForVisualHit(int visual_row, int visual_col) const;` per design D3, reusing `TextLayout::TextColumnForVisualColumn` for tab-expanded snapping and clamping into `[visual_start, visual_end]`.
- [x] 2.3 Add a `preferred_column` field to the `SecondaryCaret` struct (or equivalent) used by `secondary_carets_`. Default to the caret's current visual column when constructed via `AddSecondaryCaret`/`SetSecondaryCarets`.
- [x] 2.4 Extract a private helper `AdvanceCaretVertical(Caret& caret, int& preferred, int direction, int count)` that contains the existing primary-caret vertical-motion math (wrap and no-wrap branches). Have `MoveCursorVertical()` call it for the primary caret first, then for each secondary caret using its own `preferred_column`.
- [x] 2.5 After multi-caret motion, deduplicate carets by `(line, column)` and keep the primary caret distinct; mirror existing dedup rules used in horizontal multi-caret motion.
- [x] 2.6 Reset every caret's `preferred_column` on horizontal motion (Left/Right/Home/End) and on any mouse-driven caret placement, to match single-caret stickiness rules.

## 3. Render path

- [x] 3.1 In `EditorViewRenderer.cpp`, gate the row loop on `viewport.soft_wrap()`. When true, iterate `r ∈ [0, visible_row_count)` and resolve each row through `viewport.VisibleWrappedRowLayout(scroll_line + r)`.
- [x] 3.2 In the wrap branch, paint the slice of `lines[layout.line_index]` between `layout.visual_start` and `layout.visual_end` using existing `TextLayout` helpers; do not apply `horizontal_scroll`.
- [x] 3.3 Update gutter painter so a row renders its line number only when `layout.visual_start == 0`; continuation rows paint an empty cell of identical pixel width.
- [x] 3.4 Update click hit-testing: when `viewport.soft_wrap()` is true, call `LogicalPositionForVisualHit(visual_row, visual_col)` and feed the result into the existing place-caret path. Leave the no-wrap path untouched.
- [x] 3.5 Verify selection-rectangle painting under wrap renders correctly across continuation rows by clipping the selection to each row's `[visual_start, visual_end]` interval.
- [x] 3.6 Confirm no new string allocations on the hot row loop (no `std::string(...)`, `+`, `to_string`, `format`); the architectural lint already enforces this — re-run it.

## 4. Scroll & viewport interactions

- [x] 4.1 Confirm `EnsureCursorVisible()` and `ClampScrollState()` use visual rows under wrap (already true per investigation). Add a regression test that PageUp/PageDown advances by visible rows under wrap.
- [x] 4.2 Confirm `SetViewportSize()` updates `visible_columns_` and that this triggers a single cache rebuild on the next `EnsureWrappedRowLayouts()` call. Add a test using the build counter from 1.2.
- [x] 4.3 Confirm horizontal scroll is forced to zero in the wrap branch (no UI surface should expose horizontal scroll affordance when wrap is on).

## 5. Tests

- [x] 5.1 `tests/TextViewportTests.cpp`: extend `TestTextViewportSoftWrapMoveCursorVerticalUsesWrappedRows` to cover Up/Down crossing in and out of a wrapped logical line, asserting `preferred_column` survives a short continuation row and re-applies on the next wide row.
- [x] 5.2 New test: two secondary carets placed on different logical lines; press Down once with wrap enabled; assert every caret advanced by exactly one visual row using its own preferred column.
- [x] 5.3 New test: caret placed mid-way on a continuation row of a wrapped logical line; assert its visual row matches the layout entry and `LogicalPositionForVisualHit` round-trips.
- [x] 5.4 New test: click hit-testing on a continuation row resolves to `visual_start + visual_col`, not column 0.
- [x] 5.5 New test: render two consecutive frames with no document edit, no tab-size change, and no resize; assert the build counter from 1.2 increments at most once.
- [x] 5.6 New test in `tests/EditorViewRendererTests.cpp` (or a row-emission fake): with wrap on, the renderer requests `VisibleWrappedRowLayout(r)` for each visible row index and the gutter emits a number only when `visual_start == 0`.
- [x] 5.7 Empty-line edge case: a logical empty line still produces exactly one visible row under wrap; render paints nothing but advances one row.
- [x] 5.8 Run focused suite: `./build/microide/microide_tests TextViewport` and `./build/microide/microide_tests EditorView`. Then run full `ctest --test-dir build --output-on-failure`.

## 6. Performance & sanitizers

- [x] 6.1 Run a wrap-on vs wrap-off render benchmark on a 10k-line file via `dev-docs/performance/perf-harness.md` scenarios; record per-frame paint cost. Confirm wrap-on stays within the existing editor-paint budget.
- [x] 6.2 Run ASAN preset over the new tests: `cmake --preset microide-asan && cmake --build build/microide-asan && ctest --test-dir build/microide-asan --output-on-failure`.
- [x] 6.3 Run UBSAN preset similarly.
- [x] 6.4 Confirm no per-frame allocations introduced (architectural-lint test).

## 7. Documentation & spec sync

- [x] 7.1 Update `dev-docs/project/active-work.md` to reflect that soft wrap is fixed end-to-end (render, gutter, multi-caret motion, hit-test).
- [x] 7.2 No change to `dev-docs/project/implementation-guide.md` is expected; verify and skip if so.
- [x] 7.3 Run `openspec validate fix-word-wrap-cursor-placement --strict` and resolve any spec/format issues before archive.
