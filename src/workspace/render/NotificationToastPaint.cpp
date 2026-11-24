#include "workspace/render/NotificationToastPaint.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "render/PaintElement.h"
#include "render/ScopedRenderClip.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/render/NotificationLayout.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

namespace {

// The fingerprint of everything a toast paints. The structured bindings name
// every field of the entry, its layout and its buttons: one added to them stops
// this from compiling until it is folded in too.
std::uint64_t FingerprintOf(const NotificationEntryViewModel& entry, std::uint64_t style) {
  const auto& [index, tone, message, lines, progress, layout, buttons, focused] = entry;
  const auto& [rect, accent, text, button_row, progress_track] = layout;
  PaintFingerprint fingerprint;
  fingerprint.Add(index).Add(tone).Add(message).Add(lines.size());
  for (const std::string_view line : lines) {
    fingerprint.Add(line);
  }
  fingerprint.Add(progress.has_value()).Add(progress.value_or(0.0f));
  fingerprint.Add(rect).Add(accent).Add(text).Add(button_row).Add(progress_track);
  fingerprint.Add(buttons.size());
  for (const NotificationButtonViewModel& button : buttons) {
    const auto& [label, label_width, button_rect, hovered, button_focused] = button;
    fingerprint.Add(label).Add(label_width).Add(button_rect).Add(hovered).Add(button_focused);
  }
  return fingerprint.Add(focused).Add(style).value();
}

// A toast's unit rect: the card's, snapped as its PaintElement clips (clip-discipline D2).

SDL_Color ToneAccent(const render::Theme& theme, NotificationService::Tone tone) {
  switch (tone) {
    case NotificationService::Tone::Error:
      return theme.diagnostic_error;
    case NotificationService::Tone::Warning:
      return theme.diagnostic_warning;
    case NotificationService::Tone::Info:
      break;
  }
  return theme.diagnostic_info;
}

}  // namespace

void PrepareNotificationToastsFrame(PreparedFrameState& frame, SDL_Renderer* renderer,
                                    const NotificationsViewModel& toasts, const render::Theme& theme,
                                    const std::function<void(const SDL_FRect&)>& damage) {
  frame.notification_toasts = toasts;
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::NotificationToasts);
  const std::uint64_t style = PaintFingerprint()
                                  .Add(theme.overlay_background)
                                  .Add(theme.border)
                                  .Add(theme.text_primary)
                                  .Add(theme.accent)
                                  .Add(theme.diagnostic_info)
                                  .Add(theme.diagnostic_warning)
                                  .Add(theme.diagnostic_error)
                                  .Add(theme.surface_raised)
                                  .Add(theme.row_highlight)
                                  .value();
  for (const NotificationEntryViewModel& entry : frame.notification_toasts.entries) {
    units.push_back(DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(renderer, entry.layout.rect),
                                        .fingerprint = FingerprintOf(entry, style)});
  }
  frame.derived_damage.Commit(DerivedSurface::NotificationToasts, damage);
}

void PaintNotificationToasts(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                             const render::TextRenderer& text_renderer) {
  for (const NotificationEntryViewModel& entry : frame.notification_toasts.entries) {
    const NotificationToastLayout& toast = entry.layout;
    const render::PaintElement element(renderer, toast.rect, "notification");
    DrawCardFrame(renderer, theme, toast.rect, CardStyle::Overlay);
    const SDL_Color accent = ToneAccent(theme, entry.tone);
    FillRect(renderer, toast.accent, accent);

    const SDL_Rect clip{static_cast<int>(toast.text.x), static_cast<int>(toast.text.y),
                        static_cast<int>(std::ceil(toast.text.w)), static_cast<int>(std::ceil(toast.text.h))};
    {
      // Truncate rather than let the clip rect shear the last glyph: a toast is
      // transient, so a message that ends mid-word with no "…" reads as a
      // rendering fault instead of "there was more here".
      const render::ScopedRenderClip clip_scope(renderer, clip);
      const float line_height = text_renderer.LineHeight();
      for (std::size_t line = 0; line < entry.lines.size(); ++line) {
        const SDL_FRect line_rect{toast.text.x,
                                  toast.text.y + kNotificationToastPadding + line_height * static_cast<float>(line),
                                  toast.text.w, line_height};
        DrawVCenteredTextOn(text_renderer, renderer, line_rect, 0.0f, theme.text_primary, theme.overlay_background,
                            text_renderer.TruncateToWidthEphemeralView(entry.lines[line], toast.text.w));
      }
    }

    // The last button is the primary one (VS Code's order), drawn in the accent.
    for (std::size_t i = 0; i < entry.buttons.size(); ++i) {
      const NotificationButtonViewModel& button = entry.buttons[i];
      DrawButtonCentered(text_renderer, renderer, theme, button.rect,
                         text_renderer.TruncateToWidthEphemeralView(button.label, button.label_width),
                         i + 1 == entry.buttons.size() ? ButtonTone::Accent : ButtonTone::Neutral,
                         ButtonVisualState{.enabled = true, .hovered = button.hovered, .active = false});
      if (button.focused) {
        OutlineRect(renderer,
                    SDL_FRect{button.rect.x - 2.0f, button.rect.y - 2.0f, button.rect.w + 4.0f, button.rect.h + 4.0f},
                    theme.accent);
      }
    }
    if (entry.focused) {
      OutlineRect(renderer, toast.rect, theme.accent);
    }

    if (entry.progress.has_value()) {
      // Track then fill, in the row's own tone: a progress row reports work that
      // is still running, so it is sticky and this is the only thing on the card
      // that changes between frames.
      FillRect(renderer, toast.progress_track, theme.overlay_background);
      SDL_FRect fill = toast.progress_track;
      fill.w = toast.progress_track.w * *entry.progress;
      FillRect(renderer, fill, accent);
    }
  }
}

SDL_FRect NotificationToastsBounds(const NotificationsViewModel& toasts) {
  SDL_FRect bounds{};
  bool any = false;
  for (const NotificationEntryViewModel& entry : toasts.entries) {
    const SDL_FRect& rect = entry.layout.rect;
    if (!any) {
      bounds = rect;
      any = true;
      continue;
    }
    const float left = std::min(bounds.x, rect.x);
    const float top = std::min(bounds.y, rect.y);
    const float right = std::max(bounds.x + bounds.w, rect.x + rect.w);
    const float bottom = std::max(bounds.y + bounds.h, rect.y + rect.h);
    bounds = SDL_FRect{left, top, right - left, bottom - top};
  }
  return bounds;
}

}  // namespace microide::workspace
