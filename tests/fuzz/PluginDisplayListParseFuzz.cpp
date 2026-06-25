// Fuzzes the display-list validation invariant: arbitrary bytes are interpreted
// as a raw op buffer (the adversarial shape a hostile plugin could try to smuggle
// past the Lua parser) and fed to ValidateDisplayList. The invariant under test:
// any list ValidateDisplayList accepts has every op's data reference in-bounds, so
// the allocation-free replay can trust it. After a list validates we exercise the
// hash, which walks the same buffers. Run under ASAN/UBSAN.
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

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microide::render;
  PluginDisplayList list;
  std::size_t pos = 0;

  // First bytes seed the arenas so text/polyline ops can reference them.
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

  // Remaining bytes become a stream of ops with attacker-controlled offsets.
  while (pos < size && list.ops.size() < kMaxDisplayOps) {
    DisplayOp op;
    op.op = static_cast<DrawOp>(data[pos++] % 7);
    op.data_offset = ReadU32(data, size, pos);
    op.data_count = ReadU32(data, size, pos);
    list.ops.push_back(op);
  }

  std::string error;
  if (ValidateDisplayList(list, &error)) {
    // A validated list must be safe to hash (walks all ops + arenas).
    volatile std::uint64_t sink = ComputeDisplayListHash(list);
    (void)sink;
  }
  return 0;
}
