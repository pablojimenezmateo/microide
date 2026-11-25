#include "render/AsciiGlyphAtlas.h"

#include "render/GlyphCoverageBlit.h"
#include "render/GlyphSurfaceFormat.h"

#if MICROIDE_HAS_SDL3_TTF

#include <algorithm>
#include <cstdint>

namespace microide::render {

namespace {
constexpr SDL_Color kWhite{255, 255, 255, 255};
}

std::unique_ptr<AsciiGlyphAtlas> AsciiGlyphAtlas::Build(TTF_Font* font,
                                                        SDL_PixelFormat surface_format) {
  if (font == nullptr) {
    return nullptr;
  }

  const int font_height = std::max(1, TTF_GetFontHeight(font));

  int max_glyph_width = 1;
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    const char ch = static_cast<char>(kFirstChar + i);
    int w = 0;
    int h = 0;
    if (TTF_GetStringSize(font, &ch, 1, &w, &h)) {
      max_glyph_width = std::max(max_glyph_width, w);
    }
  }
  const int reserved = max_glyph_width + 2;

  auto atlas = std::unique_ptr<AsciiGlyphAtlas>(new AsciiGlyphAtlas());
  atlas->font_ = font;
  atlas->font_height_px_ = font_height;
  atlas->reserved_slot_width_ = reserved;
  atlas->atlas_.reset(SDL_CreateSurface(static_cast<int>(kSlotCount) * reserved, font_height,
                                        EnsureAlphaCapableFormat(surface_format)));
  if (atlas->atlas_ == nullptr) {
    return nullptr;
  }
  SDL_FillSurfaceRect(atlas->atlas_.get(), nullptr, 0);

  for (std::size_t i = 0; i < kSlotCount; ++i) {
    atlas->slots_[i].x = static_cast<int>(i) * reserved;
  }

  return atlas;
}

bool AsciiGlyphAtlas::Covers(char ch) const {
  const unsigned char uch = static_cast<unsigned char>(ch);
  return uch >= kFirstChar && uch <= kLastChar;
}

bool AsciiGlyphAtlas::EnsureSlotFilled(std::size_t index, char ch) {
  Slot& slot = slots_[index];
  if (slot.filled) {
    return true;
  }
  if (slot.failed) {
    return false;
  }

  const SurfacePtr glyph(TTF_RenderText_Blended(font_, &ch, 1, kWhite));
  if (glyph == nullptr) {
    slot.failed = true;
    return false;
  }

  SDL_SetSurfaceBlendMode(glyph.get(), SDL_BLENDMODE_NONE);
  const int w = std::min(glyph->w, reserved_slot_width_);
  const int h = std::min(glyph->h, font_height_px_);
  SDL_Rect src{0, 0, w, h};
  SDL_Rect dst{slot.x, 0, w, h};
  if (!SDL_BlitSurface(glyph.get(), &src, atlas_.get(), &dst)) {
    slot.failed = true;
    return false;
  }
  slot.w = w;
  slot.h = h;
  slot.filled = true;
  return true;
}

bool AsciiGlyphAtlas::EnsureAllSlotsFilled() {
  if (atlas_ == nullptr) {
    return false;
  }
  bool any = false;
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    if (EnsureSlotFilled(i, static_cast<char>(kFirstChar + i))) {
      any = true;
    }
  }
  return any;
}

bool AsciiGlyphAtlas::SlotRect(char ch, int* x, int* width, int* height) {
  if (atlas_ == nullptr) {
    return false;
  }
  const unsigned char uch = static_cast<unsigned char>(ch);
  if (uch < kFirstChar || uch > kLastChar) {
    return false;
  }
  const std::size_t index = uch - kFirstChar;
  if (!EnsureSlotFilled(index, ch)) {
    return false;
  }
  const Slot& slot = slots_[index];
  if (x != nullptr) {
    *x = slot.x;
  }
  if (width != nullptr) {
    *width = slot.w;
  }
  if (height != nullptr) {
    *height = slot.h;
  }
  return true;
}

bool AsciiGlyphAtlas::BlitInto(SDL_Surface* dst, int dst_x, char ch, SDL_Color color) {
  if (atlas_ == nullptr || dst == nullptr || color.a != 255) {
    return false;
  }
  const unsigned char uch = static_cast<unsigned char>(ch);
  if (uch < kFirstChar || uch > kLastChar) {
    return false;
  }
  const std::size_t index = uch - kFirstChar;
  if (!EnsureSlotFilled(index, ch)) {
    return false;
  }
  const Slot& slot = slots_[index];

  if (TintCopyCoverage(atlas_.get(), dst, dst_x, slot.x, slot.w, slot.h, color)) {
    return true;
  }

  SDL_SetSurfaceColorMod(atlas_.get(), color.r, color.g, color.b);
  SDL_SetSurfaceAlphaMod(atlas_.get(), 255);
  SDL_SetSurfaceBlendMode(atlas_.get(), SDL_BLENDMODE_NONE);

  SDL_Rect src{slot.x, 0, slot.w, slot.h};
  SDL_Rect dst_rect{dst_x, 0, slot.w, slot.h};
  return SDL_BlitSurface(atlas_.get(), &src, dst, &dst_rect);
}

}

#endif
