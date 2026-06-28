#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <string_view>

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
  virtual float CharWidth() const = 0;
  virtual float LineHeight() const = 0;
  virtual TextClipPadding ClipPadding() const { return {}; }
  virtual float MeasureWidth(std::string_view text) const = 0;
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
  // True when DrawRuns actually coalesces runs into a batched submission (the GPU
  // atlas path). Callers use this to decide whether collecting runs for a batched
  // flush is worthwhile; on a non-batching backend they keep the cheaper inline
  // draw and avoid the collection overhead.
  virtual bool BatchesRuns() const { return false; }
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
};

}  // namespace microide::render
