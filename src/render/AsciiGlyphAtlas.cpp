#include "render/AsciiGlyphAtlas.h"

#include "render/GlyphSurfaceFormat.h"

#if MICROIDE_HAS_SDL3_TTF

#include <algorithm>
#include <cstdint>

namespace microide::render {

namespace {
constexpr SDL_Color kWhite{255, 255, 255, 255};

// Straight-copy one glyph's coverage into `dst`, tinted to `color`, without going
// through SDL_BlitSurface.
//
// Worth its own path because of the CALL COUNT, not the pixel count: a rendered
// row string composites one of these per character, so a 200-column row is 200
// blitter dispatches back to back, and they measured 78.8 ms of the 84.5 ms that
// `editor_soft_wrap_long_line_scroll` spends rasterizing strings -- 93 %
// (TD-2026-08-30-276). Each individual glyph is ~8x17 pixels, far too small for
// SDL's per-blit setup to amortize.
//
// The arithmetic is an identity rather than an approximation, which is what makes
// this safe to substitute: the atlas stores white coverage (RGB = 255, A = the
// glyph's alpha), and modulating that by an opaque colour is
// `255 * c / 255 == c` in every channel. So the result is a CONSTANT rgb word
// carrying the source's alpha byte, and there is nothing to multiply per pixel.
//
// Returns false -- leaving `dst` untouched -- for anything this cannot serve
// exactly, and the caller falls back to SDL_BlitSurface.
bool TintCopyGlyph(SDL_Surface* atlas, SDL_Surface* dst, int dst_x, int src_x, int width,
                   int height, SDL_Color color) {
  if (atlas->format != dst->format || SDL_MUSTLOCK(atlas) || SDL_MUSTLOCK(dst)) {
    return false;
  }
  const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(atlas->format);
  if (details == nullptr || details->bytes_per_pixel != 4 || details->Amask == 0) {
    return false;
  }
  // Clip to the destination the way SDL_BlitSurface's own clipping would, so a
  // glyph overhanging the composite's right edge is truncated rather than wrapping
  // into the next row of pixels.
  width = std::min(width, dst->w - dst_x);
  height = std::min(height, dst->h);
  if (dst_x < 0 || width <= 0 || height <= 0) {
    return false;
  }

  const std::uint32_t rgb = (static_cast<std::uint32_t>(color.r) << details->Rshift) |
                            (static_cast<std::uint32_t>(color.g) << details->Gshift) |
                            (static_cast<std::uint32_t>(color.b) << details->Bshift);
  const std::uint32_t alpha_mask = details->Amask;
  const auto* src_pixels = static_cast<const std::uint8_t*>(atlas->pixels);
  auto* dst_pixels = static_cast<std::uint8_t*>(dst->pixels);
  if (src_pixels == nullptr || dst_pixels == nullptr) {
    return false;
  }
  for (int y = 0; y < height; ++y) {
    const auto* src_row = reinterpret_cast<const std::uint32_t*>(src_pixels +
                                                                 static_cast<std::size_t>(y) *
                                                                     static_cast<std::size_t>(
                                                                         atlas->pitch)) +
                          src_x;
    auto* dst_row = reinterpret_cast<std::uint32_t*>(dst_pixels +
                                                     static_cast<std::size_t>(y) *
                                                         static_cast<std::size_t>(dst->pitch)) +
                    dst_x;
    for (int x = 0; x < width; ++x) {
      dst_row[x] = rgb | (src_row[x] & alpha_mask);
    }
  }
  return true;
}
}  // namespace

std::unique_ptr<AsciiGlyphAtlas> AsciiGlyphAtlas::Build(TTF_Font* font,
                                                        SDL_PixelFormat surface_format) {
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
  // Every slot holds one glyph's coverage, and coverage IS the alpha channel --
  // an alpha-less surface silently turns each glyph into an opaque rectangle.
  // The caller already picks an alpha-capable format; enforce it here too,
  // because this class is the one that cannot function without it.
  atlas->atlas_ = SDL_CreateSurface(static_cast<int>(kSlotCount) * reserved, font_height,
                                    EnsureAlphaCapableFormat(surface_format));
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
  // Clip on the SOURCE rect. SDL_BlitSurface ignores dstrect's width/height (it
  // takes the size from the source), so passing a null srcrect made the clamps
  // above dead code: a rendered glyph wider than the measured advance + 2 px of
  // headroom — real for italic or overhanging glyphs — blitted straight over the
  // start of the NEXT slot, corrupting that character in the atlas.
  SDL_Rect src{0, 0, w, h};
  SDL_Rect dst{slot.x, 0, w, h};
  const bool blitted = SDL_BlitSurface(glyph, &src, atlas_, &dst);
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

  if (TintCopyGlyph(atlas_, dst, dst_x, slot.x, slot.w, slot.h, color)) {
    return true;
  }

  // Modulate the white coverage by the requested colour and straight-copy it.
  // For opaque colours this yields (color.rgb, coverage) — identical to
  // rendering the glyph directly at `color`, since 255 * c / 255 == c.
  SDL_SetSurfaceColorMod(atlas_, color.r, color.g, color.b);
  SDL_SetSurfaceAlphaMod(atlas_, 255);
  SDL_SetSurfaceBlendMode(atlas_, SDL_BLENDMODE_NONE);

  SDL_Rect src{slot.x, 0, slot.w, slot.h};
  SDL_Rect dst_rect{dst_x, 0, slot.w, slot.h};
  return SDL_BlitSurface(atlas_, &src, dst, &dst_rect);
}

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
