#include "TestSupport.h"

#include "support/SoftwareCanvas.h"
#include "support/TinyPng.h"

#include "project/ProjectBackgroundExecutor.h"
#include "render/SurfaceTextureCache.h"
#include "util/Waker.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::ProjectBackgroundExecutor;
using microide::render::SurfaceTextureCache;

void TestFailedHashSetIsBounded() {
  ProjectBackgroundExecutor executor;
  SurfaceTextureCache cache(executor);

  const std::vector<std::byte> garbage(16, std::byte{0x7F});

  constexpr std::size_t kRequests = 2600;
  for (std::size_t i = 0; i < kRequests; ++i) {
    std::vector<std::byte> bytes = garbage;
    cache.Request(static_cast<std::uint64_t>(i + 1), SurfaceTextureCache::RasterFormat::Png,
                  std::move(bytes), 0, 0);
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    cache.Upload(nullptr);
    const std::size_t count = cache.failed_hash_count();
    Expect(count <= 1024, "failed-hash set must never exceed kMaxFailedHashes");
    if (cache.in_flight_encoded_bytes() == 0 && cache.pending_decoded_bytes() == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  cache.Upload(nullptr);
  Expect(cache.failed_hash_count() <= 1024,
         "after draining, the failure set is still bounded by the cap");
  Expect(cache.failed_hash_count() == 1024,
         "requesting > 2x the cap of distinct invalid images fills the bounded set to the cap");

  executor.Shutdown();
}

void TestPendingDecodedBytesAreBounded() {
  ProjectBackgroundExecutor executor;
  constexpr std::size_t kPendingBudget = 4u * 1024 * 1024;
  SurfaceTextureCache cache(executor, SurfaceTextureCache::kDefaultVramBudgetBytes, kPendingBudget);

  constexpr int kW = 512;
  constexpr int kH = 512;
  const std::size_t kImageBytes = static_cast<std::size_t>(kW) * kH * 4;
  constexpr std::size_t kRequests = 32;

  for (std::size_t i = 0; i < kRequests; ++i) {
    std::vector<std::byte> rgba(kImageBytes, std::byte{0x40});
    cache.Request(static_cast<std::uint64_t>(i + 1), SurfaceTextureCache::RasterFormat::Rgba8,
                  std::move(rgba), kW, kH);
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  std::size_t peak = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const std::size_t pending = cache.pending_decoded_bytes();
    peak = pending > peak ? pending : peak;
    Expect(pending <= kPendingBudget,
           "pending decoded bytes must never exceed the host-memory budget");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  Expect(peak > 0, "the decode backlog should have accumulated toward the budget");
  cache.Upload(nullptr);
  Expect(cache.pending_decoded_bytes() == 0, "Upload drains the pending decoded backlog to zero");

  executor.Shutdown();
}

void TestInFlightEncodedBytesAreBounded() {
  ProjectBackgroundExecutor executor;
  constexpr std::size_t kEncodedBudget = 1u * 1024 * 1024;
  SurfaceTextureCache cache(executor, SurfaceTextureCache::kDefaultVramBudgetBytes,
                            SurfaceTextureCache::kMaxPendingDecodedBytes, kEncodedBudget);

  std::atomic<bool> release{false};
  executor.Post([&release]() {
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  constexpr std::size_t kRasterBytes = 256u * 1024;
  constexpr std::size_t kRequests = 50;
  for (std::size_t i = 0; i < kRequests; ++i) {
    std::vector<std::byte> bytes(kRasterBytes, std::byte{0x7F});
    cache.Request(static_cast<std::uint64_t>(i + 1), SurfaceTextureCache::RasterFormat::Png,
                  std::move(bytes), 0, 0);
    Expect(cache.in_flight_encoded_bytes() <= kEncodedBudget,
           "in-flight encoded bytes must never exceed the budget");
  }
  Expect(cache.in_flight_encoded_bytes() <= kEncodedBudget,
         "the in-flight encoded budget bounds the retained encoded payload");
  Expect(cache.in_flight_encoded_bytes() > 0,
         "some requests were charged (the worker is stalled, nothing drained)");

  release.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cache.in_flight_encoded_bytes() != 0 &&
         std::chrono::steady_clock::now() < deadline) {
    cache.Upload(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(cache.in_flight_encoded_bytes() == 0,
         "after the worker drains, all encoded reservations are released");
  Expect(cache.failed_hash_count() < kRequests,
         "backpressure dropped requests past the encoded budget (not all decoded)");

  executor.Shutdown();
}

void TestTextureCreateFailureFoldsIntoBoundedFifo() {
  ProjectBackgroundExecutor executor;
  SurfaceTextureCache cache(executor);
  cache.SetTextureFactoryForTesting(
      [](SDL_Renderer*, int, int) -> SDL_Texture* { return nullptr; });
  SDL_Renderer* const fake_renderer = reinterpret_cast<SDL_Renderer*>(0x1);

  constexpr int kW = 2;
  constexpr int kH = 2;
  const std::size_t kImageBytes = static_cast<std::size_t>(kW) * kH * 4;
  const std::uint64_t hash = 42;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cache.failed_hash_count() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::vector<std::byte> rgba(kImageBytes, std::byte{0x40});
    cache.Request(hash, SurfaceTextureCache::RasterFormat::Rgba8, std::move(rgba), kW, kH);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    cache.Upload(fake_renderer);
  }

  Expect(cache.failed_hash_count() == 1,
         "a permanent texture-create failure must be recorded in the bounded failure FIFO");
  Expect(cache.failed_hash_count() <= 1024,
         "the folded texture-create failures stay under kMaxFailedHashes");

  executor.Shutdown();
}

void UploadRasters(SurfaceTextureCache& cache, SDL_Renderer* renderer, std::uint64_t first_hash,
                   int count) {
  constexpr int kW = 8;
  constexpr int kH = 8;
  const std::size_t image_bytes = static_cast<std::size_t>(kW) * kH * 4;
  for (int i = 0; i < count; ++i) {
    std::vector<std::byte> rgba(image_bytes, std::byte{0x7f});
    cache.Request(first_hash + static_cast<std::uint64_t>(i),
                  SurfaceTextureCache::RasterFormat::Rgba8, std::move(rgba), kW, kH);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    cache.Upload(renderer);
    if (cache.cached_count() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      cache.Upload(renderer);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void TestVramBudgetEvictsLeastRecentlyUsed() {
  SoftwareCanvas canvas(64, 64);
  ProjectBackgroundExecutor executor;
  constexpr std::size_t kTextureBytes = 8u * 8u * 4u;
  SurfaceTextureCache cache(executor, kTextureBytes * 2);

  UploadRasters(cache, canvas.renderer(), 1000, 4);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cache.cached_count() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    cache.Upload(canvas.renderer());
  }

  Expect(cache.cached_count() > 0,
         "a real software renderer must actually create textures, or this proves nothing");
  Expect(cache.cached_count() <= 2,
         "the VRAM budget must evict down to two 8x8 textures rather than keeping all four");

  executor.Shutdown();
}

void TestUploadWithNoRendererDoesNotPermanentlySuppressTheImage() {
  SoftwareCanvas canvas(64, 64);
  ProjectBackgroundExecutor executor;
  SurfaceTextureCache cache(executor);

  constexpr int kW = 8;
  constexpr int kH = 8;
  std::vector<std::byte> rgba(static_cast<std::size_t>(kW) * kH * 4, std::byte{0x33});
  cache.Request(4242, SurfaceTextureCache::RasterFormat::Rgba8, std::move(rgba), kW, kH);

  for (int i = 0; i < 5; ++i) {
    cache.Upload(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(cache.cached_count() == 0, "no renderer means no texture was created");
  Expect(cache.failed_hash_count() == 0,
         "a missing renderer is not a decode failure and must not be recorded as one");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cache.cached_count() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::vector<std::byte> retry(static_cast<std::size_t>(kW) * kH * 4, std::byte{0x33});
    cache.Request(4242, SurfaceTextureCache::RasterFormat::Rgba8, std::move(retry), kW, kH);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    cache.Upload(canvas.renderer());
  }
  Expect(cache.cached_count() == 1,
         "the same hash re-decodes once a renderer exists rather than staying suppressed");
  Expect(cache.failed_hash_count() == 0, "and it was never recorded as a failure");

  executor.Shutdown();
}

void TestDecodeReportsTheEncodedSizeAndWakes() {
  ProjectBackgroundExecutor executor;
  SurfaceTextureCache cache(executor);
  constexpr microide::util::WakeChannel kChannel = 77;
  cache.SetWakeChannel(kChannel);
  std::atomic<int> wakes{0};
  microide::util::SetWakePusherForTesting([&wakes](microide::util::WakeChannel channel) {
    if (channel == kChannel) {
      wakes.fetch_add(1);
    }
    return true;
  });

  constexpr std::uint64_t kHash = 9001;
  cache.Request(kHash, SurfaceTextureCache::RasterFormat::Png, TinyPng(), 0,
                0);
  cache.Request(kHash + 1, SurfaceTextureCache::RasterFormat::Png,
                std::vector<std::byte>(16, std::byte{0x7F}), 0, 0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::vector<SurfaceTextureCache::RasterSize> sizes;
  while (sizes.empty() && std::chrono::steady_clock::now() < deadline) {
    sizes = cache.TakeDecodedSizes();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  executor.Shutdown();
  for (const SurfaceTextureCache::RasterSize& size : cache.TakeDecodedSizes()) {
    sizes.push_back(size);
  }
  microide::util::SetWakePusherForTesting(nullptr);

  Expect(sizes.size() == 1 && sizes[0].hash == kHash && sizes[0].width == kTinyPngWidth &&
             sizes[0].height == kTinyPngHeight,
         "the decode reports the image's own size, and a failed decode reports none");
  Expect(wakes.load() == 1, "one wake per successful decode: " + std::to_string(wakes.load()));
  const auto known = cache.DecodedSize(kHash);
  Expect(known.has_value() && known->width == kTinyPngWidth && known->height == kTinyPngHeight,
         "a taken size answers DecodedSize before anything uploads it");
  Expect(!cache.DecodedSize(kHash + 1).has_value(), "a failed decode has no size");

  SoftwareCanvas canvas(32, 32);
  cache.Upload(canvas.renderer());
  const auto uploaded = cache.DecodedSize(kHash);
  Expect(cache.cached_count() == 1 && uploaded.has_value() &&
             uploaded->height == kTinyPngHeight,
         "after the upload the texture entry answers DecodedSize");
}

}

void RegisterSurfaceTextureCacheTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SurfaceTextureCache/VramBudgetEvictsLeastRecentlyUsed",
          TestVramBudgetEvictsLeastRecentlyUsed);
  AddTest(tests, "SurfaceTextureCache/UploadWithNoRendererDoesNotPermanentlySuppressTheImage",
          TestUploadWithNoRendererDoesNotPermanentlySuppressTheImage);
  AddTest(tests, "SurfaceTextureCache/FailedHashSetIsBounded", TestFailedHashSetIsBounded);
  AddTest(tests, "SurfaceTextureCache/PendingDecodedBytesAreBounded",
          TestPendingDecodedBytesAreBounded);
  AddTest(tests, "SurfaceTextureCache/TextureCreateFailureFoldsIntoBoundedFifo",
          TestTextureCreateFailureFoldsIntoBoundedFifo);
  AddTest(tests, "SurfaceTextureCache/InFlightEncodedBytesAreBounded",
          TestInFlightEncodedBytesAreBounded);
  AddTest(tests, "SurfaceTextureCache/DecodeReportsTheEncodedSizeAndWakes",
          TestDecodeReportsTheEncodedSizeAndWakes);
}

}
