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

// The font a TextRenderer asks for. Two requests that compare equal draw the same
// glyphs, so they can be served by one backend. The size is clamped on the way in
// so that two out-of-range requests the backend would clamp alike also match.
struct FontRequest {
  std::string family;  // empty: the platform default
  float point_size = kDefaultFontPointSize;

  static float ClampPointSize(float points);
  bool operator==(const FontRequest&) const = default;
};

// The glyph backends of one window, shared between the TextRenderers that draw
// into it. The chrome, the editor and the terminal each have a renderer so that
// each can take its own font, but by default all three ask for the same family at
// the same size, and a backend per renderer filled one ASCII atlas and one glyph
// cache per renderer with identical glyphs (TD-2026-10-26-394). A pool hands every
// renderer that asks for the same FontRequest the same backend; a renderer that
// asks for a different font gets one of its own, and gives it up again when its
// font matches another renderer's.
//
// The pool does not own the backends: each renderer holds a shared_ptr, the pool
// a weak one, so a backend lives exactly as long as some renderer draws with it.
// That also makes "the only renderer using it" a use count, which is what lets a
// lone renderer resize its backend in place instead of opening a new one.
//
// Not thread-safe: the renderers that share a pool draw on one thread.
class TextBackendPool {
 public:
  using Factory = std::function<std::unique_ptr<TextRendererBackend>(SDL_Renderer* renderer)>;

  // SDL_ttf when the build has it and it initializes, the debug-text backend otherwise.
  TextBackendPool();
  explicit TextBackendPool(Factory factory);

  TextBackendPool(const TextBackendPool&) = delete;
  TextBackendPool& operator=(const TextBackendPool&) = delete;

  // The first call binds the pool to `renderer`; backends are created for it from
  // then on, and Acquire hands out a stand-in before it. Every call applies the
  // presentation scale to every live backend, and bumps ScaleEpoch when it moved.
  void Bind(SDL_Renderer* renderer, float scale_x, float scale_y);
  bool bound() const { return bound_; }
  // Changes whenever Bind changes the scale of the shared backends. A renderer
  // memoizing measurements compares it, because another renderer's Bind may be
  // the one that rescaled the backend they share.
  std::uint64_t ScaleEpoch() const { return scale_epoch_; }

  // Points `held` at a backend that draws `font` and returns whether its glyphs
  // differ from what `held` drew before. In order: a live backend already drawing
  // `font` is shared; else, when the caller is the only user of `held`, `held` is
  // changed in place; else a new backend is created. Before Bind, every caller gets
  // the same font-less stand-in.
  bool Acquire(const FontRequest& font, std::shared_ptr<TextRendererBackend>& held);

  // Live backends, for tests and diagnostics.
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

}  // namespace microide::render
