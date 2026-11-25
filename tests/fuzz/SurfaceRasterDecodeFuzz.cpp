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
