#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <string>

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderOverlaySurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  if (!surface_.overlay_visible) {
    return;
  }

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
  const SDL_FRect overlay_header = MakeRect(overlay.x, overlay.y, overlay.w, 30.0f);
  constexpr float kOverlayInset = 18.0f;
  DrawFilledRect(renderer, overlay, theme_.overlay_background);
  DrawRect(renderer, overlay, theme_.border);
  DrawFilledRect(renderer, overlay_header, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(overlay_header.x,
                          overlay_header.y + overlay_header.h - kWorkspaceDividerThickness,
                          overlay_header.w, kWorkspaceDividerThickness),
                 theme_.border);

  ClampOverlayScrollRow(overlay);
  const auto overlay_list_layout = ComputeOverlayListLayout(overlay);
  const auto draw_overlay_row = [&](int row_index, int selected_index, std::string_view label) {
    const bool selected = row_index == selected_index;
    SDL_FRect row = ScrollableListRowRect(overlay_list_layout, row_index);
    DrawFilledRect(renderer, row, selected ? theme_.row_highlight : theme_.surface_raised);
    DrawVCenteredTextOn(text_renderer_, renderer, row, 6.0f,
                        selected ? theme_.text_primary : theme_.text_secondary,
                        selected ? theme_.row_highlight : theme_.surface_raised,
                        TruncateLabel(label, row.w - 12.0f));
  };

  if (surface_.overlay_mode == OverlayMode::BufferSearch) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Search Buffer");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_secondary, theme_.overlay_background,
               "> " + overlay_workflow_.buffer_search.query);
    const std::string summary =
        overlay_workflow_.buffer_search.matches.empty()
            ? "No matches"
            : std::to_string(overlay_workflow_.buffer_search.selected_index + 1) + " / " +
                  std::to_string(overlay_workflow_.buffer_search.matches.size()) + " matches";
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               theme_.text_muted, theme_.overlay_background, summary);
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = surface_.overlay_scroll_row + row;
      if (item_index >= static_cast<int>(overlay_workflow_.buffer_search.matches.size())) {
        break;
      }
      const auto& match =
          overlay_workflow_.buffer_search.matches[static_cast<std::size_t>(item_index)];
      const std::string label = "Ln " + std::to_string(match.start.line + 1) + ", Col " +
                                std::to_string(match.start.column + 1) + "  " +
                                TruncateLabel(text_viewport_.lines()[match.start.line],
                                              overlay.w - 150.0f);
      draw_overlay_row(row,
                       static_cast<int>(overlay_workflow_.buffer_search.selected_index) -
                           surface_.overlay_scroll_row,
                       label);
    }
  } else if (surface_.overlay_mode == OverlayMode::BufferReplace) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Replace Buffer");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               surface_.buffer_search_field == BufferSearchField::Search ? theme_.text_primary
                                                                         : theme_.text_secondary,
               theme_.overlay_background,
               "find: " + overlay_workflow_.buffer_search.query);
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               surface_.buffer_search_field == BufferSearchField::Replace ? theme_.text_primary
                                                                          : theme_.text_secondary,
               theme_.overlay_background,
               "replace: " + overlay_workflow_.buffer_search.replace_text);
    const std::string summary =
        overlay_workflow_.buffer_search.matches.empty()
            ? "No matches"
            : std::to_string(overlay_workflow_.buffer_search.selected_index + 1) + " / " +
                  std::to_string(overlay_workflow_.buffer_search.matches.size()) +
                  " matches  |  Enter replace  Ctrl+Enter replace all";
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 82.0f,
               theme_.text_muted, theme_.overlay_background,
               TruncateLabel(summary, overlay.w - 36.0f));
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = surface_.overlay_scroll_row + row;
      if (item_index >= static_cast<int>(overlay_workflow_.buffer_search.matches.size())) {
        break;
      }
      const auto& match =
          overlay_workflow_.buffer_search.matches[static_cast<std::size_t>(item_index)];
      const std::string label = "Ln " + std::to_string(match.start.line + 1) + ", Col " +
                                std::to_string(match.start.column + 1) + "  " +
                                TruncateLabel(text_viewport_.lines()[match.start.line],
                                              overlay.w - 150.0f);
      draw_overlay_row(row,
                       static_cast<int>(overlay_workflow_.buffer_search.selected_index) -
                           surface_.overlay_scroll_row,
                       label);
    }
  } else if (surface_.overlay_mode == OverlayMode::ProjectSearch) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Project Search");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_secondary, theme_.overlay_background,
               "> " + overlay_workflow_.project_search.query);
    const std::string summary =
        overlay_workflow_.project_search.results.empty()
            ? "No results"
        : overlay_workflow_.project_search.truncated
            ? std::to_string(overlay_workflow_.project_search.selected_index + 1) + " / " +
                  std::to_string(overlay_workflow_.project_search.results.size()) + " shown (capped)"
            : std::to_string(overlay_workflow_.project_search.selected_index + 1) + " / " +
                  std::to_string(overlay_workflow_.project_search.results.size()) + " results";
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               theme_.text_muted, theme_.overlay_background, summary);
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = surface_.overlay_scroll_row + row;
      if (item_index >= static_cast<int>(overlay_workflow_.project_search.results.size())) {
        break;
      }
      const auto& result =
          overlay_workflow_.project_search.results[static_cast<std::size_t>(item_index)];
      const std::string label =
          result.relative_path.string() + ":" + std::to_string(result.line + 1) + ":" +
          std::to_string(result.column + 1) + "  " + TruncateLabel(result.preview, overlay.w - 220.0f);
      draw_overlay_row(row,
                       static_cast<int>(overlay_workflow_.project_search.selected_index) -
                           surface_.overlay_scroll_row,
                       label);
    }
  } else if (surface_.overlay_mode == OverlayMode::CommitPicker) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Compare against commit");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_muted, theme_.overlay_background,
               overlay_workflow_.compare_picker.path.filename().string());
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               theme_.text_secondary, theme_.overlay_background,
               "> " + overlay_workflow_.compare_picker.query);
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = surface_.overlay_scroll_row + row;
      if (item_index >= static_cast<int>(overlay_workflow_.compare_picker.matches.size())) {
        break;
      }
      const auto& commit =
          overlay_workflow_.compare_picker.matches[static_cast<std::size_t>(item_index)];
      draw_overlay_row(
          row,
          static_cast<int>(overlay_workflow_.compare_picker.selected_index) -
              surface_.overlay_scroll_row,
          commit.short_hash + "  " + commit.subject);
    }
    if (overlay_workflow_.compare_picker.matches.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 92.0f,
                 theme_.text_muted, theme_.overlay_background, "No matching commits");
    }
  } else {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Find File");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_secondary, theme_.overlay_background, "> " + file_finder_.query());

    const auto& results = file_finder_.results();
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = surface_.overlay_scroll_row + row;
      if (item_index >= static_cast<int>(results.size())) {
        break;
      }
      draw_overlay_row(
          row,
          static_cast<int>(file_finder_.selected_index()) - surface_.overlay_scroll_row,
          results[static_cast<std::size_t>(item_index)].relative_path.string());
    }
    if (results.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 80.0f,
                 theme_.text_muted, theme_.overlay_background, "No matching files");
    }
  }

  draw_vertical_scrollbar(overlay_list_layout.list_rect, static_cast<float>(OverlayItemCount()),
                          overlay_list_layout.visible_units,
                          static_cast<float>(surface_.overlay_scroll_row),
                          surface_.drag_target == DragTarget::OverlayScrollbar);
}

}  // namespace microide::workspace
