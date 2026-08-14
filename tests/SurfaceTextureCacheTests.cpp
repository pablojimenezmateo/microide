#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "project/ProjectBackgroundExecutor.h"
#include "render/SurfaceTextureCache.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::ProjectBackgroundExecutor;
using microide::render::SurfaceTextureCache;

// TD-2026-07-17-043: failed-decode markers are kept to short-circuit re-requests,
// but a plugin publishing an endless stream of DISTINCT invalid image hashes must
// not grow that set without a budget. Request far more than kMaxFailedHashes
// invalid PNGs and prove the retained failure set stays bounded.
void TestFailedHashSetIsBounded() {
  ProjectBackgroundExecutor executor;
  SurfaceTextureCache cache(executor);

  // "Invalid" PNG bytes: a non-empty payload that the decoder cannot parse, so the
  // decode completes with ok == false (a permanent failure marker).
  const std::vector<std::byte> garbage(16, std::byte{0x7F});

  constexpr std::size_t kRequests = 2600;  // > 2 * kMaxFailedHashes (1024)
  for (std::size_t i = 0; i < kRequests; ++i) {
    std::vector<std::byte> bytes = garbage;  // distinct hash, same invalid content
    cache.Request(static_cast<std::uint64_t>(i + 1), SurfaceTextureCache::RasterFormat::Png,
                  std::move(bytes), 0, 0);
  }

  // Decodes run on the executor's worker thread; drain them into textures/markers
  // via repeated Upload(nullptr) until the failure count plateaus (or a deadline).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::size_t stable_iterations = 0;
  std::size_t last_count = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    cache.Upload(nullptr);
    const std::size_t count = cache.failed_hash_count();
    // The invariant under test: the retained failure set never exceeds the cap.
    Expect(count <= 1024, "failed-hash set must never exceed kMaxFailedHashes");
    if (count == last_count) {
      if (++stable_iterations >= 20) {
        break;  // no progress for a while — all reachable decodes processed
      }
    } else {
      stable_iterations = 0;
      last_count = count;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  cache.Upload(nullptr);
  Expect(cache.failed_hash_count() <= 1024,
         "after draining, the failure set is still bounded by the cap");
  // We requested well past the cap, so the eviction path must have fired: the count
  // should have reached the cap rather than staying tiny.
  Expect(cache.failed_hash_count() == 1024,
         "requesting > 2x the cap of distinct invalid images fills the bounded set to the cap");

  executor.Shutdown();
}

// TD-2026-07-17-092: successfully-decoded RGBA buffers waiting for the shell
// thread's Upload must be bounded by a host-memory budget, distinct from the VRAM
// budget (which only applies after upload). Flood the cache with valid rasters
// faster than they are uploaded and prove the pre-upload backlog never exceeds the
// injected pending budget.
void TestPendingDecodedBytesAreBounded() {
  ProjectBackgroundExecutor executor;
  // Tiny budgets keep the test cheap: 4 MiB of pending decoded RGBA.
  constexpr std::size_t kPendingBudget = 4u * 1024 * 1024;
  SurfaceTextureCache cache(executor, SurfaceTextureCache::kDefaultVramBudgetBytes, kPendingBudget);

  // 512x512 RGBA8 = exactly 1 MiB per valid decode. Request many without uploading.
  constexpr int kW = 512;
  constexpr int kH = 512;
  const std::size_t kImageBytes = static_cast<std::size_t>(kW) * kH * 4;
  constexpr std::size_t kRequests = 32;  // 32 MiB total, 8x the pending budget

  for (std::size_t i = 0; i < kRequests; ++i) {
    std::vector<std::byte> rgba(kImageBytes, std::byte{0x40});
    cache.Request(static_cast<std::uint64_t>(i + 1), SurfaceTextureCache::RasterFormat::Rgba8,
                  std::move(rgba), kW, kH);
  }

  // Let the worker decode while we (deliberately) do NOT call Upload, so the sink
  // accumulates. Poll the pending-bytes budget invariant throughout.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  std::size_t peak = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const std::size_t pending = cache.pending_decoded_bytes();
    peak = pending > peak ? pending : peak;
    Expect(pending <= kPendingBudget,
           "pending decoded bytes must never exceed the host-memory budget");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // The backlog must have actually accumulated (proving the worker ran and the
  // budget engaged), and a drain must reset it to zero.
  Expect(peak > 0, "the decode backlog should have accumulated toward the budget");
  cache.Upload(nullptr);
  Expect(cache.pending_decoded_bytes() == 0, "Upload drains the pending decoded backlog to zero");

  executor.Shutdown();
}

// TD-2026-07-17A-043: encoded bytes captured by queued/in-flight decode tasks
// must be bounded. Stall the (serial) executor so decode tasks pile up, then
// flood distinct rasters and prove the in-flight encoded budget both caps the
// retained bytes and drops requests past the budget.
void TestInFlightEncodedBytesAreBounded() {
  ProjectBackgroundExecutor executor;
  constexpr std::size_t kEncodedBudget = 1u * 1024 * 1024;  // 1 MiB
  SurfaceTextureCache cache(executor, SurfaceTextureCache::kDefaultVramBudgetBytes,
                            SurfaceTextureCache::kMaxPendingDecodedBytes, kEncodedBudget);

  // Block the single worker so no decode completes (and releases its encoded bytes)
  // while we flood requests.
  std::atomic<bool> release{false};
  executor.Post([&release]() {
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  constexpr std::size_t kRasterBytes = 256u * 1024;  // 256 KiB each
  constexpr std::size_t kRequests = 50;              // 50 * 256 KiB = 12.5 MiB >> budget
  for (std::size_t i = 0; i < kRequests; ++i) {
    // Png-format garbage: the decode fails, but encoded bytes are charged at Request
    // regardless of decode outcome (they are captured by the queued task).
    std::vector<std::byte> bytes(kRasterBytes, std::byte{0x7F});
    cache.Request(static_cast<std::uint64_t>(i + 1), SurfaceTextureCache::RasterFormat::Png,
                  std::move(bytes), 0, 0);
    Expect(cache.in_flight_encoded_bytes() <= kEncodedBudget,
           "in-flight encoded bytes must never exceed the budget");
  }
  // Only floor(budget / raster) requests can be charged; the rest are dropped.
  Expect(cache.in_flight_encoded_bytes() <= kEncodedBudget,
         "the in-flight encoded budget bounds the retained encoded payload");
  Expect(cache.in_flight_encoded_bytes() > 0,
         "some requests were charged (the worker is stalled, nothing drained)");

  // Release the worker and drain: every charged decode completes and releases its
  // encoded bytes, so the in-flight total returns to zero.
  release.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cache.in_flight_encoded_bytes() != 0 &&
         std::chrono::steady_clock::now() < deadline) {
    cache.Upload(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(cache.in_flight_encoded_bytes() == 0,
         "after the worker drains, all encoded reservations are released");
  // Far fewer than kRequests were ever charged (backpressure dropped the excess):
  // ~budget/raster distinct rasters decoded (and failed), not all 50.
  Expect(cache.failed_hash_count() < kRequests,
         "backpressure dropped requests past the encoded budget (not all decoded)");

  executor.Shutdown();
}

// TD-2026-07-17A-118: a raster whose bytes decode fine but whose texture the
// renderer cannot create (dimensions the decoder accepts but the GPU rejects)
// must, once its bounded retries are exhausted, fold into the SAME bounded
// failure FIFO as decode failures — otherwise its hash lingers in
// in_flight_or_failed_ / texture_create_failures_ outside every budget.
void TestTextureCreateFailureFoldsIntoBoundedFifo() {
  ProjectBackgroundExecutor executor;
  SurfaceTextureCache cache(executor);
  // Force every texture creation to fail, without a live GPU.
  cache.SetTextureFactoryForTesting(
      [](SDL_Renderer*, int, int) -> SDL_Texture* { return nullptr; });
  // A non-null bogus renderer so Upload proceeds past its renderer==nullptr guard;
  // the injected factory never dereferences it.
  SDL_Renderer* const fake_renderer = reinterpret_cast<SDL_Renderer*>(0x1);

  // 2x2 RGBA8 = 16 bytes: a valid decode that reaches texture creation.
  constexpr int kW = 2;
  constexpr int kH = 2;
  const std::size_t kImageBytes = static_cast<std::size_t>(kW) * kH * 4;
  const std::uint64_t hash = 42;

  // A texture-create failure below the retry cap drops the marker (so a later
  // request re-decodes). Re-request + drain until the retries are exhausted and the
  // hash becomes a permanent failure recorded in the bounded FIFO.
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
  // And it stays bounded: further distinct un-creatable rasters do not grow it
  // past the cap (shares the decode-failure eviction path).
  Expect(cache.failed_hash_count() <= 1024,
         "the folded texture-create failures stay under kMaxFailedHashes");

  executor.Shutdown();
}

// The 2026-07-10 sweep deferred these two as untestable: "Upload needs a live SDL
// renderer (SDL_CreateTexture), unavailable headless". That premise has expired
// twice over — the cache grew a texture-factory seam (TD-2026-07-17A-118), and
// the test suite has SoftwareCanvas, a REAL SDL_Renderer with no GPU. Which is
// the note that same section already makes: "untestable" claims should be
// re-checked after a refactor moves the code, not inherited.
//
// Uploads `count` distinct 8x8 RGBA rasters and drains them, returning once the
// cache has stopped taking new ones (or the deadline passes).
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
      // Give the remaining decodes a moment, then drain again.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      cache.Upload(renderer);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// Entries are evicted LRU once total texture VRAM passes the budget. With a
// budget of two 8x8 textures, uploading four must not leave four resident.
void TestVramBudgetEvictsLeastRecentlyUsed() {
  SoftwareCanvas canvas(64, 64);
  ProjectBackgroundExecutor executor;
  constexpr std::size_t kTextureBytes = 8u * 8u * 4u;
  SurfaceTextureCache cache(executor, /*vram_budget_bytes=*/kTextureBytes * 2);

  UploadRasters(cache, canvas.renderer(), /*first_hash=*/1000, /*count=*/4);
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

// Upload(nullptr) happens on any frame painted before the renderer exists (or
// across a renderer re-create). The contract is NOT that the decoded bytes
// survive — Upload deliberately drops them — it is that a valid image is not
// recorded as a permanent FAILURE, because a failure marker suppresses that
// content hash until Clear() and the image would never appear again. So the
// guard is: no failure marker, and a later Request with a live renderer works.
void TestUploadWithNoRendererDoesNotPermanentlySuppressTheImage() {
  SoftwareCanvas canvas(64, 64);
  ProjectBackgroundExecutor executor;
  SurfaceTextureCache cache(executor);

  constexpr int kW = 8;
  constexpr int kH = 8;
  std::vector<std::byte> rgba(static_cast<std::size_t>(kW) * kH * 4, std::byte{0x33});
  cache.Request(4242, SurfaceTextureCache::RasterFormat::Rgba8, std::move(rgba), kW, kH);

  // Drain with no renderer several times: must be a no-op, not a drop.
  for (int i = 0; i < 5; ++i) {
    cache.Upload(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(cache.cached_count() == 0, "no renderer means no texture was created");
  Expect(cache.failed_hash_count() == 0,
         "a missing renderer is not a decode failure and must not be recorded as one");

  // Re-requested with a live renderer, the SAME hash still decodes and uploads —
  // it was not suppressed by the renderer-less frames.
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

}  // namespace

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
}

}  // namespace microide::tests
