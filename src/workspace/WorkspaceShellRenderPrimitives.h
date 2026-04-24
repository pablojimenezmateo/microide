#pragma once

#include "render/SurfacePrimitives.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace microide::workspace::detail {

enum class CardStyle {
  Raised,
  Overlay,
  Tooltip,
};

enum class ButtonTone {
  Neutral,
  Accent,
  Destructive,
};

struct ButtonVisualState {
  bool enabled = true;
  bool hovered = false;
  bool active = false;
};

struct ButtonColors {
  SDL_Color fill{};
  SDL_Color border{};
  SDL_Color text{};
};

struct TooltipLayout {
  SDL_FRect rect{};
  std::string text;
};

enum class StripAccentEdge {
  Top,
  Bottom,
};

struct StripTabPalette {
  SDL_Color active_fill{};
  SDL_Color inactive_fill{};
  SDL_Color active_text{};
  SDL_Color inactive_text{};
  SDL_Color active_glyph{};
  SDL_Color inactive_glyph{};
};

struct StripTabStyle {
  float text_left_padding = 8.0f;
  float badge_size = 0.0f;
  float badge_gap = 0.0f;
  float close_right_reserve = 40.0f;
  StripAccentEdge accent_edge = StripAccentEdge::Top;
};

inline void FillRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  render::FillRect(renderer, rect, color);
}

inline void OutlineRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  render::OutlineRect(renderer, rect, color);
}

inline SDL_Color CardBackground(const render::Theme& theme, CardStyle style) {
  switch (style) {
    case CardStyle::Overlay:
      return render::CardBackground(theme, render::CardStyle::Overlay);
    case CardStyle::Tooltip:
      return render::CardBackground(theme, render::CardStyle::Tooltip);
    case CardStyle::Raised:
    default:
      return render::CardBackground(theme, render::CardStyle::Raised);
  }
}

inline void DrawCardFrame(SDL_Renderer* renderer,
                          const render::Theme& theme,
                          const SDL_FRect& rect,
                          CardStyle style) {
  render::DrawCardFrame(renderer, theme, rect,
                        style == CardStyle::Overlay   ? render::CardStyle::Overlay
                        : style == CardStyle::Tooltip ? render::CardStyle::Tooltip
                                                      : render::CardStyle::Raised);
}

inline SDL_FRect DrawTitledCardFrame(SDL_Renderer* renderer,
                                     const render::Theme& theme,
                                     const SDL_FRect& rect,
                                     float header_height,
                                     CardStyle style) {
  return render::DrawTitledCardFrame(
      renderer, theme, rect, header_height,
      style == CardStyle::Overlay   ? render::CardStyle::Overlay
      : style == CardStyle::Tooltip ? render::CardStyle::Tooltip
                                    : render::CardStyle::Raised);
}

inline ButtonColors ResolveButtonColors(const render::Theme& theme,
                                        ButtonTone tone,
                                        const ButtonVisualState& state) {
  if (!state.enabled) {
    return ButtonColors{
        .fill = theme.surface_raised,
        .border = theme.border,
        .text = theme.text_muted,
    };
  }

  if (state.active) {
    if (tone == ButtonTone::Destructive) {
      return ButtonColors{
          .fill = theme.diff_deleted,
          .border = theme.diff_deleted,
          .text = theme.text_primary,
      };
    }
    return ButtonColors{
        .fill = theme.chrome_active,
        .border = theme.accent,
        .text = theme.chrome_active_text,
    };
  }

  if (state.hovered) {
    return ButtonColors{
        .fill = theme.row_highlight,
        .border = tone == ButtonTone::Destructive ? theme.diff_deleted : theme.accent,
        .text = theme.text_primary,
    };
  }

  return ButtonColors{
      .fill = theme.surface_raised,
      .border = theme.border,
      .text = tone == ButtonTone::Destructive
                  ? theme.diff_deleted
                  : tone == ButtonTone::Accent ? theme.accent : theme.text_secondary,
  };
}

inline void DrawButtonCentered(const render::TextRenderer& text_renderer,
                               SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& rect,
                               std::string_view label,
                               ButtonTone tone,
                               const ButtonVisualState& state) {
  const ButtonColors colors = ResolveButtonColors(theme, tone, state);
  FillRect(renderer, rect, colors.fill);
  OutlineRect(renderer, rect, colors.border);
  const float text_width = text_renderer.MeasureWidth(label);
  const float x = rect.x + std::floor(std::max(0.0f, rect.w - text_width) * 0.5f);
  const float y = rect.y + std::floor(std::max(0.0f, rect.h - text_renderer.LineHeight()) * 0.5f);
  text_renderer.DrawStringOn(renderer, x, y, colors.text, colors.fill, label);
}

inline void DrawSelectableRowBackground(SDL_Renderer* renderer,
                                        const render::Theme& theme,
                                        const SDL_FRect& rect,
                                        SDL_Color base_fill,
                                        bool emphasized,
                                        bool accent_strip = false,
                                        SDL_Color accent_color = SDL_Color{}) {
  const SDL_Color fill = emphasized ? theme.row_highlight : base_fill;
  FillRect(renderer, rect, fill);
  if (emphasized && accent_strip) {
    const SDL_Color strip = accent_color.a == 0 ? theme.accent : accent_color;
    FillRect(renderer, SDL_FRect{rect.x, rect.y, 2.0f, rect.h}, strip);
  }
}

inline void DrawTextFieldFrame(SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& rect,
                               bool active) {
  FillRect(renderer, rect, theme.surface_background);
  OutlineRect(renderer, rect, active ? theme.accent : theme.border);
}

inline TooltipLayout BuildTooltipLayout(const render::TextRenderer& text_renderer,
                                        std::string_view label,
                                        float max_width,
                                        float min_width = 160.0f) {
  const float clamped_max = std::max(min_width, max_width);
  const std::string text = text_renderer.TruncateToWidth(label, clamped_max - 16.0f);
  const float width = std::clamp(text_renderer.MeasureWidth(text) + 16.0f, min_width, clamped_max);
  return TooltipLayout{
      .rect = SDL_FRect{0.0f, 0.0f, width, text_renderer.LineHeight() + 10.0f},
      .text = text,
  };
}

inline void DrawTooltip(const render::TextRenderer& text_renderer,
                        SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& rect,
                        std::string_view text) {
  render::DrawCardFrame(renderer, theme, rect, render::CardStyle::Tooltip);
  const float y = rect.y + std::floor(std::max(0.0f, rect.h - text_renderer.LineHeight()) * 0.5f);
  text_renderer.DrawStringOn(renderer, rect.x + 8.0f, y, theme.text_primary,
                             render::CardBackground(theme, render::CardStyle::Tooltip), text);
}

inline std::string_view DiagnosticSeverityLabel(editor::DiagnosticSeverity severity) {
  switch (severity) {
    case editor::DiagnosticSeverity::Error:
      return "Error";
    case editor::DiagnosticSeverity::Warning:
      return "Warning";
    case editor::DiagnosticSeverity::Info:
      return "Info";
    case editor::DiagnosticSeverity::Hint:
      return "Hint";
  }
  return "Diagnostic";
}

inline void DrawScrollbarTrack(SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& track) {
  if (renderer == nullptr || track.w <= 0.0f || track.h <= 0.0f) {
    return;
  }

  FillRect(renderer, track, theme.surface_raised);
}

inline void DrawScrollbarThumb(SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& thumb,
                               bool active = false) {
  if (renderer == nullptr || thumb.w <= 0.0f || thumb.h <= 0.0f) {
    return;
  }

  const SDL_Color thumb_color = active ? theme.accent : theme.text_disabled;
  FillRect(renderer, thumb, thumb_color);
}

inline void DrawScrollbar(SDL_Renderer* renderer,
                          const render::Theme& theme,
                          const SDL_FRect& track,
                          const SDL_FRect& thumb,
                          bool active = false) {
  DrawScrollbarTrack(renderer, theme, track);
  DrawScrollbarThumb(renderer, theme, thumb, active);
}

inline char GitMarker(project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Conflicted:
      return '!';
    case project::GitFileStatus::Modified:
      return 'M';
    case project::GitFileStatus::Added:
      return 'A';
    case project::GitFileStatus::Deleted:
      return 'D';
    case project::GitFileStatus::Untracked:
      return 'U';
    case project::GitFileStatus::Clean:
    default:
      return ' ';
  }
}

inline SDL_Color GitMarkerColor(const render::Theme& theme, project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Conflicted:
      return theme.accent;
    case project::GitFileStatus::Modified:
      return theme.diff_modified;
    case project::GitFileStatus::Added:
      return theme.diff_added;
    case project::GitFileStatus::Deleted:
      return theme.diff_deleted;
    case project::GitFileStatus::Untracked:
      return theme.accent;
    case project::GitFileStatus::Clean:
    default:
      return theme.text_disabled;
  }
}

inline void DrawChevron(SDL_Renderer* renderer,
                        float x,
                        float center_y,
                        bool expanded,
                        SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  if (expanded) {
    SDL_RenderLine(renderer, x, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    SDL_RenderLine(renderer, x + 8.0f, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    return;
  }

  SDL_RenderLine(renderer, x + 2.0f, center_y - 4.0f, x + 6.0f, center_y);
  SDL_RenderLine(renderer, x + 2.0f, center_y + 4.0f, x + 6.0f, center_y);
}

inline void DrawCloseGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  SDL_RenderLine(renderer, cx - 3.0f, cy - 3.0f, cx + 3.0f, cy + 3.0f);
  SDL_RenderLine(renderer, cx + 3.0f, cy - 3.0f, cx - 3.0f, cy + 3.0f);
}

inline void DrawCheckGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float left = std::floor(rect.x + 2.0f);
  const float mid_x = std::floor(rect.x + rect.w * 0.45f);
  const float right = std::floor(rect.x + rect.w - 2.0f);
  const float upper_y = std::floor(rect.y + rect.h * 0.35f);
  const float lower_y = std::floor(rect.y + rect.h - 3.0f);
  SDL_RenderLine(renderer, left, upper_y + 2.0f, mid_x, lower_y);
  SDL_RenderLine(renderer, mid_x, lower_y, right, upper_y);
}

inline void DrawPlusGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  SDL_RenderLine(renderer, cx - 4.0f, cy, cx + 4.0f, cy);
  SDL_RenderLine(renderer, cx, cy - 4.0f, cx, cy + 4.0f);
}

inline void DrawHoverableCloseGlyph(SDL_Renderer* renderer,
                                    const SDL_FRect& rect,
                                    bool hovered,
                                    SDL_Color color,
                                    SDL_Color hover_color) {
  DrawCloseGlyph(renderer, rect, hovered ? hover_color : color);
}

inline void DrawWindowControlGlyph(SDL_Renderer* renderer,
                                   const SDL_FRect& rect,
                                   WorkspaceShell::WindowControlButtonId id,
                                   SDL_Color color,
                                   bool expanded) {
  if (renderer == nullptr) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float left = rect.x + 4.0f;
  const float right = rect.x + rect.w - 4.0f;
  const float top = rect.y + 4.0f;
  const float bottom = rect.y + rect.h - 4.0f;
  const float center_y = rect.y + rect.h * 0.5f;

  switch (id) {
    case WorkspaceShell::WindowControlButtonId::Minimize:
      SDL_RenderLine(renderer, left, center_y + 2.0f, right, center_y + 2.0f);
      return;
    case WorkspaceShell::WindowControlButtonId::Maximize:
      if (expanded) {
        const SDL_FRect back = SDL_FRect{left + 1.5f, top + 3.0f, rect.w - 9.0f, rect.h - 9.0f};
        const SDL_FRect front = SDL_FRect{left - 1.0f, top + 1.0f, rect.w - 9.0f, rect.h - 9.0f};
        SDL_RenderRect(renderer, &back);
        SDL_RenderRect(renderer, &front);
      } else {
        const SDL_FRect outline = SDL_FRect{left, top + 1.0f, rect.w - 8.0f, rect.h - 8.0f};
        SDL_RenderRect(renderer, &outline);
      }
      return;
    case WorkspaceShell::WindowControlButtonId::Close:
      SDL_RenderLine(renderer, left, top, right, bottom);
      SDL_RenderLine(renderer, right, top, left, bottom);
      return;
  }
}

inline void DrawText(const render::TextRenderer& text_renderer,
                     SDL_Renderer* renderer,
                     float x,
                     float y,
                     SDL_Color foreground,
                     std::string_view text) {
  text_renderer.DrawString(renderer, x, y, foreground, text);
}

inline void DrawTextOn(const render::TextRenderer& text_renderer,
                       SDL_Renderer* renderer,
                       float x,
                       float y,
                       SDL_Color foreground,
                       SDL_Color background,
                       std::string_view text) {
  text_renderer.DrawStringOn(renderer, x, y, foreground, background, text);
}

inline void DrawVCenteredTextOn(const render::TextRenderer& text_renderer,
                                SDL_Renderer* renderer,
                                const SDL_FRect& rect,
                                float left_padding,
                                SDL_Color foreground,
                                SDL_Color background,
                                std::string_view text) {
  (void)background;
  const float y =
      rect.y + std::floor(std::max(0.0f, rect.h - text_renderer.LineHeight()) * 0.5f);
  DrawText(text_renderer, renderer, rect.x + left_padding, y, foreground, text);
}

inline void DrawCenteredTextOn(const render::TextRenderer& text_renderer,
                               SDL_Renderer* renderer,
                               const SDL_FRect& rect,
                               SDL_Color foreground,
                               SDL_Color background,
                               std::string_view text) {
  (void)background;
  const float text_width = text_renderer.MeasureWidth(text);
  const float x = rect.x + std::floor(std::max(0.0f, rect.w - text_width) * 0.5f);
  const float y =
      rect.y + std::floor(std::max(0.0f, rect.h - text_renderer.LineHeight()) * 0.5f);
  DrawText(text_renderer, renderer, x, y, foreground, text);
}

inline void DrawPrimarySecondaryRowText(const render::TextRenderer& text_renderer,
                                        SDL_Renderer* renderer,
                                        const SDL_FRect& rect,
                                        float text_x,
                                        float right_edge,
                                        SDL_Color primary_color,
                                        SDL_Color secondary_color,
                                        SDL_Color background,
                                        std::string_view primary,
                                        std::string_view secondary,
                                        float primary_width_fraction = 0.62f,
                                        float gap = 8.0f) {
  const float text_y =
      rect.y + std::floor(std::max(0.0f, rect.h - text_renderer.LineHeight()) * 0.5f);
  const float max_width = std::max(20.0f, right_edge - text_x);
  const std::string primary_text = text_renderer.TruncateToWidth(
      primary, secondary.empty() ? max_width : max_width * primary_width_fraction);
  DrawTextOn(text_renderer, renderer, text_x, text_y, primary_color, background, primary_text);
  if (secondary.empty()) {
    return;
  }
  const float secondary_x = text_x + text_renderer.MeasureWidth(primary_text) + gap;
  const float secondary_width = std::max(0.0f, right_edge - secondary_x);
  if (secondary_width <= 24.0f) {
    return;
  }
  DrawTextOn(text_renderer, renderer, secondary_x, text_y, secondary_color, background,
             text_renderer.TruncateToWidth(secondary, secondary_width));
}

inline void DrawStripTab(const render::TextRenderer& text_renderer,
                         SDL_Renderer* renderer,
                         const render::Theme& theme,
                         const SDL_FRect& rect,
                         std::string_view label,
                         std::string_view badge_text,
                         SDL_Color badge_color,
                         bool show_badge,
                         bool active,
                         const StripTabStyle& style,
                         const StripTabPalette& palette) {
  const SDL_Color background = active ? palette.active_fill : palette.inactive_fill;
  FillRect(renderer, rect, background);
  if (active) {
    const SDL_FRect accent =
        style.accent_edge == StripAccentEdge::Top
            ? SDL_FRect{rect.x, rect.y, rect.w, 2.0f}
            : SDL_FRect{rect.x, rect.y + rect.h - 2.0f, rect.w, 2.0f};
    FillRect(renderer, accent, theme.accent);
  }
  float text_left_padding = style.text_left_padding;
  if (show_badge && style.badge_size > 0.0f && !badge_text.empty()) {
    const float badge_y =
        rect.y + std::floor(std::max(0.0f, rect.h - style.badge_size) * 0.5f);
    const SDL_FRect badge_rect{rect.x + style.text_left_padding, badge_y, style.badge_size,
                               style.badge_size};
    FillRect(renderer, badge_rect, badge_color);
    OutlineRect(renderer, badge_rect, active ? palette.active_fill : palette.inactive_fill);
    const SDL_Color badge_text_color =
        render::RelativeLuminance(badge_color) > 0.45f ? theme.chrome_background
                                                       : theme.text_primary;
    const float badge_text_width = text_renderer.MeasureWidth(badge_text);
    const float badge_text_x =
        badge_rect.x + std::floor(std::max(0.0f, badge_rect.w - badge_text_width) * 0.5f);
    const float badge_text_y =
        badge_rect.y + std::floor(std::max(0.0f, badge_rect.h - text_renderer.LineHeight()) * 0.5f);
    text_renderer.DrawStringOn(renderer, badge_text_x, badge_text_y, badge_text_color,
                               badge_color, badge_text);
    text_left_padding += style.badge_size + style.badge_gap;
  }
  DrawVCenteredTextOn(
      text_renderer, renderer, rect, text_left_padding,
      active ? palette.active_text : palette.inactive_text, background,
      text_renderer.TruncateToWidth(
          label, std::max(8.0f, rect.w - style.close_right_reserve - (text_left_padding - style.text_left_padding))));
}

inline void DrawMenuRow(const render::TextRenderer& text_renderer,
                        SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& rect,
                        std::string_view label,
                        std::string_view accelerator,
                        bool enabled,
                        bool hovered,
                        bool checked) {
  const SDL_Color background = hovered && enabled ? theme.row_highlight : theme.overlay_background;
  const SDL_Color text_color =
      !enabled ? theme.text_disabled : hovered ? theme.text_primary : theme.text_secondary;
  const SDL_Color accel_color = !enabled ? theme.text_disabled : theme.text_muted;
  DrawSelectableRowBackground(renderer, theme, rect, theme.overlay_background, hovered && enabled);
  if (checked) {
    DrawCheckGlyph(renderer, MakeRect(rect.x + 8.0f, rect.y + 3.0f, 10.0f, rect.h - 6.0f),
                   enabled ? theme.accent : theme.text_disabled);
  }
  const float accelerator_width =
      accelerator.empty() ? 0.0f : text_renderer.MeasureWidth(accelerator);
  const float label_width = std::max(0.0f, rect.w - 42.0f - accelerator_width);
  DrawVCenteredTextOn(text_renderer, renderer,
                      MakeRect(rect.x + 24.0f, rect.y, label_width, rect.h), 0.0f, text_color,
                      background, text_renderer.TruncateToWidth(label, label_width));
  if (!accelerator.empty()) {
    DrawVCenteredTextOn(
        text_renderer, renderer,
        MakeRect(rect.x + rect.w - accelerator_width - 10.0f, rect.y, accelerator_width, rect.h),
        0.0f, accel_color, background, accelerator);
  }
}

}  // namespace microide::workspace::detail
