#include "render/SurfaceTextureCache.h"

#include <cstring>
#include <utility>

#include "project/ProjectBackgroundExecutor.h"
#include "render/RasterDecode.h"

namespace microide::render {

namespace {

SurfaceTextureCache::Decoded DecodeEncoded(std::uint64_t hash,
                                           const std::vector<std::byte>& bytes) {
  SurfaceTextureCache::Decoded out;
  out.hash = hash;
  out.ok = DecodeRasterToRgba(std::span<const std::byte>(bytes.data(), bytes.size()), out.rgba,
                              out.width, out.height);
  return out;
}

SurfaceTextureCache::Decoded WrapRgba8(std::uint64_t hash, std::vector<std::byte> bytes,
                                       int declared_w, int declared_h) {
  SurfaceTextureCache::Decoded out;
  out.hash = hash;
  if (declared_w <= 0 || declared_h <= 0 || declared_w > SurfaceTextureCache::kMaxDimension ||
      declared_h > SurfaceTextureCache::kMaxDimension) {
    return out;
  }
  const std::size_t expected =
      static_cast<std::size_t>(declared_w) * static_cast<std::size_t>(declared_h) * 4;
  if (bytes.size() != expected) {
    return out;
  }
  out.rgba.resize(expected);
  std::memcpy(out.rgba.data(), bytes.data(), expected);
  out.width = declared_w;
  out.height = declared_h;
  out.ok = true;
  return out;
}

}

SurfaceTextureCache::SurfaceTextureCache(project::ProjectBackgroundExecutor& executor,
                                         std::size_t vram_budget_bytes,
                                         std::size_t pending_decoded_budget_bytes,
                                         std::size_t in_flight_encoded_budget_bytes)
    : executor_(executor),
      sink_(std::make_shared<DecodeSink>()),
      vram_budget_bytes_(vram_budget_bytes) {
  sink_->budget_bytes = pending_decoded_budget_bytes;
  sink_->encoded_budget_bytes = in_flight_encoded_budget_bytes;
}

SurfaceTextureCache::~SurfaceTextureCache() { Clear(); }

void SurfaceTextureCache::Request(std::uint64_t hash, RasterFormat format,
                                  std::vector<std::byte> bytes, int declared_w, int declared_h) {
  if (hash == 0) {
    return;
  }
  if (entries_.find(hash) != entries_.end() ||
      in_flight_or_failed_.find(hash) != in_flight_or_failed_.end()) {
    return;
  }

  const std::size_t encoded_size = bytes.size();
  {
    std::lock_guard<std::mutex> lock(sink_->mutex);
    if (encoded_size > sink_->encoded_budget_bytes - sink_->in_flight_encoded_bytes) {
      return;
    }
    sink_->in_flight_encoded_bytes += encoded_size;
  }
  in_flight_or_failed_.emplace(hash, true);

  std::shared_ptr<DecodeSink> sink = sink_;
  executor_.Post([sink = std::move(sink), hash, format, bytes = std::move(bytes), declared_w,
                  declared_h, encoded_size]() mutable {
    Decoded decoded = format == RasterFormat::Png
                          ? DecodeEncoded(hash, bytes)
                          : WrapRgba8(hash, std::move(bytes), declared_w, declared_h);
    util::WakeChannel wake = 0;
    {
      std::lock_guard<std::mutex> lock(sink->mutex);
      sink->in_flight_encoded_bytes -= encoded_size;
      if (decoded.ok) {
        sink->decoded_sizes.push_back(
            RasterSize{.hash = decoded.hash, .width = decoded.width, .height = decoded.height});
        wake = sink->wake_channel;
        const std::size_t decoded_bytes = decoded.rgba.size();
        if (decoded_bytes > sink->budget_bytes - sink->pending_bytes) {
          Decoded marker;
          marker.hash = decoded.hash;
          marker.ok = false;
          marker.dropped_over_budget = true;
          decoded = std::move(marker);
        } else {
          sink->pending_bytes += decoded_bytes;
        }
      }
      sink->ready.push_back(std::move(decoded));
    }
    (void) util::PushWake(wake);
  });
}

void SurfaceTextureCache::SetWakeChannel(util::WakeChannel channel) {
  wake_channel_ = channel;
  std::lock_guard<std::mutex> lock(sink_->mutex);
  sink_->wake_channel = channel;
}

std::vector<SurfaceTextureCache::RasterSize> SurfaceTextureCache::TakeDecodedSizes() {
  std::vector<RasterSize> sizes;
  {
    std::lock_guard<std::mutex> lock(sink_->mutex);
    sizes.swap(sink_->decoded_sizes);
  }
  for (const RasterSize& size : sizes) {
    if (entries_.find(size.hash) == entries_.end()) {
      awaiting_upload_sizes_[size.hash] = size;
    }
  }
  return sizes;
}

std::optional<SurfaceTextureCache::RasterSize> SurfaceTextureCache::DecodedSize(
    std::uint64_t hash) const {
  if (const auto it = entries_.find(hash); it != entries_.end()) {
    return RasterSize{.hash = hash, .width = it->second.width, .height = it->second.height};
  }
  if (const auto it = awaiting_upload_sizes_.find(hash); it != awaiting_upload_sizes_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void SurfaceTextureCache::Upload(SDL_Renderer* renderer) {
  std::vector<Decoded> ready;
  {
    std::lock_guard<std::mutex> lock(sink_->mutex);
    ready.swap(sink_->ready);
    sink_->pending_bytes = 0;
  }
  if (ready.empty()) {
    return;
  }
  for (Decoded& decoded : ready) {
    awaiting_upload_sizes_.erase(decoded.hash);
    const auto pending = in_flight_or_failed_.find(decoded.hash);
    if (pending == in_flight_or_failed_.end()) {
      continue;
    }
    if (decoded.dropped_over_budget) {
      in_flight_or_failed_.erase(pending);
      continue;
    }
    if (!decoded.ok) {
      RetainBoundedFailureMarker(decoded.hash);
      continue;
    }
    if (renderer == nullptr) {
      in_flight_or_failed_.erase(pending);
      continue;
    }

    SDL_Texture* texture =
        texture_factory_for_testing_
            ? texture_factory_for_testing_(renderer, decoded.width, decoded.height)
            : SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                decoded.width, decoded.height);
    if (texture == nullptr) {
      constexpr int kMaxTextureCreateRetries = 3;
      if (++texture_create_failures_[decoded.hash] < kMaxTextureCreateRetries) {
        in_flight_or_failed_.erase(pending);
      } else {
        RetainBoundedFailureMarker(decoded.hash);
      }
      continue;
    }
    texture_create_failures_.erase(decoded.hash);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(texture, nullptr, decoded.rgba.data(),
                      decoded.width * 4);

    in_flight_or_failed_.erase(pending);
    Entry entry{texture, decoded.width, decoded.height};
    entries_[decoded.hash] = entry;
    vram_bytes_ += static_cast<std::size_t>(decoded.width) *
                   static_cast<std::size_t>(decoded.height) * 4;
    lru_.push_front(decoded.hash);
    lru_pos_[decoded.hash] = lru_.begin();
    ++residency_generation_;
  }
  EvictToBudget();
}

std::size_t SurfaceTextureCache::pending_decoded_bytes() const {
  std::lock_guard<std::mutex> lock(sink_->mutex);
  return sink_->pending_bytes;
}

std::size_t SurfaceTextureCache::in_flight_encoded_bytes() const {
  std::lock_guard<std::mutex> lock(sink_->mutex);
  return sink_->in_flight_encoded_bytes;
}

const SurfaceTextureCache::Entry* SurfaceTextureCache::Lookup(std::uint64_t hash) {
  const auto it = entries_.find(hash);
  if (it == entries_.end()) {
    return nullptr;
  }
  const auto pos = lru_pos_.find(hash);
  if (pos != lru_pos_.end()) {
    lru_.erase(pos->second);
    lru_.push_front(hash);
    pos->second = lru_.begin();
  }
  return &it->second;
}

void SurfaceTextureCache::RetainBoundedFailureMarker(std::uint64_t hash) {
  failed_hash_order_.push_back(hash);
  while (failed_hash_order_.size() > kMaxFailedHashes) {
    const std::uint64_t oldest = failed_hash_order_.front();
    failed_hash_order_.pop_front();
    if (entries_.find(oldest) == entries_.end()) {
      in_flight_or_failed_.erase(oldest);
      texture_create_failures_.erase(oldest);
    }
  }
}

void SurfaceTextureCache::EvictToBudget() {
  while (lru_.size() > 1 && vram_bytes_ > vram_budget_bytes_) {
    const std::uint64_t victim = lru_.back();
    lru_.pop_back();
    lru_pos_.erase(victim);
    const auto it = entries_.find(victim);
    if (it != entries_.end()) {
      if (it->second.texture != nullptr) {
        SDL_DestroyTexture(it->second.texture);
      }
      vram_bytes_ -= static_cast<std::size_t>(it->second.width) *
                     static_cast<std::size_t>(it->second.height) * 4;
      entries_.erase(it);
      ++residency_generation_;
    }
    in_flight_or_failed_.erase(victim);
  }
}

void SurfaceTextureCache::Clear() {
  for (auto& [hash, entry] : entries_) {
    if (entry.texture != nullptr) {
      SDL_DestroyTexture(entry.texture);
    }
  }
  if (!entries_.empty()) {
    ++residency_generation_;
  }
  entries_.clear();
  lru_.clear();
  lru_pos_.clear();
  in_flight_or_failed_.clear();
  texture_create_failures_.clear();
  failed_hash_order_.clear();
  awaiting_upload_sizes_.clear();
  vram_bytes_ = 0;
}

}
