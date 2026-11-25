#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/StringTextureCache.h"

namespace microide::render {

inline constexpr float kDefaultFontPointSize = 13.0f;
inline constexpr float kMinFontPointSize = 8.0f;
inline constexpr float kMaxFontPointSize = 32.0f;

struct TextClipPadding {
  float left = 0.0f;
  float right = 0.0f;
  float top = 0.0f;
  float bottom = 0.0f;
};

struct TextRun {
  float x = 0.0f;
  float y = 0.0f;
  SDL_Color color{};
  std::string_view text;
};

enum class FontStyle : std::uint8_t {
  Regular = 0,
  Bold = 1,
  Italic = 2,
  BoldItalic = 3,
};
inline constexpr std::size_t kFontStyleCount = 4;
inline constexpr FontStyle FontStyleFor(bool bold, bool italic) {
  return static_cast<FontStyle>((bold ? 1 : 0) | (italic ? 2 : 0));
}

class TextRendererBackend {
 public:
  virtual ~TextRendererBackend() = default;

  virtual const char* Name() const = 0;
  virtual void SetPresentationScale(float scale_x, float scale_y) {
    (void) scale_x;
    (void) scale_y;
  }
  virtual void SetFontPointSize(float points) { (void) points; }
  virtual bool SetFontFamily(std::string_view family) {
    (void) family;
    return false;
  }
  virtual bool SetFontStyle(FontStyle style) {
    (void) style;
    return false;
  }
  virtual std::vector<std::string> AvailableFontFamilies() const { return {}; }
  virtual float CharWidth() const = 0;
  virtual float LineHeight() const = 0;
  virtual TextClipPadding ClipPadding() const { return {}; }
  virtual float MeasureWidth(std::string_view text) const = 0;
  virtual std::optional<float> MeasureWidthIfCheap(std::string_view text) const {
    (void) text;
    return std::nullopt;
  }
  virtual void DrawString(SDL_Renderer* renderer,
                          StringTextureCache& cache,
                          float x,
                          float y,
                          SDL_Color color,
                          std::string_view text) = 0;
  virtual void DrawStringOn(SDL_Renderer* renderer,
                            StringTextureCache& cache,
                            float x,
                            float y,
                            SDL_Color color,
                            SDL_Color background,
                            std::string_view text) {
    (void) background;
    DrawString(renderer, cache, x, y, color, text);
  }
  virtual void DrawRuns(SDL_Renderer* renderer, StringTextureCache& cache, const TextRun* runs,
                        std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      if (!runs[i].text.empty()) {
        DrawString(renderer, cache, runs[i].x, runs[i].y, runs[i].color, runs[i].text);
      }
    }
  }
  virtual void DrawEphemeralRuns(SDL_Renderer* renderer, StringTextureCache& cache,
                                 const TextRun* runs, std::size_t count) {
    DrawRuns(renderer, cache, runs, count);
  }
};

}
