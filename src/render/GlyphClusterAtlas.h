#pragma once

#if MICROIDE_HAS_SDL3_TTF

#include "render/SdlSurface.h"
#include "util/TransparentStringHash.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace microide::render {

class GlyphClusterAtlas {
 public:
  static std::unique_ptr<GlyphClusterAtlas> Build(TTF_Font* font, SDL_PixelFormat surface_format,
                                                  int slot_width_px, int font_height_px);

  GlyphClusterAtlas(const GlyphClusterAtlas&) = delete;
  GlyphClusterAtlas& operator=(const GlyphClusterAtlas&) = delete;
  ~GlyphClusterAtlas() = default;

  bool BlitInto(SDL_Surface* dst, int dst_x, int span_px, std::string_view cluster,
                SDL_Color color);

  std::size_t ResidentClusters() const { return slots_.size(); }
  std::size_t SlotCapacity() const { return kMaxSlots; }

 private:
  static constexpr std::size_t kMaxSlots = 4096;
  static constexpr std::size_t kSlotsPerChunk = 256;

  struct Slot {
    int x = 0;
    int w = 0;
    int h = 0;
    bool cacheable = false;
  };

  GlyphClusterAtlas() = default;

  bool EnsureCapacityFor(std::size_t slot_index);
  const Slot* EnsureFilled(std::string_view cluster);
  static bool IsPureCoverage(const SDL_Surface* glyph);

  TTF_Font* font_ = nullptr;
  SurfacePtr atlas_;
  SDL_PixelFormat format_ = SDL_PIXELFORMAT_RGBA32;
  int font_height_px_ = 0;
  int slot_width_px_ = 0;
  std::size_t allocated_slots_ = 0;
  std::unordered_map<std::string, Slot, util::TransparentStringHash, std::equal_to<>>
      slots_;
};

}

#endif
