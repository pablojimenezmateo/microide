#include "render/GlyphClusterAtlas.h"

#include "render/GlyphCoverageBlit.h"
#include "render/GlyphSurfaceFormat.h"

#if MICROIDE_HAS_SDL3_TTF

#include <algorithm>
#include <utility>

namespace microide::render {

namespace {
constexpr SDL_Color kWhite{255, 255, 255, 255};
}

std::unique_ptr<GlyphClusterAtlas> GlyphClusterAtlas::Build(TTF_Font* font,
                                                            SDL_PixelFormat surface_format,
                                                            int slot_width_px,
                                                            int font_height_px) {
  if (font == nullptr || slot_width_px <= 0 || font_height_px <= 0) {
    return nullptr;
  }
  auto atlas = std::unique_ptr<GlyphClusterAtlas>(new GlyphClusterAtlas());
  atlas->font_ = font;
  atlas->format_ = EnsureAlphaCapableFormat(surface_format);
  atlas->font_height_px_ = font_height_px;
  atlas->slot_width_px_ = slot_width_px;
  if (!atlas->EnsureCapacityFor(0)) {
    return nullptr;
  }
  return atlas;
}

bool GlyphClusterAtlas::EnsureCapacityFor(std::size_t slot_index) {
  if (slot_index < allocated_slots_) {
    return true;
  }
  if (slot_index >= kMaxSlots) {
    return false;
  }
  const std::size_t wanted =
      std::min(kMaxSlots, ((slot_index / kSlotsPerChunk) + 1) * kSlotsPerChunk);
  const int width = static_cast<int>(wanted) * slot_width_px_;
  SurfacePtr grown(SDL_CreateSurface(width, font_height_px_, format_));
  if (grown == nullptr || !SDL_FillSurfaceRect(grown.get(), nullptr, 0)) {
    return false;
  }
  if (atlas_ != nullptr) {
    SDL_SetSurfaceBlendMode(atlas_.get(), SDL_BLENDMODE_NONE);
    SDL_Rect src{0, 0, atlas_->w, atlas_->h};
    SDL_Rect dst{0, 0, atlas_->w, atlas_->h};
    if (!SDL_BlitSurface(atlas_.get(), &src, grown.get(), &dst)) {
      return false;
    }
  }
  atlas_ = std::move(grown);
  allocated_slots_ = wanted;
  return true;
}

bool GlyphClusterAtlas::IsPureCoverage(const SDL_Surface* glyph) {
  if (glyph == nullptr || SDL_MUSTLOCK(glyph) || glyph->pixels == nullptr) {
    return false;
  }
  const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(glyph->format);
  if (details == nullptr || details->bytes_per_pixel != 4 || details->Amask == 0) {
    return false;
  }
  const std::uint32_t rgb_mask = details->Rmask | details->Gmask | details->Bmask;
  const auto* pixels = static_cast<const std::uint8_t*>(glyph->pixels);
  for (int y = 0; y < glyph->h; ++y) {
    const auto* row = reinterpret_cast<const std::uint32_t*>(
        pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(glyph->pitch));
    for (int x = 0; x < glyph->w; ++x) {
      const std::uint32_t pixel = row[x];
      if ((pixel & details->Amask) == 0) {
        continue;
      }
      if ((pixel & rgb_mask) != rgb_mask) {
        return false;
      }
    }
  }
  return true;
}

const GlyphClusterAtlas::Slot* GlyphClusterAtlas::EnsureFilled(std::string_view cluster) {
  if (atlas_ == nullptr || cluster.empty()) {
    return nullptr;
  }
  if (const auto it = slots_.find(cluster); it != slots_.end()) {
    return it->second.cacheable ? &it->second : nullptr;
  }
  const std::size_t slot_index = slots_.size();
  if (slot_index >= kMaxSlots || !EnsureCapacityFor(slot_index)) {
    return nullptr;
  }

  Slot slot;
  slot.x = static_cast<int>(slot_index) * slot_width_px_;

  const SurfacePtr glyph(TTF_RenderText_Blended(font_, cluster.data(), cluster.size(), kWhite));
  if (glyph != nullptr && IsPureCoverage(glyph.get())) {
    SDL_SetSurfaceBlendMode(glyph.get(), SDL_BLENDMODE_NONE);
    const int w = std::min(glyph->w, slot_width_px_);
    const int h = std::min(glyph->h, font_height_px_);
    SDL_Rect src{0, 0, w, h};
    SDL_Rect dst{slot.x, 0, w, h};
    if (w > 0 && h > 0 && SDL_BlitSurface(glyph.get(), &src, atlas_.get(), &dst)) {
      slot.w = w;
      slot.h = h;
      slot.cacheable = true;
    }
  }
  const auto [it, inserted] = slots_.emplace(std::string(cluster), slot);
  if (!inserted || !it->second.cacheable) {
    return nullptr;
  }
  return &it->second;
}

bool GlyphClusterAtlas::BlitInto(SDL_Surface* dst, int dst_x, int span_px,
                                 std::string_view cluster, SDL_Color color) {
  if (dst == nullptr || color.a != 255 || span_px <= 0) {
    return false;
  }
  const Slot* slot = EnsureFilled(cluster);
  if (slot == nullptr) {
    return false;
  }
  const int width = std::min(slot->w, span_px);
  if (width <= 0) {
    return false;
  }
  if (TintCopyCoverage(atlas_.get(), dst, dst_x, slot->x, width, slot->h, color)) {
    return true;
  }
  SDL_SetSurfaceColorMod(atlas_.get(), color.r, color.g, color.b);
  SDL_SetSurfaceAlphaMod(atlas_.get(), 255);
  SDL_SetSurfaceBlendMode(atlas_.get(), SDL_BLENDMODE_NONE);
  SDL_Rect src{slot->x, 0, width, slot->h};
  SDL_Rect dst_rect{dst_x, 0, width, slot->h};
  return SDL_BlitSurface(atlas_.get(), &src, dst, &dst_rect);
}

}

#endif
