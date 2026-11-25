#include "workspace/JsonRpcMessageFraming.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }

  microide::workspace::JsonRpcMessageFramer framer;
  switch (data[0] & 0x03u) {
    case 0:
      framer.max_message_bytes = 16;
      break;
    case 1:
      framer.max_message_bytes = 256;
      break;
    case 2:
      framer.max_message_bytes = 4096;
      break;
    default:
      framer.max_message_bytes = microide::workspace::kDefaultMaxJsonRpcMessageBytes;
      break;
  }

  const std::string_view stream(reinterpret_cast<const char*>(data) + 1, size - 1);
  std::size_t offset = 0;
  while (offset < stream.size()) {
    const std::size_t chunk =
        1 + (static_cast<unsigned char>(stream[offset]) % 64u);
    const std::string_view slice = stream.substr(offset, chunk);
    offset += slice.size();
    framer.Append(slice);

    while (true) {
      const std::size_t buffered_before = framer.BufferedBytes();
      if (framer.Next().has_value()) {
        continue;
      }
      if (framer.BufferedBytes() == buffered_before) {
        break;
      }
    }
  }
  return 0;
}
