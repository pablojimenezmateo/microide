#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "render/TextRendererBackend.h"

namespace microide::render {

struct FontRequest {
  std::string family;
  float point_size = kDefaultFontPointSize;
  FontStyle style = FontStyle::Regular;

  static float ClampPointSize(float points);
  bool operator==(const FontRequest&) const = default;
};

class TextBackendPool {
 public:
  using Factory = std::function<std::unique_ptr<TextRendererBackend>(SDL_Renderer* renderer)>;

  TextBackendPool();
  explicit TextBackendPool(Factory factory);

  TextBackendPool(const TextBackendPool&) = delete;
  TextBackendPool& operator=(const TextBackendPool&) = delete;

  void Bind(SDL_Renderer* renderer, float scale_x, float scale_y);
  bool bound() const { return bound_; }
  std::uint64_t ScaleEpoch() const { return scale_epoch_; }

  bool Acquire(const FontRequest& font, std::shared_ptr<TextRendererBackend>& held);

  std::size_t LiveBackendCount() const;

 private:
  struct Entry {
    FontRequest font;
    std::weak_ptr<TextRendererBackend> backend;
  };

  Entry* FindEntry(const TextRendererBackend* backend);

  Factory factory_;
  std::shared_ptr<TextRendererBackend> unbound_stand_in_;
  std::vector<Entry> entries_;
  SDL_Renderer* renderer_ = nullptr;
  float scale_x_ = 1.0f;
  float scale_y_ = 1.0f;
  std::uint64_t scale_epoch_ = 0;
  bool bound_ = false;
};

}
