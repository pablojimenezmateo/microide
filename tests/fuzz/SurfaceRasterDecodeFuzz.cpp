// Fuzzes the plugin raster-decode path: arbitrary bytes -> stb_image PNG/JPEG
// decode (the real untrusted-input parser behind ctx.surface.setRaster). Run
// under ASAN/UBSAN to catch decoder OOB / overflow on hostile images.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "render/RasterDecode.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
  if (microide::render::DecodeRasterToRgba(bytes, rgba, width, height)) {
    // On success the buffer must exactly match the reported dimensions; reading
    // the bounds here lets ASAN flag any short allocation.
    const std::size_t expected =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (rgba.size() != expected) {
      __builtin_trap();
    }
    volatile std::uint8_t sink = 0;
    if (!rgba.empty()) {
      sink ^= rgba.front();
      sink ^= rgba.back();
    }
    (void)sink;
  }
  return 0;
}
