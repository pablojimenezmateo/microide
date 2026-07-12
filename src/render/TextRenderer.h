#pragma once

#include <SDL3/SDL.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
  // Switch the editor font family at runtime (the user `editor.font_family`
  // setting; empty restores the platform default). Returns true when the font
  // actually changed. Forwards to the backend and invalidates the width cache.
  bool SetFontFamily(std::string_view family);
  // Enumerate installed font families for the settings font picker. Forwards to
  // the active backend (empty on the debug-text fallback). This scans the system,
  // so callers should cache the result rather than calling it per frame.
  std::vector<std::string> AvailableFontFamilies() const;
  float CharWidth() const;
  float LineHeight() const;
  TextClipPadding ClipPadding() const;
  float MeasureWidth(std::string_view text) const;
  std::string_view BackendName() const;
  std::string TruncateToWidth(std::string_view text, float max_width) const;
  // Allocation-free variant for render hot paths: when `text` already fits it
  // returns `text` unchanged; when truncation is needed the returned view points
  // into a thread-local scratch that is overwritten by the NEXT call on the same
  // thread. Consume the view immediately (draw it before truncating again).
  std::string_view TruncateToWidthView(std::string_view text, float max_width) const;
  // Greedy word-wrap on spaces: invokes `emit(line)` for each wrapped line,
  // where every `line` is a view into the original `text` (no allocation).
  // A single word wider than `max_width` is emitted on its own line unbroken.
  // Header-inline so callers pay no indirection; measurement uses the cached
  // MeasureWidth. Shared by the settings/help overlays and any wrapped label.
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

  // Monotonic stamp bumped whenever glyph metrics change (font size/family or
  // presentation scale) — i.e. every time the internal width cache is invalidated.
  // External width caches keyed on MeasureWidth results (e.g. the menu-bar/popup
  // label caches) compare this to know when their entries have gone stale, since a
  // MeasureWidth value is only stable while metrics are unchanged.
  std::size_t MetricsGeneration() const { return metrics_generation_; }

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
  // Bumped in ClearWidthCache (the single choke point for every metrics-change
  // invalidation: font size, font family, presentation scale).
  mutable std::size_t metrics_generation_ = 0;

  friend struct ::microide::tests::TextRendererTestAccess;  // test seam
};

}  // namespace microide::render
