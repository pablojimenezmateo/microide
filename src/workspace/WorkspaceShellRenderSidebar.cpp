#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "editor/DiagnosticsRender.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

using namespace detail;

namespace {

constexpr float kSidebarInset = 10.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;

}  // namespace

void WorkspaceShell::RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  if (!sidebar_state_.visible) {
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
  const auto draw_action_button = [&](const SDL_FRect& button_rect,
                                      std::string_view label,
                                      bool enabled,
                                      bool destructive = false) {
    const bool hovered =
        enabled && last_mouse_position_valid_ && Contains(button_rect, last_mouse_x_, last_mouse_y_);
    const SDL_Color fill = hovered ? theme_.row_highlight : theme_.surface_raised;
    const SDL_Color border =
        !enabled ? theme_.border
                 : hovered ? (destructive ? theme_.diff_deleted : theme_.accent)
                           : theme_.border;
    const SDL_Color text = !enabled ? theme_.text_muted
                                    : hovered ? theme_.text_primary
                                              : destructive ? theme_.diff_deleted : theme_.accent;
    DrawFilledRect(renderer, button_rect, fill);
    DrawRect(renderer, button_rect, border);
    DrawCenteredTextOn(text_renderer_, renderer, button_rect, text, fill, label);
  };

  const SDL_FRect sidebar_mode_rect = SidebarModeControlRect(layout.sidebar);
  const bool sidebar_mode_hovered =
      last_mouse_position_valid_ && Contains(sidebar_mode_rect, last_mouse_x_, last_mouse_y_);
  const bool sidebar_mode_open =
      menu_state_.menu_bar_open && menu_state_.active_menu_id == MenuId::SidebarMode &&
      menu_state_.active_menu_anchor_rect.has_value();
  DrawFilledRect(renderer, sidebar_mode_rect,
                 sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                          : theme_.surface_raised);
  DrawRect(renderer, sidebar_mode_rect,
           sidebar_mode_open ? theme_.accent
                             : sidebar_mode_hovered ? theme_.text_secondary : theme_.border);
  DrawVCenteredTextOn(text_renderer_, renderer, sidebar_mode_rect, 8.0f,
                      sidebar_mode_open || sidebar_mode_hovered ? theme_.text_primary
                                                                : theme_.text_secondary,
                      sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                                : theme_.surface_raised,
                      SidebarModeControlLabel());
  DrawChevron(renderer, sidebar_mode_rect.x + sidebar_mode_rect.w - 18.0f,
              sidebar_mode_rect.y + sidebar_mode_rect.h * 0.5f, true,
              sidebar_mode_open || sidebar_mode_hovered ? theme_.text_primary : theme_.text_muted);

  if (sidebar_state_.mode == SidebarMode::Search) {
    const std::string active_query =
        overlay_workflow_.project_search.editing &&
                overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query
            ? overlay_workflow_.project_search.edit_buffer
            : overlay_workflow_.project_search.query;
    const std::string active_replace =
        overlay_workflow_.project_search.editing &&
                overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Replace
            ? overlay_workflow_.project_search.edit_buffer
            : overlay_workflow_.project_search.replace_text;
    DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
               layout.sidebar.y + 38.0f,
               overlay_workflow_.project_search.editing &&
                       overlay_workflow_.project_search.edit_field ==
                           ProjectSearchEditField::Query
                   ? theme_.text_primary
                   : theme_.text_secondary,
               theme_.surface_background,
               TruncateLabel("search> " + active_query, layout.sidebar.w - kSidebarInset * 2.0f));
    DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
               layout.sidebar.y + 54.0f,
               overlay_workflow_.project_search.editing &&
                       overlay_workflow_.project_search.edit_field ==
                           ProjectSearchEditField::Replace
                   ? theme_.text_primary
                   : theme_.text_secondary,
               theme_.surface_background,
               TruncateLabel("replace> " + active_replace,
                             layout.sidebar.w - kSidebarInset * 2.0f));
    const auto draw_search_button = [&](const SDL_FRect& rect,
                                        std::string_view label,
                                        bool active) {
      const bool hovered =
          last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
      const SDL_Color background =
          active ? (hovered ? theme_.row_highlight : theme_.chrome_active)
                 : (hovered ? theme_.row_highlight : theme_.surface_raised);
      const SDL_Color border =
          active ? theme_.accent : (hovered ? theme_.text_secondary : theme_.border);
      const SDL_Color text = active || hovered ? theme_.text_primary : theme_.text_secondary;
      DrawFilledRect(renderer, rect, background);
      DrawRect(renderer, rect, border);
      DrawCenteredTextOn(text_renderer_, renderer, rect, text, background, label);
    };

    draw_search_button(ProjectSearchModeButtonRect(layout.sidebar), ProjectSearchModeButtonLabel(),
                       overlay_workflow_.project_search.options.pattern_mode ==
                           project::ProjectSearchPatternMode::Regex);
    draw_search_button(ProjectSearchCaseButtonRect(layout.sidebar), ProjectSearchCaseButtonLabel(),
                       overlay_workflow_.project_search.options.case_mode !=
                           project::ProjectSearchCaseMode::Smart);
    draw_search_button(ProjectSearchHiddenButtonRect(layout.sidebar),
                       ProjectSearchHiddenButtonLabel(),
                       overlay_workflow_.project_search.options.show_hidden);

    const std::string match_actions =
        ProjectSearchCanReplaceAll() ? "/ query  = replace  r rerun  R replace all"
                                     : "/ query  = replace  r rerun  R needs literal mode";
    const std::string status_text =
        overlay_workflow_.project_search.editing
            ? (overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query
                   ? "Editing query  |  Enter apply  Esc cancel"
                   : "Editing replace  |  Enter apply  Esc cancel")
            : !overlay_workflow_.project_search.error.empty()
                ? "Error  |  / query  = replace  r rerun"
            : overlay_workflow_.project_search.running
                ? "Searching " +
                      std::to_string(overlay_workflow_.project_search.results.size()) + " matches"
            : overlay_workflow_.project_search.results.empty()
                ? (overlay_workflow_.project_search.query.empty()
                       ? "/ query  = replace  |  buttons change mode, case, hidden"
                       : "No matches  |  " + match_actions)
            : overlay_workflow_.project_search.truncated
                ? "Showing first " +
                      std::to_string(overlay_workflow_.project_search.results.size()) +
                      " matches  |  " + match_actions
                : std::to_string(overlay_workflow_.project_search.results.size()) +
                      " matches  |  " + match_actions;
    DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
               layout.sidebar.y + kProjectSearchStatusTop, theme_.text_muted,
               theme_.surface_background,
               TruncateLabel(status_text, layout.sidebar.w - kSidebarInset * 2.0f));

    const auto line_map = BuildProjectSearchLineMap();
    const auto list_layout = ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
    int scroll_row = list_layout.scroll_row;
    const int selected_line =
        ProjectSearchLineForResult(overlay_workflow_.project_search.selected_index);
    scroll_row = RevealScrollableListIndex(list_layout, selected_line);
    sidebar_state_.scroll_row = scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int line_index = scroll_row + row;
      if (line_index >= static_cast<int>(line_map.size())) {
        break;
      }

      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const int result_index = line_map[static_cast<std::size_t>(line_index)];
      if (result_index < 0) {
        const std::size_t next_result_index = static_cast<std::size_t>(
            std::min(line_index + 1, static_cast<int>(line_map.size()) - 1));
        const auto& file_result =
            overlay_workflow_.project_search
                .results[static_cast<std::size_t>(line_map[next_result_index])];
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_primary,
                            theme_.surface_background,
                            TruncateLabel(file_result.relative_path.string(), row_rect.w - 8.0f));
        continue;
      }

      const auto& result =
          overlay_workflow_.project_search.results[static_cast<std::size_t>(result_index)];
      const bool selected =
          static_cast<std::size_t>(result_index) ==
          overlay_workflow_.project_search.selected_index;
      if (selected) {
        DrawFilledRect(renderer, row_rect, theme_.row_highlight);
      }

      const std::string snippet = CollapseWhitespace(result.preview);
      const std::string label = std::to_string(result.line + 1) + ":" +
                                std::to_string(result.column + 1) + "  " + snippet;
      DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 6.0f,
                          selected ? theme_.text_primary : theme_.text_secondary,
                          selected ? theme_.row_highlight : theme_.surface_background,
                          TruncateLabel(label, row_rect.w - 12.0f));
    }

    if (line_map.empty()) {
      const std::string placeholder =
          !overlay_workflow_.project_search.error.empty()
              ? "Error: " + overlay_workflow_.project_search.error
          : overlay_workflow_.project_search.running
              ? "Searching..."
          : overlay_workflow_.project_search.query.empty() ? "Project search is idle"
                                                           : "No matches";
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
                 list_layout.row_y + 4.0f, theme_.text_muted, theme_.surface_background,
                 TruncateLabel(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(line_map.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            interaction_state_.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_state_.mode == SidebarMode::Git) {
    draw_action_button(GitSidebarStageAllButtonRect(layout.sidebar), "Stage All",
                       CanStageAllGitSidebarEntries());
    draw_action_button(GitSidebarDiscardAllButtonRect(layout.sidebar), "Discard All",
                       CanDiscardAllGitSidebarEntries(), true);
    draw_action_button(GitSidebarRefreshButtonRect(layout.sidebar), "Refresh", true);

    const auto lines = BuildGitSidebarLines();
    const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    const int scroll_row = list_layout.scroll_row;
    sidebar_state_.scroll_row = scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int line_index = scroll_row + row;
      if (line_index >= static_cast<int>(lines.size())) {
        break;
      }

      const auto& line = lines[static_cast<std::size_t>(line_index)];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);

      if (line.kind == GitSidebarLine::Kind::Header) {
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_muted,
                            theme_.surface_background,
                            TruncateLabel(line.label, row_rect.w - 8.0f));
        continue;
      }
      if (line.kind == GitSidebarLine::Kind::Empty || line.entry_index < 0) {
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_muted,
                            theme_.surface_background,
                            TruncateLabel(line.label, row_rect.w - 8.0f));
        continue;
      }

      const auto& entry = git_sidebar_.entries[static_cast<std::size_t>(line.entry_index)];
      const bool selected =
          static_cast<std::size_t>(line.entry_index) == git_sidebar_.selected_index;
      if (selected) {
        DrawFilledRect(renderer, row_rect, theme_.row_highlight);
        DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                       theme_.accent);
      }

      const char git_marker = GitMarker(entry.status);
      const std::string marker_text = git_marker == ' ' ? "" : std::string(1, git_marker);
      const float marker_width =
          marker_text.empty() ? 0.0f : text_renderer_.MeasureWidth(marker_text);
      const GitSidebarEntryActionLayout actions =
          ComputeGitSidebarEntryActionLayout(row_rect, entry);
      float right_edge = actions.content_right_edge;

      const auto draw_button = [&](const SDL_FRect& button_rect,
                                   std::string_view label,
                                   SDL_Color text_color) {
        DrawRect(renderer, button_rect, selected ? theme_.accent : theme_.border);
        DrawCenteredTextOn(text_renderer_, renderer, button_rect, text_color,
                           selected ? theme_.row_highlight : theme_.surface_background, label);
      };

      if (actions.primary_rect.has_value()) {
        draw_button(*actions.primary_rect, entry.staged ? "U" : "S", theme_.accent);
      }
      if (actions.discard_rect.has_value()) {
        draw_button(*actions.discard_rect, "D", theme_.diff_deleted);
      }

      if (!marker_text.empty()) {
        DrawVCenteredTextOn(
            text_renderer_, renderer,
            MakeRect(right_edge - marker_width, row_rect.y, marker_width, row_rect.h), 0.0f,
            GitMarkerColor(theme_, entry.status),
            selected ? theme_.row_highlight : theme_.surface_background, marker_text);
        right_edge -= marker_width + 8.0f;
      }

      const GitSidebarEntryTextModel text_model =
          BuildGitSidebarEntryTextModel(entry.relative_path, entry.staged);
      const SDL_Color row_background =
          selected ? theme_.row_highlight : theme_.surface_background;
      const SDL_Color primary_color = selected ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = selected ? theme_.text_secondary : theme_.text_muted;
      const float text_x = row_rect.x + 6.0f;
      const float text_y =
          row_rect.y + std::floor(std::max(0.0f, row_rect.h - text_renderer_.LineHeight()) * 0.5f);
      const float label_width = std::max(20.0f, right_edge - text_x);
      const std::string primary_label =
          text_renderer_.TruncateToWidth(text_model.primary_label, label_width);
      DrawTextOn(text_renderer_, renderer, text_x, text_y, primary_color, row_background,
                 primary_label);

      if (!primary_label.empty() && !text_model.secondary_label.empty()) {
        const float secondary_x = text_x + text_renderer_.MeasureWidth(primary_label) + 8.0f;
        const float secondary_width = std::max(0.0f, right_edge - secondary_x);
        if (secondary_width > 24.0f) {
          DrawTextOn(text_renderer_, renderer, secondary_x, text_y, secondary_color, row_background,
                     text_renderer_.TruncateToWidth(text_model.secondary_label, secondary_width));
        }
      }
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(lines.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            interaction_state_.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_state_.mode == SidebarMode::Problems) {
    const auto list_layout =
        ComputeProblemsSidebarListLayout(layout.sidebar, problems_sidebar_.entries.size());
    const int scroll_row = list_layout.scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int item_index = scroll_row + row;
      if (item_index >= static_cast<int>(problems_sidebar_.entries.size())) {
        break;
      }

      const auto& item = problems_sidebar_.entries[static_cast<std::size_t>(item_index)];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected =
          static_cast<std::size_t>(item_index) == problems_sidebar_.selected_index;
      const SDL_Color row_background =
          selected ? theme_.row_highlight : theme_.surface_background;
      if (selected) {
        DrawFilledRect(renderer, row_rect, theme_.row_highlight);
      }

      const SDL_Color severity =
          editor::DiagnosticSeverityColor(theme_, item.diagnostic.severity);
      DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h), severity);

      const float text_x = row_rect.x + 8.0f;
      const float text_y =
          row_rect.y + std::floor(std::max(0.0f, row_rect.h - text_renderer_.LineHeight()) * 0.5f);
      const float max_width = std::max(20.0f, row_rect.w - 14.0f);
      const SDL_Color primary_color = selected ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = selected ? theme_.text_secondary : theme_.text_muted;
      const std::string primary =
          text_renderer_.TruncateToWidth(item.primary_label,
                                         item.detail_label.empty() ? max_width : max_width * 0.58f);
      DrawTextOn(text_renderer_, renderer, text_x, text_y, primary_color, row_background, primary);
      if (!item.detail_label.empty()) {
        const float detail_x = text_x + text_renderer_.MeasureWidth(primary) + 8.0f;
        const float detail_width = std::max(0.0f, row_rect.x + row_rect.w - 6.0f - detail_x);
        if (detail_width > 24.0f) {
          DrawTextOn(text_renderer_, renderer, detail_x, text_y, secondary_color, row_background,
                     text_renderer_.TruncateToWidth(item.detail_label, detail_width));
        }
      }
    }

    if (problems_sidebar_.entries.empty()) {
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
                 list_layout.row_y + 4.0f, theme_.text_muted, theme_.surface_background,
                 TruncateLabel("No diagnostics", layout.sidebar.w - kSidebarInset * 2.0f));
    }

    draw_vertical_scrollbar(list_layout.list_rect,
                            static_cast<float>(problems_sidebar_.entries.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            interaction_state_.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_state_.mode == SidebarMode::Plugin) {
    const auto list_layout =
        ComputePluginSidebarListLayout(layout.sidebar, plugin_sidebar_.items.size());
    const int scroll_row = list_layout.scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int item_index = scroll_row + row;
      if (item_index >= static_cast<int>(plugin_sidebar_.items.size())) {
        break;
      }

      const auto& item = plugin_sidebar_.items[static_cast<std::size_t>(item_index)];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected =
          static_cast<std::size_t>(item_index) == plugin_sidebar_.selected_index;
      const SDL_Color row_background =
          selected ? theme_.row_highlight : theme_.surface_background;
      if (selected) {
        DrawFilledRect(renderer, row_rect, theme_.row_highlight);
        DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                       theme_.accent);
      }

      const float text_x = row_rect.x + 6.0f;
      const float text_y =
          row_rect.y + std::floor(std::max(0.0f, row_rect.h - text_renderer_.LineHeight()) * 0.5f);
      const float max_width = std::max(20.0f, row_rect.w - 12.0f);
      const SDL_Color primary_color = selected ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = selected ? theme_.text_secondary : theme_.text_muted;
      const std::string primary =
          text_renderer_.TruncateToWidth(item.label,
                                         item.detail.empty() ? max_width : max_width * 0.62f);
      DrawTextOn(text_renderer_, renderer, text_x, text_y, primary_color, row_background, primary);
      if (!item.detail.empty()) {
        const float detail_x = text_x + text_renderer_.MeasureWidth(primary) + 8.0f;
        const float detail_width = std::max(0.0f, row_rect.x + row_rect.w - 6.0f - detail_x);
        if (detail_width > 24.0f) {
          DrawTextOn(text_renderer_, renderer, detail_x, text_y, secondary_color, row_background,
                     text_renderer_.TruncateToWidth(item.detail, detail_width));
        }
      }
    }

    const std::string placeholder =
        !plugin_sidebar_.error.empty()
            ? "Error: " + plugin_sidebar_.error
            : plugin_sidebar_.items.empty() ? "No items" : std::string{};
    if (!placeholder.empty()) {
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
                 list_layout.row_y + 4.0f,
                 plugin_sidebar_.error.empty() ? theme_.text_muted : theme_.diff_deleted,
                 theme_.surface_background,
                 TruncateLabel(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(plugin_sidebar_.items.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            interaction_state_.drag_target == DragTarget::SidebarScrollbar);
  } else {
    const SDL_FRect collapse_rect = TreeSidebarCollapseButtonRect(layout.sidebar);
    const SDL_FRect refresh_rect = TreeSidebarRefreshButtonRect(layout.sidebar);
    draw_action_button(collapse_rect, "Collapse", directory_tree_.CanCollapseAll());
    draw_action_button(refresh_rect, "Refresh", true);

    const std::string tree_root_label = ProjectLabel();
    const float root_label_left = sidebar_mode_rect.x + sidebar_mode_rect.w + 10.0f;
    const float root_label_right = collapse_rect.x - 10.0f;
    const float root_label_max_width = std::max(0.0f, root_label_right - root_label_left);
    const std::string root_label = TruncateLabel(tree_root_label, root_label_max_width);
    if (!root_label.empty()) {
      DrawCenteredTextOn(text_renderer_, renderer,
                         MakeRect(root_label_left, layout.sidebar.y + 4.0f, root_label_max_width,
                                  18.0f),
                         theme_.chrome_text_secondary, theme_.chrome_background, root_label);
    }

    const auto& entries = directory_tree_.entries();
    const auto list_layout = ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
    const int scroll_row = list_layout.scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int entry_index = scroll_row + row;
      if (entry_index >= static_cast<int>(entries.size())) {
        break;
      }

      const auto& entry = entries[entry_index];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected =
          static_cast<std::size_t>(entry_index) == directory_tree_.selected_index();
      if (selected) {
        DrawFilledRect(renderer, row_rect, theme_.row_highlight);
        DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                       theme_.accent);
      }

      const float depth_offset = static_cast<float>(entry.depth) * kTreeIndentWidth;
      const float tree_x = row_rect.x + 6.0f + depth_offset;
      const float chevron_x = tree_x;
      const float label_x = tree_x + kTreeChevronSlotWidth + 4.0f;
      const float chevron_center_y = row_rect.y + row_rect.h * 0.5f;
      const char git_marker = GitMarker(entry.git_status);
      const bool has_git_marker = git_marker != ' ';
      const std::string git_marker_text = has_git_marker ? std::string(1, git_marker) : "";
      const float marker_width =
          has_git_marker ? text_renderer_.MeasureWidth(git_marker_text) : 0.0f;
      const float marker_x = row_rect.x + row_rect.w - marker_width - 8.0f;
      const float label_width =
          has_git_marker ? std::max(20.0f, marker_x - label_x - 8.0f)
                         : std::max(20.0f, row_rect.x + row_rect.w - label_x - 8.0f);

      if (entry.is_directory) {
        DrawChevron(renderer, chevron_x, chevron_center_y, entry.expanded,
                    selected ? theme_.text_primary : theme_.text_muted);
      }

      DrawVCenteredTextOn(
          text_renderer_, renderer,
          MakeRect(label_x, row_rect.y, label_width, row_rect.h), 0.0f,
          selected ? theme_.text_primary
                   : (entry.is_directory ? theme_.text_primary : theme_.text_secondary),
          selected ? theme_.row_highlight : theme_.surface_background,
          TruncateLabel(entry.label, label_width));
      if (has_git_marker) {
        DrawVCenteredTextOn(
            text_renderer_, renderer,
            MakeRect(marker_x, row_rect.y, marker_width, row_rect.h), 0.0f,
            selected ? theme_.text_primary : GitMarkerColor(theme_, entry.git_status),
            selected ? theme_.row_highlight : theme_.surface_background, git_marker_text);
      }
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(entries.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            interaction_state_.drag_target == DragTarget::SidebarScrollbar);
  }

  const std::string hovered_git_sidebar_tooltip = HoveredGitSidebarTooltipLabel(layout.sidebar);
  if (!hovered_git_sidebar_tooltip.empty()) {
    const float max_tooltip_width = std::max(120.0f, layout.full.w - 24.0f);
    const std::string tooltip_text =
        text_renderer_.TruncateToWidth(hovered_git_sidebar_tooltip, max_tooltip_width - 16.0f);
    const float tooltip_width =
        std::min(max_tooltip_width, text_renderer_.MeasureWidth(tooltip_text) + 16.0f);
    const float tooltip_height = text_renderer_.LineHeight() + 10.0f;
    const float tooltip_x =
        std::clamp(last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                   layout.full.x + layout.full.w - tooltip_width - 8.0f);
    const float tooltip_y =
        last_mouse_y_ - tooltip_height - 10.0f >= layout.full.y + 8.0f
            ? last_mouse_y_ - tooltip_height - 10.0f
            : std::clamp(last_mouse_y_ + 14.0f, layout.full.y + 8.0f,
                         layout.full.y + layout.full.h - tooltip_height - 8.0f);
    const SDL_FRect tooltip_rect = MakeRect(tooltip_x, tooltip_y, tooltip_width, tooltip_height);
    DrawFilledRect(renderer, tooltip_rect, theme_.surface_raised);
    DrawRect(renderer, tooltip_rect, theme_.border);
    DrawVCenteredTextOn(text_renderer_, renderer, tooltip_rect, 8.0f, theme_.text_primary,
                        theme_.surface_raised, tooltip_text);
  }
}

}  // namespace microide::workspace
