#include "TestSupport.h"

#include "project/ProjectBackgroundExecutor.h"
#include "render/SurfaceTextureCache.h"

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

}  // namespace

void RegisterSurfaceTextureCacheTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SurfaceTextureCache/FailedHashSetIsBounded", TestFailedHashSetIsBounded);
  AddTest(tests, "SurfaceTextureCache/PendingDecodedBytesAreBounded",
          TestPendingDecodedBytesAreBounded);
  AddTest(tests, "SurfaceTextureCache/TextureCreateFailureFoldsIntoBoundedFifo",
          TestTextureCreateFailureFoldsIntoBoundedFifo);
}

}  // namespace microide::tests
