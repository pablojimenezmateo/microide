#include "workspace/WorkspaceShellRenderPrimitives.h"

#include "workspace/RenderViewModelBuilder.h"

#include <string>

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderOverlaySurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  const OverlaySurfaceViewModel overlay_vm = RenderViewModelBuilder(context_).BuildOverlaySurface();
  if (!overlay_vm.visible) {
    return;
  }

  const TextInputSurface current_surface = CurrentTextInputSurface();
  const bool overlay_needs_visual =
      current_surface == TextInputSurface::BufferSearch ||
      current_surface == TextInputSurface::BufferReplaceSearch ||
      current_surface == TextInputSurface::BufferReplaceReplace ||
      current_surface == TextInputSurface::ProjectSearchOverlay ||
      current_surface == TextInputSurface::CommitPicker;
  const auto visual =
      overlay_needs_visual ? BuildActiveTextInputVisual(layout, std::nullopt) : std::nullopt;
  const auto overlay_display_text = [&](TextInputSurface surface,
                                        const std::string& fallback) -> std::string_view {
    if (visual.has_value() && visual->surface == surface && !visual->displayed_text.empty()) {
      return visual->displayed_text;
    }
    return fallback;
  };

  const auto draw_vertical_scrollbar = [&](const SDL_FRect& area,
                                           float total_units,
                                           float visible_units,
                                           float scroll_units,
                                           bool active = false,
                                           bool reserve_horizontal = false) {
    if (const auto geometry = MakeVerticalScrollbarGeometry(area, total_units, visible_units,
                                                            scroll_units, reserve_horizontal);
        geometry.has_value()) {
      DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
    }
  };

  DrawFilledRect(renderer, layout.editor_area, theme_.overlay_backdrop);
  const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
  constexpr float kOverlayInset = 18.0f;
  DrawTitledCardFrame(renderer, theme_, overlay, 32.0f, CardStyle::Overlay);
  const auto overlay_field_rect = [&](float text_y) {
    return MakeRect(overlay.x + 12.0f, text_y - 4.0f, std::max(0.0f, overlay.w - 24.0f), 18.0f);
  };

  ClampOverlayScrollRow(overlay);
  const auto overlay_list_layout = ComputeOverlayListLayout(overlay);
  const auto draw_overlay_row = [&](int row_index, int selected_index, std::string_view label) {
    const bool selected = row_index == selected_index;
    SDL_FRect row = ScrollableListRowRect(overlay_list_layout, row_index);
    DrawSelectableRowBackground(renderer, theme_, row, theme_.surface_raised, selected, selected);
    DrawVCenteredTextOn(text_renderer_, renderer, row, 6.0f,
                        selected ? theme_.text_primary : theme_.text_secondary,
                        selected ? theme_.row_highlight : theme_.surface_raised,
                        TruncateLabel(label, row.w - 12.0f));
  };
  const editor::TextViewport* active_viewport = ActiveEditorViewport();

  if (overlay_vm.mode == OverlayMode::BufferSearch) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Search Buffer");
    const std::string bs_fallback =
        "> " + context_.current_project_state.overlay.workflow.buffer_search.query.text;
    DrawTextFieldFrame(renderer, theme_, overlay_field_rect(overlay.y + 44.0f),
                       current_surface == TextInputSurface::BufferSearch);
    DrawSingleLineTextTail(
        renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
        std::max(1.0f, overlay.w - kOverlayInset * 2.0f), theme_.text_secondary,
        theme_.surface_background,
        overlay_display_text(TextInputSurface::BufferSearch, bs_fallback));
    const std::string summary =
        context_.current_project_state.overlay.workflow.buffer_search.matches.empty()
            ? FormatEmptyState("matches")
            : BuildSelectionSummary(
                  context_.current_project_state.overlay.workflow.buffer_search.selected_index,
                  context_.current_project_state.overlay.workflow.buffer_search.matches.size(),
                  " matches");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               theme_.text_muted, theme_.overlay_background, summary);
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.buffer_search.matches.size())) {
        break;
      }
      const auto& match =
          context_.current_project_state.overlay.workflow.buffer_search.matches[static_cast<std::size_t>(item_index)];
      std::string label;
      label.reserve(64);
      label += "Ln ";
      AppendUnsigned(label, match.start.line + 1);
      label += ", Col ";
      AppendUnsigned(label, match.start.column + 1);
      label += "  ";
      label += TruncateLabel(active_viewport != nullptr
                                 ? active_viewport->lines()[match.start.line]
                                 : std::string_view{},
                             overlay.w - 150.0f);
      draw_overlay_row(row,
                       static_cast<int>(context_.current_project_state.overlay.workflow.buffer_search.selected_index) -
                           overlay_vm.scroll_row,
                       label);
    }
  } else if (overlay_vm.mode == OverlayMode::BufferReplace) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Replace Buffer");
    const std::string br_search_fallback =
        "find: " + context_.current_project_state.overlay.workflow.buffer_search.query.text;
    const std::string br_replace_fallback =
        "replace: " + context_.current_project_state.overlay.workflow.buffer_search.replace_text.text;
    DrawTextFieldFrame(
        renderer, theme_, overlay_field_rect(overlay.y + 44.0f),
        current_surface == TextInputSurface::BufferReplaceSearch);
    DrawTextFieldFrame(
        renderer, theme_, overlay_field_rect(overlay.y + 62.0f),
        current_surface == TextInputSurface::BufferReplaceReplace);
    DrawSingleLineTextTail(
        renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
        std::max(1.0f, overlay.w - kOverlayInset * 2.0f),
        context_.current_project_state.overlay.buffer_search_field == BufferSearchField::Search
            ? theme_.text_primary
            : theme_.text_secondary,
        theme_.surface_background,
        overlay_display_text(TextInputSurface::BufferReplaceSearch, br_search_fallback));
    DrawSingleLineTextTail(
        renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
        std::max(1.0f, overlay.w - kOverlayInset * 2.0f),
        context_.current_project_state.overlay.buffer_search_field == BufferSearchField::Replace
            ? theme_.text_primary
            : theme_.text_secondary,
        theme_.surface_background,
        overlay_display_text(TextInputSurface::BufferReplaceReplace, br_replace_fallback));
    const std::string summary =
        context_.current_project_state.overlay.workflow.buffer_search.matches.empty()
            ? FormatEmptyState("matches")
            : BuildSelectionSummary(
                  context_.current_project_state.overlay.workflow.buffer_search.selected_index,
                  context_.current_project_state.overlay.workflow.buffer_search.matches.size(),
                  " matches");
    const std::string replace_hints = JoinHintSegments({"Enter replace", "Ctrl+Enter replace all"});
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 82.0f,
               theme_.text_muted, theme_.overlay_background,
               TruncateLabel(summary + "  |  " + replace_hints, overlay.w - 36.0f));
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.buffer_search.matches.size())) {
        break;
      }
      const auto& match =
          context_.current_project_state.overlay.workflow.buffer_search.matches[static_cast<std::size_t>(item_index)];
      std::string label;
      label.reserve(64);
      label += "Ln ";
      AppendUnsigned(label, match.start.line + 1);
      label += ", Col ";
      AppendUnsigned(label, match.start.column + 1);
      label += "  ";
      label += TruncateLabel(active_viewport != nullptr
                                 ? active_viewport->lines()[match.start.line]
                                 : std::string_view{},
                             overlay.w - 150.0f);
      draw_overlay_row(row,
                       static_cast<int>(context_.current_project_state.overlay.workflow.buffer_search.selected_index) -
                           overlay_vm.scroll_row,
                       label);
    }
  } else if (overlay_vm.mode == OverlayMode::ProjectSearch) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Project Search");
    const std::string ps_fallback =
        "> " + context_.current_project_state.overlay.workflow.project_search.query.text;
    DrawTextFieldFrame(renderer, theme_, overlay_field_rect(overlay.y + 44.0f),
                       current_surface == TextInputSurface::ProjectSearchOverlay);
    DrawSingleLineTextTail(
        renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
        std::max(1.0f, overlay.w - kOverlayInset * 2.0f), theme_.text_secondary,
        theme_.surface_background,
        overlay_display_text(TextInputSurface::ProjectSearchOverlay, ps_fallback));
    const std::string summary =
        context_.current_project_state.overlay.workflow.project_search.results.empty()
            ? FormatEmptyState("results")
        : context_.current_project_state.overlay.workflow.project_search.truncated
            ? BuildSelectionSummary(
                  context_.current_project_state.overlay.workflow.project_search.selected_index,
                  context_.current_project_state.overlay.workflow.project_search.results.size(),
                  " shown (capped)")
            : BuildSelectionSummary(
                  context_.current_project_state.overlay.workflow.project_search.selected_index,
                  context_.current_project_state.overlay.workflow.project_search.results.size(),
                  " results");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               theme_.text_muted, theme_.overlay_background, summary);
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.project_search.results.size())) {
        break;
      }
      const auto& result =
          context_.current_project_state.overlay.workflow.project_search.results[static_cast<std::size_t>(item_index)];
      std::string label =
          result.relative_path_string.empty() ? result.relative_path.string()
                                              : result.relative_path_string;
      label.reserve(label.size() + 64);
      label += ":";
      AppendUnsigned(label, result.line + 1);
      label += ":";
      AppendUnsigned(label, result.column + 1);
      label += "  ";
      label += TruncateLabel(result.preview, overlay.w - 220.0f);
      draw_overlay_row(row,
                       static_cast<int>(context_.current_project_state.overlay.workflow.project_search.selected_index) -
                           overlay_vm.scroll_row,
                       label);
    }
  } else if (overlay_vm.mode == OverlayMode::CommitPicker) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Compare against commit");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_muted, theme_.overlay_background,
               context_.current_project_state.overlay.workflow.compare_picker.path.filename().string());
    const std::string cp_fallback =
        "> " + context_.current_project_state.overlay.workflow.compare_picker.query.text;
    DrawTextFieldFrame(renderer, theme_, overlay_field_rect(overlay.y + 62.0f),
                       current_surface == TextInputSurface::CommitPicker);
    DrawSingleLineTextTail(
        renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
        std::max(1.0f, overlay.w - kOverlayInset * 2.0f), theme_.text_secondary,
        theme_.surface_background,
        overlay_display_text(TextInputSurface::CommitPicker, cp_fallback));
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.compare_picker.matches.size())) {
        break;
      }
      const auto& commit =
          context_.current_project_state.overlay.workflow.compare_picker.matches[static_cast<std::size_t>(item_index)];
      draw_overlay_row(
          row,
          static_cast<int>(context_.current_project_state.overlay.workflow.compare_picker.selected_index) -
              overlay_vm.scroll_row,
          commit.short_hash + "  " + commit.subject);
    }
    if (context_.current_project_state.overlay.workflow.compare_picker.matches.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 92.0f,
                 theme_.text_muted, theme_.overlay_background, FormatEmptyState("matching commits"));
    }
  } else if (overlay_vm.mode == OverlayMode::Completion) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Completions");
    const std::string summary =
        context_.current_project_state.overlay.workflow.completion.items.empty()
            ? "No completions"
            : BuildSelectionSummary(
                  context_.current_project_state.overlay.workflow.completion.selected_index,
                  context_.current_project_state.overlay.workflow.completion.items.size(),
                  " completions");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_muted, theme_.overlay_background,
               TruncateLabel(summary, overlay.w - 36.0f));
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.completion.items.size())) {
        break;
      }
      const auto& item =
          context_.current_project_state.overlay.workflow.completion.items[static_cast<std::size_t>(item_index)];
      const std::string label =
          item.detail.empty() ? item.label : item.label + "  " + item.detail;
      draw_overlay_row(
          row,
          static_cast<int>(context_.current_project_state.overlay.workflow.completion.selected_index) -
              overlay_vm.scroll_row,
          TruncateLabel(label, overlay.w - 36.0f));
    }
    if (!context_.current_project_state.overlay.workflow.completion.error.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
                 theme_.diff_deleted, theme_.overlay_background,
                 TruncateLabel(context_.current_project_state.overlay.workflow.completion.error,
                               overlay.w - 36.0f));
    }
  } else if (overlay_vm.mode == OverlayMode::CodeActions) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Code Actions");
    const std::string summary =
        context_.current_project_state.overlay.workflow.code_actions.items.empty()
            ? "No actions"
            : BuildSelectionSummary(
                  context_.current_project_state.overlay.workflow.code_actions.selected_index,
                  context_.current_project_state.overlay.workflow.code_actions.items.size(),
                  " actions");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_muted, theme_.overlay_background,
               TruncateLabel(summary, overlay.w - 36.0f));
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.code_actions.items.size())) {
        break;
      }
      const auto& item =
          context_.current_project_state.overlay.workflow.code_actions.items[static_cast<std::size_t>(item_index)];
      draw_overlay_row(
          row,
          static_cast<int>(context_.current_project_state.overlay.workflow.code_actions.selected_index) -
              overlay_vm.scroll_row,
          TruncateLabel(item.title, overlay.w - 36.0f));
    }
    if (!context_.current_project_state.overlay.workflow.code_actions.error.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
                 theme_.diff_deleted, theme_.overlay_background,
                 TruncateLabel(context_.current_project_state.overlay.workflow.code_actions.error,
                               overlay.w - 36.0f));
    }
  } else if (overlay_vm.mode == OverlayMode::TaskPicker) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Tasks");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_muted, theme_.overlay_background,
               JoinHintSegments({"Enter run selected task", "Esc cancel"}));
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.task_picker.entries.size())) {
        break;
      }
      const auto& item =
          context_.current_project_state.overlay.workflow.task_picker.entries[static_cast<std::size_t>(item_index)];
      const std::string label =
          item.group.empty() ? item.label : item.group + "  " + item.label;
      draw_overlay_row(
          row,
          static_cast<int>(context_.current_project_state.overlay.workflow.task_picker.selected_index) -
              overlay_vm.scroll_row,
          TruncateLabel(label, overlay.w - 36.0f));
    }
    if (!context_.current_project_state.overlay.workflow.task_picker.error.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
                 theme_.diff_deleted, theme_.overlay_background,
                 TruncateLabel(context_.current_project_state.overlay.workflow.task_picker.error,
                               overlay.w - 36.0f));
    }
  } else {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Find File");
    DrawTextFieldFrame(renderer, theme_, overlay_field_rect(overlay.y + 44.0f),
                       current_surface == TextInputSurface::FileFinder);
    DrawSingleLineTextTail(renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
                           std::max(1.0f, overlay.w - kOverlayInset * 2.0f),
                           theme_.text_secondary, theme_.surface_background,
                           "> " + context_.current_project_state.file_finder.query());

    const auto& results = context_.current_project_state.file_finder.results();
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(results.size())) {
        break;
      }
      draw_overlay_row(
          row,
          static_cast<int>(context_.current_project_state.file_finder.selected_index()) - overlay_vm.scroll_row,
          results[static_cast<std::size_t>(item_index)].path_string);
    }
    if (results.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 80.0f,
                 theme_.text_muted, theme_.overlay_background, FormatEmptyState("matching files"));
    }
  }

  draw_vertical_scrollbar(overlay_list_layout.list_rect, static_cast<float>(OverlayItemCount()),
                          overlay_list_layout.visible_units,
                          static_cast<float>(overlay_vm.scroll_row),
                         context_.interaction_state.drag_target == DragTarget::OverlayScrollbar);
}

}  // namespace microide::workspace
