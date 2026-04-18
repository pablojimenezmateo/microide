#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace microide::workspace {

using namespace detail;

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

}  // namespace microide::workspace
