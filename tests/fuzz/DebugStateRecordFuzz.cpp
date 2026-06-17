#include "workspace/WorkspacePersistenceFormat.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Fuzz the per-project debug-state decoder (breakpoints + launch configs).
// DecodeDebugStateRecord must never crash on arbitrary bytes, mirroring the
// PersistedRecordReader fuzz target.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::byte>(data[i]);
  }
  microide::workspace::PersistedDebugState state;
  (void)microide::workspace::DecodeDebugStateRecord(
      std::span<const std::byte>(bytes.data(), bytes.size()), &state);
  return 0;
}
