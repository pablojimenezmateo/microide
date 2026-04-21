#pragma once

#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace microide::workspace::detail {

inline void DrawScrollbarTrack(SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& track) {
  if (renderer == nullptr || track.w <= 0.0f || track.h <= 0.0f) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, theme.surface_raised.r, theme.surface_raised.g,
                         theme.surface_raised.b, theme.surface_raised.a);
  SDL_RenderFillRect(renderer, &track);
}

inline void DrawScrollbarThumb(SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& thumb,
                               bool active = false) {
  if (renderer == nullptr || thumb.w <= 0.0f || thumb.h <= 0.0f) {
    return;
  }

  const SDL_Color thumb_color = active ? theme.accent : theme.text_disabled;
  SDL_SetRenderDrawColor(renderer, thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a);
  SDL_RenderFillRect(renderer, &thumb);
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

}  // namespace microide::workspace::detail
