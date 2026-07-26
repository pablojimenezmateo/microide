// The `Content-Length` codec that turns an LSP/DAP server's raw stdout into
// discrete JSON-RPC messages. This is the editor's most directly hostile input
// surface: the bytes come from an external subprocess (a language server, a debug
// adapter, or anything a plugin declared as one), and the framer must never
// desync, never buffer without bound, and never read out of the buffer — a desync
// makes every later frame garbage, which is how the DAP copy of this codec broke
// on a peer that wrote `content-length:` without a space.
//
// The input is split into arbitrary chunks by a length-prefixed prologue so the
// fuzzer explores split points (mid-header, mid-body, on a header newline) as
// well as content; a single Append of the whole buffer would only ever exercise
// the already-complete path.
#include "workspace/JsonRpcMessageFraming.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }

  // First byte picks the message ceiling so both the normal path and the
  // oversized-frame skip/resync path are reachable. A tiny ceiling makes almost
  // every frame oversized, which is exactly the drain-and-resync state machine.
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
    // Chunk sizes derive from the stream itself, so the corpus can steer where
    // the splits land without a separate control channel.
    const std::size_t chunk =
        1 + (static_cast<unsigned char>(stream[offset]) % 64u);
    const std::string_view slice = stream.substr(offset, chunk);
    offset += slice.size();
    framer.Append(slice);

    // Drain exactly the way both transports' ParseBufferedMessages does: keep
    // pulling while the framer either yields a message or consumes bytes, and
    // stop when it makes no progress. A framer that returned nullopt without
    // consuming while a complete frame is buffered would hang here.
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
