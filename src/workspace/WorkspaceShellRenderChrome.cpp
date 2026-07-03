#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>
#include <vector>

#include "editor/GutterIconRegistry.h"
#include "render/SurfacePrimitives.h"

namespace microide::workspace {

using namespace detail;

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
      .hover_fill = theme_.row_highlight,
      .active_text = theme_.chrome_active_text,
      .inactive_text = theme_.surface_text,
      .active_glyph = theme_.chrome_text_secondary,
      .inactive_glyph = theme_.text_disabled,
  };
  const auto tab_hovered = [&](const SDL_FRect& rect) {
    return last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
  };
  // Chrome-like reorder: neighbor tabs render offset by the live slide animation,
  // and the dragged tab is lifted out of the flow (drawn as the floating ghost by
  // DrawTabDragFeedback) during an active drag. During the post-release settle
  // there is no active drag, so the dropped tab renders in-flow and glides home.
  const TabSlideState& tab_slide = context_.interaction_state.tab_slide;
  const TabDragState& tab_drag = context_.interaction_state.tab_drag;
  const auto slide_dx = [&](TabDragKind kind, std::size_t group_index, std::size_t index) -> float {
    if (tab_slide.kind != kind) {
      return 0.0f;
    }
    if (kind == TabDragKind::Editor && tab_slide.group_index != group_index) {
      return 0.0f;
    }
    return index < tab_slide.current.size() ? tab_slide.current[index] : 0.0f;
  };
  const auto tab_lifted = [&](TabDragKind kind, std::size_t group_index, std::size_t index) -> bool {
    if (!tab_drag.dragging || tab_drag.kind != kind) {
      return false;
    }
    if (kind == TabDragKind::Editor && FocusedEditorGroupIndex() != group_index) {
      return false;
    }
    return index == tab_drag.source_index;
  };

  const auto visible_menu_items = ComputeVisibleMenuBarItems(layout.menu_bar);
  const auto window_buttons = ComputeVisibleWindowControlButtons(layout.menu_bar);
  for (const VisibleMenuBarItem& item : visible_menu_items) {
    const MenuSpec* menu = FindMenuSpec(item.id);
    if (menu == nullptr) {
      continue;
    }
    const bool hovered = !item.active && last_mouse_position_valid_ &&
                         Contains(item.rect, last_mouse_x_, last_mouse_y_);
    const SDL_Color background = item.active ? theme_.chrome_active
                                : hovered    ? theme_.row_highlight
                                             : theme_.chrome_background;
    DrawFilledRect(renderer, item.rect, background);
    if (item.active) {
      DrawFilledRect(renderer,
                     MakeRect(item.rect.x, item.rect.y + item.rect.h - 2.0f, item.rect.w, 2.0f),
                     theme_.accent);
    }
    DrawVCenteredTextOn(text_renderer_, renderer, item.rect, 10.0f,
                        item.active || hovered ? theme_.chrome_active_text : theme_.chrome_text,
                        background, menu->label);
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

  // Skip all project-tab work when the strip is hidden (zero height): no tab measuring,
  // overflow controls, or drag ghost.
  if (layout.project_tab_strip.h > 0.0f) {
    const auto visible_project_tabs =
        tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
    for (const VisibleStripTab& tab : visible_project_tabs) {
      if (tab_lifted(TabDragKind::Project, 0, tab.index)) {
        continue;  // rendered as the floating ghost below
      }
      const float dx = slide_dx(TabDragKind::Project, 0, tab.index);
      SDL_FRect rect = tab.rect;
      SDL_FRect close_rect = tab.close_rect;
      rect.x += dx;
      close_rect.x += dx;
      DrawStripTab(text_renderer_, renderer, theme_, rect, tab.display_title, tab.badge_text,
                   tab.badge_color, tab.show_badge, tab.active,
                   StripTabStyle{
                       .text_left_padding = 10.0f,
                       .badge_size = 16.0f,
                       .badge_gap = 8.0f,
                       .close_right_reserve = 46.0f,
                       .accent_edge = StripAccentEdge::Top,
                   },
                   chrome_tab_palette, tab_hovered(rect));
      draw_tab_close_button(close_rect,
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
    if (const TabDragState& drag = context_.interaction_state.tab_drag;
        drag.dragging && drag.kind == TabDragKind::Project) {
      DrawTabDragFeedback(text_renderer_, renderer, theme_, layout.project_tab_strip,
                          visible_project_tabs, drag.source_index, drag.pointer_x,
                          drag.grab_offset_x,
                          StripTabStyle{
                              .text_left_padding = 10.0f,
                              .badge_size = 16.0f,
                              .badge_gap = 8.0f,
                              .close_right_reserve = 46.0f,
                              .accent_edge = StripAccentEdge::Top,
                          },
                          chrome_tab_palette);
    }
  }  // project_tab_strip visible

  // Each editor group owns its own tab strip. For a single group this is the
  // global tab-strip band (filled in the frame pass); a stacked second group
  // synthesizes its strip inside the editor surface, filled here.
  const EditorGroupRectsLayout editor_group_rects = ComputeEditorGroupRectsForState(layout);
  const std::size_t focused_group_index = FocusedEditorGroupIndex();
  for (std::size_t gi = 0; gi < editor_group_rects.groups.size(); ++gi) {
    const SDL_FRect group_tab_strip = editor_group_rects.groups[gi].tab_strip;
    if (editor_group_rects.groups.size() > 1) {
      DrawFilledRect(renderer, group_tab_strip, theme_.chrome_background);
      DrawFilledRect(renderer,
                     MakeRect(group_tab_strip.x,
                              group_tab_strip.y + group_tab_strip.h - kWorkspaceDividerThickness,
                              group_tab_strip.w, kWorkspaceDividerThickness),
                     theme_.border);
    }
    std::vector<VisibleStripTab> visible_tabs;
    if (HasActiveProjectCatalogEntry()) {
      visible_tabs = tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, group_tab_strip);
    }
    if (HasActiveProjectCatalogEntry() && visible_tabs.empty()) {
      const SDL_FRect placeholder_tab = EmptyTabStripPlaceholderRect(group_tab_strip);
      DrawStripTab(text_renderer_, renderer, theme_, placeholder_tab, "Welcome", {}, {}, false, true,
                   StripTabStyle{
                       .text_left_padding = 10.0f,
                       .close_right_reserve = 0.0f,
                       .accent_edge = StripAccentEdge::Top,
                   },
                   chrome_tab_palette);
    } else if (HasActiveProjectCatalogEntry()) {
      for (const VisibleStripTab& tab : visible_tabs) {
        if (tab_lifted(TabDragKind::Editor, gi, tab.index)) {
          continue;  // rendered as the floating ghost below
        }
        const float dx = slide_dx(TabDragKind::Editor, gi, tab.index);
        SDL_FRect rect = tab.rect;
        SDL_FRect close_rect = tab.close_rect;
        rect.x += dx;
        close_rect.x += dx;
        DrawStripTab(text_renderer_, renderer, theme_, rect, tab.display_title, tab.badge_text,
                     tab.badge_color, tab.show_badge, tab.active,
                     StripTabStyle{
                         .text_left_padding = 10.0f,
                         .close_right_reserve = 46.0f,
                         .accent_edge = StripAccentEdge::Top,
                     },
                     chrome_tab_palette, tab_hovered(rect));
        draw_tab_close_button(close_rect,
                              tab.active ? chrome_tab_palette.active_glyph
                                         : chrome_tab_palette.inactive_glyph,
                              tab.active ? chrome_tab_palette.active_text
                                         : chrome_tab_palette.inactive_text);
      }
      const auto tab_overflow =
          tab_strip_chrome_.ComputeTabOverflowControlsForGroup(gi, group_tab_strip, visible_tabs);
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
      if (const TabDragState& drag = context_.interaction_state.tab_drag;
          drag.dragging && drag.kind == TabDragKind::Editor && gi == focused_group_index) {
        DrawTabDragFeedback(text_renderer_, renderer, theme_, group_tab_strip, visible_tabs,
                            drag.source_index, drag.pointer_x, drag.grab_offset_x,
                            StripTabStyle{
                                .text_left_padding = 10.0f,
                                .close_right_reserve = 46.0f,
                                .accent_edge = StripAccentEdge::Top,
                            },
                            chrome_tab_palette);
      }
    }
  }

  const auto status_items = ComputeVisibleStatusItems(layout.breadcrumb);
  float breadcrumb_text_x = layout.breadcrumb.x + 12.0f;
  float breadcrumb_text_right = layout.breadcrumb.x + layout.breadcrumb.w - 12.0f;
  for (const VisibleStatusItem& item : status_items) {
    // Tone tints the item background even when not hovered (so a warning/error
    // status reads at a glance); hover still lifts it to row_highlight.
    SDL_Color tone_fill = theme_.chrome_background;
    switch (item.item.tone) {
      case StatusItemTone::Error:
        tone_fill = theme_.diagnostic_error;
        break;
      case StatusItemTone::Warning:
        tone_fill = theme_.diagnostic_warning;
        break;
      case StatusItemTone::Info:
        tone_fill = theme_.diagnostic_info;
        break;
      case StatusItemTone::Default:
        break;
    }
    const bool has_tone = item.item.tone != StatusItemTone::Default;
    // Only command-bound items are clickable, so only they earn the hover lift.
    // A bare path/status segment must not advertise clickability it doesn't have
    // (this also keeps the breadcrumb's hover/cursor/click regions in agreement).
    const bool hovered = item.hovered && !item.item.command.empty();
    const SDL_Color text_color = hovered ? theme_.text_primary : theme_.text_muted;
    if (hovered) {
      DrawSelectableRowBackground(renderer, theme_, item.rect, theme_.chrome_background, true);
    } else if (has_tone) {
      const SDL_Color tinted = render::BlendColors(theme_.chrome_background, tone_fill, 0.28f);
      SDL_SetRenderDrawColor(renderer, tinted.r, tinted.g, tinted.b, 0xff);
      SDL_RenderFillRect(renderer, &item.rect);
    }
    const SDL_Color row_bg = hovered ? theme_.row_highlight
                                          : (has_tone ? render::BlendColors(theme_.chrome_background,
                                                                    tone_fill, 0.28f)
                                                      : theme_.chrome_background);

    float text_x = item.rect.x + 8.0f;
    // Leading icon, if the contributed name resolves to a built-in shape.
    if (!item.item.icon.empty()) {
      if (const auto shape = editor::GutterIconRegistry::ResolveShape(item.item.icon)) {
        const SDL_Color icon_color = has_tone ? tone_fill : theme_.accent;
        editor::GutterIconRegistry::Draw(renderer, *shape, icon_color, text_x, item.rect.y, 14.0f,
                                         item.rect.h);
        text_x += 16.0f;
      }
    }
    // Trailing progress bar, if the item declares one.
    float text_right = item.rect.x + item.rect.w - 8.0f;
    if (item.item.progress >= 0.0f) {
      constexpr float kBarW = 28.0f;
      const float bar_x = item.rect.x + item.rect.w - kBarW - 6.0f;
      const float bar_h = 4.0f;
      const float bar_y = item.rect.y + (item.rect.h - bar_h) * 0.5f;
      const SDL_FRect track{bar_x, bar_y, kBarW, bar_h};
      const SDL_Color track_color = render::BlendColors(row_bg, theme_.border, 0.6f);
      SDL_SetRenderDrawColor(renderer, track_color.r, track_color.g, track_color.b, 0xff);
      SDL_RenderFillRect(renderer, &track);
      const SDL_FRect fill{bar_x, bar_y, kBarW * std::clamp(item.item.progress, 0.0f, 1.0f), bar_h};
      const SDL_Color fill_color = has_tone ? tone_fill : theme_.accent;
      SDL_SetRenderDrawColor(renderer, fill_color.r, fill_color.g, fill_color.b, 0xff);
      SDL_RenderFillRect(renderer, &fill);
      text_right = bar_x - 6.0f;
    }
    const SDL_FRect text_rect =
        MakeRect(text_x, item.rect.y, std::max(0.0f, text_right - text_x), item.rect.h);
    DrawVCenteredTextOn(text_renderer_, renderer, text_rect, 0.0f, text_color, row_bg,
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
