#pragma once

#include <SDL3/SDL.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

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
  ~TextRenderer();

  void EnsureInitialized(SDL_Renderer* renderer,
                         float presentation_scale_x = 1.0f,
                         float presentation_scale_y = 1.0f);
  // Change the editor glyph point size at runtime (the project `editor.font_size`
  // setting). Forwards to the active backend and invalidates the width cache.
  void SetFontPointSize(float points);
  float CharWidth() const;
  float LineHeight() const;
  TextClipPadding ClipPadding() const;
  float MeasureWidth(std::string_view text) const;
  std::string_view BackendName() const;
  std::string TruncateToWidth(std::string_view text, float max_width) const;
  TextRendererCacheStats CacheStats() const;
  void ResetCacheStats() const;

  void DrawString(SDL_Renderer* renderer,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) const;
  // Draw a row's worth of positioned runs, batched into a single GPU submission
  // when the backend supports it (else one DrawString per run).
  void DrawRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) const;
  // True when DrawRuns actually batches (GPU atlas backend). Callers use it to
  // skip run-collection overhead on backends that draw inline anyway.
  bool BatchesRuns() const;
  void DrawStringOn(SDL_Renderer* renderer,
                    float x,
                    float y,
                    SDL_Color color,
                    SDL_Color background,
                    std::string_view text) const;

 private:
  void ClearWidthCache() const;
  void RememberMeasuredWidth(std::string text, float width) const;

  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
      return std::hash<std::string_view>{}(sv);
    }
  };

  std::unique_ptr<TextRendererBackend> backend_;
  bool attempted_optional_backend_ = false;
  // `width_cache_order_` stores std::string_view pointing at the map's keys. `unordered_map` does
  // not invalidate references to elements on rehash, so the views stay valid for the lifetime of
  // the entry. This avoids the per-entry duplicate std::string the previous deque held.
  mutable std::unordered_map<std::string, float, StringHash, std::equal_to<>> width_cache_;
  mutable std::deque<std::string_view> width_cache_order_;
  mutable std::string width_cache_backend_name_ = "debug";
  mutable float width_cache_scale_x_ = 1.0f;
  mutable float width_cache_scale_y_ = 1.0f;
  mutable bool width_cache_initialized_ = false;
  mutable std::size_t width_cache_queries_ = 0;
  mutable std::size_t width_cache_hits_ = 0;

#ifdef MICROIDE_TESTING
  friend struct ::microide::tests::TextRendererTestAccess;
#endif
};

}  // namespace microide::render
