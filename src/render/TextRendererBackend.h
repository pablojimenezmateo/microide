#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string_view>

namespace microide::render {

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
};

}  // namespace microide::render
