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
  if (notification_service_.Empty()) {
    return;
  }
  // Geometry — card, message line, button rects — is composed by the builder, and
  // the click and hover paths hit-test the same view model, so a button can never
  // be painted somewhere a click does not reach.
  const NotificationsViewModel vm = RenderViewModelBuilder(context_).BuildNotifications(
      notification_service_, layout.status_bar, text_renderer_);

  for (const NotificationEntryViewModel& entry : vm.entries) {
    const NotificationToastLayout& toast = entry.layout;
    DrawCardFrame(renderer, theme_, toast.rect, CardStyle::Overlay);
    SDL_Color accent = theme_.diagnostic_info;
    switch (entry.tone) {
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
      const float line_height = text_renderer_.LineHeight();
      for (std::size_t line = 0; line < entry.lines.size(); ++line) {
        const SDL_FRect line_rect{toast.text.x,
                                  toast.text.y + kNotificationToastPadding +
                                      line_height * static_cast<float>(line),
                                  toast.text.w, line_height};
        DrawVCenteredTextOn(text_renderer_, renderer, line_rect, 0.0f, theme_.text_primary,
                            theme_.overlay_background,
                            text_renderer_.TruncateToWidthEphemeralView(entry.lines[line], toast.text.w));
      }
    }

    // The last button is the primary one (VS Code's order), drawn in the accent.
    for (std::size_t i = 0; i < entry.buttons.size(); ++i) {
      const NotificationButtonViewModel& button = entry.buttons[i];
      DrawButtonCentered(
          text_renderer_, renderer, theme_, button.rect,
          text_renderer_.TruncateToWidthEphemeralView(button.label, button.label_width),
          i + 1 == entry.buttons.size() ? ButtonTone::Accent : ButtonTone::Neutral,
          ButtonVisualState{.enabled = true, .hovered = button.hovered, .active = false});
      if (button.focused) {
        OutlineRect(renderer,
                    SDL_FRect{button.rect.x - 2.0f, button.rect.y - 2.0f, button.rect.w + 4.0f,
                              button.rect.h + 4.0f},
                    theme_.accent);
      }
    }
    if (entry.focused) {
      OutlineRect(renderer, toast.rect, theme_.accent);
    }

    if (entry.progress.has_value()) {
      // Track then fill, in the row's own tone: a progress row reports work that
      // is still running, so it is sticky and this is the only thing on the card
      // that changes between frames.
      DrawFilledRect(renderer, toast.progress_track, theme_.overlay_background);
      SDL_FRect fill = toast.progress_track;
      fill.w = toast.progress_track.w * *entry.progress;
      DrawFilledRect(renderer, fill, accent);
    }
  }
}

}  // namespace microide::workspace
