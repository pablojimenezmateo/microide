#include "render/SurfaceTextureCache.h"

#include <cstring>
#include <utility>

#include "project/ProjectBackgroundExecutor.h"
#include "render/RasterDecode.h"

namespace microide::render {

namespace {

// Decode encoded image bytes (PNG/JPEG) to RGBA8. Returns ok=false on any
// failure (malformed, oversize, OOM) without producing a partial buffer.
SurfaceTextureCache::Decoded DecodeEncoded(std::uint64_t hash,
                                           const std::vector<std::byte>& bytes) {
  SurfaceTextureCache::Decoded out;
  out.hash = hash;
  out.ok = DecodeRasterToRgba(std::span<const std::byte>(bytes.data(), bytes.size()), out.rgba,
                              out.width, out.height);
  return out;
}

// Validate (not decode) raw RGBA8 bytes whose dimensions the plugin declared.
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

}  // namespace

SurfaceTextureCache::SurfaceTextureCache(project::ProjectBackgroundExecutor& executor,
                                         std::size_t vram_budget_bytes,
                                         std::size_t pending_decoded_budget_bytes)
    : executor_(executor),
      sink_(std::make_shared<DecodeSink>()),
      vram_budget_bytes_(vram_budget_bytes) {
  sink_->budget_bytes = pending_decoded_budget_bytes;
}

SurfaceTextureCache::~SurfaceTextureCache() { Clear(); }

void SurfaceTextureCache::Request(std::uint64_t hash, RasterFormat format,
                                  std::vector<std::byte> bytes, int declared_w, int declared_h) {
  if (hash == 0) {
    return;
  }
  if (entries_.find(hash) != entries_.end() ||
      in_flight_or_failed_.find(hash) != in_flight_or_failed_.end()) {
    return;  // already cached, decoding, or known-bad
  }
  in_flight_or_failed_.emplace(hash, true);

  // The worker captures a copy of the sink shared_ptr and the bytes by value, and
  // never touches `this`, so the cache can outlive-or-predecease the task safely.
  std::shared_ptr<DecodeSink> sink = sink_;
  executor_.Post([sink = std::move(sink), hash, format, bytes = std::move(bytes), declared_w,
                  declared_h]() mutable {
    Decoded decoded = format == RasterFormat::Png
                          ? DecodeEncoded(hash, bytes)
                          : WrapRgba8(hash, std::move(bytes), declared_w, declared_h);
    std::lock_guard<std::mutex> lock(sink->mutex);
    // TD-2026-07-17-092: bound the pre-upload decoded-buffer backlog. If this valid
    // decode would push pending host memory over budget, drop its RGBA and hand
    // Upload a lightweight "over budget" marker so the in-flight marker is released
    // and the image can be re-decoded later when the sink has drained.
    if (decoded.ok) {
      const std::size_t decoded_bytes = decoded.rgba.size();
      if (decoded_bytes > sink->budget_bytes - sink->pending_bytes) {
        Decoded marker;
        marker.hash = decoded.hash;
        marker.ok = false;
        marker.dropped_over_budget = true;
        sink->ready.push_back(std::move(marker));
        return;
      }
      sink->pending_bytes += decoded_bytes;
    }
    sink->ready.push_back(std::move(decoded));
  });
}

void SurfaceTextureCache::Upload(SDL_Renderer* renderer) {
  std::vector<Decoded> ready;
  {
    std::lock_guard<std::mutex> lock(sink_->mutex);
    ready.swap(sink_->ready);
    sink_->pending_bytes = 0;  // the whole backlog is now owned by `ready`
  }
  if (ready.empty()) {
    return;
  }
  for (Decoded& decoded : ready) {
    // The hash may have been Clear()'d while decoding; only honor still-pending
    // requests. A failed decode drops the in-flight marker so we never retry, but
    // keeps it in the map so a later identical Request short-circuits.
    const auto pending = in_flight_or_failed_.find(decoded.hash);
    if (pending == in_flight_or_failed_.end()) {
      continue;
    }
    if (decoded.dropped_over_budget) {
      // Transient host-memory backpressure, NOT a decode failure: drop the marker
      // so a later Request re-decodes when the pre-upload backlog has drained.
      in_flight_or_failed_.erase(pending);
      continue;
    }
    if (!decoded.ok) {
      // Permanent failure: same content hash → correct never to retry. Keep the
      // marker to short-circuit re-requests, but bound the retained-failure set so
      // a plugin streaming distinct invalid hashes cannot grow it without limit
      // (TD-2026-07-17-043). Evict the oldest failure once over the cap.
      failed_hash_order_.push_back(decoded.hash);
      while (failed_hash_order_.size() > kMaxFailedHashes) {
        const std::uint64_t oldest = failed_hash_order_.front();
        failed_hash_order_.pop_front();
        // Only forget it if it is still a failure marker (not resurrected as a live
        // entry or re-requested in-flight since).
        if (entries_.find(oldest) == entries_.end()) {
          in_flight_or_failed_.erase(oldest);
          texture_create_failures_.erase(oldest);
        }
      }
      continue;
    }
    if (renderer == nullptr) {
      // Transient, NOT a decode failure: a valid image arrived without a live
      // renderer (e.g. mid renderer re-create). Drop the marker so a later Upload
      // with a renderer re-decodes and displays it, instead of permanently
      // suppressing a good image until Clear().
      in_flight_or_failed_.erase(pending);
      continue;
    }

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC, decoded.width,
                                             decoded.height);
    if (texture == nullptr) {
      // The bytes decoded fine; only texture creation failed. Treat a bounded
      // number of failures as transient (a renderer re-create / momentary VRAM
      // pressure) and drop the marker so a later Upload retries — mirroring the
      // renderer==nullptr path above. Past the cap, keep the marker so a
      // permanently un-creatable texture stops re-decoding every request.
      constexpr int kMaxTextureCreateRetries = 3;
      if (++texture_create_failures_[decoded.hash] < kMaxTextureCreateRetries) {
        in_flight_or_failed_.erase(pending);
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
  }
  EvictToBudget();
}

std::size_t SurfaceTextureCache::pending_decoded_bytes() const {
  std::lock_guard<std::mutex> lock(sink_->mutex);
  return sink_->pending_bytes;
}

const SurfaceTextureCache::Entry* SurfaceTextureCache::Lookup(std::uint64_t hash) {
  const auto it = entries_.find(hash);
  if (it == entries_.end()) {
    return nullptr;
  }
  // Move to the front of the LRU list.
  const auto pos = lru_pos_.find(hash);
  if (pos != lru_pos_.end()) {
    lru_.erase(pos->second);
    lru_.push_front(hash);
    pos->second = lru_.begin();
  }
  return &it->second;
}

void SurfaceTextureCache::EvictToBudget() {
  // Keep at least the newest entry (front of lru_) even if it alone exceeds the
  // budget: otherwise a single over-budget texture is uploaded and immediately
  // evicted every frame, so it never displays and perpetually re-decodes on the
  // background executor. Mirrors the sibling text-texture cache's guard.
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
    }
    // An evicted hash may be re-requested later; drop any stale marker so it can.
    in_flight_or_failed_.erase(victim);
  }
}

void SurfaceTextureCache::Clear() {
  for (auto& [hash, entry] : entries_) {
    if (entry.texture != nullptr) {
      SDL_DestroyTexture(entry.texture);
    }
  }
  entries_.clear();
  lru_.clear();
  lru_pos_.clear();
  in_flight_or_failed_.clear();
  texture_create_failures_.clear();
  failed_hash_order_.clear();
  vram_bytes_ = 0;
}

}  // namespace microide::render
