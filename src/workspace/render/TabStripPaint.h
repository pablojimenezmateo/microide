#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "workspace/render/PreparedFrameState.h"
#include "workspace/services/TabStripService.h"

namespace microide::workspace {

inline void ResetTabStripPaint(PreparedFrameState::TabStripPaint& strip) {
  strip.shown = false;
  strip.placeholder = false;
  strip.tabs.clear();
  strip.text.clear();
  strip.overflow_left = {};
  strip.overflow_right = {};
  strip.dragging = false;
  strip.drag_tabs.clear();
}

template <typename PointerOver>
void SettleTabStripTabs(PreparedFrameState::TabStripPaint& strip, const std::vector<VisibleStripTab>& visible,
                        std::span<const float> slide, std::optional<std::size_t> lifted, bool at_rest,
                        PointerOver&& pointer_over) {
  strip.tabs.clear();
  strip.text.clear();
  std::size_t text_size = 0;
  for (const VisibleStripTab& tab : visible) {
    text_size += tab.display_title.size() + tab.badge_text.size() + tab.secondary_title.size();
  }
  strip.text.reserve(text_size);
  const auto own = [&strip](std::string_view text) {
    const std::size_t at = strip.text.size();
    strip.text.append(text);
    return std::string_view(strip.text).substr(at, text.size());
  };
  for (const VisibleStripTab& tab : visible) {
    if (lifted.has_value() && tab.index == *lifted) {
      continue;
    }
    const float dx = tab.index < slide.size() ? slide[tab.index] : 0.0f;
    SDL_FRect rect = tab.rect;
    SDL_FRect close_rect = tab.close_rect;
    rect.x += dx;
    close_rect.x += dx;
    strip.tabs.push_back(PreparedFrameState::PanelTabPaint{
        .rect = rect,
        .close_rect = close_rect,
        .title = own(tab.display_title),
        .badge_text = own(tab.badge_text),
        .badge_color = tab.badge_color,
        .secondary_title = own(tab.secondary_title),
        .remote_state = tab.remote_state,
        .show_badge = tab.show_badge,
        .active = tab.active,
        .hovered = at_rest && pointer_over(rect),
        .close_hovered = at_rest && pointer_over(close_rect),
        .dimmed = tab.dimmed,
        .activity_dot = tab.activity_dot,
        .bell_mark = tab.bell_mark,
    });
  }
}

template <typename PointerOver>
void SettleTabStripOverflow(PreparedFrameState::TabStripPaint& strip, const TabStripOverflowControls& overflow,
                            PointerOver&& pointer_over) {
  strip.overflow_left = PreparedFrameState::PanelButtonPaint{
      .rect = overflow.left_button, .hovered = pointer_over(overflow.left_button), .hidden = overflow.hidden_left};
  strip.overflow_right = PreparedFrameState::PanelButtonPaint{
      .rect = overflow.right_button, .hovered = pointer_over(overflow.right_button), .hidden = overflow.hidden_right};
}

inline void SettleTabStripDrag(PreparedFrameState::TabStripPaint& strip, const std::vector<VisibleStripTab>& source_tabs,
                               const SDL_FRect& source_strip, std::size_t source_index, float pointer_x,
                               float grab_offset_x) {
  strip.dragging = true;
  strip.drag_tabs = source_tabs;
  strip.drag_tabs_strip = source_strip;
  strip.drag_source = source_index;
  strip.drag_pointer_x = pointer_x;
  strip.drag_grab_offset_x = grab_offset_x;
}

}
