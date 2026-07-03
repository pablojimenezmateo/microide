#pragma once

#include "render/TextRendererBackend.h"

namespace microide::render {

class DebugTextBackend final : public TextRendererBackend {
 public:
  const char* Name() const override { return "debug"; }
  float CharWidth() const override { return kDebugCharWidth; }
  float LineHeight() const override { return kDebugLineHeight; }
  float MeasureWidth(std::string_view text) const override {
    return static_cast<float>(text.size()) * kDebugCharWidth;
  }
  // The debug backend is fixed-width, so every measurement is the O(1) product
  // that MeasureWidth returns — no case ever needs the cache.
  std::optional<float> TryMeasureFastWidth(std::string_view text) const override {
    return static_cast<float>(text.size()) * kDebugCharWidth;
  }
  void DrawString(SDL_Renderer* renderer,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) override;

 private:
  static constexpr float kDebugCharWidth = 8.0f;
  static constexpr float kDebugLineHeight = 14.0f;
};

}  // namespace microide::render
