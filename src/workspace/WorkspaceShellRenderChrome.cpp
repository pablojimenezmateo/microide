#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>
#include <vector>

#include "render/SurfacePrimitives.h"

namespace microide::workspace {

using namespace detail;

namespace {

// Paints a chevron-glyph button with an optional hidden-tab count badge,
// matching the look of the workspace tab strip chrome. Used for the
// click-to-scroll overflow indicators at the strip ends.
void DrawTabStripOverflowButton(const render::TextRenderer& text_renderer,
                                SDL_Renderer* renderer,
                                const render::Theme& theme,
                                const SDL_FRect& rect,
                                bool point_right,
                                std::size_t hidden_count,
                                bool hovered) {
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  const SDL_Color background = hovered ? theme.row_highlight : theme.surface_raised;
  const SDL_Color foreground = hovered ? theme.text_primary : theme.text_secondary;
  render::FillRect(renderer, rect, background);
  render::OutlineRect(renderer, rect, theme.border);

  const float cx = rect.x + 9.0f;
  const float cy = rect.y + rect.h * 0.5f;
  const float arm = std::max(3.0f, rect.h * 0.22f);
  SDL_SetRenderDrawColor(renderer, foreground.r, foreground.g, foreground.b, foreground.a);
  if (point_right) {
    SDL_RenderLine(renderer, cx - arm * 0.5f, cy - arm, cx + arm * 0.5f, cy);
    SDL_RenderLine(renderer, cx + arm * 0.5f, cy, cx - arm * 0.5f, cy + arm);
  } else {
    SDL_RenderLine(renderer, cx + arm * 0.5f, cy - arm, cx - arm * 0.5f, cy);
    SDL_RenderLine(renderer, cx - arm * 0.5f, cy, cx + arm * 0.5f, cy + arm);
  }

  if (hidden_count > 0) {
    std::array<char, 24> count_buffer{};
    const auto conv = std::to_chars(count_buffer.data(),
                                    count_buffer.data() + count_buffer.size(), hidden_count);
    const std::string_view count_text(count_buffer.data(),
                                      static_cast<std::size_t>(conv.ptr - count_buffer.data()));
    const float count_x = rect.x + 15.0f;
    const float count_right_padding = 2.0f;
    const SDL_FRect count_rect{
        count_x, rect.y,
        std::max(0.0f, rect.x + rect.w - count_x - count_right_padding), rect.h};
    DrawVCenteredTextOn(text_renderer, renderer, count_rect, 0.0f, foreground, background,
                        count_text);
  }
}

}  // namespace

std::string WorkspaceShell::HoveredProjectTabTooltipLabel(const SDL_FRect& project_tab_strip) const {
  if (!last_mouse_position_valid_ || MenuSurfaceCapturingMouse() ||
      !Contains(project_tab_strip, last_mouse_x_, last_mouse_y_)) {
    return {};
  }

  const auto visible_project_tabs = tab_strip_chrome_.ComputeVisibleProjectTabs(project_tab_strip);
  const auto hovered_project_tab = std::find_if(
      visible_project_tabs.begin(), visible_project_tabs.end(),
      [this](const VisibleStripTab& tab) { return Contains(tab.rect, last_mouse_x_, last_mouse_y_); });
  if (hovered_project_tab == visible_project_tabs.end()) {
    return {};
  }
  return hovered_project_tab->tooltip_label;
}

void WorkspaceShell::RenderWindowChrome(SDL_Renderer* renderer,
                                        const WorkspaceLayout& layout) const {
  const auto draw_tab_close_button = [&](const SDL_FRect& rect, SDL_Color color,
                                         SDL_Color hover_color) {
    DrawHoverableCloseGlyph(renderer, rect,
                            last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_),
                            color, hover_color);
  };
  const StripTabPalette chrome_tab_palette{
      .active_fill = theme_.chrome_active,
      .inactive_fill = theme_.surface_raised,
      .active_text = theme_.chrome_active_text,
      .inactive_text = theme_.surface_text,
      .active_glyph = theme_.chrome_text_secondary,
      .inactive_glyph = theme_.text_disabled,
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

  if (const auto chevron = MenuOverflowChevronRect(layout.menu_bar); chevron.has_value()) {
    const bool hovered = last_mouse_position_valid_ &&
                         Contains(*chevron, last_mouse_x_, last_mouse_y_);
    const SDL_Color background = hovered ? theme_.row_highlight : theme_.chrome_background;
    const SDL_Color glyph = hovered ? theme_.text_primary : theme_.chrome_text_secondary;
    DrawFilledRect(renderer, *chevron, background);
    const float cx = std::floor(chevron->x + chevron->w * 0.5f);
    const float cy = std::floor(chevron->y + chevron->h * 0.5f);
    for (int i = -1; i <= 1; ++i) {
      DrawFilledRect(renderer,
                     MakeRect(cx - 1.0f, cy + static_cast<float>(i) * 5.0f - 1.0f, 2.0f, 2.0f),
                     glyph);
    }
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

  const auto visible_project_tabs = tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
  for (const VisibleStripTab& tab : visible_project_tabs) {
    DrawStripTab(text_renderer_, renderer, theme_, tab.rect, tab.display_title, tab.badge_text,
                 tab.badge_color, tab.show_badge, tab.active,
                 StripTabStyle{
                     .text_left_padding = 10.0f,
                     .badge_size = 16.0f,
                     .badge_gap = 8.0f,
                     .close_right_reserve = 46.0f,
                     .accent_edge = StripAccentEdge::Top,
                 },
                 chrome_tab_palette);
    draw_tab_close_button(tab.close_rect,
                          tab.active ? chrome_tab_palette.active_glyph
                                     : chrome_tab_palette.inactive_glyph,
                          tab.active ? chrome_tab_palette.active_text
                                     : chrome_tab_palette.inactive_text);
  }
  {
    const auto project_overflow =
        tab_strip_chrome_.ComputeProjectTabOverflowControls(layout.project_tab_strip, visible_project_tabs);
    DrawTabStripOverflowButton(text_renderer_, renderer, theme_, project_overflow.left_button,
                               /*point_right=*/false, project_overflow.hidden_left,
                               last_mouse_position_valid_ &&
                                   Contains(project_overflow.left_button, last_mouse_x_,
                                            last_mouse_y_));
    DrawTabStripOverflowButton(text_renderer_, renderer, theme_, project_overflow.right_button,
                               /*point_right=*/true, project_overflow.hidden_right,
                               last_mouse_position_valid_ &&
                                   Contains(project_overflow.right_button, last_mouse_x_,
                                            last_mouse_y_));
  }

  std::vector<VisibleStripTab> visible_tabs;
  if (HasActiveProjectCatalogEntry()) {
    visible_tabs = tab_strip_chrome_.ComputeVisibleTabs(layout.tab_strip);
  }
  if (HasActiveProjectCatalogEntry() && visible_tabs.empty()) {
    const SDL_FRect placeholder_tab = EmptyTabStripPlaceholderRect(layout.tab_strip);
    DrawStripTab(text_renderer_, renderer, theme_, placeholder_tab, "Welcome", {}, {}, false, true,
                 StripTabStyle{
                     .text_left_padding = 10.0f,
                     .close_right_reserve = 0.0f,
                     .accent_edge = StripAccentEdge::Top,
                 },
                 chrome_tab_palette);
  } else if (HasActiveProjectCatalogEntry()) {
    for (const VisibleStripTab& tab : visible_tabs) {
      DrawStripTab(text_renderer_, renderer, theme_, tab.rect, tab.display_title, tab.badge_text,
                   tab.badge_color, tab.show_badge, tab.active,
                   StripTabStyle{
                       .text_left_padding = 10.0f,
                       .close_right_reserve = 46.0f,
                       .accent_edge = StripAccentEdge::Top,
                   },
                   chrome_tab_palette);
      draw_tab_close_button(tab.close_rect,
                            tab.active ? chrome_tab_palette.active_glyph
                                       : chrome_tab_palette.inactive_glyph,
                            tab.active ? chrome_tab_palette.active_text
                                       : chrome_tab_palette.inactive_text);
    }
    const auto tab_overflow = tab_strip_chrome_.ComputeTabOverflowControls(layout.tab_strip, visible_tabs);
    DrawTabStripOverflowButton(text_renderer_, renderer, theme_, tab_overflow.left_button,
                               /*point_right=*/false, tab_overflow.hidden_left,
                               last_mouse_position_valid_ &&
                                   Contains(tab_overflow.left_button, last_mouse_x_,
                                            last_mouse_y_));
    DrawTabStripOverflowButton(text_renderer_, renderer, theme_, tab_overflow.right_button,
                               /*point_right=*/true, tab_overflow.hidden_right,
                               last_mouse_position_valid_ &&
                                   Contains(tab_overflow.right_button, last_mouse_x_,
                                            last_mouse_y_));
  }

  const auto status_items = ComputeVisibleStatusItems(layout.breadcrumb);
  float breadcrumb_text_x = layout.breadcrumb.x + 12.0f;
  float breadcrumb_text_right = layout.breadcrumb.x + layout.breadcrumb.w - 12.0f;
  for (const VisibleStatusItem& item : status_items) {
    const SDL_Color background = item.hovered ? theme_.row_highlight : theme_.chrome_background;
    const SDL_Color text_color = item.hovered ? theme_.text_primary : theme_.text_muted;
    if (item.hovered) {
      DrawSelectableRowBackground(renderer, theme_, item.rect, theme_.chrome_background, true);
    }
    DrawVCenteredTextOn(text_renderer_, renderer, item.rect, 8.0f, text_color, background,
                        item.item.text);
    if (item.item.alignment == StatusAlignment::Left) {
      breadcrumb_text_x = std::max(breadcrumb_text_x, item.rect.x + item.rect.w + 8.0f);
    } else {
      breadcrumb_text_right = std::min(breadcrumb_text_right, item.rect.x - 8.0f);
    }
  }
  const float breadcrumb_text_width =
      std::max(0.0f, breadcrumb_text_right - breadcrumb_text_x);
  DrawVCenteredTextOn(
      text_renderer_, renderer,
      MakeRect(breadcrumb_text_x, layout.breadcrumb.y, breadcrumb_text_width, layout.breadcrumb.h),
      0.0f, theme_.chrome_text, theme_.chrome_background,
      TruncateLabel(BreadcrumbLabel(), breadcrumb_text_width));
}

std::optional<SDL_FRect> WorkspaceShell::HoveredProjectTabTooltipRect(
    const WorkspaceLayout& layout) const {
  const std::string tooltip_label = HoveredProjectTabTooltipLabel(layout.project_tab_strip);
  if (tooltip_label.empty()) {
    return std::nullopt;
  }

  const auto visible_project_tabs = tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
  const auto hovered_project_tab = std::find_if(
      visible_project_tabs.begin(), visible_project_tabs.end(),
      [this](const VisibleStripTab& tab) { return Contains(tab.rect, last_mouse_x_, last_mouse_y_); });
  if (hovered_project_tab == visible_project_tabs.end()) {
    return std::nullopt;
  }

  const auto tooltip = BuildTooltipLayout(
      text_renderer_, tooltip_label, std::max(160.0f, layout.full.w - 24.0f));
  const float center_x = hovered_project_tab->rect.x + hovered_project_tab->rect.w * 0.5f;
  const float tooltip_x =
      std::clamp(center_x - tooltip.rect.w * 0.5f, layout.full.x + 8.0f,
                 layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
  return MakeRect(tooltip_x, hovered_project_tab->rect.y + hovered_project_tab->rect.h + 6.0f,
                  tooltip.rect.w, tooltip.rect.h);
}

std::optional<SDL_FRect> WorkspaceShell::HoveredTabTooltipRect(const WorkspaceLayout& layout) const {
  if (!last_mouse_position_valid_ || MenuSurfaceCapturingMouse()) {
    return std::nullopt;
  }
  if (!Contains(layout.tab_strip, last_mouse_x_, last_mouse_y_)) {
    return std::nullopt;
  }

  const auto visible_tabs = tab_strip_chrome_.ComputeVisibleTabs(layout.tab_strip);
  const auto hovered_tab = std::find_if(
      visible_tabs.begin(), visible_tabs.end(),
      [this](const VisibleStripTab& tab) {
        return Contains(tab.rect, last_mouse_x_, last_mouse_y_);
      });
  if (hovered_tab == visible_tabs.end() || hovered_tab->tooltip_label.empty()) {
    return std::nullopt;
  }

  const auto tooltip =
      BuildTooltipLayout(text_renderer_, hovered_tab->tooltip_label, std::max(160.0f, layout.full.w - 24.0f));
  const float hovered_tab_center_x = hovered_tab->rect.x + hovered_tab->rect.w * 0.5f;
  const float tooltip_x =
      std::clamp(hovered_tab_center_x - tooltip.rect.w * 0.5f, layout.full.x + 8.0f,
                 layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
  return MakeRect(tooltip_x, hovered_tab->rect.y + hovered_tab->rect.h + 6.0f, tooltip.rect.w,
                  tooltip.rect.h);
}

std::optional<SDL_FRect> WorkspaceShell::HoveredStatusTooltipRect(const WorkspaceLayout& layout) const {
  if (!last_mouse_position_valid_) {
    return std::nullopt;
  }
  if (!Contains(layout.breadcrumb, last_mouse_x_, last_mouse_y_)) {
    return std::nullopt;
  }
  const std::string status_tooltip = HoveredStatusTooltip(layout.breadcrumb);
  if (status_tooltip.empty()) {
    return std::nullopt;
  }

  const auto tooltip =
      BuildTooltipLayout(text_renderer_, status_tooltip, std::max(160.0f, layout.full.w - 24.0f));
  const float tooltip_x =
      std::clamp(last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                 layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
  return MakeRect(tooltip_x, layout.breadcrumb.y + layout.breadcrumb.h + 6.0f, tooltip.rect.w,
                  tooltip.rect.h);
}

void WorkspaceShell::RenderChromeTooltips(SDL_Renderer* renderer,
                                          const WorkspaceLayout& layout) const {
  // Chrome tooltips never fire when a menu surface owns the mouse; otherwise
  // empty tooltip cards can paint beneath the popup (the rect probes return
  // a rect while the gated label probes return empty text).
  if (MenuSurfaceCapturingMouse()) {
    return;
  }
  if (const auto tooltip_rect = HoveredProjectTabTooltipRect(layout); tooltip_rect.has_value()) {
    const std::string tooltip_label = HoveredProjectTabTooltipLabel(layout.project_tab_strip);
    DrawTooltip(text_renderer_, renderer, theme_, *tooltip_rect,
                BuildTooltipLayout(text_renderer_, tooltip_label, tooltip_rect->w).text);
  }

  if (const auto tooltip_rect = HoveredTabTooltipRect(layout); tooltip_rect.has_value()) {
    const std::string tooltip_label = HoveredTabTooltipLabel(layout.tab_strip);
    DrawTooltip(text_renderer_, renderer, theme_, *tooltip_rect,
                BuildTooltipLayout(text_renderer_, tooltip_label, tooltip_rect->w).text);
  }

  if (const auto tooltip_rect = HoveredStatusTooltipRect(layout); tooltip_rect.has_value()) {
    const std::string tooltip_label = HoveredStatusTooltip(layout.breadcrumb);
    DrawTooltip(text_renderer_, renderer, theme_, *tooltip_rect,
                BuildTooltipLayout(text_renderer_, tooltip_label, tooltip_rect->w).text);
  }
}

}  // namespace microide::workspace
