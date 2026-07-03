#include "render/AsciiGlyphAtlas.h"

#if MICROIDE_HAS_SDL3_TTF

#include <algorithm>

namespace microide::render {

namespace {
constexpr SDL_Color kWhite{255, 255, 255, 255};
}  // namespace

std::unique_ptr<AsciiGlyphAtlas> AsciiGlyphAtlas::Build(TTF_Font* font) {
  if (font == nullptr) {
    return nullptr;
  }

  const int font_height = std::max(1, TTF_GetFontHeight(font));

  // Measure (without rasterizing) the widest printable glyph so every slot can
  // reserve a uniform cell that any glyph fits into. Lazy fill then rasterizes
  // individual glyphs into their slot on first use.
  int max_glyph_width = 1;
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    const char ch = static_cast<char>(kFirstChar + i);
    int w = 0;
    int h = 0;
    if (TTF_GetStringSize(font, &ch, 1, &w, &h)) {
      max_glyph_width = std::max(max_glyph_width, w);
    }
  }
  // A little headroom: rendered widths can exceed the measured extent by a pixel
  // for glyphs with horizontal overhang.
  const int reserved = max_glyph_width + 2;

  auto atlas = std::unique_ptr<AsciiGlyphAtlas>(new AsciiGlyphAtlas());
  atlas->font_ = font;
  atlas->font_height_px_ = font_height;
  atlas->reserved_slot_width_ = reserved;
  atlas->atlas_ = SDL_CreateSurface(static_cast<int>(kSlotCount) * reserved, font_height,
                                    SDL_PIXELFORMAT_RGBA32);
  if (atlas->atlas_ == nullptr) {
    return nullptr;
  }
  // Fully transparent; lazy glyph blits supply coverage as glyphs are first used.
  SDL_FillSurfaceRect(atlas->atlas_, nullptr, 0);

  for (std::size_t i = 0; i < kSlotCount; ++i) {
    atlas->slots_[i].x = static_cast<int>(i) * reserved;
  }

  return atlas;
}

AsciiGlyphAtlas::~AsciiGlyphAtlas() {
  if (atlas_ != nullptr) {
    SDL_DestroySurface(atlas_);
    atlas_ = nullptr;
  }
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

  SDL_Surface* glyph = TTF_RenderText_Blended(font_, &ch, 1, kWhite);
  if (glyph == nullptr) {
    slot.failed = true;
    return false;
  }

  // Straight-copy the white coverage into the reserved slot so per-pixel alpha
  // is preserved exactly (default blending would mix coverage with the zero
  // destination alpha and thin the glyph).
  SDL_SetSurfaceBlendMode(glyph, SDL_BLENDMODE_NONE);
  const int w = std::min(glyph->w, reserved_slot_width_);
  const int h = std::min(glyph->h, font_height_px_);
  SDL_Rect dst{slot.x, 0, w, h};
  const bool blitted = SDL_BlitSurface(glyph, nullptr, atlas_, &dst);
  SDL_DestroySurface(glyph);
  if (!blitted) {
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

bool AsciiGlyphAtlas::BeginTint(SDL_Color color) {
  if (atlas_ == nullptr || color.a != 255) {
    return false;
  }
  // Modulate the white coverage by the requested colour and straight-copy it.
  // For opaque colours this yields (color.rgb, coverage) — identical to
  // rendering the glyph directly at `color`, since 255 * c / 255 == c. Set once
  // per run; BlitGlyphInto reuses this state for every glyph.
  SDL_SetSurfaceColorMod(atlas_, color.r, color.g, color.b);
  SDL_SetSurfaceAlphaMod(atlas_, 255);
  SDL_SetSurfaceBlendMode(atlas_, SDL_BLENDMODE_NONE);
  return true;
}

bool AsciiGlyphAtlas::BlitGlyphInto(SDL_Surface* dst, int dst_x, char ch) {
  if (atlas_ == nullptr || dst == nullptr) {
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
  SDL_Rect src{slot.x, 0, slot.w, slot.h};
  SDL_Rect dst_rect{dst_x, 0, slot.w, slot.h};
  return SDL_BlitSurface(atlas_, &src, dst, &dst_rect);
}

bool AsciiGlyphAtlas::BlitInto(SDL_Surface* dst, int dst_x, char ch, SDL_Color color) {
  if (dst == nullptr || !BeginTint(color)) {
    return false;
  }
  return BlitGlyphInto(dst, dst_x, ch);
}

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
