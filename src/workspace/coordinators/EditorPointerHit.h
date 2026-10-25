#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "editor/EditTypes.h"
#include "editor/EditorRowYLayout.h"

namespace microide::editor {
class TextViewport;
struct EditorViewMetrics;
}  // namespace microide::editor

namespace microide::render {
class TextRenderer;
}  // namespace microide::render

namespace microide::workspace {

struct ProjectWorkspaceState;

using SettingValueGetter = std::function<std::optional<std::string>(std::string_view)>;

// Where a pointer lands in an editor pane: the mapping from screen pixels to a
// buffer position that the click, the drag and the context-menu click all share.
// They used to carry three copies of it, and the context-menu copy had drifted --
// it did not mirror the multi-line ghost-text gap, so a right-click below a
// suggestion retargeted the caret one or more lines too high.

// The gap-aware visible-row offset for a pointer at screen-y `y`. When an
// inline-inset setting (`plugins.inline_surfaces` / `plugins.code_lens_above`) is
// on, the insets are resolved into the same row gaps the renderer drew (via the
// single EditorRowYLayout mapping), and so is a multi-line ghost-text suggestion,
// so a click below a gap lands on the correct logical line. With nothing visible
// the gap list is empty and this collapses to the legacy
// `(y - first_line_y) / line_height` result.
editor::EditorRowYLayout::HitResult ResolveGapAwareRow(
    const ProjectWorkspaceState& state, const editor::TextViewport& viewport,
    const editor::EditorViewMetrics& metrics, float y, const SettingValueGetter& get_setting_value);

// Maps a click's DISPLAY visual column (which counts on-screen mid-line inlay-hint
// cells) back to the real visual column, so clicks land on the intended glyph when
// inlay hints have shifted the line's text rightward. Identity when the row has no
// mid-line hints; suppressed on soft-wrapped lines (hints are not rendered there
// in v1). Cold click path only.
int CorrectClickVisualColumnForInlay(const editor::TextViewport& viewport,
                                     const ProjectWorkspaceState& state,
                                     const render::TextRenderer& text_renderer,
                                     std::size_t visual_row, int display_visual_column);

struct EditorPointerHit {
  int visual_row = 0;
  // SCREEN-RELATIVE, as LogicalPositionForVisualHit and MoveCursorToVisualHit take
  // it: they re-add the row's own visual origin. An absolute column counts the
  // horizontal scroll twice (the caret snaps to the right edge once a line is
  // scrolled), and under soft wrap it addresses a continuation row from the start
  // of its logical line.
  int hit_column = 0;
  editor::LogicalPosition position;
};

// The buffer position under the pointer at (x, y) in a pane laid out by `metrics`.
EditorPointerHit ResolveEditorPointerHit(const ProjectWorkspaceState& state,
                                         const editor::TextViewport& viewport,
                                         const editor::EditorViewMetrics& metrics,
                                         const render::TextRenderer& text_renderer, float x,
                                         float y, const SettingValueGetter& get_setting_value);

}  // namespace microide::workspace
