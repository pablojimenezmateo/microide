#pragma once

#include <SDL3/SDL.h>

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "render/TextRendererBackend.h"

namespace microide::tests {
struct TextRendererTestAccess;
}

namespace microide::render {

class TextRenderer {
 public:
  TextRenderer();
  ~TextRenderer();

  void EnsureInitialized(SDL_Renderer* renderer,
                         float presentation_scale_x = 1.0f,
                         float presentation_scale_y = 1.0f);
  float CharWidth() const;
  float LineHeight() const;
  TextClipPadding ClipPadding() const;
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
  void ClearWidthCache() const;
  void RememberMeasuredWidth(std::string text, float width) const;

  std::unique_ptr<TextRendererBackend> backend_;
  bool attempted_optional_backend_ = false;
  mutable std::unordered_map<std::string, float> width_cache_;
  mutable std::deque<std::string> width_cache_order_;
  mutable std::string width_cache_backend_name_ = "debug";
  mutable float width_cache_scale_x_ = 1.0f;
  mutable float width_cache_scale_y_ = 1.0f;
  mutable bool width_cache_initialized_ = false;

#ifdef MICROIDE_TESTING
  friend struct ::microide::tests::TextRendererTestAccess;
#endif
};

}  // namespace microide::render
