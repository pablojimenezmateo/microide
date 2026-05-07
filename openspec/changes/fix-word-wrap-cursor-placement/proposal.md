## Why

Soft wrap (`editor.wrap = "word"`) appears completely broken to users: enabling it has no visible effect. Investigation shows `TextViewport` correctly computes and caches a wrapped-row layout (`wrapped_row_layouts_`), but the render path in `EditorViewRenderer` ignores that layout and continues to paint raw logical lines clipped horizontally. The viewport's wrap-aware vertical navigation, hit-testing, and scroll model are therefore exercised against a UI that never displays wrapped rows. Multi-caret vertical motion is also silently broken under wrap: only the primary caret advances when Up/Down is pressed, leaving secondary carets stranded and violating the "carets move as one command" contract.

## What Changes

- Render `EditorViewRenderer` against `TextViewport::VisibleWrappedRowLayout()` when `soft_wrap()` is true, so each visible row corresponds to one wrapped segment instead of one logical line. Horizontal scrolling is suppressed in this mode (already zeroed in viewport state).
- Route gutter line-number rendering through the same wrapped-row layout: only the first visual row of a logical line shows its number; continuation rows render an empty gutter.
- Move secondary carets through the same wrapped-row vertical-motion path as the primary caret, preserving each caret's own `preferred_column_` (sticky target X) so multi-caret Up/Down is consistent with single-caret behaviour and matches the multi-caret spec.
- Fix mouse hit-testing to convert (visual_row, visual_column) → (logical_line, logical_column) when wrap is enabled, so click-to-place lands where the user pointed.
- Cache the wrapped-row layout per `(layout_revision, tab_size, visible_columns)` so editing, cursor motion, and frame paint are O(visible_rows) on cache hit and O(document_lines) only on invalidation. No per-frame and no per-keystroke recomputation.
- Add regression coverage for: render iterates wrapped rows when wrap is on; multi-caret Up/Down with wrap; click hit-testing on a continuation row; preferred-column stickiness across continuation boundaries; cache reuse across frames.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `editor-multicursor-and-wrap`: Tighten the soft-wrap requirement so the wrapped-row layout is the single source of truth for *render*, *gutter*, *vertical motion of every caret*, and *mouse hit-testing*. Add an explicit performance requirement that wrap layout is cached and only invalidated on document edit, tab-size change, or viewport-width change.

## Impact

- Affected code:
  - `src/editor/EditorViewRenderer.cpp` (render loop, gutter painter, hit-testing helpers)
  - `src/editor/TextViewport.{h,cpp}` (multi-caret vertical motion, hit-testing, public visual-row API surface)
  - `tests/TextViewportTests.cpp`, `tests/EditorViewRendererTests.cpp` (new coverage)
- Affected specs: `openspec/specs/editor-multicursor-and-wrap/spec.md` (delta).
- No persisted-state, plugin, or platform impact. The `editor.wrap` setting key, default, and persistence path are unchanged.
- Performance budget: wrap-on rendering must stay within the existing per-frame budget for editor paint; cache hit path is O(visible rows) and allocates nothing on the hot path.
