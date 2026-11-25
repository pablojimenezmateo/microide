#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace microide::render {

inline constexpr int kRasterMaxDimension = 8192;
inline constexpr std::size_t kRasterMaxEncodedBytes = 64u * 1024 * 1024;

bool DecodeRasterToRgba(std::span<const std::byte> bytes, std::vector<std::uint8_t>& out_rgba,
                        int& out_width, int& out_height);

}
