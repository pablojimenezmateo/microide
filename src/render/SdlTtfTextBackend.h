#pragma once

#include "render/TextRendererBackend.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
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
  struct CacheEntry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
  };

  struct GlyphEntry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    int minx = 0;
    int maxy = 0;
    bool loaded = false;
  };

  SdlTtfTextBackend() = default;

  bool Initialize(SDL_Renderer* renderer);
  void RefreshMetrics();
  void ClearCache();
  void ClearGlyphCache();
  static std::filesystem::path LocateFontFile();
  static std::vector<std::filesystem::path> LocateFallbackFontFiles(
      const std::filesystem::path& primary_font);
  void CloseFonts();
  void LoadFallbackFonts();
  bool CanUseFastAscii(std::string_view text) const;
  void DrawFastAsciiString(SDL_Renderer* renderer,
                           float x,
                           float y,
                           SDL_Color color,
                           const SDL_Color* background,
                           std::string_view text);
  GlyphEntry* ResolveGlyph(unsigned char ch);
  CacheEntry* ResolveEntry(std::string_view text,
                           SDL_Color color,
                           const SDL_Color* background);
  std::string BuildCacheKey(std::string_view text,
                            SDL_Color color,
                            const SDL_Color* background) const;

  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  std::vector<TTF_Font*> fallback_fonts_;
  std::filesystem::path font_path_;
  float char_width_ = 8.0f;
  float line_height_ = 14.0f;
  float font_ascent_pixels_ = 11.0f;
  float presentation_scale_x_ = 1.0f;
  float presentation_scale_y_ = 1.0f;
  bool ttf_initialized_ = false;
  std::unordered_map<std::string, CacheEntry> cache_;
  std::deque<std::string> cache_order_;
  std::array<GlyphEntry, 128> glyph_cache_{};
};

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
