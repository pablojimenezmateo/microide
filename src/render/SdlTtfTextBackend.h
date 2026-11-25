#pragma once

#include "render/AsciiGlyphAtlas.h"
#include "render/GlyphClusterAtlas.h"
#include "render/SdlSurface.h"
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

  SdlTtfTextBackend& operator=(const SdlTtfTextBackend&) = delete;

  const char* Name() const override { return "sdl3_ttf"; }
  void SetPresentationScale(float scale_x, float scale_y) override;
  void SetFontPointSize(float points) override;
  bool SetFontFamily(std::string_view family) override;
  bool SetFontStyle(FontStyle style) override;
  std::vector<std::string> AvailableFontFamilies() const override;
  static std::filesystem::path StyledFaceFile(const std::filesystem::path& regular, FontStyle style);
  static std::string FontDisplayNameFromStemForTesting(std::string_view stem);
  static std::filesystem::path FaceCoveringCodepointForTesting(char32_t codepoint);
  float CharWidth() const override { return char_width_; }
  float LineHeight() const override { return line_height_; }
  TextClipPadding ClipPadding() const override { return clip_padding_; }
  float MeasureWidth(std::string_view text) const override;
  std::optional<float> MeasureWidthIfCheap(std::string_view text) const override {
    if (font_ == nullptr || !CanUseFastAscii(text)) {
      return std::nullopt;
    }
    return static_cast<float>(text.size()) * char_width_;
  }
  void DrawString(SDL_Renderer* renderer,
                  StringTextureCache& cache,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) override;
  void DrawRuns(SDL_Renderer* renderer, StringTextureCache& cache, const TextRun* runs,
                std::size_t count) override;
  void DrawEphemeralRuns(SDL_Renderer* renderer, StringTextureCache& cache, const TextRun* runs,
                         std::size_t count) override;

 private:
  SdlTtfTextBackend() = default;

  bool Initialize(SDL_Renderer* renderer);
  void ApplyFontSizeAtCurrentScale();
  int CurrentHorizontalDpi() const;
  int CurrentVerticalDpi() const;
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
    int synthetic_style = 0;

    bool operator<(const MetricsKey& other) const {
      return std::tie(font_path, point_size, hdpi, vdpi, synthetic_style) <
             std::tie(other.font_path, other.point_size, other.hdpi, other.vdpi, other.synthetic_style);
    }
  };
  static PixelMetrics MeasurePixelMetrics(TTF_Font* font, const MetricsKey& key);
  void RefreshMetrics();
  void ClearGlyphAtlases();
  bool OpenPrimaryFont(const std::filesystem::path& path);
  static std::filesystem::path ResolveFamilyToFile(std::string_view family);
  static std::filesystem::path LocateFontFile();
  static std::vector<std::filesystem::path> FontSearchRoots();
  bool OpenFaceForStyle();
  static std::vector<std::filesystem::path> LocateFallbackFontFiles(
      const std::filesystem::path& primary_font);
  void CloseFonts();
  void CloseFallbackFonts();
  void LoadFallbackFonts();
  void EnsureCodepointCovered(char32_t codepoint) const;
  void EnsureFallbackFontsLoaded() const;
  bool CanUseFastAscii(std::string_view text) const;
  void EnsureAsciiAtlas();
  void EnsureClusterAtlas();
  SurfacePtr AcquireCompositeSurface(int width, int height);
  SurfacePtr BuildAsciiCompositeSurface(std::string_view text, SDL_Color color);
  SurfacePtr BuildGridCompositeSurface(std::string_view text, SDL_Color color);
  const StringTextureCache::Entry* ResolveEntry(StringTextureCache& cache, std::string_view text,
                                                SDL_Color color);
  bool EnsureAtlasTexture();
  int AsciiRunSurfaceWidthPx(std::size_t char_count) const;
  bool AppendAsciiRunGeometry(float x, float y, SDL_Color color, std::string_view text);
  bool DrawAsciiRunFromAtlas(float x, float y, SDL_Color color, std::string_view text);

  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  mutable std::vector<TTF_Font*> fallback_fonts_;
  mutable bool fallback_fonts_loaded_ = false;
  mutable std::vector<std::uint64_t> checked_codepoints_;
  std::filesystem::path font_path_;
  std::filesystem::path regular_font_path_;
  FontStyle font_style_ = FontStyle::Regular;
  int synthetic_style_ = 0;
  std::string requested_font_family_;
  float char_width_ = 8.0f;
  float line_height_ = 14.0f;
  TextClipPadding clip_padding_{};
  float font_point_size_ = kDefaultFontPointSize;
  float presentation_scale_x_ = 1.0f;
  float presentation_scale_y_ = 1.0f;
  bool ttf_initialized_ = false;

  SDL_PixelFormat texture_format_ = SDL_PIXELFORMAT_RGBA32;

  std::unique_ptr<AsciiGlyphAtlas> ascii_atlas_;

  std::unique_ptr<GlyphClusterAtlas> cluster_atlas_;

  bool is_gpu_renderer_ = false;
  bool glyph_atlas_enabled_ = false;
  SDL_Texture* atlas_texture_ = nullptr;
  int atlas_texture_width_ = 0;
  int atlas_texture_height_ = 0;
  static constexpr std::size_t kMaxCompositeScratchBytes = 4u * 1024 * 1024;
  std::vector<std::uint32_t> composite_scratch_;
  std::vector<SDL_Vertex> geom_vertices_;
  std::vector<int> geom_indices_;
};

}

#endif
