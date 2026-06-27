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
  void SetPresentationScale(float scale_x, float scale_y) override;
  float CharWidth() const override { return char_width_; }
  float LineHeight() const override { return line_height_; }
  TextClipPadding ClipPadding() const override { return clip_padding_; }
  float MeasureWidth(std::string_view text) const override;
  void DrawString(SDL_Renderer* renderer,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) override;
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
  void RefreshMetrics();
  void ClearCache();
  static std::filesystem::path LocateFontFile();
  static std::vector<std::filesystem::path> LocateFallbackFontFiles(
      const std::filesystem::path& primary_font);
  void CloseFonts();
  void LoadFallbackFonts();
  bool CanUseFastAscii(std::string_view text) const;
  void EnsureAsciiAtlas();
  SDL_Surface* BuildAsciiCompositeSurface(std::string_view text, SDL_Color color);
  CacheEntry* ResolveEntry(std::string_view text, SDL_Color color);
  // Approximate VRAM footprint of a cached entry (RGBA texture: w * h * 4). Used
  // to bound the cache by bytes, not just entry count -- a few thousand wide
  // whole-string textures can pin far more VRAM than the entry cap implies.
  static std::size_t EntryByteCost(int width, int height);

  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  std::vector<TTF_Font*> fallback_fonts_;
  std::filesystem::path font_path_;
  float char_width_ = 8.0f;
  float line_height_ = 14.0f;
  TextClipPadding clip_padding_{};
  float presentation_scale_x_ = 1.0f;
  float presentation_scale_y_ = 1.0f;
  bool ttf_initialized_ = false;
  std::unordered_map<CacheKey, CacheEntry, CacheKeyHash, CacheKeyEqual> cache_;
  std::list<CacheKey> cache_order_;
  // Running sum of EntryByteCost over every live cache_ entry; kept in lockstep
  // with cache_ so eviction can enforce a VRAM budget without rescanning.
  std::size_t cache_bytes_ = 0;

  // Colour-independent coverage atlas for ASCII composites. Built lazily on the
  // first ASCII miss and rebuilt (via ClearCache) when the font size changes.
  std::unique_ptr<AsciiGlyphAtlas> ascii_atlas_;
};

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
