#include "workspace/render/WorkspaceShellRenderPrimitives.h"

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

void WorkspaceShell::RenderWindowChrome(SDL_Renderer* renderer,
                                        const WorkspaceLayout& layout) const {
  const auto draw_tab_close_button = [&](const SDL_FRect& rect, SDL_Color color,
                                         SDL_Color hover_color) {
    DrawHoverableCloseGlyph(renderer, rect,
                            !context_.interaction_state.tab_drag.dragging &&
                                last_mouse_position_valid_ &&
                                Contains(rect, last_mouse_x_, last_mouse_y_),
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
  // A live drag owns the pointer, so the motion path stops resolving hover for it
  // and `last_mouse_*` stays where the press landed. Nothing on a strip may claim
  // hover from that stale point — an auto-scrolled strip would otherwise light up
  // whichever tab drifted under it. VS Code drops tab hover during a drag too.
  const auto tab_hovered = [&](const SDL_FRect& rect) {
    return !context_.interaction_state.tab_drag.dragging && last_mouse_position_valid_ &&
           Contains(rect, last_mouse_x_, last_mouse_y_);
  };
  // Chrome-like reorder: neighbor tabs render offset by the live slide animation,
  // and the dragged tab is lifted out of the flow (drawn as the floating ghost by
  // DrawTabDragFeedback) during an active drag. During the post-release settle
  // there is no active drag, so the dropped tab renders in-flow and glides home.
  const TabSlideState& tab_slide = context_.interaction_state.tab_slide;
  const TabDragState& tab_drag = context_.interaction_state.tab_drag;
  const auto slide_dx = [&](TabDragKind kind, std::size_t group_index, std::size_t index) -> float {
    const std::span<const float> offsets = tab_slide.OffsetsFor(kind, group_index);
    return index < offsets.size() ? offsets[index] : 0.0f;
  };
  const auto tab_lifted = [&](TabDragKind kind, std::size_t group_index, std::size_t index) -> bool {
    if (!tab_drag.dragging || tab_drag.kind != kind) {
      return false;
    }
    // The dragged tab is lifted out of its SOURCE group's strip for the whole
    // gesture, including while the pointer is over the other group's — the ghost
    // follows the pointer across, and the hole stays open where it came from until
    // the drop commits (TD-2026-08-14-213).
    if (kind == TabDragKind::Editor && tab_drag.source_group_index != group_index) {
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
    const auto& visible_project_tabs =
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
  // global tab-strip band (filled in the frame pass); a pane stacked below
  // another synthesizes its strip inside the editor area, filled here.
  const EditorGroupRectsLayout editor_group_rects = ComputeEditorGroupRectsForState(layout);

  // Drag-to-split feedback (VS Code's drop overlay). Painted before the strips so
  // the floating ghost and the tab strips stay on top of it, and after the editor
  // panes (window chrome runs later in the frame) so it tints the buffer it will
  // replace. The rect was resolved on the motion event that produced it; nothing
  // is recomputed per frame.
  if (tab_drag.body_drop() && tab_drag.dragging) {
    const SDL_FRect& region = tab_drag.body_drop_rect;
    DrawFilledRect(renderer, region,
                   SDL_Color{theme_.accent.r, theme_.accent.g, theme_.accent.b, 64});
    render::OutlineRect(renderer, region, theme_.accent);
  }

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
    const std::vector<VisibleStripTab>& visible_tabs =
        HasActiveProjectCatalogEntry()
            ? tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, group_tab_strip)
            : TabStripService::EmptyVisibleTabs();
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
          drag.dragging && drag.kind == TabDragKind::Editor && gi == drag.target_group_index) {
        // The ghost tracks the pointer, so it paints over the strip the pointer is
        // over — which in a cross-group drag is not the strip the tab came from.
        // Its label still comes from the SOURCE group's list, the only place the
        // dragged tab exists until the drop commits (TD-2026-08-14-213). Asking for
        // another group's visible list is safe here: the memo is per group index,
        // so it cannot invalidate the `visible_tabs` reference above it.
        const bool cross_group = drag.source_group_index != gi;
        const std::vector<VisibleStripTab>& ghost_tabs =
            !cross_group ? visible_tabs
            : drag.source_group_index < editor_group_rects.groups.size()
                ? tab_strip_chrome_.ComputeVisibleTabsForGroup(
                      drag.source_group_index,
                      editor_group_rects.groups[drag.source_group_index].tab_strip)
                : TabStripService::EmptyVisibleTabs();
        DrawTabDragFeedback(text_renderer_, renderer, theme_, group_tab_strip, ghost_tabs,
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
      DrawFilledRect(renderer, item.rect, SDL_Color{tinted.r, tinted.g, tinted.b, 0xff});
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
      DrawFilledRect(renderer, track, SDL_Color{track_color.r, track_color.g, track_color.b, 0xff});
      const SDL_FRect fill{bar_x, bar_y, kBarW * std::clamp(item.item.progress, 0.0f, 1.0f), bar_h};
      const SDL_Color fill_color = has_tone ? tone_fill : theme_.accent;
      DrawFilledRect(renderer, fill, SDL_Color{fill_color.r, fill_color.g, fill_color.b, 0xff});
      text_right = bar_x - 6.0f;
    }
    const SDL_FRect text_rect =
        MakeRect(text_x, item.rect.y, std::max(0.0f, text_right - text_x), item.rect.h);
    DrawVCenteredTextOn(text_renderer_, renderer, text_rect, 0.0f, text_color, row_bg,
                        item.item.text);
  }

  // The breadcrumb is per PANE, not per window: a pane at the top of the editor
  // area owns the band above its own column, and the label there names THAT
  // pane's file. Painting one band for the focused pane put the wrong file over
  // every other column — with a split open, the left column's band named whatever
  // the right column was showing. Panes below the top row have no band of their
  // own (`breadcrumb.w == 0`); their column's band belongs to the pane it sits
  // over. For a single pane this is byte-identical to the old full-width band.
  for (std::size_t gi = 0; gi < editor_group_rects.groups.size(); ++gi) {
    const SDL_FRect band = editor_group_rects.groups[gi].breadcrumb;
    if (band.w <= 0.0f || band.h <= 0.0f) {
      continue;
    }
    float breadcrumb_text_x = band.x + 12.0f;
    float breadcrumb_text_right = band.x + band.w - 12.0f;
    for (const VisibleStatusItem& item : status_items) {
      // Contributed status items are window chrome, laid out across the whole
      // band; only the ones actually overlapping this column push its text in.
      if (item.rect.x + item.rect.w <= band.x || item.rect.x >= band.x + band.w) {
        continue;
      }
      if (item.item.alignment == StatusAlignment::Left) {
        breadcrumb_text_x = std::max(breadcrumb_text_x, item.rect.x + item.rect.w + 8.0f);
      } else {
        breadcrumb_text_right = std::min(breadcrumb_text_right, item.rect.x - 8.0f);
      }
    }
    const float breadcrumb_text_width = std::max(0.0f, breadcrumb_text_right - breadcrumb_text_x);
    DrawVCenteredTextOn(text_renderer_, renderer,
                        MakeRect(breadcrumb_text_x, band.y, breadcrumb_text_width, band.h), 0.0f,
                        theme_.chrome_text, theme_.chrome_background,
                        TruncateLabelView(BreadcrumbLabel(gi), breadcrumb_text_width));
  }
}

}  // namespace microide::workspace
