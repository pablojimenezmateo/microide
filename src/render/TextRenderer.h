#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <string_view>

#include "render/TextRendererBackend.h"

namespace microide::render {

class TextRenderer {
 public:
  TextRenderer();
  ~TextRenderer();

  void EnsureInitialized(SDL_Renderer* renderer);
  float CharWidth() const;
  float LineHeight() const;
  float MeasureWidth(std::string_view text) const;
  std::string_view BackendName() const;
  std::string TruncateToWidth(std::string_view text, float max_width) const;

  void DrawString(SDL_Renderer* renderer,
                  float x,
                  float y,
                  SDL_Color color,
                  std::string_view text) const;
  void DrawStringOn(SDL_Renderer* renderer,
                    float x,
                    float y,
                    SDL_Color color,
                    SDL_Color background,
                    std::string_view text) const;

 private:
  std::unique_ptr<TextRendererBackend> backend_;
  bool attempted_optional_backend_ = false;
};

}  // namespace microide::render
