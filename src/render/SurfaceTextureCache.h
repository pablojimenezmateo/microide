#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "util/Waker.h"

namespace microide::project {
class ProjectBackgroundExecutor;
}

namespace microide::render {

class SurfaceTextureCache {
 public:
  enum class RasterFormat { Png, Rgba8 };

  static constexpr int kMaxDimension = 8192;
  static constexpr std::size_t kMaxEncodedBytes = 64u * 1024 * 1024;
  static constexpr std::size_t kDefaultVramBudgetBytes = 256u * 1024 * 1024;
  static constexpr std::size_t kMaxPendingDecodedBytes = 256u * 1024 * 1024;
  static constexpr std::size_t kMaxInFlightEncodedBytes = 128u * 1024 * 1024;

  explicit SurfaceTextureCache(project::ProjectBackgroundExecutor& executor,
                               std::size_t vram_budget_bytes = kDefaultVramBudgetBytes,
                               std::size_t pending_decoded_budget_bytes = kMaxPendingDecodedBytes,
                               std::size_t in_flight_encoded_budget_bytes = kMaxInFlightEncodedBytes);
  ~SurfaceTextureCache();

  SurfaceTextureCache(const SurfaceTextureCache&) = delete;
  SurfaceTextureCache& operator=(const SurfaceTextureCache&) = delete;

  void Request(std::uint64_t hash, RasterFormat format, std::vector<std::byte> bytes,
               int declared_w, int declared_h);

  void Upload(SDL_Renderer* renderer);

  struct Entry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
  };

  const Entry* Lookup(std::uint64_t hash);

  void Clear();

  struct RasterSize {
    std::uint64_t hash = 0;
    int width = 0;
    int height = 0;
  };

  void SetWakeChannel(util::WakeChannel channel);
  util::WakeChannel wake_channel() const { return wake_channel_; }

  std::vector<RasterSize> TakeDecodedSizes();
  std::optional<RasterSize> DecodedSize(std::uint64_t hash) const;

  std::uint64_t residency_generation() const { return residency_generation_; }

  std::size_t cached_count() const { return entries_.size(); }
  std::size_t failed_hash_count() const { return failed_hash_order_.size(); }

  using TextureFactoryForTesting = std::function<SDL_Texture*(SDL_Renderer*, int, int)>;
  void SetTextureFactoryForTesting(TextureFactoryForTesting factory) {
    texture_factory_for_testing_ = std::move(factory);
  }

  struct Decoded {
    std::uint64_t hash = 0;
    int width = 0;
    int height = 0;
    bool ok = false;
    bool dropped_over_budget = false;
    std::vector<std::uint8_t> rgba;
  };

  std::size_t pending_decoded_bytes() const;

  std::size_t in_flight_encoded_bytes() const;

 private:
  struct DecodeSink {
    std::mutex mutex;
    std::vector<Decoded> ready;
    std::size_t pending_bytes = 0;
    std::size_t budget_bytes = kMaxPendingDecodedBytes;
    std::size_t in_flight_encoded_bytes = 0;
    std::size_t encoded_budget_bytes = kMaxInFlightEncodedBytes;
    std::vector<RasterSize> decoded_sizes;
    util::WakeChannel wake_channel = 0;
  };

  void EvictToBudget();
  void RetainBoundedFailureMarker(std::uint64_t hash);

  project::ProjectBackgroundExecutor& executor_;
  std::shared_ptr<DecodeSink> sink_;
  std::size_t vram_budget_bytes_;
  std::size_t vram_bytes_ = 0;
  std::uint64_t residency_generation_ = 0;

  std::unordered_map<std::uint64_t, RasterSize> awaiting_upload_sizes_;
  util::WakeChannel wake_channel_ = 0;

  std::unordered_map<std::uint64_t, Entry> entries_;
  std::list<std::uint64_t> lru_;
  std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> lru_pos_;

  std::unordered_map<std::uint64_t, bool> in_flight_or_failed_;
  static constexpr std::size_t kMaxFailedHashes = 1024;
  std::deque<std::uint64_t> failed_hash_order_;
  std::unordered_map<std::uint64_t, int> texture_create_failures_;
  TextureFactoryForTesting texture_factory_for_testing_;
};

}
