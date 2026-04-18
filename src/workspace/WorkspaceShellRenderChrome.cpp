#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/DiagnosticsRender.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarInset = 10.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;

void DrawScrollbarTrack(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& track) {
  if (renderer == nullptr || track.w <= 0.0f || track.h <= 0.0f) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, theme.surface_raised.r, theme.surface_raised.g,
                         theme.surface_raised.b, theme.surface_raised.a);
  SDL_RenderFillRect(renderer, &track);
}

void DrawScrollbarThumb(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& thumb,
                        bool active = false) {
  if (renderer == nullptr || thumb.w <= 0.0f || thumb.h <= 0.0f) {
    return;
  }

  const SDL_Color thumb_color = active ? theme.accent : theme.text_disabled;
  SDL_SetRenderDrawColor(renderer, thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a);
  SDL_RenderFillRect(renderer, &thumb);
}

void DrawScrollbar(SDL_Renderer* renderer,
                   const render::Theme& theme,
                   const SDL_FRect& track,
                   const SDL_FRect& thumb,
                   bool active = false) {
  DrawScrollbarTrack(renderer, theme, track);
  DrawScrollbarThumb(renderer, theme, thumb, active);
}

char GitMarker(project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Conflicted:
      return '!';
    case project::GitFileStatus::Modified:
      return 'M';
    case project::GitFileStatus::Added:
      return 'A';
    case project::GitFileStatus::Deleted:
      return 'D';
    case project::GitFileStatus::Untracked:
      return 'U';
    case project::GitFileStatus::Clean:
    default:
      return ' ';
  }
}

SDL_Color GitMarkerColor(const render::Theme& theme, project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Conflicted:
      return theme.accent;
    case project::GitFileStatus::Modified:
      return theme.diff_modified;
    case project::GitFileStatus::Added:
      return theme.diff_added;
    case project::GitFileStatus::Deleted:
      return theme.diff_deleted;
    case project::GitFileStatus::Untracked:
      return theme.accent;
    case project::GitFileStatus::Clean:
    default:
      return theme.text_disabled;
  }
}

void DrawChevron(SDL_Renderer* renderer, float x, float center_y, bool expanded, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  if (expanded) {
    SDL_RenderLine(renderer, x, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    SDL_RenderLine(renderer, x + 8.0f, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    return;
  }

  SDL_RenderLine(renderer, x + 2.0f, center_y - 4.0f, x + 6.0f, center_y);
  SDL_RenderLine(renderer, x + 2.0f, center_y + 4.0f, x + 6.0f, center_y);
}

void DrawCloseGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }

  const float center_x = std::floor(rect.x + rect.w * 0.5f);
  const float center_y = std::floor(rect.y + rect.h * 0.5f);
  const auto draw_dot = [&](float x, float y) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const SDL_FRect dot = MakeRect(x, y, 1.0f, 1.0f);
    SDL_RenderFillRect(renderer, &dot);
  };

  for (int offset = -3; offset <= 3; ++offset) {
    draw_dot(center_x + static_cast<float>(offset), center_y + static_cast<float>(offset));
    draw_dot(center_x + static_cast<float>(offset), center_y - static_cast<float>(offset));
  }
}

void DrawWindowControlGlyph(SDL_Renderer* renderer,
                            const SDL_FRect& rect,
                            microide::workspace::WorkspaceShell::WindowControlButtonId id,
                            SDL_Color color,
                            bool expanded) {
  if (renderer == nullptr) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float left = rect.x + 4.0f;
  const float right = rect.x + rect.w - 4.0f;
  const float top = rect.y + 4.0f;
  const float bottom = rect.y + rect.h - 4.0f;
  const float center_y = rect.y + rect.h * 0.5f;

  switch (id) {
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Minimize:
      SDL_RenderLine(renderer, left, center_y + 2.0f, right, center_y + 2.0f);
      return;
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Maximize:
      if (expanded) {
        const SDL_FRect back = SDL_FRect{left + 1.5f, top + 3.0f, rect.w - 9.0f, rect.h - 9.0f};
        const SDL_FRect front = SDL_FRect{left - 1.0f, top + 1.0f, rect.w - 9.0f, rect.h - 9.0f};
        SDL_RenderRect(renderer, &back);
        SDL_RenderRect(renderer, &front);
      } else {
        const SDL_FRect outline = SDL_FRect{left, top + 1.0f, rect.w - 8.0f, rect.h - 8.0f};
        SDL_RenderRect(renderer, &outline);
      }
      return;
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Close:
      SDL_RenderLine(renderer, left, top, right, bottom);
      SDL_RenderLine(renderer, right, top, left, bottom);
      return;
  }
}

void DrawText(const render::TextRenderer& text_renderer,
              SDL_Renderer* renderer,
              float x,
              float y,
              SDL_Color foreground,
              std::string_view text) {
  text_renderer.DrawString(renderer, x, y, foreground, text);
}

void DrawTextOn(const render::TextRenderer& text_renderer,
                SDL_Renderer* renderer,
                float x,
                float y,
                SDL_Color foreground,
                SDL_Color background,
                std::string_view text) {
  text_renderer.DrawStringOn(renderer, x, y, foreground, background, text);
}

void DrawVCenteredTextOn(const render::TextRenderer& text_renderer,
                         SDL_Renderer* renderer,
                         const SDL_FRect& rect,
                         float left_padding,
                         SDL_Color foreground,
                         SDL_Color background,
                         std::string_view text) {
  (void)background;
  const float y =
      rect.y + std::floor(std::max(0.0f, rect.h - text_renderer.LineHeight()) * 0.5f);
  DrawText(text_renderer, renderer, rect.x + left_padding, y, foreground, text);
}

void DrawCenteredTextOn(const render::TextRenderer& text_renderer,
                        SDL_Renderer* renderer,
                        const SDL_FRect& rect,
                        SDL_Color foreground,
                        SDL_Color background,
                        std::string_view text) {
  (void)background;
  const float text_width = text_renderer.MeasureWidth(text);
  const float x = rect.x + std::floor(std::max(0.0f, rect.w - text_width) * 0.5f);
  const float y =
      rect.y + std::floor(std::max(0.0f, rect.h - text_renderer.LineHeight()) * 0.5f);
  DrawText(text_renderer, renderer, x, y, foreground, text);
}

}  // namespace

void WorkspaceShell::RenderWindowChrome(SDL_Renderer* renderer,
                                        const WorkspaceLayout& layout) const {
  const auto draw_tab_close_button = [&](const SDL_FRect& rect,
                                         SDL_Color color,
                                         SDL_Color hover_color) {
    const bool hovered =
        last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
    DrawCloseGlyph(renderer, rect, hovered ? hover_color : color);
  };

  const auto visible_menu_items = ComputeVisibleMenuBarItems(layout.menu_bar);
  const auto window_buttons = ComputeVisibleWindowControlButtons(layout.menu_bar);
  for (const VisibleMenuBarItem& item : visible_menu_items) {
    const MenuSpec* menu = FindMenuSpec(item.id);
    if (menu == nullptr) {
      continue;
    }
    const SDL_Color background = item.active ? theme_.chrome_active : theme_.chrome_background;
    DrawFilledRect(renderer, item.rect, background);
    if (item.active) {
      DrawFilledRect(renderer,
                     MakeRect(item.rect.x, item.rect.y + item.rect.h - 2.0f, item.rect.w, 2.0f),
                     theme_.accent);
    }
    DrawVCenteredTextOn(text_renderer_, renderer, item.rect, 10.0f,
                        item.active ? theme_.chrome_active_text : theme_.chrome_text, background,
                        menu->label);
  }

  if (CurrentWindowChromeState().custom_enabled) {
    const std::string title = "microide";
    const float title_width = text_renderer_.MeasureWidth(title);
    const float left_limit =
        visible_menu_items.empty()
            ? layout.menu_bar.x + 12.0f
            : visible_menu_items.back().rect.x + visible_menu_items.back().rect.w + 16.0f;
    const float right_limit =
        window_buttons.empty() ? layout.menu_bar.x + layout.menu_bar.w - 12.0f
                               : window_buttons.front().rect.x - 16.0f;
    const float title_x =
        std::floor(layout.menu_bar.x + (layout.menu_bar.w - title_width) * 0.5f);
    if (title_x >= left_limit && title_x + title_width <= right_limit) {
      DrawVCenteredTextOn(text_renderer_, renderer,
                          MakeRect(title_x, layout.menu_bar.y, title_width, layout.menu_bar.h),
                          0.0f, theme_.chrome_text_secondary, theme_.chrome_background, title);
    }
  }

  for (const VisibleWindowControlButton& button : window_buttons) {
    SDL_Color background = button.hovered ? theme_.row_highlight : theme_.chrome_background;
    SDL_Color glyph = button.hovered ? theme_.text_primary : theme_.chrome_text_secondary;
    if (button.id == WindowControlButtonId::Close && button.hovered) {
      background = theme_.diff_deleted;
      glyph = theme_.text_primary;
    }

    DrawFilledRect(renderer, button.rect, background);
    DrawWindowControlGlyph(renderer, button.rect, button.id, glyph,
                           CurrentWindowChromeState().Expanded());
  }

  for (const VisibleStripTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
    DrawFilledRect(renderer, tab.rect, tab.active ? theme_.chrome_active : theme_.surface_raised);
    if (tab.active) {
      DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f), theme_.accent);
    }
    DrawVCenteredTextOn(text_renderer_, renderer, tab.rect, 10.0f,
                        tab.active ? theme_.chrome_active_text : theme_.surface_text,
                        tab.active ? theme_.chrome_active : theme_.surface_raised,
                        TruncateLabel(tab.display_title, tab.rect.w - 46.0f));
    draw_tab_close_button(tab.close_rect,
                          tab.active ? theme_.chrome_text_secondary : theme_.text_disabled,
                          tab.active ? theme_.chrome_active_text : theme_.surface_text);
  }

  if (!project_root_.empty() && open_tabs_.empty()) {
    const SDL_FRect placeholder_tab =
        MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 2.0f, 220.0f,
                 std::max(22.0f, layout.tab_strip.h - 2.0f));
    DrawFilledRect(renderer, placeholder_tab, theme_.chrome_active);
    DrawFilledRect(renderer, MakeRect(placeholder_tab.x, placeholder_tab.y, placeholder_tab.w, 2.0f),
                   theme_.accent);
    DrawVCenteredTextOn(text_renderer_, renderer, placeholder_tab, 10.0f,
                        theme_.chrome_active_text, theme_.chrome_active, "welcome");
  } else if (!project_root_.empty()) {
    for (const VisibleStripTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      DrawFilledRect(renderer, tab.rect,
                     tab.active ? theme_.chrome_active : theme_.surface_raised);
      if (tab.active) {
        DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                       theme_.accent);
      }
      DrawVCenteredTextOn(text_renderer_, renderer, tab.rect, 10.0f,
                          tab.active ? theme_.chrome_active_text : theme_.surface_text,
                          tab.active ? theme_.chrome_active : theme_.surface_raised,
                          TruncateLabel(tab.display_title, tab.rect.w - 46.0f));
      draw_tab_close_button(tab.close_rect,
                            tab.active ? theme_.chrome_text_secondary : theme_.text_disabled,
                            tab.active ? theme_.chrome_active_text : theme_.surface_text);
    }
  }

  const std::string hovered_tab_tooltip = HoveredTabTooltipLabel(layout.tab_strip);
  const float breadcrumb_text_x = layout.breadcrumb.x + 12.0f;
  const float breadcrumb_text_width =
      std::max(0.0f, layout.breadcrumb.x + layout.breadcrumb.w - 12.0f - breadcrumb_text_x);
  DrawVCenteredTextOn(
      text_renderer_, renderer,
      MakeRect(breadcrumb_text_x, layout.breadcrumb.y, breadcrumb_text_width, layout.breadcrumb.h),
      0.0f, theme_.chrome_text, theme_.chrome_background,
      TruncateLabel(BreadcrumbLabel(), breadcrumb_text_width));
  if (!hovered_tab_tooltip.empty()) {
    const float max_tooltip_width = std::max(160.0f, layout.full.w - 24.0f);
    const std::string tooltip_text =
        text_renderer_.TruncateToWidth(hovered_tab_tooltip, max_tooltip_width - 16.0f);
    const float tooltip_width =
        std::min(max_tooltip_width, text_renderer_.MeasureWidth(tooltip_text) + 16.0f);
    const float tooltip_height = text_renderer_.LineHeight() + 10.0f;
    const float tooltip_x =
        std::clamp(last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                   layout.full.x + layout.full.w - tooltip_width - 8.0f);
    const SDL_FRect tooltip_rect = MakeRect(tooltip_x, layout.tab_strip.y + layout.tab_strip.h + 6.0f,
                                            tooltip_width, tooltip_height);
    DrawFilledRect(renderer, tooltip_rect, theme_.surface_raised);
    DrawRect(renderer, tooltip_rect, theme_.border);
    DrawVCenteredTextOn(text_renderer_, renderer, tooltip_rect, 8.0f, theme_.text_primary,
                        theme_.surface_raised, tooltip_text);
  }
}

void WorkspaceShell::RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  if (!surface_.sidebar_visible) {
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
      surface_.menu_bar_open && surface_.active_menu_id == MenuId::SidebarMode &&
      surface_.active_menu_anchor_rect.has_value();
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

  if (surface_.sidebar_mode == SidebarMode::Search) {
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
    surface_.sidebar_scroll_row = scroll_row;

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
                            surface_.drag_target == DragTarget::SidebarScrollbar);
  } else if (surface_.sidebar_mode == SidebarMode::Git) {
    draw_action_button(GitSidebarStageAllButtonRect(layout.sidebar), "Stage All",
                       CanStageAllGitSidebarEntries());
    draw_action_button(GitSidebarDiscardAllButtonRect(layout.sidebar), "Discard All",
                       CanDiscardAllGitSidebarEntries(), true);
    draw_action_button(GitSidebarRefreshButtonRect(layout.sidebar), "Refresh", true);

    const auto lines = BuildGitSidebarLines();
    const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    const int scroll_row = list_layout.scroll_row;
    surface_.sidebar_scroll_row = scroll_row;

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
                            surface_.drag_target == DragTarget::SidebarScrollbar);
  } else if (surface_.sidebar_mode == SidebarMode::Problems) {
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
                            surface_.drag_target == DragTarget::SidebarScrollbar);
  } else if (surface_.sidebar_mode == SidebarMode::Plugin) {
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
                            surface_.drag_target == DragTarget::SidebarScrollbar);
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
                            surface_.drag_target == DragTarget::SidebarScrollbar);
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

void WorkspaceShell::RenderBottomPanelSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout,
                                              std::size_t terminal_line_count) {
  if (!BottomPanelVisible()) {
    return;
  }

  util::PerformanceTrace::Scope bottom_panel_scope("WorkspaceShell::RenderBottomPanel");
  const SDL_FRect panel_header =
      MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
               kWorkspaceBottomPanelHeaderHeight);
  const bool terminal_panel = ActiveTerminalTab() != nullptr;

  const auto draw_tab_close_button = [&](const SDL_FRect& rect,
                                         SDL_Color color,
                                         SDL_Color hover_color) {
    const bool hovered =
        last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
    DrawCloseGlyph(renderer, rect, hovered ? hover_color : color);
  };

  const auto resolve_terminal_colors = [&](const terminal::TerminalStyle& style, bool selected) {
    SDL_Color foreground = style.foreground.value_or(theme_.text_muted);
    SDL_Color background = style.background.value_or(theme_.surface_background);
    if (style.inverse) {
      std::swap(foreground, background);
    }
    if (selected) {
      foreground = theme_.text_primary;
      background = theme_.row_highlight;
    }
    return std::pair{foreground, background};
  };
  const auto draw_terminal_line = [&](float x,
                                      float y,
                                      float width,
                                      const terminal::TerminalLine& line,
                                      std::size_t row_index) {
    if (width <= 0.0f || line.cells.empty()) {
      return;
    }

    const float char_width = std::max(1.0f, text_renderer_.CharWidth());
    const std::size_t visible_columns =
        std::min(line.cells.size(), std::max<std::size_t>(
                                      1, static_cast<std::size_t>(std::floor(width / char_width))));
    if (visible_columns == 0) {
      return;
    }

    for (std::size_t column = 0; column < visible_columns;) {
      const auto& cell = line.cells[column];
      const bool selected = TerminalCellSelected(row_index, column);
      const SDL_Color background = resolve_terminal_colors(cell.style, selected).second;

      std::size_t run_end = column + 1;
      while (run_end < visible_columns) {
        const auto& next_cell = line.cells[run_end];
        const bool next_selected = TerminalCellSelected(row_index, run_end);
        const SDL_Color next_background =
            resolve_terminal_colors(next_cell.style, next_selected).second;
        if (next_background.r != background.r || next_background.g != background.g ||
            next_background.b != background.b || next_background.a != background.a) {
          break;
        }
        ++run_end;
      }

      const float run_x = x + static_cast<float>(column) * char_width;
      DrawFilledRect(renderer,
                     MakeRect(run_x, y - 1.0f,
                              static_cast<float>(run_end - column) * char_width,
                              text_renderer_.LineHeight()),
                     background);
      column = run_end;
    }

    for (std::size_t column = 0; column < visible_columns; ++column) {
      const auto& cell = line.cells[column];
      const bool selected = TerminalCellSelected(row_index, column);
      const auto [foreground, background] = resolve_terminal_colors(cell.style, selected);
      (void)background;
      const float cell_x = x + static_cast<float>(column) * char_width;
      const std::string_view display_text = cell.DisplayText();
      if (display_text.empty() || display_text == " ") {
        continue;
      }
      text_renderer_.DrawString(renderer, cell_x, y, foreground, display_text);
    }
  };

  if (terminal_panel) {
    for (const VisibleStripTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
      const auto* terminal_tab =
          tab.index < terminal_tabs_.size() ? terminal_tabs_[tab.index].get() : nullptr;
      if (terminal_tab == nullptr) {
        continue;
      }

      const SDL_Color background = tab.active ? theme_.chrome_active : theme_.surface_raised;
      const SDL_Color foreground =
          tab.active ? theme_.chrome_active_text : theme_.surface_text;
      DrawFilledRect(renderer, tab.rect, background);
      if (tab.active) {
        DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                       theme_.accent);
      }
      DrawVCenteredTextOn(text_renderer_, renderer, tab.rect, 8.0f, foreground, background,
                          TruncateLabel(tab.display_title, tab.rect.w - 40.0f));
      draw_tab_close_button(tab.close_rect, foreground, theme_.chrome_active_text);
    }
    const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
    DrawFilledRect(renderer, new_tab_rect, theme_.surface_raised);
    DrawRect(renderer, new_tab_rect, theme_.border);
    DrawCenteredTextOn(text_renderer_, renderer, new_tab_rect, theme_.surface_text,
                       theme_.surface_raised, "+");
  } else {
    DrawVCenteredTextOn(text_renderer_, renderer, panel_header, 12.0f, theme_.chrome_text,
                        theme_.chrome_background, "Command");
  }

  const BottomPanelLogLayout panel_layout =
      ComputeBottomPanelLogLayout(layout, terminal_panel ? terminal_line_count : 0);
  SetBottomPanelScrollRow(panel_layout.scroll.vertical_scroll,
                          terminal_panel ? terminal_line_count : 0,
                          panel_layout.scroll.visible_rows);
  const std::size_t first_terminal_row =
      static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
  const std::vector<terminal::TerminalLine> terminal_lines =
      terminal_panel && ActiveTerminalTab() != nullptr
          ? ActiveTerminalTab()->session.SnapshotLineRange(
                first_terminal_row,
                static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)))
          : std::vector<terminal::TerminalLine>{};
  for (int row = 0; row < panel_layout.scroll.visible_rows; ++row) {
    const int index = panel_layout.scroll.vertical_scroll + row;
    if (index >= static_cast<int>(terminal_line_count)) {
      break;
    }
    const float line_y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height;
    if (terminal_panel) {
      draw_terminal_line(panel_layout.text_x, line_y, panel_layout.text_width,
                         terminal_lines[static_cast<std::size_t>(index) - first_terminal_row],
                         static_cast<std::size_t>(index));
    }
  }

  if (terminal_panel) {
    if (auto* active_terminal = ActiveTerminalTab();
        active_terminal != nullptr && active_terminal->session.cursor_visible()) {
      const std::size_t cursor_row = active_terminal->session.cursor_row();
      const std::size_t cursor_column = active_terminal->session.cursor_column();
      if (cursor_row >= static_cast<std::size_t>(panel_layout.scroll.vertical_scroll) &&
          cursor_row <
              static_cast<std::size_t>(panel_layout.scroll.vertical_scroll +
                                       panel_layout.scroll.visible_rows) &&
          (surface_.focus != FocusTarget::Panel || CaretVisibleNow())) {
        const float char_width = std::max(1.0f, text_renderer_.CharWidth());
        const float cursor_x = panel_layout.text_x + static_cast<float>(cursor_column) * char_width;
        const float cursor_y =
            panel_layout.text_y +
            static_cast<float>(cursor_row -
                               static_cast<std::size_t>(panel_layout.scroll.vertical_scroll)) *
                panel_layout.line_height;
        if (cursor_x <= panel_layout.content_rect.x + panel_layout.content_rect.w - char_width) {
          DrawFilledRect(renderer,
                         MakeRect(cursor_x, cursor_y - 1.0f, char_width, panel_layout.line_height),
                         theme_.cursor);
          if (cursor_row >= first_terminal_row &&
              cursor_row - first_terminal_row < terminal_lines.size()) {
            const auto& line = terminal_lines[cursor_row - first_terminal_row];
            if (cursor_column < line.cells.size()) {
              const auto& cell = line.cells[cursor_column];
              const auto display_text = cell.DisplayText();
              if (!display_text.empty()) {
                const SDL_Color cursor_foreground =
                    resolve_terminal_colors(cell.style, false).second;
                text_renderer_.DrawString(renderer, cursor_x, cursor_y, cursor_foreground,
                                          std::string(display_text));
              }
            }
          }
        }
      }
    }
  }

  if (surface_.command_mode) {
    const SDL_FRect command_area = BottomPanelCommandAreaRect(layout);
    DrawFilledRect(renderer, command_area, theme_.surface_raised);
    DrawFilledRect(renderer,
                   MakeRect(command_area.x, command_area.y, command_area.w,
                            kWorkspaceDividerThickness),
                   theme_.border);

    const float status_y = command_area.y + kWorkspaceBottomPanelCommandTopPadding;
    DrawTextOn(text_renderer_, renderer, command_area.x + 12.0f, status_y, theme_.text_muted,
               theme_.surface_raised,
               TruncateLabel(CommandPromptCoordinator::PromptStatusText(*this),
                             command_area.w - 24.0f));

    const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
    DrawFilledRect(renderer, prompt_rect, theme_.chrome_active);
    DrawVCenteredTextOn(text_renderer_, renderer, prompt_rect, 6.0f,
                        theme_.chrome_active_text, theme_.chrome_active,
                        "> " + command_.input);
  }

  if (panel_layout.scroll.vertical_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, panel_layout.scroll.vertical_scrollbar->track,
                  panel_layout.scroll.vertical_scrollbar->thumb,
                  surface_.drag_target == DragTarget::BottomPanelScrollbar);
  }
}

void WorkspaceShell::RenderMenuPopups(SDL_Renderer* renderer,
                                      const WorkspaceLayout& layout) const {
  if (surface_.menu_bar_open) {
    const auto draw_popup_menu =
        [&](MenuId menu_id, int active_item_index, const std::optional<SDL_FRect>& anchor_rect) {
          const MenuSpec* menu = FindMenuSpec(menu_id);
          if (menu == nullptr) {
            return;
          }
          const auto items = MenuItems(menu_id);
          const auto popup_rect =
              anchor_rect.has_value() ? ActiveSubmenuRect(layout.menu_bar)
                                      : ComputePopupMenuRect(layout.menu_bar, menu_id);
          if (!popup_rect.has_value()) {
            return;
          }

          DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
          DrawRect(renderer, *popup_rect, theme_.border);
          for (const VisiblePopupMenuItem& item :
               ComputeVisiblePopupMenuItems(items, active_item_index, *popup_rect)) {
            if (item.separator) {
              DrawFilledRect(renderer,
                             MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                      std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                             theme_.border);
              continue;
            }

            const MenuItemSpec& spec = items[item.index];
            const SDL_Color background =
                item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
            const SDL_Color text_color =
                !item.enabled ? theme_.text_disabled
                              : item.hovered ? theme_.text_primary : theme_.text_secondary;
            const SDL_Color accel_color =
                !item.enabled ? theme_.text_disabled : theme_.text_muted;
            DrawFilledRect(renderer, item.rect, background);
            if (item.checked) {
              DrawCenteredTextOn(text_renderer_, renderer,
                                 MakeRect(item.rect.x + 8.0f, item.rect.y, 10.0f, item.rect.h),
                                 item.enabled ? theme_.accent : theme_.text_disabled, background,
                                 "x");
            }
            const std::string accelerator = MenuItemAccelerator(spec);
            const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
            const float label_width = std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
            DrawVCenteredTextOn(text_renderer_, renderer,
                                MakeRect(item.rect.x + 24.0f, item.rect.y, label_width, item.rect.h),
                                0.0f, text_color, background,
                                TruncateLabel(MenuItemLabel(spec), label_width));
            if (!accelerator.empty()) {
              DrawVCenteredTextOn(
                  text_renderer_, renderer,
                  MakeRect(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y,
                           accelerator_width, item.rect.h),
                  0.0f, accel_color, background, accelerator);
            }
          }
        };
    draw_popup_menu(surface_.active_menu_id, surface_.active_menu_item_index, std::nullopt);
    if (surface_.active_submenu_id != MenuId::None) {
      draw_popup_menu(surface_.active_submenu_id, surface_.active_submenu_item_index,
                      surface_.active_submenu_anchor_rect);
    }
  }

  if (!surface_.tree_context_menu.open) {
    return;
  }

  const auto items = TreeContextMenuItems(surface_.tree_context_menu.target);
  const auto popup_rect = ComputeTreeContextMenuRect();
  if (items.empty() || !popup_rect.has_value()) {
    return;
  }

  DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
  DrawRect(renderer, *popup_rect, theme_.border);
  for (const VisiblePopupMenuItem& item : ComputeVisiblePopupMenuItems(
           items, surface_.tree_context_menu.active_item_index, *popup_rect)) {
    if (item.separator) {
      DrawFilledRect(renderer,
                     MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                              std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                     theme_.border);
      continue;
    }

    const MenuItemSpec& spec = items[item.index];
    const SDL_Color background =
        item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
    const SDL_Color text_color =
        !item.enabled ? theme_.text_disabled
                      : item.hovered ? theme_.text_primary : theme_.text_secondary;
    const SDL_Color accel_color = !item.enabled ? theme_.text_disabled : theme_.text_muted;
    DrawFilledRect(renderer, item.rect, background);
    if (item.checked) {
      DrawCenteredTextOn(text_renderer_, renderer,
                         MakeRect(item.rect.x + 8.0f, item.rect.y, 10.0f, item.rect.h),
                         item.enabled ? theme_.accent : theme_.text_disabled, background, "x");
    }
    const std::string accelerator = MenuItemAccelerator(spec);
    const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
    const float label_width = std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
    DrawVCenteredTextOn(text_renderer_, renderer,
                        MakeRect(item.rect.x + 24.0f, item.rect.y, label_width, item.rect.h), 0.0f,
                        text_color, background, TruncateLabel(MenuItemLabel(spec), label_width));
    if (!accelerator.empty()) {
      DrawVCenteredTextOn(
          text_renderer_, renderer,
          MakeRect(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y,
                   accelerator_width, item.rect.h),
          0.0f, accel_color, background, accelerator);
    }
  }
}

void WorkspaceShell::RenderPromptSurface(
    SDL_Renderer* renderer,
    const WorkspaceLayout& layout,
    const std::optional<TextInputVisual>& active_text_input_visual) const {
  if (!prompts_.surface_visible) {
    return;
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

  const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
  const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
  const SDL_FRect message_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 36.0f);
  DrawFilledRect(renderer, dialog, theme_.overlay_background);
  DrawRect(renderer, dialog, theme_.border);
  DrawFilledRect(renderer, header, theme_.chrome_background);
  DrawFilledRect(renderer, MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                 theme_.border);
  DrawVCenteredTextOn(text_renderer_, renderer, header, 16.0f, theme_.chrome_text,
                      theme_.chrome_background, PromptSurfaceTitle());
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y, theme_.text_muted,
             theme_.overlay_background, TruncateLabel(PromptSurfaceMessage(), message_rect.w));

  if (prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
    const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
    DrawFilledRect(renderer, input_rect, theme_.surface_background);
    DrawRect(renderer, input_rect, theme_.border);
    DrawVCenteredTextOn(text_renderer_, renderer, input_rect, 6.0f, theme_.surface_text,
                        theme_.surface_background,
                        TruncateLabel(prompts_.surface.input, input_rect.w - 12.0f));
    if (surface_.window_has_input_focus && text_composition_.text.empty() &&
        active_text_input_visual.has_value() &&
        active_text_input_visual->surface == TextInputSurface::PromptInput) {
      DrawFilledRect(renderer,
                     MakeRect(active_text_input_visual->cursor_x,
                              active_text_input_visual->text_y - 1.0f, 1.5f,
                              text_renderer_.LineHeight()),
                     theme_.cursor);
    }
  }

  const auto buttons = ComputePromptSurfaceButtonRects(dialog);
  const auto labels = PromptSurfaceActionLabels();
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    const bool selected = prompts_.surface.selected_button == static_cast<int>(i);
    const SDL_Color background = selected ? theme_.chrome_active : theme_.surface_raised;
    DrawFilledRect(renderer, buttons[i], background);
    DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
    DrawCenteredTextOn(text_renderer_, renderer, buttons[i],
                       selected ? theme_.chrome_active_text : theme_.surface_text, background,
                       labels[i]);
  }
}

void WorkspaceShell::RenderDirtyPromptSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout) const {
  if (!prompts_.dirty_visible) {
    return;
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

  const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
  const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
  const SDL_FRect message_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 54.0f);
  DrawFilledRect(renderer, dialog, theme_.overlay_background);
  DrawRect(renderer, dialog, theme_.border);
  DrawFilledRect(renderer, header, theme_.chrome_background);
  DrawFilledRect(renderer, MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                 theme_.border);

  DrawVCenteredTextOn(text_renderer_, renderer, header, 12.0f, theme_.chrome_text,
                      theme_.chrome_background, DirtyPromptTitle());
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y, theme_.text_secondary,
             theme_.overlay_background, TruncateLabel(DirtyPromptMessage(), message_rect.w));
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y + 22.0f, theme_.text_muted,
             theme_.overlay_background, "Enter confirm  Left/Right choose  Esc cancel");

  const auto buttons = ComputeDirtyPromptButtonRects(dialog);
  const auto labels = DirtyPromptActionLabels();
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    const bool selected = prompts_.dirty.selected_action == static_cast<int>(i);
    DrawFilledRect(renderer, buttons[i],
                   selected ? theme_.chrome_active : theme_.surface_raised);
    DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
    DrawCenteredTextOn(text_renderer_, renderer, buttons[i],
                       selected ? theme_.chrome_active_text : theme_.surface_text,
                       selected ? theme_.chrome_active : theme_.surface_raised, labels[i]);
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

}  // namespace microide::workspace
