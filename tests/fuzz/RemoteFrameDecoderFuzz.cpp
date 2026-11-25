#include "project/remote/RemoteFrame.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  using microide::project::remote::AppendFrame;
  using microide::project::remote::Frame;
  using microide::project::remote::FrameDecoder;
  using microide::project::remote::kMaxFramePayloadBytes;

  const std::size_t chunk = static_cast<std::size_t>(data[0]) + 1;
  const std::string_view stream(reinterpret_cast<const char*>(data) + 1, size - 1);

  FrameDecoder decoder;
  std::string reencoded;
  std::size_t fed = 0;
  bool failed = false;
  for (std::size_t offset = 0; offset < stream.size(); offset += chunk) {
    const std::string_view piece = stream.substr(offset, chunk);
    decoder.Feed(piece);
    fed += piece.size();
    while (std::optional<Frame> frame = decoder.Next()) {
      if (failed || frame->payload.size() > kMaxFramePayloadBytes) {
        std::abort();
      }
      if (!AppendFrame(reencoded, frame->type, frame->lane, frame->id, frame->payload)) {
        std::abort();
      }
    }
    failed = failed || decoder.error() != FrameDecoder::Error::None;
  }
  if (!failed) {
    if (reencoded.size() + decoder.buffered_bytes() != fed ||
        stream.substr(0, reencoded.size()) != reencoded) {
      std::abort();
    }
  }
  return 0;
}
