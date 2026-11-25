#include "render/PluginDisplayListRenderer.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "render/ClipStableLine.h"

#include "render/PluginDisplayList.h"
#include "render/ScopedRenderClip.h"
#include "render/SurfacePrimitives.h"
#include "render/SurfaceTextureCache.h"
#include "render/TextRenderer.h"

namespace microide::render {

namespace {

SDL_Rect ToIntRect(const SDL_FRect& rect) {
  constexpr float kBound = 1'000'000.0f;
  const float x0 = std::floor(std::clamp(rect.x, -kBound, kBound));
  const float y0 = std::floor(std::clamp(rect.y, -kBound, kBound));
  const float x1 = std::ceil(std::clamp(rect.x + rect.w, -kBound, kBound));
  const float y1 = std::ceil(std::clamp(rect.y + rect.h, -kBound, kBound));
  return SDL_Rect{static_cast<int>(x0), static_cast<int>(y0),
                  static_cast<int>(std::max(0.0f, x1 - x0)),
                  static_cast<int>(std::max(0.0f, y1 - y0))};
}

SDL_Rect IntersectRect(const SDL_Rect& a, const SDL_Rect& b) {
  const int x0 = std::max(a.x, b.x);
  const int y0 = std::max(a.y, b.y);
  const int x1 = std::min(a.x + a.w, b.x + b.w);
  const int y1 = std::min(a.y + a.h, b.y + b.h);
  return SDL_Rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

}

void ReplayDisplayList(SDL_Renderer* renderer, TextRenderer& text_renderer,
                       SurfaceTextureCache& texture_cache, const PluginDisplayList& list,
                       const DisplayListReplayParams& params) {
  if (renderer == nullptr || list.ops.empty()) {
    return;
  }

  const float ox = params.origin_x;
  const float oy = params.origin_y;

  const bool had_clip = SDL_RenderClipEnabled(renderer);
  SDL_Rect previous_clip{};
  if (had_clip) {
    SDL_GetRenderClipRect(renderer, &previous_clip);
  }
  const SDL_Rect base_clip = NarrowToCurrentClip(renderer, ToIntRect(params.clip));
  thread_local std::vector<SDL_Rect> clip_stack;
  clip_stack.clear();
  clip_stack.push_back(base_clip);
  SDL_SetRenderClipRect(renderer, &base_clip);
  thread_local std::vector<SDL_FPoint> polyline_points;

  for (const DisplayOp& op : list.ops) {
    switch (op.op) {
      case DrawOp::Rect:
        FillRect(renderer, SDL_FRect{ox + op.rect.x, oy + op.rect.y, op.rect.w, op.rect.h},
                 op.color);
        break;
      case DrawOp::Line:
        SetDrawColor(renderer, op.color);
        render::RenderClipStableLine(renderer, ox + op.rect.x, oy + op.rect.y, ox + op.rect.w,
                       oy + op.rect.h);
        break;
      case DrawOp::Polyline: {
        SetDrawColor(renderer, op.color);
        polyline_points.clear();
        polyline_points.reserve(op.data_count);
        for (std::uint32_t i = 0; i < op.data_count; ++i) {
          const SDL_FPoint& p = list.point_arena[op.data_offset + i];
          polyline_points.push_back(SDL_FPoint{ox + p.x, oy + p.y});
        }
        DrawPolyline(renderer, polyline_points.data(), static_cast<int>(polyline_points.size()));
        break;
      }
      case DrawOp::Text: {
        const std::string_view text(list.text_arena.data() + op.data_offset, op.data_count);
        text_renderer.DrawString(renderer, ox + op.rect.x, oy + op.rect.y, op.color, text);
        break;
      }
      case DrawOp::Image: {
        const std::uint64_t hash = list.image_hashes[op.data_offset];
        if (const SurfaceTextureCache::Entry* entry = texture_cache.Lookup(hash);
            entry != nullptr && entry->texture != nullptr) {
          const SDL_FRect dest{ox + op.rect.x, oy + op.rect.y, op.rect.w, op.rect.h};
          DrawTexture(renderer, entry->texture, nullptr, dest);
        }
        break;
      }
      case DrawOp::ClipPush: {
        const SDL_Rect translated =
            ToIntRect(SDL_FRect{ox + op.rect.x, oy + op.rect.y, op.rect.w, op.rect.h});
        const SDL_Rect clipped = IntersectRect(clip_stack.back(), translated);
        clip_stack.push_back(clipped);
        SDL_SetRenderClipRect(renderer, &clipped);
        break;
      }
      case DrawOp::ClipPop:
        if (clip_stack.size() > 1) {
          clip_stack.pop_back();
          SDL_SetRenderClipRect(renderer, &clip_stack.back());
        }
        break;
    }
  }

  if (had_clip) {
    SDL_SetRenderClipRect(renderer, &previous_clip);
  } else {
    SDL_SetRenderClipRect(renderer, nullptr);
  }
}

}
