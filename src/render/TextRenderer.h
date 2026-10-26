#pragma once

#include <SDL3/SDL.h>

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

// Measures and draws text in one font. The glyph backend comes from a
// TextBackendPool, so renderers that share a pool and ask for the same font draw
// with one backend (one atlas, one glyph cache); a default-constructed renderer
// has a pool of its own.
class TextRenderer {
 public:
  TextRenderer();
  explicit TextRenderer(std::shared_ptr<TextBackendPool> pool);
  ~TextRenderer();

  void EnsureInitialized(SDL_Renderer* renderer,
                         float presentation_scale_x = 1.0f,
                         float presentation_scale_y = 1.0f);
  // Change the glyph point size at runtime (clamped to the backend's range).
  // Returns true when the glyphs actually changed; a no-op on an unchanged size.
  bool SetFontPointSize(float points);
  // Switch the font family at runtime (empty restores the platform default).
  // Returns true when the glyphs actually changed, so callers can relayout.
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
  // NEVER store the returned view (in a view model, member, or across a second
  // call) — the "Ephemeral" in the name is the contract. Use TruncateToWidth (owns
  // a std::string) for anything that outlives the immediate draw. (TD-2026-07-17-042.)
  std::string_view TruncateToWidthEphemeralView(std::string_view text, float max_width) const;
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
  // DrawRuns for text that will not repeat (line numbers). See
  // TextRendererBackend::DrawEphemeralRuns for why the distinction is worth an
  // entry point rather than a heuristic inside the cache.
  void DrawEphemeralRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) const;
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
  // The pool this renderer's backend comes from, to build a renderer that shares it.
  const std::shared_ptr<TextBackendPool>& backend_pool() const { return pool_; }

  std::size_t MetricsGeneration() const {
    ValidateCaches();
    return metrics_generation_;
  }

 private:
  // Asks the pool for the backend that draws `font_`; returns whether the glyphs changed.
  bool AcquireBackend();
  bool RequestFont(FontRequest font);
  // Drops both caches (widths and string textures) when they were filled by
  // another backend or at another scale. The scale is the pool's: a renderer
  // sharing a backend can see it rescaled by another renderer's EnsureInitialized.
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
  // Never null: the pool's stand-in until the first EnsureInitialized.
  std::shared_ptr<TextRendererBackend> backend_;
  // The font last asked for. It is what the pool resolves on the first
  // EnsureInitialized, so a size set before the first frame (`--set
  // editor.font_size`, a control spec) is the size that frame draws at.
  FontRequest font_;
  // Set by the first EnsureInitialized, which resolves `font_` once the pool is bound.
  bool initialized_ = false;
  // `width_cache_order_` stores std::string_view pointing at the map's keys. `unordered_map` does
  // not invalidate references to elements on rehash, so the views stay valid for the lifetime of
  // the entry. This avoids the per-entry duplicate std::string the previous deque held.
  mutable std::unordered_map<std::string, float, StringHash, std::equal_to<>> width_cache_;
  mutable std::deque<std::string_view> width_cache_order_;
  // This renderer's composites. Not the backend's: a backend is shared, and two
  // renderers' strings in one bounded cache evict each other (StringTextureCache).
  mutable StringTextureCache texture_cache_;
  // What both caches' entries were built with (see ValidateCaches).
  mutable const TextRendererBackend* width_cache_backend_ = nullptr;
  mutable std::uint64_t width_cache_scale_epoch_ = 0;
  mutable std::size_t width_cache_queries_ = 0;
  mutable std::size_t width_cache_hits_ = 0;
  // Bumped in ClearCaches (the single choke point for every glyph-change
  // invalidation: font size, font family, presentation scale, backend switch).
  mutable std::size_t metrics_generation_ = 0;

  friend struct ::microide::tests::TextRendererTestAccess;  // test seam
};

}  // namespace microide::render
