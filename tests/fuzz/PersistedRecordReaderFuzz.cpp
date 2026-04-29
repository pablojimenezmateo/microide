#include "persistence/PersistedRecordReader.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::byte>(data[i]);
  }
  microide::persistence::PersistedRecordReaderError error =
      microide::persistence::PersistedRecordReaderError::None;
  (void)microide::persistence::PersistedRecordReader::Decode(
      std::span<const std::byte>(bytes.data(), bytes.size()), &error);
  return 0;
}
