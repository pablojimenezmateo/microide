#pragma once

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <memory>

namespace microide::render {

// A single CPU-side coverage atlas for the printable ASCII range (0x20..0x7E).
//
// Glyphs are rasterized in white (RGB = 255, A = coverage) and packed into one
// contiguous SDL_Surface keyed implicitly by the font + size it was built
// against. A glyph's *shape* is colour-independent, so a single atlas serves
// every syntax-highlight colour: callers tint at blit time via colour
// modulation. This replaces the previous per-(char, colour) glyph-surface cache,
// whose key multiplied each glyph by every colour it appeared in and churned a
// fixed-size LRU under multi-colour highlighting.
//
// Slots are filled lazily on first use: only glyphs that are actually drawn get
// rasterized, so a cold paint never pays to rasterize the whole ASCII range (the
// behaviour the old per-glyph cache had). Each glyph is still rasterized at most
// once for the lifetime of the atlas, regardless of how many colours use it.
//
// This is the software-renderer-safe shape: it only changes how the composite
// string surface is assembled on a texture-cache miss. The runtime draw path
// stays exactly one SDL_RenderTexture call per cached (text, colour) string.
class AsciiGlyphAtlas {
 public:
  // Reserves the atlas surface and per-glyph layout for `font` at its current
  // size, without rasterizing any glyph yet. Returns nullptr if the font is null
  // or the backing surface cannot be allocated.
  static std::unique_ptr<AsciiGlyphAtlas> Build(TTF_Font* font);

  AsciiGlyphAtlas(const AsciiGlyphAtlas&) = delete;
  AsciiGlyphAtlas& operator=(const AsciiGlyphAtlas&) = delete;
  ~AsciiGlyphAtlas();

  // Pixel height shared by every glyph slot (the font height the atlas was
  // built at).
  int FontHeightPx() const { return font_height_px_; }

  // True when `ch` is in the printable ASCII range this atlas serves.
  bool Covers(char ch) const;

  // Straight-copies the glyph for `ch`, tinted to `color`, into `dst` with its
  // top-left at (dst_x, 0), rasterizing the glyph into the atlas on first use.
  // `color` must be opaque (a == 255); the coverage alpha is preserved so the
  // result is identical to rendering the glyph directly at `color`. Returns
  // false if `ch` is uncovered, the colour is translucent, or rasterization
  // fails.
  bool BlitInto(SDL_Surface* dst, int dst_x, char ch, SDL_Color color);

 private:
  static constexpr unsigned char kFirstChar = 0x20;
  static constexpr unsigned char kLastChar = 0x7E;
  static constexpr std::size_t kSlotCount = kLastChar - kFirstChar + 1;

  struct Slot {
    int x = 0;       // left edge of this glyph's reserved cell in the atlas
    int w = 0;       // rendered glyph width (valid once filled)
    int h = 0;       // rendered glyph height (valid once filled)
    bool filled = false;
    bool failed = false;  // rasterization was attempted and failed
  };

  AsciiGlyphAtlas() = default;

  // Rasterizes the glyph for slot `index` into the atlas if not already present.
  bool EnsureSlotFilled(std::size_t index, char ch);

  TTF_Font* font_ = nullptr;      // non-owning; outlives the atlas (reset on size change)
  SDL_Surface* atlas_ = nullptr;  // white coverage, height = font_height_px_
  int font_height_px_ = 0;
  int reserved_slot_width_ = 0;   // uniform per-slot cell width
  std::array<Slot, kSlotCount> slots_{};
};

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
