#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace microide::render {

// Hard limits shared by the decoder and its callers. A raster above these is
// rejected outright (no texture, no partial buffer).
inline constexpr int kRasterMaxDimension = 8192;
inline constexpr std::size_t kRasterMaxEncodedBytes = 64u * 1024 * 1024;  // 64 MiB

// Decode encoded image bytes (PNG/JPEG) to tightly-packed RGBA8. SDL-free so it
// can be fuzzed in isolation. Returns false on any failure — malformed input,
// oversize dimensions/bytes, or OOM — leaving `out_rgba` empty. On success
// `out_rgba.size() == out_width * out_height * 4`.
bool DecodeRasterToRgba(std::span<const std::byte> bytes, std::vector<std::uint8_t>& out_rgba,
                        int& out_width, int& out_height);

}  // namespace microide::render
