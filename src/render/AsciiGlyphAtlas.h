#pragma once

#if MICROIDE_HAS_SDL3_TTF

#include "render/SdlSurface.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <memory>

namespace microide::render {

class AsciiGlyphAtlas {
 public:
  static std::unique_ptr<AsciiGlyphAtlas> Build(TTF_Font* font,
                                                SDL_PixelFormat surface_format);

  AsciiGlyphAtlas(const AsciiGlyphAtlas&) = delete;
  AsciiGlyphAtlas& operator=(const AsciiGlyphAtlas&) = delete;
  ~AsciiGlyphAtlas() = default;

  int FontHeightPx() const { return font_height_px_; }

  bool Covers(char ch) const;

  bool BlitInto(SDL_Surface* dst, int dst_x, char ch, SDL_Color color);

  bool EnsureAllSlotsFilled();

  SDL_Surface* Surface() const { return atlas_.get(); }
  int SurfaceWidth() const { return atlas_ != nullptr ? atlas_->w : 0; }
  int SurfaceHeight() const { return font_height_px_; }

  bool SlotRect(char ch, int* x, int* width, int* height);

 private:
  static constexpr unsigned char kFirstChar = 0x20;
  static constexpr unsigned char kLastChar = 0x7E;
  static constexpr std::size_t kSlotCount = kLastChar - kFirstChar + 1;

  struct Slot {
    int x = 0;
    int w = 0;
    int h = 0;
    bool filled = false;
    bool failed = false;
  };

  AsciiGlyphAtlas() = default;

  bool EnsureSlotFilled(std::size_t index, char ch);

  TTF_Font* font_ = nullptr;
  SurfacePtr atlas_;
  int font_height_px_ = 0;
  int reserved_slot_width_ = 0;
  std::array<Slot, kSlotCount> slots_{};
};

}

#endif
