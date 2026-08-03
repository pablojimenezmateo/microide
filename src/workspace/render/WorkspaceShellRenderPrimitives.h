#pragma once

#include "render/SurfacePrimitives.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

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
  // Background for an inactive tab while the pointer is over it. Defaults (zero
  // alpha) fall back to inactive_fill so callers that never set it keep the old look.
  SDL_Color hover_fill{};
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
          .fill = render::BlendColors(theme.diff_deleted, theme.editor_background, 0.22f),
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
                  ? render::BlendColors(theme.diff_deleted, theme.text_secondary, 0.45f)
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
  text_renderer.DrawString(renderer, x, y, colors.text, label);
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

struct WrappedTooltipLayout {
  SDL_FRect rect{};
  std::vector<std::string> lines;
};

// Multi-line variant of BuildTooltipLayout: greedy word-wrap on spaces to a card
// no wider than max_width, sized to the wrapped line count. Use for longer help
// text (e.g. the settings scope chip) that a single-line tooltip would truncate.
inline WrappedTooltipLayout BuildWrappedTooltipLayout(const render::TextRenderer& text_renderer,
                                                      std::string_view label,
                                                      float max_width,
                                                      float min_width = 200.0f) {
  const float card_max = std::max(min_width, max_width);
  const float content_max = card_max - 16.0f;
  std::vector<std::string> lines;
  float widest = 0.0f;
  const auto emit = [&](std::string_view s) {
    lines.emplace_back(s);
    widest = std::max(widest, text_renderer.MeasureWidth(s));
  };
  std::size_t line_start = 0;
  std::size_t line_end = 0;
  std::size_t word_start = 0;
  while (word_start < label.size()) {
    const std::size_t space = label.find(' ', word_start);
    const std::size_t word_end = space == std::string_view::npos ? label.size() : space;
    const std::string_view candidate = label.substr(line_start, word_end - line_start);
    if (line_start != word_start && text_renderer.MeasureWidth(candidate) > content_max) {
      emit(label.substr(line_start, line_end - line_start));
      line_start = word_start;
    }
    line_end = word_end;
    word_start = space == std::string_view::npos ? label.size() : space + 1;
  }
  emit(label.substr(line_start, label.size() - line_start));
  const float width = std::clamp(widest + 16.0f, min_width, card_max);
  const float height = static_cast<float>(std::max<std::size_t>(1, lines.size())) *
                           text_renderer.LineHeight() +
                       10.0f;
  return WrappedTooltipLayout{
      .rect = SDL_FRect{0.0f, 0.0f, width, height},
      .lines = std::move(lines),
  };
}

inline void DrawWrappedTooltip(const render::TextRenderer& text_renderer,
                               SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& rect,
                               const std::vector<std::string>& lines) {
  render::DrawCardFrame(renderer, theme, rect, render::CardStyle::Tooltip);
  const SDL_Color background = render::CardBackground(theme, render::CardStyle::Tooltip);
  float y = rect.y + 5.0f;
  for (const std::string& line : lines) {
    text_renderer.DrawStringOn(renderer, rect.x + 8.0f, y, theme.text_primary, background, line);
    y += text_renderer.LineHeight();
  }
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

  // Resting thumbs need to read as grabbable at a glance; the prior 0.35 blend sat
  // almost flush with the track. Lift the resting contrast and keep accent for the
  // active drag so the grab gives a strong, distinct response.
  const SDL_Color thumb_color =
      active ? theme.accent : render::BlendColors(theme.text_muted, theme.surface_raised, 0.6f);
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
  render::SetDrawColor(renderer, color);
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

  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  SDL_RenderLine(renderer, cx - 3.0f, cy - 3.0f, cx + 3.0f, cy + 3.0f);
  SDL_RenderLine(renderer, cx + 3.0f, cy - 3.0f, cx - 3.0f, cy + 3.0f);
}

inline void DrawArrowGlyph(SDL_Renderer* renderer,
                           const SDL_FRect& rect,
                           bool up,
                           SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }

  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  if (up) {
    SDL_RenderLine(renderer, cx - 3.0f, cy + 2.0f, cx, cy - 2.0f);
    SDL_RenderLine(renderer, cx + 3.0f, cy + 2.0f, cx, cy - 2.0f);
  } else {
    SDL_RenderLine(renderer, cx - 3.0f, cy - 2.0f, cx, cy + 2.0f);
    SDL_RenderLine(renderer, cx + 3.0f, cy - 2.0f, cx, cy + 2.0f);
  }
}

// --- Debug-toolbar control glyphs ----------------------------------------------
// Simple line/scanline glyphs (matching the existing gutter/arrow style) for the
// floating debug control bar. Each is centered in its button rect. (DrawGlyphDot
// is defined further down; forward-declared here so these can use it.)

inline void DrawGlyphDot(SDL_Renderer* renderer, float cx, float cy, SDL_Color color);

// Stroke an arc `thickness` px wide.
//
// SDL's line renderer draws 1px, unantialiased. A curve built from a handful of
// long hairline segments therefore reads at icon size as a faceted scribble with
// gaps at the joins — which is exactly what the step-over and restart glyphs
// looked like. Walking the arc at roughly one sample per pixel and stamping a
// small square at each sample gives a continuous stroke with round joins for
// free, and batches into one SDL_RenderFillRects call.
inline void DrawArcStroke(SDL_Renderer* renderer, float cx, float cy, float radius,
                          float start_rad, float end_rad, SDL_Color color,
                          float thickness = 1.0f) {
  if (renderer == nullptr || radius <= 0.0f) {
    return;
  }
  const float sweep = std::fabs(end_rad - start_rad);
  // Two samples per pixel of arc length: the defect in the old glyphs was sample
  // DENSITY, not stroke weight. A handful of long chords shows its facets; enough
  // samples give a smooth curve at the same 1px weight as the straight-line glyphs
  // (Step Into/Out), which is the shell's line-art convention.
  const int samples = std::max(12, static_cast<int>(sweep * radius * 2.0f) + 2);
  constexpr int kMaxSamples = 320;
  const int count = std::min(samples, kMaxSamples);
  std::array<SDL_FRect, kMaxSamples> stamps{};
  for (int i = 0; i < count; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(count - 1);
    const float ang = start_rad + (end_rad - start_rad) * t;
    // Snap to the pixel grid so a 1px stroke stays crisp instead of smearing
    // across two rows at sub-pixel positions.
    stamps[static_cast<std::size_t>(i)] =
        SDL_FRect{std::floor(cx + radius * std::cos(ang)), std::floor(cy + radius * std::sin(ang)),
                  thickness, thickness};
  }
  render::SetDrawColor(renderer, color);
  SDL_RenderFillRects(renderer, stamps.data(), count);
}

// Right-pointing filled triangle (Continue / play).
inline void DrawPlayGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  const float base_x = std::floor(rect.x + rect.w * 0.38f);
  const float tip_x = std::floor(rect.x + rect.w * 0.66f);
  const float half_h = std::floor(rect.h * 0.22f);
  const int rows = static_cast<int>(half_h * 2.0f);
  for (int i = 0; i <= rows; ++i) {
    const float row_y = cy - half_h + static_cast<float>(i);
    const float t = half_h > 0.0f ? std::fabs(row_y - cy) / half_h : 0.0f;
    const float right = tip_x - (tip_x - base_x) * t;
    if (right <= base_x) {
      continue;
    }
    const SDL_FRect span{base_x, row_y, right - base_x, 1.0f};
    SDL_RenderFillRect(renderer, &span);
  }
}

// Two vertical bars (Pause).
inline void DrawPauseGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  const float bar_h = std::floor(rect.h * 0.34f);
  const SDL_FRect left{cx - 4.0f, cy - bar_h * 0.5f, 2.0f, bar_h};
  const SDL_FRect right{cx + 2.0f, cy - bar_h * 0.5f, 2.0f, bar_h};
  SDL_RenderFillRect(renderer, &left);
  SDL_RenderFillRect(renderer, &right);
}

// Filled square (Stop / terminate).
inline void DrawStopGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float side = std::floor(rect.h * 0.34f);
  const SDL_FRect square{std::floor(rect.x + (rect.w - side) * 0.5f),
                         std::floor(rect.y + (rect.h - side) * 0.5f), side, side};
  SDL_RenderFillRect(renderer, &square);
}

// Arc arrow over a dot (Step Over).
inline void DrawStepOverGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  // An arc hopping left-to-right over a dot, at the same 1px weight and with the
  // same two-line head as Step Into/Out. The old version drew the arc as ten long
  // chords, so it showed its facets and read as a scribble; the fix is sample
  // density, not a heavier stroke.
  constexpr float kPi = 3.14159265f;
  const float cx = std::floor(rect.x + rect.w * 0.5f) + 0.5f;
  const float cy = std::floor(rect.y + rect.h * 0.52f) + 0.5f;
  const float r = std::max(3.0f, rect.w * 0.26f);
  DrawArcStroke(renderer, cx, cy, r, kPi, 2.0f * kPi, color);
  const float tip_x = std::floor(cx + r);
  render::SetDrawColor(renderer, color);
  SDL_RenderLine(renderer, tip_x - 3.0f, cy - 3.0f, tip_x, cy);
  SDL_RenderLine(renderer, tip_x + 3.0f, cy - 3.0f, tip_x, cy);
  DrawGlyphDot(renderer, cx, cy + 4.0f, color);
}

// Down arrow into a dot (Step Into).
inline void DrawStepIntoGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float top = std::floor(rect.y + rect.h * 0.30f);
  const float tip = std::floor(rect.y + rect.h * 0.58f);
  SDL_RenderLine(renderer, cx, top, cx, tip);
  SDL_RenderLine(renderer, cx - 3.0f, tip - 3.0f, cx, tip);
  SDL_RenderLine(renderer, cx + 3.0f, tip - 3.0f, cx, tip);
  DrawGlyphDot(renderer, cx, std::floor(rect.y + rect.h * 0.76f), color);
}

// Up arrow out of a dot (Step Out).
inline void DrawStepOutGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float bottom = std::floor(rect.y + rect.h * 0.56f);
  const float tip = std::floor(rect.y + rect.h * 0.28f);
  SDL_RenderLine(renderer, cx, bottom, cx, tip);
  SDL_RenderLine(renderer, cx - 3.0f, tip + 3.0f, cx, tip);
  SDL_RenderLine(renderer, cx + 3.0f, tip + 3.0f, cx, tip);
  DrawGlyphDot(renderer, cx, std::floor(rect.y + rect.h * 0.76f), color);
}

// Open circular arrow (Restart).
inline void DrawRestartGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  // A 1px ring with a gap at the top-right and a two-line head, matching the other
  // toolbar glyphs. The old version stepped a full turn in sixteen chords, so it
  // drew a visibly lumpy polygon instead of a circle.
  constexpr float kPi = 3.14159265f;
  const float cx = std::floor(rect.x + rect.w * 0.5f) + 0.5f;
  const float cy = std::floor(rect.y + rect.h * 0.5f) + 0.5f;
  const float r = std::max(3.0f, rect.h * 0.28f);
  constexpr float kStart = -0.30f * kPi;
  DrawArcStroke(renderer, cx, cy, r, kStart, kStart + 1.62f * kPi, color);
  const float hx = std::floor(cx + r * std::cos(kStart));
  const float hy = std::floor(cy + r * std::sin(kStart));
  render::SetDrawColor(renderer, color);
  SDL_RenderLine(renderer, hx, hy, hx - 4.0f, hy - 1.0f);
  SDL_RenderLine(renderer, hx, hy, hx - 1.0f, hy + 4.0f);
}

// Left-pointing filled triangle (Reverse Continue) — the mirror of DrawPlayGlyph.
inline void DrawReverseContinueGlyph(SDL_Renderer* renderer, const SDL_FRect& rect,
                                     SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  const float base_x = std::floor(rect.x + rect.w * 0.62f);
  const float tip_x = std::floor(rect.x + rect.w * 0.34f);
  const float half_h = std::floor(rect.h * 0.22f);
  const int rows = static_cast<int>(half_h * 2.0f);
  for (int i = 0; i <= rows; ++i) {
    const float row_y = cy - half_h + static_cast<float>(i);
    const float t = half_h > 0.0f ? std::fabs(row_y - cy) / half_h : 0.0f;
    const float left = tip_x + (base_x - tip_x) * t;
    if (left >= base_x) {
      continue;
    }
    const SDL_FRect span{left, row_y, base_x - left, 1.0f};
    SDL_RenderFillRect(renderer, &span);
  }
}

// Mirror of DrawStepOverGlyph, head landing on the left (Step Back).
inline void DrawStepBackGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  constexpr float kPi = 3.14159265f;
  const float cx = std::floor(rect.x + rect.w * 0.5f) + 0.5f;
  const float cy = std::floor(rect.y + rect.h * 0.52f) + 0.5f;
  const float r = std::max(3.0f, rect.w * 0.26f);
  DrawArcStroke(renderer, cx, cy, r, kPi, 2.0f * kPi, color);
  const float tip_x = std::floor(cx - r);
  render::SetDrawColor(renderer, color);
  SDL_RenderLine(renderer, tip_x - 3.0f, cy - 3.0f, tip_x, cy);
  SDL_RenderLine(renderer, tip_x + 3.0f, cy - 3.0f, tip_x, cy);
  DrawGlyphDot(renderer, cx, cy + 4.0f, color);
}

inline void DrawCheckGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }

  // Proportional geometry at the shell's 1px line-art weight. The tick used to mix
  // fractions with a fixed -3px inset, which made it lopsided and clipped at the
  // bottom of the box; the weight was never the problem.
  render::SetDrawColor(renderer, color);
  const float left = std::floor(rect.x + rect.w * 0.22f);
  const float mid_x = std::floor(rect.x + rect.w * 0.43f);
  const float right = std::floor(rect.x + rect.w * 0.82f);
  const float mid_y = std::floor(rect.y + rect.h * 0.50f);
  const float low_y = std::floor(rect.y + rect.h * 0.70f);
  const float top_y = std::floor(rect.y + rect.h * 0.30f);
  SDL_RenderLine(renderer, left, mid_y, mid_x, low_y);
  SDL_RenderLine(renderer, mid_x, low_y, right, top_y);
}

inline void DrawPlusGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }

  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  SDL_RenderLine(renderer, cx - 4.0f, cy, cx + 4.0f, cy);
  SDL_RenderLine(renderer, cx, cy - 4.0f, cx, cy + 4.0f);
}

inline void DrawGlyphDot(SDL_Renderer* renderer, float cx, float cy, SDL_Color color) {
  render::SetDrawColor(renderer, color);
  const SDL_FRect dot{std::floor(cx) - 1.0f, std::floor(cy) - 1.0f, 2.0f, 2.0f};
  SDL_RenderFillRect(renderer, &dot);
}

// Sidebar mode-tab icons (line art, sized to match the existing glyphs above).
inline void DrawFolderGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  const SDL_FRect body{cx - 5.0f, cy - 2.0f, 10.0f, 7.0f};
  SDL_RenderRect(renderer, &body);
  // Folder tab on the top-left edge.
  SDL_RenderLine(renderer, cx - 5.0f, cy - 2.0f, cx - 3.0f, cy - 4.0f);
  SDL_RenderLine(renderer, cx - 3.0f, cy - 4.0f, cx, cy - 4.0f);
  SDL_RenderLine(renderer, cx, cy - 4.0f, cx, cy - 2.0f);
}

inline void DrawSearchGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  // Diamond lens (rounder than a square) plus a diagonal handle.
  const float lx = cx - 1.0f;
  const float ly = cy - 1.0f;
  SDL_RenderLine(renderer, lx, ly - 4.0f, lx + 4.0f, ly);
  SDL_RenderLine(renderer, lx + 4.0f, ly, lx, ly + 4.0f);
  SDL_RenderLine(renderer, lx, ly + 4.0f, lx - 4.0f, ly);
  SDL_RenderLine(renderer, lx - 4.0f, ly, lx, ly - 4.0f);
  SDL_RenderLine(renderer, lx + 2.0f, ly + 2.0f, cx + 5.0f, cy + 5.0f);
}

inline void DrawBranchGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  const float trunk_x = cx - 3.0f;
  // Trunk with two nodes, and a branch that forks up to a third node.
  SDL_RenderLine(renderer, trunk_x, cy - 5.0f, trunk_x, cy + 5.0f);
  SDL_RenderLine(renderer, trunk_x, cy + 1.0f, cx + 3.0f, cy - 3.0f);
  DrawGlyphDot(renderer, trunk_x, cy - 5.0f, color);
  DrawGlyphDot(renderer, trunk_x, cy + 5.0f, color);
  DrawGlyphDot(renderer, cx + 3.0f, cy - 4.0f, color);
}

inline void DrawEllipsisGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  DrawGlyphDot(renderer, cx - 4.0f, cy, color);
  DrawGlyphDot(renderer, cx, cy, color);
  DrawGlyphDot(renderer, cx + 4.0f, cy, color);
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

  render::SetDrawColor(renderer, color);
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

// Empty-state hint for a narrow rail (sidebar, debug pane). These strings are
// written to be actionable — "No breakpoints — click the editor gutter to add
// one." — which is roughly twice what a ~270px rail fits on one line, so drawing
// them flat either clipped at the panel edge or truncated away the half that
// told the user what to do. Greedy word-wrap instead, ellipsizing only if the
// line budget genuinely runs out.
//
// Allocation-free: the wrapped lines are views into `text` (which every caller
// owns for longer than the call), and only the final clipped line goes through
// the renderer's ephemeral truncation scratch — so `emit` must consume each line
// before asking for the next. Returns the number of lines emitted.
template <typename EmitFn>
inline std::size_t ForEachWrappedLabelLine(const render::TextRenderer& text_renderer,
                                           std::string_view text,
                                           float max_width,
                                           std::size_t max_lines,
                                           EmitFn&& emit) {
  if (text.empty() || max_width <= 0.0f || max_lines == 0) {
    return 0;
  }

  std::size_t emitted = 0;
  std::size_t line_start = 0;
  std::size_t line_end = 0;  // end of the longest prefix known to fit
  std::size_t word_start = 0;
  const auto push = [&](std::string_view line) {
    emit(emitted, line);
    ++emitted;
  };

  while (word_start < text.size()) {
    const std::size_t space = text.find(' ', word_start);
    const std::size_t word_end = space == std::string_view::npos ? text.size() : space;
    const std::string_view candidate = text.substr(line_start, word_end - line_start);
    if (line_start != word_start && text_renderer.MeasureWidth(candidate) > max_width) {
      if (emitted + 1 == max_lines) {
        // Last line allowed: spend it on as much of the remainder as fits.
        push(text_renderer.TruncateToWidthEphemeralView(text.substr(line_start), max_width));
        return emitted;
      }
      push(text.substr(line_start, line_end - line_start));
      line_start = word_start;
    }
    line_end = word_end;
    word_start = space == std::string_view::npos ? text.size() : space + 1;
  }
  push(text_renderer.TruncateToWidthEphemeralView(text.substr(line_start), max_width));
  return emitted;
}

inline std::size_t DrawWrappedPlaceholder(const render::TextRenderer& text_renderer,
                                          SDL_Renderer* renderer,
                                          float x,
                                          float y,
                                          float max_width,
                                          SDL_Color foreground,
                                          SDL_Color background,
                                          std::string_view text,
                                          std::size_t max_lines = 3) {
  const float line_height = text_renderer.LineHeight();
  return ForEachWrappedLabelLine(
      text_renderer, text, max_width, max_lines,
      [&](std::size_t index, std::string_view line) {
        text_renderer.DrawStringOn(renderer, x, y + static_cast<float>(index) * line_height,
                                   foreground, background, line);
      });
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

// One tab of a mode strip: the icon-plus-label chips that pick the sidebar view
// (Explorer / Search / Source Control / ...) and the debug pane's mode
// (Variables / Watch / Breakpoints). Both strips collapse to icon-only when the
// pane is too narrow for labels, which is what `label.empty()` selects.
//
// The two strips are laid out by different code and carry different tab types,
// but they are the same control and must keep looking like one: same neutral
// button tone, same 4px icon inset, same 16px icon column, same 1px gap before
// the label, same 4px trailing inset. Those five constants used to live twice.
//
// `draw_icon(icon_rect, color)` paints the glyph; each strip owns its own
// mode-to-glyph mapping.
template <typename DrawIcon>
inline void DrawModeTab(const render::TextRenderer& text_renderer,
                        SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& rect,
                        bool hovered,
                        bool active,
                        std::string_view label,
                        DrawIcon&& draw_icon) {
  const ButtonColors colors = ResolveButtonColors(
      theme, ButtonTone::Neutral,
      ButtonVisualState{.enabled = true, .hovered = hovered, .active = active});
  FillRect(renderer, rect, colors.fill);
  OutlineRect(renderer, rect, colors.border);
  if (label.empty()) {
    draw_icon(rect, colors.text);
    return;
  }
  const SDL_FRect icon_rect = MakeRect(rect.x + 4.0f, rect.y, 16.0f, rect.h);
  draw_icon(icon_rect, colors.text);
  const float label_x = icon_rect.x + icon_rect.w + 1.0f;
  DrawVCenteredTextOn(
      text_renderer, renderer,
      MakeRect(label_x, rect.y, std::max(0.0f, rect.x + rect.w - label_x - 4.0f), rect.h),
      0.0f, colors.text, colors.fill, label);
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
  // Ephemeral views, not owned strings: this runs once per visible row per frame in
  // the secondary sidebars and the debug pane, and the common case (a label that
  // fits) then costs no allocation at all instead of a std::string copy. The two
  // views never overlap — the primary is drawn and measured before the secondary
  // truncation overwrites the shared scratch.
  const std::string_view primary_text = text_renderer.TruncateToWidthEphemeralView(
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
             text_renderer.TruncateToWidthEphemeralView(secondary, secondary_width));
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
                         const StripTabPalette& palette,
                         bool hovered = false) {
  const SDL_Color inactive_background =
      (hovered && palette.hover_fill.a != 0) ? palette.hover_fill : palette.inactive_fill;
  const SDL_Color background = active ? palette.active_fill : inactive_background;
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
    OutlineRect(renderer, badge_rect, background);
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
  // Every visible tab in the project, editor and panel strips passes through here on
  // every frame, and most tab labels fit — so an owned truncation was a per-tab
  // per-frame std::string copy for nothing. The view is consumed by this one call.
  DrawVCenteredTextOn(
      text_renderer, renderer, rect, text_left_padding,
      active ? palette.active_text : palette.inactive_text, background,
      text_renderer.TruncateToWidthEphemeralView(
          label, std::max(8.0f, rect.w - style.close_right_reserve - (text_left_padding - style.text_left_padding))));
}

// Paints a chevron-glyph button with an optional hidden-tab count badge,
// matching the look of the workspace tab strip chrome. Used for the
// click-to-scroll overflow indicators at the strip ends (project, editor, and
// bottom-panel tab strips).
inline void DrawTabStripOverflowButton(const render::TextRenderer& text_renderer,
                                       SDL_Renderer* renderer,
                                       const render::Theme& theme,
                                       const SDL_FRect& rect,
                                       bool point_right,
                                       std::size_t hidden_count,
                                       bool hovered) {
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  const SDL_Color background = hovered ? theme.row_highlight : theme.surface_raised;
  const SDL_Color foreground = hovered ? theme.text_primary : theme.text_secondary;
  FillRect(renderer, rect, background);
  OutlineRect(renderer, rect, theme.border);

  const float cx = rect.x + 9.0f;
  const float cy = rect.y + rect.h * 0.5f;
  const float arm = std::max(3.0f, rect.h * 0.22f);
  render::SetDrawColor(renderer, foreground);
  if (point_right) {
    SDL_RenderLine(renderer, cx - arm * 0.5f, cy - arm, cx + arm * 0.5f, cy);
    SDL_RenderLine(renderer, cx + arm * 0.5f, cy, cx - arm * 0.5f, cy + arm);
  } else {
    SDL_RenderLine(renderer, cx + arm * 0.5f, cy - arm, cx - arm * 0.5f, cy);
    SDL_RenderLine(renderer, cx - arm * 0.5f, cy, cx + arm * 0.5f, cy + arm);
  }

  if (hidden_count > 0) {
    std::array<char, 24> count_buffer{};
    const auto conv = std::to_chars(count_buffer.data(),
                                    count_buffer.data() + count_buffer.size(), hidden_count);
    const std::string_view count_text(count_buffer.data(),
                                      static_cast<std::size_t>(conv.ptr - count_buffer.data()));
    const float count_x = rect.x + 15.0f;
    const float count_right_padding = 2.0f;
    const SDL_FRect count_rect{count_x, rect.y,
                               std::max(0.0f, rect.x + rect.w - count_x - count_right_padding),
                               rect.h};
    DrawVCenteredTextOn(text_renderer, renderer, count_rect, 0.0f, foreground, background,
                        count_text);
  }
}

// Renders the floating "lifted" ghost for an in-flight tab drag, shared by every
// tab strip. `tabs` are the visible tabs (model-space); the ghost is the tab
// whose index == `source_index`, offset to track under the pointer. The
// insertion point is shown by the gap the neighbor tabs slide open (Chrome-like),
// so no caret is drawn here. No allocations: the ghost reuses the visible tab's
// already-built title string_view.
inline void DrawTabDragFeedback(const render::TextRenderer& text_renderer,
                                SDL_Renderer* renderer,
                                const render::Theme& theme,
                                const SDL_FRect& strip,
                                const std::vector<VisibleStripTab>& tabs,
                                std::size_t source_index,
                                float pointer_x,
                                float grab_offset_x,
                                const StripTabStyle& style,
                                const StripTabPalette& palette) {
  if (tabs.empty()) {
    return;
  }

  // Floating ghost of the dragged tab (skipped when it scrolled off-screen).
  const VisibleStripTab* ghost = nullptr;
  for (const VisibleStripTab& tab : tabs) {
    if (tab.index == source_index) {
      ghost = &tab;
      break;
    }
  }
  if (ghost == nullptr) {
    return;
  }
  const float ghost_x =
      std::clamp(pointer_x - grab_offset_x, strip.x, strip.x + strip.w - ghost->rect.w);
  const SDL_FRect ghost_rect = MakeRect(ghost_x, ghost->rect.y, ghost->rect.w, ghost->rect.h);
  FillRect(renderer, MakeRect(ghost_rect.x + 1.0f, ghost_rect.y + 2.0f, ghost_rect.w, ghost_rect.h),
           render::BlendColors(theme.surface_background, SDL_Color{0, 0, 0, 255}, 0.5f));
  DrawStripTab(text_renderer, renderer, theme, ghost_rect, ghost->display_title, ghost->badge_text,
               ghost->badge_color, ghost->show_badge, /*active=*/true, style, palette);
  OutlineRect(renderer, ghost_rect, theme.accent);
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
  const SDL_Color background = hovered ? theme.row_highlight : theme.overlay_background;
  const SDL_Color text_color =
      !enabled ? theme.text_disabled : hovered ? theme.text_primary : theme.text_secondary;
  const SDL_Color accel_color = !enabled ? theme.text_disabled : theme.text_muted;
  DrawSelectableRowBackground(renderer, theme, rect, theme.overlay_background, hovered);
  if (checked) {
    DrawCheckGlyph(renderer, MakeRect(rect.x + 8.0f, rect.y + 3.0f, 10.0f, rect.h - 6.0f),
                   enabled ? theme.accent : theme.text_disabled);
  }
  const float accelerator_width =
      accelerator.empty() ? 0.0f : text_renderer.MeasureWidth(accelerator);
  const float label_width = std::max(0.0f, rect.w - 42.0f - accelerator_width);
  // Ephemeral view: consumed by this call, and the accelerator drawn below is a
  // separate borrowed view that does not touch the truncation scratch. An open menu
  // repaints on every pointer move, so this was an allocation per row per frame.
  DrawVCenteredTextOn(text_renderer, renderer,
                      MakeRect(rect.x + 24.0f, rect.y, label_width, rect.h), 0.0f, text_color,
                      background, text_renderer.TruncateToWidthEphemeralView(label, label_width));
  if (!accelerator.empty()) {
    DrawVCenteredTextOn(
        text_renderer, renderer,
        MakeRect(rect.x + rect.w - accelerator_width - 10.0f, rect.y, accelerator_width, rect.h),
        0.0f, accel_color, background, accelerator);
  }
}

}  // namespace microide::workspace::detail
