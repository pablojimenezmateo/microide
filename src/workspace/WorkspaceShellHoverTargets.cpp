#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

#include "editor/DiagnosticsRender.h"
#include "editor/TextLayout.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

TextGridInteractionLayout BuildEditorInteractionLayout(
    const render::TextRenderer& text_renderer,
    const editor::TextViewport& viewport,
    const SDL_FRect& rect) {
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect);
  return ComputeTextGridInteractionLayout(
      rect, metrics.text_x, metrics.first_line_y, metrics.line_height, text_renderer.CharWidth(),
      viewport.scroll_line(), viewport.line_count(), viewport.horizontal_scroll(),
      metrics.visible_rows, metrics.visible_columns);
}

}  // namespace

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::DiagnosticHoverTargetForViewport(
    const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction,
    std::span<const editor::PublishedDiagnostic> diagnostics,
    float x,
    float y) const {
  if (viewport.path().empty() || viewport.dirty() || diagnostics.empty() ||
      !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> line_index = VisibleTextGridLineAtY(interaction, y);
  if (!line_index.has_value() || *line_index >= viewport.lines().size()) {
    return std::nullopt;
  }

  const float line_y =
      interaction.first_line_y +
      static_cast<float>(*line_index - interaction.scroll_line) * interaction.line_height;
  const std::string& line = viewport.lines()[*line_index];
  for (const editor::PublishedDiagnostic& diagnostic : diagnostics) {
    const auto rect = editor::DiagnosticUnderlineRect(
        text_renderer_, interaction.text_x, line_y, interaction.line_height, line, *line_index,
        interaction.horizontal_scroll, interaction.visible_columns, viewport.tab_size(),
        diagnostic);
    if (rect.has_value() && Contains(*rect, x, y)) {
      return EditorHoverTarget{
          .kind = EditorHoverTarget::Kind::Diagnostic,
          .anchor_rect = *rect,
          .blame_line_index = 0,
          .diagnostic = diagnostic,
          .plugin_hover = std::nullopt,
      };
    }
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::PluginHoverTargetForLine(
    const std::filesystem::path& path,
    std::string_view line,
    std::size_t line_index,
    std::size_t tab_size,
    const TextGridInteractionLayout& interaction,
    float x,
    float y) const {
  if (path.empty() || x < interaction.text_x || !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> hovered_line = VisibleTextGridLineAtY(interaction, y);
  if (!hovered_line.has_value() || *hovered_line != line_index) {
    return std::nullopt;
  }

  const std::string line_text(line);
  if (line_text.empty()) {
    return std::nullopt;
  }

  const std::size_t line_visual_width =
      editor::TextLayout::VisualColumnForTextColumn(line_text, line_text.size(), tab_size);
  const float local_x = std::max(0.0f, x - interaction.text_x);
  const std::size_t visual_column =
      interaction.horizontal_scroll +
      static_cast<std::size_t>(std::floor(local_x / std::max(1.0f, interaction.char_width)));
  if (visual_column >= line_visual_width) {
    return std::nullopt;
  }

  const std::size_t text_column =
      editor::TextLayout::TextColumnForVisualColumn(line_text, visual_column, tab_size);
  plugin::PluginHost::HoverResult hover;
  if (!plugin_runtime_.Host().QueryHover(path, line_index + 1, text_column + 1, &hover,
                                         nullptr)) {
    return std::nullopt;
  }

  const std::size_t start_visual =
      editor::TextLayout::VisualColumnForTextColumn(line_text, text_column, tab_size);
  const std::size_t end_visual =
      text_column < line_text.size()
          ? editor::TextLayout::VisualColumnForTextColumn(
                line_text, editor::TextLayout::NextTextColumn(line_text, text_column), tab_size)
          : start_visual + 1;
  const float line_y = TextGridLineY(interaction, line_index);
  const SDL_FRect anchor_rect =
      MakeRect(TextGridCursorX(interaction, start_visual), line_y + interaction.line_height - 2.0f,
               std::max(1.0f, static_cast<float>(std::max<std::size_t>(1, end_visual - start_visual)) *
                                   interaction.char_width),
               2.0f);
  return EditorHoverTarget{
      .kind = EditorHoverTarget::Kind::Plugin,
      .anchor_rect = anchor_rect,
      .blame_line_index = 0,
      .diagnostic = std::nullopt,
      .plugin_hover = std::move(hover),
  };
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::PluginHoverTargetForViewport(
    const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction,
    float x,
    float y) const {
  if (viewport.path().empty() || viewport.dirty() || !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> line_index = VisibleTextGridLineAtY(interaction, y);
  if (!line_index.has_value() || *line_index >= viewport.lines().size()) {
    return std::nullopt;
  }

  return PluginHoverTargetForLine(viewport.path(), viewport.lines()[*line_index], *line_index,
                                  viewport.tab_size(), interaction, x, y);
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::DiagnosticHoverTargetAtPosition(
    float x,
    float y) const {
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr || !compare_tab->right_editable ||
        compare_tab->right_viewport.path().empty() || compare_tab->right_viewport.dirty()) {
      return std::nullopt;
    }

    const auto* diagnostics = context_.current_project_state.diagnostics_store.FindByPath(compare_tab->right_viewport.path());
    if (diagnostics == nullptr || diagnostics->empty()) {
      return std::nullopt;
    }

    const CompareSurfaceLayout surface =
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    const TextGridInteractionLayout interaction = ComputeTextGridInteractionLayout(
        MakeRect(surface.right_x, surface.rows_y, surface.gutter_width + surface.right_width,
                 static_cast<float>(surface.visible_rows) * surface.line_height),
        surface.right_x + surface.gutter_width, surface.rows_y, surface.line_height,
        text_renderer_.CharWidth(), static_cast<std::size_t>(std::max(0, compare_tab->scroll_row)),
        compare_tab->model.rows.size(), compare_tab->horizontal_scroll,
        static_cast<std::size_t>(surface.visible_rows), surface.right_visible_columns);
    if (!Contains(interaction.rect, x, y)) {
      return std::nullopt;
    }

    const std::optional<std::size_t> model_row = VisibleTextGridLineAtY(interaction, y);
    if (!model_row.has_value() || *model_row >= compare_tab->model.rows.size()) {
      return std::nullopt;
    }

    const auto& row = compare_tab->model.rows[*model_row];
    if (row.right_line <= 0 ||
        static_cast<std::size_t>(row.right_line) > compare_tab->right_viewport.lines().size()) {
      return std::nullopt;
    }

    const std::size_t line_index = static_cast<std::size_t>(row.right_line - 1);
    const float line_y =
        surface.rows_y +
        static_cast<float>(*model_row - static_cast<std::size_t>(std::max(0, compare_tab->scroll_row))) *
            surface.line_height;
    for (const editor::PublishedDiagnostic& diagnostic : *diagnostics) {
      const auto rect = editor::DiagnosticUnderlineRect(
          text_renderer_, interaction.text_x, line_y, surface.line_height, row.right_text,
          line_index, compare_tab->horizontal_scroll, surface.right_visible_columns,
          compare_tab->right_viewport.tab_size(), diagnostic);
      if (rect.has_value() && Contains(*rect, x, y)) {
        return EditorHoverTarget{
            .kind = EditorHoverTarget::Kind::Diagnostic,
            .anchor_rect = *rect,
            .blame_line_index = 0,
            .diagnostic = diagnostic,
            .plugin_hover = std::nullopt,
        };
      }
    }
    return std::nullopt;
  }

  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr || merge_tab->result_viewport.path().empty() ||
        merge_tab->result_viewport.dirty()) {
      return std::nullopt;
    }

    const auto* diagnostics = context_.current_project_state.diagnostics_store.FindByPath(merge_tab->result_viewport.path());
    if (diagnostics == nullptr || diagnostics->empty()) {
      return std::nullopt;
    }

    const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        layout.editor_surface, surface.center_x, surface.rows_y, surface.gutter_width,
        surface.center_width, surface.show_horizontal);
    return DiagnosticHoverTargetForViewport(
        merge_tab->result_viewport,
        BuildEditorInteractionLayout(text_renderer_, merge_tab->result_viewport, result_rect),
        std::span<const editor::PublishedDiagnostic>(*diagnostics), x, y);
  }

  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const TabEntry::EditorTabState* editor_tab = ActiveEditorTab();
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr && !active_viewport->is_placeholder()) {
    const auto* diagnostics = context_.current_project_state.diagnostics_store.FindByPath(active_viewport->path());
    return diagnostics != nullptr
               ? DiagnosticHoverTargetForViewport(
                     *active_viewport,
                     BuildEditorInteractionLayout(text_renderer_, *active_viewport,
                                                  layout.editor_surface),
                     std::span<const editor::PublishedDiagnostic>(*diagnostics), x, y)
               : std::nullopt;
  }

  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport =
        pane.active ? ActiveEditorViewport()
                    : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id) : nullptr);
    if (viewport == nullptr || viewport->path().empty() || viewport->dirty()) {
      continue;
    }

    const auto* diagnostics = context_.current_project_state.diagnostics_store.FindByPath(viewport->path());
    if (diagnostics == nullptr || diagnostics->empty()) {
      continue;
    }

    if (const auto target = DiagnosticHoverTargetForViewport(
            *viewport, BuildEditorInteractionLayout(text_renderer_, *viewport, pane.rect),
            std::span<const editor::PublishedDiagnostic>(*diagnostics), x, y);
        target.has_value()) {
      return target;
    }
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::PluginHoverTargetAtPosition(
    float x,
    float y) const {
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr || !compare_tab->right_editable ||
        compare_tab->right_viewport.path().empty() || compare_tab->right_viewport.dirty()) {
      return std::nullopt;
    }

    const CompareSurfaceLayout surface =
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    const TextGridInteractionLayout interaction = ComputeTextGridInteractionLayout(
        MakeRect(surface.right_x, surface.rows_y, surface.gutter_width + surface.right_width,
                 static_cast<float>(surface.visible_rows) * surface.line_height),
        surface.right_x + surface.gutter_width, surface.rows_y, surface.line_height,
        text_renderer_.CharWidth(), static_cast<std::size_t>(std::max(0, compare_tab->scroll_row)),
        compare_tab->model.rows.size(), compare_tab->horizontal_scroll,
        static_cast<std::size_t>(surface.visible_rows), surface.right_visible_columns);
    if (!Contains(interaction.rect, x, y)) {
      return std::nullopt;
    }

    const std::optional<std::size_t> model_row = VisibleTextGridLineAtY(interaction, y);
    if (!model_row.has_value() || *model_row >= compare_tab->model.rows.size()) {
      return std::nullopt;
    }

    const auto& row = compare_tab->model.rows[*model_row];
    if (row.right_line <= 0 ||
        static_cast<std::size_t>(row.right_line) > compare_tab->right_viewport.lines().size()) {
      return std::nullopt;
    }

    const std::size_t line_index = static_cast<std::size_t>(row.right_line - 1);
    return PluginHoverTargetForLine(compare_tab->right_viewport.path(), row.right_text, line_index,
                                    compare_tab->right_viewport.tab_size(), interaction, x, y);
  }

  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr || merge_tab->result_viewport.path().empty() ||
        merge_tab->result_viewport.dirty()) {
      return std::nullopt;
    }

    const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        layout.editor_surface, surface.center_x, surface.rows_y, surface.gutter_width,
        surface.center_width, surface.show_horizontal);
    return PluginHoverTargetForViewport(
        merge_tab->result_viewport,
        BuildEditorInteractionLayout(text_renderer_, merge_tab->result_viewport, result_rect), x, y);
  }

  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const TabEntry::EditorTabState* editor_tab = ActiveEditorTab();
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr && !active_viewport->is_placeholder()) {
    return PluginHoverTargetForViewport(
        *active_viewport,
        BuildEditorInteractionLayout(text_renderer_, *active_viewport, layout.editor_surface), x, y);
  }

  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport =
        pane.active ? ActiveEditorViewport()
                    : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id) : nullptr);
    if (viewport == nullptr || viewport->path().empty() || viewport->dirty()) {
      continue;
    }

    if (const auto target = PluginHoverTargetForViewport(
            *viewport, BuildEditorInteractionLayout(text_renderer_, *viewport, pane.rect), x, y);
        target.has_value()) {
      return target;
    }
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::EditorHoverTargetAtPosition(
    float x,
    float y) const {
  if (const editor::EditorBlameLine* blame_line = EditorBlameLineAtPosition(x, y);
      blame_line != nullptr && blame_line->interactive) {
    return EditorHoverTarget{
        .kind = EditorHoverTarget::Kind::Blame,
        .anchor_rect = blame_line->rect,
        .blame_line_index = blame_line->line_index,
        .diagnostic = std::nullopt,
        .plugin_hover = std::nullopt,
    };
  }

  if (const auto diagnostic = DiagnosticHoverTargetAtPosition(x, y); diagnostic.has_value()) {
    return diagnostic;
  }

  return PluginHoverTargetAtPosition(x, y);
}

}  // namespace microide::workspace
