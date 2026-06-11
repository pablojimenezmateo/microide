#include "workspace/CompareMergeRender.h"

#include <cmath>
#include <charconv>
#include <system_error>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kCollapsedContextButtonGap = 6.0f;
constexpr float kCollapsedContextButtonHeight = 16.0f;
constexpr float kCollapsedContextButtonHorizontalPadding = 12.0f;

float CollapsedContextButtonWidth(const render::TextRenderer& text_renderer,
                                  std::string_view label) {
  return std::max(48.0f, text_renderer.MeasureWidth(label) + kCollapsedContextButtonHorizontalPadding);
}

}  // namespace

std::string_view FormatLineNumber(std::size_t value, std::array<char, 20>& scratch) {
  const auto [end, ec] = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
  if (ec != std::errc{}) {
    return {};
  }
  return std::string_view(scratch.data(), static_cast<std::size_t>(end - scratch.data()));
}

void DrawScrollbarTrack(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& track) {
  if (renderer == nullptr || track.w <= 0.0f || track.h <= 0.0f) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, theme.surface_raised.r, theme.surface_raised.g,
                         theme.surface_raised.b, theme.surface_raised.a);
  SDL_RenderFillRect(renderer, &track);
}

void DrawScrollbarThumb(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& thumb,
                        bool active) {
  if (renderer == nullptr || thumb.w <= 0.0f || thumb.h <= 0.0f) {
    return;
  }
  // Match the shared workspace scrollbar resting tone so a diff scrollbar does not
  // look like a different widget than every other scrollbar in the shell.
  const SDL_Color thumb_color =
      active ? theme.accent : render::BlendColors(theme.text_muted, theme.surface_raised, 0.6f);
  SDL_SetRenderDrawColor(renderer, thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a);
  SDL_RenderFillRect(renderer, &thumb);
}

void DrawScrollbar(SDL_Renderer* renderer,
                   const render::Theme& theme,
                   const SDL_FRect& track,
                   const SDL_FRect& thumb,
                   bool active) {
  DrawScrollbarTrack(renderer, theme, track);
  DrawScrollbarThumb(renderer, theme, thumb, active);
}

CollapsedContextActionRects BuildCollapsedContextActionRects(
    const render::TextRenderer& text_renderer,
    const SDL_FRect& row_rect,
    bool show_previous,
    bool show_next) {
  const float center_y = row_rect.y + std::floor(std::max(0.0f, row_rect.h - kCollapsedContextButtonHeight) * 0.5f);
  const float all_width = CollapsedContextButtonWidth(text_renderer, "Show all");
  const float next_width = show_next ? CollapsedContextButtonWidth(text_renderer, "Show next 20") : 0.0f;
  const float previous_width =
      show_previous ? CollapsedContextButtonWidth(text_renderer, "Show previous 20") : 0.0f;

  float x = row_rect.x + row_rect.w - all_width;
  if (show_next) {
    x -= kCollapsedContextButtonGap + next_width;
  }
  if (show_previous) {
    x -= kCollapsedContextButtonGap + previous_width;
  }

  CollapsedContextActionRects rects;
  rects.text_right_edge = std::max(row_rect.x, x - 10.0f);
  if (show_previous) {
    rects.previous_rect = MakeRect(x, center_y, previous_width, kCollapsedContextButtonHeight);
    x += previous_width + kCollapsedContextButtonGap;
  }
  rects.all_rect = MakeRect(x, center_y, all_width, kCollapsedContextButtonHeight);
  x += all_width + kCollapsedContextButtonGap;
  if (show_next) {
    rects.next_rect = MakeRect(x, center_y, next_width, kCollapsedContextButtonHeight);
  }
  return rects;
}

}  // namespace microide::workspace
