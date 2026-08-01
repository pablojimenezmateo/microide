#pragma once

#include "render/AsciiGlyphAtlas.h"
#include "render/TextRendererBackend.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace microide::render {

class SdlTtfTextBackend final : public TextRendererBackend {
 public:
  static std::unique_ptr<SdlTtfTextBackend> Create(SDL_Renderer* renderer);
  ~SdlTtfTextBackend() override;

  const char* Name() const override { return "sdl3_ttf"; }
  bool BatchesRuns() const override { return glyph_atlas_enabled_ && is_gpu_renderer_; }
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
  void DrawString(SDL_Renderer* renderer,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) override;
  // Batches an entire row's runs into a single SDL_RenderGeometry call on the
  // GPU atlas path (per-vertex colour, so a multi-colour line is one submit);
  // falls back to the base per-run DrawString loop otherwise.
  void DrawRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) override;
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
    std::list<CacheKey>::iterator order;
  };

  SdlTtfTextBackend() = default;

  bool Initialize(SDL_Renderer* renderer);
  // Re-applies `font_point_size_` to the primary + fallback fonts at the current
  // presentation scale, then clears caches and refreshes metrics. Shared by the
  // presentation-scale and font-size entry points.
  void ApplyFontSizeAtCurrentScale();
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
  bool CanUseFastAscii(std::string_view text) const;
  void EnsureAsciiAtlas();
  SDL_Surface* BuildAsciiCompositeSurface(std::string_view text, SDL_Color color);
  CacheEntry* ResolveEntry(std::string_view text, SDL_Color color);
  // GPU-only batched-text path: upload the ASCII coverage atlas once and draw a
  // same-colour ASCII run as a single SDL_RenderGeometry call (per-vertex colour,
  // so colour never enters a cache key). Returns false to fall back to the
  // composite/whole-string path (non-GPU renderer, atlas opt-out, upload failed,
  // or a glyph outside the ASCII atlas). Gated by `glyph_atlas_enabled_` +
  // `is_gpu_renderer_`; see the glyph-atlas closeout guardrail for why this is
  // GPU-only (SDL_RenderGeometry regresses on the software renderer).
  bool EnsureGpuAtlas();
  // Append `text`'s ASCII glyph quads (positioned from x,y, tinted to `color`) to
  // the geometry scratch buffers, reproducing the composite path's device-pixel
  // positions. Returns false without appending the offending glyph if a glyph is
  // outside the atlas (caller falls back to the composite path for that run).
  bool AppendAsciiRunGeometry(float x, float y, SDL_Color color, std::string_view text);
  // Approximate VRAM footprint of a cached entry (RGBA texture: w * h * 4). Used
  // to bound the cache by bytes, not just entry count -- a few thousand wide
  // whole-string textures can pin far more VRAM than the entry cap implies.
  static std::size_t EntryByteCost(int width, int height);

  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  std::vector<TTF_Font*> fallback_fonts_;
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
  std::unordered_map<CacheKey, CacheEntry, CacheKeyHash, CacheKeyEqual> cache_;
  std::list<CacheKey> cache_order_;
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

  // GPU batched-text path state. `is_gpu_renderer_` is captured at init from the
  // SDL renderer driver; `glyph_atlas_enabled_` is the MICROIDE_RENDER_GLYPH_ATLAS
  // opt-in (default off). The texture is the ascii_atlas_ surface uploaded once;
  // it is destroyed alongside ascii_atlas_ in ClearCache (font-size change). The
  // scratch buffers are reused across runs to avoid per-run allocation.
  bool is_gpu_renderer_ = false;
  bool glyph_atlas_enabled_ = false;
  SDL_Texture* gpu_atlas_texture_ = nullptr;
  int gpu_atlas_width_ = 0;
  int gpu_atlas_height_ = 0;
  std::vector<SDL_Vertex> geom_vertices_;
  std::vector<int> geom_indices_;
};

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
