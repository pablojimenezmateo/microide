#include <cstddef>
#include <cstdint>
#include <string>

#include "render/PluginDisplayList.h"

namespace {

std::uint32_t ReadU32(const std::uint8_t* data, std::size_t size, std::size_t& pos) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4 && pos < size; ++i, ++pos) {
    value = (value << 8) | data[pos];
  }
  return value;
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microide::render;
  PluginDisplayList list;
  std::size_t pos = 0;

  const std::uint32_t text_len = size ? (data[pos++] % 64) : 0;
  for (std::uint32_t i = 0; i < text_len && pos < size; ++i) {
    list.text_arena.push_back(static_cast<char>(data[pos++]));
  }
  const std::uint32_t point_count = pos < size ? (data[pos++] % 64) : 0;
  for (std::uint32_t i = 0; i < point_count && pos + 1 < size; ++i) {
    list.point_arena.push_back(SDL_FPoint{static_cast<float>(data[pos]), static_cast<float>(data[pos + 1])});
    pos += 2;
  }
  if (pos < size && (data[pos] & 1)) {
    list.image_hashes.push_back(0xabcdef);
  }

  while (pos < size && list.ops.size() < kMaxDisplayOps) {
    DisplayOp op;
    op.op = static_cast<DrawOp>(data[pos++] % 7);
    op.data_offset = ReadU32(data, size, pos);
    op.data_count = ReadU32(data, size, pos);
    list.ops.push_back(op);
  }

  std::string error;
  if (ValidateDisplayList(list, &error)) {
    volatile std::uint64_t sink = ComputeDisplayListHash(list);
    (void)sink;
  }
  return 0;
}
