#include "workspace/shell/WorkspaceShell.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "render/ScopedRenderClip.h"
#include "workspace/render/NotificationLayout.h"
#include "workspace/services/StatusBarService.h"

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
    // the color. Clickable segments then take the accent color on top of that.
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

  ForEachStatusBarSegmentRect(vm, text_renderer_, draw_segment);
}

void WorkspaceShell::RenderNotifications(SDL_Renderer* renderer,
                                        const WorkspaceLayout& layout) const {
  const NotificationsViewModel vm =
      RenderViewModelBuilder(context_).BuildNotifications(notification_service_);
  if (vm.entries.empty()) {
    return;
  }

  // Stack upward from just above the status bar, newest toast at the bottom. The
  // geometry lives in NotificationLayout.h because the click that dismisses a
  // toast has to hit exactly the card that was painted.
  std::size_t stack_position = 0;
  for (auto it = vm.entries.rbegin(); it != vm.entries.rend(); ++it, ++stack_position) {
    const std::string_view message = it->message;
    const NotificationToastLayout toast =
        NotificationToastLayoutAt(layout.status_bar, text_renderer_.LineHeight(), stack_position,
                                  text_renderer_.MeasureWidth(message));

    DrawCardFrame(renderer, theme_, toast.rect, CardStyle::Overlay);
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
    DrawFilledRect(renderer, toast.accent, accent);

    const SDL_Rect clip{static_cast<int>(toast.text.x), static_cast<int>(toast.text.y),
                        static_cast<int>(std::ceil(toast.text.w)),
                        static_cast<int>(std::ceil(toast.text.h))};
    {
      // Truncate rather than let the clip rect shear the last glyph: a toast is
      // transient, so a message that ends mid-word with no "…" reads as a
      // rendering fault instead of "there was more here".
      const render::ScopedRenderClip clip_scope(renderer, clip);
      DrawVCenteredTextOn(text_renderer_, renderer, toast.text, 0.0f, theme_.text_primary,
                          theme_.overlay_background,
                          text_renderer_.TruncateToWidthEphemeralView(message, toast.text.w));
    }
  }
}

}  // namespace microide::workspace
