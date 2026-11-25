#include "render/RasterDecode.h"

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS microide::render::kRasterMaxDimension
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

namespace microide::render {

bool DecodeRasterToRgba(std::span<const std::byte> bytes, std::vector<std::uint8_t>& out_rgba,
                        int& out_width, int& out_height) {
  out_rgba.clear();
  out_width = 0;
  out_height = 0;
  if (bytes.empty() || bytes.size() > kRasterMaxEncodedBytes) {
    return false;
  }
  int w = 0;
  int h = 0;
  int channels = 0;
  stbi_uc* pixels =
      stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                            static_cast<int>(bytes.size()), &w, &h, &channels, 4);
  if (pixels == nullptr) {
    return false;
  }
  if (w <= 0 || h <= 0 || w > kRasterMaxDimension || h > kRasterMaxDimension) {
    stbi_image_free(pixels);
    return false;
  }
  const std::size_t byte_count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
  out_rgba.resize(byte_count);
  std::memcpy(out_rgba.data(), pixels, byte_count);
  stbi_image_free(pixels);
  out_width = w;
  out_height = h;
  return true;
}

}
