#include "workspace/coordinators/EditorPointerHit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "editor/EditorInsetLayout.h"
#include "editor/EditorViewRenderer.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/TextViewport.h"
#include "render/TextRenderer.h"
#include "util/PathMatch.h"
#include "workspace/SettingFlags.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

namespace {

bool SettingOn(const SettingValueGetter& get_setting_value, std::string_view id) {
  return get_setting_value && SettingFlagEnabled(get_setting_value(id));
}

}  // namespace

editor::EditorRowYLayout::HitResult ResolveGapAwareRow(
    const ProjectWorkspaceState& state, const editor::TextViewport& viewport,
    const editor::EditorViewMetrics& metrics, float y, const SettingValueGetter& get_setting_value) {
  // No plugin/LSP contribution: no insets can exist, so the gap list is empty and
  // the mapping collapses to the legacy formula. Skip the store probing entirely.
  const auto* pres = state.plugin_presentation_if_present();
  if (pres == nullptr) {
    return editor::EditorRowYLayout(metrics.first_line_y, metrics.line_height,
                                    static_cast<std::uint32_t>(viewport.scroll_line()))
        .HitTest(y, metrics.visible_rows);
  }
  editor::InsetGapOptions options{
      .inline_surfaces = SettingOn(get_setting_value, "plugins.inline_surfaces"),
      .code_lens_above = SettingOn(get_setting_value, "plugins.code_lens_above"),
      .code_lens_height = metrics.line_height};
  // Mirror the render path's ghost-text Below gap (geometry only) so a click below
  // a multi-line suggestion lands on the line the user actually sees.
  if (const auto* ghost = state.ghost_text_if_present();
      ghost != nullptr && ghost->lines.size() > 1 &&
      SettingOn(get_setting_value, "plugins.ghost_text") &&
      util::SamePathNormalized(viewport.path(), ghost->path)) {
    options.ghost_anchor_line = ghost->anchor_line;
    options.ghost_height = static_cast<float>(ghost->lines.size() - 1) * metrics.line_height;
  }
  return editor::ResolveInsetClick(pres->surfaces, pres->decorations, viewport,
                                   metrics.first_line_y, metrics.line_height, metrics.visible_rows,
                                   y, options)
      .hit;
}

int CorrectClickVisualColumnForInlay(const editor::TextViewport& viewport,
                                     const ProjectWorkspaceState& state,
                                     const render::TextRenderer& text_renderer,
                                     std::size_t visual_row, int display_visual_column) {
  if (display_visual_column < 0 || viewport.soft_wrap() ||
      visual_row >= viewport.visual_line_count()) {
    return display_visual_column;
  }
  const auto* pres = state.plugin_presentation_if_present();
  if (pres == nullptr) {
    return display_visual_column;
  }
  const editor::FileDecorations* file_dec = pres->decorations.FindByPath(viewport.path());
  if (file_dec == nullptr) {
    return display_visual_column;
  }
  const std::size_t line_index = viewport.VisualRowLineIndex(visual_row);
  const auto inline_texts = file_dec->InlineTextsForLine(static_cast<std::uint32_t>(line_index));
  if (inline_texts.empty()) {
    return display_visual_column;
  }
  const editor::LayoutLine& layout = viewport.VisibleLineLayoutRef(line_index);
  const std::size_t row_start = viewport.horizontal_scroll();
  const std::size_t row_end = row_start + viewport.visible_columns();
  return static_cast<int>(editor::RealVisualColumnForDisplayColumn(
      inline_texts, &layout, nullptr, row_start, row_end, text_renderer,
      text_renderer.CharWidth(), static_cast<std::size_t>(display_visual_column)));
}

EditorPointerHit ResolveEditorPointerHit(const ProjectWorkspaceState& state,
                                         const editor::TextViewport& viewport,
                                         const editor::EditorViewMetrics& metrics,
                                         const render::TextRenderer& text_renderer, float x,
                                         float y, const SettingValueGetter& get_setting_value) {
  const std::size_t row = ResolveGapAwareRow(state, viewport, metrics, y, get_setting_value).row;
  const float text_offset_x = std::max(0.0f, x - metrics.text_x);
  const int display_visual_column = static_cast<int>(
      viewport.horizontal_scroll() +
      static_cast<std::size_t>(
          std::max(0L, std::lround(text_offset_x / std::max(1.0f, text_renderer.CharWidth())))));
  EditorPointerHit hit;
  hit.visual_row = static_cast<int>(viewport.scroll_line() + row);
  const int visual_column = CorrectClickVisualColumnForInlay(
      viewport, state, text_renderer, static_cast<std::size_t>(hit.visual_row),
      display_visual_column);
  hit.hit_column = std::max(0, visual_column - static_cast<int>(viewport.horizontal_scroll()));
  hit.position = viewport.LogicalPositionForVisualHit(hit.visual_row, hit.hit_column);
  return hit;
}

}  // namespace microide::workspace
