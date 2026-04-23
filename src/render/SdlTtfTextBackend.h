#pragma once

#include "render/TextRendererBackend.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3_ttf/SDL_ttf.h>

#include <array>
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
 void DrawStringOn(SDL_Renderer* renderer,
                    float x,
                    float y,
                    SDL_Color color,
                    SDL_Color background,
                    std::string_view text) override;

 private:
  struct CacheKeyView {
    std::string_view text;
    SDL_Color color{};
    bool has_background = false;
    SDL_Color background{};
  };

  struct CacheKey {
    std::string text;
    SDL_Color color{};
    bool has_background = false;
    SDL_Color background{};

    operator CacheKeyView() const noexcept {
      return CacheKeyView{
          .text = text,
          .color = color,
          .has_background = has_background,
          .background = background,
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
  CacheEntry* ResolveEntry(std::string_view text,
                           SDL_Color color,
                           const SDL_Color* background);

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
};

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
