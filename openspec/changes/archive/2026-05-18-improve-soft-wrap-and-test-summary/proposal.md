## Why

Soft wrap currently breaks lines on a strict column boundary, splitting words mid-token and leaving stale unwrapped rows visible after a Wrap toggle because the View-menu path requests no redraw. The horizontal scrollbar also remains rendered (and short) when wrap is enabled, advertising scroll affordance that does nothing. Together these defects make the wrap mode look broken on first toggle.

Separately, `microide_tests` prints nothing on success, so agents and CI scrapes have to inspect exit codes to distinguish a real pass from an empty filter match — an avoidable papercut.

## What Changes

- Soft-wrap reflow prefers the most recent whitespace boundary inside the wrap window; long unbreakable tokens still hard-break at the column boundary as today.
- The horizontal scrollbar is hidden when soft wrap is active (it cannot scroll).
- Toggling soft wrap from the `View → Word Wrap` action (and shortcut) requests a full editor-surface redraw so previously-painted unwrapped rows do not persist.
- `microide_tests` prints a final `microide_tests: OK (N tests passed)` summary on success.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `editor-multicursor-and-wrap`: tighten the soft-wrap requirement to specify whitespace-preferred break selection, hide the horizontal scrollbar when wrap is on, and require a full editor-surface redraw on the wrap toggle.

## Impact

- `src/editor/TextViewport.cpp` — replace the column-only wrap loop with a single-pass whitespace-preferring walk; tighten `VisibleWrappedRowLayout` slicing to the actual row width so jagged rows do not over-paint into the next row.
- `src/workspace/WorkspaceShellRedraw.cpp` — `ComputeEditorScrollLayout` collapses `total_columns` to `visible_columns` when soft wrap is on, suppressing the bar.
- `src/workspace/WorkspaceActionServices.cpp` — `SetSoftWrap` requests `request_active_tab_redraw(false)`.
- `tests/TextViewportTests.cpp` — new regression coverage for whitespace-preferred wrap and long-token hard-break fallback.
- `tests/TestMain.cpp` — final success summary line.
- No persisted-state or plugin-host contract changes; no breaking API changes.
