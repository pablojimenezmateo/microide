#include "workspace/CompareMergeRender.h"

#include <charconv>
#include <system_error>

#include "render/Theme.h"

namespace microide::workspace {

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
  const SDL_Color thumb_color = active ? theme.accent : theme.text_disabled;
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

}  // namespace microide::workspace
