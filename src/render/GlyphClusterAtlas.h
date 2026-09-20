#pragma once

#if MICROIDE_HAS_SDL3_TTF

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

// The non-ASCII counterpart of AsciiGlyphAtlas: a coverage cache keyed by a
// glyph CLUSTER's UTF-8 bytes (a base code point plus the zero-width marks that
// sit on it).
//
// Why it exists. `SdlTtfTextBackend::BuildGridCompositeSurface` lays a non-ASCII
// string out on the cell grid and, before this, rendered every cluster with its
// own `TTF_RenderText_Blended` — so a row of 40 CJK code points was 40 shaping +
// rasterization calls on every string-texture-cache miss, and a PageDown sweep
// through a CJK file measured 275,564 of them (~1,722 per painted frame) against
// 1 for the same sweep over ASCII (TD-2026-09-06-289a). The ASCII cells inside
// such a string already came from the atlas; the wide ones did not, and the
// distinct-cluster count of a real file is a few thousand at most while the
// cluster OCCURRENCE count is unbounded. Caching by cluster collapses the second
// number onto the first.
//
// The same identity AsciiGlyphAtlas relies on makes this exact rather than
// approximate: a glyph's shape is colour-independent, so one white-coverage
// raster (RGB = 255, A = coverage) modulated by an opaque colour reproduces a
// direct render at that colour byte for byte. Two cases break the identity and
// both are detected rather than assumed:
//
//  - a translucent colour — the caller is told no, and falls back;
//  - a COLOUR glyph (an emoji font's bitmap), whose pixels are not white where
//    they are opaque. `EnsureFilled` scans each newly rasterized cluster once and
//    marks it uncacheable, so such a cluster keeps taking the direct path
//    forever rather than being flattened to a tinted silhouette.
//
// Storage is a growable strip of uniform slots, like the ASCII atlas: one
// surface, so a hit is one straight copy out of memory the composite is about to
// write anyway. Slots are bounded (`kMaxSlots`); once the strip is full a cluster
// that is not resident is rasterized directly and not cached, which keeps a
// pathological document from pinning unbounded coverage. Real text does not get
// near the bound, and a bound that never evicts means a resident cluster's slot
// rect never moves, so nothing can dangle.
class GlyphClusterAtlas {
 public:
  // `slot_width_px` is the widest cluster this atlas will store whole — callers
  // pass the grid span of a double-width cell plus headroom. `font` must outlive
  // the atlas; the backend rebuilds the atlas whenever it reopens or resizes a
  // font, because coverage is only reusable at one size.
  static std::unique_ptr<GlyphClusterAtlas> Build(TTF_Font* font, SDL_PixelFormat surface_format,
                                                  int slot_width_px, int font_height_px);

  GlyphClusterAtlas(const GlyphClusterAtlas&) = delete;
  GlyphClusterAtlas& operator=(const GlyphClusterAtlas&) = delete;
  ~GlyphClusterAtlas();

  // Straight-copies `cluster`'s coverage, tinted to `color`, into `dst` with its
  // top-left at (dst_x, 0), clipped to `span_px` — rasterizing the cluster into
  // the atlas on first use.
  //
  // Returns false without touching `dst` when this atlas cannot serve the
  // cluster exactly: a translucent colour, a colour glyph, a cluster wider than
  // a slot, a full atlas, or a failed rasterization. The caller then renders the
  // cluster directly, which is what it did for every cluster before this existed.
  bool BlitInto(SDL_Surface* dst, int dst_x, int span_px, std::string_view cluster,
                SDL_Color color);

  // Resident cluster count and slot capacity, for tests and the cap's own
  // regression coverage.
  std::size_t ResidentClusters() const { return slots_.size(); }
  std::size_t SlotCapacity() const { return kMaxSlots; }

 private:
  // A few thousand distinct clusters covers any real document: CJK in common use
  // is ~3,000 characters, and a slot is one glyph's coverage (at a 10 px cell and
  // a 20 px line, ~1.6 KB), so the whole strip at the cap is a handful of MB. A
  // document that exceeds it keeps working at the pre-cache cost.
  static constexpr std::size_t kMaxSlots = 4096;
  // Rasterize into the strip in chunks rather than reserving the cap up front:
  // an ASCII-only session never allocates more than the first chunk, and a CJK
  // one grows a few times.
  static constexpr std::size_t kSlotsPerChunk = 256;

  struct Slot {
    int x = 0;  // left edge of this cluster's slot in the strip
    int w = 0;  // rasterized width, clamped to the slot
    int h = 0;  // rasterized height, clamped to the strip height
    bool cacheable = false;  // false => colour glyph or a failed raster; never blit it
  };

  GlyphClusterAtlas() = default;

  // Grows the strip so `slot_index` is addressable. Returns false when the
  // reallocation fails, which leaves the existing strip intact.
  bool EnsureCapacityFor(std::size_t slot_index);
  // Returns the slot for `cluster`, rasterizing it on first use. Returns nullptr
  // when the cluster cannot be stored; the returned pointer stays valid for the
  // atlas's lifetime (slots are never evicted or moved).
  const Slot* EnsureFilled(std::string_view cluster);
  // True when every opaque pixel of `glyph` is white, i.e. the raster is pure
  // coverage and the tint identity holds.
  static bool IsPureCoverage(const SDL_Surface* glyph);

  TTF_Font* font_ = nullptr;      // non-owning; outlives the atlas
  SDL_Surface* atlas_ = nullptr;  // white coverage, height = font_height_px_
  SDL_PixelFormat format_ = SDL_PIXELFORMAT_RGBA32;
  int font_height_px_ = 0;
  int slot_width_px_ = 0;
  std::size_t allocated_slots_ = 0;
  // Keyed by the cluster's own bytes. Clusters are short (one code point plus
  // any marks), so the key is its own storage and needs no interning.
  std::unordered_map<std::string, Slot, util::TransparentStringHash, std::equal_to<>>
      slots_;
};

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
