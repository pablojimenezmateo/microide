#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::render {

struct TextClipPadding {
  float left = 0.0f;
  float right = 0.0f;
  float top = 0.0f;
  float bottom = 0.0f;
};

// One positioned, single-colour text run. A row of decorated text is a sequence
// of these (same y, ascending x, non-overlapping). DrawRuns can submit a whole
// row's worth in one batched draw on backends that support it.
struct TextRun {
  float x = 0.0f;
  float y = 0.0f;
  SDL_Color color{};
  std::string_view text;
};

class TextRendererBackend {
 public:
  virtual ~TextRendererBackend() = default;

  virtual const char* Name() const = 0;
  virtual void SetPresentationScale(float scale_x, float scale_y) {
    (void) scale_x;
    (void) scale_y;
  }
  // Set the editor glyph point size. Default no-op: the debug-text fallback
  // renders at a fixed size and has no font to resize.
  virtual void SetFontPointSize(float points) { (void) points; }
  // Switch the primary editor font to the given family name (empty restores the
  // built-in default). Returns true when the font actually changed (so callers can
  // relayout). Default no-op: the debug-text fallback has no real font.
  virtual bool SetFontFamily(std::string_view family) {
    (void) family;
    return false;
  }
  // Enumerate the font families installed on the system, for the settings font
  // picker. Default empty: the debug-text fallback has no real fonts to list.
  virtual std::vector<std::string> AvailableFontFamilies() const { return {}; }
  virtual float CharWidth() const = 0;
  virtual float LineHeight() const = 0;
  virtual TextClipPadding ClipPadding() const { return {}; }
  virtual float MeasureWidth(std::string_view text) const = 0;
  // The width, when this backend can answer arithmetically rather than by
  // shaping — a monospaced backend answers a run of ASCII with one multiply.
  // `std::nullopt` means "shape it", which is what the caller memoizes.
  //
  // `TextRenderer` memoizes every measurement it makes, and a memo entry costs a
  // heap copy of the string plus a slot in a 4,096-entry LRU. Paying that to save
  // one multiply is a loss on its own; what makes it a bug is what arrives here.
  // The merge and compare surfaces measure every visible ROW through
  // `BuildDecoratedRow`, so scrolling a large diff inserted thousands of distinct
  // document rows into a cache sized for chrome labels and evicted every one of
  // them — 1,647 string copies in a single `merge_next_conflict_large_file`
  // phase. Returning the width here keeps document text out of the cache
  // entirely, and one call answers both "is it cheap" and "what is it" so the
  // string is scanned once.
  virtual std::optional<float> MeasureWidthIfCheap(std::string_view text) const {
    (void) text;
    return std::nullopt;
  }
  virtual void DrawString(SDL_Renderer* renderer,
                          float x,
                          float y,
                          SDL_Color color,
                          std::string_view text) = 0;
  virtual void DrawStringOn(SDL_Renderer* renderer,
                            float x,
                            float y,
                            SDL_Color color,
                            SDL_Color background,
                            std::string_view text) {
    (void) background;
    DrawString(renderer, x, y, color, text);
  }
  // Draw a sequence of positioned runs. The default is one DrawString per run;
  // backends may override to batch them into a single GPU submission. Runs are
  // co-planar and non-overlapping, so batching does not change visible output.
  virtual void DrawRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      if (!runs[i].text.empty()) {
        DrawString(renderer, runs[i].x, runs[i].y, runs[i].color, runs[i].text);
      }
    }
  }
  // Same contract as DrawRuns, plus a promise from the caller: this text is
  // EPHEMERAL — a distinct string per row that will not be drawn again (line
  // numbers are the case this exists for). A whole-string texture cache is worse
  // than useless for those. Every draw is a miss, so it pays a surface build and
  // a texture upload per row; and each miss then evicts a row-text entry that
  // WOULD have been reused, so the cost lands twice. Measured on the software
  // renderer, gutter line numbers were 74% of all text-texture work in the scroll
  // scenarios and 91% of misses evicted something.
  //
  // A backend that can position glyphs individually should draw these straight
  // from its coverage atlas and touch no cache. The default forwards to DrawRuns,
  // which is correct for any backend that cannot.
  virtual void DrawEphemeralRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) {
    DrawRuns(renderer, runs, count);
  }
};

}  // namespace microide::render
