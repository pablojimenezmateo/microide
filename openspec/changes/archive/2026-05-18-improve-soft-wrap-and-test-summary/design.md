## Context

`TextViewport::EnsureWrappedRowLayouts` currently builds the wrapped layout for a soft-wrapped line with a strict integer loop: `for (start = 0; start < visual_width; start += wrap_columns)`. It pays one `VisualColumnForTextColumn` walk over the line, then splits at column multiples. That keeps the rebuild O(line_length) but breaks mid-word and ignores whitespace.

`VisibleWrappedRowLayout` then materialises a `LayoutLine` by calling `BuildVisibleLine(line, row.visual_start, visible_columns_, tab_size_)`. With a fixed-width row that was fine — every row is exactly `wrap_columns` wide. Once rows can be shorter than `wrap_columns` (whitespace breaks), passing `visible_columns_` would over-paint into the next row's text.

`ComputeEditorScrollLayout` always passes `max(visible_columns, MaxVisualColumns(viewport))` as `total_columns` to `ComputeScrollSurfaceLayout`. With soft wrap enabled the horizontal scroll value is already clamped to zero by `ClampScrollState`, but the scrollbar geometry computes from the raw column counts and still renders.

`WorkspaceActionContext::SetSoftWrap` (driven by the `Wrap` action) calls `apply_editor_preferences_to_all_tabs` + `save_config_state` and stops. Unlike the Settings overlay path (which calls `MarkLayoutDirty()` + `RequestWindowRedraw()`), it never asks for an editor redraw — so the partial-redraw scope that closed the menu leaves the previously-painted unwrapped rows on screen.

`microide_tests` returns an exit code on success but prints no terminator. CI and agents have to inspect `$?` to distinguish "ran and passed" from "matched zero filters" (which the harness already prints a clear message for).

## Goals / Non-Goals

**Goals:**
- Wrap at the most recent whitespace inside the current row's window when one exists.
- Fall back to a hard column break inside a single long token (today's behavior).
- Keep the wrap rebuild O(total chars) with no extra allocations vs. today's loop.
- Suppress the horizontal scrollbar geometry when wrap is on so the bar is fully hidden.
- Make the Wrap menu/shortcut path repaint the full editor surface.
- Emit a one-line pass summary from `microide_tests` so agents can grep for it.

**Non-Goals:**
- Locale-aware word segmentation (Unicode UAX #14, CJK rules) — we treat ASCII space and tab as the only break opportunities for now.
- Hyphen / dash / slash as soft-break candidates — out of scope.
- Persisting wrap-break decisions across runs.
- Reworking the trivial-layout fast path (`wrapped_row_layouts_trivial_`) — word wrap only runs when `soft_wrap_` is true, which already excludes the trivial path.

## Decisions

### Word-boundary scan in a single pass

Walk each line once, tracking `(visual, text_pos)` for the iteration cursor and the most recent whitespace break opportunity. When advancing the next character would push `next_visual - row_start_visual` past `wrap_columns`, prefer the last whitespace break inside the current row; otherwise hard-break at the current text position (so the offending character starts the next row).

Why a single pass and not a precomputed cumulative-width array per line: the array would mean an allocation per line on every reflow. A two-cursor walk pays the same character visits with zero allocation and reuses the same growth pattern (`wrapped_row_layouts_.push_back`) as today.

Why not break on punctuation: keeps the algorithm cheap (one whitespace check per char), avoids surprising splits inside identifiers, and matches the "minimum sensible improvement" the user asked for.

### Slice the visible layout to the actual row width

`VisibleWrappedRowLayout` now passes `min(visible_columns_, row.visual_end - row.visual_start)` to `BuildVisibleLine`, so a row that ended early on a whitespace break does not leak the next word's characters into its rendered slice.

For the trivial-layout path the row width still equals `visible_columns_`, so the `min(...)` is a no-op there.

### Hide the horizontal scrollbar instead of suppressing it downstream

`ComputeEditorScrollLayout` collapses `total_columns` to `visible_columns` when `viewport.soft_wrap()` is true. `ComputeScrollSurfaceLayout` then derives `max_horizontal_scroll == 0`, `show_horizontal == false`, and the scrollbar geometry is built with zero extent — no special-case branch in the renderer.

Why at compute time and not at draw time: the draw paths (`WorkspaceShellRenderFrame`, `WorkspaceShellCompareRender`, `WorkspaceShellMergeRender`) already gate on `horizontal_scrollbar.has_value()`. Collapsing the total at compute time keeps cursor hit-testing, drag handling, and scroll-wheel routing consistent without touching multiple draw TUs.

### Wrap toggle triggers full editor redraw

`WorkspaceActionContext::SetSoftWrap` now calls `operations_.request_active_tab_redraw(false)` after `apply_editor_preferences_to_all_tabs`. We do not need `MarkLayoutDirty()` because the workspace layout (sidebar/panel sizes) does not change — only the editor's visual rows do, and `request_active_tab_redraw` already invalidates the editor surface, breadcrumb, and tab strip.

### Test runner emits a final summary

On the success path, `tests/TestMain.cpp` prints `microide_tests: OK (N tests passed)` to stderr (matching where failures are printed). The "no tests matched" path already prints its own diagnostic and was left as-is.

## Risks / Trade-offs

- **Risk**: Continuation rows may begin with leading whitespace if a line starts with a long run of spaces that itself exceeds `wrap_columns`. → Mitigation: matches the existing column-break behavior and looks identical to the old wrap mode in that pathological case; documented as accepted.
- **Risk**: Wider/jagged rows might surprise users who relied on every row being exactly `wrap_columns` wide. → Mitigation: this is the requested change; selection and caret math go through `row.visual_end - row.visual_start` already, so the renderer and hit-testing handle short rows correctly.
- **Trade-off**: The `min(visible_columns_, row_columns)` in `VisibleWrappedRowLayout` runs on every wrap-on `WrappedVisualRowLayout` lookup. This is one branch and one subtraction per call — negligible vs. the existing per-character `BuildVisibleLine` walk.
- **Trade-off**: The test summary line adds one extra stderr write per test run; trivial.
