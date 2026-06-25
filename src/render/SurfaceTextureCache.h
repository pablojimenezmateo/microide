#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace microide::project {
class ProjectBackgroundExecutor;
}  // namespace microide::project

namespace microide::render {

// Host-owned cache of plugin-supplied raster surfaces, keyed by content hash.
//
// Plugins hand the host either an encoded PNG/JPEG or raw RGBA8 bytes via
// `ctx.surface.setRaster`. Decoding runs OFF the shell thread (on the project
// background executor); the resulting RGBA buffer is uploaded to an SDL_Texture
// ON the shell thread (SDL texture creation must happen on the render thread).
// Entries are evicted LRU once the total texture VRAM passes a byte budget.
//
// Lifetime: decode tasks capture a shared "sink" (not `this`), so the cache may
// be destroyed while a decode is still in flight — the in-flight task just drops
// its result into the sink, which outlives both. No network access: the cache
// only ever decodes plugin-provided local bytes.
class SurfaceTextureCache {
 public:
  enum class RasterFormat { Png, Rgba8 };

  // Hard caps. A surface above these is rejected at decode time (no texture).
  static constexpr int kMaxDimension = 8192;
  static constexpr std::size_t kMaxEncodedBytes = 64u * 1024 * 1024;  // 64 MiB
  static constexpr std::size_t kDefaultVramBudgetBytes = 256u * 1024 * 1024;

  explicit SurfaceTextureCache(project::ProjectBackgroundExecutor& executor,
                               std::size_t vram_budget_bytes = kDefaultVramBudgetBytes);
  ~SurfaceTextureCache();

  SurfaceTextureCache(const SurfaceTextureCache&) = delete;
  SurfaceTextureCache& operator=(const SurfaceTextureCache&) = delete;

  // Shell thread. Schedule a decode for `hash` if it is not already cached or
  // in flight. `bytes` is moved onto the worker. `declared_w/h` are only used to
  // validate the rgba8 byte length; PNG/JPEG dimensions come from the decoder.
  void Request(std::uint64_t hash, RasterFormat format, std::vector<std::byte> bytes,
               int declared_w, int declared_h);

  // Shell thread. Drain decoded buffers and upload them. Call once at the top of
  // the render pass, before any Lookup, with a live renderer.
  void Upload(SDL_Renderer* renderer);

  struct Entry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
  };

  // Shell thread. The uploaded texture for `hash`, or nullptr while the decode
  // is pending or after a decode failure. Touches LRU order on a hit.
  const Entry* Lookup(std::uint64_t hash);

  // Shell thread. Drop everything (textures destroyed). In-flight decodes still
  // complete into the sink but their results are ignored.
  void Clear();

  std::size_t cached_count() const { return entries_.size(); }
  std::size_t vram_bytes() const { return vram_bytes_; }

  // A decoded RGBA8 image waiting for shell-thread upload. Public so the
  // off-thread decode helpers can construct it.
  struct Decoded {
    std::uint64_t hash = 0;
    int width = 0;
    int height = 0;
    bool ok = false;
    std::vector<std::uint8_t> rgba;  // width*height*4, only when ok
  };

 private:
  // Cross-thread handoff. Held by shared_ptr so worker tasks never touch `this`.
  struct DecodeSink {
    std::mutex mutex;
    std::vector<Decoded> ready;
  };

  void EvictToBudget();

  project::ProjectBackgroundExecutor& executor_;
  std::shared_ptr<DecodeSink> sink_;
  std::size_t vram_budget_bytes_;
  std::size_t vram_bytes_ = 0;

  // hash -> uploaded texture. lru_ holds hashes most-recently-used at the front.
  std::unordered_map<std::uint64_t, Entry> entries_;
  std::list<std::uint64_t> lru_;
  std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> lru_pos_;

  // Hashes that are decoding or have permanently failed: suppresses re-requests.
  std::unordered_map<std::uint64_t, bool> in_flight_or_failed_;  // value unused
};

}  // namespace microide::render
