## 1. Word-boundary wrap

- [x] 1.1 Replace the column-only loop in `TextViewport::EnsureWrappedRowLayouts` (`src/editor/TextViewport.cpp`) with a single-pass walk that tracks the most recent whitespace break opportunity per line; prefer that break when one fits inside the wrap window, otherwise hard-break before the offending character.
- [x] 1.2 Tighten `TextViewport::VisibleWrappedRowLayout` (`src/editor/TextViewport.cpp`) to slice `BuildVisibleLine` to `min(visible_columns_, row.visual_end - row.visual_start)` so jagged rows do not over-paint into the next row's text.
- [x] 1.3 Add `TextViewport/SoftWrapPrefersWhitespaceBoundaries` and `TextViewport/SoftWrapHardBreaksInsideLongWords` regression tests in `tests/TextViewportTests.cpp` and register them in the same harness block as the existing soft-wrap suite.

## 2. Hide horizontal scrollbar under soft wrap

- [x] 2.1 In `WorkspaceShell::ComputeEditorScrollLayout` (`src/workspace/WorkspaceShellRedraw.cpp`), collapse `total_columns` to `metrics.visible_columns` when `viewport.soft_wrap()` is true so `ComputeScrollSurfaceLayout` derives `max_horizontal_scroll == 0` and skips the bar.

## 3. Wrap toggle repaints the editor surface

- [x] 3.1 In `WorkspaceActionContext::SetSoftWrap` (`src/workspace/WorkspaceActionServices.cpp`), call `operations_.request_active_tab_redraw(false)` after `apply_editor_preferences_to_all_tabs` and `save_config_state`.

## 4. Test runner success summary

- [x] 4.1 In `tests/TestMain.cpp`, after the run loop completes successfully, print `microide_tests: OK (N tests passed)` (singular/plural form) to stderr before returning 0.

## 5. Validation

- [x] 5.1 `cmake --build build -j8` is green.
- [x] 5.2 `ctest --test-dir build --output-on-failure` is green (816 tests pass, including the two new soft-wrap regressions).
