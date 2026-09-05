#pragma once

#include <optional>

#include "render/TextRendererBackend.h"
#include "util/StringUtil.h"

namespace microide::render {

class DebugTextBackend final : public TextRendererBackend {
 public:
  const char* Name() const override { return "debug"; }
  float CharWidth() const override { return kDebugCharWidth; }
  float LineHeight() const override { return kDebugLineHeight; }
  // Cells, not bytes, so a layout that positions by cells agrees with this
  // backend the way it agrees with the SDL_ttf one.
  float MeasureWidth(std::string_view text) const override {
    return static_cast<float>(util::GridCellCount(text)) * kDebugCharWidth;
  }
  // Every measurement here is one multiply, so nothing is worth memoizing.
  std::optional<float> MeasureWidthIfCheap(std::string_view text) const override {
    return MeasureWidth(text);
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
