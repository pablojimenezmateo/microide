#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <array>
#include <charconv>
#include <string>

namespace microide::workspace {

using namespace detail;

namespace {

void AppendUnsigned(std::string& out, std::size_t value) {
  std::array<char, 20> scratch;
  const auto [end, ec] =
      std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
  if (ec == std::errc{}) {
    out.append(scratch.data(),
               static_cast<std::size_t>(end - scratch.data()));
  }
}

std::string BuildSelectionSummary(std::size_t selected,
                                  std::size_t total,
                                  std::string_view suffix) {
  std::string summary;
  summary.reserve(48 + suffix.size());
  AppendUnsigned(summary, selected + 1);
  summary += " / ";
  AppendUnsigned(summary, total);
  summary += suffix;
  return summary;
}

}  // namespace

void WorkspaceShell::RenderOverlaySurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  if (!context_.current_project_state.overlay.visible) {
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
  const editor::TextViewport* active_viewport = ActiveEditorViewport();

  if (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Search Buffer");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_secondary, theme_.overlay_background,
               "> " + context_.current_project_state.overlay.workflow.buffer_search.query);
    const std::string summary =
        context_.current_project_state.overlay.workflow.buffer_search.matches.empty()
            ? "No matches"
            : BuildSelectionSummary(
                  context_.current_project_state.overlay.workflow.buffer_search.selected_index,
                  context_.current_project_state.overlay.workflow.buffer_search.matches.size(),
                  " matches");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               theme_.text_muted, theme_.overlay_background, summary);
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = context_.current_project_state.overlay.scroll_row + row;
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
                           context_.current_project_state.overlay.scroll_row,
                       label);
    }
  } else if (context_.current_project_state.overlay.mode == OverlayMode::BufferReplace) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Replace Buffer");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               context_.current_project_state.overlay.buffer_search_field == BufferSearchField::Search ? theme_.text_primary
                                                                         : theme_.text_secondary,
               theme_.overlay_background,
               "find: " + context_.current_project_state.overlay.workflow.buffer_search.query);
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               context_.current_project_state.overlay.buffer_search_field == BufferSearchField::Replace ? theme_.text_primary
                                                                          : theme_.text_secondary,
               theme_.overlay_background,
               "replace: " + context_.current_project_state.overlay.workflow.buffer_search.replace_text);
    const std::string summary =
        context_.current_project_state.overlay.workflow.buffer_search.matches.empty()
            ? "No matches"
            : BuildSelectionSummary(
                  context_.current_project_state.overlay.workflow.buffer_search.selected_index,
                  context_.current_project_state.overlay.workflow.buffer_search.matches.size(),
                  " matches  |  Enter replace  Ctrl+Enter replace all");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 82.0f,
               theme_.text_muted, theme_.overlay_background,
               TruncateLabel(summary, overlay.w - 36.0f));
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = context_.current_project_state.overlay.scroll_row + row;
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
                           context_.current_project_state.overlay.scroll_row,
                       label);
    }
  } else if (context_.current_project_state.overlay.mode == OverlayMode::ProjectSearch) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Project Search");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_secondary, theme_.overlay_background,
               "> " + context_.current_project_state.overlay.workflow.project_search.query);
    const std::string summary =
        context_.current_project_state.overlay.workflow.project_search.results.empty()
            ? "No results"
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
      const int item_index = context_.current_project_state.overlay.scroll_row + row;
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
                           context_.current_project_state.overlay.scroll_row,
                       label);
    }
  } else if (context_.current_project_state.overlay.mode == OverlayMode::CommitPicker) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Compare against commit");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_muted, theme_.overlay_background,
               context_.current_project_state.overlay.workflow.compare_picker.path.filename().string());
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               theme_.text_secondary, theme_.overlay_background,
               "> " + context_.current_project_state.overlay.workflow.compare_picker.query);
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = context_.current_project_state.overlay.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.compare_picker.matches.size())) {
        break;
      }
      const auto& commit =
          context_.current_project_state.overlay.workflow.compare_picker.matches[static_cast<std::size_t>(item_index)];
      draw_overlay_row(
          row,
          static_cast<int>(context_.current_project_state.overlay.workflow.compare_picker.selected_index) -
              context_.current_project_state.overlay.scroll_row,
          commit.short_hash + "  " + commit.subject);
    }
    if (context_.current_project_state.overlay.workflow.compare_picker.matches.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 92.0f,
                 theme_.text_muted, theme_.overlay_background, "No matching commits");
    }
  } else if (context_.current_project_state.overlay.mode == OverlayMode::Completion) {
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
      const int item_index = context_.current_project_state.overlay.scroll_row + row;
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
              context_.current_project_state.overlay.scroll_row,
          TruncateLabel(label, overlay.w - 36.0f));
    }
    if (!context_.current_project_state.overlay.workflow.completion.error.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
                 theme_.diff_deleted, theme_.overlay_background,
                 TruncateLabel(context_.current_project_state.overlay.workflow.completion.error,
                               overlay.w - 36.0f));
    }
  } else if (context_.current_project_state.overlay.mode == OverlayMode::CodeActions) {
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
      const int item_index = context_.current_project_state.overlay.scroll_row + row;
      if (item_index >= static_cast<int>(context_.current_project_state.overlay.workflow.code_actions.items.size())) {
        break;
      }
      const auto& item =
          context_.current_project_state.overlay.workflow.code_actions.items[static_cast<std::size_t>(item_index)];
      draw_overlay_row(
          row,
          static_cast<int>(context_.current_project_state.overlay.workflow.code_actions.selected_index) -
              context_.current_project_state.overlay.scroll_row,
          TruncateLabel(item.title, overlay.w - 36.0f));
    }
    if (!context_.current_project_state.overlay.workflow.code_actions.error.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
                 theme_.diff_deleted, theme_.overlay_background,
                 TruncateLabel(context_.current_project_state.overlay.workflow.code_actions.error,
                               overlay.w - 36.0f));
    }
  } else if (context_.current_project_state.overlay.mode == OverlayMode::TaskPicker) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Tasks");
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_muted, theme_.overlay_background,
               "Enter runs the selected task");
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = context_.current_project_state.overlay.scroll_row + row;
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
              context_.current_project_state.overlay.scroll_row,
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
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 44.0f,
               theme_.text_secondary, theme_.overlay_background, "> " + context_.current_project_state.file_finder.query());

    const auto& results = context_.current_project_state.file_finder.results();
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = context_.current_project_state.overlay.scroll_row + row;
      if (item_index >= static_cast<int>(results.size())) {
        break;
      }
      draw_overlay_row(
          row,
          static_cast<int>(context_.current_project_state.file_finder.selected_index()) - context_.current_project_state.overlay.scroll_row,
          results[static_cast<std::size_t>(item_index)].path_string);
    }
    if (results.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 80.0f,
                 theme_.text_muted, theme_.overlay_background, "No matching files");
    }
  }

  draw_vertical_scrollbar(overlay_list_layout.list_rect, static_cast<float>(OverlayItemCount()),
                          overlay_list_layout.visible_units,
                          static_cast<float>(context_.current_project_state.overlay.scroll_row),
                         context_.interaction_state.drag_target == DragTarget::OverlayScrollbar);
}

}  // namespace microide::workspace
