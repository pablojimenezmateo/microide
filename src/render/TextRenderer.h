#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "render/TextBackendPool.h"
#include "render/TextRendererBackend.h"

namespace microide::tests {
struct TextRendererTestAccess;
}

namespace microide::render {

struct TextRendererCacheStats {
  std::size_t width_cache_queries = 0;
  std::size_t width_cache_hits = 0;
};

class TextRenderer {
 public:
  TextRenderer();
  explicit TextRenderer(std::shared_ptr<TextBackendPool> pool);
  ~TextRenderer();

  void EnsureInitialized(SDL_Renderer* renderer,
                         float presentation_scale_x = 1.0f,
                         float presentation_scale_y = 1.0f);
  bool SetFontPointSize(float points);
  bool SetFontFamily(std::string_view family);
  std::vector<std::string> AvailableFontFamilies() const;
  float CharWidth() const;
  float LineHeight() const;
  TextClipPadding ClipPadding() const;
  float MeasureWidth(std::string_view text) const;
  std::string_view BackendName() const;
  std::string TruncateToWidth(std::string_view text, float max_width) const;
  std::string_view TruncateToWidthEphemeralView(std::string_view text, float max_width) const;
  template <class Emit>
  void ForEachWrappedLine(std::string_view text, float max_width, Emit&& emit) const {
    if (text.empty()) {
      return;
    }
    std::size_t line_start = 0;
    std::size_t line_end = 0;
    std::size_t word_start = 0;
    while (word_start < text.size()) {
      const std::size_t space = text.find(' ', word_start);
      const std::size_t word_end = space == std::string_view::npos ? text.size() : space;
      const std::string_view candidate = text.substr(line_start, word_end - line_start);
      if (line_start != word_start && MeasureWidth(candidate) > max_width) {
        emit(text.substr(line_start, line_end - line_start));
        line_start = word_start;
      }
      line_end = word_end;
      word_start = space == std::string_view::npos ? text.size() : space + 1;
    }
    emit(text.substr(line_start, text.size() - line_start));
  }
  TextRendererCacheStats CacheStats() const;
  void ResetCacheStats() const;

  void DrawString(SDL_Renderer* renderer,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) const;
  void DrawRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) const;
  void DrawEphemeralRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) const;
  void NoteRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) const;
  void DrawStringOn(SDL_Renderer* renderer,
                    float x,
                    float y,
                    SDL_Color color,
                    SDL_Color background,
                    std::string_view text) const;

  const std::shared_ptr<TextBackendPool>& backend_pool() const { return pool_; }

  const TextRenderer& Face(FontStyle style) const;

  std::size_t MetricsGeneration() const {
    ValidateCaches();
    return metrics_generation_;
  }

 private:
  bool AcquireBackend();
  bool RequestFont(FontRequest font);
  void ValidateCaches() const;
  void ClearCaches() const;
  void RememberMeasuredWidth(std::string text, float width) const;

  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
      return std::hash<std::string_view>{}(sv);
    }
  };

  std::shared_ptr<TextBackendPool> pool_;
  mutable std::array<std::unique_ptr<TextRenderer>, kFontStyleCount - 1> faces_;
  std::shared_ptr<TextRendererBackend> backend_;
  FontRequest font_;
  bool initialized_ = false;
  mutable std::unordered_map<std::string, float, StringHash, std::equal_to<>> width_cache_;
  mutable std::deque<std::string_view> width_cache_order_;
  mutable StringTextureCache texture_cache_;
  mutable const TextRendererBackend* width_cache_backend_ = nullptr;
  mutable std::uint64_t width_cache_scale_epoch_ = 0;
  mutable std::size_t width_cache_queries_ = 0;
  mutable std::size_t width_cache_hits_ = 0;
  mutable std::size_t metrics_generation_ = 0;

  friend struct ::microide::tests::TextRendererTestAccess;
};

}
