#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/StatusBarService.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderStatusBar(SDL_Renderer* renderer,
                                     const WorkspaceLayout& layout) const {
  const StatusBarViewModel vm = RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
  if (!vm.visible) {
    return;
  }

  DrawFilledRect(renderer, vm.rect, theme_.chrome_background);
  DrawFilledRect(renderer, MakeRect(vm.rect.x, vm.rect.y, vm.rect.w, 1.0f), theme_.border);

  const float padding = 12.0f;
  const float gap = 14.0f;
  const auto tone_color = [&](StatusBarSegmentTone tone, SDL_Color fallback) -> SDL_Color {
    switch (tone) {
      case StatusBarSegmentTone::Error:
        return theme_.diagnostic_error;
      case StatusBarSegmentTone::Warning:
        return theme_.diagnostic_warning;
      case StatusBarSegmentTone::Info:
        return theme_.diagnostic_info;
      case StatusBarSegmentTone::Default:
      default:
        return fallback;
    }
  };
  const auto segment_color = [&](const StatusBarSegmentViewModel& seg) -> SDL_Color {
    // Diagnostic segments are colored by semantic tone (set in
    // StatusBarModelService) so the count/state -- not the display text -- picks
    // the color. Problems keeps a severity color even though it is clickable;
    // the clickable hover affordance is applied separately in draw_segment.
    switch (seg.id) {
      case StatusBarSegmentId::Problems:
        return tone_color(seg.tone, theme_.diagnostic_warning);
      case StatusBarSegmentId::Lsp:
        return tone_color(seg.tone, theme_.chrome_text_secondary);
      default:
        break;
    }
    if (seg.clickable) {
      return theme_.accent;
    }
    switch (seg.id) {
      case StatusBarSegmentId::Project:
      case StatusBarSegmentId::Branch:
      case StatusBarSegmentId::LineColumn:
        return theme_.chrome_text;
      default:
        return theme_.chrome_text_secondary;
    }
  };
  // Clickable segments get a hover background so the pointer cursor (already returned by
  // CursorKindForPosition for these segments) is matched by a visible affordance.
  const auto draw_segment = [&](const StatusBarSegmentViewModel& seg, const SDL_FRect& row) {
    const bool hovered = seg.clickable && last_mouse_position_valid_ &&
                         Contains(row, last_mouse_x_, last_mouse_y_);
    SDL_Color text_bg = theme_.chrome_background;
    if (hovered) {
      const SDL_FRect hover_rect = MakeRect(row.x - 4.0f, row.y, row.w + 8.0f, row.h);
      DrawSelectableRowBackground(renderer, theme_, hover_rect, theme_.chrome_background, true);
      text_bg = theme_.row_highlight;
    }
    DrawVCenteredTextOn(text_renderer_, renderer, row, 0.0f,
                        hovered ? theme_.text_primary : segment_color(seg), text_bg, seg.text);
  };

  float left_x = vm.rect.x + padding;
  for (const StatusBarSegmentViewModel& seg : vm.left_segments) {
    const float width = text_renderer_.MeasureWidth(seg.text);
    draw_segment(seg, MakeRect(left_x, vm.rect.y, width, vm.rect.h));
    left_x += width + gap;
  }

  float right_x = vm.rect.x + vm.rect.w - padding;
  for (auto it = vm.right_segments.rbegin(); it != vm.right_segments.rend(); ++it) {
    const float width = text_renderer_.MeasureWidth(it->text);
    right_x -= width;
    draw_segment(*it, MakeRect(right_x, vm.rect.y, width, vm.rect.h));
    right_x -= gap;
  }
}

void WorkspaceShell::RenderNotifications(SDL_Renderer* renderer,
                                        const WorkspaceLayout& layout) const {
  const NotificationsViewModel vm =
      RenderViewModelBuilder(context_).BuildNotifications(notification_service_);
  if (vm.entries.empty()) {
    return;
  }

  constexpr float kMargin = 12.0f;
  constexpr float kPadding = 10.0f;
  constexpr float kGap = 8.0f;
  constexpr float kAccentWidth = 3.0f;
  constexpr float kMaxTextWidth = 320.0f;

  const float toast_height = text_renderer_.LineHeight() + kPadding * 2.0f;
  const float right_edge = layout.status_bar.x + layout.status_bar.w - kMargin;
  // Stack upward from just above the status bar, newest toast at the bottom.
  float bottom = layout.status_bar.y - kMargin;

  for (auto it = vm.entries.rbegin(); it != vm.entries.rend(); ++it) {
    const std::string_view message = it->message;
    const float text_width = std::min(kMaxTextWidth, text_renderer_.MeasureWidth(message));
    const float toast_width = kAccentWidth + kPadding * 2.0f + text_width;
    const SDL_FRect rect =
        MakeRect(right_edge - toast_width, bottom - toast_height, toast_width, toast_height);

    DrawCardFrame(renderer, theme_, rect, CardStyle::Overlay);
    SDL_Color accent = theme_.diagnostic_info;
    switch (it->tone) {
      case NotificationService::Tone::Error:
        accent = theme_.diagnostic_error;
        break;
      case NotificationService::Tone::Warning:
        accent = theme_.diagnostic_warning;
        break;
      case NotificationService::Tone::Info:
        break;
    }
    DrawFilledRect(renderer, MakeRect(rect.x, rect.y, kAccentWidth, rect.h), accent);

    const SDL_FRect text_rect =
        MakeRect(rect.x + kAccentWidth + kPadding, rect.y, text_width, rect.h);
    const SDL_Rect clip{static_cast<int>(text_rect.x), static_cast<int>(text_rect.y),
                        static_cast<int>(std::ceil(text_rect.w)),
                        static_cast<int>(std::ceil(text_rect.h))};
    SDL_SetRenderClipRect(renderer, &clip);
    DrawVCenteredTextOn(text_renderer_, renderer, text_rect, 0.0f, theme_.text_primary,
                        theme_.overlay_background, message);
    SDL_SetRenderClipRect(renderer, nullptr);

    bottom = rect.y - kGap;
  }
}

}  // namespace microide::workspace
