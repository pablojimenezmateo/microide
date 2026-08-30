#pragma once

#include "render/AsciiGlyphAtlas.h"
#include "render/TextRendererBackend.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace microide::render {

class SdlTtfTextBackend final : public TextRendererBackend {
 public:
  static std::unique_ptr<SdlTtfTextBackend> Create(SDL_Renderer* renderer);
  ~SdlTtfTextBackend() override;

  const char* Name() const override { return "sdl3_ttf"; }
  void SetPresentationScale(float scale_x, float scale_y) override;
  void SetFontPointSize(float points) override;
  bool SetFontFamily(std::string_view family) override;
  std::vector<std::string> AvailableFontFamilies() const override;
  // Test seam: derive the picker's display family name from a font file stem the
  // way the no-fontconfig fallback does (strips weight/style tails, splits words).
  static std::string FontDisplayNameFromStemForTesting(std::string_view stem);
  float CharWidth() const override { return char_width_; }
  float LineHeight() const override { return line_height_; }
  TextClipPadding ClipPadding() const override { return clip_padding_; }
  float MeasureWidth(std::string_view text) const override;
  // Monospaced: a run of printable ASCII measures as `size * char_width_`, which
  // is exactly the condition MeasureWidth's own fast path tests.
  std::optional<float> MeasureWidthIfCheap(std::string_view text) const override {
    if (font_ == nullptr || !CanUseFastAscii(text)) {
      return std::nullopt;
    }
    return static_cast<float>(text.size()) * char_width_;
  }
  void DrawString(SDL_Renderer* renderer,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) override;
  // Batches an entire row's runs into a single SDL_RenderGeometry call on the
  // GPU atlas path (per-vertex colour, so a multi-colour line is one submit);
  // falls back to the base per-run DrawString loop otherwise.
  void DrawRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) override;
  // Ephemeral runs never enter the string-texture cache. On the GPU renderer they
  // take the same batched-geometry path as DrawRuns; on the software renderer,
  // where SDL_RenderGeometry regresses, they are drawn one glyph at a time
  // straight from the atlas texture (colour-modulated coverage, so the pixels are
  // the composite path's pixels without building or uploading a composite).
  void DrawEphemeralRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) override;
  // DrawStringOn is intentionally not overridden: this backend does not paint a
  // glyph background, so the base-class default (which ignores `background` and
  // forwards to DrawString) is exactly the desired behaviour.

 private:
  struct CacheKeyView {
    std::string_view text;
    SDL_Color color{};
  };

  struct CacheKey {
    std::string text;
    SDL_Color color{};

    operator CacheKeyView() const noexcept {
      return CacheKeyView{
          .text = text,
          .color = color,
      };
    }
  };

  struct CacheKeyHash {
    using is_transparent = void;

    std::size_t operator()(const CacheKey& key) const noexcept {
      return (*this)(static_cast<CacheKeyView>(key));
    }

    std::size_t operator()(const CacheKeyView& key) const noexcept;
  };

  struct CacheKeyEqual {
    using is_transparent = void;

    bool operator()(const CacheKey& lhs, const CacheKey& rhs) const noexcept {
      return (*this)(static_cast<CacheKeyView>(lhs), static_cast<CacheKeyView>(rhs));
    }

    bool operator()(const CacheKey& lhs, const CacheKeyView& rhs) const noexcept {
      return (*this)(static_cast<CacheKeyView>(lhs), rhs);
    }

    bool operator()(const CacheKeyView& lhs, const CacheKey& rhs) const noexcept {
      return (*this)(lhs, static_cast<CacheKeyView>(rhs));
    }

    bool operator()(const CacheKeyView& lhs, const CacheKeyView& rhs) const noexcept;
  };

  struct CacheEntry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    // Intrusive LRU. The order used to be a `std::list<CacheKey>`, so every
    // insert allocated a list node AND a second copy of the key's string — two
    // allocations per cache MISS, on a path where scrolling through fresh
    // content misses on every row (TD-2026-08-14-224). `unordered_map` never
    // moves its elements, so links into them stay valid across rehash.
    const CacheKey* key = nullptr;  // back-pointer into the map, for eviction
    CacheEntry* lru_prev = nullptr;
    CacheEntry* lru_next = nullptr;
  };

  SdlTtfTextBackend() = default;

  bool Initialize(SDL_Renderer* renderer);
  // Re-applies `font_point_size_` to the primary + fallback fonts at the current
  // presentation scale, then clears caches and refreshes metrics. Shared by the
  // presentation-scale and font-size entry points.
  void ApplyFontSizeAtCurrentScale();
  // The DPI pair TTF_SetFontSizeDPI is driven with, derived from the current
  // presentation scale. A lazily-opened fallback has to be sized with the same
  // pair the primary already got, so this is shared rather than recomputed.
  int CurrentHorizontalDpi() const;
  int CurrentVerticalDpi() const;
  // Everything RefreshMetrics needs from the font itself, in device pixels and
  // before any presentation-scale division — so the cache below keys on exactly
  // what the numbers depend on.
  struct PixelMetrics {
    int font_height = 0;
    int max_left_padding = 0;
    int max_right_padding = 0;
    int max_advance = 0;
  };
  struct MetricsKey {
    std::string font_path;
    float point_size = 0.0f;
    int hdpi = 0;
    int vdpi = 0;

    bool operator<(const MetricsKey& other) const {
      return std::tie(font_path, point_size, hdpi, vdpi) <
             std::tie(other.font_path, other.point_size, other.hdpi, other.vdpi);
    }
  };
  static PixelMetrics MeasurePixelMetrics(TTF_Font* font, const MetricsKey& key);
  void RefreshMetrics();
  void ClearCache();
  // Open `path` as the primary font, applying hinting/kerning; on success closes
  // the previous primary font and adopts the new one. Returns false (leaving the
  // current font untouched) when the path fails to open. Shared by Initialize and
  // SetFontFamily so both go through the same setup.
  bool OpenPrimaryFont(const std::filesystem::path& path);
  // Resolve a font family name to a concrete file. Empty family -> LocateFontFile.
  // Uses fontconfig when available (compile-time gated), else a cached scan of the
  // standard font directories. Returns an empty path when nothing matches.
  static std::filesystem::path ResolveFamilyToFile(std::string_view family);
  static std::filesystem::path LocateFontFile();
  // Standard font directories searched for family resolution and enumeration
  // (user font dirs first, then the system trees). Shared by ResolveFamilyToFile
  // and AvailableFontFamilies so the scan-root list lives in one place.
  static std::vector<std::filesystem::path> FontSearchRoots();
  static std::vector<std::filesystem::path> LocateFallbackFontFiles(
      const std::filesystem::path& primary_font);
  void CloseFonts();
  // Clear and free just the fallback fonts (unregistering them from the primary
  // first). Called before reloading fallbacks on a family switch so the previous
  // set is freed rather than leaked, and by CloseFonts during teardown.
  void CloseFallbackFonts();
  void LoadFallbackFonts();
  // Open the broad-Unicode / symbol fallbacks the first time a caller actually
  // asks the primary font about a non-ASCII subject.
  //
  // The set is up to five whole TTFs (Noto Sans, Noto Sans Symbols 1+2, DejaVu
  // Sans, FreeSans) and opening them was ~60% of this backend's initialization,
  // paid on the frame the user is waiting for. Nothing they carry can affect an
  // ASCII subject: a fallback is only consulted for a code point the primary has
  // no glyph for, and the primary is a programming font with full ASCII coverage
  // (it is also the atlas the ASCII fast paths rasterize from directly, without
  // going through TTF shaping at all). So the load moves to the first shaped
  // non-ASCII string — MeasureWidth and the texture-cache miss below — which in a
  // session that never renders one never happens.
  //
  // `const` + mutable state because MeasureWidth is const and is one of those
  // two entry points.
  void EnsureFallbackFontsLoaded() const;
  bool CanUseFastAscii(std::string_view text) const;
  void EnsureAsciiAtlas();
  SDL_Surface* BuildAsciiCompositeSurface(std::string_view text, SDL_Color color);
  CacheEntry* ResolveEntry(std::string_view text, SDL_Color color);
  void LruUnlink(CacheEntry& entry);
  void LruPushBack(CacheEntry& entry);
  void LruTouch(CacheEntry& entry) {
    if (lru_tail_ != &entry) {
      LruUnlink(entry);
      LruPushBack(entry);
    }
  }
  // Upload the ASCII coverage atlas once, as one texture. Renderer-agnostic: the
  // texture is what both atlas draw paths sample, the batched SDL_RenderGeometry
  // one (GPU) and the per-glyph SDL_RenderTexture one (software). Returns false
  // when the atlas is unusable (atlas opt-out, upload failed) and the caller must
  // fall back to the composite/whole-string path.
  bool EnsureAtlasTexture();
  // Append `text`'s ASCII glyph quads (positioned from x,y, tinted to `color`) to
  // the geometry scratch buffers, reproducing the composite path's device-pixel
  // positions. Returns false without appending the offending glyph if a glyph is
  // outside the atlas (caller falls back to the composite path for that run).
  // Width in device pixels of the composite surface BuildAsciiCompositeSurface
  // would build for a run of `char_count` ASCII cells -- the right edge both atlas
  // paths clip glyph overhang against, so all three paths draw the same pixels.
  int AsciiRunSurfaceWidthPx(std::size_t char_count) const;
  bool AppendAsciiRunGeometry(float x, float y, SDL_Color color, std::string_view text);
  // Draw `text`'s ASCII glyphs one SDL_RenderTexture at a time from the atlas
  // texture, at the same device-pixel positions AppendAsciiRunGeometry and the
  // composite surface use. Returns false without drawing the offending glyph if a
  // glyph is outside the atlas (caller falls back to the composite path for that
  // run). This is the software-renderer counterpart of the geometry path: N small
  // blits, but no surface build, no texture upload, and no cache eviction.
  bool DrawAsciiRunFromAtlas(float x, float y, SDL_Color color, std::string_view text);
  // Approximate VRAM footprint of a cached entry (RGBA texture: w * h * 4). Used
  // to bound the cache by bytes, not just entry count -- a few thousand wide
  // whole-string textures can pin far more VRAM than the entry cap implies.
  static std::size_t EntryByteCost(int width, int height);

  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  mutable std::vector<TTF_Font*> fallback_fonts_;
  // False until EnsureFallbackFontsLoaded has run for the current primary font.
  // Reset by a primary-font change (SetFontFamily) so the next non-ASCII subject
  // re-resolves fallbacks against the new primary.
  mutable bool fallback_fonts_loaded_ = false;
  std::filesystem::path font_path_;
  // The requested editor font family (empty = platform default). Retained so a
  // later size / presentation-scale change re-resolves the same family.
  std::string requested_font_family_;
  float char_width_ = 8.0f;
  float line_height_ = 14.0f;
  TextClipPadding clip_padding_{};
  // Project-configurable glyph point size (the `editor.font_size` setting). The
  // default matches the setting/registry default; runtime changes route through
  // SetFontPointSize. Applied at the current presentation scale via TTF DPI.
  float font_point_size_ = 13.0f;
  float presentation_scale_x_ = 1.0f;
  float presentation_scale_y_ = 1.0f;
  bool ttf_initialized_ = false;
  using CacheMap = std::unordered_map<CacheKey, CacheEntry, CacheKeyHash, CacheKeyEqual>;
  CacheMap cache_;
  // Least / most recently used ends of the intrusive list threaded through
  // CacheEntry. Same shape as TextLayoutCache's visible-line LRU.
  CacheEntry* lru_head_ = nullptr;
  CacheEntry* lru_tail_ = nullptr;
  // Running sum of EntryByteCost over every live cache_ entry; kept in lockstep
  // with cache_ so eviction can enforce a VRAM budget without rescanning.
  std::size_t cache_bytes_ = 0;

  // The pixel format the renderer stores textures in, resolved at init. Glyph
  // composites (and the coverage atlas they are blitted from) are built in this
  // format so uploading one is a copy rather than a whole-bitmap channel
  // conversion. See Initialize.
  SDL_PixelFormat texture_format_ = SDL_PIXELFORMAT_RGBA32;

  // Colour-independent coverage atlas for ASCII composites. Built lazily on the
  // first ASCII miss and rebuilt (via ClearCache) when the font size changes.
  std::unique_ptr<AsciiGlyphAtlas> ascii_atlas_;

  // Atlas draw-path state. `is_gpu_renderer_` is captured at init from the SDL
  // renderer driver and selects between the two atlas paths (batched geometry vs
  // per-glyph blits); `glyph_atlas_enabled_` is the MICROIDE_RENDER_GLYPH_ATLAS
  // switch, on unless it is set to "0". The texture is the ascii_atlas_ surface
  // uploaded once; it is destroyed alongside ascii_atlas_ in ClearCache
  // (font-size change). The geometry scratch buffers are reused across runs to
  // avoid per-run allocation.
  bool is_gpu_renderer_ = false;
  bool glyph_atlas_enabled_ = false;
  SDL_Texture* atlas_texture_ = nullptr;
  int atlas_texture_width_ = 0;
  int atlas_texture_height_ = 0;
  std::vector<SDL_Vertex> geom_vertices_;
  std::vector<int> geom_indices_;
};

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
