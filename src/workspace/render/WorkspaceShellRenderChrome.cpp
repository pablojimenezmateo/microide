#include <optional>
#include "render/PaintElement.h"
#include "render/ScopedRenderClip.h"
#include "workspace/BreadcrumbLayout.h"
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
  const StripTabPalette chrome_tab_palette{
      .active_fill = theme_.chrome_active,
      .inactive_fill = theme_.surface_raised,
      .hover_fill = theme_.row_highlight,
      .active_text = theme_.chrome_active_text,
      .inactive_text = theme_.surface_text,
      .active_glyph = theme_.chrome_text_secondary,
      .inactive_glyph = theme_.text_disabled,
  };
  const PreparedFrameState::WindowChromePaint& chrome = frame_state_.window_chrome;
  const auto draw_strip_tabs = [&](const PreparedFrameState::TabStripPaint& strip, const StripTabStyle& style,
                                   const StripTabPalette& palette, std::string_view element_name) {
    for (const PreparedFrameState::PanelTabPaint& tab : strip.tabs) {
      const render::PaintElement element(renderer, tab.rect, element_name);
      DrawStripTab(text_renderer_, renderer, theme_, tab.rect, tab.title, tab.badge_text, tab.badge_color,
                   tab.show_badge, tab.active, style, palette, tab.hovered,
                   StripTabRemoteMark{
                       .secondary_label = tab.secondary_title, .state = tab.remote_state, .dimmed = tab.dimmed});
      DrawStripTabCloseGlyph(renderer, tab.rect, tab.close_rect, tab.close_hovered,
                             tab.active ? palette.active_glyph : palette.inactive_glyph,
                             tab.active ? palette.active_text : palette.inactive_text);
    }
    DrawTabStripOverflowButton(text_renderer_, renderer, theme_, strip.overflow_left.rect, false,
                               strip.overflow_left.hidden, strip.overflow_left.hovered);
    DrawTabStripOverflowButton(text_renderer_, renderer, theme_, strip.overflow_right.rect, true,
                               strip.overflow_right.hidden, strip.overflow_right.hovered);
  };

  for (const PreparedFrameState::WindowChromePaint::MenuItem& item : chrome.menu_items) {
    const bool hovered = item.hovered;
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
                        background, item.label);
  }

  if (const std::optional<SDL_FRect>& chevron = chrome.chevron; chevron.has_value()) {
    const bool hovered = chrome.chevron_hovered;
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

  if (chrome.title_rect.has_value()) {
    DrawVCenteredTextOn(text_renderer_, renderer, *chrome.title_rect, 0.0f, theme_.chrome_text_secondary,
                        theme_.chrome_background, frame_state_.window_title);
  }

  for (const PreparedFrameState::WindowChromePaint::WindowButton& button : chrome.window_buttons) {
    const bool hovered = button.hovered;
    const auto id = static_cast<WindowControlButtonId>(button.id);
    SDL_Color background = hovered ? theme_.row_highlight : theme_.chrome_background;
    SDL_Color glyph = hovered ? theme_.text_primary : theme_.chrome_text_secondary;
    if (id == WindowControlButtonId::Close && hovered) {
      background = theme_.diff_deleted;
      glyph = theme_.text_primary;
    }

    DrawFilledRect(renderer, button.rect, background);
    DrawWindowControlGlyph(renderer, button.rect, id, glyph, chrome.expanded);
  }

  if (const PreparedFrameState::TabStripPaint& strip = frame_state_.project_strip; strip.shown) {
    constexpr StripTabStyle kProjectTabStyle{
        .text_left_padding = 10.0f,
        .badge_size = 16.0f,
        .badge_gap = 8.0f,
        .close_right_reserve = 46.0f,
        .accent_edge = StripAccentEdge::Top,
    };
    draw_strip_tabs(strip, kProjectTabStyle, chrome_tab_palette, "project-tab");
    if (strip.dragging) {
      DrawTabDragFeedback(text_renderer_, renderer, theme_, layout.project_tab_strip, strip.drag_tabs,
                          strip.drag_tabs_strip, strip.drag_source, strip.drag_pointer_x, strip.drag_grab_offset_x,
                          kProjectTabStyle, chrome_tab_palette);
    }
  }

  if (chrome.tab_body_drop.has_value()) {
    const SDL_FRect& region = *chrome.tab_body_drop;
    DrawFilledRect(renderer, region,
                   SDL_Color{theme_.accent.r, theme_.accent.g, theme_.accent.b, 64});
    render::OutlineRect(renderer, region, theme_.accent);
  }

  const StripTabPalette unfocused_group_tab_palette{
      .active_fill = render::BlendColors(theme_.chrome_active, theme_.chrome_background, 0.55f),
      .inactive_fill = chrome_tab_palette.inactive_fill,
      .hover_fill = chrome_tab_palette.hover_fill,
      .active_text = render::BlendColors(theme_.chrome_active_text, theme_.text_muted, 0.55f),
      .inactive_text = chrome_tab_palette.inactive_text,
      .active_glyph = chrome_tab_palette.active_glyph,
      .inactive_glyph = chrome_tab_palette.inactive_glyph,
      .active_accent = render::BlendColors(theme_.accent, theme_.chrome_background, 0.6f),
  };
  const std::size_t focused_group_index = chrome.focused_group;
  for (std::size_t gi = 0; gi < chrome.groups.size(); ++gi) {
    const SDL_FRect group_tab_strip = chrome.groups[gi].tab_strip;
    if (group_tab_strip.h <= 0.0f) {
      continue;
    }
    const StripTabPalette& group_tab_palette =
        (chrome.groups.size() > 1 && gi != focused_group_index)
            ? unfocused_group_tab_palette
            : chrome_tab_palette;
    if (chrome.groups.size() > 1) {
      DrawFilledRect(renderer, group_tab_strip, theme_.chrome_background);
      DrawFilledRect(renderer,
                     MakeRect(group_tab_strip.x,
                              group_tab_strip.y + group_tab_strip.h - kWorkspaceDividerThickness,
                              group_tab_strip.w, kWorkspaceDividerThickness),
                     theme_.border);
    }
    constexpr StripTabStyle kEditorTabStyle{
        .text_left_padding = 10.0f,
        .close_right_reserve = 46.0f,
        .accent_edge = StripAccentEdge::Top,
    };
    const PreparedFrameState::TabStripPaint& strip = frame_state_.editor_strips[gi];
    if (!strip.shown) {
      continue;
    }
    std::optional<render::ScopedRenderClip> strip_clip;
    strip_clip.emplace(renderer, SDL_Rect{static_cast<int>(std::floor(group_tab_strip.x)),
                                          static_cast<int>(std::floor(group_tab_strip.y)),
                                          static_cast<int>(std::ceil(group_tab_strip.w)),
                                          static_cast<int>(std::ceil(group_tab_strip.h))});
    if (strip.placeholder) {
      DrawStripTab(text_renderer_, renderer, theme_, EmptyTabStripPlaceholderRect(group_tab_strip), "Welcome", {}, {},
                   false, true,
                   StripTabStyle{
                       .text_left_padding = 10.0f,
                       .close_right_reserve = 0.0f,
                       .accent_edge = StripAccentEdge::Top,
                   },
                   group_tab_palette);
      continue;
    }
    draw_strip_tabs(strip, kEditorTabStyle, group_tab_palette, "tab");
    strip_clip.reset();
    if (strip.dragging) {
      DrawTabDragFeedback(text_renderer_, renderer, theme_, group_tab_strip, strip.drag_tabs, strip.drag_tabs_strip,
                          strip.drag_source, strip.drag_pointer_x, strip.drag_grab_offset_x, kEditorTabStyle,
                          chrome_tab_palette);
    }
  }

  const auto& status_items = chrome.status_items;
  for (const PreparedFrameState::WindowChromePaint::StatusItem& item : status_items) {
    const SDL_Color tone_fill =
        StatusToneColor(theme_, item.tone, theme_.chrome_background);
    const bool has_tone = item.tone != StatusTone::Default;
    const bool hovered = item.hovered;
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
    if (!item.icon.empty()) {
      if (const auto shape = editor::GutterIconRegistry::ResolveShape(item.icon)) {
        const SDL_Color icon_color = has_tone ? tone_fill : theme_.accent;
        editor::GutterIconRegistry::Draw(renderer, *shape, icon_color, text_x, item.rect.y, 14.0f,
                                         item.rect.h);
        text_x += 16.0f;
      }
    }
    float text_right = item.rect.x + item.rect.w - 8.0f;
    if (item.progress >= 0.0f) {
      constexpr float kBarW = 28.0f;
      const float bar_x = item.rect.x + item.rect.w - kBarW - 6.0f;
      const float bar_h = 4.0f;
      const float bar_y = item.rect.y + (item.rect.h - bar_h) * 0.5f;
      const SDL_FRect track{bar_x, bar_y, kBarW, bar_h};
      const SDL_Color track_color = render::BlendColors(row_bg, theme_.border, 0.6f);
      DrawFilledRect(renderer, track, SDL_Color{track_color.r, track_color.g, track_color.b, 0xff});
      const SDL_FRect fill{bar_x, bar_y, kBarW * std::clamp(item.progress, 0.0f, 1.0f), bar_h};
      const SDL_Color fill_color = has_tone ? tone_fill : theme_.accent;
      DrawFilledRect(renderer, fill, SDL_Color{fill_color.r, fill_color.g, fill_color.b, 0xff});
      text_right = bar_x - 6.0f;
    }
    const SDL_FRect text_rect =
        MakeRect(text_x, item.rect.y, std::max(0.0f, text_right - text_x), item.rect.h);
    DrawVCenteredTextOn(text_renderer_, renderer, text_rect, 0.0f, text_color, row_bg,
                        item.text);
  }

  for (std::size_t gi = 0; gi < chrome.groups.size(); ++gi) {
    const SDL_FRect band = chrome.groups[gi].breadcrumb;
    if (band.w <= 0.0f || band.h <= 0.0f) {
      continue;
    }
    const auto [breadcrumb_text_x, breadcrumb_text_right] = BreadcrumbTextSpan(
        band, status_items,
        [](const PreparedFrameState::WindowChromePaint::StatusItem& item) { return item.left_aligned; });
    const float breadcrumb_text_width = std::max(0.0f, breadcrumb_text_right - breadcrumb_text_x);
    DrawVCenteredTextOn(text_renderer_, renderer,
                        MakeRect(breadcrumb_text_x, band.y, breadcrumb_text_width, band.h), 0.0f,
                        theme_.chrome_text, theme_.chrome_background,
                        TruncateLabelView(chrome.groups[gi].breadcrumb_label, breadcrumb_text_width));
  }

  if (chrome.path_drop.has_value()) {
    DrawFilledRect(renderer, *chrome.path_drop, SDL_Color{theme_.accent.r, theme_.accent.g, theme_.accent.b, 64});
    render::OutlineRect(renderer, *chrome.path_drop, theme_.accent);
  }
  if (chrome.path_ghost.has_value()) {
    const SDL_FRect& ghost = *chrome.path_ghost;
    DrawFilledRect(renderer, ghost, theme_.surface_raised);
    render::OutlineRect(renderer, ghost, theme_.border);
    DrawVCenteredTextOn(text_renderer_, renderer, ghost, kPathDragGhostPaddingPx, theme_.text_primary,
                        theme_.surface_raised,
                        TruncateLabelView(chrome.path_label, ghost.w - 2.0f * kPathDragGhostPaddingPx));
  }
}

}
